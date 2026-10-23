#include "computermanager.h"
#include "nvaddress.h"
#include "nvhttp.h"
#include "nvpairingmanager.h"

#include "mslinkbridge.h"

#include <Limelight.h>
#include <QtEndian>

#include <QThread>
#include <QThreadPool>
#include <QCoreApplication>
#include <QRandomGenerator>

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <thread>

#define SER_HOSTS "hosts"
#define SER_HOSTS_BACKUP "hostsbackup"

// =============================================================================
//  MiniStream 自动配对
//
//  背景：Sunshine 的配对需要用户在「被控端」上输入 4 位 PIN。这套流程对一个
//  面向普通用户的客户端来说是不可接受的（尤其远程场景），所以主控端这边改用：
//
//    1) 客户端照常发起配对（第 1 阶段 getservercert，被控端会把本次配对登记为 pending）
//    2) 客户端用被控端 Sunshine 管理网页的 /api/pin 接口，用我们自己生成的 PIN
//       把这条 pending approve 掉（凭据由安装被控端时生成，主控端填一次）
//    3) 配对流程继续，全程无需人工输入 PIN
//
//  失败时返回中文原因，UI 直接展示；手动输 PIN 的老路径完全不受影响。
// =============================================================================
namespace {

constexpr quint16 kSunshineWebPort = 47990;

// 取被控端地址：优先当前正在用的地址，其次手动地址 / 局域网 / 公网
QString pairingHostAddress(const NvComputer* computer)
{
    if (!computer->activeAddress.address().isEmpty()) {
        return computer->activeAddress.address();
    }
    if (!computer->manualAddress.address().isEmpty()) {
        return computer->manualAddress.address();
    }
    if (!computer->localAddress.address().isEmpty()) {
        return computer->localAddress.address();
    }
    return computer->remoteAddress.address();
}

class SunshineAutoApprover
{
public:
    SunshineAutoApprover(QString host, QString user, QString pass, QString pin, QString deviceName)
        : m_Host(std::move(host)),
          m_User(std::move(user)),
          m_Pass(std::move(pass)),
          m_Pin(std::move(pin)),
          m_DeviceName(std::move(deviceName))
    {
    }

    // 关键：必须在 pairingManager.pair() **之前**在后台线程起来。
    // Sunshine 的 pair?phrase=getservercert 会一直挂着等管理员输入 PIN，
    // 所以「先 pair 再 approve」在同一条线程里会直接卡死；必须并行。
    void start()
    {
        m_Thread = std::thread([this]() {
            QString err;
            bool ok = run(&err);
            QMutexLocker lock(&m_Mutex);
            m_Ok = ok;
            m_Error = err;
            m_Done = true;
        });
    }

    void stop()
    {
        m_Stop.store(true);
        if (m_Thread.joinable()) {
            m_Thread.join();
        }
    }

    bool succeeded()
    {
        QMutexLocker lock(&m_Mutex);
        return m_Ok;
    }

    QString error()
    {
        QMutexLocker lock(&m_Mutex);
        return m_Error;
    }

    bool run(QString* error)
    {
        const QUrl apiUrl(QStringLiteral("https://%1:%2/api/pin").arg(m_Host).arg(kSunshineWebPort));

        // ---- 第 1 步：等被控端出现待确认的配对（pair() 发起后立刻就登记）----
        QString pairingId;
        int lastStatus = 0;
        for (int attempt = 0; attempt < 30; ++attempt) {
            if (m_Stop.load()) {
                if (error) {
                    *error = QStringLiteral("自动配对已取消");
                }
                return false;
            }

            QByteArray body;
            QString netError;
            int status = 0;
            if (!perform(apiUrl, QByteArrayLiteral("GET"), QByteArray(), &body, &status, &netError)) {
                if (error) {
                    *error = QStringLiteral("无法连接被控端的 Sunshine 管理网页(%1:%2)：%3。"
                                            "请确认被控端 Sunshine 正在运行、47990 端口可达。")
                                     .arg(m_Host).arg(kSunshineWebPort).arg(netError);
                }
                return false;
            }

            lastStatus = status;
            if (status == 401 || status == 403) {
                if (error) {
                    *error = QStringLiteral("被控端的 Sunshine 管理密码不正确（HTTP %1）。"
                                            "请检查「Sunshine 管理密码」是否为被控端安装时生成的那个。").arg(status);
                }
                return false;
            }

            if (status == 200) {
                const QJsonDocument doc = QJsonDocument::fromJson(body);
                for (const QJsonValue& value : doc.object().value(QStringLiteral("pairings")).toArray()) {
                    const QString id = value.toObject().value(QStringLiteral("id")).toString();
                    if (!id.isEmpty()) {
                        pairingId = id;
                        break;
                    }
                }
                if (!pairingId.isEmpty()) {
                    break;
                }
            }

            QThread::msleep(400);
        }

        if (pairingId.isEmpty()) {
            if (error) {
                *error = QStringLiteral("被控端没有出现待确认的配对请求（最后 HTTP %1）。"
                                        "可能 Sunshine 未开启，或配对被占用，请稍后重试。")
                                 .arg(lastStatus);
            }
            return false;
        }

        // ---- 第 2 步：用我们自己的 PIN approve ----
        QJsonObject request;
        request.insert(QStringLiteral("pairing_id"), pairingId);
        request.insert(QStringLiteral("pin"), m_Pin);
        request.insert(QStringLiteral("name"), m_DeviceName);

        QByteArray body;
        QString netError;
        int status = 0;
        if (!perform(apiUrl, QByteArrayLiteral("POST"),
                     QJsonDocument(request).toJson(QJsonDocument::Compact),
                     &body, &status, &netError, 9000)) {
            // 有些 Sunshine 版本/链路上，approve 成功后服务端会直接断开连接
            // （curl 表现为 HTTP 000，QNAM 表现为超时）。这时不能用状态码判断，
            // 改看「这条 pending 是否已经消失」。
            if (pendingGone(apiUrl, pairingId)) {
                qInfo() << "MiniStream: approve 请求没收完整响应，但 pending 已消失，视为成功";
                qInfo() << "MiniStream: auto pairing approved by" << m_Host;
                return true;
            }
            if (error) {
                *error = QStringLiteral("提交自动配对确认失败：%1").arg(netError);
            }
            return false;
        }

        if (status < 200 || status >= 300) {
            if (pendingGone(apiUrl, pairingId)) {
                qInfo() << "MiniStream: approve 返回 HTTP" << status
                        << "但 pending 已消失，视为成功";
                qInfo() << "MiniStream: auto pairing approved by" << m_Host;
                return true;
            }
            if (error) {
                *error = QStringLiteral("被控端拒绝了自动配对确认（HTTP %1）。"
                                        "请确认 Sunshine 管理密码正确，且被控端没有正在进行的其它配对。").arg(status);
            }
            return false;
        }

        qInfo() << "MiniStream: auto pairing approved by" << m_Host;
        return true;
    }

private:
    // approve 之后 pending 是否已经不在列表里（Sunshine 有时不回完整响应）
    bool pendingGone(const QUrl& apiUrl, const QString& pairingId)
    {
        QByteArray body;
        QString netError;
        int status = 0;
        if (!perform(apiUrl, QByteArrayLiteral("GET"), QByteArray(), &body, &status, &netError)) {
            return false;
        }
        if (status != 200) {
            return false;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        for (const QJsonValue& value : doc.object().value(QStringLiteral("pairings")).toArray()) {
            if (value.toObject().value(QStringLiteral("id")).toString() == pairingId) {
                return false;   // 还在，没批准
            }
        }
        return true;
    }
    // 同步发一个请求（本函数运行在配对工作线程里，不能依赖主线程事件循环）
    bool perform(const QUrl& url, const QByteArray& verb, const QByteArray& payload,
                 QByteArray* body, int* status, QString* error, int timeoutMs = 5000)
    {
        QNetworkAccessManager nam;

        QNetworkRequest request(url);

        // 被控端是自签证书，自动配对阶段不做证书校验（配对完成后仍沿用
        // moonlight 原有的证书 pinning 机制，安全性不受影响）
        QSslConfiguration ssl = request.sslConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        request.setSslConfiguration(ssl);

        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/json"));
        request.setRawHeader("Authorization",
                             "Basic " + QByteArray(QStringLiteral("%1:%2").arg(m_User, m_Pass).toUtf8()).toBase64());

        QNetworkReply* reply;
        if (verb == "POST") {
            reply = nam.post(request, payload);
        }
        else {
            reply = nam.get(request);
        }

        QEventLoop loop;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(reply, &QNetworkReply::sslErrors, reply, [reply](const QList<QSslError>&) {
            reply->ignoreSslErrors();
        });

        watchdog.start(timeoutMs);
        loop.exec();

        bool ok = true;
        if (!reply->isFinished()) {
            reply->abort();
            if (error) {
                *error = QStringLiteral("请求超时");
            }
            ok = false;
        }
        else {
            *status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            *body = reply->readAll();

            if (reply->error() != QNetworkReply::NoError &&
                    *status == 0 &&
                    reply->error() != QNetworkReply::SslHandshakeFailedError) {
                if (error) {
                    *error = reply->errorString();
                }
                ok = false;
            }
        }

        reply->deleteLater();
        return ok;
    }

    QString m_Host;
    QString m_User;
    QString m_Pass;
    QString m_Pin;
    QString m_DeviceName;

    std::thread m_Thread;
    std::atomic<bool> m_Stop{false};
    QMutex m_Mutex;
    bool m_Done = false;
    bool m_Ok = false;
    QString m_Error;
};

} // namespace

class PcMonitorThread : public QThread
{
    Q_OBJECT

#define TRIES_BEFORE_OFFLINING 2
#define POLLS_PER_APPLIST_FETCH 10

public:
    PcMonitorThread(NvComputer* computer)
        : m_Computer(computer)
    {
        setObjectName("Polling thread for " + computer->name);
    }

private:
    bool tryPollComputer(QNetworkAccessManager* nam, NvAddress address, bool& changed)
    {
        NvHTTP http(address, 0, m_Computer->serverCert, !m_Computer->isNvidiaServerSoftware, nam);

        QString serverInfo;
        try {
            serverInfo = http.getServerInfo(NvHTTP::NvLogLevel::NVLL_NONE, true);
        } catch (...) {
            return false;
        }

        NvComputer newState(http, serverInfo);

        // Ensure the machine that responded is the one we intended to contact
        if (m_Computer->uuid != newState.uuid) {
            qInfo() << "Found unexpected PC" << newState.name << "looking for" << m_Computer->name;
            return false;
        }

        changed = m_Computer->update(newState);
        return true;
    }

    bool updateAppList(QNetworkAccessManager* nam, bool& changed)
    {
        NvHTTP http(m_Computer, nam);

        QVector<NvApp> appList;

        try {
            appList = http.getAppList();
            if (appList.isEmpty()) {
                return false;
            }
        } catch (...) {
            return false;
        }

        QWriteLocker lock(&m_Computer->lock);
        changed = m_Computer->updateAppList(appList);
        return true;
    }

    void run() override
    {
        // Reduce the power and performance impact of our
        // computer status polling while it's running.
        setPriority(QThread::LowPriority);
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        setServiceLevel(QThread::QualityOfService::Eco);
#endif

        // Share the QNetworkAccessManager to conserve resources when polling.
        // Each instance creates a worker thread, so sharing them ensures that
        // we are not spamming a new thread for every single polling attempt.
        //
        // Since QThread inherit the priority of the current thread, this also
        // ensures that the NAM's worker thread will inherit our lower priority.
        QNetworkAccessManager nam;

        // Always fetch the applist the first time
        int pollsSinceLastAppListFetch = POLLS_PER_APPLIST_FETCH;
        while (!isInterruptionRequested()) {
            bool stateChanged = false;
            bool online = false;
            bool wasOnline = m_Computer->state == NvComputer::CS_ONLINE;
            for (int i = 0; i < (wasOnline ? TRIES_BEFORE_OFFLINING : 1) && !online; i++) {
                for (auto& address : m_Computer->uniqueAddresses()) {
                    if (isInterruptionRequested()) {
                        return;
                    }

                    if (tryPollComputer(&nam, address, stateChanged)) {
                        if (!wasOnline) {
                            qInfo() << m_Computer->name << "is now online at" << m_Computer->activeAddress.toString();
                        }
                        online = true;
                        break;
                    }
                }
            }

            // Check if we failed after all retry attempts
            // Note: we don't need to acquire the read lock here,
            // because we're on the writing thread.
            if (!online && m_Computer->state != NvComputer::CS_OFFLINE) {
                qInfo() << m_Computer->name << "is now offline";
                m_Computer->state = NvComputer::CS_OFFLINE;
                stateChanged = true;
            }

            // Grab the applist if it's empty or it's been long enough that we need to refresh
            pollsSinceLastAppListFetch++;
            if (m_Computer->state == NvComputer::CS_ONLINE &&
                    m_Computer->pairState == NvComputer::PS_PAIRED &&
                    (m_Computer->appList.isEmpty() || pollsSinceLastAppListFetch >= POLLS_PER_APPLIST_FETCH)) {
                // Notify prior to the app list poll since it may take a while, and we don't
                // want to delay onlining of a machine, especially if we already have a cached list.
                if (stateChanged) {
                    emit computerStateChanged(m_Computer);
                    stateChanged = false;
                }

                if (updateAppList(&nam, stateChanged)) {
                    pollsSinceLastAppListFetch = 0;
                }
            }

            if (stateChanged) {
                // Tell anyone listening that we've changed state
                emit computerStateChanged(m_Computer);
            }

            // Wait a bit to poll again, but do it in 100 ms chunks
            // so we can be interrupted reasonably quickly.
            // FIXME: QWaitCondition would be better.
            for (int i = 0; i < 30 && !isInterruptionRequested(); i++) {
                QThread::msleep(100);
            }
        }
    }

signals:
   void computerStateChanged(NvComputer* computer);

private:
    NvComputer* m_Computer;
};

ComputerManager::ComputerManager(StreamingPreferences* prefs)
    : m_Prefs(prefs),
      m_PollingRef(0),
      m_NeedsDelayedFlush(false)
{
    QSettings settings;

    // If there's a hosts backup copy, we must have failed to commit
    // a previous update before exiting. Restore the backup now.
    int hosts = settings.beginReadArray(SER_HOSTS_BACKUP);
    if (hosts == 0) {
        // If there's no host backup, read from the primary location.
        settings.endArray();
        hosts = settings.beginReadArray(SER_HOSTS);
    }

    // Inflate our hosts from QSettings
    for (int i = 0; i < hosts; i++) {
        settings.setArrayIndex(i);
        NvComputer* computer = new NvComputer(settings);
        m_KnownHosts[computer->uuid] = computer;
        m_LastSerializedHosts[computer->uuid] = *computer;
    }
    settings.endArray();

    // MiniStream: 启动时不再联网拉取 GFE 兼容性数据（保持离线可用）。
    // CompatFetcher::isGfeVersionSupported() 在无缓存时默认返回“支持”。

    // Start the delayed flush thread to handle saveHosts() calls
    m_DelayedFlushThread = new DelayedFlushThread(this);
    m_DelayedFlushThread->start();

    // To quit in a timely manner, we must block additional requests
    // after we receive the aboutToQuit() signal. This is necessary
    // because NvHTTP uses aboutToQuit() to abort requests in progress
    // while quitting, however this is a one time signal - additional
    // requests would not be aborted and block termination.
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &ComputerManager::handleAboutToQuit);
}

ComputerManager::~ComputerManager()
{
    // Stop the delayed flush thread before acquiring the lock in write mode
    // to avoid deadlocking with a flush that needs the lock in read mode.
    {
        // Wake the delayed flush thread
        m_DelayedFlushThread->requestInterruption();
        m_DelayedFlushCondition.wakeOne();

        // Wait for it to terminate (and finish any pending flush)
        m_DelayedFlushThread->wait();
        delete m_DelayedFlushThread;

        // Delayed flushes should have completed by now
        Q_ASSERT(!m_NeedsDelayedFlush);
    }

    QWriteLocker lock(&m_Lock);

    // Interrupt polling
    for (ComputerPollingEntry* entry : std::as_const(m_PollEntries)) {
        entry->interrupt();
    }

    // Delete all polling entries (and associated threads)
    for (ComputerPollingEntry* entry : std::as_const(m_PollEntries)) {
        delete entry;
    }

    // Destroy all NvComputer objects now that polling is halted
    for (NvComputer* computer : std::as_const(m_KnownHosts)) {
        delete computer;
    }
}

void DelayedFlushThread::run() {
    for (;;) {
        // Wait for a delayed flush request or an interruption
        {
            QMutexLocker locker(&m_ComputerManager->m_DelayedFlushMutex);

            while (!QThread::currentThread()->isInterruptionRequested() && !m_ComputerManager->m_NeedsDelayedFlush) {
                m_ComputerManager->m_DelayedFlushCondition.wait(&m_ComputerManager->m_DelayedFlushMutex);
            }

            // Bail without flushing if we woke up for an interruption alone.
            // If we have both an interruption and a flush request, do the flush.
            if (!m_ComputerManager->m_NeedsDelayedFlush) {
                Q_ASSERT(QThread::currentThread()->isInterruptionRequested());
                break;
            }

            // Reset the delayed flush flag to ensure any racing saveHosts() call will set it again
            m_ComputerManager->m_NeedsDelayedFlush = false;

            // Update the last serialized hosts map under the delayed flush mutex
            m_ComputerManager->m_LastSerializedHosts.clear();
            for (const NvComputer* computer : std::as_const(m_ComputerManager->m_KnownHosts)) {
                // Copy the current state of the NvComputer to allow us to check later if we need
                // to serialize it again when attribute updates occur.
                QReadLocker computerLock(&computer->lock);
                m_ComputerManager->m_LastSerializedHosts[computer->uuid] = *computer;
            }
        }

        // Perform the flush
        {
            QSettings settings;

            // First, write to the backup location
            settings.beginWriteArray(SER_HOSTS_BACKUP);
            {
                QReadLocker lock(&m_ComputerManager->m_Lock);
                int i = 0;
                for (const NvComputer* computer : std::as_const(m_ComputerManager->m_KnownHosts)) {
                    settings.setArrayIndex(i++);
                    computer->serialize(settings, false);
                }
            }
            settings.endArray();

            // Next, write to the primary location
            settings.remove(SER_HOSTS);
            settings.beginWriteArray(SER_HOSTS);
            {
                QReadLocker lock(&m_ComputerManager->m_Lock);
                int i = 0;
                for (const NvComputer* computer : std::as_const(m_ComputerManager->m_KnownHosts)) {
                    settings.setArrayIndex(i++);
                    computer->serialize(settings, true);
                }
            }
            settings.endArray();

            // Finally, delete the backup copy
            settings.remove(SER_HOSTS_BACKUP);
        }
    }
}

void ComputerManager::saveHosts()
{
    Q_ASSERT(m_DelayedFlushThread != nullptr && m_DelayedFlushThread->isRunning());

    // Punt to a worker thread because QSettings on macOS can take ages (> 500 ms)
    // to persist our host list to disk (especially when a host has a bunch of apps).
    QMutexLocker locker(&m_DelayedFlushMutex);
    m_NeedsDelayedFlush = true;
    m_DelayedFlushCondition.wakeOne();
}

void ComputerManager::startPolling()
{
    QWriteLocker lock(&m_Lock);

    if (++m_PollingRef > 1) {
        return;
    }

    // MiniStream: mDNS 自动发现已移除，主机列表只来自用户手动添加。
    // 这里只对已知主机启动状态轮询线程。

    // Start polling threads for each known host
    QMapIterator<QString, NvComputer*> i(m_KnownHosts);
    while (i.hasNext()) {
        i.next();
        startPollingComputer(i.value());
    }
}

// Must hold m_Lock for write
void ComputerManager::startPollingComputer(NvComputer* computer)
{
    if (m_PollingRef == 0) {
        return;
    }

    ComputerPollingEntry* pollingEntry;

    if (!m_PollEntries.contains(computer->uuid)) {
        pollingEntry = m_PollEntries[computer->uuid] = new ComputerPollingEntry();
    }
    else {
        pollingEntry = m_PollEntries[computer->uuid];
    }

    if (!pollingEntry->isActive()) {
        PcMonitorThread* thread = new PcMonitorThread(computer);
        connect(thread, &PcMonitorThread::computerStateChanged,
                this, &ComputerManager::handleComputerStateChanged);
        pollingEntry->setActiveThread(thread);
        thread->start();
    }
}

void ComputerManager::saveHost(NvComputer *computer)
{
    // If no serializable properties changed, don't bother saving hosts
    QMutexLocker lock(&m_DelayedFlushMutex);
    QReadLocker computerLock(&computer->lock);
    if (!m_LastSerializedHosts.value(computer->uuid).isEqualSerialized(*computer)) {
        // Queue a request for a delayed flush to QSettings outside of the lock
        computerLock.unlock();
        lock.unlock();
        saveHosts();
    }
}

void ComputerManager::handleComputerStateChanged(NvComputer* computer)
{
    emit computerStateChanged(computer);

    if (computer->pendingQuit && computer->currentGameId == 0) {
        computer->pendingQuit = false;
        emit quitAppCompleted(QVariant());
    }

    // Save updates to this host
    saveHost(computer);
}

QVector<NvComputer*> ComputerManager::getComputers()
{
    QReadLocker lock(&m_Lock);

    // Return a sorted host list
    auto hosts = QVector<NvComputer*>::fromList(m_KnownHosts.values());
    std::stable_sort(hosts.begin(), hosts.end(), [](const NvComputer* host1, const NvComputer* host2) {
        return host1->name.toLower() < host2->name.toLower();
    });
    return hosts;
}

class DeferredHostDeletionTask : public QRunnable
{
public:
    DeferredHostDeletionTask(ComputerManager* cm, NvComputer* computer)
        : m_Computer(computer),
          m_ComputerManager(cm) {}

    void run()
    {
        ComputerPollingEntry* pollingEntry;

        // Only do the minimum amount of work while holding the writer lock.
        // We must release it before calling saveHosts().
        {
            QWriteLocker lock(&m_ComputerManager->m_Lock);

            pollingEntry = m_ComputerManager->m_PollEntries.take(m_Computer->uuid);

            m_ComputerManager->m_KnownHosts.remove(m_Computer->uuid);
        }

        // Persist the new host list with this computer deleted
        m_ComputerManager->saveHosts();

        // Delete the polling entry first. This will stop all polling threads too.
        delete pollingEntry;

        // Finally, delete the computer itself. This must be done
        // last because the polling thread might be using it.
        delete m_Computer;
    }

private:
    NvComputer* m_Computer;
    ComputerManager* m_ComputerManager;
};

void ComputerManager::deleteHost(NvComputer* computer)
{
    // Punt to a worker thread to avoid stalling the
    // UI while waiting for the polling thread to die
    QThreadPool::globalInstance()->start(new DeferredHostDeletionTask(this, computer));
}

void ComputerManager::renameHost(NvComputer* computer, QString name)
{
    {
        QWriteLocker lock(&computer->lock);

        computer->name = name;
        computer->hasCustomName = true;
    }

    // Notify the UI of the state change
    handleComputerStateChanged(computer);
}

void ComputerManager::clientSideAttributeUpdated(NvComputer* computer)
{
    // Notify the UI of the state change
    handleComputerStateChanged(computer);
}

void ComputerManager::handleAboutToQuit()
{
    QReadLocker lock(&m_Lock);

    // Interrupt polling threads immediately, so they
    // avoid making additional requests while quitting
    for (ComputerPollingEntry* entry : std::as_const(m_PollEntries)) {
        entry->interrupt();
    }
}

class PendingPairingTask : public QObject, public QRunnable
{
    Q_OBJECT

public:
    PendingPairingTask(ComputerManager* computerManager, NvComputer* computer, QString pin)
        : m_ComputerManager(computerManager),
          m_Computer(computer),
          m_Pin(pin)
    {
        connect(this, &PendingPairingTask::pairingCompleted,
                computerManager, &ComputerManager::pairingCompleted);
    }

signals:
    void pairingCompleted(NvComputer* computer, QString error);

private:
    void run()
    {
        NvPairingManager pairingManager(m_Computer);

        // ---- MiniStream：自动配对（失败则退回原来的手动输 PIN 流程）----
        //
        // Sunshine 的 pair?phrase=getservercert 会一直挂住等管理员输 PIN，
        // 所以 approve 必须在**另一个线程**里跟 pair() 并行跑：
        //   pair() 发起 -> Sunshine 登记 pending -> 后台线程 GET /api/pin 拿到 id
        //   -> POST approve(同一个 PIN) -> pair() 的请求随即返回，继续走完协议。
        QString autoPairError;
        bool autoPairAttempted = false;
        SunshineAutoApprover* approver = nullptr;

        MslinkTunnel* tunnel = MslinkTunnel::get();
        if (tunnel == nullptr) {
            qInfo() << "MiniStream: auto pairing skipped (no MslinkTunnel singleton)";
        }
        else if (!tunnel->autoPairReady()) {
            // 这条日志很重要：没有它，autopair 只会在「配置缺密码」时静默退回手动 PIN，
            // 让人误以为是代码没跑。
            qInfo() << "MiniStream: auto pairing skipped - mslink.ini 里没有 sunshine_pass"
                    << "(自动配对账号/密码没填)";
        }
        else {
            // 目标地址优先用 Moonlight 实际连接的那个地址：
            //   隧道模式 = 隧道在本机监听的地址（如 127.0.0.2），47990 也走同一条隧道；
            //   直连模式 = 被控端自己的地址。
            QString host = pairingHostAddress(m_Computer);
            if (host.isEmpty()) {
                host = tunnel->autoPairHost();
            }

            if (host.isEmpty()) {
                qInfo() << "MiniStream: auto pairing skipped (no host address resolved)";
            }
            else {
                autoPairAttempted = true;
                qInfo() << "MiniStream: auto pairing via" << host
                        << "user" << tunnel->sunshineUser()
                        << "tunnelActive" << MslinkBridge::active();
                approver = new SunshineAutoApprover(host, tunnel->sunshineUser(),
                                                    tunnel->sunshinePassword(), m_Pin,
                                                    QStringLiteral("MiniStream"));
                approver->start();
            }
        }

        try {
           NvPairingManager::PairState result = pairingManager.pair(m_Computer->appVersion, m_Pin, m_Computer->serverCert);

           if (approver != nullptr) {
               approver->stop();
               autoPairError = approver->error();
               qInfo() << "MiniStream: auto pairing finished, ok =" << approver->succeeded()
                       << "error =" << autoPairError;
               delete approver;
               approver = nullptr;
           }

           switch (result)
           {
           case NvPairingManager::PairState::PIN_WRONG:
               // 自动配对失败时给出具体原因（密码错 / 端口不通 / 无 pending），
               // 比笼统的「PIN 不对」对用户有用得多。
               if (autoPairAttempted && !autoPairError.isEmpty()) {
                   emit pairingCompleted(m_Computer, autoPairError);
               }
               else {
                   emit pairingCompleted(m_Computer, tr("The PIN from the PC didn't match. Please try again."));
               }
               break;
           case NvPairingManager::PairState::FAILED:
               if (m_Computer->currentGameId != 0) {
                   emit pairingCompleted(m_Computer, tr("You cannot pair while a previous session is still running on the host PC. Quit any running games or reboot the host PC, then try pairing again."));
               }
               else {
                   emit pairingCompleted(m_Computer, tr("Pairing failed. Please try again."));
               }
               break;
           case NvPairingManager::PairState::ALREADY_IN_PROGRESS:
               emit pairingCompleted(m_Computer, tr("Another pairing attempt is already in progress."));
               break;
           case NvPairingManager::PairState::PAIRED:
               // Persist the newly pinned server certificate for this host
               m_ComputerManager->saveHost(m_Computer);

               emit pairingCompleted(m_Computer, nullptr);
               break;
           }
        } catch (const GfeHttpResponseException& e) {
            emit pairingCompleted(m_Computer, tr("GeForce Experience returned error: %1").arg(e.toQString()));
        } catch (const QtNetworkReplyException& e) {
            emit pairingCompleted(m_Computer, e.toQString());
        }
    }

    ComputerManager* m_ComputerManager;
    NvComputer* m_Computer;
    QString m_Pin;
};

void ComputerManager::pairHost(NvComputer* computer, QString pin)
{
    // Punt to a worker thread to avoid stalling the
    // UI while waiting for pairing to complete
    PendingPairingTask* pairing = new PendingPairingTask(this, computer, pin);
    QThreadPool::globalInstance()->start(pairing);
}

class PendingQuitTask : public QObject, public QRunnable
{
    Q_OBJECT

public:
    PendingQuitTask(ComputerManager* computerManager, NvComputer* computer)
        : m_Computer(computer)
    {
        connect(this, &PendingQuitTask::quitAppFailed,
                computerManager, &ComputerManager::quitAppCompleted);
    }

signals:
    void quitAppFailed(QString error);

private:
    void run()
    {
        NvHTTP http(m_Computer);

        try {
            if (m_Computer->currentGameId != 0) {
                http.quitApp();
            }
        } catch (const GfeHttpResponseException& e) {
            {
                QWriteLocker lock(&m_Computer->lock);
                m_Computer->pendingQuit = false;
            }
            if (e.getStatusCode() == 599) {
                // 599 is a special code we make a custom message for
                emit quitAppFailed(tr("The running game wasn't started by this PC. "
                                      "You must quit the game on the host PC manually or use the device that originally started the game."));
            }
            else {
                emit quitAppFailed(e.toQString());
            }
        } catch (const QtNetworkReplyException& e) {
            {
                QWriteLocker lock(&m_Computer->lock);
                m_Computer->pendingQuit = false;
            }
            emit quitAppFailed(e.toQString());
        }
    }

    NvComputer* m_Computer;
};

void ComputerManager::quitRunningApp(NvComputer* computer)
{
    QWriteLocker lock(&computer->lock);
    computer->pendingQuit = true;

    PendingQuitTask* quit = new PendingQuitTask(this, computer);
    QThreadPool::globalInstance()->start(quit);
}

void ComputerManager::stopPollingAsync()
{
    QWriteLocker lock(&m_Lock);

    Q_ASSERT(m_PollingRef > 0);
    if (--m_PollingRef > 0) {
        return;
    }

    // Interrupt all threads, but don't wait for them to terminate
    for (ComputerPollingEntry* entry : std::as_const(m_PollEntries)) {
        entry->interrupt();
    }
}

void ComputerManager::addNewHostManually(QString address)
{
    QUrl url = QUrl::fromUserInput("moonlight://" + address);
    if (url.isValid() && !url.host().isEmpty() && url.scheme() == "moonlight") {
        // If there wasn't a port specified, use the default
        addNewHost(NvAddress(url.host(), url.port(DEFAULT_HTTP_PORT)), false);
    }
    else if (QHostAddress(address).protocol() == QAbstractSocket::IPv6Protocol) {
        // The user specified an IPv6 literal without URL escaping, so use the default port
        addNewHost(NvAddress(address, DEFAULT_HTTP_PORT), false);
    }
    else {
        emit computerAddCompleted(false, false);
    }
}

class PendingAddTask : public QObject, public QRunnable
{
    Q_OBJECT

public:
    PendingAddTask(ComputerManager* computerManager, NvAddress address)
        : m_ComputerManager(computerManager),
          m_Address(address),
          m_AboutToQuit(false)
    {
        connect(this, &PendingAddTask::computerAddCompleted,
                computerManager, &ComputerManager::computerAddCompleted);
        connect(this, &PendingAddTask::computerStateChanged,
                computerManager, &ComputerManager::handleComputerStateChanged);
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                this, &PendingAddTask::handleAboutToQuit);
    }

signals:
    void computerAddCompleted(QVariant success, QVariant detectedPortBlocking);

    void computerStateChanged(NvComputer* computer);

private:
    void handleAboutToQuit()
    {
        m_AboutToQuit = true;
    }

    QString fetchServerInfo(NvHTTP& http)
    {
        QString serverInfo;

        // Do nothing if we're quitting
        if (m_AboutToQuit) {
            return QString();
        }

        try {
            // There's a race condition between GameStream servers becoming ready
            // and the HTTPS server being ready to respond to our queries. To work
            // around this issue, we will issue the request again after a few seconds if
            // we see a ServiceUnavailableError error.
            try {
                serverInfo = http.getServerInfo(NvHTTP::NVLL_VERBOSE);
            } catch (const QtNetworkReplyException& e) {
                if (e.getError() == QNetworkReply::ServiceUnavailableError) {
                    qWarning() << "Retrying request in 5 seconds after ServiceUnavailableError";
                    QThread::sleep(5);
                    serverInfo = http.getServerInfo(NvHTTP::NVLL_VERBOSE);
                    qInfo() << "Retry successful";
                }
                else {
                    // Rethrow other errors
                    throw e;
                }
            }
            return serverInfo;
        } catch (...) {
            unsigned int portTestResult;

            if (m_ComputerManager->m_Prefs->detectNetworkBlocking) {
                // We failed to connect to the specified PC. Let's test to make sure this network
                // isn't blocking Moonlight, so we can tell the user about it.
                portTestResult = LiTestClientConnectivity("qt.conntest.moonlight-stream.org", 443,
                                                          ML_PORT_FLAG_TCP_47984 | ML_PORT_FLAG_TCP_47989);
            }
            else {
                portTestResult = 0;
            }

            emit computerAddCompleted(false, portTestResult != 0 && portTestResult != ML_TEST_RESULT_INCONCLUSIVE);
            return QString();
        }
    }

    void run()
    {
        // Use the placeholder UID for the initial poll, then we'll switch to the real one if it's not GFE
        NvHTTP http(m_Address, 0, QSslCertificate(), false);

        qInfo() << "Processing new PC at" << m_Address.toString() << "from user";

        // Perform initial serverinfo fetch over HTTP since we don't know which cert to use
        QString serverInfo = fetchServerInfo(http);
        if (serverInfo.isEmpty()) {
            return;
        }

        // Create initial newComputer using HTTP serverinfo with no pinned cert
        NvComputer* newComputer = new NvComputer(http, serverInfo);
        http.setTrueUid(!newComputer->isNvidiaServerSoftware);

        // Check if we have a record of this host UUID to pull the pinned cert
        NvComputer* existingComputer;
        {
            QReadLocker lock(&m_ComputerManager->m_Lock);
            existingComputer = m_ComputerManager->m_KnownHosts.value(newComputer->uuid);
            if (existingComputer != nullptr) {
                http.setServerCert(existingComputer->serverCert);
            }
        }

        // Fetch serverinfo again over HTTPS with the pinned cert
        if (existingComputer != nullptr) {
            Q_ASSERT(http.httpsPort() != 0);
            serverInfo = fetchServerInfo(http);
            if (serverInfo.isEmpty()) {
                return;
            }

            // Update the polled computer with the HTTPS information
            NvComputer httpsComputer(http, serverInfo);
            newComputer->update(httpsComputer);
        }

        // Update addresses for a manually-added host
        newComputer->manualAddress = m_Address;

        QHostAddress hostAddress(m_Address.address());
        bool addressIsSiteLocalV4 =
                hostAddress.isInSubnet(QHostAddress("10.0.0.0"), 8) ||
                hostAddress.isInSubnet(QHostAddress("172.16.0.0"), 12) ||
                hostAddress.isInSubnet(QHostAddress("192.168.0.0"), 16);

        {
            // Check if this PC already exists using opportunistic read lock
            m_ComputerManager->m_Lock.lockForRead();
            NvComputer* existingComputer = m_ComputerManager->m_KnownHosts.value(newComputer->uuid);

            // If it doesn't already exist, convert to a write lock in preparation for updating.
            //
            // NB: ComputerManager's lock protects the host map itself, not the elements inside.
            // Those are protected by their individual locks. Since we only mutate the map itself
            // when the PC doesn't exist, we need the lock in write-mode for that case only.
            if (existingComputer == nullptr) {
                m_ComputerManager->m_Lock.unlock();
                m_ComputerManager->m_Lock.lockForWrite();

                // Since we had to unlock to lock for write, someone could have raced and added
                // this PC before us. We have to check again whether it already exists.
                existingComputer = m_ComputerManager->m_KnownHosts.value(newComputer->uuid);
            }

            if (existingComputer != nullptr) {
                // Fold it into the existing PC
                bool changed = existingComputer->update(*newComputer);
                delete newComputer;

                // Drop the lock before notifying
                m_ComputerManager->m_Lock.unlock();

                // Let the caller know it succeeded
                emit computerAddCompleted(true, false);

                // Tell our client if something changed
                if (changed) {
                    qInfo() << existingComputer->name << "is now at" << existingComputer->activeAddress.toString();
                    emit computerStateChanged(existingComputer);
                }
            }
            else {
                // Store this in our active sets
                m_ComputerManager->m_KnownHosts[newComputer->uuid] = newComputer;

                // Start polling if enabled (write lock required)
                m_ComputerManager->startPollingComputer(newComputer);

                // Drop the lock before notifying
                m_ComputerManager->m_Lock.unlock();

                // If this is a RFC 1918 IPv4 address and not a VPN,
                // do the STUN request now to populate an external address.
                if (addressIsSiteLocalV4 && newComputer->getActiveAddressReachability() != NvComputer::RI_VPN) {
                    quint32 addr;
                    int err = LiFindExternalAddressIP4("stun.moonlight-stream.org", 3478, &addr);
                    if (err == 0) {
                        newComputer->setRemoteAddress(QHostAddress(qFromBigEndian(addr)));
                    }
                    else {
                        qWarning() << "STUN failed to get WAN address:" << err;
                    }
                }

                // Let the caller know it succeeded
                emit computerAddCompleted(true, false);

                // Tell our client about this new PC
                emit computerStateChanged(newComputer);
            }
        }
    }

    ComputerManager* m_ComputerManager;
    NvAddress m_Address;
    bool m_AboutToQuit;
};

void ComputerManager::addNewHost(NvAddress address, bool mdns, QString name, NvAddress mdnsIpv6Address)
{
    // MiniStream: 同一个地址重复添加会堆出一排一模一样的磁贴。
    // 「一键连接」每次启动都会自动补一台 127.0.0.1，所以这里必须先查重：
    // 已经存在相同 manualAddress 就直接返回，不再新建。
    {
        QReadLocker locker(&m_Lock);
        for (NvComputer* c : m_KnownHosts.values()) {
            if (c && c->manualAddress == address) {
                emit computerAddCompleted(false, true);
                return;
            }
        }
    }

    // Punt to a worker thread to avoid stalling the
    // UI while waiting for serverinfo query to complete
    PendingAddTask* addTask = new PendingAddTask(this, address);
    QThreadPool::globalInstance()->start(addTask);
}

QString ComputerManager::generatePinString()
{
    return QString::asprintf("%04u", QRandomGenerator::system()->bounded(10000));
}

#include "computermanager.moc"

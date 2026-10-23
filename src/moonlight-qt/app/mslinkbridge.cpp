// =============================================================================
//  mslinkbridge.cpp —— 2 端口复用通道的客户端集成
// =============================================================================
#include "mslinkbridge.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QLoggingCategory>
#include <QTextStream>

#include <memory>

#include "mslink/relay.h"

namespace MslinkBridge {

namespace {

std::unique_ptr<mslink::Relay> g_relay;
QString g_lastError;
bool g_started = false;

// 极简 ini 解析：key = value，# 或 ; 开头为注释
QHash<QString, QString> parseIni(const QString& path) {
    QHash<QString, QString> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;
    QTextStream ts(&f);
    bool first = true;
    while (!ts.atEnd()) {
        QString line = ts.readLine();
        if (first) {
            first = false;
            if (!line.isEmpty() && line.at(0) == QChar(0xFEFF))
                line = line.mid(1);   // 去掉 UTF-8 BOM（记事本存的文件会带）
        }
        line = line.trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith('#') || line.startsWith(';')) continue;
        if (line.startsWith('[')) continue;   // section 名忽略
        const int eq = line.indexOf('=');
        if (eq <= 0) continue;
        const QString k = line.left(eq).trimmed().toLower();
        QString v = line.mid(eq + 1).trimmed();
        // 去掉行尾注释
        const int hash = v.indexOf('#');
        if (hash >= 0) v = v.left(hash).trimmed();
        out.insert(k, v);
    }
    return out;
}

// 日志优先写 exe 同目录（便携版就是这样用的）；如果那里不可写
// （例如被装进 Program Files 且没有写权限），退到 %TEMP%\MiniStream\，
// 避免「日志写不出来，现场完全没法排障」。
QString logPath() {
    static QString cached;
    if (!cached.isEmpty()) return cached;
    const QString local = QCoreApplication::applicationDirPath() + "/mslink.log";
    QFile probe(local);
    if (probe.open(QIODevice::Append | QIODevice::Text)) {
        probe.close();
        cached = local;
        return cached;
    }
    const QString dir = QDir(QDir::tempPath()).filePath("MiniStream");
    QDir().mkpath(dir);
    cached = QDir(dir).filePath("mslink.log");
    return cached;
}

// 日志同时写一份到 mslink.log（Moonlight 自己的日志在 UI 里，
// 现场排障时能直接看文件更方便）
void logSink(const char* line, void*) {
    const QString s = QString::fromUtf8(line);
    qInfo().noquote() << "[mslink]" << s;
    QFile f(logPath());
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&f);
        ts << QDateTime::currentDateTime().toString("hh:mm:ss.zzz") << " " << s << "\n";
    }
}

void mark(const QString& s) {
    QFile f(logPath());
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&f);
        ts << QDateTime::currentDateTime().toString("hh:mm:ss.zzz") << " [bridge] " << s << "\n";
    }
}

// 真正的启动实现：ini 与 QML 两条路都走它
bool startClientImpl(const QString& server, int tcpPort, int udpPort,
                     const QString& password, const QString& listen,
                     const QString& tcpPortsOverride, const QString& udpPortsOverride,
                     QString* err) {
    g_lastError.clear();
    if (g_started && g_relay) return true;

    if (server.isEmpty() || tcpPort <= 0 || udpPort <= 0 || tcpPort > 65535 || udpPort > 65535) {
        g_lastError = QStringLiteral("服务器地址或端口不合法");
        if (err) *err = g_lastError;
        mark(g_lastError);
        return false;
    }

    mslink::ClientConfig cc;
    cc.serverHost = server.toStdString();
    cc.serverTcpPort = uint16_t(tcpPort);
    cc.serverUdpPort = uint16_t(udpPort);
    cc.password = password.toStdString();
    cc.listenAddr = listen.toStdString();

    // 允许自定义本地监听端口（默认就是 Moonlight 的 6 个标准端口）
    if (!tcpPortsOverride.isEmpty()) {
        QVector<uint16_t> v;
        for (const QString& s : tcpPortsOverride.split(',', Qt::SkipEmptyParts)) {
            const int p = s.trimmed().toInt();
            if (p > 0 && p < 65536) v.append(uint16_t(p));
        }
        if (!v.isEmpty()) cc.tcpListenPorts.assign(v.begin(), v.end());
    }
    if (!udpPortsOverride.isEmpty()) {
        QVector<uint16_t> v;
        for (const QString& s : udpPortsOverride.split(',', Qt::SkipEmptyParts)) {
            const int p = s.trimmed().toInt();
            if (p > 0 && p < 65536) v.append(uint16_t(p));
        }
        if (!v.isEmpty()) cc.udpListenPorts.assign(v.begin(), v.end());
    }

    g_relay.reset(new mslink::Relay());
    g_relay->set_log_sink(&logSink, nullptr);

    mark(QStringLiteral("调用 start_client: %1:%2/%3 listen=%4").arg(server).arg(tcpPort).arg(udpPort).arg(listen));
    std::string serr;
    if (!g_relay->start_client(cc, serr)) {
        g_lastError = QString::fromUtf8(serr.c_str());
        if (err) *err = g_lastError;
        qWarning().noquote() << "[mslink] 隧道启动失败:" << g_lastError;
        g_relay.reset();
        return false;
    }

    g_started = true;
    mark(QStringLiteral("start_client 返回成功"));
    qInfo().noquote() << QStringLiteral("[mslink] 2 端口隧道已启动 → %1 (TCP %2 / UDP %3)，"
                                        "Moonlight 里请添加主机 127.0.0.1")
                             .arg(server).arg(tcpPort).arg(udpPort);
    return true;
}

} // namespace

bool startFromIni(const QString& iniPath, QString* err) {
    mark(QStringLiteral("startFromIni 进入，ini=%1 存在=%2")
             .arg(iniPath).arg(QFileInfo::exists(iniPath) ? "是" : "否"));
    g_lastError.clear();
    if (g_started) return true;

    if (!QFileInfo::exists(iniPath)) {
        // 没有配置文件 = 不用隧道（普通局域网直连模式），不是错误
        return false;
    }

    const auto kv = parseIni(iniPath);
    const QString server = kv.value("server");
    const int tcpPort = kv.value("tcp_port").toInt();
    const int udpPort = kv.value("udp_port").toInt();
    const QString password = kv.value("password");
    const QString listen = kv.value("listen", "127.0.0.1");

    if (server.isEmpty() || tcpPort <= 0 || udpPort <= 0) {
        mark(QStringLiteral("配置不完整: server=%1 tcp=%2 udp=%3").arg(server).arg(tcpPort).arg(udpPort));
        g_lastError = QStringLiteral("mslink.ini 缺少 server / tcp_port / udp_port");
        if (err) *err = g_lastError;
        qWarning().noquote() << "[mslink]" << g_lastError;
        return false;
    }

    // 自动配对需要访问被控端的 Sunshine 管理网页（47990）。隧道默认只带 3 个 TCP 端口，
    // 这里在用户没显式配置 tcp_ports 时把 47990 也带上；如果本机 47990 已被占用
    // （例如本机自己也装了 Sunshine），退回标准 3 端口，自动配对会自动改用直连地址。
    const QString userTcpPorts = kv.value("tcp_ports");
    const bool defaultPorts = userTcpPorts.isEmpty();
    const QString tcpPorts = defaultPorts
            ? QStringLiteral("47989,47984,48010,47990")
            : userTcpPorts;

    if (startClientImpl(server, tcpPort, udpPort, password, listen,
                        tcpPorts, kv.value("udp_ports"), err)) {
        return true;
    }

    if (defaultPorts) {
        mark(QStringLiteral("带 47990 的隧道启动失败（%1），降级为 3 个标准端口").arg(g_lastError));
        return startClientImpl(server, tcpPort, udpPort, password, listen,
                               QStringLiteral("47989,47984,48010"), kv.value("udp_ports"), err);
    }
    return false;
}

void stop() {
    if (g_relay) {
        g_relay->stop();
        g_relay.reset();
    }
    g_started = false;
}

bool active() { return g_started && g_relay && g_relay->running(); }

bool tunnelUp() {
    if (!g_started || !g_relay) return false;
    return g_relay->stats().tunnelUp;
}

// P2-5: 从 relay 读取真实 UDP 状态
bool udpDirectOk() {
    if (!g_started || !g_relay) return false;
    return g_relay->udpDirectOk();
}

bool udpViaTcp() {
    if (!g_started || !g_relay) return false;
    return g_relay->udpViaTcp();
}

QString statusText() {
    if (!g_started || !g_relay) return QStringLiteral("隧道未启用");
    const mslink::RelayStats s = g_relay->stats();
    return QStringLiteral("隧道 %1 · TCP流 %2 · UDP↑%3 ↓%4")
        .arg(s.tunnelUp ? "已连接" : "连接中")
        .arg(qulonglong(s.tcpStreamsOpened))
        .arg(qulonglong(s.udpPacketsUp))
        .arg(qulonglong(s.udpPacketsDown));
}

} // namespace MslinkBridge

// =============================================================================
//  MslinkTunnel
// =============================================================================
MslinkTunnel* MslinkTunnel::get() {
    static MslinkTunnel* s_inst = nullptr;
    if (!s_inst) {
        s_inst = new MslinkTunnel();
        s_inst->loadConfigFromIni();   // 让 QML 一打开就能看到上次填的地址
    }
    return s_inst;
}

MslinkTunnel::MslinkTunnel(QObject* parent) : QObject(parent) {
    // 状态变化每 1 秒推给 QML 一次（隧道握手/重连是异步的）
    m_statusTimer.setInterval(1000);
    connect(&m_statusTimer, &QTimer::timeout, this, [this]() {
        const bool up = MslinkBridge::tunnelUp();

        // 正在连接：等远端认证，超时就明确报错（本地监听成功 ≠ 远端通了）
        if (m_connecting) {
            if (up) {
                m_connecting = false;
                m_lastError.clear();
                m_wasRunning = true;
                emit stateChanged();
                return;
            }
            if (QDateTime::currentMSecsSinceEpoch() > m_connectDeadlineMs) {
                m_connecting = false;
                m_lastError = QStringLiteral(
                    "连接超时：本机已就绪，但连不上被控端。请检查地址/端口是否正确、"
                    "被控端是否在运行、口令是否一致（详见 mslink.log）。");
                emit stateChanged();
                return;
            }
            emit stateChanged();   // 仍倒计时中，刷新界面上的"正在连接"
            return;
        }

        if (up != m_wasRunning) {
            m_wasRunning = up;
            emit stateChanged();
        } else if (up) {
            emit stateChanged();   // 刷新流量数字
        }
    });
    m_statusTimer.start();
}

QString MslinkTunnel::iniPath() const {
    return QCoreApplication::applicationDirPath() + "/mslink.ini";
}

void MslinkTunnel::loadConfigFromIni() {
    QFile f(iniPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qInfo().noquote() << QStringLiteral("[MiniStream] 没有 mslink.ini（自动配对未配置）");
        return;
    }
    QTextStream ts(&f);
    bool first = true;
    while (!ts.atEnd()) {
        QString line = ts.readLine();
        if (first) {
            first = false;
            if (!line.isEmpty() && line.at(0) == QChar(0xFEFF)) line = line.mid(1);
        }
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith(';') || line.startsWith('[')) continue;
        const int eq = line.indexOf('=');
        if (eq <= 0) continue;
        const QString k = line.left(eq).trimmed().toLower();
        const QString v = line.mid(eq + 1).trimmed();
        if      (k == "server")   m_server = v;
        else if (k == "tcp_port") m_tcpPort = v.toInt();
        else if (k == "udp_port") m_udpPort = v.toInt();
        else if (k == "password") m_password = v;
        else if (k == "listen")   m_listen = v.isEmpty() ? QStringLiteral("127.0.0.1") : v;
        // 自动配对：被控端 Sunshine 管理网页的 Basic Auth 凭据
        else if (k == "sunshine_user") m_sunshineUser = v.isEmpty() ? QStringLiteral("admin") : v;
        else if (k == "sunshine_pass") m_sunshinePassword = v;
    }
    // 一行日志，GUI / CLI 两条路径都能看出自动配对到底有没有凭据
    qInfo().noquote() << QStringLiteral("[MiniStream] mslink.ini 已读取：server=%1 listen=%2 "
                                        "sunshine 凭据=%3")
                             .arg(m_server.isEmpty() ? QStringLiteral("(无)") : m_server,
                                  m_listen,
                                  m_sunshinePassword.isEmpty() ? QStringLiteral("未配置")
                                                               : QStringLiteral("已加载"));
    emit configChanged();
}

void MslinkTunnel::reloadConfig() {
    m_server.clear(); m_tcpPort = 0; m_udpPort = 0; m_password.clear();
    loadConfigFromIni();
}

void MslinkTunnel::persistConfig() {
    const QString path = iniPath();
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        MslinkBridge::mark(QStringLiteral("写 mslink.ini 失败: %1").arg(path));
        return;
    }
    QTextStream ts(&f);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    ts.setEncoding(QStringConverter::Utf8);
#endif
    ts << "; MiniStream 连接配置（由程序自动生成，可手工修改）\n"
       << "server = "   << m_server   << "\n"
       << "tcp_port = " << m_tcpPort  << "\n"
       << "udp_port = " << m_udpPort  << "\n"
       << "password = " << m_password << "\n"
       << "listen = "   << m_listen   << "\n"
       << "; 自动配对用：被控端 Sunshine 管理网页（47990）的账号口令\n"
       << "sunshine_user = " << m_sunshineUser << "\n"
       << "sunshine_pass = " << m_sunshinePassword << "\n";
    f.close();
}

void MslinkTunnel::setSunshineCredentials(const QString& user, const QString& pass) {
    m_sunshineUser = user.trimmed().isEmpty() ? QStringLiteral("admin") : user.trimmed();
    m_sunshinePassword = pass;
    persistConfig();
    emit configChanged();
}

QString MslinkTunnel::autoPairHost() const {
    // 隧道在跑：本机 listen 地址上的 47990 已经被转发到被控端的 Sunshine 管理网页
    if (MslinkBridge::active() && !m_server.isEmpty()) {
        return m_listen.isEmpty() ? QStringLiteral("127.0.0.1") : m_listen;
    }
    // 没有隧道：交给调用方用被控端自己的地址
    return QString();
}

QString MslinkTunnel::connectToServer(const QString& server, int tcpPort,
                                      int udpPort, const QString& password) {
    m_lastError.clear();
    emit stateChanged();

    if (server.trimmed().isEmpty()) {
        m_lastError = QStringLiteral("请填写服务器地址");
        emit stateChanged();
        return m_lastError;
    }
    if (tcpPort <= 0 || tcpPort > 65535 || udpPort <= 0 || udpPort > 65535) {
        m_lastError = QStringLiteral("端口必须是 1–65535");
        emit stateChanged();
        return m_lastError;
    }
    if (MslinkBridge::active()) {
        // 用户改了地址/口令又点了一次「连接」：先停掉旧的再重连
        MslinkBridge::mark(QStringLiteral("重新连接：先停止现有隧道"));
        MslinkBridge::stop();
    }

    // P1-1: 统一配置生成函数。隧道模式下本地监听 127.0.0.1，
    // TCP 端口始终包含 47989,47984,48010,47990（自动配对需要）。
    const QString tcpPorts = QStringLiteral("47989,47984,48010,47990");
    const QString udpPorts = QStringLiteral("47998,47999,48000");

    QString err;
    if (!MslinkBridge::startClientImpl(server.trimmed(), tcpPort, udpPort, password,
                                       QStringLiteral("127.0.0.1"), tcpPorts, udpPorts, &err)) {
        m_lastError = err.isEmpty() ? QStringLiteral("隧道启动失败") : err;
        emit stateChanged();
        return m_lastError;
    }

    m_server = server.trimmed();
    m_tcpPort = tcpPort;
    m_udpPort = udpPort;
    m_password = password;
    persistConfig();
    m_wasRunning = false;
    // 注意：这里只代表「本机监听已就绪」，远端是否认证由状态定时器判定
    m_connecting = MslinkBridge::tunnelUp() ? false : true;
    m_connectDeadlineMs = QDateTime::currentMSecsSinceEpoch() + 12000;
    if (!m_connecting) {
        m_lastError.clear();
        m_wasRunning = true;
    }
    emit configChanged();
    emit stateChanged();
    return QString();
}

void MslinkTunnel::disconnectTunnel() {
    MslinkBridge::stop();
    m_wasRunning = false;
    m_connecting = false;
    emit stateChanged();
}

// P2-6: UDP 直连状态查询
bool MslinkTunnel::udpDirectOk() const {
    return MslinkBridge::udpDirectOk();
}

bool MslinkTunnel::udpViaTcp() const {
    return MslinkBridge::udpViaTcp();
}

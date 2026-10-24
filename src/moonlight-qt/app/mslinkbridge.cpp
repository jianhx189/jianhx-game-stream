#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
// =============================================================================
//  mslinkbridge.cpp —— 2 端口复用通道的客户端集成
// =============================================================================
#include "mslinkbridge.h"
#include "mslinkcredentials.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QHash>
#include <QLoggingCategory>
#include <QStringList>
#include <QTextStream>

#include <memory>

#include "mslink/relay.h"

namespace MslinkBridge {

namespace {

std::unique_ptr<mslink::Relay> g_relay;
QString g_lastError;
bool g_started = false;
// 配置落盘失败时给 statusText() 加一句提示。QML 里隧道连通后不会再显示
// lastError（成功分支优先），状态文字的悬浮提示是唯一不依赖 QML 改动的出口。
QString g_configSaveHint;

void setConfigSaveHint(const QString& hint) { g_configSaveHint = hint; }

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

// 读取 ini 里的一个秘密字段：优先 "dpapi:v1:" 密文，其次旧版明文。
// 返回值是实际可用的明文；usedPlaintext 表示值来自旧明文（可迁移）。
// decodeErr 只在「密文存在但解不开」时非空；这时如果同时还有明文，
// 就退回明文，保证迁移失败/换了 Windows 用户时旧配置至少还能用。
QString readStoredSecret(const QHash<QString, QString>& kv,
                         const QString& encryptedKey, const QString& plainKey,
                         bool* usedPlaintext, QString* decodeErr) {
    if (usedPlaintext) *usedPlaintext = false;
    if (decodeErr) decodeErr->clear();

    const QString plainValue = kv.value(plainKey);
    if (usedPlaintext && !plainValue.isEmpty()) *usedPlaintext = true;

    const QString stored = kv.value(encryptedKey);
    if (!stored.isEmpty()) {
        QString plain;
        QString err;
        if (MslinkCredentials::decodeStored(stored, &plain, nullptr, &err)) {
            if (usedPlaintext) *usedPlaintext = false;   // 密文可用，明文只是遗留
            return plain;
        }
        if (decodeErr) *decodeErr = err;
        return plainValue;   // 解不开就退回明文；没有明文时就是空口令
    }
    return plainValue;
}

struct IniLineReplacement {
    QStringList sourceKeys;   // 这些键（小写）第一次出现的位置换成 line，其余出现删除
    QString line;             // 新的整行内容；空串 = 删除该字段
};

// 按行重写 ini：只动指定的键，其它行（注释、未知键、tcp_ports 等）原样保留。
// 用 QSaveFile 原子落盘，写失败时原文件不动。
bool rewriteIniAtomic(const QString& path, const QList<IniLineReplacement>& reps, QString* err) {
    QFile in(path);
    QByteArray raw;
    if (in.exists()) {
        if (!in.open(QIODevice::ReadOnly)) {
            if (err) *err = QStringLiteral("读不到 %1：%2").arg(path, in.errorString());
            return false;
        }
        raw = in.readAll();
        in.close();
    }

    const bool hadBom = raw.startsWith("\xEF\xBB\xBF");
    const QByteArray body = hadBom ? raw.mid(3) : raw;
    const QString text = QString::fromUtf8(body);
    const QString newline = raw.contains("\r\n") ? QStringLiteral("\r\n") : QStringLiteral("\n");

    QStringList lines = text.split(QLatin1Char('\n'));
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();   // 末尾换行拆出来的空串
    for (QString& l : lines) {
        if (l.endsWith(QLatin1Char('\r')))
            l.chop(1);
    }

    const int repCount = int(reps.size());
    QList<bool> emitted(repCount, false);
    QStringList out;
    out.reserve(lines.size() + reps.size());
    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();
        int repIndex = -1;
        if (!trimmed.isEmpty() && trimmed.at(0) != QLatin1Char('#')
                && trimmed.at(0) != QLatin1Char(';') && trimmed.at(0) != QLatin1Char('[')) {
            const int eq = trimmed.indexOf(QLatin1Char('='));
            if (eq > 0) {
                const QString key = trimmed.left(eq).trimmed().toLower();
                for (int i = 0; i < repCount; ++i) {
                    if (reps.at(i).sourceKeys.contains(key)) {
                        repIndex = i;
                        break;
                    }
                }
            }
        }
        if (repIndex < 0) {
            out.append(line);
            continue;
        }
        if (emitted.at(repIndex))
            continue;   // 同名字段重复出现：只保留第一次替换后的那一行
        emitted[repIndex] = true;
        if (!reps.at(repIndex).line.isEmpty())
            out.append(reps.at(repIndex).line);
    }
    // 文件里原本没有对应行的字段追加到末尾（正常迁移不会走到这里）
    for (int i = 0; i < repCount; ++i) {
        if (!emitted.at(i) && !reps.at(i).line.isEmpty())
            out.append(reps.at(i).line);
    }

    QByteArray output;
    if (hadBom) output.append("\xEF\xBB\xBF", 3);
    output.append(out.join(newline).toUtf8());
    output.append(newline.toUtf8());

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = QStringLiteral("无法写入 %1：%2").arg(path, f.errorString());
        return false;
    }
    if (f.write(output) != output.size()) {
        if (err) *err = QStringLiteral("写入 %1 不完整：%2").arg(path, f.errorString());
        f.cancelWriting();   // 原文件保持不动
        return false;
    }
    if (!f.commit()) {
        if (err) *err = QStringLiteral("保存 %1 失败：%2").arg(path, f.errorString());
        return false;
    }
    return true;
}

// 把 ini 里的旧明文口令迁移成 DPAPI 密文：先加密好两份再一次性原子写入，
// 任何一步失败都不碰原文件（调用方继续用内存里的明文，并如实说明「未受保护」）。
bool migratePlaintextSecrets(const QString& path,
                             bool migratePassword, const QString& password,
                             bool migrateSunshine, const QString& sunshinePassword,
                             QString* err) {
    QList<IniLineReplacement> reps;
    if (migratePassword) {
        IniLineReplacement r;
        r.sourceKeys = QStringList{QStringLiteral("password"),
                                   QStringLiteral("password_dpapi")};
        if (!password.isEmpty()) {
            QString enc;
            if (!MslinkCredentials::encodeSecret(password, &enc, err)) {
                if (err) *err = QStringLiteral("隧道口令加密失败：%1").arg(*err);
                return false;
            }
            r.line = QStringLiteral("password_dpapi = %1").arg(enc);
        }
        reps.append(r);
    }
    if (migrateSunshine) {
        IniLineReplacement r;
        r.sourceKeys = QStringList{QStringLiteral("sunshine_pass"),
                                   QStringLiteral("sunshine_pass_dpapi")};
        if (!sunshinePassword.isEmpty()) {
            QString enc;
            if (!MslinkCredentials::encodeSecret(sunshinePassword, &enc, err)) {
                if (err) *err = QStringLiteral("Sunshine 口令加密失败：%1").arg(*err);
                return false;
            }
            r.line = QStringLiteral("sunshine_pass_dpapi = %1").arg(enc);
        }
        reps.append(r);
    }
    if (reps.isEmpty())
        return true;
    return rewriteIniAtomic(path, reps, err);
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
    const QString listen = kv.value("listen", "127.0.0.1");

    // 口令字段：优先 DPAPI 密文（dpapi:v1:），兼容旧版明文。
    // 这里只读不写 —— 迁移统一交给 MslinkTunnel::loadConfigFromIni()（主配置载入），
    // 避免在 QML 起来之前就把 ini 改掉、迁移一旦出问题连明文都没了。
    bool passwordIsPlaintext = false;
    QString passwordDecodeErr;
    const QString password = readStoredSecret(kv, QStringLiteral("password_dpapi"),
                                              QStringLiteral("password"),
                                              &passwordIsPlaintext, &passwordDecodeErr);
    if (!passwordDecodeErr.isEmpty()) {
        mark(QStringLiteral("警告：mslink.ini 里的隧道口令无法解密（%1）；"
                            "如果文件是从别的 Windows 用户/机器拷来的，请重新填写口令")
                 .arg(passwordDecodeErr));
    } else if (passwordIsPlaintext && !password.isEmpty()) {
        mark(QStringLiteral("提示：本次仍从明文读取隧道口令，将在主配置载入时迁移为 DPAPI 密文"));
    }

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
    QString text = QStringLiteral("隧道 %1 · TCP流 %2 · UDP↑%3 ↓%4")
        .arg(s.tunnelUp ? "已连接" : "连接中")
        .arg(qulonglong(s.tcpStreamsOpened))
        .arg(qulonglong(s.udpPacketsUp))
        .arg(qulonglong(s.udpPacketsDown));
    if (!g_configSaveHint.isEmpty())
        text += QStringLiteral(" · ") + g_configSaveHint;
    return text;
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
                // 配置落盘失败时保留那条提示，让用户知道「连上了，但配置没保存」
                if (!m_configSaveFailed)
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
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dir);
    return dir + "/mslink.ini";
}

void MslinkTunnel::loadConfigFromIni() {
    const QString path = iniPath();
    if (!QFileInfo::exists(path)) {
        qInfo().noquote() << QStringLiteral("[MiniStream] 没有 mslink.ini（自动配对未配置）");
        return;
    }

    const QHash<QString, QString> kv = MslinkBridge::parseIni(path);
    if (kv.isEmpty()) {
        qWarning().noquote() << QStringLiteral("[MiniStream] mslink.ini 读不到内容：%1").arg(path);
        return;
    }

    // 缺字段兼容：只覆盖文件里真正出现的键，其余保留当前值/默认值
    if (kv.contains("server"))   m_server = kv.value("server");
    if (kv.contains("tcp_port")) m_tcpPort = kv.value("tcp_port").toInt();
    if (kv.contains("udp_port")) m_udpPort = kv.value("udp_port").toInt();
    if (kv.contains("listen"))
        m_listen = kv.value("listen").isEmpty() ? QStringLiteral("127.0.0.1") : kv.value("listen");
    // 自动配对：被控端 Sunshine 管理网页的 Basic Auth 凭据
    if (kv.contains("sunshine_user"))
        m_sunshineUser = kv.value("sunshine_user").isEmpty() ? QStringLiteral("admin")
                                                             : kv.value("sunshine_user");

    // 口令字段：优先 dpapi:v1 密文，兼容旧版明文。
    // 是否还需要迁移由「文件里有没有明文行」决定（见下），这里不关心值从哪来。
    QString passwordDecodeErr;
    QString sunshineDecodeErr;

    if (kv.contains("password_dpapi") || kv.contains("password")) {
        m_password = MslinkBridge::readStoredSecret(kv, QStringLiteral("password_dpapi"),
                                                    QStringLiteral("password"),
                                                    nullptr, &passwordDecodeErr);
    }
    if (kv.contains("sunshine_pass_dpapi") || kv.contains("sunshine_pass")) {
        m_sunshinePassword = MslinkBridge::readStoredSecret(kv, QStringLiteral("sunshine_pass_dpapi"),
                                                            QStringLiteral("sunshine_pass"),
                                                            nullptr, &sunshineDecodeErr);
    }

    // 旧版 ini 里还留着明文口令：在「主配置载入」这一处做原子迁移。
    // startFromIni() 只读不迁移，所以不会出现 QML 还没起来、ini 已被改写的窗口。
    // 迁移失败不阻断启动：内存里已经有明文，本次照常可用，但必须如实说明
    // 「文件里仍是明文」，不能让用户以为已经受保护。
    const bool migratePassword = !kv.value("password").isEmpty();
    const bool migrateSunshine = !kv.value("sunshine_pass").isEmpty();
    QString migrationError;
    if (migratePassword || migrateSunshine) {
        QString migErr;
        if (MslinkBridge::migratePlaintextSecrets(path, migratePassword, m_password,
                                                  migrateSunshine, m_sunshinePassword, &migErr)) {
            // 迁移后对应的明文报错已作废（值已经重新加密写回）
            if (migratePassword) passwordDecodeErr.clear();
            if (migrateSunshine) sunshineDecodeErr.clear();
            qInfo().noquote() << QStringLiteral(
                "[MiniStream] mslink.ini 中的明文口令已迁移为 DPAPI 密文（dpapi:v1）");
        } else {
            migrationError = QStringLiteral(
                "mslink.ini 口令迁移失败（%1）。本次仍按明文读取，配置文件未改动——"
                "口令目前仍以明文保存在磁盘上，尚未受 DPAPI 保护。").arg(migErr);
            qWarning().noquote() << "[MiniStream]" << migrationError;
            MslinkBridge::mark(migrationError);
        }
    }

    if (!passwordDecodeErr.isEmpty()) {
        m_lastError = QStringLiteral(
            "mslink.ini 里的隧道口令无法用当前 Windows 用户解密（dpapi:v1）。"
            "如果 ini 是从别的用户或别的机器复制的，请重新填写口令。");
        qWarning().noquote() << QStringLiteral("[MiniStream] %1（%2）").arg(m_lastError,
                                                                           passwordDecodeErr);
    }
    if (!sunshineDecodeErr.isEmpty()) {
        if (m_lastError.isEmpty())
            m_lastError = QStringLiteral(
                "mslink.ini 里的 Sunshine 口令无法用当前 Windows 用户解密（dpapi:v1），"
                "自动配对已停用。请重新填写 Sunshine 密码。");
        qWarning().noquote() << QStringLiteral("[MiniStream] Sunshine 口令解密失败：%1")
                                    .arg(sunshineDecodeErr);
    }
    if (!migrationError.isEmpty() && m_lastError.isEmpty())
        m_lastError = migrationError;

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
    m_sunshinePassword.clear();
    m_lastError.clear();
    loadConfigFromIni();
}

bool MslinkTunnel::persistConfig() {
    const QString path = iniPath();
    // 先当成失败；真正写成功后清掉。这样任何一个提前返回都会留下
    // 「配置没保存」标记，状态定时器不会把提示清掉。
    m_configSaveFailed = true;
    MslinkBridge::setConfigSaveHint(QStringLiteral("配置未保存"));

    // 先把两个秘密都加密好，再动磁盘：任何一步失败都直接返回，
    // 绝不用半个配置覆盖掉现有的 mslink.ini。
    QString passwordCipher;
    QString sunshineCipher;
    QString encErr;
    if (!m_password.isEmpty()
            && !MslinkCredentials::encodeSecret(m_password, &passwordCipher, &encErr)) {
        m_lastError = QStringLiteral("保存失败：无法用 Windows DPAPI 加密隧道口令（%1）。"
                                     "mslink.ini 未改动。").arg(encErr);
        MslinkBridge::mark(m_lastError);
        return false;
    }
    if (!m_sunshinePassword.isEmpty()
            && !MslinkCredentials::encodeSecret(m_sunshinePassword, &sunshineCipher, &encErr)) {
        m_lastError = QStringLiteral("保存失败：无法用 Windows DPAPI 加密 Sunshine 口令（%1）。"
                                     "mslink.ini 未改动。").arg(encErr);
        MslinkBridge::mark(m_lastError);
        return false;
    }

    QList<MslinkBridge::IniLineReplacement> reps;
    auto replace = [&](const QString& key, const QString& value) {
        MslinkBridge::IniLineReplacement r;
        r.sourceKeys = {key};
        r.line = key + QStringLiteral(" = ") + value;
        reps.append(r);
    };
    replace(QStringLiteral("server"), m_server);
    replace(QStringLiteral("tcp_port"), QString::number(m_tcpPort));
    replace(QStringLiteral("udp_port"), QString::number(m_udpPort));
    replace(QStringLiteral("listen"), m_listen);
    replace(QStringLiteral("sunshine_user"), m_sunshineUser);
    MslinkBridge::IniLineReplacement passwordRep;
    passwordRep.sourceKeys = {QStringLiteral("password"), QStringLiteral("password_dpapi")};
    if (!passwordCipher.isEmpty())
        passwordRep.line = QStringLiteral("password_dpapi = ") + passwordCipher;
    reps.append(passwordRep);
    MslinkBridge::IniLineReplacement sunshineRep;
    sunshineRep.sourceKeys = {QStringLiteral("sunshine_pass"), QStringLiteral("sunshine_pass_dpapi")};
    if (!sunshineCipher.isEmpty())
        sunshineRep.line = QStringLiteral("sunshine_pass_dpapi = ") + sunshineCipher;
    reps.append(sunshineRep);
    QString saveError;
    if (!MslinkBridge::rewriteIniAtomic(path, reps, &saveError)) {
        m_lastError = QStringLiteral("保存失败：%1").arg(saveError);
        MslinkBridge::mark(m_lastError);
        return false;
    }
    m_configSaveFailed = false;
    MslinkBridge::setConfigSaveHint(QString());
    return true;
}

void MslinkTunnel::setSunshineCredentials(const QString& user, const QString& pass) {
    m_sunshineUser = user.trimmed().isEmpty() ? QStringLiteral("admin") : user.trimmed();
    m_sunshinePassword = pass;
    if (!persistConfig())
        emit stateChanged();   // lastError 已由 persistConfig 写入，让界面立刻看到保存失败
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
    if (password.isEmpty()) {
        m_lastError = QStringLiteral("请填写隧道口令");
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
    // 先把新配置落盘（DPAPI 加密 + QSaveFile 原子写）。
    // 保存失败只影响「下次启动还能不能自动连」，连接本身已经起来了，
    // 所以不改返回值，只把原因留在 lastError 里让界面显示。
    const bool saved = persistConfig();
    m_wasRunning = false;
    // 注意：这里只代表「本机监听已就绪」，远端是否认证由状态定时器判定
    m_connecting = MslinkBridge::tunnelUp() ? false : true;
    m_connectDeadlineMs = QDateTime::currentMSecsSinceEpoch() + 12000;
    if (!m_connecting) {
        if (saved) m_lastError.clear();   // 保存失败时保留 persistConfig 写的提示
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

QVariantMap MslinkTunnel::telemetry() const {
    if(!MslinkBridge::g_relay || !MslinkBridge::g_started)return {};
    return QJsonDocument::fromJson(QByteArray::fromStdString(MslinkBridge::g_relay->telemetry_json())).object().toVariantMap();
}
QString MslinkTunnel::exportDiagnostics() {
    auto data=QJsonObject::fromVariantMap(telemetry());data.remove("device_name");data.remove("peer_report_json");
    const QString directory=QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)+"/MiniStream";QDir().mkpath(directory);
    const QString path=directory+"/diagnostics-"+QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")+".json";
    QSaveFile file(path);if(!file.open(QIODevice::WriteOnly))return QStringLiteral("诊断保存失败");file.write(QJsonDocument(data).toJson());return file.commit()?path:QStringLiteral("诊断保存失败");
}

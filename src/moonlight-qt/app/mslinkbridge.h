// =============================================================================
//  mslinkbridge.h —— 把 2 端口复用通道嵌进 Moonlight 客户端
//
//  两种使用方式：
//   1) 自动：exe 同目录有 mslink.ini 时，启动即建隧道（无人值守）
//   2) 手动：QML 里用 MslinkTunnel 单例填服务器地址/端口/口令，连接成功后
//            自动写回 mslink.ini，下次启动即自动连（普通用户不需要碰文件）
//
//  口令落盘：ini 里存的是当前 Windows 用户的 DPAPI 密文（dpapi:v1:<base64>），
//  旧版明文 ini 会在主配置载入时原子迁移。注意这只是「本机同一用户可解」的
//  静态保护，不是端到端加密，运行期口令仍在内存里（见 mslinkcredentials.h）。
//
//  Moonlight 里添加主机填 127.0.0.1 即可（本程序已在本地监听全部端口）。
// =============================================================================
#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

namespace MslinkBridge {

// 从 ini 读取配置并启动隧道。ini 不存在或没有 server 时返回 false（不视为错误）。
bool startFromIni(const QString& iniPath, QString* err);

void stop();

bool active();

// 隧道是否已经和远端完成认证（active() 只表示本线程在跑，不代表远端通）
bool tunnelUp();

// 供 UI/日志展示的一行状态
QString statusText();

// 有界遥测历史/事件（schema/events/history），不含设备名与对端信息
QVariantMap historySnapshot();

} // namespace MslinkBridge

// =============================================================================
//  MslinkTunnel —— 暴露给 QML 的单例，负责「首次使用填地址」这件事
// =============================================================================
class MslinkTunnel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap telemetry READ telemetry NOTIFY stateChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY stateChanged)
    Q_PROPERTY(QVariantList events  READ events  NOTIFY stateChanged)
    Q_PROPERTY(QString server     READ server     NOTIFY configChanged)
    Q_PROPERTY(int     tcpPort    READ tcpPort    NOTIFY configChanged)
    Q_PROPERTY(int     udpPort    READ udpPort    NOTIFY configChanged)
    Q_PROPERTY(QString password   READ password   NOTIFY configChanged)
    Q_PROPERTY(bool    running    READ running    NOTIFY stateChanged)
    Q_PROPERTY(bool    tunnelUp   READ tunnelUp   NOTIFY stateChanged)
    Q_PROPERTY(bool    connecting READ connecting NOTIFY stateChanged)
    Q_PROPERTY(bool    configured READ configured NOTIFY configChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString lastError  READ lastError  NOTIFY stateChanged)
    // P1-1: 本地服务入口地址（隧道模式下 Moonlight 应连接此地址）
    Q_PROPERTY(QString localServiceEndpoint READ localServiceEndpoint NOTIFY configChanged)
    // P2-6: UDP 直连是否可用 / 是否已回退到 TCP 兼容模式
    Q_PROPERTY(bool    udpDirectOk READ udpDirectOk NOTIFY stateChanged)
    Q_PROPERTY(bool    udpViaTcp   READ udpViaTcp   NOTIFY stateChanged)

    // MiniStream：自动配对用。被控端 Sunshine 管理网页（47990）的 Basic Auth 凭据，
    // 由被控端安装时生成，装主控端时填一次，口令以 DPAPI 密文存进 mslink.ini。
    // 注意：password / sunshinePassword 属性返回的是解密后的明文，QML 用它回填输入框。
    Q_PROPERTY(QString sunshineUser     READ sunshineUser     NOTIFY configChanged)
    Q_PROPERTY(QString sunshinePassword READ sunshinePassword NOTIFY configChanged)
    Q_PROPERTY(QString listenAddress    READ listenAddress    NOTIFY configChanged)
    Q_PROPERTY(bool    autoPairReady    READ autoPairReady    NOTIFY configChanged)

public:
    explicit MslinkTunnel(QObject* parent = nullptr);
    static MslinkTunnel* get();

    QVariantMap telemetry() const;
    // 最近 300 个约 1 秒采样点 / 最近 100 条事件；缓存 1 秒，QML 同时读两个属性也只解析一次。
    QVariantList history() const;
    QVariantList events() const;
    Q_INVOKABLE QVariantMap metricsSnapshot() const;
    Q_INVOKABLE QString formatBytes(qint64 bytes) const;
    Q_INVOKABLE QString formatRate(qint64 bytesPerSecond) const;
    Q_INVOKABLE QString exportDiagnostics();
    QString server() const     { return m_server; }
    int     tcpPort() const    { return m_tcpPort; }
    int     udpPort() const    { return m_udpPort; }
    QString password() const   { return m_password; }
    bool    running() const    { return MslinkBridge::active(); }
    bool    tunnelUp() const   { return MslinkBridge::tunnelUp(); }
    bool    connecting() const { return m_connecting; }
    bool    configured() const { return !m_server.isEmpty() && m_tcpPort > 0 && m_udpPort > 0; }
    QString statusText() const { return MslinkBridge::statusText(); }
    QString lastError() const  { return m_lastError; }
    // P1-1: 本地服务入口（隧道模式下 Moonlight 应连接此地址）
    QString localServiceEndpoint() const { return m_listen.isEmpty() ? QStringLiteral("127.0.0.1") : m_listen; }
    // P2-6: UDP 直连状态
    bool udpDirectOk() const;
    bool udpViaTcp() const;

    QString sunshineUser() const     { return m_sunshineUser; }
    QString sunshinePassword() const { return m_sunshinePassword; }
    QString listenAddress() const    { return m_listen; }
    bool    autoPairReady() const    { return !m_sunshinePassword.isEmpty(); }

    // 自动配对时应访问的 Sunshine 管理接口主机名：
    // 隧道已配置/已在跑 → 走本机监听地址（隧道把 47990 也带过去）；
    // 否则返回空串，调用方用被控端自己的地址。
    QString autoPairHost() const;

    // QML 调用：连接。返回空字符串表示「本机已开始尝试连接」，**不代表远端已通**；
    // 远端是否连通看 tunnelUp 属性（连接失败/超时会把原因写进 lastError）。
    Q_INVOKABLE QString connectToServer(const QString& server, int tcpPort,
                                        int udpPort, const QString& password);

    // QML 调用：断开
    Q_INVOKABLE void disconnectTunnel();

    // QML 调用：重新读取 ini（比如用户手工改了文件）
    Q_INVOKABLE void reloadConfig();

    // QML 调用：保存 Sunshine 管理网页凭据（自动配对用），立即写回 ini
    Q_INVOKABLE void setSunshineCredentials(const QString& user, const QString& pass);

    // 从 ini 载入配置（不启动），main.cpp 在注册单例前调用一次
    void loadConfigFromIni();

signals:
    void configChanged();
    void stateChanged();

private:
    // 用 DPAPI 加密后经 QSaveFile 原子写回 ini。失败返回 false，并把原因写进
    // m_lastError（QML 的 lastError 属性）——此时磁盘上的文件保持原样。
    bool persistConfig();
    QString iniPath() const;

    QString m_server;
    int     m_tcpPort = 0;
    int     m_udpPort = 0;
    QString m_password;
    QString m_lastError;
    // 配置落盘失败（DPAPI 加密或写 ini）时置位：即使隧道连通，也不让状态定时器
    // 把这条 lastError 清掉，用户还能在界面上看到「配置没保存」。
    bool    m_configSaveFailed = false;
    QTimer  m_statusTimer;
    bool    m_wasRunning = false;
    bool    m_connecting = false;
    qint64  m_connectDeadlineMs = 0;   // 连接超时时刻（QDateTime::currentMSecsSinceEpoch）
    mutable QVariantMap m_historyCache;
    mutable qint64      m_historyCacheMs = 0;

    // 自动配对
    QString m_sunshineUser = QStringLiteral("admin");
    QString m_sunshinePassword;
    QString m_listen = QStringLiteral("127.0.0.1");
};

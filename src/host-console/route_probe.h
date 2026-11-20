#pragma once
#include <QtWidgets>
#include <QtNetwork>

namespace RouteProbe {

inline QJsonObject status(const QString &directory)
{
    QFile file(directory + "/mslink-status.json");
    if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024) return {};
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    const auto age = QDateTime::currentMSecsSinceEpoch() - object.value("updated_unix_ms").toInteger();
    return age >= 0 && age <= 5000 ? object : QJsonObject{};
}

// A real, isolated client must receive an authenticated UDP response and appear
// under our random device name in this host's authenticated TCP telemetry.
inline QString check(QWidget *owner, const QString &root, const QString &ip, int tcp, int udp, const QString &password)
{
    const auto host = status(root);
    if (host.value("role") != "host") return "本机隧道未上报状态。请先启动服务并运行自检。";
    if (host.value("tunnel_up").toBool()) return "已有用户连接。请在会话结束后再校验映射。";
    QTemporaryDir dir;
    const auto exe = dir.filePath("mslink.exe");
    if (!dir.isValid() || !QFile::copy(root + "/mslink.exe", exe)) return "无法准备本地校验程序。";
    QTcpServer localTcp;
    QUdpSocket localUdp;
    if (!localTcp.listen(QHostAddress::LocalHost, 0) || !localUdp.bind(QHostAddress::LocalHost, 0))
        return "无法分配本机临时端口。";
    const auto probeId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QStringList args{"client","--server",ip,"--tcp",QString::number(tcp),"--udp",QString::number(udp),
        "--password-stdin","--listen","127.0.0.1","--device-name",probeId,
        "--tcp-ports",QString::number(localTcp.serverPort()),"--udp-ports",QString::number(localUdp.localPort())};
    localTcp.close();
    localUdp.close();
    QProcess process;
    process.setWorkingDirectory(dir.path());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a){ a->flags |= CREATE_NO_WINDOW; });
    QProgressDialog progress("正在通过映射地址认证 TCP 隧道…", "取消", 0, 0, owner);
    progress.setWindowTitle("FRP · 双向校验");
    progress.setMinimumDuration(0);
    progress.setWindowModality(Qt::WindowModal);
    QTimer poll, deadline;
    deadline.setSingleShot(true);
    QEventLoop loop;
    QString result = "校验程序提前退出。请检查端口或重新尝试。";
    bool authenticated = false, ownHost = false, ack = false;
    QObject::connect(&process, &QProcess::started, &loop, [&]{
        process.write(password.toLatin1() + '\n');
        process.closeWriteChannel();
    });
    QObject::connect(&process, &QProcess::errorOccurred, &loop, [&]{ loop.quit(); });
    QObject::connect(&process, &QProcess::finished, &loop, [&]{ loop.quit(); });
    QObject::connect(&progress, &QProgressDialog::canceled, &loop, [&]{ result = "已取消校验。"; loop.quit(); });
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]{
        const auto client = status(dir.path());
        const auto local = status(root);
        if (client.value("pid").toInteger() != process.processId() || client.value("role") != "client") return;
        authenticated = client.value("tunnel_up").toBool();
        if (!authenticated) return;
        const auto peer = QJsonDocument::fromJson(local.value("peer_report_json").toString().toUtf8()).object();
        ownHost = local.value("role") == "host" && local.value("tunnel_up").toBool() &&
            local.value("peer_report_age_ms").toDouble(99999) < 5000 && peer.value("device_name") == probeId;
        ack = client.value("udp_probe_ack").toInteger() > 0 && client.value("transport") == "udp";
        progress.setLabelText(ownHost ? "TCP 已回到本机，正在确认 UDP 回包…" : "TCP 已认证，正在核对被控端身份…");
        if (ownHost && ack) { result.clear(); loop.quit(); }
    });
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&]{
        result = !authenticated ? "TCP 认证未通过。检查公网 TCP 映射、监听服务与公网地址回环能力。"
            : !ownHost ? "TCP 已认证，但本机未看到此次连接。请确认映射目标是这台电脑。"
            : "TCP 已回到本机，UDP 未双向通过。检查 UDP 映射、防火墙和公网地址回环能力。";
        loop.quit();
    });
    process.start(exe, args);
    poll.start(200);
    deadline.start(15000);
    loop.exec();
    if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(1500); }
    return result;
}

}

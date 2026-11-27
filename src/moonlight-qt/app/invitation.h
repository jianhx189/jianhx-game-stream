#pragma once
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVariantMap>

inline QVariantMap decodeInvitation(const QString &url)
{
    const QString prefix = QStringLiteral("ministream://connect/#");
    if (!url.startsWith(prefix) || url.size() > 2048) return {};
    const auto payload = url.mid(prefix.size());
    static const QRegularExpression encoded("\\A[A-Za-z0-9_-]+\\z"), secret("\\A[A-Za-z0-9_.@-]{1,64}\\z");
    if (!encoded.match(payload).hasMatch()) return {};
    const auto bytes = QByteArray::fromBase64(payload.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    const auto data = QJsonDocument::fromJson(bytes).object();
    const QString host = data.value("ip").toString(), mode = data.value("mode").toString();
    const QHostAddress ip(host);
    if (data.value("v").toInt() != 1 || (mode != "lan" && mode != "frp") ||
        ip.protocol() != QAbstractSocket::IPv4Protocol || ip.toString() != host || ip.isMulticast() ||
        ip == QHostAddress::AnyIPv4 || ip == QHostAddress::Broadcast || !secret.match(data.value("key").toString()).hasMatch()) return {};
    for (const auto field : {"tcp", "udp"}) {
        const int port = data.value(field).toInt();
        if (port < 1 || port > 65535) return {};
    }
    auto result = data.toVariantMap();
    static const QRegularExpression ticketFormat(QStringLiteral(R"(\A[a-f0-9]{64}\z)"));
    const bool hasTicket = result.contains("pair") || result.contains("cert");
    if (hasTicket && (!ticketFormat.match(result.value("pair").toString()).hasMatch() ||
                      !ticketFormat.match(result.value("cert").toString()).hasMatch())) return {};
    return result;
}

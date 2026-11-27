#pragma once

#include <QtNetwork>
#include <QDeadlineTimer>
#include <QFile>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimer>

class InvitationPairing : public QObject {
public:
    explicit InvitationPairing(QObject* parent)
        : QObject(parent), server(this), network(this)
    {
        network.setProxy(QNetworkProxy::NoProxy);
        server.setMaxPendingConnections(8);
        server.setHandshakeTimeout(3000);
        connect(&server, &QSslServer::pendingConnectionAvailable, this, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                socket->setReadBufferSize(512);
                connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
                QTimer::singleShot(3000, socket, [socket] { socket->disconnectFromHost(); });
                connect(socket, &QSslSocket::readyRead, this, [this, socket] { readRequest(socket); });
                readRequest(socket);
            }
        });
    }

    QJsonObject issue(const QString& root)
    {
        QFile info(root + QStringLiteral("/安装信息.txt"));
        QFile cert(root + QStringLiteral("/sunshine/config/credentials/cacert.pem"));
        QFile key(root + QStringLiteral("/sunshine/config/credentials/cakey.pem"));
        if (!info.open(QIODevice::ReadOnly) || !cert.open(QIODevice::ReadOnly) ||
            !key.open(QIODevice::ReadOnly)) return {};
        const QString details = QString::fromUtf8(info.read(32768));
        static const QRegularExpression user(QStringLiteral(R"(Sunshine 用户名：([^\r\n]+))"));
        static const QRegularExpression password(QStringLiteral(R"(Sunshine 网页口令：([^\r\n]+))"));
        const auto username = user.match(details).captured(1).trimmed();
        const auto pass = password.match(details).captured(1).trimmed();
        const QSslCertificate currentCert(&cert);
        const QSslKey privateKey(&key, QSsl::Rsa);
        if (username.isEmpty() || pass.isEmpty() || currentCert.isNull() || privateKey.isNull()) return {};
        if (certificate != currentCert) {
            server.close();
            tickets.clear();
            certificate = currentCert;
        }
        if (!server.isListening()) {
            auto ssl = QSslConfiguration::defaultConfiguration();
            ssl.setLocalCertificate(certificate);
            ssl.setPrivateKey(privateKey);
            ssl.setProtocol(QSsl::TlsV1_2OrLater);
            ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
            server.setSslConfiguration(ssl);
            if (!server.listen(QHostAddress::LocalHost, 47991)) return {};
        }
        authorization = "Basic " + (username + ':' + pass).toUtf8().toBase64();
        for (auto it = tickets.begin(); it != tickets.end();) {
            if (it.value().hasExpired()) it = tickets.erase(it);
            else ++it;
        }
        if (tickets.size() >= 32) return {};
        quint32 random[8];
        QRandomGenerator::system()->fillRange(random);
        const QByteArray token = QByteArray(reinterpret_cast<const char*>(random), sizeof(random)).toHex();
        tickets.insert(token, QDeadlineTimer(10 * 60 * 1000));
        return {{"pair", QString::fromLatin1(token)},
                {"cert", QString::fromLatin1(certificate.digest(QCryptographicHash::Sha256).toHex())}};
    }

private:
    void readRequest(QTcpSocket* socket)
    {
        if (socket->bytesAvailable() >= 512) { socket->abort(); return; }
        if (!socket->canReadLine()) return;
        const auto request = QJsonDocument::fromJson(socket->readLine(512)).object();
        const auto token = request.value("ticket").toString().toLatin1();
        const auto ticket = tickets.find(token);
        const QString marker = request.value("marker").toString(), pin = request.value("pin").toString();
        static const QRegularExpression markerPattern(QStringLiteral(R"(\AMiniStream-[0-9a-f]{32}\z)"));
        static const QRegularExpression pinPattern(QStringLiteral(R"(\A[0-9]{4}\z)"));
        if (ticket != tickets.end() && !ticket.value().hasExpired() &&
            markerPattern.match(marker).hasMatch() && pinPattern.match(pin).hasMatch()) {
            tickets.erase(ticket);
            socket->write("OK\n");
            approve(marker, pin, 20);
        } else {
            socket->write("NO\n");
        }
        socket->disconnectFromHost();
    }

    void approve(const QString& marker, const QString& pin, int remaining)
    {
        if (remaining == 0) return;
        QNetworkRequest request(QUrl(QStringLiteral("https://127.0.0.1:47990/api/pin")));
        auto ssl = request.sslConfiguration();
        ssl.setCaCertificates({certificate});
        request.setSslConfiguration(ssl);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setTransferTimeout(2500);
        request.setRawHeader("Authorization", authorization);
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

        const auto trustLocalName = [this](QNetworkReply *reply) {
            connect(reply, &QNetworkReply::encrypted, reply, [this, reply] {
                if (reply->sslConfiguration().peerCertificate() != certificate) reply->abort();
            });
            connect(reply, &QNetworkReply::sslErrors, reply, [this, reply](const QList<QSslError>& errors) {
                if (reply->sslConfiguration().peerCertificate() == certificate)
                    reply->ignoreSslErrors(errors);
            });
        };
        auto *reply = network.get(request);
        trustLocalName(reply);
        connect(reply, &QNetworkReply::finished, this, [this, marker, pin, remaining, reply, request, trustLocalName] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) return;
            const auto pending = QJsonDocument::fromJson(reply->readAll()).object().value("pairings").toArray();
            QString id;
            for (const auto entry : pending) {
                const auto pairing = entry.toObject();
                if (pairing.value("name").toString() != marker ||
                    pairing.value("address").toString() != QStringLiteral("127.0.0.1")) continue;
                if (!id.isEmpty()) return;
                id = pairing.value("id").toString();
            }
            if (id.isEmpty()) {
                QTimer::singleShot(300, this, [this, marker, pin, remaining] { approve(marker, pin, remaining - 1); });
                return;
            }
            auto submission = request;
            submission.setTransferTimeout(15000);
            const QJsonObject confirmation{{"pairing_id", id}, {"pin", pin}, {"name", "MiniStream"}};
            auto *approval = network.post(submission, QJsonDocument(confirmation).toJson(QJsonDocument::Compact));
            trustLocalName(approval);
            connect(approval, &QNetworkReply::finished, approval, &QObject::deleteLater);
        });
    }

    QSslServer server;
    QNetworkAccessManager network;
    QSslCertificate certificate;
    QByteArray authorization;
    QHash<QByteArray, QDeadlineTimer> tickets;
};

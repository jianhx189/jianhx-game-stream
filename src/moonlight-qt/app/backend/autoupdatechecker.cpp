#include "autoupdatechecker.h"
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVersionNumber>

AutoUpdateChecker::AutoUpdateChecker(QObject *parent) : QObject(parent) {}

void AutoUpdateChecker::start()
{
    if (checking) return;
    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/jianhx189/jianhx-game-stream/releases/latest")));
    request.setRawHeader("User-Agent", "MiniStream/" VERSION_STR);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setTransferTimeout(10000);
    checking = true;
    auto *reply = network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        checking = false;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;
        const auto release = QJsonDocument::fromJson(reply->readAll()).object();
        const QString tag = release.value("tag_name").toString();
        static const QRegularExpression stable(QStringLiteral("\\Av([0-9]{1,6}\\.[0-9]{1,6}\\.[0-9]{1,6})\\z"));
        const auto match = stable.match(tag);
        if (!match.hasMatch() || release.value("draft").toBool(true) || release.value("prerelease").toBool(true)) return;
        const auto version = match.captured(1);
        if (QVersionNumber::fromString(version) > QVersionNumber::fromString(VERSION_STR))
            emit updateAvailable(version, "https://github.com/jianhx189/jianhx-game-stream/releases/tag/" + tag);
    });
}

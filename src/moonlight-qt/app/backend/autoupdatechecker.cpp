#include "autoupdatechecker.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSettings>
#include <QVersionNumber>

namespace {
const QString repo = QStringLiteral("https://github.com/jianhx189/jianhx-game-stream");

QVersionNumber parseVersion(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\A[vV]?([0-9]{1,6}\\.[0-9]{1,6}\\.[0-9]{1,6}(?:\\.[0-9]{1,6})?)\\z"));
    const auto match = pattern.match(value.trimmed());
    return match.hasMatch() ? QVersionNumber::fromString(match.captured(1)).normalized() : QVersionNumber();
}
}

AutoUpdateChecker::AutoUpdateChecker(QObject *parent) : QObject(parent) {}

QString AutoUpdateChecker::newerClientRelease(const QJsonObject &release, const QString &installed)
{
    if (release.value("draft").toBool(true) || release.value("prerelease").toBool(true)) return {};
    const QString tag = release.value("tag_name").toString();
    const auto offered = parseVersion(tag);
    const auto current = parseVersion(installed);
    if (!tag.startsWith('v') || offered.isNull() || current.isNull() || offered <= current) return {};
    for (const auto &entry : release.value("assets").toArray()) {
        const auto asset = entry.toObject();
        if (asset.value("name").toString() == "MiniStream-Client-Setup.exe" &&
            asset.value("state").toString() == "uploaded" &&
            asset.value("size").toDouble() > 0) return tag;
    }
    return {};
}

void AutoUpdateChecker::dismiss(const QString &version)
{
    const auto offered = parseVersion(version);
    if (offered.isNull() || offered <= parseVersion(QCoreApplication::applicationVersion())) return;
    QSettings().setValue(QStringLiteral("updates/dismissedClientVersion"), offered.toString());
    emit noUpdateAvailable();
}

void AutoUpdateChecker::start()
{
    if (checking) return;
    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/jianhx189/jianhx-game-stream/releases/latest")));
    request.setRawHeader("User-Agent", ("MiniStream/" + QCoreApplication::applicationVersion()).toUtf8());
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(10000);
    checking = true;
    auto *reply = network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        checking = false;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;
        const auto doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) return;
        const auto tag = newerClientRelease(doc.object(), QCoreApplication::applicationVersion());
        const auto dismissed = parseVersion(QSettings().value(QStringLiteral("updates/dismissedClientVersion")).toString());
        if (tag.isEmpty() || (!dismissed.isNull() && parseVersion(tag) <= dismissed)) emit noUpdateAvailable();
        else emit updateAvailable(tag.mid(1), repo + "/releases/tag/" + tag);
    });
}

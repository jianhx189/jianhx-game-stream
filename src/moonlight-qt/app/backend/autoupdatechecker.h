#pragma once
#include <QObject>
#include <QNetworkAccessManager>

class AutoUpdateChecker : public QObject {
    Q_OBJECT
public:
    explicit AutoUpdateChecker(QObject *parent = nullptr);
    Q_INVOKABLE void start();
    Q_INVOKABLE void dismiss(const QString& version);
    static QString newerClientRelease(const QJsonObject &release, const QString &installed);
signals:
    void noUpdateAvailable();
    void updateAvailable(QString version, QString url);
private:
    QNetworkAccessManager network;
    bool checking = false;
};

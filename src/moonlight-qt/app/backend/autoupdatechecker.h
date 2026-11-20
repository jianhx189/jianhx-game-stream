#pragma once
#include <QObject>
#include <QNetworkAccessManager>

class AutoUpdateChecker : public QObject {
    Q_OBJECT
public:
    explicit AutoUpdateChecker(QObject *parent = nullptr);
    Q_INVOKABLE void start();
signals:
    void updateAvailable(QString version, QString url);
private:
    QNetworkAccessManager network;
    bool checking = false;
};

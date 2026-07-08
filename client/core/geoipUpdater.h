#ifndef GEOIPUPDATER_H
#define GEOIPUPDATER_H

#include <QObject>
#include <QTimer>
#include <QDateTime>

class QNetworkAccessManager;
class QNetworkReply;
class Settings;

class GeoipUpdater : public QObject
{
    Q_OBJECT

public:
    explicit GeoipUpdater(QNetworkAccessManager *nam, std::shared_ptr<Settings> settings, QObject *parent = nullptr);

    // Returns the path to the freshest available CIDR file:
    // local cache if fresh enough, otherwise QRC fallback.
    static QString cidrFilePath(const std::shared_ptr<Settings> &settings);

    void startPeriodicUpdates();

signals:
    void updateFinished(bool success, int cidrCount);

public slots:
    void checkAndUpdate();

private:
    static QString localCachePath();
    void download();
    void handleReply(QNetworkReply *reply);

    QNetworkAccessManager *m_nam = nullptr;
    std::shared_ptr<Settings> m_settings;
    QTimer m_timer;

    static constexpr int UpdateIntervalDays = 7;
    static constexpr const char *DownloadUrl =
        "https://raw.githubusercontent.com/herrbischoff/country-ip-blocks/master/ipv4/ru.cidr";
};

#endif // GEOIPUPDATER_H

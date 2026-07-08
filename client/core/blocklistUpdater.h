#ifndef BLOCKLISTUPDATER_H
#define BLOCKLISTUPDATER_H

#include <QObject>
#include <QTimer>
#include <QDateTime>

class QNetworkAccessManager;
class QNetworkReply;
class Settings;

class BlocklistUpdater : public QObject
{
    Q_OBJECT

public:
    explicit BlocklistUpdater(QNetworkAccessManager *nam, std::shared_ptr<Settings> settings, QObject *parent = nullptr);

    // Returns the path to the cached blocked-domains file, or empty string if not available.
    static QString domainsFilePath(const std::shared_ptr<Settings> &settings);

    void startPeriodicUpdates();

signals:
    void updateFinished(bool success, int domainCount);

public slots:
    void checkAndUpdate();

private:
    static QString localCachePath();
    void download();
    void handleReply(QNetworkReply *reply);

    QNetworkAccessManager *m_nam = nullptr;
    std::shared_ptr<Settings> m_settings;
    QTimer m_timer;

    static constexpr int UpdateIntervalDays = 3;
    static constexpr const char *DownloadUrl =
        "https://community.antifilter.download/list/domains.lst";
};

#endif // BLOCKLISTUPDATER_H

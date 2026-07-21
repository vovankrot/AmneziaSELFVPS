#ifndef GEOIPUPDATER_H
#define GEOIPUPDATER_H

#include <QObject>
#include <QTimer>
#include <QDateTime>

class QNetworkAccessManager;
class QNetworkReply;
class Settings;

// Downloads and refreshes the RU CIDR list used to route Russian IPs past the VPN.
// Source URL and refresh interval are USER-CONFIGURABLE (stored in Settings) and the
// current state is exposed to QML so the user can see which list is in use.
class GeoipUpdater : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString sourceUrl READ sourceUrl WRITE setSourceUrl NOTIFY statusChanged)
    Q_PROPERTY(int intervalHours READ intervalHours WRITE setIntervalHours NOTIFY statusChanged)
    Q_PROPERTY(QString lastUpdateText READ lastUpdateText NOTIFY statusChanged)
    Q_PROPERTY(int cidrCount READ cidrCount NOTIFY statusChanged)
    Q_PROPERTY(QString listPath READ listPath NOTIFY statusChanged)
    Q_PROPERTY(bool usingBundledList READ usingBundledList NOTIFY statusChanged)
    Q_PROPERTY(bool updating READ updating NOTIFY statusChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY statusChanged)

public:
    explicit GeoipUpdater(QNetworkAccessManager *nam, std::shared_ptr<Settings> settings, QObject *parent = nullptr);

    // Returns the path to the freshest available CIDR file:
    // local cache if present, otherwise the bundled QRC fallback.
    static QString cidrFilePath(const std::shared_ptr<Settings> &settings);

    void startPeriodicUpdates();

    QString sourceUrl() const;
    void setSourceUrl(const QString &url);

    int intervalHours() const;
    void setIntervalHours(int hours);

    QString lastUpdateText() const;
    int cidrCount() const;
    QString listPath() const;
    bool usingBundledList() const;
    bool updating() const { return m_updating; }
    QString lastError() const { return m_lastError; }

    // Force a download now, ignoring the interval (the "Update now" button).
    Q_INVOKABLE void updateNow();
    // Restore the built-in source URL.
    Q_INVOKABLE void resetSourceToDefault();

signals:
    void updateFinished(bool success, int cidrCount);
    void statusChanged();

public slots:
    void checkAndUpdate();

private:
    static QString localCachePath();
    void download();
    void handleReply(QNetworkReply *reply);
    void applyTimerInterval();

    QNetworkAccessManager *m_nam = nullptr;
    std::shared_ptr<Settings> m_settings;
    QTimer m_timer;
    bool m_updating = false;
    QString m_lastError;
};

#endif // GEOIPUPDATER_H

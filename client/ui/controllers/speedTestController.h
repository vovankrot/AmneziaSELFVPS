#ifndef SPEEDTESTCONTROLLER_H
#define SPEEDTESTCONTROLLER_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>

class QNetworkAccessManager;
class QNetworkReply;
class ServersModel;

class SpeedTestController : public QObject
{
    Q_OBJECT

public:
    Q_PROPERTY(bool isRunning READ isRunning NOTIFY isRunningChanged)
    Q_PROPERTY(double downloadSpeed READ downloadSpeed NOTIFY downloadSpeedChanged)
    Q_PROPERTY(double uploadSpeed READ uploadSpeed NOTIFY uploadSpeedChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(QString serverPingText READ serverPingText NOTIFY serverPingTextChanged)
    Q_PROPERTY(QString moscowPingText READ moscowPingText NOTIFY moscowPingTextChanged)

    explicit SpeedTestController(ServersModel *serversModel, QObject *parent = nullptr);
    ~SpeedTestController() override;

    bool isRunning() const;
    double downloadSpeed() const;
    double uploadSpeed() const;
    QString statusText() const;
    QString serverPingText() const;
    QString moscowPingText() const;

public slots:
    void runSpeedTest();

signals:
    void isRunningChanged();
    void downloadSpeedChanged();
    void uploadSpeedChanged();
    void statusTextChanged();
    void serverPingTextChanged();
    void moscowPingTextChanged();
    void speedTestFinished();
    void speedTestFailed(const QString &error);

private slots:
    void onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void onDownloadFinished();
    void onUploadProgress(qint64 bytesSent, qint64 bytesTotal);
    void onUploadFinished();
    void onPhaseTimeout();

private:
    enum class Phase
    {
        Idle,
        Latency,
        Download,
        Upload
    };

    struct ActiveReplyState
    {
        qint64 bytesTransferred = 0;
        bool switchRequested = false;
        QTimer *probeTimer = nullptr;
    };

    void startLatencyMeasurements();
    void startDownloadTest();
    void startUploadTest();
    bool startDownloadRequest(bool allowRefill, const QString &excludedUrl = QString());
    bool startUploadRequest(bool allowRefill, const QString &excludedUrl = QString());
    void ensureDownloadConcurrency(bool allowRefill, const QString &excludedUrl = QString());
    void ensureUploadConcurrency(bool allowRefill, const QString &excludedUrl = QString());
    void finalizeDownloadPhase();
    void finalizeUploadPhase();
    void finishWithError(const QString &status, const QString &error);
    void resetPhaseTransferState();
    double currentPhaseAverageMbps() const;
    qint64 currentInFlightBytes() const;
    QString currentServerHost() const;
    quint16 currentServerPort() const;
    void stopAndDeleteProbeTimer(QTimer *probeTimer);
    void clearActiveReplies();
    void setIsRunning(bool running);
    void setDownloadSpeed(double mbps);
    void setUploadSpeed(double mbps);
    void setStatusText(const QString &text);
    void setServerPingText(const QString &text);
    void setMoscowPingText(const QString &text);

    QNetworkAccessManager *m_nam = nullptr;
    ServersModel *m_serversModel = nullptr;
    QElapsedTimer m_timer;
    QTimer m_phaseLimitTimer;

    bool m_isRunning = false;
    double m_downloadSpeed = 0.0;
    double m_uploadSpeed = 0.0;
    QString m_statusText;
    QString m_serverPingText;
    QString m_moscowPingText;

    qint64 m_lastTimestamp = 0;
    qint64 m_phaseAccumulatedBytes = 0;
    int m_pendingLatencyProbes = 0;
    Phase m_phase = Phase::Idle;
    bool m_phaseStopRequested = false;
    QByteArray m_uploadPayload;

    QStringList m_pendingDownloadUrls;
    QStringList m_pendingUploadUrls;
    QHash<QNetworkReply *, ActiveReplyState> m_activeReplies;

    static const QStringList s_downloadUrls;
    static const QStringList s_uploadUrls;
};

#endif // SPEEDTESTCONTROLLER_H

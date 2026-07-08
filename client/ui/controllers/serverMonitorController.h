#ifndef SERVERMONITORCONTROLLER_H
#define SERVERMONITORCONTROLLER_H

#include <QObject>
#include <QTimer>
#include <QVariantList>

#include "settings.h"
#include "ui/models/servers_model.h"

class ServerMonitorController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool isPolling READ isPolling NOTIFY isPollingChanged)
    Q_PROPERTY(QVariantList cpuHistory READ cpuHistory NOTIFY metricsUpdated)
    Q_PROPERTY(QVariantList ramHistory READ ramHistory NOTIFY metricsUpdated)
    Q_PROPERTY(QVariantList netRxHistory READ netRxHistory NOTIFY metricsUpdated)
    Q_PROPERTY(QVariantList netTxHistory READ netTxHistory NOTIFY metricsUpdated)
    Q_PROPERTY(double cpuPercent READ cpuPercent NOTIFY metricsUpdated)
    Q_PROPERTY(double ramUsedMb READ ramUsedMb NOTIFY metricsUpdated)
    Q_PROPERTY(double ramTotalMb READ ramTotalMb NOTIFY metricsUpdated)
    Q_PROPERTY(double netRxKBs READ netRxKBs NOTIFY metricsUpdated)
    Q_PROPERTY(double netTxKBs READ netTxKBs NOTIFY metricsUpdated)
    Q_PROPERTY(int diskPercent READ diskPercent NOTIFY metricsUpdated)
    Q_PROPERTY(QString uptime READ uptime NOTIFY metricsUpdated)
    Q_PROPERTY(int dockerContainers READ dockerContainers NOTIFY metricsUpdated)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)

public:
    explicit ServerMonitorController(const QSharedPointer<ServersModel> &serversModel,
                                     const std::shared_ptr<Settings> &settings,
                                     QObject *parent = nullptr);
    ~ServerMonitorController() override;

    bool isPolling() const;
    QVariantList cpuHistory() const;
    QVariantList ramHistory() const;
    QVariantList netRxHistory() const;
    QVariantList netTxHistory() const;
    double cpuPercent() const;
    double ramUsedMb() const;
    double ramTotalMb() const;
    double netRxKBs() const;
    double netTxKBs() const;
    int diskPercent() const;
    QString uptime() const;
    int dockerContainers() const;
    QString errorString() const;

public slots:
    void startMonitoring();
    void stopMonitoring();

signals:
    void isPollingChanged();
    void metricsUpdated();
    void errorStringChanged();

private slots:
    void pollMetrics();

private:
    void parseOutput(const QString &output);

    QSharedPointer<ServersModel> m_serversModel;
    std::shared_ptr<Settings> m_settings;
    QTimer m_timer;

    static const int kMaxHistory = 60;
    static const int kPollIntervalMs = 10000;

    bool m_pollInFlight = false;

    QList<double> m_cpuHistory;
    QList<double> m_ramHistory;
    QList<double> m_netRxHistory;
    QList<double> m_netTxHistory;

    double m_cpuPercent = 0;
    double m_ramUsedMb = 0;
    double m_ramTotalMb = 0;
    double m_netRxKBs = 0;
    double m_netTxKBs = 0;
    int m_diskPercent = 0;
    QString m_uptime;
    int m_dockerContainers = 0;
    QString m_errorString;

    bool m_isPolling = false;

    // Previous network counters for delta calculation
    qint64 m_prevRxBytes = -1;
    qint64 m_prevTxBytes = -1;
    qint64 m_prevCpuTotal = -1;
    qint64 m_prevCpuIdle = -1;
};

#endif // SERVERMONITORCONTROLLER_H

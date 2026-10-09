#ifndef XRAYPROTOCOL_H
#define XRAYPROTOCOL_H

#include "QProcess"
#include <QTimer>
#include <QFutureWatcher>
#include "core/tunnelDataProbe.h"

#include "core/ipcclient.h"
#include "vpnprotocol.h"
#include <QPointer>

class AsyncIpcSequence;
class AsyncProcessRequest;
#include "settings.h"
#include <QtCore/qsharedpointer.h>

class XrayProtocol : public VpnProtocol
{
public:
    XrayProtocol(const QJsonObject &configuration, QObject *parent = nullptr);
    virtual ~XrayProtocol() override;

    ErrorCode start() override;
    void stop() override;
    bool stopsAsynchronously() const override { return true; }

private:
    void setupRouting();
    ErrorCode startTun2Socks();
    void configureTun2Socks();
    void scheduleHealthCheck();
    void cancelHealthCheck();
    void runHealthCheck();
    void handleHealthCheckResult(bool ok);
    int localSocksPort() const;
    QString probeHost() const;
    static void ensureDnsOverDoh(QJsonObject &config);

    QJsonObject m_xrayConfig;
    Settings::RouteMode m_routeMode;
    QList<QHostAddress> m_dnsServers;
    QString m_remoteAddress;

    QString m_socksUser;
    QString m_socksPassword;
    int m_socksPort = 10808;

    QSharedPointer<IpcProcessInterfaceReplica> m_tun2socksProcess;
    bool m_stopping = false;
    QPointer<AsyncIpcSequence> m_routingSetup;
    QPointer<AsyncIpcSequence> m_startupSequence;
    QPointer<AsyncProcessRequest> m_processRequest;

    // Post-Connect healthcheck: after the tunnel reports Connected we still
    // occasionally see a "ghost connected" state after cold boot -- xray is up,
    // tun2socks is up, tun routes are in place, but the tunnel doesn't actually
    // carry traffic. Probe asynchronously through SOCKS. Repeated failures
    // report a warning while preserving the session and existing bypass flows.
    QTimer *m_healthTimer = nullptr;
    int m_healthCheckFailures = 0;
    quint64 m_healthGeneration = 0;
    QPointer<QFutureWatcher<TunnelDataProbe::Result>> m_healthProbe;
};

#endif // XRAYPROTOCOL_H

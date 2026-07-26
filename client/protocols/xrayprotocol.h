#ifndef XRAYPROTOCOL_H
#define XRAYPROTOCOL_H

#include "QProcess"
#include <QTimer>

#include "core/ipcclient.h"
#include "vpnprotocol.h"
#include "settings.h"
#include <QtCore/qsharedpointer.h>

class XrayProtocol : public VpnProtocol
{
public:
    XrayProtocol(const QJsonObject &configuration, QObject *parent = nullptr);
    virtual ~XrayProtocol() override;

    ErrorCode start() override;
    void stop() override;

private:
    ErrorCode setupRouting();
    ErrorCode startTun2Socks();
    ErrorCode startXrayProcess(const QSharedPointer<IpcInterfaceReplica> &iface);
    bool ensureProxyReachable();
    bool performSocks5Probe(const QString &targetHost, quint16 targetPort, int timeoutMs);
    void scheduleHealthCheck();
    void cancelHealthCheck();
    void runHealthCheck();
    int localSocksPort() const;
    QString probeHost() const;
    quint16 probePort() const;
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

    // Post-Connect healthcheck: after the tunnel reports Connected we still
    // occasionally see a "ghost connected" state after cold boot -- xray is up,
    // tun2socks is up, tun routes are in place, but the tunnel doesn't actually
    // carry traffic. This timer fires shortly after Connected and re-probes
    // through the SOCKS inbound; if the probe fails the protocol is torn down
    // with an error so the standard reconnect/failover path runs. by vovankrot
    QTimer *m_healthTimer = nullptr;
    int m_healthCheckFailures = 0;
};

#endif // XRAYPROTOCOL_H

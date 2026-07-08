#ifndef XRAYPROTOCOL_H
#define XRAYPROTOCOL_H

#include "QProcess"

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
};

#endif // XRAYPROTOCOL_H

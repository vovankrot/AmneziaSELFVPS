#ifndef ANYTLSPROTOCOL_H
#define ANYTLSPROTOCOL_H

#include <QHostAddress>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QSharedPointer>
#include <QString>

#include "core/ipcclient.h"
#include "settings.h"
#include "vpnprotocol.h"

class AnyTlsProtocol : public VpnProtocol
{
    Q_OBJECT
public:
    AnyTlsProtocol(const QJsonObject &configuration, QObject *parent = nullptr);
    ~AnyTlsProtocol() override;

    ErrorCode start() override;
    void stop() override;

private:
    ErrorCode startAnyTlsProcess();
    ErrorCode startXrayRouter(const QSharedPointer<IpcInterfaceReplica> &iface);
    ErrorCode startTun2Socks();
    ErrorCode setupRouting();
    bool ensureProxyReachable();
    bool ensureXrayRouterReachable();
    bool performSocks5Probe(const QString &targetHost, quint16 targetPort, int timeoutMs, int socksPort,
                            const QString &user = QString(), const QString &password = QString());
    int parseLocalPort(const QString &listen) const;
    QString buildServerUri() const;

    Settings::RouteMode m_routeMode;
    QList<QHostAddress> m_dnsServers;
    QString m_remoteAddress;

    QString m_serverAddress;
    QString m_password;
    QString m_sni;
    int m_socksPort = 10810;

    QJsonObject m_xrayRouterConfig;
    QString m_xrayRouterUser;
    QString m_xrayRouterPassword;
    int m_xrayRouterSocksPort = 0;

    QPointer<QProcess> m_anyTlsProcess;
    QSharedPointer<IpcProcessInterfaceReplica> m_tun2socksProcess;
    bool m_stopping = false;
};

#endif // ANYTLSPROTOCOL_H
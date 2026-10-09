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
#include <QPointer>

class AsyncIpcSequence;
class AsyncProcessRequest;
class AsyncReplicaReady;
class AsyncSocksProbe;

class AnyTlsProtocol : public VpnProtocol
{
    Q_OBJECT
public:
    AnyTlsProtocol(const QJsonObject &configuration, QObject *parent = nullptr);
    ~AnyTlsProtocol() override;

    ErrorCode start() override;
    void stop() override;
    bool stopsAsynchronously() const override { return true; }

private:
    ErrorCode startAnyTlsProcess();
    void afterHelperStarted();
    void startXrayRouter();
    ErrorCode startTun2Socks();
    void configureTun2Socks();
    void setupRouting();
    int parseLocalPort(const QString &listen) const;
    QString buildServerUri() const;

    Settings::RouteMode m_routeMode;
    QList<QHostAddress> m_dnsServers;
    QString m_remoteAddress;

    QString m_serverAddress;
    QString m_password;
    QString m_sni;
    QString m_certificatePin;
    int m_socksPort = 10810;

    QJsonObject m_xrayRouterConfig;
    QString m_xrayRouterUser;
    QString m_xrayRouterPassword;
    int m_xrayRouterSocksPort = 0;

    QPointer<QProcess> m_anyTlsProcess;
    QSharedPointer<IpcProcessInterfaceReplica> m_tun2socksProcess;
    bool m_stopping = false;
    QPointer<AsyncIpcSequence> m_routingSetup;
    QPointer<AsyncIpcSequence> m_startupSequence;
    QPointer<AsyncProcessRequest> m_processRequest;
    QPointer<AsyncReplicaReady> m_startupReady;
    QPointer<AsyncSocksProbe> m_startupProbe;
    quint64 m_startGeneration = 0;
};

#endif // ANYTLSPROTOCOL_H

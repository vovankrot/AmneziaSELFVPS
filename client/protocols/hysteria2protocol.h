#ifndef HYSTERIA2PROTOCOL_H
#define HYSTERIA2PROTOCOL_H

#include <QHostAddress>
#include <QPointer>
#include <QProcess>
#include <QSharedPointer>
#include <QString>
#include <QTemporaryFile>

#include "core/ipcclient.h"
#include "settings.h"
#include "vpnprotocol.h"
#include <QPointer>

class AsyncIpcSequence;
class AsyncProcessRequest;
class AsyncReplicaReady;
class AsyncSocksProbe;

class Hysteria2Protocol : public VpnProtocol
{
    Q_OBJECT
public:
    Hysteria2Protocol(const QJsonObject &configuration, QObject *parent = nullptr);
    ~Hysteria2Protocol() override;

    ErrorCode start() override;
    void stop() override;
    bool stopsAsynchronously() const override { return true; }

private:
    ErrorCode startHysteriaProcess();
    void afterHelperStarted();
    void startXrayRouter();
    ErrorCode startTun2Socks();
    void configureTun2Socks();
    void setupRouting();
    QString writeConfigToTempFile();
    int parseSocksPortFromYaml() const;
    quint16 remotePort() const;

    QString m_yamlConfig;
    QString m_configPath;       // path to temporary yaml file (cleaned on stop)
    int m_socksPort = 10809;

    Settings::RouteMode m_routeMode;
    QList<QHostAddress> m_dnsServers;
    QString m_remoteAddress;
    QString m_masqueradeHost;

    QJsonObject m_xrayRouterConfig;
    QString m_xrayRouterUser;
    QString m_xrayRouterPassword;
    int m_xrayRouterSocksPort = 0;

    QPointer<QProcess> m_hysteriaProcess; // runs as the same user as the client
    QSharedPointer<IpcProcessInterfaceReplica> m_tun2socksProcess;
    bool m_stopping = false;
    QPointer<AsyncIpcSequence> m_routingSetup;
    QPointer<AsyncIpcSequence> m_startupSequence;
    QPointer<AsyncProcessRequest> m_processRequest;
    QPointer<AsyncReplicaReady> m_startupReady;
    QPointer<AsyncSocksProbe> m_startupProbe;
    quint64 m_startGeneration = 0;
};

#endif // HYSTERIA2PROTOCOL_H

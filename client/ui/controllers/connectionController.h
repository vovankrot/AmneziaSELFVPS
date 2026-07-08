#ifndef CONNECTIONCONTROLLER_H
#define CONNECTIONCONTROLLER_H

#include "protocols/vpnprotocol.h"
#include "ui/models/clientManagementModel.h"
#include "ui/models/containers_model.h"
#include "ui/models/servers_model.h"
#include "vpnconnection.h"

class ConnectionController : public QObject
{
    Q_OBJECT

public:
    Q_PROPERTY(bool isConnected READ isConnected NOTIFY connectionStateChanged)
    Q_PROPERTY(bool isConnectionInProgress READ isConnectionInProgress NOTIFY connectionStateChanged)
    Q_PROPERTY(QString connectionStateText READ connectionStateText NOTIFY connectionStateChanged)
    Q_PROPERTY(QString actionButtonText READ actionButtonText NOTIFY connectionStateChanged)
    Q_PROPERTY(bool isAutoFailoverActive READ isAutoFailoverActive NOTIFY autoFailoverStateChanged)

    explicit ConnectionController(const QSharedPointer<ServersModel> &serversModel, const QSharedPointer<ContainersModel> &containersModel,
                                  const QSharedPointer<ClientManagementModel> &clientManagementModel,
                                  const QSharedPointer<VpnConnection> &vpnConnection, const std::shared_ptr<Settings> &settings,
                                  QObject *parent = nullptr);

    ~ConnectionController() = default;

    bool isConnected() const;
    bool isConnectionInProgress() const;
    QString connectionStateText() const;
    QString actionButtonText() const;
    bool isAutoFailoverActive() const;

public slots:
    void toggleConnection();

    void openConnection();
    void closeConnection();
    void reconnectToVpn();
    Q_INVOKABLE void switchToContainer(int containerIndex);

    ErrorCode getLastConnectionError();
    void onConnectionStateChanged(Vpn::ConnectionState state);

    void onCurrentContainerUpdated();

    void onTranslationsUpdated();

    void tryAutoFailover();
    void resetAutoFailover();

signals:
    void connectToVpn(int serverIndex, const ServerCredentials &credentials, DockerContainer container, const QJsonObject &vpnConfiguration);
    void disconnectFromVpn();
    void connectionStateChanged();

    void connectionErrorOccurred(ErrorCode errorCode);
    void splitTunnelingUnsupported(const QString &message);
    void reconnectWithUpdatedContainer(const QString &message);

    void connectButtonClicked();
    void preparingConfig();
    void prepareConfig();

    void autoFailoverStateChanged();
    void autoFailoverTriggered(const QString &message);

private:
    Vpn::ConnectionState getCurrentConnectionState();

    void continueConnection();
    bool tryNextFallback();

    QSharedPointer<ServersModel> m_serversModel;
    QSharedPointer<ContainersModel> m_containersModel;
    QSharedPointer<ClientManagementModel> m_clientManagementModel;

    QSharedPointer<VpnConnection> m_vpnConnection;

    std::shared_ptr<Settings> m_settings;

    bool m_isConnected = false;
    bool m_isConnectionInProgress = false;
    bool m_reconnectAfterDisconnect = false;
    QString m_connectionStateText = tr("Connect");
    QString m_actionButtonText = tr("Connect");

    Vpn::ConnectionState m_state;

    // Auto-failover state
    bool m_autoFailoverActive = false;
    int m_failoverAttempts = 0;
    int m_originalServerIndex = -1;
    DockerContainer m_originalContainer = DockerContainer::None;
    static constexpr int MAX_FAILOVER_ATTEMPTS = 3;
};

#endif // CONNECTIONCONTROLLER_H

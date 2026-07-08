#include "connectionController.h"

#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS) || defined(MACOS_NE)
    #include <QGuiApplication>
#else
    #include <QApplication>
#endif

#include "amnezia_application.h"
#include "utilities.h"
#include "core/controllers/vpnConfigurationController.h"
#include "containers/containers_defs.h"
#include "version.h"

ConnectionController::ConnectionController(const QSharedPointer<ServersModel> &serversModel,
                                           const QSharedPointer<ContainersModel> &containersModel,
                                           const QSharedPointer<ClientManagementModel> &clientManagementModel,
                                           const QSharedPointer<VpnConnection> &vpnConnection, const std::shared_ptr<Settings> &settings,
                                           QObject *parent)
    : QObject(parent),
      m_serversModel(serversModel),
      m_containersModel(containersModel),
      m_clientManagementModel(clientManagementModel),
      m_vpnConnection(vpnConnection),
      m_settings(settings)
{
    connect(m_vpnConnection.get(), &VpnConnection::connectionStateChanged, this, &ConnectionController::onConnectionStateChanged);
    connect(m_vpnConnection.get(), &VpnConnection::siteSplitTunnelingWarning, this, &ConnectionController::splitTunnelingUnsupported);
    connect(this, &ConnectionController::connectToVpn, m_vpnConnection.get(), &VpnConnection::connectToVpn, Qt::QueuedConnection);
    connect(this, &ConnectionController::disconnectFromVpn, m_vpnConnection.get(), &VpnConnection::disconnectFromVpn, Qt::QueuedConnection);

    connect(this, &ConnectionController::connectButtonClicked, this, &ConnectionController::toggleConnection, Qt::QueuedConnection);

    m_state = Vpn::ConnectionState::Disconnected;
}

void ConnectionController::openConnection()
{
#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS) && !defined(MACOS_NE)
    if (!Utils::processIsRunning(Utils::executable(SERVICE_NAME, false), true))
    {
        emit connectionErrorOccurred(ErrorCode::AmneziaServiceNotRunning);
        return;
    }
#endif

    int serverIndex = m_serversModel->getDefaultServerIndex();
    QJsonObject serverConfig = m_serversModel->getServerConfig(serverIndex);

    DockerContainer container = qvariant_cast<DockerContainer>(m_serversModel->data(serverIndex, ServersModel::Roles::DefaultContainerRole));

    if (!m_containersModel->isSupportedByCurrentPlatform(container)) {
        emit connectionErrorOccurred(ErrorCode::NotSupportedOnThisPlatform);
        return;
    }

    QSharedPointer<ServerController> serverController(new ServerController(m_settings));
    VpnConfigurationsController vpnConfigurationController(m_settings, serverController);

    QJsonObject containerConfig = m_containersModel->getContainerConfig(container);
    ServerCredentials credentials = m_serversModel->getServerCredentials(serverIndex);

    auto dns = m_serversModel->getDnsPair(serverIndex);

    auto vpnConfiguration = vpnConfigurationController.createVpnConfiguration(dns, serverConfig, containerConfig, container);
    emit connectToVpn(serverIndex, credentials, container, vpnConfiguration);
}

void ConnectionController::closeConnection()
{
    emit disconnectFromVpn();
}

void ConnectionController::switchToContainer(int containerIndex)
{
    const int serverIndex = m_serversModel->getDefaultServerIndex();
    if (serverIndex < 0) {
        return;
    }

    const auto nextContainer = static_cast<DockerContainer>(containerIndex);
    const auto currentContainer = qvariant_cast<DockerContainer>(
        m_serversModel->data(serverIndex, ServersModel::Roles::DefaultContainerRole));

    if (nextContainer == DockerContainer::None || nextContainer == currentContainer) {
        return;
    }

    if (!m_containersModel->isSupportedByCurrentPlatform(nextContainer)) {
        emit connectionErrorOccurred(ErrorCode::NotSupportedOnThisPlatform);
        return;
    }

    m_serversModel->setDefaultContainer(serverIndex, nextContainer);

    if (m_isConnected || m_isConnectionInProgress) {
        m_reconnectAfterDisconnect = true;
        emit reconnectWithUpdatedContainer(tr("Protocol changed, reconnecting..."));
        closeConnection();
        return;
    }

    emit reconnectWithUpdatedContainer(tr("Protocol changed"));
}

void ConnectionController::reconnectToVpn()
{
    if (m_state != Vpn::ConnectionState::Connected) {
        return;
    }

    m_vpnConnection->reconnectToVpn();
}

ErrorCode ConnectionController::getLastConnectionError()
{
    return m_vpnConnection->lastError();
}

void ConnectionController::onConnectionStateChanged(Vpn::ConnectionState state)
{
    m_state = state;

    m_isConnected = false;
    m_connectionStateText = tr("Connecting...");
    m_actionButtonText = tr("Disconnect");
    switch (state) {
    case Vpn::ConnectionState::Connected: {
        amnApp->networkManager()->clearConnectionCache();

        m_isConnectionInProgress = false;
        m_isConnected = true;
        m_connectionStateText = tr("Connected");
        m_actionButtonText = tr("Disconnect");

        // Reset failover on successful connection
        if (m_autoFailoverActive) {
            m_autoFailoverActive = false;
            emit autoFailoverStateChanged();
        }
        m_failoverAttempts = 0;
        break;
    }
    case Vpn::ConnectionState::Connecting: {
        m_isConnectionInProgress = true;
        break;
    }
    case Vpn::ConnectionState::Reconnecting: {
        m_isConnectionInProgress = true;
        m_connectionStateText = tr("Reconnecting...");
        m_actionButtonText = tr("Disconnect");
        break;
    }
    case Vpn::ConnectionState::Disconnected: {
        m_isConnectionInProgress = false;
        m_connectionStateText = tr("Connect");
        m_actionButtonText = tr("Connect");
        if (m_reconnectAfterDisconnect) {
            m_reconnectAfterDisconnect = false;
            QMetaObject::invokeMethod(this, &ConnectionController::openConnection, Qt::QueuedConnection);
        }
        break;
    }
    case Vpn::ConnectionState::Disconnecting: {
        m_isConnectionInProgress = true;
        m_connectionStateText = tr("Disconnecting...");
        m_actionButtonText = tr("Disconnect");
        break;
    }
    case Vpn::ConnectionState::Preparing: {
        m_isConnectionInProgress = true;
        m_connectionStateText = tr("Preparing...");
        m_actionButtonText = tr("Preparing...");
        break;
    }
    case Vpn::ConnectionState::Error: {
        m_isConnectionInProgress = false;
        m_connectionStateText = tr("Connect");
        m_actionButtonText = tr("Connect");

        // Try auto-failover if enabled and not too many attempts
        if (m_settings->isAutoFailoverEnabled() &&
            m_failoverAttempts < MAX_FAILOVER_ATTEMPTS &&
            tryNextFallback()) {
            return; // Don't emit error, trying fallback
        }

        resetAutoFailover();
        emit connectionErrorOccurred(getLastConnectionError());
        break;
    }
    case Vpn::ConnectionState::Unknown: {
        m_isConnectionInProgress = false;
        m_connectionStateText = tr("Connect");
        m_actionButtonText = tr("Connect");
        emit connectionErrorOccurred(getLastConnectionError());
        break;
    }
    }
    emit connectionStateChanged();
}

void ConnectionController::onCurrentContainerUpdated()
{
    if (m_isConnected || m_isConnectionInProgress) {
        m_reconnectAfterDisconnect = true;
        emit reconnectWithUpdatedContainer(tr("Settings updated successfully, reconnecting..."));
        closeConnection();
    } else {
        emit reconnectWithUpdatedContainer(tr("Settings updated successfully"));
    }
}

void ConnectionController::onTranslationsUpdated()
{
    // get translated text of current state
    onConnectionStateChanged(getCurrentConnectionState());
}

Vpn::ConnectionState ConnectionController::getCurrentConnectionState()
{
    return m_state;
}

QString ConnectionController::connectionStateText() const
{
    return m_connectionStateText;
}

QString ConnectionController::actionButtonText() const
{
    return m_actionButtonText;
}

void ConnectionController::toggleConnection()
{
    if (m_state == Vpn::ConnectionState::Preparing) {
        emit preparingConfig();
        return;
    }

    if (isConnectionInProgress()) {
        closeConnection();
    } else if (isConnected()) {
        closeConnection();
    } else {
        emit prepareConfig();
    }
}

bool ConnectionController::isConnectionInProgress() const
{
    return m_isConnectionInProgress;
}

bool ConnectionController::isConnected() const
{
    return m_isConnected;
}

bool ConnectionController::isAutoFailoverActive() const
{
    return m_autoFailoverActive;
}

void ConnectionController::tryAutoFailover()
{
    if (!m_settings->isAutoFailoverEnabled()) {
        return;
    }

    if (m_failoverAttempts >= MAX_FAILOVER_ATTEMPTS) {
        resetAutoFailover();
        return;
    }

    if (tryNextFallback()) {
        emit autoFailoverTriggered(tr("Trying fallback connection..."));
    }
}

void ConnectionController::resetAutoFailover()
{
    if (m_autoFailoverActive) {
        m_autoFailoverActive = false;
        emit autoFailoverStateChanged();
    }
    m_failoverAttempts = 0;
    m_originalServerIndex = -1;
    m_originalContainer = DockerContainer::None;
}

bool ConnectionController::tryNextFallback()
{
    int currentServerIndex = m_serversModel->getDefaultServerIndex();
    DockerContainer currentContainer = qvariant_cast<DockerContainer>(
        m_serversModel->data(currentServerIndex, ServersModel::Roles::DefaultContainerRole));

    // Save original settings on first attempt
    if (!m_autoFailoverActive) {
        m_originalServerIndex = currentServerIndex;
        m_originalContainer = currentContainer;
        m_autoFailoverActive = true;
        emit autoFailoverStateChanged();
    }

    m_failoverAttempts++;

    // Strategy: First try other containers on same server, then try other servers
    QJsonObject serverConfig = m_serversModel->getServerConfig(currentServerIndex);
    QJsonObject containers = serverConfig.value("containers").toObject();

    // Get list of available containers for this server
    QList<DockerContainer> availableContainers;
    for (const QString &key : containers.keys()) {
        DockerContainer c = ContainerProps::containerFromString(key);
        if (c != DockerContainer::None && c != currentContainer &&
            m_containersModel->isSupportedByCurrentPlatform(c)) {
            availableContainers.append(c);
        }
    }

    // Try next container on same server
    if (!availableContainers.isEmpty()) {
        DockerContainer nextContainer = availableContainers.first();
        emit autoFailoverTriggered(tr("Switching to %1...").arg(ContainerProps::containerHumanNames().value(nextContainer)));

        // Set the new default container and reconnect
        m_serversModel->setDefaultContainer(currentServerIndex, nextContainer);
        openConnection();
        return true;
    }

    // Try next server
    int serversCount = m_serversModel->getServersCount();
    if (serversCount > 1) {
        int nextServerIndex = (currentServerIndex + 1) % serversCount;
        if (nextServerIndex != m_originalServerIndex) {
            QString serverName = m_serversModel->data(nextServerIndex, ServersModel::Roles::NameRole).toString();
            emit autoFailoverTriggered(tr("Switching to server %1...").arg(serverName));

            m_settings->setDefaultServer(nextServerIndex);
            openConnection();
            return true;
        }
    }

    return false; // No more fallbacks available
}

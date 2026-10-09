#include "ipcserver.h"
#include "localPeerAuth.h"
#include "../client/daemon/daemon.h"

#include <QDateTime>
#include <QDebug>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QObject>
#include <QRemoteObjectHost>
#include <QRemoteObjectNode>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "logger.h"
#include "router.h"
#include "killswitch.h"
#include "networkdiagnostics.h"
#include "xray.h"

#ifdef Q_OS_WIN
    #include "tapcontroller_win.h"
#endif


IpcServer::IpcServer(QObject *parent) : IpcInterfaceSource(parent),
    m_ownerLease(this, [this](auto completed) { recoverOwner(std::move(completed)); }, 30000,
        [this] { return !hasOwnedResources(); })
{
    connect(&m_pingHelper, &PingHelper::connectionLose, this, &IpcServer::connectionLose);
}

int IpcServer::createPrivilegedProcess()
{
    if (!m_ownerLease.available()) return -1;
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::createPrivilegedProcess";
#endif

    m_localpid++;

    ProcessDescriptor pd(this);
    pd.ownerIdentity = m_ownerLease.identity();

    pd.localServer->setSocketOptions(QLocalServer::WorldAccessOption);

    if (!pd.localServer->listen(amnezia::getIpcProcessUrl(m_localpid))) {
        qDebug() << QString("Unable to start the server: %1.").arg(pd.localServer->errorString());
        return -1;
    }

    const int id = m_localpid;
    m_processes.insert(id, pd);
    // Never capture the descriptor by value in signals owned by its objects:
    // that creates a shared-pointer cycle and leaks every old process endpoint.
    QObject::connect(pd.localServer.data(), &QLocalServer::newConnection, this, [this, id]() {
        auto it = m_processes.find(id);
        if (it == m_processes.end()) return;
        auto *socket = it->localServer->nextPendingConnection();
        if (!socket) return;
        if (it->closing || amnezia::localPeerProcessIdentity(socket) != it->ownerIdentity || !acceptClient(socket)) {
            socket->abort(); socket->deleteLater(); return;
        }
        ++it->connectedPeers;
        ++it->peerGeneration;
        QObject::connect(socket, &QLocalSocket::disconnected, this, [this, id] {
            auto it = m_processes.find(id);
            if (it == m_processes.end()) return;
            it->connectedPeers = qMax(0, it->connectedPeers - 1);
            const auto generation = ++it->peerGeneration;
            QTimer::singleShot(30000, this, [this, id, generation] {
                auto it = m_processes.find(id);
                if (it != m_processes.end() && it->peerGeneration == generation) releasePrivilegedProcess(id);
            });
        });
        it->serverNode->addHostSideConnection(socket);
        if (!it->remotingEnabled)
            it->remotingEnabled = it->serverNode->enableRemoting(it->ipcProcess.data());
    });
    QObject::connect(pd.ipcProcess.data(), &IpcServerProcess::releaseRequested, this,
                     [this, id] { releasePrivilegedProcess(id, true); }, Qt::QueuedConnection);
    QObject::connect(pd.ipcProcess.data(), &IpcServerProcess::finished, this,
                     [this, id](int, QProcess::ExitStatus) {
                         auto it = m_processes.find(id);
                         if (it != m_processes.end() && it->closing) releasePrivilegedProcess(id, true);
                         else QTimer::singleShot(30000, this, [this, id] { releasePrivilegedProcess(id); });
                     });
    QObject::connect(pd.serverNode.data(), &QRemoteObjectHost::error, this,
                     [id](QRemoteObjectNode::ErrorCode code) { qDebug() << "Process IPC host error" << id << code; });
    // A generation prevents an old creation timer from truncating a later grace period.
    QTimer::singleShot(30000, this, [this, id] {
        auto it = m_processes.find(id);
        if (it != m_processes.end() && it->peerGeneration == 0) releasePrivilegedProcess(id);
    });
    return id;
}

void IpcServer::releasePrivilegedProcess(int id, bool explicitlyClosed)
{
    auto it = m_processes.find(id);
    if (it == m_processes.end()) return;
    if (!explicitlyClosed && it->connectedPeers > 0) return;
    it->closing = true;
    if (it->ipcProcess->isRunning()) {
        it->ipcProcess->kill(); // The exact service-owned child, never an EXE-name scan.
        return; // Retain until finished confirms the OS process exit.
    }
    if (it->remotingEnabled) it->serverNode->disableRemoting(it->ipcProcess.data());
    it->localServer->close();
    m_processes.erase(it);
}

bool IpcServer::acceptClient(QLocalSocket *socket)
{
    if (!amnezia::authorizeLocalPeer(socket)) return false;
    if (m_ownerLease.attach(socket, amnezia::localPeerProcessIdentity(socket))) return true;
    qWarning() << "IPC: another client owns the service or recovery is incomplete";
    socket->abort(); socket->deleteLater(); return false;
}

bool IpcServer::hasOwnedResources() const
{
    const auto *daemon = Daemon::instance();
    // Unknown native state cannot authorize handing the network to a new PID.
    return !daemon || daemon->hasActiveTunnel() || !m_processes.isEmpty()
        || m_ownedPolicy || m_ownedDns || m_ownedIpv6 || m_ownedXray || m_ownedRoutes
        || !m_ownedDevices.isEmpty();
}

void IpcServer::recoverOwner(std::function<void(bool)> completed)
{
    const auto owner = m_ownerLease.identity();
    for (auto it = m_processes.begin(); it != m_processes.end(); ++it) {
        if (it->ownerIdentity != owner) continue;
        it->closing = true;
        if (it->ipcProcess->isRunning()) it->ipcProcess->kill();
    }
    recoverOwnerAfterHelpers(std::move(completed), 100);
}

void IpcServer::recoverOwnerAfterHelpers(std::function<void(bool)> completed, int remainingPolls)
{
    const auto owner = m_ownerLease.identity();
    for (auto it = m_processes.begin(); it != m_processes.end(); ++it) {
        if (it->ownerIdentity != owner || !it->ipcProcess->isRunning()) continue;
        if (remainingPolls <= 0) {
            qCritical() << "Owner recovery: helper exit unconfirmed; retaining network ownership";
            completed(false); return;
        }
        QTimer::singleShot(50, this, [this, completed, remainingPolls] { recoverOwnerAfterHelpers(completed, remainingPolls - 1); });
        return;
    }
    bool success = !recoverNativeTunnel || recoverNativeTunnel();
    if (m_ownedPolicy) { const bool ok = KillSwitch::instance()->disableKillSwitch(); if (ok) m_ownedPolicy = false; success = ok && success; }
    if (m_ownedIpv6) { const bool ok = Router::StartRoutingIpv6(); if (ok) m_ownedIpv6 = false; success = ok && success; }
    if (m_ownedDns) { const bool ok = Router::restoreResolvers(); if (ok) m_ownedDns = false; success = ok && success; }
    if (m_ownedRoutes) { const bool ok = Router::clearSavedRoutes(); if (ok) m_ownedRoutes = false; success = ok && success; }
    if (!m_ownedPolicy) {
        for (const auto &device : QSet<QString>(m_ownedDevices)) {
            if (Router::deleteTun(device)) m_ownedDevices.remove(device);
            else success = false;
        }
    } else success = false;
    if (m_ownedXray) { const bool ok = Xray::getInstance().stopXray(); if (ok) m_ownedXray = false; success = ok && success; }
    if (!success) qCritical() << "Owner recovery incomplete; another client cannot take over";
    else {
        qInfo() << "Owner recovery completed; shared resources released";
        const auto ids = m_processes.keys();
        for (int id : ids) if (m_processes[id].ownerIdentity == owner) releasePrivilegedProcess(id, true);
    }
    completed(success);
}

int IpcServer::routeAddList(const QString &gw, const QStringList &ips)
{
    if (!m_ownerLease.available()) return -1;
    m_ownedRoutes = true;
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::routeAddList";
#endif

    return Router::routeAddList(gw, ips);
}

bool IpcServer::clearSavedRoutes()
{
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::clearSavedRoutes";
#endif

    const bool success = Router::clearSavedRoutes();
    if (success) m_ownedRoutes = false;
    return success;
}

bool IpcServer::routeDeleteList(const QString &gw, const QStringList &ips)
{
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::routeDeleteList";
#endif

    return Router::routeDeleteList(gw, ips);
}

bool IpcServer::flushDns()
{
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::flushDns";
#endif

    return Router::flushDns();
}

void IpcServer::resetIpStack()
{
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::resetIpStack";
#endif

    Router::resetIpStack();
}

bool IpcServer::checkAndInstallDriver()
{
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::checkAndInstallDriver";
#endif

#ifdef Q_OS_WIN
    return TapController::checkAndSetup();
#else
    return true;
#endif
}

QStringList IpcServer::getTapList()
{
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::getTapList";
#endif

#ifdef Q_OS_WIN
    return TapController::getTapList();
#else
    return QStringList();
#endif
}

void IpcServer::cleanUp()
{
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::cleanUp";
#endif

    Logger::deInit();
    Logger::cleanUp();
}

void IpcServer::clearLogs()
{
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::clearLogs";
#endif

    Logger::clearLogs(true);
}

bool IpcServer::createTun(const QString &dev, const QString &subnet)
{
    if (!m_ownerLease.available()) return false;
    m_ownedDevices.insert(dev);
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::createTun";
#endif

    return Router::createTun(dev, subnet);
}

bool IpcServer::deleteTun(const QString &dev)
{
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::deleteTun";
#endif

    const bool success = Router::deleteTun(dev);
    if (success) m_ownedDevices.remove(dev);
    return success;
}

bool IpcServer::updateResolvers(const QString &ifname, const QList<QHostAddress> &resolvers)
{
    if (!m_ownerLease.available()) return false;
    m_ownedDns = true;
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::updateResolvers";
#endif

    return Router::updateResolvers(ifname, resolvers);
}

bool IpcServer::restoreResolvers()
{
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::restoreResolvers";
#endif

    const bool success = Router::restoreResolvers();
    if (success) m_ownedDns = false;
    return success;
}

bool IpcServer::StartRoutingIpv6()
{
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::StartRoutingIpv6";
#endif

    const bool success = Router::StartRoutingIpv6();
    if (success) m_ownedIpv6 = false;
    return success;
}

bool IpcServer::StopRoutingIpv6()
{
    if (!m_ownerLease.available()) return false;
    m_ownedIpv6 = true;
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::StopRoutingIpv6";
#endif

    return Router::StopRoutingIpv6();
}

void IpcServer::setLogsEnabled(bool enabled)
{
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::setLogsEnabled";
#endif

    // Service file logging is always on; this is now a no-op.
    // Logger::init(true) is called at service startup in main.cpp.
    Q_UNUSED(enabled);
}

bool IpcServer::startNetworkCheck(const QString& serverIpv4Gateway, const QString& deviceIpv4Address)
{
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::startNetworkCheck";
#endif

    m_pingHelper.start(serverIpv4Gateway, deviceIpv4Address);
    return true;
}

bool IpcServer::stopNetworkCheck()
{
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::stopNetworkCheck";
#endif

    m_pingHelper.stop();
    return true;
}

QString IpcServer::runNetworkDiagnostics()
{
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::runNetworkDiagnostics";
#endif

    return NetworkDiagnostics::run();
}

bool IpcServer::resetKillSwitchAllowedRange(QStringList ranges)
{
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::resetKillSwitchAllowedRange";
#endif

    return KillSwitch::instance()->resetAllowedRange(ranges);
}

bool IpcServer::addKillSwitchAllowedRange(QStringList ranges)
{
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::addKillSwitchAllowedRange";
#endif

    return KillSwitch::instance()->addAllowedRange(ranges);
}

bool IpcServer::disableAllTraffic()
{
    if (!m_ownerLease.available()) return false;
    m_ownedPolicy = true;
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::disableAllTraffic";
#endif

    return KillSwitch::instance()->disableAllTraffic();
}

bool IpcServer::enableKillSwitch(const QJsonObject &configStr, int vpnAdapterIndex)
{
    if (!m_ownerLease.available()) return false;
    m_ownedPolicy = true;
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::enableKillSwitch";
#endif

    return KillSwitch::instance()->enableKillSwitch(configStr, vpnAdapterIndex);
}

bool IpcServer::disableKillSwitch()
{
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::disableKillSwitch";
#endif

    const bool success = KillSwitch::instance()->disableKillSwitch();
    if (success) m_ownedPolicy = false;
    return success;
}

bool IpcServer::enablePeerTraffic(const QJsonObject &configStr)
{
    if (!m_ownerLease.available()) return false;
    m_ownedPolicy = true;
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::enablePeerTraffic";
#endif

    return KillSwitch::instance()->enablePeerTraffic(configStr);
}

bool IpcServer::refreshKillSwitch(bool enabled)
{
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::refreshKillSwitch";
#endif

    if (!m_ownerLease.available()) return false;
    return KillSwitch::instance()->refresh(enabled, m_ownedPolicy || Daemon::instance()->hasActiveTunnel());
}

bool IpcServer::xrayStart(const QString& cfg)
{
    if (!m_ownerLease.available()) return false;
    m_ownedXray = true;
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::xrayStart";
#endif

    return Xray::getInstance().startXray(cfg);
}

bool IpcServer::xrayStop()
{
#ifdef MZ_DEBUG
    qDebug() << "IpcServer::xrayStop";
#endif

    const bool success = Xray::getInstance().stopXray();
    if (success) m_ownedXray = false;
    return success;
}

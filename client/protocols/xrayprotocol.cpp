#include "xrayprotocol.h"

#include "core/ipcclient.h"
#include "core/serialization/serialization.h"
#include "ipc.h"
#include "utilities.h"
#include "core/networkUtilities.h"

#include <exception>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QJsonDocument>
#include <QTcpSocket>
#include <QThread>
#include <QtCore/qlogging.h>
#include <QtCore/qobjectdefs.h>
#include <QtCore/qprocess.h>

#ifdef Q_OS_MACOS
static const QString tunName = "utun22";
#else
static const QString tunName = "tun2";
#endif

XrayProtocol::XrayProtocol(const QJsonObject &configuration, QObject *parent) : VpnProtocol(configuration, parent)
{
    m_vpnGateway = amnezia::protocols::xray::defaultLocalAddr;
    m_vpnLocalAddress = amnezia::protocols::xray::defaultLocalAddr;
    m_routeGateway = NetworkUtilities::getGatewayAndIface().first;

    m_routeMode = static_cast<Settings::RouteMode>(configuration.value(amnezia::config_key::splitTunnelType).toInt());
    m_remoteAddress = NetworkUtilities::getIPAddress(m_rawConfig.value(amnezia::config_key::hostName).toString());

    const QString primaryDns = configuration.value(amnezia::config_key::dns1).toString();
    m_dnsServers.push_back(QHostAddress(primaryDns));
    if (primaryDns != amnezia::protocols::dns::amneziaDnsIp) {
        const QString secondaryDns = configuration.value(amnezia::config_key::dns2).toString();
        m_dnsServers.push_back(QHostAddress(secondaryDns));
    }

    QJsonObject xrayConfiguration = configuration.value(ProtocolProps::key_proto_config_data(Proto::Xray)).toObject();
    if (xrayConfiguration.isEmpty()) {
        xrayConfiguration = configuration.value(ProtocolProps::key_proto_config_data(Proto::SSXray)).toObject();
    }
    m_xrayConfig = xrayConfiguration;
}

XrayProtocol::~XrayProtocol()
{
    qDebug() << "XrayProtocol::~XrayProtocol()";
    XrayProtocol::stop();
}

namespace {
bool waitForSocketBytes(QTcpSocket &socket, qint64 minBytes, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();

    while (socket.bytesAvailable() < minBytes) {
        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0 || !socket.waitForReadyRead(qMin(remaining, 250))) {
            return false;
        }
    }

    return true;
}

bool isAppSplitTunnelActive(const QJsonObject &config)
{
    const auto appsRouteMode = static_cast<Settings::AppsRouteMode>(
        config.value(amnezia::config_key::appSplitTunnelType).toInt());

    return appsRouteMode != Settings::AppsRouteMode::VpnAllApps
        && !config.value(amnezia::config_key::splitTunnelApps).toArray().isEmpty();
}
}

ErrorCode XrayProtocol::start()
{
    qDebug() << "XrayProtocol::start()";
    m_stopping = false;

    try {
        const auto creds = amnezia::serialization::inbounds::EnsureInboundAuth(m_xrayConfig);
        m_socksUser = creds.username;
        m_socksPassword = creds.password;
        m_socksPort = creds.port;
    } catch (const std::exception &e) {
        qCritical() << "Failed to prepare XRay SOCKS inbound:" << e.what();
        return ErrorCode::InternalError;
    }

    // Ensure DNS resolves over the tunnel via DoH (TCP). Plain UDP DNS does not
    // return reliably through VLESS+Reality(+vision), so names never resolved
    // and sites would not open even though the proxy itself worked. by vovankrot
    ensureDnsOverDoh(m_xrayConfig);

    return IpcClient::withInterface([&](QSharedPointer<IpcInterfaceReplica> iface) {
        ErrorCode error = startXrayProcess(iface);
        if (error != ErrorCode::NoError) {
            return error;
        }

        if (!ensureProxyReachable()) {
            qWarning() << "Initial XRay proxy probe failed. Retrying XRay start before routing and killswitch.";

            auto xrayStop = iface->xrayStop();
            if (!xrayStop.waitForFinished() || !xrayStop.returnValue()) {
                qWarning() << "Failed to stop xray before retry";
            }

            error = startXrayProcess(iface);
            if (error != ErrorCode::NoError) {
                return error;
            }

            if (!ensureProxyReachable()) {
                qCritical() << "XRay proxy probe failed after reconnect attempt";
                return ErrorCode::XrayExecutableCrashed;
            }
        }

        return startTun2Socks();
    }, [] () {
        return ErrorCode::AmneziaServiceConnectionFailed;
    });
}

void XrayProtocol::ensureDnsOverDoh(QJsonObject &config)
{
    // Minimal, surgical runtime fix: ONLY disable mux on the proxy outbound.
    // mux is INCOMPATIBLE with xtls-rprx-vision — an enabled mux corrupts the
    // data stream (server logs "common/mux: failed to read metadata", the
    // connection resets), so requests are accepted but no response returns and
    // sites never open. vision itself is kept — it's what hides the traffic from
    // DPI/Roskomnadzor. (Earlier DoH/dns/routing injection removed: it was extra
    // surgery on the imported config that could itself break the data path; the
    // bundled amnezia xray core is official and works for others.) by vovankrot
    QJsonArray outbounds = config.value("outbounds").toArray();
    if (!outbounds.isEmpty()) {
        QJsonObject proxy = outbounds.at(0).toObject();
        QJsonObject mux = proxy.value("mux").toObject();
        mux["enabled"] = false;
        proxy["mux"] = mux;
        outbounds[0] = proxy;
        config["outbounds"] = outbounds;
    }
}

ErrorCode XrayProtocol::startXrayProcess(const QSharedPointer<IpcInterfaceReplica> &iface)
{
    auto xrayStart = iface->xrayStart(QJsonDocument(m_xrayConfig).toJson());
    if (!xrayStart.waitForFinished() || !xrayStart.returnValue()) {
        qCritical() << "Failed to start xray";
        return ErrorCode::XrayExecutableCrashed;
    }

    return ErrorCode::NoError;
}

void XrayProtocol::stop()
{
    qDebug() << "XrayProtocol::stop()";
    m_stopping = true;

    // Use short timeouts (2s) to prevent disconnect from hanging
    // if service is slow to respond. Default Qt RO timeout is 30s!
    constexpr int kIpcTimeoutMs = 2000;

    IpcClient::withInterface([](QSharedPointer<IpcInterfaceReplica> iface) {
        auto disableKillSwitch = iface->disableKillSwitch();
        if (!disableKillSwitch.waitForFinished(kIpcTimeoutMs) || !disableKillSwitch.returnValue())
            qWarning() << "Failed to disable killswitch";

        auto StartRoutingIpv6 = iface->StartRoutingIpv6();
        if (!StartRoutingIpv6.waitForFinished(kIpcTimeoutMs) || !StartRoutingIpv6.returnValue())
            qWarning() << "Failed to start routing ipv6";

        auto restoreResolvers = iface->restoreResolvers();
        if (!restoreResolvers.waitForFinished(kIpcTimeoutMs) || !restoreResolvers.returnValue())
            qWarning() << "Failed to restore resolvers";

        auto deleteTun = iface->deleteTun(tunName);
        if (!deleteTun.waitForFinished(kIpcTimeoutMs) || !deleteTun.returnValue())
            qWarning() << "Failed to delete tun";

        auto xrayStop = iface->xrayStop();
        if (!xrayStop.waitForFinished(kIpcTimeoutMs) || !xrayStop.returnValue())
            qWarning() << "Failed to stop xray";
    });

    if (m_tun2socksProcess) {
        m_tun2socksProcess->blockSignals(true);

#ifndef Q_OS_WIN
        m_tun2socksProcess->terminate();
        auto waitForFinished = m_tun2socksProcess->waitForFinished(1000);
        if (!waitForFinished.waitForFinished() || !waitForFinished.returnValue()) {
            qWarning() << "Failed to terminate tun2socks. Killing the process...";
            m_tun2socksProcess->kill();
        }
#else
        // terminate does not do anything useful on Windows
        // so just kill the process
        m_tun2socksProcess->kill();
#endif

        m_tun2socksProcess->close();
        m_tun2socksProcess.reset();
    }

    setConnectionState(Vpn::ConnectionState::Disconnected);
}

ErrorCode XrayProtocol::startTun2Socks()
{
    m_tun2socksProcess = IpcClient::CreatePrivilegedProcess();
    if (!m_tun2socksProcess->waitForSource()) {
        return ErrorCode::AmneziaServiceConnectionFailed;
    }

    // SOCKS port + creds were prepared in start() via EnsureInboundAuth.
    QString proxyUrl;
    if (!m_socksUser.isEmpty() && !m_socksPassword.isEmpty()) {
        proxyUrl = QString("socks5://%1:%2@127.0.0.1:%3").arg(m_socksUser, m_socksPassword).arg(m_socksPort);
    } else {
        proxyUrl = QString("socks5://127.0.0.1:%1").arg(m_socksPort);
    }

    m_tun2socksProcess->setProgram(PermittedProcess::Tun2Socks);
    m_tun2socksProcess->setArguments({
        "-device", QString("tun://%1").arg(tunName),
        "-proxy", proxyUrl,
        // NOTE 1: bundled tun2socks (xjasonlyu, c8f8cb5) has NO `-stack` flag —
        // it's compiled with a single netstack (gVisor) selected at build time.
        // Passing `-stack lwip` makes it exit with code 2 (flag parse error),
        // which the client reports as ErrorCode 804. Do NOT add `-stack` here
        // unless you also replace tun2socks.exe with a build that supports it.
        //
        // NOTE 2: `-tcp-auto-tuning` IS recognized by --help but crashes the
        // bundled Windows build (c8f8cb5) at runtime ~5–8 s after the first
        // real TCP flow — observed 2026-04-25 09:39 in the service log as a
        // repeating QProcess::Crashed loop ("sites ping but don't open"
        // because gVisor answers ICMP locally even after the proxy dies).
        // Do NOT re-enable until tun2socks.exe is rebuilt from a newer xjasonlyu
        // tag with the auto-tuning crash fix and verified on Windows.
    });

    connect(m_tun2socksProcess.data(), &IpcProcessInterfaceReplica::readyReadStandardOutput, this, [this]() {
        if (m_stopping || !m_tun2socksProcess) {
            return;
        }

        auto readAllStandardOutput = m_tun2socksProcess->readAllStandardOutput();
        if (!readAllStandardOutput.waitForFinished()) {
            qWarning() << "Failed to read output from tun2socks";
            return;
        }

        const QString line = readAllStandardOutput.returnValue();

        if (!line.contains("[TCP]") && !line.contains("[UDP]"))
            qDebug() << "[tun2socks]:" << line;
        
        if (line.contains("[STACK] tun://") && line.contains("<-> socks5://")) {
            disconnect(m_tun2socksProcess.data(), &IpcProcessInterfaceReplica::readyReadStandardOutput, this, nullptr);

            if (ErrorCode res = setupRouting(); res != ErrorCode::NoError) {
                stop();
                setLastError(res);
            } else {
                setConnectionState(Vpn::ConnectionState::Connected);
            }
        }
    }, Qt::QueuedConnection);

    connect(m_tun2socksProcess.data(), &IpcProcessInterfaceReplica::finished, this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (m_stopping || connectionState() == Vpn::ConnectionState::Disconnecting
            || connectionState() == Vpn::ConnectionState::Disconnected) {
            qDebug() << "Tun2socks (xray) finished during controlled shutdown, code" << exitCode
                     << "status" << exitStatus;
            return;
        }

        if (exitStatus == QProcess::ExitStatus::CrashExit) {
            qCritical() << "Tun2socks process crashed!";
        } else {
            qCritical() << QString("Tun2socks process was closed with %1 exit code").arg(exitCode);
        }
        stop();
        setLastError(ErrorCode::Tun2SockExecutableCrashed);
    }, Qt::QueuedConnection);

    m_tun2socksProcess->start();
    return ErrorCode::NoError;
}

int XrayProtocol::localSocksPort() const
{
    if (m_socksPort > 0)
        return m_socksPort;

    const QJsonArray inbounds = m_xrayConfig.value("inbounds").toArray();
    if (!inbounds.isEmpty()) {
        return inbounds.first().toObject().value("port").toInt(10808);
    }
    return 10808;
}

QString XrayProtocol::probeHost() const
{
    const QJsonArray outbounds = m_xrayConfig.value("outbounds").toArray();
    if (!outbounds.isEmpty()) {
        const QJsonObject streamSettings = outbounds.first().toObject().value("streamSettings").toObject();
        const QJsonObject realitySettings = streamSettings.value("realitySettings").toObject();
        const QString serverName = realitySettings.value("serverName").toString().trimmed();
        if (!serverName.isEmpty()) {
            return serverName;
        }
    }

    const QString configuredSite = m_rawConfig.value(config_key::site).toString().trimmed();
    if (!configuredSite.isEmpty()) {
        return configuredSite;
    }

    return QString::fromLatin1(amnezia::protocols::xray::defaultSite);
}

quint16 XrayProtocol::probePort() const
{
    bool ok = false;
    const quint16 configuredPort = m_rawConfig.value(config_key::port)
                                       .toString(QString::fromLatin1(amnezia::protocols::xray::defaultPort))
                                       .toUShort(&ok);
    return ok && configuredPort > 0 ? configuredPort : 443;
}

bool XrayProtocol::ensureProxyReachable()
{
    return performSocks5Probe(probeHost(), probePort(), 3500);
}

bool XrayProtocol::performSocks5Probe(const QString &targetHost, quint16 targetPort, int timeoutMs)
{
    const QByteArray hostBytes = targetHost.toUtf8();
    if (hostBytes.isEmpty() || hostBytes.size() > 255) {
        qWarning() << "Skipping XRay proxy probe because target host is invalid:" << targetHost;
        return false;
    }

    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < timeoutMs) {
        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0) {
            break;
        }

        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, localSocksPort());
        if (!socket.waitForConnected(qMin(remaining, 700))) {
            QThread::msleep(150);
            continue;
        }

        // Offer NO_AUTH (0x00) and USERNAME/PASSWORD (0x02). xray accepts whichever it has configured.
        socket.write(QByteArray::fromHex("05020002"));
        if (!socket.waitForBytesWritten(qMin(remaining, 500)) || !waitForSocketBytes(socket, 2, qMin(remaining, 700))) {
            socket.disconnectFromHost();
            QThread::msleep(150);
            continue;
        }

        const QByteArray authReply = socket.read(2);
        if (authReply.size() < 2 || quint8(authReply.at(0)) != 0x05) {
            socket.disconnectFromHost();
            QThread::msleep(150);
            continue;
        }
        const quint8 chosenMethod = quint8(authReply.at(1));
        if (chosenMethod == 0x02) {
            // RFC 1929 username/password sub-negotiation.
            const QByteArray userBytes = m_socksUser.toUtf8();
            const QByteArray passBytes = m_socksPassword.toUtf8();
            if (userBytes.size() > 255 || passBytes.size() > 255 || userBytes.isEmpty() || passBytes.isEmpty()) {
                socket.disconnectFromHost();
                QThread::msleep(150);
                continue;
            }
            QByteArray authReq;
            authReq.append(char(0x01));
            authReq.append(char(userBytes.size()));
            authReq.append(userBytes);
            authReq.append(char(passBytes.size()));
            authReq.append(passBytes);
            socket.write(authReq);
            if (!socket.waitForBytesWritten(qMin(remaining, 500)) || !waitForSocketBytes(socket, 2, qMin(remaining, 700))) {
                socket.disconnectFromHost();
                QThread::msleep(150);
                continue;
            }
            const QByteArray authResp = socket.read(2);
            if (authResp.size() < 2 || quint8(authResp.at(1)) != 0x00) {
                socket.disconnectFromHost();
                QThread::msleep(150);
                continue;
            }
        } else if (chosenMethod != 0x00) {
            socket.disconnectFromHost();
            QThread::msleep(150);
            continue;
        }

        QByteArray request;
        request.append(char(0x05));
        request.append(char(0x01));
        request.append(char(0x00));
        request.append(char(0x03));
        request.append(char(hostBytes.size()));
        request.append(hostBytes);
        request.append(char((targetPort >> 8) & 0xff));
        request.append(char(targetPort & 0xff));

        socket.write(request);
        if (!socket.waitForBytesWritten(qMin(remaining, 500)) || !waitForSocketBytes(socket, 5, qMin(remaining, 1200))) {
            socket.disconnectFromHost();
            QThread::msleep(150);
            continue;
        }

        const QByteArray connectReply = socket.peek(5);
        if (connectReply.size() >= 5 && quint8(connectReply.at(0)) == 0x05 && quint8(connectReply.at(1)) == 0x00) {
            socket.disconnectFromHost();
            qDebug() << "XRay SOCKS probe succeeded for" << targetHost << targetPort;
            return true;
        }

        socket.disconnectFromHost();
        QThread::msleep(150);
    }

    qWarning() << "XRay SOCKS probe failed for" << targetHost << targetPort;
    return false;
}

ErrorCode XrayProtocol::setupRouting() {
    return IpcClient::withInterface([this](QSharedPointer<IpcInterfaceReplica> iface) -> ErrorCode {
        QString tunGateway = m_vpnGateway;
        if (!NetworkUtilities::checkIPv4Format(tunGateway)) {
            qWarning() << "XRay setupRouting: invalid TUN gateway" << tunGateway
                       << "using default" << amnezia::protocols::xray::defaultLocalAddr;
            tunGateway = amnezia::protocols::xray::defaultLocalAddr;
        }
        if (!NetworkUtilities::checkIPv4Format(tunGateway)) {
            qCritical() << "XRay setupRouting: default TUN gateway is invalid" << tunGateway;
            return ErrorCode::InternalError;
        }

#ifdef Q_OS_WIN
        const int inetAdapterIndex = NetworkUtilities::AdapterIndexTo(QHostAddress(m_remoteAddress));
#endif
        auto createTun = iface->createTun(tunName, amnezia::protocols::xray::defaultLocalAddr);
        if (!createTun.waitForFinished() || !createTun.returnValue()) {
            qCritical() << "Failed to assign IP address for TUN";
            return ErrorCode::InternalError;
        }

        auto updateResolvers = iface->updateResolvers(tunName, m_dnsServers);
        if (!updateResolvers.waitForFinished() || !updateResolvers.returnValue()) {
            qCritical() << "Failed to set DNS resolvers for TUN";
            return ErrorCode::InternalError;
        }

#ifdef Q_OS_WIN
        int vpnAdapterIndex = -1;
        QList<QNetworkInterface> netInterfaces = QNetworkInterface::allInterfaces();
        for (auto& netInterface : netInterfaces) {
            for (auto& address : netInterface.addressEntries()) {
                if (m_vpnLocalAddress == address.ip().toString())
                    vpnAdapterIndex = netInterface.index();
            }
        }
#else
        static const int vpnAdapterIndex = 0;
#endif
        const bool killSwitchEnabled = QVariant(m_rawConfig.value(config_key::killSwitchOption).toString()).toBool();
        const bool appSplitTunnelActive = isAppSplitTunnelActive(m_rawConfig);
        if (killSwitchEnabled && appSplitTunnelActive) {
            qDebug() << "Skipping strict killswitch firewall rules while app split tunneling is active";
        } else if (killSwitchEnabled) {
            if (vpnAdapterIndex != -1) {
                QJsonObject config = m_rawConfig;
                config.insert("vpnServer", m_remoteAddress);

                auto enableKillSwitch = IpcClient::Interface()->enableKillSwitch(config, vpnAdapterIndex);
                if (!enableKillSwitch.waitForFinished() || !enableKillSwitch.returnValue()) {
                    qCritical() << "Failed to enable killswitch";
                    return ErrorCode::InternalError;
                }
            } else
                qWarning() << "Failed to get vpnAdapterIndex. Killswitch disabled";
        }

        // For both VpnAllSites and VpnAllExceptSites, all OS traffic must enter the TUN.
        // Xray routing rules (geoip:ru → direct, blocked domains → proxy, etc.) handle
        // the per-destination decision inside the tunnel. Without these OS-level routes
        // the TUN is created but no traffic reaches it, so the VPN appears connected
        // but does not work.  VpnOnlySites is intentionally excluded — only specific
        // destination CIDRs should be routed through the TUN in that mode.
        if (m_routeMode == Settings::RouteMode::VpnAllSites ||
            m_routeMode == Settings::RouteMode::VpnAllExceptSites) {
            // Exclude the XRay server's own IP from the TUN. The catch-all
            // subnets below (1.0.0.0/8 ... 128.0.0.0/1) otherwise capture the
            // server endpoint (e.g. 203.0.113.10 falls inside 32.0.0.0/3) into
            // the tunnel, which then depends on a server it can no longer reach
            // -> the xray outbound times out and ALL traffic dies
            // ("Timeout connecting to <server>", sites do not open). A /32 via
            // the physical gateway is more specific, so server traffic bypasses
            // the TUN. Mirrors WireGuard's addExclusionRoute(serverEndpoint).
            // by vovankrot
            if (NetworkUtilities::checkIPv4Format(m_remoteAddress)
                && NetworkUtilities::checkIPv4Format(m_routeGateway)) {
                const QStringList serverExclusion = { m_remoteAddress + "/32" };
                auto excludeServer = iface->routeAddList(m_routeGateway, serverExclusion);
                if (!excludeServer.waitForFinished() || excludeServer.returnValue() != serverExclusion.count()) {
                    qWarning() << "XRay setupRouting: failed to add server exclusion route for"
                               << m_remoteAddress << "via" << m_routeGateway;
                } else {
                    qDebug() << "XRay setupRouting: excluded server" << m_remoteAddress
                             << "via" << m_routeGateway;
                }
            } else {
                qWarning() << "XRay setupRouting: cannot exclude server, invalid address/gateway"
                           << m_remoteAddress << m_routeGateway;
            }

            const QStringList subnets = { "1.0.0.0/8", "2.0.0.0/7", "4.0.0.0/6", "8.0.0.0/5", "16.0.0.0/4", "32.0.0.0/3", "64.0.0.0/2", "128.0.0.0/1" };

            qDebug() << "XRay setupRouting: adding TUN routes via" << tunGateway << "count" << subnets.count();
            auto routeAddList =  iface->routeAddList(tunGateway, subnets);
            if (!routeAddList.waitForFinished() || routeAddList.returnValue() != subnets.count()) {
                qCritical() << "Failed to set routes for TUN";
                return ErrorCode::InternalError;
            }
        }

        auto StopRoutingIpv6 = iface->StopRoutingIpv6();
        if (!StopRoutingIpv6.waitForFinished() || !StopRoutingIpv6.returnValue()) {
            qCritical() << "Failed to disable IPv6 routing";
            return ErrorCode::InternalError;
        }

#ifdef Q_OS_WIN
        // enablePeerTraffic drives TWO things inside KillSwitch::enablePeerTraffic: the
        // strict firewall block (gated there on killSwitchOption) AND, unconditionally,
        // WindowsDaemon::activateSplitTunnel -- the latter is what actually engages the WFP
        // per-app split-tunnel driver. So this must fire whenever the kill-switch is on OR
        // per-app split tunnelling is active. It was previously gated on killSwitchEnabled
        // alone, so with app-split ON + kill-switch OFF the WFP driver never engaged and
        // per-app split tunnelling silently did nothing on XRay -- the same bug that was
        // fixed for Hysteria2 (hysteria2protocol.cpp) but never ported here.
        //
        // CRITICAL (LAN-safe): when app-split is active we stamp killSwitchOption=false.
        // The strict killswitch (enableKillSwitch, which adds the "Allow VPN Adapter"
        // escape) is skipped above for app-split, so letting enablePeerTraffic install
        // "Block Internet 0.0.0.0/0" here would leave the block with NO tunnel-adapter
        // escape -> every non-bypassed app loses all internet. Stamping false skips only
        // the firewall block; activateSplitTunnel (the WFP driver) still runs
        // unconditionally inside enablePeerTraffic, so app-split works, and the LAN bypass
        // (enableLanBypass, applied unconditionally service-side) stays intact. by vovankrot
        if (killSwitchEnabled || appSplitTunnelActive) {
            if (inetAdapterIndex != -1 && vpnAdapterIndex != -1) {
                QJsonObject config = m_rawConfig;
                config.insert("inetAdapterIndex", inetAdapterIndex);
                config.insert("vpnAdapterIndex", vpnAdapterIndex);
                config.insert("vpnGateway", tunGateway);
                config.insert("vpnServer", m_remoteAddress);
                config.insert(config_key::killSwitchOption,
                              (killSwitchEnabled && !appSplitTunnelActive) ? "true" : "false");

                auto enablePeerTraffic = iface->enablePeerTraffic(config);
                if (!enablePeerTraffic.waitForFinished() || !enablePeerTraffic.returnValue()) {
                    qCritical() << "XRay: failed to enable peer traffic / app split tunnel";
                    return ErrorCode::InternalError;
                }
            } else {
                qWarning() << "XRay: split-tunnel adapter indices unknown, app-split/killswitch skipped"
                           << "inet=" << inetAdapterIndex << "vpn=" << vpnAdapterIndex;
            }
        }
#endif
        return ErrorCode::NoError;
    },
    [] () {
        return ErrorCode::AmneziaServiceConnectionFailed;
    });
}

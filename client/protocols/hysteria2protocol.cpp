#include "hysteria2protocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QThread>

#include "core/ipcclient.h"
#include "core/networkUtilities.h"
#include "core/serialization/serialization.h"
#include "ipc.h"
#include "protocols/protocols_defs.h"
#include "utilities.h"

#ifdef Q_OS_MACOS
static const QString tunName = "utun22";
#else
static const QString tunName = "tun2";
#endif

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

QString upgradeLegacyBandwidthHints(QString yamlConfig)
{
    // Strip ANY cached bandwidth block -> BBR (adaptive). The user only RECONNECTS (never
    // reinstalls hysteria), so old configs carry whatever bandwidth hint was baked at the
    // last install (1 gbps / 200 mbps). Brutal at a FIXED rate is wrong for this variable,
    // RKN-shaped cross-border link: it caps throughput at the declared rate and, whenever the
    // path dips below it, overshoots and manufactures loss -> throughput collapses (measured:
    // Brutal-25/80 tanked the speed badly). BBR adapts to the real, varying capacity (~86
    // Mbit/s). NOTE: Brutal did NOT fix the Discord voice drops either -> that loss is on the
    // path (cross-border peering to a flagged IP), not a CC problem; CC can't fix it. So we
    // always normalize to BBR here and chase the voice loss elsewhere (path/VPS). by vovankrot
    static const QRegularExpression bandwidthBlockRe(
        QStringLiteral("(?m)^bandwidth:[ \\t]*\\n[ \\t]+up:[^\\n]*\\n[ \\t]+down:[^\\n]*\\n?"));
    yamlConfig.remove(bandwidthBlockRe);
    return yamlConfig;
}
} // namespace

Hysteria2Protocol::Hysteria2Protocol(const QJsonObject &configuration, QObject *parent)
    : VpnProtocol(configuration, parent)
{
    m_vpnGateway = amnezia::protocols::hysteria2::defaultLocalAddr;
    m_vpnLocalAddress = amnezia::protocols::hysteria2::defaultLocalAddr;
    m_routeGateway = NetworkUtilities::getGatewayAndIface().first;

    m_routeMode = static_cast<Settings::RouteMode>(configuration.value(amnezia::config_key::splitTunnelType).toInt());
    m_remoteAddress = NetworkUtilities::getIPAddress(m_rawConfig.value(amnezia::config_key::hostName).toString());

    const QString primaryDns = configuration.value(amnezia::config_key::dns1).toString();
    m_dnsServers.push_back(QHostAddress(primaryDns));
    if (primaryDns != amnezia::protocols::dns::amneziaDnsIp) {
        const QString secondaryDns = configuration.value(amnezia::config_key::dns2).toString();
        m_dnsServers.push_back(QHostAddress(secondaryDns));
    }

    // The Hysteria2 configurator stores a YAML payload (not JSON) inside the
    // protocol config slot. vpnConfigurationController wraps it in a JSON
    // object: { "yaml_config": "...", "local_port": "...", "site": "..." }.
    const QJsonObject h2 = configuration.value(ProtocolProps::key_proto_config_data(Proto::Hysteria2)).toObject();
    m_yamlConfig = upgradeLegacyBandwidthHints(h2.value(QStringLiteral("yaml_config")).toString());

    const QString localPortStr = h2.value(QStringLiteral("local_port")).toString();
    bool ok = false;
    int parsed = localPortStr.toInt(&ok);
    if (ok && parsed > 0 && parsed < 65536) {
        m_socksPort = parsed;
    } else {
        // Fallback: try to extract socks5 listen port from the rendered YAML itself.
        const int yamlPort = parseSocksPortFromYaml();
        if (yamlPort > 0) {
            m_socksPort = yamlPort;
        }
    }

    m_masqueradeHost = h2.value(QStringLiteral("site")).toString();
    if (m_masqueradeHost.isEmpty()) {
        m_masqueradeHost = QString::fromLatin1(amnezia::protocols::hysteria2::defaultMasqueradeHost);
    }

    m_xrayRouterConfig = configuration.value(amnezia::config_key::xrayRouterConfig).toObject();
}

Hysteria2Protocol::~Hysteria2Protocol()
{
    qDebug() << "Hysteria2Protocol::~Hysteria2Protocol()";
    Hysteria2Protocol::stop();
}

int Hysteria2Protocol::parseSocksPortFromYaml() const
{
    // Look for "listen: 127.0.0.1:NNNN" inside the socks5 block.
    static const QRegularExpression re(QStringLiteral("listen\\s*:\\s*127\\.0\\.0\\.1:(\\d+)"));
    const auto match = re.match(m_yamlConfig);
    if (match.hasMatch()) {
        return match.captured(1).toInt();
    }
    return 0;
}

quint16 Hysteria2Protocol::remotePort() const
{
    bool ok = false;
    const QJsonObject h2 = m_rawConfig.value(ProtocolProps::key_proto_config_data(Proto::Hysteria2)).toObject();
    const quint16 p = h2.value(QStringLiteral("port")).toString().toUShort(&ok);
    if (ok && p > 0) {
        return p;
    }
    return QString::fromLatin1(amnezia::protocols::hysteria2::defaultPort).toUShort();
}

QString Hysteria2Protocol::writeConfigToTempFile()
{
    const QString tmpDir = QDir::tempPath();
    QDir().mkpath(tmpDir);

    const QString name = QStringLiteral("hysteria2_%1.yaml")
                             .arg(QString::number(QCoreApplication::applicationPid()));
    const QString path = QDir(tmpDir).absoluteFilePath(name);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "Hysteria2Protocol: failed to open config file for writing:" << path;
        return {};
    }
    f.write(m_yamlConfig.toUtf8());
    f.close();
    return path;
}

ErrorCode Hysteria2Protocol::start()
{
    qDebug() << "Hysteria2Protocol::start()";
    m_stopping = false;

    if (m_yamlConfig.isEmpty()) {
        qCritical() << "Hysteria2Protocol::start(): empty yaml_config in configuration";
        return ErrorCode::InternalError;
    }

    const QString hysteriaExe = Utils::hysteriaPath();
    if (!QFileInfo::exists(hysteriaExe)) {
        qCritical() << "Hysteria2Protocol::start(): hysteria executable not found at" << hysteriaExe;
        return ErrorCode::InternalError;
    }

    if (ErrorCode code = startHysteriaProcess(); code != ErrorCode::NoError) {
        return code;
    }

    if (!ensureProxyReachable()) {
        qWarning() << "Initial Hysteria2 proxy probe failed. Retrying once.";
        if (m_hysteriaProcess) {
            m_hysteriaProcess->blockSignals(true);
            m_hysteriaProcess->kill();
            m_hysteriaProcess->waitForFinished(1500);
            m_hysteriaProcess->deleteLater();
            m_hysteriaProcess.clear();
        }
        if (ErrorCode code = startHysteriaProcess(); code != ErrorCode::NoError) {
            return code;
        }
        if (!ensureProxyReachable()) {
            qCritical() << "Hysteria2 proxy probe failed after reconnect attempt";
            return ErrorCode::InternalError;
        }
    }

    return IpcClient::withInterface([&](QSharedPointer<IpcInterfaceReplica> iface) -> ErrorCode {
        if (!m_xrayRouterConfig.isEmpty()) {
            if (ErrorCode code = startXrayRouter(iface); code != ErrorCode::NoError) {
                return code;
            }
            if (!ensureXrayRouterReachable()) {
                qWarning() << "Initial Hysteria2 XRay router probe failed. Retrying XRay router once.";
                auto xrayStop = iface->xrayStop();
                if (!xrayStop.waitForFinished(2000) || !xrayStop.returnValue()) {
                    qWarning() << "Failed to stop Hysteria2 XRay router before retry";
                }
                if (ErrorCode code = startXrayRouter(iface); code != ErrorCode::NoError) {
                    return code;
                }
                if (!ensureXrayRouterReachable()) {
                    qCritical() << "Hysteria2 XRay router probe failed after reconnect attempt";
                    return ErrorCode::XrayExecutableCrashed;
                }
            }
        }
        return startTun2Socks();
    }, [] () {
        return ErrorCode::AmneziaServiceConnectionFailed;
    });
}

ErrorCode Hysteria2Protocol::startXrayRouter(const QSharedPointer<IpcInterfaceReplica> &iface)
{
    try {
        const auto creds = amnezia::serialization::inbounds::EnsureInboundAuth(m_xrayRouterConfig);
        m_xrayRouterUser = creds.username;
        m_xrayRouterPassword = creds.password;
        m_xrayRouterSocksPort = creds.port;
    } catch (const std::exception &e) {
        qCritical() << "Failed to prepare Hysteria2 XRay router SOCKS inbound:" << e.what();
        return ErrorCode::InternalError;
    }

    auto xrayStart = iface->xrayStart(QJsonDocument(m_xrayRouterConfig).toJson());
    if (!xrayStart.waitForFinished() || !xrayStart.returnValue()) {
        qCritical() << "Failed to start Hysteria2 XRay router";
        return ErrorCode::XrayExecutableCrashed;
    }

    qDebug() << "Hysteria2 XRay router started on local SOCKS port" << m_xrayRouterSocksPort;
    return ErrorCode::NoError;
}

ErrorCode Hysteria2Protocol::startHysteriaProcess()
{
    m_configPath = writeConfigToTempFile();
    if (m_configPath.isEmpty()) {
        return ErrorCode::InternalError;
    }

    m_hysteriaProcess = new QProcess(this);
    m_hysteriaProcess->setProgram(Utils::hysteriaPath());
    m_hysteriaProcess->setArguments({ QStringLiteral("client"), QStringLiteral("--disable-update-check"),
                                      QStringLiteral("-c"), m_configPath });
    m_hysteriaProcess->setProcessChannelMode(QProcess::MergedChannels);

    connect(m_hysteriaProcess.data(), &QProcess::readyReadStandardOutput, this, [this]() {
        if (!m_hysteriaProcess)
            return;
        const QByteArray out = m_hysteriaProcess->readAllStandardOutput();
        for (const QByteArray &lineRaw : out.split('\n')) {
            const QString line = QString::fromUtf8(lineRaw).trimmed();
            if (!line.isEmpty()) {
                qDebug().noquote() << "[hysteria]" << line;
            }
        }
    });

    connect(m_hysteriaProcess.data(),
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                if (m_stopping || connectionState() == Vpn::ConnectionState::Disconnecting
                    || connectionState() == Vpn::ConnectionState::Disconnected) {
                    qDebug() << "Hysteria2 process finished during controlled shutdown, code:" << exitCode
                             << "status:" << exitStatus;
                    return;
                }
                qWarning() << "Hysteria2 process finished, code:" << exitCode << "status:" << exitStatus;
                if (connectionState() == Vpn::ConnectionState::Connected
                    || connectionState() == Vpn::ConnectionState::Connecting) {
                    stop();
                    setLastError(ErrorCode::InternalError);
                }
            });

    m_hysteriaProcess->start();
    if (!m_hysteriaProcess->waitForStarted(3000)) {
        qCritical() << "Hysteria2Protocol: failed to start hysteria.exe:"
                    << m_hysteriaProcess->errorString();
        return ErrorCode::InternalError;
    }
    qDebug() << "Hysteria2Protocol: hysteria.exe started, pid=" << m_hysteriaProcess->processId()
             << "config=" << m_configPath;
    return ErrorCode::NoError;
}

void Hysteria2Protocol::stop()
{
    qDebug() << "Hysteria2Protocol::stop()";
    m_stopping = true;

    constexpr int kIpcTimeoutMs = 2000;

    IpcClient::withInterface([this](QSharedPointer<IpcInterfaceReplica> iface) {
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

        if (!m_xrayRouterConfig.isEmpty()) {
            auto xrayStop = iface->xrayStop();
            if (!xrayStop.waitForFinished(kIpcTimeoutMs) || !xrayStop.returnValue())
                qWarning() << "Failed to stop Hysteria2 XRay router";
        }
    });

    if (m_tun2socksProcess) {
        m_tun2socksProcess->blockSignals(true);
#ifndef Q_OS_WIN
        m_tun2socksProcess->terminate();
        auto wait = m_tun2socksProcess->waitForFinished(1000);
        if (!wait.waitForFinished() || !wait.returnValue()) {
            m_tun2socksProcess->kill();
        }
#else
        m_tun2socksProcess->kill();
#endif
        m_tun2socksProcess->close();
        m_tun2socksProcess.reset();
    }

    if (m_hysteriaProcess) {
        m_hysteriaProcess->blockSignals(true);
        if (m_hysteriaProcess->state() != QProcess::NotRunning) {
            m_hysteriaProcess->kill();
            m_hysteriaProcess->waitForFinished(1500);
        }
        m_hysteriaProcess->deleteLater();
        m_hysteriaProcess.clear();
    }

    if (!m_configPath.isEmpty()) {
        QFile::remove(m_configPath);
        m_configPath.clear();
    }

    setConnectionState(Vpn::ConnectionState::Disconnected);
}

ErrorCode Hysteria2Protocol::startTun2Socks()
{
    m_tun2socksProcess = IpcClient::CreatePrivilegedProcess();
    if (!m_tun2socksProcess || !m_tun2socksProcess->waitForSource()) {
        return ErrorCode::AmneziaServiceConnectionFailed;
    }

    QString proxyUrl;
    if (!m_xrayRouterConfig.isEmpty() && m_xrayRouterSocksPort > 0) {
        proxyUrl = QStringLiteral("socks5://%1:%2@127.0.0.1:%3")
                       .arg(m_xrayRouterUser, m_xrayRouterPassword)
                       .arg(m_xrayRouterSocksPort);
    } else {
        proxyUrl = QStringLiteral("socks5://127.0.0.1:%1").arg(m_socksPort);
    }

    m_tun2socksProcess->setProgram(PermittedProcess::Tun2Socks);
    m_tun2socksProcess->setArguments({
        "-device", QString("tun://%1").arg(tunName),
        "-proxy", proxyUrl,
        // See xrayprotocol.cpp for notes about -stack / -tcp-auto-tuning being
        // unsafe with the bundled tun2socks binary — same caveats apply here.
    });

    connect(m_tun2socksProcess.data(), &IpcProcessInterfaceReplica::readyReadStandardOutput, this, [this]() {
        auto readAll = m_tun2socksProcess->readAllStandardOutput();
        if (!readAll.waitForFinished()) {
            return;
        }
        const QString line = readAll.returnValue();
        if (!line.contains("[TCP]") && !line.contains("[UDP]"))
            qDebug() << "[tun2socks-h2]:" << line;

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

    connect(m_tun2socksProcess.data(), &IpcProcessInterfaceReplica::finished, this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                if (m_stopping || connectionState() == Vpn::ConnectionState::Disconnecting
                    || connectionState() == Vpn::ConnectionState::Disconnected) {
                    qDebug() << "Tun2socks (hysteria2) finished during controlled shutdown, code" << exitCode
                             << "status" << exitStatus;
                    return;
                }
                if (exitStatus == QProcess::ExitStatus::CrashExit) {
                    qCritical() << "Tun2socks (hysteria2) crashed";
                } else {
                    qCritical() << "Tun2socks (hysteria2) exited with code" << exitCode;
                }
                stop();
                setLastError(ErrorCode::Tun2SockExecutableCrashed);
            }, Qt::QueuedConnection);

    m_tun2socksProcess->start();
    return ErrorCode::NoError;
}

bool Hysteria2Protocol::ensureProxyReachable()
{
    return performSocks5Probe(m_masqueradeHost, 443, 4500, m_socksPort);
}

bool Hysteria2Protocol::ensureXrayRouterReachable()
{
    if (m_xrayRouterSocksPort <= 0) {
        return false;
    }
    return performSocks5Probe(m_masqueradeHost, 443, 4500, m_xrayRouterSocksPort,
                              m_xrayRouterUser, m_xrayRouterPassword);
}

bool Hysteria2Protocol::performSocks5Probe(const QString &targetHost, quint16 targetPort, int timeoutMs, int socksPort,
                                           const QString &user, const QString &password)
{
    const QByteArray hostBytes = targetHost.toUtf8();
    if (hostBytes.isEmpty() || hostBytes.size() > 255) {
        qWarning() << "Hysteria2 probe: invalid target host" << targetHost;
        return false;
    }

    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < timeoutMs) {
        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0)
            break;

        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, socksPort);
        if (!socket.waitForConnected(qMin(remaining, 700))) {
            QThread::msleep(150);
            continue;
        }

        socket.write((user.isEmpty() || password.isEmpty())
                     ? QByteArray::fromHex("050100")
                     : QByteArray::fromHex("05020002"));
        if (!socket.waitForBytesWritten(qMin(remaining, 500)) ||
            !waitForSocketBytes(socket, 2, qMin(remaining, 700))) {
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
            const QByteArray userBytes = user.toUtf8();
            const QByteArray passBytes = password.toUtf8();
            if (userBytes.isEmpty() || passBytes.isEmpty() || userBytes.size() > 255 || passBytes.size() > 255) {
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
            if (!socket.waitForBytesWritten(qMin(remaining, 500)) ||
                !waitForSocketBytes(socket, 2, qMin(remaining, 700))) {
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
        if (!socket.waitForBytesWritten(qMin(remaining, 500)) ||
            !waitForSocketBytes(socket, 5, qMin(remaining, 2000))) {
            socket.disconnectFromHost();
            QThread::msleep(200);
            continue;
        }

        const QByteArray connectReply = socket.peek(5);
        if (connectReply.size() >= 5 && quint8(connectReply.at(0)) == 0x05 && quint8(connectReply.at(1)) == 0x00) {
            socket.disconnectFromHost();
            qDebug() << "Hysteria2 SOCKS probe succeeded for" << targetHost << targetPort;
            return true;
        }
        socket.disconnectFromHost();
        QThread::msleep(200);
    }

    qWarning() << "Hysteria2 SOCKS probe failed for" << targetHost << targetPort;
    return false;
}

ErrorCode Hysteria2Protocol::setupRouting()
{
    return IpcClient::withInterface([this](QSharedPointer<IpcInterfaceReplica> iface) -> ErrorCode {
#ifdef Q_OS_WIN
        const int inetAdapterIndex = NetworkUtilities::AdapterIndexTo(QHostAddress(m_remoteAddress));
#endif
        auto createTun = iface->createTun(tunName, amnezia::protocols::hysteria2::defaultLocalAddr);
        if (!createTun.waitForFinished() || !createTun.returnValue()) {
            qCritical() << "Hysteria2: failed to assign IP for TUN";
            return ErrorCode::InternalError;
        }

        auto updateResolvers = iface->updateResolvers(tunName, m_dnsServers);
        if (!updateResolvers.waitForFinished() || !updateResolvers.returnValue()) {
            qCritical() << "Hysteria2: failed to set DNS resolvers";
            return ErrorCode::InternalError;
        }

#ifdef Q_OS_WIN
        int vpnAdapterIndex = -1;
        QList<QNetworkInterface> netInterfaces = QNetworkInterface::allInterfaces();
        for (auto &netInterface : netInterfaces) {
            for (auto &address : netInterface.addressEntries()) {
                if (m_vpnLocalAddress == address.ip().toString())
                    vpnAdapterIndex = netInterface.index();
            }
        }
#else
        static const int vpnAdapterIndex = 0;
#endif

        const bool killSwitchEnabled = QVariant(m_rawConfig.value(amnezia::config_key::killSwitchOption).toString()).toBool();
        const bool appSplitTunnelActive = isAppSplitTunnelActive(m_rawConfig);
        if (killSwitchEnabled && appSplitTunnelActive) {
            qDebug() << "Hysteria2: skipping strict killswitch firewall rules while app split tunneling is active";
        } else if (killSwitchEnabled) {
            if (vpnAdapterIndex != -1) {
                QJsonObject config = m_rawConfig;
                config.insert("vpnServer", m_remoteAddress);
                auto enableKillSwitch = IpcClient::Interface()->enableKillSwitch(config, vpnAdapterIndex);
                if (!enableKillSwitch.waitForFinished() || !enableKillSwitch.returnValue()) {
                    qCritical() << "Hysteria2: failed to enable killswitch";
                    return ErrorCode::InternalError;
                }
            } else {
                qWarning() << "Hysteria2: vpnAdapterIndex unknown, killswitch skipped";
            }
        }

        if (m_routeMode == Settings::RouteMode::VpnAllSites ||
            m_routeMode == Settings::RouteMode::VpnAllExceptSites) {
            // Exclude the Hysteria server's own IP from the TUN via the physical
            // gateway BEFORE the catch-all subnets. Those subnets (1.0.0.0/8 ...
            // 128.0.0.0/1) otherwise capture the server endpoint (e.g. 203.0.113.10
            // sits inside 32.0.0.0/3) into the tunnel, so the hysteria client's outer
            // UDP loops into the TUN and the handshake times out ("no recent network
            // activity") — VPN shows connected but nothing opens. A /32 via the
            // physical gateway is more specific, so server traffic bypasses the TUN.
            // Mirrors XrayProtocol::setupRouting. by vovankrot
            if (NetworkUtilities::checkIPv4Format(m_remoteAddress)
                && NetworkUtilities::checkIPv4Format(m_routeGateway)) {
                const QStringList serverExclusion = { m_remoteAddress + "/32" };
                auto excludeServer = iface->routeAddList(m_routeGateway, serverExclusion);
                if (!excludeServer.waitForFinished() || excludeServer.returnValue() != serverExclusion.count()) {
                    qWarning() << "Hysteria2 setupRouting: failed to add server exclusion route for"
                               << m_remoteAddress << "via" << m_routeGateway;
                } else {
                    qDebug() << "Hysteria2 setupRouting: excluded server" << m_remoteAddress
                             << "via" << m_routeGateway;
                }
            } else {
                qWarning() << "Hysteria2 setupRouting: cannot exclude server, invalid address/gateway"
                           << m_remoteAddress << m_routeGateway;
            }

            static const QStringList subnets = { "1.0.0.0/8", "2.0.0.0/7", "4.0.0.0/6", "8.0.0.0/5",
                                                 "16.0.0.0/4", "32.0.0.0/3", "64.0.0.0/2", "128.0.0.0/1" };
            auto routeAddList = iface->routeAddList(m_vpnGateway, subnets);
            if (!routeAddList.waitForFinished() || routeAddList.returnValue() != subnets.count()) {
                qCritical() << "Hysteria2: failed to set TUN routes";
                return ErrorCode::InternalError;
            }
        }

        auto StopRoutingIpv6 = iface->StopRoutingIpv6();
        if (!StopRoutingIpv6.waitForFinished() || !StopRoutingIpv6.returnValue()) {
            qCritical() << "Hysteria2: failed to disable IPv6 routing";
            return ErrorCode::InternalError;
        }

#ifdef Q_OS_WIN
        // enablePeerTraffic drives TWO things inside KillSwitch::enablePeerTraffic: the
        // strict firewall block (gated there on killSwitchOption) AND, unconditionally,
        // WindowsDaemon::activateSplitTunnel — the latter is what actually engages the WFP
        // per-app split-tunnel driver. So this call must fire whenever the kill-switch is
        // on OR per-app split tunnelling is active. Previously the call was skipped while
        // app-split was active, which silently disabled per-app split tunnelling on
        // Hysteria2 — the tun2socks/socks path never reaches the WireGuard daemon run().
        //
        // CRITICAL (regression fix): when app-split is active we MUST stamp
        // killSwitchOption=false. The strict killswitch (enableKillSwitch, which adds the
        // "Allow usage of VPN Adapter" escape) is skipped above for app-split, so letting
        // enablePeerTraffic install "Block Internet 0.0.0.0/0" here leaves the block with
        // NO tunnel-adapter escape — every non-bypassed app loses all internet. Stamping
        // false skips only the firewall block; activateSplitTunnel (the WFP driver) still
        // runs unconditionally inside enablePeerTraffic, so app-split works. app-split thus
        // takes precedence over the strict killswitch (which would need its own VPN-adapter
        // allow to coexist — a separate, larger change). by vovankrot
        if (killSwitchEnabled || appSplitTunnelActive) {
            if (inetAdapterIndex != -1 && vpnAdapterIndex != -1) {
                QJsonObject config = m_rawConfig;
                config.insert("inetAdapterIndex", inetAdapterIndex);
                config.insert("vpnAdapterIndex", vpnAdapterIndex);
                config.insert("vpnGateway", m_vpnGateway);
                config.insert("vpnServer", m_remoteAddress);
                config.insert(amnezia::config_key::killSwitchOption,
                              (killSwitchEnabled && !appSplitTunnelActive) ? "true" : "false");
                auto enablePeerTraffic = iface->enablePeerTraffic(config);
                if (!enablePeerTraffic.waitForFinished() || !enablePeerTraffic.returnValue()) {
                    qCritical() << "Hysteria2: failed to enable peer traffic / app split tunnel";
                    return ErrorCode::InternalError;
                }
            } else {
                qWarning() << "Hysteria2: split-tunnel adapter indices unknown, app-split/killswitch skipped"
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

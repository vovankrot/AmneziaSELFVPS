#include "anytlsprotocol.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QThread>
#include <QUrl>

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
} // namespace

AnyTlsProtocol::AnyTlsProtocol(const QJsonObject &configuration, QObject *parent)
    : VpnProtocol(configuration, parent)
{
    m_vpnGateway = amnezia::protocols::anytls::defaultLocalAddr;
    m_vpnLocalAddress = amnezia::protocols::anytls::defaultLocalAddr;
    m_routeGateway = NetworkUtilities::getGatewayAndIface().first;

    m_routeMode = static_cast<Settings::RouteMode>(configuration.value(amnezia::config_key::splitTunnelType).toInt());
    m_remoteAddress = NetworkUtilities::getIPAddress(m_rawConfig.value(amnezia::config_key::hostName).toString());

    const QString primaryDns = configuration.value(amnezia::config_key::dns1).toString();
    m_dnsServers.push_back(QHostAddress(primaryDns));
    if (primaryDns != amnezia::protocols::dns::amneziaDnsIp) {
        const QString secondaryDns = configuration.value(amnezia::config_key::dns2).toString();
        m_dnsServers.push_back(QHostAddress(secondaryDns));
    }

    const QJsonObject anytls = configuration.value(ProtocolProps::key_proto_config_data(Proto::AnyTls)).toObject();
    m_serverAddress = anytls.value(QStringLiteral("server")).toString();
    m_password = anytls.value(QStringLiteral("password")).toString();
    m_sni = anytls.value(QStringLiteral("sni")).toString();

    const int parsedPort = parseLocalPort(anytls.value(QStringLiteral("socks5_listen")).toString());
    if (parsedPort > 0) {
        m_socksPort = parsedPort;
    }

    m_xrayRouterConfig = configuration.value(amnezia::config_key::xrayRouterConfig).toObject();
}

AnyTlsProtocol::~AnyTlsProtocol()
{
    qDebug() << "AnyTlsProtocol::~AnyTlsProtocol()";
    AnyTlsProtocol::stop();
}

int AnyTlsProtocol::parseLocalPort(const QString &listen) const
{
    static const QRegularExpression re(QStringLiteral(":(\\d+)\\s*$"));
    const auto match = re.match(listen.trimmed());
    if (!match.hasMatch()) {
        return 0;
    }

    bool ok = false;
    const int port = match.captured(1).toInt(&ok);
    return ok && port > 0 && port < 65536 ? port : 0;
}

QString AnyTlsProtocol::buildServerUri() const
{
    const QString encodedPassword = QString::fromLatin1(QUrl::toPercentEncoding(m_password));
    QString uri = QStringLiteral("anytls://%1@%2/?insecure=1").arg(encodedPassword, m_serverAddress);
    if (!m_sni.trimmed().isEmpty()) {
        uri += QStringLiteral("&sni=%1").arg(QString::fromLatin1(QUrl::toPercentEncoding(m_sni.trimmed())));
    }
    return uri;
}

ErrorCode AnyTlsProtocol::start()
{
    qDebug() << "AnyTlsProtocol::start()";
    m_stopping = false;

    if (m_serverAddress.isEmpty() || m_password.isEmpty()) {
        qCritical() << "AnyTlsProtocol::start(): incomplete AnyTLS config";
        return ErrorCode::InternalError;
    }

    const QString anytlsExe = Utils::anytlsPath();
    if (!QFileInfo::exists(anytlsExe)) {
        qCritical() << "AnyTlsProtocol::start(): anytls-client executable not found at" << anytlsExe;
        return ErrorCode::InternalError;
    }

    if (ErrorCode code = startAnyTlsProcess(); code != ErrorCode::NoError) {
        return code;
    }

    if (!ensureProxyReachable()) {
        qWarning() << "Initial AnyTLS proxy probe failed. Retrying once.";
        if (m_anyTlsProcess) {
            m_anyTlsProcess->blockSignals(true);
            m_anyTlsProcess->kill();
            m_anyTlsProcess->waitForFinished(1500);
            m_anyTlsProcess->deleteLater();
            m_anyTlsProcess.clear();
        }
        if (ErrorCode code = startAnyTlsProcess(); code != ErrorCode::NoError) {
            return code;
        }
        if (!ensureProxyReachable()) {
            qCritical() << "AnyTLS proxy probe failed after reconnect attempt";
            return ErrorCode::InternalError;
        }
    }

    return IpcClient::withInterface([&](QSharedPointer<IpcInterfaceReplica> iface) -> ErrorCode {
        if (!m_xrayRouterConfig.isEmpty()) {
            if (ErrorCode code = startXrayRouter(iface); code != ErrorCode::NoError) {
                return code;
            }
            if (!ensureXrayRouterReachable()) {
                qWarning() << "Initial AnyTLS XRay router probe failed. Retrying XRay router once.";
                auto xrayStop = iface->xrayStop();
                if (!xrayStop.waitForFinished(2000) || !xrayStop.returnValue()) {
                    qWarning() << "Failed to stop AnyTLS XRay router before retry";
                }
                if (ErrorCode code = startXrayRouter(iface); code != ErrorCode::NoError) {
                    return code;
                }
                if (!ensureXrayRouterReachable()) {
                    qCritical() << "AnyTLS XRay router probe failed after reconnect attempt";
                    return ErrorCode::XrayExecutableCrashed;
                }
            }
        }
        return startTun2Socks();
    }, [] () {
        return ErrorCode::AmneziaServiceConnectionFailed;
    });
}

ErrorCode AnyTlsProtocol::startXrayRouter(const QSharedPointer<IpcInterfaceReplica> &iface)
{
    try {
        const auto creds = amnezia::serialization::inbounds::EnsureInboundAuth(m_xrayRouterConfig);
        m_xrayRouterUser = creds.username;
        m_xrayRouterPassword = creds.password;
        m_xrayRouterSocksPort = creds.port;
    } catch (const std::exception &e) {
        qCritical() << "Failed to prepare AnyTLS XRay router SOCKS inbound:" << e.what();
        return ErrorCode::InternalError;
    }

    auto xrayStart = iface->xrayStart(QJsonDocument(m_xrayRouterConfig).toJson());
    if (!xrayStart.waitForFinished() || !xrayStart.returnValue()) {
        qCritical() << "Failed to start AnyTLS XRay router";
        return ErrorCode::XrayExecutableCrashed;
    }

    qDebug() << "AnyTLS XRay router started on local SOCKS port" << m_xrayRouterSocksPort;
    return ErrorCode::NoError;
}

ErrorCode AnyTlsProtocol::startAnyTlsProcess()
{
    m_anyTlsProcess = new QProcess(this);
    m_anyTlsProcess->setProgram(Utils::anytlsPath());
    m_anyTlsProcess->setArguments({
        QStringLiteral("-l"), QStringLiteral("127.0.0.1:%1").arg(m_socksPort),
        QStringLiteral("-s"), buildServerUri(),
    });
    m_anyTlsProcess->setProcessChannelMode(QProcess::MergedChannels);

    connect(m_anyTlsProcess.data(), &QProcess::readyReadStandardOutput, this, [this]() {
        if (!m_anyTlsProcess)
            return;
        const QByteArray out = m_anyTlsProcess->readAllStandardOutput();
        for (const QByteArray &lineRaw : out.split('\n')) {
            const QString line = QString::fromUtf8(lineRaw).trimmed();
            if (!line.isEmpty()) {
                qDebug().noquote() << "[anytls]" << line;
            }
        }
    });

    connect(m_anyTlsProcess.data(),
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                if (m_stopping || connectionState() == Vpn::ConnectionState::Disconnecting
                    || connectionState() == Vpn::ConnectionState::Disconnected) {
                    qDebug() << "AnyTLS process finished during controlled shutdown, code:" << exitCode
                             << "status:" << exitStatus;
                    return;
                }
                qWarning() << "AnyTLS process finished, code:" << exitCode << "status:" << exitStatus;
                if (connectionState() == Vpn::ConnectionState::Connected
                    || connectionState() == Vpn::ConnectionState::Connecting) {
                    stop();
                    setLastError(ErrorCode::InternalError);
                }
            });

    m_anyTlsProcess->start();
    if (!m_anyTlsProcess->waitForStarted(3000)) {
        qCritical() << "AnyTlsProtocol: failed to start anytls-client:" << m_anyTlsProcess->errorString();
        return ErrorCode::InternalError;
    }

    qDebug() << "AnyTlsProtocol: anytls-client started, pid=" << m_anyTlsProcess->processId()
             << "local socks=" << m_socksPort;
    return ErrorCode::NoError;
}

void AnyTlsProtocol::stop()
{
    qDebug() << "AnyTlsProtocol::stop()";
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
                qWarning() << "Failed to stop AnyTLS XRay router";
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

    if (m_anyTlsProcess) {
        m_anyTlsProcess->blockSignals(true);
        if (m_anyTlsProcess->state() != QProcess::NotRunning) {
            m_anyTlsProcess->kill();
            m_anyTlsProcess->waitForFinished(1500);
        }
        m_anyTlsProcess->deleteLater();
        m_anyTlsProcess.clear();
    }

    setConnectionState(Vpn::ConnectionState::Disconnected);
}

ErrorCode AnyTlsProtocol::startTun2Socks()
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
    });

    connect(m_tun2socksProcess.data(), &IpcProcessInterfaceReplica::readyReadStandardOutput, this, [this]() {
        auto readAll = m_tun2socksProcess->readAllStandardOutput();
        if (!readAll.waitForFinished()) {
            return;
        }
        const QString line = readAll.returnValue();
        if (!line.contains("[TCP]") && !line.contains("[UDP]"))
            qDebug() << "[tun2socks-anytls]:" << line;

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
                    qDebug() << "Tun2socks (AnyTLS) finished during controlled shutdown, code" << exitCode
                             << "status" << exitStatus;
                    return;
                }
                if (exitStatus == QProcess::ExitStatus::CrashExit) {
                    qCritical() << "Tun2socks (AnyTLS) crashed";
                } else {
                    qCritical() << "Tun2socks (AnyTLS) exited with code" << exitCode;
                }
                stop();
                setLastError(ErrorCode::Tun2SockExecutableCrashed);
            }, Qt::QueuedConnection);

    m_tun2socksProcess->start();
    return ErrorCode::NoError;
}

bool AnyTlsProtocol::ensureProxyReachable()
{
    const QString host = m_sni.trimmed().isEmpty()
        ? QString::fromLatin1(amnezia::protocols::anytls::defaultSni)
        : m_sni.trimmed();
    return performSocks5Probe(host, 443, 4500, m_socksPort);
}

bool AnyTlsProtocol::ensureXrayRouterReachable()
{
    if (m_xrayRouterSocksPort <= 0) {
        return false;
    }
    const QString host = m_sni.trimmed().isEmpty()
        ? QString::fromLatin1(amnezia::protocols::anytls::defaultSni)
        : m_sni.trimmed();
    return performSocks5Probe(host, 443, 4500, m_xrayRouterSocksPort,
                              m_xrayRouterUser, m_xrayRouterPassword);
}

bool AnyTlsProtocol::performSocks5Probe(const QString &targetHost, quint16 targetPort, int timeoutMs, int socksPort,
                                        const QString &user, const QString &password)
{
    const QByteArray hostBytes = targetHost.toUtf8();
    if (hostBytes.isEmpty() || hostBytes.size() > 255) {
        qWarning() << "AnyTLS probe: invalid target host" << targetHost;
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
            qDebug() << "AnyTLS SOCKS probe succeeded for" << targetHost << targetPort;
            return true;
        }
        socket.disconnectFromHost();
        QThread::msleep(200);
    }

    qWarning() << "AnyTLS SOCKS probe failed for" << targetHost << targetPort;
    return false;
}

ErrorCode AnyTlsProtocol::setupRouting()
{
    return IpcClient::withInterface([this](QSharedPointer<IpcInterfaceReplica> iface) -> ErrorCode {
#ifdef Q_OS_WIN
        const int inetAdapterIndex = NetworkUtilities::AdapterIndexTo(QHostAddress(m_remoteAddress));
#endif
        auto createTun = iface->createTun(tunName, amnezia::protocols::anytls::defaultLocalAddr);
        if (!createTun.waitForFinished() || !createTun.returnValue()) {
            qCritical() << "AnyTLS: failed to assign IP for TUN";
            return ErrorCode::InternalError;
        }

        auto updateResolvers = iface->updateResolvers(tunName, m_dnsServers);
        if (!updateResolvers.waitForFinished() || !updateResolvers.returnValue()) {
            qCritical() << "AnyTLS: failed to set DNS resolvers";
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
            qDebug() << "AnyTLS: skipping strict killswitch firewall rules while app split tunneling is active";
        } else if (killSwitchEnabled) {
            if (vpnAdapterIndex != -1) {
                QJsonObject config = m_rawConfig;
                config.insert("vpnServer", m_remoteAddress);
                auto enableKillSwitch = IpcClient::Interface()->enableKillSwitch(config, vpnAdapterIndex);
                if (!enableKillSwitch.waitForFinished() || !enableKillSwitch.returnValue()) {
                    qCritical() << "AnyTLS: failed to enable killswitch";
                    return ErrorCode::InternalError;
                }
            } else {
                qWarning() << "AnyTLS: vpnAdapterIndex unknown, killswitch skipped";
            }
        }

        if (m_routeMode == Settings::RouteMode::VpnAllSites ||
            m_routeMode == Settings::RouteMode::VpnAllExceptSites) {
            static const QStringList subnets = { "1.0.0.0/8", "2.0.0.0/7", "4.0.0.0/6", "8.0.0.0/5",
                                                 "16.0.0.0/4", "32.0.0.0/3", "64.0.0.0/2", "128.0.0.0/1" };
            auto routeAddList = iface->routeAddList(m_vpnGateway, subnets);
            if (!routeAddList.waitForFinished() || routeAddList.returnValue() != subnets.count()) {
                qCritical() << "AnyTLS: failed to set TUN routes";
                return ErrorCode::InternalError;
            }
        }

        auto StopRoutingIpv6 = iface->StopRoutingIpv6();
        if (!StopRoutingIpv6.waitForFinished() || !StopRoutingIpv6.returnValue()) {
            qCritical() << "AnyTLS: failed to disable IPv6 routing";
            return ErrorCode::InternalError;
        }

#ifdef Q_OS_WIN
        if (killSwitchEnabled && appSplitTunnelActive) {
            qDebug() << "AnyTLS: skipping peer traffic killswitch block while app split tunneling is active";
        } else if (killSwitchEnabled && inetAdapterIndex != -1 && vpnAdapterIndex != -1) {
            QJsonObject config = m_rawConfig;
            config.insert("inetAdapterIndex", inetAdapterIndex);
            config.insert("vpnAdapterIndex", vpnAdapterIndex);
            config.insert("vpnGateway", m_vpnGateway);
            config.insert("vpnServer", m_remoteAddress);
            auto enablePeerTraffic = iface->enablePeerTraffic(config);
            if (!enablePeerTraffic.waitForFinished() || !enablePeerTraffic.returnValue()) {
                qCritical() << "AnyTLS: failed to enable peer traffic";
                return ErrorCode::InternalError;
            }
        } else if (killSwitchEnabled) {
            qWarning() << "AnyTLS: split-tunnel adapter indices unknown, skipped";
        }
#endif
        return ErrorCode::NoError;
    },
    [] () {
        return ErrorCode::AmneziaServiceConnectionFailed;
    });
}
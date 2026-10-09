#include "anytlsprotocol.h"
#include "core/tun2socksOutputReader.h"
#include "core/tun2socksProcessObserver.h"
#include "core/socksRoutingSetup.h"
#include "core/localSocksUrl.h"
#include "core/asyncProcessRequest.h"
#include "core/asyncTunnelStop.h"
#include "core/asyncSocksProbe.h"

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
#include <QProcessEnvironment>
#include <QUrl>

#include "core/networkUtilities.h"
#include "core/socksProbe.h"
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
        if (!secondaryDns.isEmpty() && secondaryDns != primaryDns) m_dnsServers.push_back(QHostAddress(secondaryDns));
    }

    const QJsonObject anytls = configuration.value(ProtocolProps::key_proto_config_data(Proto::AnyTls)).toObject();
    m_serverAddress = anytls.value(QStringLiteral("server")).toString();
    m_password = anytls.value(QStringLiteral("password")).toString();
    m_sni = anytls.value(QStringLiteral("sni")).toString();
    m_certificatePin = anytls.value(QStringLiteral("certificate_sha256")).toString();

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
    QString uri = QStringLiteral("anytls://%1@%2/").arg(encodedPassword, m_serverAddress);
    if (!m_sni.trimmed().isEmpty()) {
        uri += QStringLiteral("&sni=%1").arg(QString::fromLatin1(QUrl::toPercentEncoding(m_sni.trimmed())));
    }
    return uri;
}

ErrorCode AnyTlsProtocol::start()
{
    if (cleanupInProgress() || cleanupFailed()) return ErrorCode::AmneziaServiceConnectionFailed;
    if (!m_stopping && (connectionState() == Vpn::Connecting || connectionState() == Vpn::Connected)) {
        qWarning() << "Ignoring duplicate protocol start";
        return ErrorCode::NoError;
    }
    qDebug() << "AnyTlsProtocol::start()";
    m_stopping = false;
    m_completedStopSteps.clear();

    if (m_serverAddress.isEmpty() || m_password.isEmpty()) {
        qCritical() << "AnyTlsProtocol::start(): incomplete AnyTLS config";
        return ErrorCode::InternalError;
    }

    if (!QRegularExpression(QStringLiteral("^[0-9a-fA-F]{64}$")).match(m_certificatePin).hasMatch())
        return ErrorCode::TlsCertificateTrustMissing;

    const QString anytlsExe = Utils::anytlsPath();
    if (!QFileInfo::exists(anytlsExe)) {
        qCritical() << "AnyTlsProtocol::start(): anytls-client executable not found at" << anytlsExe;
        return ErrorCode::InternalError;
    }

    ++m_startGeneration;
    setConnectionState(Vpn::ConnectionState::Connecting);
    startTimeoutTimer();
    return startAnyTlsProcess();
}

void AnyTlsProtocol::afterHelperStarted()
{
    if (m_stopping) return;
    const auto generation = m_startGeneration;
    const QString host = (m_sni.trimmed().isEmpty() ? QString::fromLatin1(amnezia::protocols::anytls::defaultSni) : m_sni.trimmed());
    m_startupProbe = new AsyncSocksProbe(this, host, 443, 4500, quint16(m_socksPort), {}, {},
        [this, generation](bool ok) {
            if (generation != m_startGeneration || m_stopping) return;
            m_startupProbe = nullptr;
            if (!ok) {
                stop();
                setLastError(ErrorCode::InternalError);
                return;
            }
            if (m_xrayRouterConfig.isEmpty()) startTun2Socks();
            else startXrayRouter();
        });
}

void AnyTlsProtocol::startXrayRouter()
{
    try {
        const auto creds = amnezia::serialization::inbounds::EnsureInboundAuth(m_xrayRouterConfig);
        m_xrayRouterUser = creds.username;
        m_xrayRouterPassword = creds.password;
        m_xrayRouterSocksPort = creds.port;
    } catch (const std::exception &) {
        stop();
        setLastError(ErrorCode::InternalError);
        return;
    }
    const auto iface = IpcClient::InterfaceWithoutWait();
    const auto generation = m_startGeneration;
    m_startupReady = new AsyncReplicaReady(this, iface.data(), 10000,
        [this, iface, generation](bool ready) {
            if (generation != m_startGeneration || m_stopping) return;
            m_startupReady = nullptr;
            if (!ready) { stop(); setLastError(ErrorCode::AmneziaServiceConnectionFailed); return; }
            const auto config = QJsonDocument(m_xrayRouterConfig).toJson();
            m_startupSequence = new AsyncIpcSequence(this,
                {{"xrayStart", [iface, config] { return iface->xrayStart(config); }}},
                [this, generation](AsyncIpcSequence::Result result, const QString &) {
                    if (generation != m_startGeneration || m_stopping) return;
                    m_startupSequence = nullptr;
                    if (result != AsyncIpcSequence::Result::Success) {
                        stop(); setLastError(ErrorCode::XrayExecutableCrashed); return;
                    }
                    const QString host = (m_sni.trimmed().isEmpty() ? QString::fromLatin1(amnezia::protocols::anytls::defaultSni) : m_sni.trimmed());
                    m_startupProbe = new AsyncSocksProbe(this, host, 443, 4500,
                        quint16(m_xrayRouterSocksPort), m_xrayRouterUser, m_xrayRouterPassword,
                        [this, generation](bool ok) {
                            if (generation != m_startGeneration || m_stopping) return;
                            m_startupProbe = nullptr;
                            if (!ok) { stop(); setLastError(ErrorCode::XrayExecutableCrashed); return; }
                            startTun2Socks();
                        });
                });
        });
}

ErrorCode AnyTlsProtocol::startAnyTlsProcess()
{
    m_anyTlsProcess = new QProcess(this);
    m_anyTlsProcess->setProgram(Utils::anytlsPath());
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove("TLS_KEY_LOG");
    environment.insert("ANYTLS_PASSWORD", m_password);
    m_anyTlsProcess->setProcessEnvironment(environment);
    m_anyTlsProcess->setArguments({
        QStringLiteral("-l"), QStringLiteral("127.0.0.1:%1").arg(m_socksPort),
        QStringLiteral("-s"), m_serverAddress,
        QStringLiteral("-sni"), m_sni,
        QStringLiteral("-pin"), m_certificatePin,
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

    auto *process = m_anyTlsProcess.data();
    const auto generation = m_startGeneration;
    connect(process, &QProcess::started, this, [this, process, generation] {
        if (m_stopping || generation != m_startGeneration || m_anyTlsProcess.data() != process) return;
        afterHelperStarted();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, generation](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || m_stopping
            || generation != m_startGeneration || m_anyTlsProcess.data() != process) return;
        stop();
        setLastError(ErrorCode::InternalError);
    });
    QTimer::singleShot(3000, this, [this, generation] {
        if (m_stopping || generation != m_startGeneration) return;
        if (!m_anyTlsProcess || m_anyTlsProcess->state() != QProcess::Running) {
            stop();
            setLastError(ErrorCode::InternalError);
        }
    });
    process->start();
    return ErrorCode::NoError;
}

void AnyTlsProtocol::stop()
{
    if (m_stopping && !cleanupFailed()) return;
    qDebug() << "AnyTlsProtocol::stop()";
    m_stopping = true;
    ++m_startGeneration;
    stopTimeoutTimer();
    if (m_startupProbe) { m_startupProbe->cancel(); m_startupProbe = nullptr; }
    if (m_startupReady) { m_startupReady->cancel(); m_startupReady = nullptr; }
    if (m_processRequest) { m_processRequest->cancel(); m_processRequest = nullptr; }
    if (m_startupSequence) { m_startupSequence->cancel(); m_startupSequence = nullptr; }
    if (m_routingSetup) { m_routingSetup->cancel(); m_routingSetup = nullptr; }

    beginAsyncStop();
    new AsyncTunnelStop(this, m_tun2socksProcess, m_anyTlsProcess.data(),
        IpcClient::InterfaceWithoutWait(), tunName, !m_xrayRouterConfig.isEmpty(), m_completedStopSteps,
        [this](bool success, const QString &step) {
            if (success) {
                m_tun2socksProcess.reset();
                if (m_anyTlsProcess) { m_anyTlsProcess->deleteLater(); m_anyTlsProcess.clear(); }
            } else {
                qCritical() << "Tunnel cleanup failed at" << step;
                emit networkPolicyWarning(tr("Не удалось полностью очистить VPN-сессию. Новое подключение заблокировано до успешной очистки."));
            }
            finishAsyncStop(success);
        });
}

ErrorCode AnyTlsProtocol::startTun2Socks()
{
    const auto generation = m_startGeneration;
    m_processRequest = new AsyncProcessRequest(this, IpcClient::InterfaceWithoutWait(),
        [](int id) { return QUrl(QString("local:%1").arg(amnezia::getIpcProcessUrl(id))); },
        [this, generation](AsyncProcessRequest::Replica process) {
            if (m_stopping || generation != m_startGeneration) { if (process) process->close(); return; }
            m_processRequest = nullptr;
            if (!process) { stop(); setLastError(ErrorCode::AmneziaServiceConnectionFailed); return; }
            m_tun2socksProcess = std::move(process);
            configureTun2Socks();
        });
    return ErrorCode::NoError;
}

void AnyTlsProtocol::configureTun2Socks()
{
    const bool router = !m_xrayRouterConfig.isEmpty() && m_xrayRouterSocksPort > 0;
    const QString proxyUrl = router
        ? LocalSocksUrl::make(quint16(m_xrayRouterSocksPort), m_xrayRouterUser, m_xrayRouterPassword)
        : LocalSocksUrl::make(quint16(m_socksPort));

    m_tun2socksProcess->setProgram(PermittedProcess::Tun2Socks);
    // v2.7.0 logs to stderr, and the Connected transition depends on seeing the
    // "[STACK] ..." line on stdout -- merge them. See xrayprotocol.cpp for details.
    m_tun2socksProcess->setProcessChannelMode(QProcess::MergedChannels);
    m_tun2socksProcess->setArguments({
        "--device", QString("tun://%1").arg(tunName),
        "--proxy", proxyUrl,
        // tun2socks upgraded to v2.7.0 (2026-08-05) needs double-dash long flags
        // (cobra/pflag parser) -- see xrayprotocol.cpp::startTun2Socks() for the
        // full note. Same binary, same caveats apply here.
    });

    auto *outputProcess = m_tun2socksProcess.data();
    new Tun2SocksOutputReader(outputProcess, this,
        [this, outputProcess] { return !m_stopping && m_tun2socksProcess.data() == outputProcess; },
        [this] { setupRouting(); });

    new Tun2SocksProcessObserver(outputProcess, this,
        [this, outputProcess] {
            return !m_stopping && m_tun2socksProcess.data() == outputProcess
                && connectionState() != Vpn::ConnectionState::Disconnecting
                && connectionState() != Vpn::ConnectionState::Disconnected;
        },
        [this] { stop(); setLastError(ErrorCode::Tun2SockExecutableCrashed); });

    m_tun2socksProcess->start();
}

void AnyTlsProtocol::setupRouting()
{
    if (m_stopping) return;
    if (m_routingSetup) { m_routingSetup->cancel(); m_routingSetup = nullptr; }
    const auto iface = IpcClient::Interface();
    if (!iface || !iface->isReplicaValid()) {
        stop();
        setLastError(ErrorCode::AmneziaServiceConnectionFailed);
        return;
    }
    SocksRoutingSetup::Parameters p;
    p.device = tunName;
    p.localAddress = amnezia::protocols::anytls::defaultLocalAddr;
    p.vpnAddress = m_vpnLocalAddress;
    p.vpnGateway = NetworkUtilities::checkIPv4Format(m_vpnGateway) ? m_vpnGateway : p.localAddress;
    p.serverAddress = m_remoteAddress;
    p.externalGateway = m_routeGateway;
    p.dns = m_dnsServers;
    p.peerConfig = m_rawConfig;
    p.allSites = m_routeMode == Settings::RouteMode::VpnAllSites
              || m_routeMode == Settings::RouteMode::VpnAllExceptSites;
    p.killSwitch = QVariant(m_rawConfig.value(amnezia::config_key::killSwitchOption).toString()).toBool();
    p.appSplit = isAppSplitTunnelActive(m_rawConfig);
    // Per-app routing supplies its own filtering; a global strict block would
    // also interrupt applications explicitly excluded from this VPN.
    p.peerConfig.insert(amnezia::config_key::killSwitchOption,
                        (p.killSwitch && !p.appSplit) ? "true" : "false");
    m_routingSetup = new AsyncIpcSequence(this, SocksRoutingSetup::steps(iface, p),
        [this](AsyncIpcSequence::Result result, const QString &step) {
            m_routingSetup = nullptr;
            if (m_stopping) return;
            if (result != AsyncIpcSequence::Result::Success) {
                qCritical() << "AnyTlsProtocol: routing setup failed at" << step << "result" << int(result);
                stop();
                setLastError(ErrorCode::InternalError);
                return;
            }
            stopTimeoutTimer();
            setConnectionState(Vpn::ConnectionState::Connected);
        });
}

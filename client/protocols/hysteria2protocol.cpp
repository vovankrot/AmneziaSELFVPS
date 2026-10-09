#include "hysteria2protocol.h"
#include "core/tun2socksOutputReader.h"
#include "core/tun2socksProcessObserver.h"
#include "core/socksRoutingSetup.h"
#include "core/localSocksUrl.h"
#include "core/asyncProcessRequest.h"
#include "core/asyncTunnelStop.h"
#include "core/asyncSocksProbe.h"

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
#include <QTemporaryFile>

#include "core/ipcclient.h"
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
        if (!secondaryDns.isEmpty() && secondaryDns != primaryDns) m_dnsServers.push_back(QHostAddress(secondaryDns));
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

    QTemporaryFile f(QDir(tmpDir).absoluteFilePath("hysteria2_XXXXXX.yaml"));
    if (!f.open()) {
        qWarning() << "Hysteria2Protocol: failed to create private configuration file";
        return {};
    }
    const QByteArray bytes = m_yamlConfig.toUtf8();
    if (!f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || f.write(bytes) != bytes.size() || !f.flush()) return {};
    const QString path = f.fileName();
    f.setAutoRemove(false); // Removed by stop() after the helper exits.
    f.close();
    return path;
}

ErrorCode Hysteria2Protocol::start()
{
    if (cleanupInProgress() || cleanupFailed()) return ErrorCode::AmneziaServiceConnectionFailed;
    if (!m_stopping && (connectionState() == Vpn::Connecting || connectionState() == Vpn::Connected)) {
        qWarning() << "Ignoring duplicate protocol start";
        return ErrorCode::NoError;
    }
    qDebug() << "Hysteria2Protocol::start()";
    m_stopping = false;
    m_completedStopSteps.clear();

    if (m_yamlConfig.isEmpty()) {
        qCritical() << "Hysteria2Protocol::start(): empty yaml_config in configuration";
        return ErrorCode::InternalError;
    }

    const QString hysteriaExe = Utils::hysteriaPath();
    static const QRegularExpression pinPattern(QStringLiteral("(?m)^[ \\t]+pinSHA256:[ \\t]*[0-9a-fA-F:]{64,95}[ \\t]*$"));
    if (!pinPattern.match(m_yamlConfig).hasMatch()) return ErrorCode::TlsCertificateTrustMissing;
    if (!QFileInfo::exists(hysteriaExe)) {
        qCritical() << "Hysteria2Protocol::start(): hysteria executable not found at" << hysteriaExe;
        return ErrorCode::InternalError;
    }

    ++m_startGeneration;
    setConnectionState(Vpn::ConnectionState::Connecting);
    startTimeoutTimer();
    return startHysteriaProcess();
}

void Hysteria2Protocol::afterHelperStarted()
{
    if (m_stopping) return;
    const auto generation = m_startGeneration;
    const QString host = m_masqueradeHost;
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

void Hysteria2Protocol::startXrayRouter()
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
                    const QString host = m_masqueradeHost;
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

    auto *process = m_hysteriaProcess.data();
    const auto generation = m_startGeneration;
    connect(process, &QProcess::started, this, [this, process, generation] {
        if (m_stopping || generation != m_startGeneration || m_hysteriaProcess.data() != process) return;
        afterHelperStarted();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, generation](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || m_stopping
            || generation != m_startGeneration || m_hysteriaProcess.data() != process) return;
        stop();
        setLastError(ErrorCode::InternalError);
    });
    QTimer::singleShot(3000, this, [this, generation] {
        if (m_stopping || generation != m_startGeneration) return;
        if (!m_hysteriaProcess || m_hysteriaProcess->state() != QProcess::Running) {
            stop();
            setLastError(ErrorCode::InternalError);
        }
    });
    process->start();
    return ErrorCode::NoError;
}

void Hysteria2Protocol::stop()
{
    if (m_stopping && !cleanupFailed()) return;
    qDebug() << "Hysteria2Protocol::stop()";
    m_stopping = true;
    ++m_startGeneration;
    stopTimeoutTimer();
    if (m_startupProbe) { m_startupProbe->cancel(); m_startupProbe = nullptr; }
    if (m_startupReady) { m_startupReady->cancel(); m_startupReady = nullptr; }
    if (m_processRequest) { m_processRequest->cancel(); m_processRequest = nullptr; }
    if (m_startupSequence) { m_startupSequence->cancel(); m_startupSequence = nullptr; }
    if (m_routingSetup) { m_routingSetup->cancel(); m_routingSetup = nullptr; }

    beginAsyncStop();
    new AsyncTunnelStop(this, m_tun2socksProcess, m_hysteriaProcess.data(),
        IpcClient::InterfaceWithoutWait(), tunName, !m_xrayRouterConfig.isEmpty(), m_completedStopSteps,
        [this](bool success, const QString &step) {
            if (success) {
                m_tun2socksProcess.reset();
                if (m_hysteriaProcess) { m_hysteriaProcess->deleteLater(); m_hysteriaProcess.clear(); }
                if (!m_configPath.isEmpty()) { QFile::remove(m_configPath); m_configPath.clear(); }
            } else {
                qCritical() << "Tunnel cleanup failed at" << step;
                emit networkPolicyWarning(tr("Не удалось полностью очистить VPN-сессию. Новое подключение заблокировано до успешной очистки."));
            }
            finishAsyncStop(success);
        });
}

ErrorCode Hysteria2Protocol::startTun2Socks()
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

void Hysteria2Protocol::configureTun2Socks()
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

void Hysteria2Protocol::setupRouting()
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
    p.localAddress = amnezia::protocols::hysteria2::defaultLocalAddr;
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
                qCritical() << "Hysteria2Protocol: routing setup failed at" << step << "result" << int(result);
                stop();
                setLastError(ErrorCode::InternalError);
                return;
            }
            stopTimeoutTimer();
            setConnectionState(Vpn::ConnectionState::Connected);
        });
}

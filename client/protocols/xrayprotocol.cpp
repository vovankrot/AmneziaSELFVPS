#include "xrayprotocol.h"
#include "core/tun2socksOutputReader.h"
#include "core/tun2socksProcessObserver.h"
#include "core/socksRoutingSetup.h"
#include "core/localSocksUrl.h"
#include "core/asyncProcessRequest.h"
#include "core/asyncTunnelStop.h"

#include "core/ipcclient.h"
#include "core/serialization/serialization.h"
#include "ipc.h"
#include "utilities.h"
#include "core/networkUtilities.h"

#include <exception>

#include <QCryptographicHash>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QJsonDocument>
#include <QSslSocket>
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
        if (!secondaryDns.isEmpty() && secondaryDns != primaryDns) m_dnsServers.push_back(QHostAddress(secondaryDns));
    }

    QJsonObject xrayConfiguration = configuration.value(ProtocolProps::key_proto_config_data(Proto::Xray)).toObject();
    if (xrayConfiguration.isEmpty()) {
        xrayConfiguration = configuration.value(ProtocolProps::key_proto_config_data(Proto::SSXray)).toObject();
    }
    m_xrayConfig = xrayConfiguration;
    qDebug() << "XrayProtocol::XrayProtocol() this=" << static_cast<void *>(this)
              << "thread=" << QThread::currentThread();
}

XrayProtocol::~XrayProtocol()
{
    qDebug() << "XrayProtocol::~XrayProtocol() this=" << static_cast<void *>(this)
              << "thread=" << QThread::currentThread();
    XrayProtocol::stop();
}

namespace {
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
    if (cleanupInProgress() || cleanupFailed()) return ErrorCode::AmneziaServiceConnectionFailed;
    if (!m_stopping && (connectionState() == Vpn::Connecting || connectionState() == Vpn::Connected)) {
        qWarning() << "Ignoring duplicate protocol start";
        return ErrorCode::NoError;
    }
    qDebug() << "XrayProtocol::start()";
    m_stopping = false;
    m_completedStopSteps.clear();

    try {
        const auto creds = amnezia::serialization::inbounds::EnsureInboundAuth(m_xrayConfig);
        m_socksUser = creds.username;
        m_socksPassword = creds.password;
        m_socksPort = creds.port;
    } catch (const std::exception &e) {
        qCritical() << "Failed to prepare XRay SOCKS inbound:" << e.what();
        return ErrorCode::InternalError;
    }

    // Preserve imported DNS/routing; disable the incompatible outbound mux.
    ensureDnsOverDoh(m_xrayConfig);

    setConnectionState(Vpn::ConnectionState::Connecting);
    startTimeoutTimer();
    return startTun2Socks();
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

void XrayProtocol::stop()
{
    // Guard against reentrant / duplicate stop() calls. VpnConnection calls stop()
    // explicitly on protocol change; then when the QSharedPointer<XrayProtocol> gets
    // reset, ~XrayProtocol() runs stop() a second time. That second pass then hits
    // xrayStop over an IPC that was already torn down and logs "failed to close all
    // features > use of closed network connection" -- benign in isolation but the
    // real cost is that on rapid protocol/server flapping we end up firing SplitTunnel
    // stop, firewall clear and routing teardown twice back-to-back, which is what left
    // the daemon in a wedged state on 2026-07-26 (16:45-16:53 window). Single-stop
    // semantics; start() resets m_stopping. by vovankrot
    if (m_stopping && !cleanupFailed()) return;

    qDebug() << "XrayProtocol::stop() this=" << static_cast<void *>(this)
              << "m_healthTimer=" << static_cast<void *>(m_healthTimer)
              << "thread=" << QThread::currentThread();
    m_stopping = true;
    stopTimeoutTimer();
    if (m_processRequest) { m_processRequest->cancel(); m_processRequest = nullptr; }
    if (m_startupSequence) { m_startupSequence->cancel(); m_startupSequence = nullptr; }
    if (m_routingSetup) { m_routingSetup->cancel(); m_routingSetup = nullptr; }
    cancelHealthCheck();

    beginAsyncStop();
    new AsyncTunnelStop(this, m_tun2socksProcess, nullptr,
        IpcClient::InterfaceWithoutWait(), tunName, true, m_completedStopSteps,
        [this](bool success, const QString &step) {
            if (success) {
                m_tun2socksProcess.reset();

            } else {
                qCritical() << "Tunnel cleanup failed at" << step;
                emit networkPolicyWarning(tr("Не удалось полностью очистить VPN-сессию. Новое подключение заблокировано до успешной очистки."));
            }
            finishAsyncStop(success);
        });
}

ErrorCode XrayProtocol::startTun2Socks()
{
    const auto iface = IpcClient::InterfaceWithoutWait();
    m_processRequest = new AsyncProcessRequest(this, iface,
        [](int id) { return QUrl(QString("local:%1").arg(amnezia::getIpcProcessUrl(id))); },
        [this, iface](AsyncProcessRequest::Replica process) {
            m_processRequest = nullptr;
            if (m_stopping) { if (process) process->close(); return; }
            if (!process) {
                stop();
                setLastError(ErrorCode::AmneziaServiceConnectionFailed);
                return;
            }
            m_tun2socksProcess = std::move(process);
            const auto config = QJsonDocument(m_xrayConfig).toJson();
            m_startupSequence = new AsyncIpcSequence(this,
                {{"xrayStart", [iface, config] { return iface->xrayStart(config); }}},
                [this](AsyncIpcSequence::Result result, const QString &) {
                    m_startupSequence = nullptr;
                    if (m_stopping) return;
                    if (result != AsyncIpcSequence::Result::Success) {
                        stop();
                        setLastError(ErrorCode::XrayExecutableCrashed);
                        return;
                    }
                    configureTun2Socks();
                });
        });
    return ErrorCode::NoError;
}

void XrayProtocol::configureTun2Socks()
{

    // SOCKS port + creds were prepared in start() via EnsureInboundAuth.
    const QString proxyUrl = LocalSocksUrl::make(quint16(m_socksPort), m_socksUser, m_socksPassword);

    m_tun2socksProcess->setProgram(PermittedProcess::Tun2Socks);
    // CRITICAL: tun2socks v2.7.0 logs to STDERR (zap/JSON), whereas the old c8f8cb5
    // build logged to STDOUT (logrus text). We only wire up readyReadStandardOutput
    // below, and the transition to Connected is driven ENTIRELY by spotting the
    // "[STACK] tun://... <-> socks5://..." line in that stream. Without merging the
    // channels the line never arrives, setupRouting() never runs, and the client sits
    // in "Connecting..." forever even though the tunnel is already carrying traffic --
    // exactly the hang seen 2026-08-05 07:24:52-07:27:10. by vovankrot
    m_tun2socksProcess->setProcessChannelMode(QProcess::MergedChannels);
    m_tun2socksProcess->setArguments({
        "--device", QString("tun://%1").arg(tunName),
        "--proxy", proxyUrl,
        // Bundled tun2socks upgraded 2026-08-05 from xjasonlyu c8f8cb5 (pre-2026-04)
        // to release v2.7.0 (8dda19e) -- the old build's gVisor netstack crashed
        // (QProcess::Crashed) ~5-8s after the first real TCP flow REGARDLESS of
        // whether -tcp-auto-tuning was passed (that flag only controls an opt-in
        // buffer-sizing behavior; the crash itself was in the default netstack
        // path). Confirmed via SELFVPS service log 2026-08-05 06:13: two crashes,
        // 6.9s and ~53s after connect, with ERR_NETWORK_CHANGED in the browser as
        // the tunnel flapped. v2.7.0 also switched its CLI parser from Go's flag
        // package (single- or double-dash both accepted) to cobra/pflag, which
        // treats a single dash before a multi-character name as bundled short
        // flags -- so `-device`/`-proxy` now fail to parse and MUST be `--device`/
        // `--proxy`. Still no `-stack` flag (single gVisor netstack, same as
        // before) and `--tcp-auto-tuning` remains opt-in/unused here. by vovankrot
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

void XrayProtocol::setupRouting()
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
    p.localAddress = amnezia::protocols::xray::defaultLocalAddr;
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
                qCritical() << "XrayProtocol: routing setup failed at" << step << "result" << int(result);
                stop();
                setLastError(ErrorCode::InternalError);
                return;
            }
            stopTimeoutTimer();
            setConnectionState(Vpn::ConnectionState::Connected);
            scheduleHealthCheck();
        });
}

// Post-Connect healthcheck: the tunnel can reach Connected (xray up, tun2socks
// up, routes in place) but still not actually carry traffic -- this shows up
// after cold boot where some part of the stack (WFP filters, split-tunnel
// driver, adapter binding) is in a stale/half-initialized state carried over
// from the previous session. The client sits happily on "Подключено" while
// nothing works, and only a full Windows restart clears it -- reported by the
// user on 2026-07-26.
//
// Fix: after we transition to Connected, kick off a background probe through
// the SOCKS inbound at intervals. Require certificate validation and an HTTP
// response; several consecutive failures produce a warning, not a reconnect.
namespace {
// First check runs LONG after Connected. 5 seconds was too eager: the tun2socks
// stack and the mKCP window are still warming up, so a probe that arrives during
// that ramp times out even on a perfectly healthy tunnel. That single false
// negative used to be enough to feed the retry-loop the caller was watching --
// see 2026-08-05 14:59 where one dropped probe cascaded into a full reconnect.
constexpr int kHealthCheckFirstDelayMs = 20000;
constexpr int kHealthCheckIntervalMs = 30000;
constexpr int kHealthCheckTimeoutMs = 5000;
constexpr int kHealthCheckFailuresBeforeReset = 3;
}  // namespace

void XrayProtocol::scheduleHealthCheck()
{
    qDebug() << "XrayProtocol::scheduleHealthCheck() this=" << static_cast<void *>(this)
              << "m_healthTimer=" << static_cast<void *>(m_healthTimer);
    m_healthCheckFailures = 0;
    const auto generation = ++m_healthGeneration;
    if (!m_healthTimer) {
        m_healthTimer = new QTimer(this);
        m_healthTimer->setSingleShot(false);
        connect(m_healthTimer, &QTimer::timeout, this, &XrayProtocol::runHealthCheck);
        qDebug() << "XrayProtocol::scheduleHealthCheck() created m_healthTimer="
                  << static_cast<void *>(m_healthTimer) << "for" << static_cast<void *>(this);
    }
    m_healthTimer->stop();
    // Fire the FIRST check after the short delay, then let interval mode take over.
    m_healthTimer->setInterval(kHealthCheckIntervalMs);
    QTimer::singleShot(kHealthCheckFirstDelayMs, this, [this, generation]() {
        if (generation != m_healthGeneration || m_stopping || connectionState() != Vpn::ConnectionState::Connected) {
            return;
        }
        runHealthCheck();
        if (m_healthTimer) {
            m_healthTimer->start();
        }
    });
}

void XrayProtocol::cancelHealthCheck()
{
    qDebug() << "XrayProtocol::cancelHealthCheck() this=" << static_cast<void *>(this)
              << "m_healthTimer=" << static_cast<void *>(m_healthTimer)
              << "active=" << (m_healthTimer && m_healthTimer->isActive());
    if (m_healthTimer) {
        m_healthTimer->stop();
    }
    ++m_healthGeneration;
    if (m_healthProbe) {
        disconnect(m_healthProbe, nullptr, this, nullptr);
        m_healthProbe->cancel();
        m_healthProbe->deleteLater();
        m_healthProbe = nullptr;
    }
    m_healthCheckFailures = 0;
}

void XrayProtocol::runHealthCheck()
{
    qDebug() << "XrayProtocol::runHealthCheck() this=" << static_cast<void *>(this)
              << "m_stopping=" << m_stopping << "state=" << static_cast<int>(connectionState())
              << "thread=" << QThread::currentThread();
    if (m_stopping || connectionState() != Vpn::ConnectionState::Connected) {
        cancelHealthCheck();
        return;
    }

    if (m_healthProbe) return;
    const QString host = probeHost();
    const QNetworkProxy proxy(QNetworkProxy::Socks5Proxy, QStringLiteral("127.0.0.1"),
                              quint16(localSocksPort()), m_socksUser, m_socksPassword);
    const auto generation = m_healthGeneration;
    auto *watcher = new QFutureWatcher<TunnelDataProbe::Result>(this);
    m_healthProbe = watcher;
    const QPointer<QFutureWatcher<TunnelDataProbe::Result>> probe(watcher);
    connect(watcher, &QFutureWatcher<TunnelDataProbe::Result>::finished, this, [this, probe, generation] {
        if (!probe || generation != m_healthGeneration || m_stopping
            || connectionState() != Vpn::ConnectionState::Connected || probe->isCanceled()) return;
        const auto result = probe->result();
        probe->deleteLater();
        if (m_healthProbe == probe) m_healthProbe = nullptr;
        if (!result.success)
            qWarning() << "XRay application probe failed at stage" << int(result.failure) << "after" << result.elapsedMs << "ms";
        handleHealthCheckResult(result.success);
    });
    watcher->setFuture(QtConcurrent::run([host, proxy] {
        return TunnelDataProbe::run(host, 443, proxy, kHealthCheckTimeoutMs);
    }));
}

void XrayProtocol::handleHealthCheckResult(bool ok)
{
    if (ok) {
        if (m_healthCheckFailures > 0) {
            qDebug() << "XRay healthcheck recovered after" << m_healthCheckFailures << "failure(s)";
        }
        m_healthCheckFailures = 0;
        return;
    }

    // Report the threshold once, then keep probing without flooding warnings
    // or repeatedly rebuilding routes/WFP while excluded applications stream.
    if (m_healthCheckFailures >= kHealthCheckFailuresBeforeReset) return;
    ++m_healthCheckFailures;
    qWarning() << "XRay healthcheck failed" << m_healthCheckFailures << "/"
               << kHealthCheckFailuresBeforeReset;

    if (m_healthCheckFailures < kHealthCheckFailuresBeforeReset) {
        return;
    }

    // Failure of an external probe does not prove that local helpers or the
    // whole tunnel are dead. Rebuilding WFP/routes here also interrupts bypass
    // video sessions. Keep the session; helper exits still use the error path.
    if (m_healthCheckFailures == kHealthCheckFailuresBeforeReset) {
        qWarning() << "XRay healthcheck unavailable; preserving session and app bypass flows";
        emit networkPolicyWarning(tr("XRay: проверочный сайт недоступен. Соединение сохранено, чтобы не прерывать исключённые приложения. Если VPN не работает, переподключите его вручную."));
    }
}

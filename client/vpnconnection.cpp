#include "vpnconnection.h"

#include <algorithm>
#include <QDebug>
#include <QEventLoop>
#include <QFile>
#include <QHostInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QRegularExpression>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <configurators/cloak_configurator.h>
#include <configurators/openvpn_configurator.h>
#include <configurators/shadowsocks_configurator.h>
#include <configurators/wireguard_configurator.h>

#ifdef AMNEZIA_DESKTOP
    #include "core/ipcclient.h"
    #include <protocols/wireguardprotocol.h>
    #include "utilities.h"
    #include "ui/notificationhandler.h"
#endif

#ifdef Q_OS_ANDROID
    #include "platforms/android/android_controller.h"
    #include <QThread>

#endif

#if defined(Q_OS_IOS) || defined(MACOS_NE)
    #include "platforms/ios/ios_controller.h"
#endif

#include "core/networkUtilities.h"
#include "core/geoipUpdater.h"
#include "core/blocklistUpdater.h"
#include "vpnconnection.h"

namespace {
bool hasMeaningfulWildcardContent(const QString &site)
{
    QString stripped = site.trimmed();
    stripped.remove('*');
    static const QRegularExpression meaningfulPattern(QStringLiteral("[A-Za-z0-9]"));
    return meaningfulPattern.match(stripped).hasMatch();
}

bool isWildcardSitePattern(const QString &site)
{
    return site.contains('*') && hasMeaningfulWildcardContent(site);
}

QString wildcardSitePatternToRegex(const QString &site)
{
    const QString normalizedSite = site.trimmed().toLower();
    if (!isWildcardSitePattern(normalizedSite)) {
        return QString();
    }

    const bool legacySuffixPattern = normalizedSite.startsWith("*.")
                                     && normalizedSite.indexOf('*', 2) < 0;
    if (legacySuffixPattern) {
        QString suffix = QRegularExpression::escape(normalizedSite.mid(2));
        return QString("regexp:(^|.+\\.)%1$").arg(suffix);
    }

    QString escapedPattern = QRegularExpression::escape(normalizedSite);
    escapedPattern.replace(QStringLiteral("\\*"), QStringLiteral(".*"));
    return QString("regexp:^%1$").arg(escapedPattern);
}

bool isXrayLikeProtocolName(const QString &protocolName)
{
    return protocolName == ProtocolProps::protoToString(Proto::Xray)
           || protocolName == ProtocolProps::protoToString(Proto::SSXray);
}

bool isHysteria2ProtocolName(const QString &protocolName)
{
    return protocolName == ProtocolProps::protoToString(Proto::Hysteria2);
}

bool isAnyTlsProtocolName(const QString &protocolName)
{
    return protocolName == ProtocolProps::protoToString(Proto::AnyTls);
}

bool siteSplitTunnelingSupportedForProtocolName(const QString &protocolName)
{
    return isXrayLikeProtocolName(protocolName);
}

bool xraySiteSplitTunnelingHandledInCore(bool splitTunnelingEnabled, const QString &protocolName)
{
    // In-core (XRay router) site split tunneling only works correctly for
    // the NATIVE XRay outbound, where Xray::sockCallback() binds every
    // outbound socket (including freedom/direct) to the physical NIC via
    // IP_UNICAST_IF.  That guarantees direct traffic does NOT loop back
    // through the full-tunnel TUN.
    //
    // For Hysteria2 and AnyTLS the in-core router was a SOCKS5→SOCKS5
    // wrapper around an external upstream proxy.  Its freedom outbound
    // still tried to send "direct" traffic via the physical NIC, but
    // because the TUN owns the default route on Windows, those packets
    // looped back and nothing opened.  Those protocols must not expose
    // site-based split tunneling until a different architecture exists.
    if (!splitTunnelingEnabled) return false;
    return siteSplitTunnelingSupportedForProtocolName(protocolName);
}

struct XraySplitTunnelPatterns
{
    QJsonArray domainPatterns;
    QJsonArray ipPatterns;

    bool isEmpty() const
    {
        return domainPatterns.isEmpty() && ipPatterns.isEmpty();
    }
};

struct XraySplitTunnelRuleSpec
{
    QString outboundTag;
    QJsonArray domainPatterns;
    QJsonArray ipPatterns;
    int specificity = 0;
    int order = 0;
};

int siteRuleSpecificity(const Settings::SiteSplitRule &rule)
{
    const QString normalizedHostname = rule.hostname.trimmed().toLower();
    if (NetworkUtilities::checkIpSubnetFormat(normalizedHostname)) {
        const QStringList subnetParts = normalizedHostname.split('/');
        const int prefixLength = subnetParts.size() > 1 ? subnetParts.at(1).toInt() : 32;
        return 4000 + prefixLength;
    }

    if (QHostAddress(normalizedHostname).protocol() != QAbstractSocket::UnknownNetworkLayerProtocol) {
        return 3900;
    }

    if (!isWildcardSitePattern(normalizedHostname)) {
        return 3000 + normalizedHostname.count('.') * 16 + normalizedHostname.length();
    }

    int meaningfulCharacters = 0;
    int wildcardCount = 0;
    for (const QChar symbol : normalizedHostname) {
        if (symbol == '*') {
            ++wildcardCount;
        } else if (symbol.isLetterOrNumber()) {
            ++meaningfulCharacters;
        }
    }

    return 1000 + meaningfulCharacters * 8 + normalizedHostname.count('.') * 4 - wildcardCount * 32;
}

XraySplitTunnelPatterns buildXraySplitTunnelPatterns(const Settings::SiteSplitRule &rule)
{
    XraySplitTunnelPatterns patterns;

    const QString hostname = rule.hostname.trimmed().toLower();
    const QString ip = rule.ip.trimmed();

    if (isWildcardSitePattern(hostname)) {
        const QString regexDomain = wildcardSitePatternToRegex(hostname);
        if (!regexDomain.isEmpty()) {
            patterns.domainPatterns.append(regexDomain);
        }
    } else if (hostname.contains('/')) {
        patterns.ipPatterns.append(hostname);
    } else if (QHostAddress(hostname).protocol() != QAbstractSocket::UnknownNetworkLayerProtocol) {
        patterns.ipPatterns.append(hostname);
    } else {
        patterns.domainPatterns.append(QString("domain:%1").arg(hostname));
    }

    if (!ip.isEmpty() && QHostAddress(ip).protocol() != QAbstractSocket::UnknownNetworkLayerProtocol) {
        patterns.ipPatterns.append(ip);
    }

    return patterns;
}

QVector<XraySplitTunnelRuleSpec> buildOrderedXraySplitTunnelRules(const QVector<Settings::SiteSplitRule> &siteRules)
{
    QVector<XraySplitTunnelRuleSpec> orderedRules;
    orderedRules.reserve(siteRules.size() * 2);

    for (int index = 0; index < siteRules.size(); ++index) {
        const Settings::SiteSplitRule &siteRule = siteRules.at(index);
        const XraySplitTunnelPatterns patterns = buildXraySplitTunnelPatterns(siteRule);
        if (patterns.isEmpty()) {
            continue;
        }

        XraySplitTunnelRuleSpec ruleSpec;
        ruleSpec.outboundTag = siteRule.useVpn ? QStringLiteral("proxy") : QStringLiteral("direct");
        ruleSpec.domainPatterns = patterns.domainPatterns;
        ruleSpec.ipPatterns = patterns.ipPatterns;
        ruleSpec.specificity = siteRuleSpecificity(siteRule);
        ruleSpec.order = index;
        orderedRules.append(ruleSpec);
    }

    std::stable_sort(orderedRules.begin(), orderedRules.end(), [](const XraySplitTunnelRuleSpec &left,
                                                                  const XraySplitTunnelRuleSpec &right) {
        if (left.specificity != right.specificity) {
            return left.specificity > right.specificity;
        }

        return left.order < right.order;
    });

    return orderedRules;
}
}

VpnConnection::VpnConnection(std::shared_ptr<Settings> settings, QObject *parent)
    : QObject(parent), m_settings(settings), m_checkTimer(new QTimer(this))
{
#if defined(Q_OS_IOS) || defined(MACOS_NE)
    m_checkTimer.setInterval(1000);
    connect(IosController::Instance(), &IosController::connectionStateChanged, this, &VpnConnection::setConnectionState);
    connect(IosController::Instance(), &IosController::bytesChanged, this, &VpnConnection::onBytesChanged);
#endif
}

VpnConnection::~VpnConnection()
{
}

void VpnConnection::onBytesChanged(quint64 receivedBytes, quint64 sentBytes)
{
    emit bytesChanged(receivedBytes, sentBytes);
}

void VpnConnection::onKillSwitchModeChanged(bool enabled)
{
#ifdef AMNEZIA_DESKTOP
    IpcClient::withInterface([enabled](QSharedPointer<IpcInterfaceReplica> iface){
        QRemoteObjectPendingReply<bool> reply = iface->refreshKillSwitch(enabled);
        if (reply.waitForFinished() && reply.returnValue())
            qDebug() << "VpnConnection::onKillSwitchModeChanged: Killswitch refreshed";
        else
            qWarning() << "VpnConnection::onKillSwitchModeChanged: Failed to execute remote refreshKillSwitch call";
    });
#endif
}

void VpnConnection::onConnectionStateChanged(Vpn::ConnectionState state)
{
#ifdef AMNEZIA_DESKTOP
    auto container = m_settings->defaultContainer(m_settings->defaultServerIndex());
    const QString protocolName = m_vpnConfiguration.value(config_key::vpnproto).toString();
    const bool siteSplitTunnelingEnabled = m_settings->isSitesSplitTunnelingEnabled()
            && siteSplitTunnelingSupportedForProtocolName(protocolName);
    const bool xraySitesRoutedInCore = xraySiteSplitTunnelingHandledInCore(
        siteSplitTunnelingEnabled,
        protocolName);

    IpcClient::withInterface([&](QSharedPointer<IpcInterfaceReplica> iface) {
        switch (state) {
            case Vpn::ConnectionState::Connected: {
                iface->resetIpStack();

                // Warn the user if a local system proxy (e.g. a 3rd-party
                // interceptor) is sitting in front of the VPN and crippling
                // throughput. By default, transparently disable it so the
                // tunnel can actually reach foreign sites; user can opt out
                // via Settings → Application.
                QString hijackProxy;
                if (Utils::isLoopbackSystemProxyActive(&hijackProxy)) {
                    const bool autoDisable = m_settings->autoDisableLoopbackProxyOnConnect();
                    if (autoDisable && Utils::disableSystemProxy()) {
                        qInfo() << "VpnConnection::onConnectionStateChanged: auto-disabled loopback system proxy"
                                << hijackProxy;
                        if (auto *notifier = NotificationHandler::instance()) {
                            notifier->systemProxyAutoDisabledNotification(hijackProxy);
                        }
                    } else {
                        qWarning() << "VpnConnection::onConnectionStateChanged: a local system proxy is active"
                                   << "and may intercept VPN traffic:" << hijackProxy;
                        if (auto *notifier = NotificationHandler::instance()) {
                            notifier->systemProxyDetectedNotification(hijackProxy);
                        }
                    }
                }

                auto flushDns = iface->flushDns();
                if (flushDns.waitForFinished() && flushDns.returnValue())
                    qDebug() << "VpnConnection::onConnectionStateChanged: Successfully flushed DNS";
                else
                    qWarning() << "VpnConnection::onConnectionStateChanged: Failed to clear saved routes";


                if (!ContainerProps::isAwgContainer(container) &&
                    container != DockerContainer::WireGuard) {
                    QString dns1 = m_vpnConfiguration.value(config_key::dns1).toString();
                    QString dns2 = m_vpnConfiguration.value(config_key::dns2).toString();

                    // TODO: add error code handling for all routeAddList (or rework the code below)
                    iface->routeAddList(m_vpnProtocol->vpnGateway(), QStringList() << dns1 << dns2);

                    if (siteSplitTunnelingEnabled) {
                        if (xraySitesRoutedInCore) {
                            qDebug() << "VpnConnection::onConnectionStateChanged: site split tunneling uses in-core routing; skipping OS site route changes";
                        } else {
                            iface->routeDeleteList(m_vpnProtocol->vpnGateway(), QStringList() << "0.0.0.0");
                            // qDebug() << "VpnConnection::onConnectionStateChanged :: adding custom routes, count:" << forwardIps.size();
                            if (m_settings->routeMode() == Settings::VpnOnlyForwardSites) {
                                QTimer::singleShot(1000, m_vpnProtocol.data(),
                                                   [this]() { addSitesRoutes(m_vpnProtocol->vpnGateway(), m_settings->routeMode()); });
                            } else if (m_settings->routeMode() == Settings::VpnAllExceptSites) {
                                iface->routeAddList(m_vpnProtocol->vpnGateway(), QStringList() << "0.0.0.0/1");
                                iface->routeAddList(m_vpnProtocol->vpnGateway(), QStringList() << "128.0.0.0/1");

                                iface->routeAddList(m_vpnProtocol->routeGateway(), QStringList() << remoteAddress());
                                addSitesRoutes(m_vpnProtocol->routeGateway(), m_settings->routeMode());
                            }
                        }
                    }
                }
            } break;
            case Vpn::ConnectionState::Disconnected:
            case Vpn::ConnectionState::Error: {
                auto restoreDns = iface->restoreResolvers();
                if (restoreDns.waitForFinished() && restoreDns.returnValue())
                    qDebug() << "VpnConnection::onConnectionStateChanged: Successfully restored DNS resolvers";
                else
                    qWarning() << "VpnConnection::onConnectionStateChanged: Failed to restore DNS resolvers";

                auto flushDns = iface->flushDns();
                if (flushDns.waitForFinished() && flushDns.returnValue())
                    qDebug() << "VpnConnection::onConnectionStateChanged: Successfully flushed DNS";
                else
                    qWarning() << "VpnConnection::onConnectionStateChanged: Failed to flush DNS";

                auto clearSavedRoutes = iface->clearSavedRoutes();
                if (clearSavedRoutes.waitForFinished() && clearSavedRoutes.returnValue())
                    qDebug() << "VpnConnection::onConnectionStateChanged: Successfully cleared saved routes";
                else
                    qWarning() << "VpnConnection::onConnectionStateChanged: Failed to clear saved routes";
            } break;
            default:
                break;
        }
    });
#endif

#if defined(Q_OS_IOS) || defined(MACOS_NE)
    if (state == Vpn::ConnectionState::Connected ||
        state == Vpn::ConnectionState::Connecting ||
        state == Vpn::ConnectionState::Reconnecting) {
        m_checkTimer.start();
    } else {
        m_checkTimer.stop();
    }
#endif
}

const QString &VpnConnection::remoteAddress() const
{
    return m_remoteAddress;
}

void VpnConnection::addSitesRoutes(const QString &gw, Settings::RouteMode mode)
{
#ifdef AMNEZIA_DESKTOP
    QStringList ips;
    QStringList sites;
    const QVariantMap &m = m_settings->vpnSites(mode);
    for (auto i = m.constBegin(); i != m.constEnd(); ++i) {
        if (NetworkUtilities::checkIpSubnetFormat(i.key())) {
            ips.append(i.key());
        } else {
            if (NetworkUtilities::checkIpSubnetFormat(i.value().toString())) {
                ips.append(i.value().toString());
            }
            sites.append(i.key());
        }
    }
    ips.removeDuplicates();

    if (!ips.isEmpty()) {
        IpcClient::withInterface([&](QSharedPointer<IpcInterfaceReplica> iface) {
            iface->routeAddList(gw, ips);
        });
    }

    // re-resolve domains
    for (const QString &site : sites) {
        if (isWildcardSitePattern(site)) {
            qDebug() << "VpnConnection::addSitesRoutes: wildcard pattern does not resolve to a single IP route" << site;
            continue;
        }

        const auto &cbResolv = [this, site, gw, mode, ips](const QHostInfo &hostInfo) {
            if (hostInfo.error() != QHostInfo::NoError) {
                qWarning() << "VpnConnection::addSitesRoutes: failed to resolve" << site << hostInfo.errorString();
                return;
            }

            QStringList resolvedIps;
            for (const QHostAddress &addr : hostInfo.addresses()) {
                if (addr.protocol() == QAbstractSocket::NetworkLayerProtocol::IPv4Protocol) {
                    const QString &ip = addr.toString();
                    if (!ips.contains(ip) && !resolvedIps.contains(ip)) {
                        resolvedIps.append(ip);
                    }
                }
            }

            if (resolvedIps.isEmpty()) {
                qWarning() << "VpnConnection::addSitesRoutes: no new IPv4 routes for" << site;
                return;
            }

            IpcClient::withInterface([&gw, resolvedIps](QSharedPointer<IpcInterfaceReplica> iface) {
                auto routeAddList = iface->routeAddList(gw, resolvedIps);
                if (!routeAddList.waitForFinished() || routeAddList.returnValue() != resolvedIps.count()) {
                    qWarning() << "VpnConnection::addSitesRoutes: failed to add all resolved routes"
                               << resolvedIps << "via" << gw;
                }
            });
            m_settings->addVpnSite(mode, site, resolvedIps.first());

            IpcClient::withInterface([](QSharedPointer<IpcInterfaceReplica> iface) {
                auto reply = iface->flushDns();
                if (!reply.waitForFinished() || !reply.returnValue())
                    qWarning() << "VpnConnection::addSitesRoutes: Failed to flush DNS";
            });
        };
        QHostInfo::lookupHost(site, this, cbResolv);
    }
#endif
}

QSharedPointer<VpnProtocol> VpnConnection::vpnProtocol() const
{
    return m_vpnProtocol;
}

void VpnConnection::disconnectSlots()
{
    if (m_vpnProtocol) {
        m_vpnProtocol->disconnect();
    }
}

ErrorCode VpnConnection::lastError() const
{
#ifdef Q_OS_ANDROID
    return ErrorCode::AndroidError;
#endif

    if (m_vpnProtocol.isNull()) {
        return ErrorCode::InternalError;
    }

    return m_vpnProtocol.data()->lastError();
}

void VpnConnection::connectToVpn(int serverIndex, const ServerCredentials &credentials, DockerContainer container,
                                 const QJsonObject &vpnConfiguration)
{
    qDebug() << QString("Trying to connect to VPN, server index is %1, container is %2, route mode is")
                        .arg(serverIndex)
                        .arg(ContainerProps::containerToString(container))
             << m_settings->routeMode();

    m_remoteAddress = NetworkUtilities::getIPAddress(credentials.hostName);
    setConnectionState(Vpn::ConnectionState::Connecting);

    m_container = container;
    m_vpnConfigurationBase = vpnConfiguration;
    m_vpnConfiguration = vpnConfiguration;

#ifdef AMNEZIA_DESKTOP
    if (m_vpnProtocol) {
        disconnect(m_vpnProtocol.data(), &VpnProtocol::protocolError, this, &VpnConnection::vpnProtocolError);
        m_vpnProtocol->stop();
        m_vpnProtocol.reset();
    }
    appendKillSwitchConfig();
#endif

    appendSplitTunnelingConfig();
    appendXrayRoutingConfig();

#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS) && !defined(MACOS_NE)
    m_vpnProtocol.reset(VpnProtocol::factory(container, m_vpnConfiguration));
    if (!m_vpnProtocol) {
        setConnectionState(Vpn::ConnectionState::Error);
        return;
    }
    m_vpnProtocol->prepare();
#elif defined Q_OS_ANDROID
    androidVpnProtocol = createDefaultAndroidVpnProtocol();
    createAndroidConnections();

    m_vpnProtocol.reset(androidVpnProtocol);
#elif defined Q_OS_IOS || defined(MACOS_NE)
    Proto proto = ContainerProps::defaultProtocol(container);
    IosController::Instance()->connectVpn(proto, m_vpnConfiguration);
    connect(&m_checkTimer, &QTimer::timeout, IosController::Instance(), &IosController::checkStatus);
    return;
#endif

    createProtocolConnections();

    if (ErrorCode err = m_vpnProtocol->start(); err != ErrorCode::NoError) {
        m_vpnProtocol->setLastError(err);
        setConnectionState(Vpn::ConnectionState::Error);
        emit vpnProtocolError(err);
    }
}

void VpnConnection::createProtocolConnections()
{
    connect(m_vpnProtocol.data(), &VpnProtocol::protocolError, this, &VpnConnection::vpnProtocolError);
    connect(m_vpnProtocol.data(), &VpnProtocol::connectionStateChanged, this, &VpnConnection::setConnectionState);
    connect(m_vpnProtocol.data(), SIGNAL(bytesChanged(quint64, quint64)), this, SLOT(onBytesChanged(quint64, quint64)));

#ifdef AMNEZIA_DESKTOP
    IpcClient::withInterface([this](QSharedPointer<IpcInterfaceReplica> rep) {
        connect(rep.data(), &IpcInterfaceReplica::networkChanged, this, &VpnConnection::reconnectToVpn, Qt::QueuedConnection);
        connect(rep.data(), &IpcInterfaceReplica::wakeup, this, &VpnConnection::reconnectToVpn, Qt::QueuedConnection);
    });
#endif
}

void VpnConnection::appendKillSwitchConfig()
{
    m_vpnConfiguration.insert(config_key::killSwitchOption, QVariant(m_settings->isKillSwitchEnabled()).toString());
    m_vpnConfiguration.insert(config_key::allowedDnsServers, QVariant(m_settings->allowedDnsServers()).toJsonValue());
}

void VpnConnection::appendSplitTunnelingConfig()
{
    bool allowSiteBasedSplitTunneling = true;
    m_vpnConfiguration.remove(config_key::xrayRouterConfig);

    // this block is for old native configs and for old self-hosted configs
    const QString protocolName = m_vpnConfiguration.value(config_key::vpnproto).toString();
    const bool requestedSiteSplitTunneling = m_settings->isSitesSplitTunnelingEnabled();
    const bool siteSplitTunnelingEnabled = requestedSiteSplitTunneling
            && siteSplitTunnelingSupportedForProtocolName(protocolName);
    if (requestedSiteSplitTunneling && !siteSplitTunnelingEnabled) {
        // Surface the silent downgrade to the user, but only if they actually have rules.
        const QVector<Settings::SiteSplitRule> siteRules = m_settings->getVpnSiteRules();
        if (!siteRules.isEmpty()) {
            emit siteSplitTunnelingWarning(
                tr("Site split tunneling works only with XRay; full VPN is used for this protocol"));
        }
        qWarning() << "Site split tunneling is disabled for unsupported protocol" << protocolName
                   << "(supported: XRay/SSXray). Falling back to full VPN routing.";
    }
    if (protocolName == ProtocolProps::protoToString(Proto::Awg) || protocolName == ProtocolProps::protoToString(Proto::WireGuard)) {
        allowSiteBasedSplitTunneling = false;
        auto configData = m_vpnConfiguration.value(protocolName + "_config_data").toObject();
        if (configData.value(config_key::allowed_ips).isString()) {
            QJsonArray allowedIpsJsonArray = QJsonArray::fromStringList(configData.value(config_key::allowed_ips).toString().split(", "));
            configData.insert(config_key::allowed_ips, allowedIpsJsonArray);
            m_vpnConfiguration.insert(protocolName + "_config_data", configData);
        } else if (configData.value(config_key::allowed_ips).isUndefined()) {
            auto nativeConfig = configData.value(config_key::config).toString();
            auto nativeConfigLines = nativeConfig.split("\n");
            for (auto &line : nativeConfigLines) {
                if (line.contains("AllowedIPs")) {
                    auto allowedIpsString = line.split(" = ");
                    if (allowedIpsString.size() < 1) {
                        break;
                    }
                    QJsonArray allowedIpsJsonArray = QJsonArray::fromStringList(allowedIpsString.at(1).split(", "));
                    configData.insert(config_key::allowed_ips, allowedIpsJsonArray);
                    m_vpnConfiguration.insert(protocolName + "_config_data", configData);
                    break;
                }
            }
        }

        if (configData.value(config_key::persistent_keep_alive).isUndefined()) {
            auto nativeConfig = configData.value(config_key::config).toString();
            auto nativeConfigLines = nativeConfig.split("\n");
            for (auto &line : nativeConfigLines) {
                if (line.contains("PersistentKeepalive")) {
                    auto persistentKeepaliveString = line.split(" = ");
                    if (persistentKeepaliveString.size() < 1) {
                        break;
                    }
                    configData.insert(config_key::persistent_keep_alive, persistentKeepaliveString.at(1));
                    m_vpnConfiguration.insert(protocolName + "_config_data", configData);
                    break;
                }
            }
        }

        QJsonArray allowedIpsJsonArray = configData.value(config_key::allowed_ips).toArray();
        if (allowedIpsJsonArray.contains("0.0.0.0/0") && allowedIpsJsonArray.contains("::/0")) {
            allowSiteBasedSplitTunneling = true;
        }
    }
    const bool xraySitesRoutedInCore = xraySiteSplitTunnelingHandledInCore(
        siteSplitTunnelingEnabled, protocolName);

    Settings::RouteMode routeMode = Settings::RouteMode::VpnAllSites;
    QJsonArray sitesJsonArray;
    if (siteSplitTunnelingEnabled) {
        const Settings::RouteMode configuredRouteMode = m_settings->routeMode();
        const QVariantMap configuredSites = m_settings->vpnSites(configuredRouteMode);
        routeMode = xraySitesRoutedInCore ? Settings::RouteMode::VpnAllSites : configuredRouteMode;

        if (allowSiteBasedSplitTunneling && !xraySitesRoutedInCore) {
            auto sites = m_settings->getVpnIps(configuredRouteMode);
            for (const auto &site : sites) {
                sitesJsonArray.append(site);
            }

            if (sitesJsonArray.isEmpty()) {
                if (configuredSites.isEmpty()) {
                    routeMode = Settings::RouteMode::VpnAllSites;
                } else {
                    qWarning() << "Site split tunneling has configured host rules but no IP routes yet; keeping route mode" << routeMode;
                }
            }

            if (configuredRouteMode == Settings::VpnOnlyForwardSites) {
                // Allow traffic to Amnezia DNS
                sitesJsonArray.append(m_vpnConfiguration.value(config_key::dns1).toString());
                sitesJsonArray.append(m_vpnConfiguration.value(config_key::dns2).toString());
            }
        } else if (xraySitesRoutedInCore) {
            qDebug() << "Site split tunneling will be enforced by in-core routing with full-tunnel TUN";
        }
    }

    m_vpnConfiguration.insert(config_key::splitTunnelType, routeMode);
    m_vpnConfiguration.insert(config_key::splitTunnelSites, sitesJsonArray);

    // Load Russian IP CIDRs only for protocols that explicitly support the
    // site-split path. For XRay/SSXray this is skipped because XRay handles
    // geoip:ru → direct in-core (via routing rules + IP_UNICAST_IF on freedom
    // outbound).
    // OS-level CIDR routes (/19, /24) are more specific than the TUN
    // routes (/1..8) and would pre-empt ALL traffic to Russian IPs from
    // entering TUN — including traffic from processes NOT caught by the
    // WFP split tunnel driver (child processes, etc.).  This breaks app
    // split tunneling: a bypassed app's child process that used to go
    // TUN → XRay → proxy → VPN now goes direct with the home IP.
    {
        if (siteSplitTunnelingEnabled && m_settings->bypassRuGeoIp()
            && !xraySitesRoutedInCore) {
            const QString cidrPath = GeoipUpdater::cidrFilePath(m_settings);
            QFile cidrFile(cidrPath);
            if (cidrFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QJsonArray geoCidrs;
                while (!cidrFile.atEnd()) {
                    QString line = QString::fromUtf8(cidrFile.readLine()).trimmed();
                    if (!line.isEmpty()) {
                        geoCidrs.append(line);
                    }
                }
                cidrFile.close();
                if (!geoCidrs.isEmpty()) {
                    m_vpnConfiguration.insert(config_key::bypassRuSitesGeo, geoCidrs);
                    qDebug() << "bypassRuSitesGeo: loaded" << geoCidrs.size()
                             << "RU CIDRs from" << cidrPath << "for" << protocolName;
                }
            } else {
                qWarning() << "bypassRuSitesGeo: failed to open" << cidrPath;
            }
        } else if (xraySitesRoutedInCore && m_settings->bypassRuGeoIp()) {
            qDebug() << "bypassRuSitesGeo: skipped OS-level CIDR routes for"
                     << protocolName << "(handled in-core by XRay geoip:ru → direct)";
        }
    }

    Settings::AppsRouteMode appsRouteMode = Settings::AppsRouteMode::VpnAllApps;
    QJsonArray appsJsonArray;
    if (m_settings->isAppsSplitTunnelingEnabled()) {
        appsRouteMode = Settings::AppsRouteMode::VpnAllExceptApps;

        // Only "except" (bypass VPN) apps are sent to the service
        auto apps = m_settings->getVpnApps(Settings::VpnAllExceptApps);
        for (const auto &app : apps) {
            appsJsonArray.append(app.appPath.isEmpty() ? app.packageName : app.appPath);
        }

        if (appsJsonArray.isEmpty()) {
            appsRouteMode = Settings::AppsRouteMode::VpnAllApps;
        }
    }

    m_vpnConfiguration.insert(config_key::appSplitTunnelType, appsRouteMode);
    m_vpnConfiguration.insert(config_key::splitTunnelApps, appsJsonArray);

    qDebug() << QString("Site split tunneling is %1, route mode is %2")
                        .arg(siteSplitTunnelingEnabled ? "enabled" : "disabled")
                        .arg(routeMode);
    qDebug() << QString("App split tunneling is %1, route mode is %2")
                        .arg(m_settings->isAppsSplitTunnelingEnabled() ? "enabled" : "disabled")
                        .arg(appsRouteMode);
}

void VpnConnection::appendXrayRoutingConfig()
{
    auto protocolName = m_vpnConfiguration.value(config_key::vpnproto).toString();
    const bool xrayLikeProtocol = isXrayLikeProtocolName(protocolName);
    m_vpnConfiguration.remove(config_key::xrayRouterConfig);
    if (!xrayLikeProtocol) {
        return;
    }

    // XRay routing rules handle domain patterns (*.ru, geosite, etc.)
    // that OS-level CIDR routes cannot match.  The freedom/direct outbound
    // does NOT loop through TUN because Xray::sockCallback() sets
    // IP_UNICAST_IF on every outbound socket, binding it to the physical
    // NIC.  No service-process TUN exclusion is needed, so tun2socks UDP
    // relay on loopback remains intact.

    QString configKey;
    QJsonObject xrayConfig;
    configKey = ProtocolProps::key_proto_config_data(Proto::Xray);
    xrayConfig = m_vpnConfiguration.value(configKey).toObject();
    if (xrayConfig.isEmpty()) {
        configKey = ProtocolProps::key_proto_config_data(Proto::SSXray);
        xrayConfig = m_vpnConfiguration.value(configKey).toObject();
    }
    if (xrayConfig.isEmpty()) return;

    // Collect routing rules
    QJsonArray rules;
    bool hasIpBasedRule = false;
    bool forwardSitesNeedCatchAll = false;
    const Settings::RouteMode routeMode = m_settings->routeMode();
    const QVector<Settings::SiteSplitRule> siteRules = m_settings->getVpnSiteRules();
    const QVector<XraySplitTunnelRuleSpec> orderedUserRules = buildOrderedXraySplitTunnelRules(siteRules);

    // by vovankrot: DIAG — pinpoint why user domain masks don't route through VPN.
    // Prints how many rules getVpnSiteRules() actually loaded and each rule's useVpn,
    // plus the resulting outbound tag, so the runtime log answers stored-false vs read-bug.
    qWarning().nospace() << "XRay routing DIAG: getVpnSiteRules loaded " << siteRules.size()
                         << " rule(s); orderedUserRules " << orderedUserRules.size();
    for (const auto &dr : siteRules) {
        qWarning().nospace() << "  DIAG siteRule host='" << dr.hostname << "' ip='" << dr.ip
                             << "' useVpn=" << dr.useVpn;
    }
    for (const auto &ors : orderedUserRules) {
        qWarning().nospace() << "  DIAG orderedRule tag=" << ors.outboundTag
                             << " domains=" << ors.domainPatterns.size()
                             << " ips=" << ors.ipPatterns.size();
    }

    const auto appendRuleSpec = [&rules, &hasIpBasedRule](const XraySplitTunnelRuleSpec &ruleSpec) {
        if (!ruleSpec.domainPatterns.isEmpty()) {
            QJsonObject rule;
            rule["type"] = QString("field");
            rule["outboundTag"] = ruleSpec.outboundTag;
            rule["domain"] = ruleSpec.domainPatterns;
            rules.append(rule);
        }

        if (!ruleSpec.ipPatterns.isEmpty()) {
            QJsonObject rule;
            rule["type"] = QString("field");
            rule["outboundTag"] = ruleSpec.outboundTag;
            rule["ip"] = ruleSpec.ipPatterns;
            rules.append(rule);
            hasIpBasedRule = true;
        }
    };

    // --- Rule priority: user-defined lists FIRST, then auto-toggles (RKN, Geo).
    // XRay evaluates rules top-to-bottom, first match wins.
    // This means a user mask like *.ru → direct will override RKN → proxy
    // for the same domain, which is the intended behavior: explicit user
    // configuration always takes precedence over automatic lists.

    // 1) User-defined split tunnel sites (HIGHEST PRIORITY)
    if (m_settings->isSitesSplitTunnelingEnabled()) {
        bool hasExplicitProxyRule = false;

        for (const auto &ruleSpec : orderedUserRules) {
            if (ruleSpec.outboundTag == QStringLiteral("proxy")) {
                hasExplicitProxyRule = true;
            }

            appendRuleSpec(ruleSpec);
        }

        if (routeMode == Settings::VpnOnlyForwardSites) {
            if (hasExplicitProxyRule) {
                forwardSitesNeedCatchAll = true;
            } else {
                qWarning() << "XRay routing: ForwardSites mode has no explicit VPN-routed site rules; keeping default proxy routing";
            }
        }
    }

    // 2) RKN blocklist: blocked domains → proxy (overrides geo direct, but NOT user-defined rules above)
    if (m_settings->isSitesSplitTunnelingEnabled() && m_settings->isAutoBypassRknEnabled()) {
        const QString blocklistPath = BlocklistUpdater::domainsFilePath(m_settings);
        if (!blocklistPath.isEmpty()) {
            QFile f(blocklistPath);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QJsonArray blockedDomains;
                while (!f.atEnd()) {
                    const QByteArray line = f.readLine().trimmed();
                    if (line.isEmpty()) continue;
                    blockedDomains.append(QString::fromUtf8(line));
                }
                f.close();
                if (!blockedDomains.isEmpty()) {
                    QJsonObject rknRule;
                    rknRule["type"] = QString("field");
                    rknRule["outboundTag"] = QString("proxy");
                    rknRule["domain"] = blockedDomains;
                    rules.append(rknRule);
                    qDebug() << "XRay routing: injected" << blockedDomains.size() << "RKN blocked domains → proxy";
                }
            }
        }
    }

    // 3) Optional Russian GeoSite / GeoIP bypass rules (lowest auto-priority)
    if (m_settings->isSitesSplitTunnelingEnabled()) {
        if (m_settings->bypassRuGeoSites()) {
            QJsonObject geositeRule;
            geositeRule["type"] = QString("field");
            geositeRule["outboundTag"] = QString("direct");
            geositeRule["domain"] = QJsonArray({ QString("geosite:category-ru") });
            rules.append(geositeRule);
        }

        if (m_settings->bypassRuGeoIp()) {
            // For XRay: use protocol-filtered geoip:ru.
            // Bare "geoip:ru → direct" sends ALL Russian-IP traffic direct,
            // including raw-IP connections (RDP, SSH) that need VPN source IP.
            // By requiring protocol=tls|http, we limit direct bypass to web
            // traffic (where sniffing detects TLS ClientHello / HTTP).
            // Non-web protocols (RDP on port 9559, SSH, etc.) have no TLS/HTTP
            // signature, so sniffing won't tag them → they fall through to proxy.
            //
            // Non-XRay protocols use OS-level CIDR routes instead (set in
            // appendSplitTunnelingConfig) where this distinction is not possible.
            if (xrayLikeProtocol) {
                QJsonObject geoipRule;
                geoipRule["type"] = QString("field");
                geoipRule["outboundTag"] = QString("direct");
                geoipRule["ip"] = QJsonArray({ QString("geoip:ru") });
                geoipRule["protocol"] = QJsonArray({ QString("tls"), QString("http") });
                rules.append(geoipRule);
                hasIpBasedRule = true;
                qDebug() << "XRay routing: geoip:ru → direct (protocol-filtered: tls+http only;"
                         << "non-web traffic like RDP/SSH goes through proxy)";
            } else {
                QJsonObject geoipRule;
                geoipRule["type"] = QString("field");
                geoipRule["outboundTag"] = QString("direct");
                geoipRule["ip"] = QJsonArray({ QString("geoip:ru") });
                rules.append(geoipRule);
                hasIpBasedRule = true;
            }
        }
    }

    // 4) Catch-all direct rule for ForwardSites mode (must be LAST)
    if (forwardSitesNeedCatchAll) {
        QJsonObject catchAllDirectRule;
        catchAllDirectRule["type"] = QString("field");
        catchAllDirectRule["outboundTag"] = QString("direct");
        catchAllDirectRule["network"] = QString("tcp,udp");
        rules.append(catchAllDirectRule);
    }

    // Only add routing section if there are rules
    if (!rules.isEmpty()) {
        QJsonObject routing;
        routing["domainStrategy"] = hasIpBasedRule ? QString("IPIfNonMatch")
                                                     : QString("AsIs");
        routing["rules"] = rules;
        xrayConfig["routing"] = routing;

        m_vpnConfiguration.insert(configKey, xrayConfig);
        qDebug() << "XRay routing: injected" << rules.size()
                 << "domainStrategy:" << routing.value("domainStrategy").toString()
                 << "rules (geoSite:" << m_settings->bypassRuGeoSites()
                 << "geoIp:" << m_settings->bypassRuGeoIp() << ")";
    }
}

#ifdef Q_OS_ANDROID
void VpnConnection::restoreConnection()
{
    createAndroidConnections();

    m_vpnProtocol.reset(androidVpnProtocol);

    createProtocolConnections();
}

void VpnConnection::createAndroidConnections()
{
    androidVpnProtocol = createDefaultAndroidVpnProtocol();

    connect(AndroidController::instance(), &AndroidController::connectionStateChanged, androidVpnProtocol,
            &AndroidVpnProtocol::setConnectionState);
    connect(AndroidController::instance(), &AndroidController::statisticsUpdated, androidVpnProtocol, &AndroidVpnProtocol::setBytesChanged);
}

AndroidVpnProtocol *VpnConnection::createDefaultAndroidVpnProtocol()
{
    return new AndroidVpnProtocol(m_vpnConfiguration);
}
#endif

QString VpnConnection::bytesPerSecToText(quint64 bytes)
{
    double mbps = bytes * 8 / 1e6;
    return QString("%1 %2").arg(QString::number(mbps, 'f', 2)).arg(tr("Mbps")); // Mbit/s
}

void VpnConnection::reconnectToVpn() {
    if (m_vpnProtocol.isNull())
        return;

    if (m_connectionState != Vpn::ConnectionState::Connected) {
        qWarning() << QString("Reconnect triggered on %1 during inappropriate state: %2; ignoring slot")
                              .arg(QMetaEnum::fromType<Vpn::ConnectionState>().valueToKey(m_connectionState));
        return;
    }

    qDebug() << "Reconnect triggered. Rebuilding config and reconnecting to the server";

    setConnectionState(Vpn::ConnectionState::Reconnecting);

    // Stop and destroy old protocol
    disconnect(m_vpnProtocol.data(), &VpnProtocol::protocolError, this, &VpnConnection::vpnProtocolError);
    m_vpnProtocol->stop();
    m_vpnProtocol.reset();

    // Rebuild configuration from base (split tunneling + XRay routing may have changed)
    m_vpnConfiguration = m_vpnConfigurationBase;
#ifdef AMNEZIA_DESKTOP
    appendKillSwitchConfig();
#endif
    appendSplitTunnelingConfig();
    appendXrayRoutingConfig();

    // Recreate protocol with fresh config
#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS) && !defined(MACOS_NE)
    m_vpnProtocol.reset(VpnProtocol::factory(m_container, m_vpnConfiguration));
    if (!m_vpnProtocol) {
        setConnectionState(Vpn::ConnectionState::Error);
        return;
    }
    m_vpnProtocol->prepare();
#endif

    createProtocolConnections();

    if (ErrorCode err = m_vpnProtocol->start(); err != ErrorCode::NoError) {
        m_vpnProtocol->setLastError(err);
        setConnectionState(Vpn::ConnectionState::Error);
        emit vpnProtocolError(err);
    }
}

void VpnConnection::disconnectFromVpn()
{
#if defined(Q_OS_IOS) || defined(MACOS_NE)
    // iOS/macOS NE use IosController directly; m_vpnProtocol is not set there.
    IosController::Instance()->disconnectVpn();
    disconnect(&m_checkTimer, &QTimer::timeout, IosController::Instance(), &IosController::checkStatus);
#endif

    if (m_vpnProtocol.isNull()) {
        setConnectionState(Vpn::ConnectionState::Disconnected);
        return;
    }

    setConnectionState(Vpn::ConnectionState::Disconnecting);

#ifdef Q_OS_ANDROID
    auto *const connection = new QMetaObject::Connection;
    *connection = connect(AndroidController::instance(), &AndroidController::vpnStateChanged, this,
                          [this, connection](AndroidController::ConnectionState state) {
                              if (state == AndroidController::ConnectionState::DISCONNECTED) {
                                  setConnectionState(Vpn::ConnectionState::Disconnected);
                                  disconnect(*connection);
                                  delete connection;
                              }
                          });
#endif

    m_vpnProtocol->stop();

#if !defined(Q_OS_ANDROID) && !defined(AMNEZIA_DESKTOP)
    m_vpnProtocol->deleteLater();
#endif

    m_vpnProtocol = nullptr;
}

void VpnConnection::setConnectionState(Vpn::ConnectionState state) {
    onConnectionStateChanged(state);

    if (state == Vpn::Disconnected && m_connectionState == Vpn::Reconnecting)
        return;

    m_connectionState = state;
    emit connectionStateChanged(state);
}

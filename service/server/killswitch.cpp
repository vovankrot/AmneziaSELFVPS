#include "killswitch.h"


#include <QApplication>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <algorithm>

#include "../client/protocols/protocols_defs.h"
#include "qjsonarray.h"
#include "version.h"

#ifdef Q_OS_WIN
    #include "../client/platforms/windows/daemon/windowsfirewall.h"
    #include "../client/platforms/windows/daemon/windowsdaemon.h"
#endif

#ifdef Q_OS_LINUX
    #include "../client/platforms/linux/daemon/linuxfirewall.h"
#endif

#ifdef Q_OS_MACOS
    #include "../client/platforms/macos/daemon/macosfirewall.h"
#endif

namespace {
bool isValidIpOrCidr(const QString &value)
{
    static const QRegularExpression expression(
            QStringLiteral(R"(^(\d{1,3}\.){3}\d{1,3}(/\d{1,2})?$)"));
    if (!expression.match(value).hasMatch()) {
        return false;
    }

    const QStringList addressAndPrefix = value.split(QLatin1Char('/'));
    const QStringList octets = addressAndPrefix.first().split(QLatin1Char('.'));
    for (const QString &part : octets) {
        bool ok = false;
        const int octet = part.toInt(&ok);
        if (!ok || octet < 0 || octet > 255) {
            return false;
        }
    }

    if (addressAndPrefix.size() == 2) {
        bool ok = false;
        const int prefix = addressAndPrefix.at(1).toInt(&ok);
        if (!ok || prefix < 0 || prefix > 32) {
            return false;
        }
    }
    return true;
}

#ifdef Q_OS_WIN
QList<IPAddress> getLanBypassRanges()
{
    QList<IPAddress> ranges;

    auto appendRange = [&ranges](const IPAddress &range) {
        if (range.isValid() && !ranges.contains(range)) {
            ranges.append(range);
        }
    };

    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const QNetworkInterface::InterfaceFlags flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp)
                || !flags.testFlag(QNetworkInterface::IsRunning)
                || flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }

        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            const QHostAddress ip = entry.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol) {
                continue;
            }

            const int prefixLength = entry.prefixLength();
            if (prefixLength < 0 || prefixLength >= 32) {
                continue;
            }

            const auto subnet = QHostAddress::parseSubnet(
                    QStringLiteral("%1/%2").arg(ip.toString()).arg(prefixLength));
            if (subnet.first.protocol() != QAbstractSocket::IPv4Protocol
                    || subnet.second < 0 || subnet.second > 32) {
                continue;
            }

            appendRange(IPAddress(subnet.first, subnet.second));
        }
    }

    appendRange(IPAddress(QStringLiteral("224.0.0.0/4")));
    appendRange(IPAddress(QStringLiteral("255.255.255.255/32")));

    return ranges;
}
#endif
}

KillSwitch* s_instance = nullptr;

KillSwitch* KillSwitch::instance()
{
    if (s_instance == nullptr) {
        s_instance = new KillSwitch(qApp);
    }
    return s_instance;
}

bool KillSwitch::init()
{
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    m_appSettigns = QSharedPointer<SecureQSettings>(new SecureQSettings(ORGANIZATION_NAME, APPLICATION_NAME, nullptr));
#endif

    if (isStrictKillSwitchEnabled()) {
        return disableAllTraffic();
    }

    return true;
}

bool KillSwitch::refresh(bool enabled, bool preserveActivePolicy)
{
    const bool previous = isStrictKillSwitchEnabled();
#ifdef Q_OS_WIN
    QSettings RegHLM("HKEY_LOCAL_MACHINE\\Software\\" + QString(ORGANIZATION_NAME)
                             + "\\" + QString(APPLICATION_NAME), QSettings::NativeFormat);
    RegHLM.setValue("strictKillSwitchEnabled", enabled);
    RegHLM.sync();
    if (RegHLM.status() != QSettings::NoError) return false;
#endif

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    m_appSettigns->setValue("Conf/strictKillSwitchEnabled", enabled);
#endif

    // Strict mode governs the disconnected state. Updating its preference
    // must not tear down an active app bypass or remove the peer DNS permits.
    if (preserveActivePolicy) return true;
    const bool success = enabled ? disableAllTraffic() : disableKillSwitch();
    if (!success) {
#ifdef Q_OS_WIN
        RegHLM.setValue("strictKillSwitchEnabled", previous);
        RegHLM.sync();
#endif
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
        m_appSettigns->setValue("Conf/strictKillSwitchEnabled", previous);
#endif
    }
    return success;
}

bool KillSwitch::isStrictKillSwitchEnabled()
{
#ifdef Q_OS_WIN
    QSettings RegHLM("HKEY_LOCAL_MACHINE\\Software\\" + QString(ORGANIZATION_NAME)
                             + "\\" + QString(APPLICATION_NAME), QSettings::NativeFormat);
    return RegHLM.value("strictKillSwitchEnabled", false).toBool();
#endif
    return m_appSettigns->value("Conf/strictKillSwitchEnabled", false).toBool();
}

bool KillSwitch::disableKillSwitch() {
#ifdef Q_OS_LINUX
    if (isStrictKillSwitchEnabled()) {
        LinuxFirewall::setAnchorEnabled(LinuxFirewall::Both, QStringLiteral("000.allowLoopback"), true);
        LinuxFirewall::setAnchorEnabled(LinuxFirewall::Both, QStringLiteral("100.blockAll"), true);
        LinuxFirewall::setAnchorEnabled(LinuxFirewall::IPv4, QStringLiteral("110.allowNets"), false);
        LinuxFirewall::setAnchorEnabled(LinuxFirewall::IPv4, QStringLiteral("120.blockNets"), false);
        LinuxFirewall::setAnchorEnabled(LinuxFirewall::IPv4, QStringLiteral("200.allowVPN"), false);
        LinuxFirewall::setAnchorEnabled(LinuxFirewall::IPv6, QStringLiteral("250.blockIPv6"), true);
        LinuxFirewall::setAnchorEnabled(LinuxFirewall::Both, QStringLiteral("290.allowDHCP"), false);
        LinuxFirewall::setAnchorEnabled(LinuxFirewall::Both, QStringLiteral("300.allowLAN"), false);
        LinuxFirewall::setAnchorEnabled(LinuxFirewall::IPv4, QStringLiteral("310.blockDNS"), false);
        LinuxFirewall::setAnchorEnabled(LinuxFirewall::IPv4, QStringLiteral("320.allowDNS"), false);
        LinuxFirewall::setAnchorEnabled(LinuxFirewall::Both, QStringLiteral("400.allowPIA"), false);
    } else {
        LinuxFirewall::setAnchorEnabled(LinuxFirewall::Both, QStringLiteral("000.allowLoopback"), true);
        LinuxFirewall::setAnchorEnabled(LinuxFirewall::Both, QStringLiteral("100.blockAll"), false);
        LinuxFirewall::setAnchorEnabled(LinuxFirewall::IPv4, QStringLiteral("110.allowNets"), false);
        LinuxFirewall::setAnchorEnabled(LinuxFirewall::IPv4, QStringLiteral("120.blockNets"), false);
        LinuxFirewall::setAnchorEnabled(LinuxFirewall::IPv4, QStringLiteral("200.allowVPN"), false);
        LinuxFirewall::setAnchorEnabled(LinuxFirewall::IPv6, QStringLiteral("250.blockIPv6"), false);
        LinuxFirewall::setAnchorEnabled(LinuxFirewall::Both, QStringLiteral("290.allowDHCP"), true);
        LinuxFirewall::setAnchorEnabled(LinuxFirewall::Both, QStringLiteral("300.allowLAN"), true);
        LinuxFirewall::setAnchorEnabled(LinuxFirewall::IPv4, QStringLiteral("310.blockDNS"), false);
        LinuxFirewall::setAnchorEnabled(LinuxFirewall::IPv4, QStringLiteral("320.allowDNS"), true);
        LinuxFirewall::setAnchorEnabled(LinuxFirewall::Both, QStringLiteral("400.allowPIA"), false);
        LinuxFirewall::uninstall();
    }
#endif

#ifdef Q_OS_MACOS
    if (isStrictKillSwitchEnabled()) {
        MacOSFirewall::setAnchorEnabled(QStringLiteral("000.allowLoopback"), true);
        MacOSFirewall::setAnchorEnabled(QStringLiteral("100.blockAll"), true);
        MacOSFirewall::setAnchorEnabled(QStringLiteral("110.allowNets"), false);
        MacOSFirewall::setAnchorEnabled(QStringLiteral("120.blockNets"), false);
        MacOSFirewall::setAnchorEnabled(QStringLiteral("200.allowVPN"), false);
        MacOSFirewall::setAnchorEnabled(QStringLiteral("250.blockIPv6"), true);
        MacOSFirewall::setAnchorEnabled(QStringLiteral("290.allowDHCP"), false);
        MacOSFirewall::setAnchorEnabled(QStringLiteral("300.allowLAN"), false);
        MacOSFirewall::setAnchorEnabled(QStringLiteral("310.blockDNS"), false);
    } else {
        MacOSFirewall::uninstall();
    }
#endif

#ifdef Q_OS_WIN
    // Clean up site exclusion routes
    static_cast<WindowsDaemon*>(WindowsDaemon::instance())->deactivateSiteExclusionRoutes();
    // Clean up geo exclusion routes
    static_cast<WindowsDaemon*>(WindowsDaemon::instance())->deactivateGeoExclusionRoutes();

    // Tear down the WFP per-app split-tunnel driver on disconnect. The socks-based
    // protocols (Hysteria2/AnyTLS/Xray) engage it via enablePeerTraffic but, unlike the
    // WireGuard daemon, never stop it on teardown. activateSplitTunnel with an empty
    // config falls through to WindowsSplitTunnel::stop() and is a no-op when not running. by vovankrot
    const bool splitStopped = WindowsDaemon::instance()->activateSplitTunnel(InterfaceConfig(), 0);
    if (!splitStopped) {
        // A timed-out driver request may still own a WFP transaction. Starting
        // another transaction here can wedge the service's IPC event loop.
        qWarning() << "Split tunnel did not stop; skipping firewall transaction so the service remains responsive";
        return false;
    }

    if (isStrictKillSwitchEnabled()) {
        const bool firewallRestored = disableAllTraffic();
        return firewallRestored && splitStopped;
    }
    const bool firewallRestored = WindowsFirewall::create(this)->allowAllTraffic();
    return firewallRestored && splitStopped;
#endif

    m_allowedRanges.clear();
    return true;
}

bool KillSwitch::disableAllTraffic() {
#ifdef Q_OS_WIN
    auto *firewall = WindowsFirewall::create(this);
    if (!firewall || !firewall->enableInterface(-1, true)) return false;
#endif
#ifdef Q_OS_LINUX
    if (!LinuxFirewall::isInstalled()) {
        LinuxFirewall::install();
    }
    LinuxFirewall::setAnchorEnabled(LinuxFirewall::Both, QStringLiteral("100.blockAll"), true);
    LinuxFirewall::setAnchorEnabled(LinuxFirewall::Both, QStringLiteral("000.allowLoopback"), true);
    LinuxFirewall::setAnchorEnabled(LinuxFirewall::IPv6, QStringLiteral("250.blockIPv6"), true);
#endif
#ifdef Q_OS_MACOS
    // double-check + ensure our firewall is installed and enabled. This is necessary as
    // other software may disable pfctl before re-enabling with their own rules (e.g other VPNs)
    if (!MacOSFirewall::isInstalled())
        MacOSFirewall::install();
    MacOSFirewall::ensureRootAnchorPriority();
    MacOSFirewall::setAnchorEnabled(QStringLiteral("100.blockAll"), true);
    MacOSFirewall::setAnchorEnabled(QStringLiteral("000.allowLoopback"), true);
    MacOSFirewall::setAnchorEnabled(QStringLiteral("250.blockIPv6"), true);
#endif
    m_allowedRanges.clear();
    return true;
}

bool KillSwitch::resetAllowedRange(const QStringList &ranges) {

    if (!std::all_of(ranges.cbegin(), ranges.cend(), isValidIpOrCidr)) {
        qCritical() << "IPC: invalid IP/CIDR in ranges, rejecting resetAllowedRange";
        return false;
    }

#ifdef Q_OS_WIN
    auto *firewall = WindowsFirewall::create(this);
    if (!firewall) return false;
    const bool installed = isStrictKillSwitchEnabled()
        ? firewall->enableInterface(-1, true, ranges)
        : firewall->allowTrafficRange(ranges, true);
    if (!installed) return false;
#endif
    m_allowedRanges = ranges;

#ifdef Q_OS_LINUX
    LinuxFirewall::setAnchorEnabled(LinuxFirewall::IPv4, QStringLiteral("110.allowNets"), true);
    LinuxFirewall::updateAllowNets(m_allowedRanges);
#endif

#ifdef Q_OS_MACOS
    MacOSFirewall::setAnchorEnabled(QStringLiteral("110.allowNets"), true);
    MacOSFirewall::setAnchorTable(QStringLiteral("110.allowNets"), true, QStringLiteral("allownets"), m_allowedRanges);
#endif


    return true;
}

bool KillSwitch::addAllowedRange(const QStringList &ranges) {
    if (!std::all_of(ranges.cbegin(), ranges.cend(), isValidIpOrCidr)) {
        qCritical() << "IPC: invalid IP/CIDR in ranges, rejecting addAllowedRange";
        return false;
    }

    auto combinedRanges = m_allowedRanges;
    for (const QString &range : ranges) {
        if (!range.isEmpty() && !combinedRanges.contains(range)) {
            combinedRanges.append(range);
        }
    }

    return resetAllowedRange(combinedRanges);
}

bool KillSwitch::enablePeerTraffic(const QJsonObject &configStr) {
#ifdef Q_OS_WIN
    InterfaceConfig config;

    config.m_primaryDnsServer = configStr.value(amnezia::config_key::dns1).toString();

    // We don't use secondary DNS if primary DNS is AmneziaDNS
    if (!config.m_primaryDnsServer.contains(amnezia::protocols::dns::amneziaDnsIp)) {
        config.m_secondaryDnsServer = configStr.value(amnezia::config_key::dns2).toString();
    }

    config.m_serverPublicKey = "openvpn";
    config.m_serverIpv4Gateway = configStr.value("vpnGateway").toString();
    config.m_serverIpv4AddrIn = configStr.value("vpnServer").toString();
    int vpnAdapterIndex = configStr.value("vpnAdapterIndex").toInt();
    int inetAdapterIndex = configStr.value("inetAdapterIndex").toInt();

    int splitTunnelType = configStr.value("splitTunnelType").toInt();
    QJsonArray splitTunnelSites = configStr.value("splitTunnelSites").toArray();

    // Use APP split tunnel
    if (splitTunnelType == 0 || splitTunnelType == 2) {
        config.m_allowedIPAddressRanges.append(IPAddress(QHostAddress("0.0.0.0"), 0));
        config.m_allowedIPAddressRanges.append(IPAddress(QHostAddress("::"), 0));
    }

    if (splitTunnelType == 1) {
        for (auto v : splitTunnelSites) {
            QString ipRange = v.toString();
            if (ipRange.split('/').size() > 1) {
                config.m_allowedIPAddressRanges.append(
                        IPAddress(QHostAddress(ipRange.split('/')[0]), atoi(ipRange.split('/')[1].toLocal8Bit())));
            } else {
                config.m_allowedIPAddressRanges.append(IPAddress(QHostAddress(ipRange), 32));
            }
        }
    }

    config.m_excludedAddresses.append(configStr.value("vpnServer").toString());
    if (splitTunnelType == 2) {
        for (auto v : splitTunnelSites) {
            QString ipRange = v.toString();
            config.m_excludedAddresses.append(ipRange);
        }
    }

    for (const QJsonValue &i : configStr.value(amnezia::config_key::splitTunnelApps).toArray()) {
        if (!i.isString()) {
            break;
        }
        config.m_vpnDisabledApps.append(i.toString());
    }

    for (auto dns : configStr.value(amnezia::config_key::allowedDnsServers).toArray()) {
        if (!dns.isString()) {
            break;
        }
        config.m_allowedDnsServers.append(dns.toString());
    }

    WindowsFirewall *firewall = WindowsFirewall::create(this);
    if (firewall == nullptr) {
        qWarning() << "KillSwitch::enablePeerTraffic: Windows firewall is not available";
        return false;
    } else {
        // killSwitch toggle
        if (QVariant(configStr.value(amnezia::config_key::killSwitchOption).toString()).toBool()) {
            if (!firewall->enablePeerTraffic(config)) {
                qWarning() << "Could not establish peer firewall policy";
                return false;
            }
        }

        const QList<IPAddress> lanBypassRanges = getLanBypassRanges();
        if (!lanBypassRanges.isEmpty()) {
            QStringList lanBypassRangeNames;
            lanBypassRangeNames.reserve(lanBypassRanges.size());
            for (const IPAddress &range : lanBypassRanges) {
                lanBypassRangeNames.append(range.toString());
            }

            qDebug() << "KillSwitch::enablePeerTraffic: enabling LAN bypass ranges" << lanBypassRangeNames;
            if (!firewall->enableLanBypass(lanBypassRanges)) {
                qWarning() << "Could not establish LAN bypass; refusing to report a connected tunnel";
                return false;
            }
        }
    }

    WindowsDaemon::instance()->prepareActivation(config, inetAdapterIndex);
    if (!WindowsDaemon::instance()->activateSplitTunnel(config, vpnAdapterIndex)) {
        qWarning() << "Split tunnel activation failed; refusing to report a connected tunnel";
        return false;
    }

    if (vpnAdapterIndex > 0 && !config.m_vpnDisabledApps.isEmpty()) {
        if (!static_cast<WindowsDaemon*>(WindowsDaemon::instance())->appBypassActive()) {
            qWarning() << "Requested app bypass is unavailable; refusing a mixed IPv4/IPv6 policy";
            return false;
        }
        // Match the paths actually accepted by the driver. Raw settings can
        // contain removed executables or non-canonical paths after an update.
        const auto& activeApps = static_cast<WindowsDaemon*>(WindowsDaemon::instance())->activeAppBypassPaths();
        if (!firewall || !firewall->enableIpv6AppBypass(activeApps)) {
            qWarning() << "Could not establish IPv6 policy for excluded apps";
            return false;
        }
    }

    // Activate site-based exclusion routes for split tunneling (independent of killswitch)
    if (splitTunnelType == 2 && !config.m_excludedAddresses.isEmpty()) {
        static_cast<WindowsDaemon*>(WindowsDaemon::instance())->activateSiteExclusionRoutes(config.m_excludedAddresses);
    }

    // Activate geo-based exclusion routes for Russian IPs.
    // Only non-XRay protocols send CIDRs here; XRay handles geoip:ru in-core.
    {
        QJsonArray geoCidrs = configStr.value(amnezia::config_key::bypassRuSitesGeo).toArray();
        if (!geoCidrs.isEmpty()) {
            QStringList ruCidrs;
            ruCidrs.reserve(geoCidrs.size());
            for (const auto &v : geoCidrs) {
                ruCidrs.append(v.toString());
            }
            static_cast<WindowsDaemon*>(WindowsDaemon::instance())->activateGeoExclusionRoutes(ruCidrs);
        }
    }
#endif
    return true;
}

bool KillSwitch::enableKillSwitch(const QJsonObject &configStr, int vpnAdapterIndex) {
#ifdef Q_OS_WIN
    auto *firewall = WindowsFirewall::create(this);
    if (!firewall) return false;
    return firewall->enableInterface(vpnAdapterIndex, true);
#endif

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    int splitTunnelType = configStr.value("splitTunnelType").toInt();
    QJsonArray splitTunnelSites = configStr.value("splitTunnelSites").toArray();
    bool blockAll = 0;
    bool allowNets = 0;
    bool blockNets = 0;
    QStringList allownets;
    QStringList blocknets;
    QStringList allowedDnsServers;

    const QString dns1 = configStr.value(amnezia::config_key::dns1).toString();
    // Do not use secondary DNS when the primary server is AmneziaDNS.
    const QString dns2 = dns1.contains(amnezia::protocols::dns::amneziaDnsIp)
            ? QString()
            : configStr.value(amnezia::config_key::dns2).toString();

    if ((!dns1.isEmpty() && !isValidIpOrCidr(dns1))
            || (!dns2.isEmpty() && !isValidIpOrCidr(dns2))) {
        qCritical() << "IPC: invalid dns1/dns2, rejecting enableKillSwitch";
        return false;
    }

    for (const QJsonValue &dns : configStr.value(amnezia::config_key::allowedDnsServers).toArray()) {
        if (!dns.isString()) {
            break;
        }
        const QString dnsValue = dns.toString();
        if (isValidIpOrCidr(dnsValue)) {
            allowedDnsServers.append(dnsValue);
        } else if (!dnsValue.isEmpty()) {
            qWarning() << "IPC: rejected invalid allowedDnsServer:" << dnsValue;
        }
    }

    if (splitTunnelType == 0) {
        blockAll = true;
        allowNets = true;
        allownets.append(configStr.value("vpnServer").toString());
    } else if (splitTunnelType == 1) {
        blockNets = true;
        for (auto v : splitTunnelSites) {
            blocknets.append(v.toString());
        }
    } else if (splitTunnelType == 2) {
        blockAll = true;
        allowNets = true;
        allownets.append(configStr.value("vpnServer").toString());
        for (auto v : splitTunnelSites) {
            allownets.append(v.toString());
        }
    }

    if (!std::all_of(allownets.cbegin(), allownets.cend(), isValidIpOrCidr)
            || !std::all_of(blocknets.cbegin(), blocknets.cend(), isValidIpOrCidr)) {
        qCritical() << "IPC: invalid IP/CIDR in allownets/blocknets, rejecting enableKillSwitch";
        return false;
    }
#endif

#ifdef Q_OS_LINUX
    if (!LinuxFirewall::isInstalled()) {
        LinuxFirewall::install();
    }

    // double-check + ensure our firewall is installed and enabled
    LinuxFirewall::setAnchorEnabled(LinuxFirewall::Both, QStringLiteral("000.allowLoopback"), true);
    LinuxFirewall::setAnchorEnabled(LinuxFirewall::Both, QStringLiteral("100.blockAll"), blockAll);
    LinuxFirewall::setAnchorEnabled(LinuxFirewall::IPv4, QStringLiteral("110.allowNets"), allowNets);
    LinuxFirewall::updateAllowNets(allownets);
    LinuxFirewall::setAnchorEnabled(LinuxFirewall::IPv4, QStringLiteral("120.blockNets"), blockAll);
    LinuxFirewall::updateBlockNets(blocknets);
    LinuxFirewall::setAnchorEnabled(LinuxFirewall::IPv4, QStringLiteral("200.allowVPN"), true);
    LinuxFirewall::setAnchorEnabled(LinuxFirewall::IPv6, QStringLiteral("250.blockIPv6"), true);
    LinuxFirewall::setAnchorEnabled(LinuxFirewall::Both, QStringLiteral("290.allowDHCP"), true);
    LinuxFirewall::setAnchorEnabled(LinuxFirewall::Both, QStringLiteral("300.allowLAN"), true);
    LinuxFirewall::setAnchorEnabled(LinuxFirewall::IPv4, QStringLiteral("310.blockDNS"), true);
    QStringList dnsServers;

    if (!dns1.isEmpty())
        dnsServers.append(dns1);
    if (!dns2.isEmpty())
        dnsServers.append(dns2);

    dnsServers.append("127.0.0.1");
    dnsServers.append("127.0.0.53");
    dnsServers.append(allowedDnsServers);

    LinuxFirewall::updateDNSServers(dnsServers);
    LinuxFirewall::setAnchorEnabled(LinuxFirewall::IPv4, QStringLiteral("320.allowDNS"), true);
    LinuxFirewall::setAnchorEnabled(LinuxFirewall::Both, QStringLiteral("400.allowPIA"), true);
#endif

#ifdef Q_OS_MACOS
    // double-check + ensure our firewall is installed and enabled. This is necessary as
    // other software may disable pfctl before re-enabling with their own rules (e.g other VPNs)
    if (!MacOSFirewall::isInstalled())
        MacOSFirewall::install();

    MacOSFirewall::ensureRootAnchorPriority();
    MacOSFirewall::setAnchorEnabled(QStringLiteral("000.allowLoopback"), true);
    MacOSFirewall::setAnchorEnabled(QStringLiteral("100.blockAll"), blockAll);
    MacOSFirewall::setAnchorEnabled(QStringLiteral("110.allowNets"), allowNets);
    MacOSFirewall::setAnchorTable(QStringLiteral("110.allowNets"), allowNets, QStringLiteral("allownets"), allownets);

    MacOSFirewall::setAnchorEnabled(QStringLiteral("120.blockNets"), blockNets);
    MacOSFirewall::setAnchorTable(QStringLiteral("120.blockNets"), blockNets, QStringLiteral("blocknets"), blocknets);
    MacOSFirewall::setAnchorEnabled(QStringLiteral("200.allowVPN"), true);
    MacOSFirewall::setAnchorEnabled(QStringLiteral("250.blockIPv6"), true);
    MacOSFirewall::setAnchorEnabled(QStringLiteral("290.allowDHCP"), true);
    MacOSFirewall::setAnchorEnabled(QStringLiteral("300.allowLAN"), true);

    QStringList dnsServers;
    if (!dns1.isEmpty())
        dnsServers.append(dns1);
    if (!dns2.isEmpty())
        dnsServers.append(dns2);
    dnsServers.append(allowedDnsServers);

    MacOSFirewall::setAnchorEnabled(QStringLiteral("310.blockDNS"), true);
    MacOSFirewall::setAnchorTable(QStringLiteral("310.blockDNS"), true, QStringLiteral("dnsaddr"), dnsServers);
#endif
    return true;
}

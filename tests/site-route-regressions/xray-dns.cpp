#include <QtTest>
#include <QHostAddress>
#include <QFile>
#include <memory>
namespace config_key { const QString vpnproto="vpnproto", dns1="dns1", dns2="dns2", xrayRouterConfig="router"; }
namespace Proto { enum { Xray, SSXray }; }
namespace ProtocolProps { QString key_proto_config_data(int p) { return p == Proto::Xray ? "xray" : "ssxray"; } }
bool isXrayLikeProtocolName(const QString &s) { return s == "xray" || s == "ssxray"; }
struct XraySplitTunnelRuleSpec { QString outboundTag; QJsonArray domainPatterns, ipPatterns; };
struct Settings {
    enum RouteMode { VpnAllExceptSites, VpnOnlyForwardSites };
    using SiteSplitRule = XraySplitTunnelRuleSpec;
    bool enabled=false, geo=false;
    RouteMode mode=VpnAllExceptSites;
    QVector<SiteSplitRule> sites;
    RouteMode routeMode() const { return mode; }
    QVector<SiteSplitRule> getVpnSiteRules() const { return sites; }
    bool isSitesSplitTunnelingEnabled() const { return enabled; }
    bool isAutoBypassRknEnabled() const { return false; }
    bool bypassRuGeoSites() const { return false; }
    bool bypassRuGeoIp() const { return geo; }
};
QVector<XraySplitTunnelRuleSpec> buildOrderedXraySplitTunnelRules(const QVector<Settings::SiteSplitRule> &sites,
    const QHash<QString, QStringList> &) { return sites; }
struct BlocklistUpdater { static QString domainsFilePath(std::shared_ptr<Settings>) { return {}; } };
struct VpnConnection {
    QJsonObject m_vpnConfiguration;
    QHash<QString, QStringList> m_resolvedSiteIps;
    std::shared_ptr<Settings> m_settings = std::make_shared<Settings>();
    void appendXrayRoutingConfig();
};
#include "production-xray-dns.inc"
class DnsRoutingTests : public QObject {
    Q_OBJECT
    static QJsonObject config() {
        return QJsonObject{{"vpnproto","xray"},{"dns1","172.29.172.254"},{"dns2","1.1.1.1"},
            {"xray",QJsonObject{{"routing",QJsonObject{{"domainStrategy","IPIfNonMatch"},{"domainMatcher","mph"},
                {"rules",QJsonArray{QJsonObject{{"type","field"},{"outboundTag","direct"},{"network","tcp,udp"}}}}}}}}};
    }
private slots:
    void dnsPrecedesImportedCatchAllAndPreservesRoutingOptions() {
        VpnConnection connection; connection.m_vpnConfiguration=config(); connection.appendXrayRoutingConfig();
        const auto routing=connection.m_vpnConfiguration["xray"].toObject()["routing"].toObject();
        QCOMPARE(routing["domainStrategy"].toString(),QString("IPIfNonMatch"));
        QCOMPARE(routing["domainMatcher"].toString(),QString("mph"));
        const auto rules=routing["rules"].toArray(); QCOMPARE(rules.size(),2);
        const auto dns=rules[0].toObject(); QCOMPARE(dns["outboundTag"].toString(),QString("proxy"));
        QCOMPARE(dns["port"].toString(),QString("53")); QCOMPARE(dns["network"].toString(),QString("tcp,udp"));
        QCOMPARE(dns["ip"].toArray(),QJsonArray({"172.29.172.254","1.1.1.1"}));
        QCOMPARE(rules[1].toObject()["outboundTag"].toString(),QString("direct"));
    }
    void dnsPrecedesSiteDirectCatchAllAndGeoBypass() {
        VpnConnection connection; connection.m_vpnConfiguration=config();
        connection.m_settings->enabled=true; connection.m_settings->geo=true;
        connection.m_settings->mode=Settings::VpnOnlyForwardSites;
        connection.m_settings->sites={{"proxy",QJsonArray{"domain:example.test"},{}}};
        connection.appendXrayRoutingConfig();
        const auto rules=connection.m_vpnConfiguration["xray"].toObject()["routing"].toObject()["rules"].toArray();
        QCOMPARE(rules.size(),4); QCOMPARE(rules.first().toObject()["port"].toString(),QString("53"));
        QCOMPARE(rules.last().toObject()["outboundTag"].toString(),QString("direct"));
    }
    void russianIpBypassIsIndependentOfDnsAndApplicationProtocol_data() {
        QTest::addColumn<QString>("protocol");
        QTest::newRow("xray") << QString("xray");
        QTest::newRow("ssxray") << QString("ssxray");
    }
    void russianIpBypassIsIndependentOfDnsAndApplicationProtocol() {
        QFETCH(QString, protocol);
        VpnConnection connection;
        connection.m_vpnConfiguration=config();
        connection.m_vpnConfiguration["vpnproto"]=protocol;
        connection.m_vpnConfiguration.remove("dns1");
        connection.m_vpnConfiguration.remove("dns2");
        if (protocol == "ssxray") {
            connection.m_vpnConfiguration["ssxray"]=connection.m_vpnConfiguration.take("xray");
        }
        connection.m_settings->enabled=true;
        connection.m_settings->geo=true;
        // An explicit VPN-only IP exception must override automatic RU bypass.
        connection.m_settings->sites={{"proxy",{},QJsonArray{"203.0.113.7/32"}}};
        connection.appendXrayRoutingConfig();
        const auto routing=connection.m_vpnConfiguration[protocol].toObject()["routing"].toObject();
        const auto rules=routing["rules"].toArray();
        QCOMPARE(rules.size(),2);
        QCOMPARE(rules[0].toObject()["outboundTag"].toString(),QString("proxy"));
        QCOMPARE(rules[0].toObject()["ip"].toArray(),QJsonArray({"203.0.113.7/32"}));
        const auto geo=rules[1].toObject();
        QCOMPARE(geo["outboundTag"].toString(),QString("direct"));
        QCOMPARE(geo["ip"].toArray(),QJsonArray({"geoip:ru"}));
        for (const auto &restriction : {"protocol", "network", "port", "domain"}) {
            QVERIFY2(!geo.contains(restriction),restriction);
        }
        QCOMPARE(routing["domainStrategy"].toString(),QString("IPIfNonMatch"));
        connection.m_settings->geo=false;
        connection.appendXrayRoutingConfig();
        const auto disabledRules=connection.m_vpnConfiguration[protocol].toObject()["routing"].toObject()["rules"].toArray();
        for (const auto &rule : disabledRules) {
            QVERIFY(!rule.toObject()["ip"].toArray().contains("geoip:ru"));
        }
    }
    void repeatedBuildDoesNotDuplicateRulesAndOtherProtocolsAreUntouched() {
        VpnConnection connection; connection.m_vpnConfiguration=config();
        // The base configuration is restored on reconnect; verify that path.
        const auto base=connection.m_vpnConfiguration;
        connection.appendXrayRoutingConfig(); const auto first=connection.m_vpnConfiguration;
        connection.m_vpnConfiguration=base; connection.appendXrayRoutingConfig(); QCOMPARE(connection.m_vpnConfiguration,first);
        connection.m_vpnConfiguration=base; connection.m_vpnConfiguration["vpnproto"]="openvpn";
        const auto untouched=connection.m_vpnConfiguration; connection.appendXrayRoutingConfig();
        QCOMPARE(connection.m_vpnConfiguration,untouched);
    }
};
QTEST_GUILESS_MAIN(DnsRoutingTests)
#include "xray-dns.moc"

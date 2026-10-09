#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <memory>
#include "client/core/asyncSiteDnsRefresh.h"

namespace config_key { const QString vpnproto = "vpnproto"; }
namespace Vpn { enum ConnectionState { Connecting, Disconnected }; }
struct Settings {
    struct SiteSplitRule { QString hostname, ip; bool useVpn = false; };
    bool enabled = true;
    int mode = 0;
    QVector<SiteSplitRule> rules;
    bool isSitesSplitTunnelingEnabled() const { return enabled; }
    int routeMode() const { return mode; }
    QVector<SiteSplitRule> getVpnSiteRules() const { return rules; }
};
bool isXrayLikeProtocolName(const QString &name) { return name == "xray" || name == "ssxray"; }
bool isWildcardSitePattern(const QString &name) { return name.contains('*'); }
QString wildcardSitePatternToRegex(const QString &name) {
    QString result = QRegularExpression::escape(name);
    result.replace("\\*", ".*");
    return "regexp:^" + result + '$';
}
namespace NetworkUtilities {
bool checkIpSubnetFormat(const QString &text) { return QHostAddress::parseSubnet(text).second >= 0; }
}
#include "production-site-patterns.inc"

struct TestDns {
    struct Pending { int id; QString host; QPointer<QObject> owner; AsyncSiteDnsRefresh::Reply reply; };
    static inline QVector<Pending> requests;
    static inline QVector<int> aborted;
    static inline int id = 0, peak = 0;
    static int lookup(const QString &host, QObject *owner, AsyncSiteDnsRefresh::Reply reply) {
        requests.append({++id, host, owner, std::move(reply)});
        peak = qMax(peak, int(requests.size()));
        return id;
    }
    static void abort(int id) { aborted.append(id); }
    static void deliver(QStringList addresses = {"203.0.113.5", "203.0.113.6"}, bool error = false) {
        const auto request = requests.takeFirst();
        QHostInfo info;
        if (error) info.setError(QHostInfo::HostNotFound);
        QList<QHostAddress> ips;
        for (const auto &ip : addresses) ips.append(QHostAddress(ip));
        info.setAddresses(ips);
        if (request.owner) request.reply(info);
    }
    static void reset() { requests.clear(); aborted.clear(); id = peak = 0; }
};
struct TestRefresh : AsyncSiteDnsRefresh {
    TestRefresh(QObject *owner, QVector<Entry> entries, Completed completed)
        : AsyncSiteDnsRefresh(owner, entries, std::move(completed), 500, 2, TestDns::lookup, TestDns::abort) {}
};
class PreparedConnection : public QObject {
public:
    std::shared_ptr<Settings> m_settings = std::make_shared<Settings>();
    QJsonObject m_vpnConfiguration {{"vpnproto", "xray"}};
    QPointer<AsyncSiteDnsRefresh> m_siteDnsRefresh;
    QHash<QString, QStringList> m_resolvedSiteIps;
    quint64 m_routeGeneration = 1;
    bool m_shutdownRequested = false, m_cleanupPending = false;
    Vpn::ConnectionState m_connectionState = Vpn::Connecting;
    int starts = 0;
    void cancelSiteDnsRefresh();
    void prepareSiteDnsAndStart();
    void startConfiguredProtocol() { ++starts; }
};
#include "production-site-prepare.inc"

class SiteDnsTests : public QObject {
    Q_OBJECT
private slots:
    void init() { TestDns::reset(); }
    void collectsAllIpv4AndReplacesStaleSingleIpBeforeStarting() {
        PreparedConnection connection;
        connection.m_settings->rules = {{"CDN.Example.", "192.0.2.1", false},
                                        {"cdn.example", "192.0.2.2", true}};
        connection.prepareSiteDnsAndStart();
        QCOMPARE(connection.starts, 0);
        QTRY_COMPARE(TestDns::requests.size(), 1);
        TestDns::deliver({"203.0.113.5", "203.0.113.5", "2001:db8::1", "203.0.113.6"});
        QCOMPARE(connection.starts, 1);
        QCOMPARE(connection.m_resolvedSiteIps.value("cdn.example"), QStringList({"203.0.113.5", "203.0.113.6"}));
        QCOMPARE(connection.m_settings->rules[0].ip, QString("192.0.2.1"));
        const auto rules = buildOrderedXraySplitTunnelRules(connection.m_settings->rules, connection.m_resolvedSiteIps);
        QCOMPARE(rules.size(), 2);
        for (const auto &rule : rules) {
            QCOMPARE(rule.ipPatterns, QJsonArray({"203.0.113.5", "203.0.113.6"}));
            QCOMPARE(rule.domainPatterns, QJsonArray({"domain:cdn.example"}));
        }
    }
    void literalRoutesAndWildcardsAreNotResolvedOrPollutedByCachedIp() {
        PreparedConnection connection;
        connection.m_settings->rules = {{"*.example", "192.0.2.99", false},
                                        {"192.0.2.0/24", {}, false}, {"203.0.113.7", {}, true}};
        connection.prepareSiteDnsAndStart();
        QCOMPARE(connection.starts, 1);
        QVERIFY(TestDns::requests.isEmpty());
        const auto rules = buildOrderedXraySplitTunnelRules(connection.m_settings->rules);
        for (const auto &rule : rules) {
            QVERIFY(!rule.ipPatterns.contains("192.0.2.99"));
            if (!rule.domainPatterns.isEmpty()) QVERIFY(rule.ipPatterns.isEmpty());
        }
        QCOMPARE(rules.first().ipPatterns, QJsonArray({"203.0.113.7"}));
        QCOMPARE(rules[1].ipPatterns, QJsonArray({"192.0.2.0/24"}));
    }
    void partialFailureKeepsOnlyTheFailedDomainsSavedAddress() {
        PreparedConnection connection;
        connection.m_settings->rules = {{"failed.example", "192.0.2.1", false},
                                        {"good.example", "192.0.2.2", false}};
        connection.prepareSiteDnsAndStart();
        QTRY_COMPARE(TestDns::requests.size(), 2);
        TestDns::deliver({}, true);
        QCOMPARE(connection.starts, 0);
        TestDns::deliver();
        QCOMPARE(connection.starts, 1);
        QCOMPARE(connection.m_resolvedSiteIps.value("failed.example"), QStringList({"192.0.2.1"}));
        QCOMPARE(connection.m_resolvedSiteIps.value("good.example"), QStringList({"203.0.113.5", "203.0.113.6"}));
    }
    void cancellationAndOldSessionResultsCannotStartAProtocol() {
        PreparedConnection connection;
        connection.m_settings->rules = {{"cdn.example", "192.0.2.1", false}};
        connection.prepareSiteDnsAndStart();
        QTRY_COMPARE(TestDns::requests.size(), 1);
        connection.cancelSiteDnsRefresh();
        TestDns::deliver();
        QCOMPARE(connection.starts, 0);
        QVERIFY(connection.m_resolvedSiteIps.isEmpty());
        QVERIFY(!TestDns::aborted.isEmpty());
        connection.prepareSiteDnsAndStart();
        QTRY_COMPARE(TestDns::requests.size(), 1);
        ++connection.m_routeGeneration;
        TestDns::deliver();
        QCOMPARE(connection.starts, 0);
    }
    void editedSettingsRestartPreparationWithNewRules() {
        PreparedConnection connection;
        connection.m_settings->rules = {{"old.example", {}, false}};
        connection.prepareSiteDnsAndStart();
        QTRY_COMPARE(TestDns::requests.size(), 1);
        connection.m_settings->rules = {{"new.example", {}, true}};
        connection.m_settings->mode = 1;
        TestDns::deliver({"192.0.2.1"});
        QCOMPARE(connection.starts, 0);
        QTRY_COMPARE(TestDns::requests.size(), 1);
        QCOMPARE(TestDns::requests.first().host, QString("new.example"));
        TestDns::deliver({"203.0.113.8"});
        QCOMPARE(connection.starts, 1);
        QVERIFY(!connection.m_resolvedSiteIps.contains("old.example"));
    }
    void shutdownCleanupAndDisconnectedStateSuppressCompletion() {
        for (int guard = 0; guard < 3; ++guard) {
            PreparedConnection connection;
            connection.m_vpnConfiguration["vpnproto"] = "ssxray";
            connection.m_settings->rules = {{"cdn.example", {}, false}};
            connection.prepareSiteDnsAndStart();
            QTRY_COMPARE(TestDns::requests.size(), 1);
            if (guard == 0) connection.m_shutdownRequested = true;
            if (guard == 1) connection.m_cleanupPending = true;
            if (guard == 2) connection.m_connectionState = Vpn::Disconnected;
            TestDns::deliver();
            QCOMPARE(connection.starts, 0);
            QVERIFY(connection.m_resolvedSiteIps.isEmpty());
        }
    }
    void deadlineStartsOnceAndLateDnsCannotChangeSnapshot() {
        PreparedConnection connection;
        connection.m_settings->rules = {{"silent.example", "192.0.2.1", false}};
        connection.prepareSiteDnsAndStart();
        QTRY_COMPARE(TestDns::requests.size(), 1);
        QTRY_COMPARE(connection.starts, 1);
        QCOMPARE(connection.m_resolvedSiteIps.value("silent.example"), QStringList({"192.0.2.1"}));
        TestDns::deliver();
        QCOMPARE(connection.starts, 1);
        QCOMPARE(connection.m_resolvedSiteIps.value("silent.example"), QStringList({"192.0.2.1"}));
    }
    void parallelismIsBoundedAndFailedHostsDoNotCancelOthers() {
        QObject owner;
        QVector<AsyncSiteDnsRefresh::Entry> entries;
        for (int i = 0; i < 9; ++i) entries.append({QString("host%1.example").arg(i), {}});
        int completed = 0;
        auto *refresh = new AsyncSiteDnsRefresh(&owner, entries,
            [&](auto result) { ++completed; QCOMPARE(result.unresolved, 9); },
            5000, 2, TestDns::lookup, TestDns::abort);
        QTRY_COMPARE(TestDns::requests.size(), 2);
        for (int i = 0; i < 9; ++i) TestDns::deliver({}, true);
        QCOMPARE(completed, 1);
        QCOMPARE(TestDns::peak, 2);
        Q_UNUSED(refresh);
    }
    void fullTunnelAndUnsupportedProtocolsDoNotWaitForDns() {
        for (const auto &protocol : {"awg", "wireguard", "hysteria2", "anytls"}) {
            PreparedConnection connection;
            connection.m_vpnConfiguration["vpnproto"] = protocol;
            connection.m_settings->rules = {{"cdn.example", {}, false}};
            connection.prepareSiteDnsAndStart();
            QCOMPARE(connection.starts, 1);
            QVERIFY(TestDns::requests.isEmpty());
        }
        PreparedConnection connection;
        connection.m_settings->enabled = false;
        connection.prepareSiteDnsAndStart();
        QCOMPARE(connection.starts, 1);
    }
    void normalizesIdnAndRejectsInvalidDomainTargets() {
        QCOMPARE(AsyncSiteDnsRefresh::hostname("https://Example.COM/path"), QString("example.com"));
        QCOMPARE(AsyncSiteDnsRefresh::hostname(QString::fromUtf8("пример.рф")), QString("xn--e1afmkfd.xn--p1ai"));
        for (const auto &invalid : {"*.ru", "*rutube*", "192.0.2.1", "192.0.2.0/24", "a..b", "a\r\nb"})
            QVERIFY(AsyncSiteDnsRefresh::hostname(invalid).isEmpty());
    }
};
QTEST_GUILESS_MAIN(SiteDnsTests)
#include "dns-refresh.moc"

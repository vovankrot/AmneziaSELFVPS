#include <QtTest>
#include <QHostInfo>
#include <QRemoteObjectHost>
#include <QTemporaryDir>
#include "rep_service_source.h"
#include "rep_service_replica.h"
#include "client/core/asyncIpcSequence.h"

namespace Vpn { enum ConnectionState { Disconnected, Connected, Disconnecting, Error }; }
namespace amnezia { enum class ErrorCode { AmneziaServiceConnectionFailed }; }
using amnezia::ErrorCode;
class VpnProtocol : public QObject {
    Q_OBJECT
public:
    Vpn::ConnectionState state = Vpn::Connected;
    Vpn::ConnectionState connectionState() const { return state; }
    bool asyncStop = true;
    bool stopsAsynchronously() const { return asyncStop; }
    bool cleanupInProgress() const { return false; }
    bool cleanupFailed() const { return false; }
    bool isDisconnected() const { return state == Vpn::Disconnected; }
    int stops = 0;
    void stop() { ++stops; }
signals:
    void reconnectRequested();
    void protocolError(amnezia::ErrorCode);
    void connectionStateChanged(Vpn::ConnectionState);
    void stopFinished(bool);
};
class Settings {
public:
    enum RouteMode { VpnAllExceptSites, VpnOnlyForwardSites };
    QVariantMap sites;
    int saved = 0;
    QVariantMap vpnSites(RouteMode) const { return sites; }
    void addVpnSite(RouteMode, const QString &, const QString &) { ++saved; }
};
namespace NetworkUtilities {
bool checkIpSubnetFormat(const QString &text) { return QHostAddress::parseSubnet(text).second >= 0; }
}
bool isWildcardSitePattern(const QString &site) { return site.contains('*'); }
class IpcClient {
public:
    static inline QSharedPointer<IpcInterfaceReplica> iface;
    static auto InterfaceWithoutWait() { return iface; }
    template<class F> static void withInterface(F f) { if (iface && iface->isReplicaValid()) f(iface); }
};
class TestDns {
public:
    static inline QList<std::function<void(const QHostInfo &)>> pending;
    template<class F> static int lookupHost(const QString &, QObject *owner, F callback) {
        pending.append([guard = QPointer<QObject>(owner), callback](const QHostInfo &info) {
            if (guard) callback(info);
        });
        return pending.size();
    }
    static void deliver() {
        auto callbacks = pending; pending.clear();
        QHostInfo answer;
        answer.setAddresses({QHostAddress("203.0.113.5"), QHostAddress("203.0.113.5"), QHostAddress("2001:db8::1")});
        for (const auto &callback : callbacks) callback(answer);
    }
};
class VpnConnection : public QObject {
public:
    std::shared_ptr<Settings> m_settings = std::make_shared<Settings>();
    QSharedPointer<VpnProtocol> m_vpnProtocol {new VpnProtocol};
    quint64 m_routeGeneration = 1;
    Vpn::ConnectionState m_connectionState = Vpn::Connected;
    void addSitesRoutes(const QString &, Settings::RouteMode);
    void cancelSiteDnsRefresh() {}
    int reconnects = 0, warnings = 0;
    void reconnectToVpn() { ++reconnects; }
    void siteSplitTunnelingWarning(const QString &) { ++warnings; }
    void connectServiceSignals();
    void connectProtocolCleanupSignals();
    void onKillSwitchModeChanged(bool);
    bool m_cleanupPending = false, m_restoringNetwork = false, m_shutdownRequested = false;
    bool m_connectionCleanupFailed = false;
    std::function<void()> m_pendingConnect;
    void shutdown(); void requestCleanup(); void protocolStopped(bool); void cleanupCompleted(bool, const QString & = {});
    void setConnectionState(Vpn::ConnectionState state) { m_connectionState = state; }
    int shutdowns = 0, errors = 0; bool shutdownSuccess = false;
    void shutdownFinished(bool ok) { ++shutdowns; shutdownSuccess = ok; }
    void vpnProtocolError(amnezia::ErrorCode) { ++errors; }
    static QString tr(const char *text) { return QString::fromUtf8(text); }
};
#include "production-site-routes.inc"
#include "production-service-signals.inc"
#include "production-session-cleanup.inc"
#include "production-protocol-cleanup-signals.inc"
#include "production-strict-ks.inc"

class RouteSource : public IpcInterfaceSimpleSource {
public:
    int writes = 0, flushes = 0;
    bool reject = false;
    bool rejectRestore = false;
    int restores = 0, clears = 0;
    int refreshes = 0;
    bool refreshKillSwitch(bool) override { ++refreshes; return !reject; }
    bool restoreResolvers() override { ++restores; return !rejectRestore; }
    bool clearSavedRoutes() override { ++clears; return true; }
    int routeAddList(const QString &, const QStringList &ips) override {
        ++writes; return reject ? 0 : ips.size();
    }
    bool flushDns() override { ++flushes; return true; }
};
class SiteRouteTests : public QObject {
    Q_OBJECT
private slots:
    void strictKillSwitchUpdateIsAsyncAndReportsServiceRejection() {
        QTemporaryDir dir; QRemoteObjectHost host(QUrl("local:" + dir.path() + "/strict-ks"));
        RouteSource source; QVERIFY(host.enableRemoting(&source));
        QRemoteObjectNode node; QVERIFY(node.connectToNode(host.hostUrl()));
        IpcClient::iface.reset(node.acquire<IpcInterfaceReplica>()); QTRY_VERIFY(IpcClient::iface->isReplicaValid());
        VpnConnection connection;
        connection.onKillSwitchModeChanged(true);
        QCOMPARE(source.refreshes,0); // No nested wait or synchronous service request.
        QTRY_COMPARE(source.refreshes,1); QCOMPARE(connection.errors,0);
        source.reject=true; connection.onKillSwitchModeChanged(false);
        QTRY_COMPARE(connection.errors,1);
        IpcClient::iface.reset(); connection.onKillSwitchModeChanged(false); QCOMPARE(connection.errors,2);
    }
    void transitionRetainsProtocolUntilNetworkRestoredAndBlocksFailedReconnect() {
        QTemporaryDir dir; QRemoteObjectHost host(QUrl("local:" + dir.path() + "/cleanup"));
        RouteSource source; QVERIFY(host.enableRemoting(&source));
        QRemoteObjectNode node; QVERIFY(node.connectToNode(host.hostUrl()));
        IpcClient::iface.reset(node.acquire<IpcInterfaceReplica>()); QTRY_VERIFY(IpcClient::iface->isReplicaValid());
        VpnConnection connection;
        QPointer<VpnProtocol> old = connection.m_vpnProtocol.data();
        int starts = 0;
        connection.m_pendingConnect = [&] { ++starts; };
        connection.requestCleanup(); connection.requestCleanup();
        QCOMPARE(old->stops, 1); QVERIFY(connection.m_vpnProtocol); QCOMPARE(starts, 0);
        source.rejectRestore = true; old->state = Vpn::Disconnected;
        connection.protocolStopped(true); connection.protocolStopped(true);
        QTRY_VERIFY(!connection.m_cleanupPending);
        QCOMPARE(connection.m_connectionState, Vpn::Error); QVERIFY(connection.m_vpnProtocol);
        QCOMPARE(starts, 0); QCOMPARE(source.restores, 1); QCOMPARE(source.clears, 0);
        source.rejectRestore = false; connection.m_pendingConnect = [&] { ++starts; };
        connection.requestCleanup();
        QTRY_COMPARE(starts, 1); QVERIFY(!connection.m_vpnProtocol);
        QCOMPARE(connection.m_connectionState, Vpn::Disconnected); QCOMPARE(source.clears, 1);
        IpcClient::iface.reset();
    }
    void nativeStopWaitsForTerminalSignalBeforeNetworkRestoration() {
        QTemporaryDir dir; QRemoteObjectHost host(QUrl("local:" + dir.path() + "/native-cleanup"));
        RouteSource source; QVERIFY(host.enableRemoting(&source));
        QRemoteObjectNode node; QVERIFY(node.connectToNode(host.hostUrl()));
        IpcClient::iface.reset(node.acquire<IpcInterfaceReplica>()); QTRY_VERIFY(IpcClient::iface->isReplicaValid());
        VpnConnection connection;
        connection.m_vpnProtocol->asyncStop = false;
        connection.connectProtocolCleanupSignals();
        auto old = connection.m_vpnProtocol;
        int starts = 0; connection.m_pendingConnect = [&] { ++starts; };
        connection.requestCleanup();
        QTest::qWait(30);
        QCOMPARE(old->stops, 1); QCOMPARE(source.restores, 0); QCOMPARE(starts, 0);
        QVERIFY(connection.m_cleanupPending); QCOMPARE(connection.m_connectionState, Vpn::Disconnecting);
        old->state = Vpn::Disconnected; emit old->connectionStateChanged(Vpn::Disconnected);
        QTRY_COMPARE(starts, 1); QCOMPARE(source.clears, 1); QVERIFY(!connection.m_vpnProtocol);
        // A stale protocol notification cannot restart cleanup for a replacement.
        emit old->connectionStateChanged(Vpn::Error);
        QTest::qWait(30); QCOMPARE(source.restores, 1); QVERIFY(!connection.m_cleanupPending);
        IpcClient::iface.reset();
    }
    void nativeStopFailureRetainsProtocolAndBlocksRestart() {
        VpnConnection connection;
        connection.m_vpnProtocol->asyncStop = false;
        connection.connectProtocolCleanupSignals();
        int starts = 0; connection.m_pendingConnect = [&] { ++starts; };
        connection.requestCleanup();
        connection.m_vpnProtocol->state = Vpn::Error;
        emit connection.m_vpnProtocol->connectionStateChanged(Vpn::Error);
        QCOMPARE(starts, 0); QVERIFY(connection.m_vpnProtocol);
        QVERIFY(connection.m_connectionCleanupFailed); QVERIFY(!connection.m_cleanupPending);
        QCOMPARE(connection.m_connectionState, Vpn::Error);
    }
    void shutdownFinishesOnlyAfterCleanupAndReportsFailure() {
        VpnConnection connection;
        IpcClient::iface.reset();
        connection.shutdown(); QCOMPARE(connection.shutdowns, 0);
        connection.protocolStopped(false);
        QCOMPARE(connection.shutdowns, 1); QVERIFY(!connection.shutdownSuccess);
        QCOMPARE(connection.m_connectionState, Vpn::Error); QVERIFY(connection.m_vpnProtocol);
    }
    void repeatedSessionsDoNotMultiplyServiceSubscriptions() {
        QTemporaryDir dir;
        QRemoteObjectHost host(QUrl("local:" + dir.path() + "/signals"));
        RouteSource source;
        QVERIFY(host.enableRemoting(&source));
        QRemoteObjectNode node;
        QVERIFY(node.connectToNode(host.hostUrl()));
        IpcClient::iface.reset(node.acquire<IpcInterfaceReplica>());
        QTRY_VERIFY(IpcClient::iface->isReplicaValid());
        VpnConnection connection;
        for (int i = 0; i < 20; ++i) connection.connectServiceSignals();
        emit source.networkChanged();
        QTRY_COMPARE(connection.reconnects, 1);
        emit source.wakeup();
        QTRY_COMPARE(connection.reconnects, 2);
        emit source.networkPolicyWarning("test warning");
        QTRY_COMPARE(connection.warnings, 1);
        IpcClient::iface.reset();
    }
    void lateDnsCannotModifyDisconnectedOrReplacementSession() {
        QTemporaryDir dir;
        QRemoteObjectHost host(QUrl("local:" + dir.path() + "/routes"));
        RouteSource source;
        QVERIFY(host.enableRemoting(&source));
        QRemoteObjectNode node;
        QVERIFY(node.connectToNode(host.hostUrl()));
        IpcClient::iface.reset(node.acquire<IpcInterfaceReplica>());
        QTRY_VERIFY(IpcClient::iface->isReplicaValid());
        VpnConnection connection;
        connection.m_settings->sites.insert("example.com", "");
        auto lookup = [&] { connection.addSitesRoutes("192.0.2.1", Settings::VpnAllExceptSites); };
        lookup();
        connection.m_connectionState = Vpn::Disconnected;
        TestDns::deliver();
        QTest::qWait(30);
        QCOMPARE(source.writes, 0);
        QCOMPARE(connection.m_settings->saved, 0);

        connection.m_connectionState = Vpn::Connected;
        lookup();
        connection.m_vpnProtocol->state = Vpn::Disconnected;
        TestDns::deliver();
        QTest::qWait(30);
        QCOMPARE(source.writes, 0);
        connection.m_vpnProtocol->state = Vpn::Connected;

        connection.m_connectionState = Vpn::Connected;
        lookup();
        ++connection.m_routeGeneration;
        TestDns::deliver();
        QTest::qWait(30);
        QCOMPARE(source.writes, 0);

        lookup();
        connection.m_vpnProtocol.reset(new VpnProtocol);
        TestDns::deliver();
        QTest::qWait(30);
        QCOMPARE(source.writes, 0);

        lookup();
        TestDns::deliver();
        // DNS passed, but cancellation happens before the first queued IPC step.
        ++connection.m_routeGeneration;
        QTest::qWait(30);
        QCOMPARE(source.writes, 0);

        lookup();
        TestDns::deliver();
        QTRY_COMPARE(connection.m_settings->saved, 1);
        QCOMPARE(source.writes, 1);
        QCOMPARE(source.flushes, 1);

        source.reject = true;
        lookup(); TestDns::deliver();
        QTRY_COMPARE(source.writes, 2);
        QTest::qWait(30);
        QCOMPARE(connection.m_settings->saved, 1);
        QCOMPARE(source.flushes, 1);
        IpcClient::iface.reset();
    }
    void destroyingConnectionSuppressesAlreadyQueuedRouting() {
        QTemporaryDir dir;
        QRemoteObjectHost host(QUrl("local:" + dir.path() + "/deleted"));
        RouteSource source;
        QVERIFY(host.enableRemoting(&source));
        QRemoteObjectNode node;
        QVERIFY(node.connectToNode(host.hostUrl()));
        IpcClient::iface.reset(node.acquire<IpcInterfaceReplica>());
        QTRY_VERIFY(IpcClient::iface->isReplicaValid());
        auto *connection = new VpnConnection;
        // Keep the old protocol alive to exercise the QPointer connection guard.
        const auto retained = connection->m_vpnProtocol;
        connection->m_settings->sites.insert("example.com", "");
        connection->addSitesRoutes("192.0.2.1", Settings::VpnOnlyForwardSites);
        TestDns::deliver();
        delete connection;
        QTest::qWait(50);
        QCOMPARE(source.writes, 0);
        QCOMPARE(source.flushes, 0);
        IpcClient::iface.reset();
    }
};
QTEST_GUILESS_MAIN(SiteRouteTests)
#include "main.moc"

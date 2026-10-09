#include <QtTest>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRemoteObjectHost>
#include <QTimer>
#include <QPointer>
#include <QUuid>
#include <QSet>
#include "../../ipc/clientSessionLease.h"
#include "rep_process_source.h"
#include "rep_process_replica.h"

namespace amnezia {
QString prefix;
QString getIpcProcessUrl(int id) { return prefix + QString::number(id); }
// Peer authorization has its own production security tests. Only lifecycle
// is exercised here, over local IPC, with no privileged child execution.
bool authorizeLocalPeer(QLocalSocket *) { return true; }
QString localPeerProcessIdentity(QLocalSocket *) { return "fixture-owner"; }
}
class IpcServerProcess : public ProcessSimpleSource {
    Q_OBJECT
public:
    explicit IpcServerProcess(QObject *parent = nullptr) : ProcessSimpleSource(parent) {}
    bool running = false;
    int kills = 0;
    void kill() { ++kills; QTimer::singleShot(0, this, [this] { finish(); }); }
    bool isRunning() const { return running; }
    void close() override { running = false; emit releaseRequested(); }
    void finish() { running = false; emit finished(0, QProcess::NormalExit); }
signals:
    void releaseRequested();
    void finished(int code, QProcess::ExitStatus status);
};
class Daemon {
public:
    static inline Daemon* current = nullptr;
    bool active = false;
    static Daemon* instance(){return current;}
    bool hasActiveTunnel() const {return active;}
};
class IpcServer : public QObject {
public:
    bool hasOwnedResources() const;
    struct LeaseFixture {
        bool available() const { return true; }
        QString identity() const { return "fixture-owner"; }
        bool attach(QLocalSocket *, const QString &) { return true; }
    } m_ownerLease;
    bool acceptClient(QLocalSocket *socket);
    void recoverOwner(std::function<void(bool)> completed);
    void recoverOwnerAfterHelpers(std::function<void(bool)> completed, int remainingPolls);
    std::function<bool()> recoverNativeTunnel;
    bool m_ownedPolicy = false, m_ownedDns = false, m_ownedIpv6 = false, m_ownedXray = false, m_ownedRoutes = false;
    QSet<QString> m_ownedDevices;
    int createPrivilegedProcess();
    void releasePrivilegedProcess(int id, bool explicitlyClosed = false);
    int m_localpid = 0;
    struct ProcessDescriptor {
        explicit ProcessDescriptor(QObject *parent = nullptr)
            : ipcProcess(new IpcServerProcess(parent)), serverNode(new QRemoteObjectHost(parent)),
              localServer(new QLocalServer(parent)) {}
        QSharedPointer<IpcServerProcess> ipcProcess;
        QSharedPointer<QRemoteObjectHost> serverNode;
        QSharedPointer<QLocalServer> localServer;
        int connectedPeers = 0;
        bool remotingEnabled = false;
        bool closing = false;
        QString ownerIdentity;
        quint64 peerGeneration = 0;
    };
    QMap<int, ProcessDescriptor> m_processes;
};
struct Router {
    static inline bool dnsSuccess = true;
    static bool StartRoutingIpv6() { return true; }
    static bool restoreResolvers() { return dnsSuccess; }
    static bool clearSavedRoutes() { return true; }
    static bool deleteTun(const QString &) { return true; }
};
struct KillSwitch {
    static KillSwitch *instance() { static KillSwitch instance; return &instance; }
    bool disableKillSwitch() { return true; }
};
struct Xray {
    static Xray &getInstance() { static Xray instance; return instance; }
    bool stopXray() { return true; }
};
#include "production-process-lifetime.inc"

class LifetimeTests : public QObject {
    Q_OBJECT
private slots:
    void init() { amnezia::prefix = "selfvps-lifetime-" + QUuid::createUuid().toString(QUuid::Id128) + '-'; }
    void explicitCloseReclaimsDescriptorAndBreaksCycles() {
        IpcServer server;
        const int id = server.createPrivilegedProcess();
        QVERIFY(id > 0);
        QPointer<IpcServerProcess> process = server.m_processes[id].ipcProcess.data();
        QPointer<QRemoteObjectHost> host = server.m_processes[id].serverNode.data();
        QPointer<QLocalServer> listener = server.m_processes[id].localServer.data();
        QRemoteObjectNode node;
        QVERIFY(node.connectToNode(QUrl("local:" + amnezia::getIpcProcessUrl(id))));
        QScopedPointer<ProcessReplica> replica(node.acquire<ProcessReplica>());
        QTRY_VERIFY(replica->isInitialized());
        QCOMPARE(server.m_processes[id].connectedPeers, 1);
        replica->close();
        QTRY_VERIFY(server.m_processes.isEmpty());
        QVERIFY(!process && !host && !listener);
    }
    void disconnectedRunningChildIsKilledAfterGrace() {
        IpcServer server;
        const int id = server.createPrivilegedProcess();
        QPointer<IpcServerProcess> process = server.m_processes[id].ipcProcess.data();
        process->running = true;
        {
            QRemoteObjectNode node;
            QVERIFY(node.connectToNode(QUrl("local:" + amnezia::getIpcProcessUrl(id))));
            QScopedPointer<ProcessReplica> replica(node.acquire<ProcessReplica>());
            QTRY_VERIFY(replica->isInitialized());
        }
        QTRY_VERIFY(!server.m_processes.contains(id) || server.m_processes.value(id).connectedPeers == 0);
        QTRY_VERIFY(server.m_processes.isEmpty());
        QVERIFY(!process);
    }
    void recoveryKillsOnlyOwnedChildAndRetainsFailedResources() {
        IpcServer server;
        const int own = server.createPrivilegedProcess(), other = server.createPrivilegedProcess();
        server.m_processes[own].ipcProcess->running = true;
        server.m_processes[other].ipcProcess->running = true;
        server.m_processes[other].ownerIdentity = "another-owner";
        server.m_processes[other].connectedPeers = 1;
        QPointer<IpcServerProcess> otherProcess = server.m_processes[other].ipcProcess.data();
        server.m_ownedDns = true; Router::dnsSuccess = false;
        int callbacks = 0; bool success = true;
        server.recoverOwner([&](bool ok) { success = ok; ++callbacks; });
        QTRY_COMPARE(callbacks, 1); QVERIFY(!success); QVERIFY(server.m_ownedDns);
        QVERIFY(otherProcess && otherProcess->running); QCOMPARE(otherProcess->kills, 0);
        Router::dnsSuccess = true;
        server.recoverOwner([&](bool ok) { success = ok; ++callbacks; });
        QTRY_COMPARE(callbacks, 2); QVERIFY(success); QVERIFY(!server.m_ownedDns);
        QVERIFY(otherProcess && otherProcess->running); QCOMPARE(otherProcess->kills, 0);
        otherProcess->running = false;
    }
    void leaseBlocksNewProcessDuringGraceAndFailedRecovery() {
        QLocalServer listener; QVERIFY(listener.listen(amnezia::prefix + "lease"));
        QLocalSocket first; first.connectToServer(listener.serverName());
        QTRY_VERIFY(listener.hasPendingConnections());
        auto *firstPeer = listener.nextPendingConnection();
        int recoveries = 0;
        std::function<void(bool)> finish;
        ClientSessionLease lease(this, [&](auto done) { ++recoveries; finish = std::move(done); }, 40);
        QVERIFY(lease.attach(firstPeer, "pid:creation-one"));
        QLocalSocket second; second.connectToServer(listener.serverName());
        QTRY_VERIFY(listener.hasPendingConnections());
        auto *secondPeer = listener.nextPendingConnection();
        QVERIFY(!lease.attach(secondPeer, "pid:creation-two"));
        first.abort(); QTRY_COMPARE(recoveries, 1);
        QVERIFY(!lease.attach(secondPeer, "pid:creation-one"));
        finish(false); QTRY_COMPARE(recoveries, 2);
        QVERIFY(!lease.attach(secondPeer, "pid:creation-two"));
        finish(true);
        QVERIFY(lease.attach(secondPeer, "pid:creation-two"));
    }
    void idleProbeReleasesOwnershipOnlyAfterLastChannelCloses() {
        QLocalServer listener; QVERIFY(listener.listen(amnezia::prefix + "idle-probe"));
        QLocalSocket probe, probeExtra, gui;
        probe.connectToServer(listener.serverName()); QTRY_VERIFY(listener.hasPendingConnections());
        auto *probePeer = listener.nextPendingConnection();
        probeExtra.connectToServer(listener.serverName()); QTRY_VERIFY(listener.hasPendingConnections());
        auto *extraPeer = listener.nextPendingConnection();
        gui.connectToServer(listener.serverName()); QTRY_VERIFY(listener.hasPendingConnections());
        auto *guiPeer = listener.nextPendingConnection();
        int recoveries = 0;
        ClientSessionLease lease(this, [&](auto completed) { ++recoveries; completed(true); }, 30000, [] { return true; });
        QVERIFY(lease.attach(probePeer, "health-probe"));
        QVERIFY(lease.attach(extraPeer, "health-probe"));
        QVERIFY(!lease.attach(guiPeer, "gui"));
        probe.abort(); QTest::qWait(20);
        QVERIFY(!lease.attach(guiPeer, "gui"));
        probeExtra.abort(); QTRY_VERIFY(lease.identity().isEmpty());
        QVERIFY(lease.attach(guiPeer, "gui"));
        QCOMPARE(recoveries, 0); // No global network cleanup from a read-only probe.
    }
    void idleCheckRetainsEveryOwnedResourceAndUnknownNativeState() {
        IpcServer server; Daemon daemon;
        QVERIFY(server.hasOwnedResources()); // Native state unknown.
        Daemon::current = &daemon;
        QVERIFY(!server.hasOwnedResources());
        daemon.active = true; QVERIFY(server.hasOwnedResources()); daemon.active = false;
        for (bool* flag : {&server.m_ownedPolicy,&server.m_ownedDns,&server.m_ownedIpv6,&server.m_ownedXray,&server.m_ownedRoutes}) {
            *flag = true; QVERIFY(server.hasOwnedResources()); *flag = false;
        }
        server.m_ownedDevices.insert("owned-tun"); QVERIFY(server.hasOwnedResources()); server.m_ownedDevices.clear();
        const int id = server.createPrivilegedProcess(); QVERIFY(id >= 0); QVERIFY(server.hasOwnedResources());
        server.releasePrivilegedProcess(id,true); QVERIFY(!server.hasOwnedResources());
        Daemon::current = nullptr;
    }
    void connectedIdleEndpointCanBeReusedAndAbandonedEndpointIsReclaimed() {
        IpcServer server;
        const int id = server.createPrivilegedProcess();
        QRemoteObjectNode node;
        QVERIFY(node.connectToNode(QUrl("local:" + amnezia::getIpcProcessUrl(id))));
        QScopedPointer<ProcessReplica> replica(node.acquire<ProcessReplica>());
        QTRY_VERIFY(replica->isInitialized());
        QTest::qWait(80);
        QCOMPARE(server.m_processes.size(), 1);
        const int abandoned = server.createPrivilegedProcess();
        QTRY_VERIFY(!server.m_processes.contains(abandoned));
        QVERIFY(server.m_processes.contains(id));
        replica->close();
        QTRY_VERIFY(server.m_processes.isEmpty());
    }
    void listenerFailureDoesNotRetainObjects() {
        // Windows permits several instances of the same named pipe, so an
        // occupied name is not a listener failure. Exceed the OS name limit.
        const auto prefix = amnezia::prefix;
        amnezia::prefix = QString(10000, 'x');
        IpcServer invalid;
        QCOMPARE(invalid.createPrivilegedProcess(), -1);
        QVERIFY(invalid.m_processes.isEmpty());
        QVERIFY(invalid.children().isEmpty());
        amnezia::prefix = prefix;
        IpcServer replacement;
        QVERIFY(replacement.createPrivilegedProcess() > 0);
        QTRY_VERIFY(replacement.m_processes.isEmpty());
    }
};
QTEST_GUILESS_MAIN(LifetimeTests)
#include "main.moc"

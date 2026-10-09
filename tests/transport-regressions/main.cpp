#include <QtTest>
#include <QRemoteObjectHost>
#include <QTemporaryDir>
#include <QTimer>
#include "rep_ipc_process_interface_source.h"
#include "client/core/tun2socksOutputReader.h"
#include "client/core/defaultRouteSelection.h"
#include "client/core/asyncIpcSequence.h"
#include "client/core/asyncProcessRequest.h"
#include "client/core/asyncTunnelStop.h"
#include "client/core/asyncSocksProbe.h"
#include "client/core/tun2socksProcessObserver.h"
#include "rep_ipc_interface_source.h"
#include "client/core/socksRoutingSetup.h"
#include "client/core/tunnelDataProbe.h"
#include "client/core/cdnRecoveryPolicy.h"
#include <QtConcurrent/QtConcurrentRun>
#include <QTcpServer>
#include <QSslKey>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

struct TestCertificate {
    QSslCertificate certificate;
    QSslKey privateKey;
    TestCertificate() {
        // An ephemeral localhost-only test key; never persisted or packaged.
        auto *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        EVP_PKEY *key = nullptr;
        EVP_PKEY_keygen_init(ctx);
        EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048);
        EVP_PKEY_keygen(ctx, &key);
        EVP_PKEY_CTX_free(ctx);
        auto *cert = X509_new();
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), -60);
        X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
        X509_set_pubkey(cert, key);
        auto *name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                  reinterpret_cast<const unsigned char *>("localhost"), -1, -1, 0);
        X509_set_issuer_name(cert, name);
        for (auto extension : {std::make_pair(NID_subject_alt_name, "DNS:localhost"),
                               std::make_pair(NID_basic_constraints, "critical,CA:TRUE")}) {
            auto *ext = X509V3_EXT_conf_nid(nullptr, nullptr, extension.first, const_cast<char *>(extension.second));
            X509_add_ext(cert, ext, -1);
            X509_EXTENSION_free(ext);
        }
        X509_sign(cert, key, EVP_sha256());
        auto *bio = BIO_new(BIO_s_mem());
        PEM_write_bio_X509(bio, cert);
        char *data = nullptr;
        auto size = BIO_get_mem_data(bio, &data);
        certificate = QSslCertificate(QByteArray(data, size));
        BIO_reset(bio);
        PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
        size = BIO_get_mem_data(bio, &data);
        privateKey = QSslKey(QByteArray(data, size), QSsl::Rsa);
        BIO_free(bio); X509_free(cert); EVP_PKEY_free(key);
    }
};

class TestHttpsServer : public QTcpServer {
public:
    QSslConfiguration ssl;
    QByteArray response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    QByteArray request;
    bool silent = false;
    void incomingConnection(qintptr descriptor) override {
        auto *socket = new QSslSocket(this);
        socket->setSocketDescriptor(descriptor);
        socket->setSslConfiguration(ssl);
        socket->setPeerVerifyMode(QSslSocket::VerifyNone); // server does not require a client certificate
        connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QSslSocket::readyRead, socket, [this, socket] {
            request += socket->readAll();
            if (!silent) { socket->write(response); socket->disconnectFromHost(); }
        });
        socket->startServerEncryption();
    }
};

// Local SOCKS relay: assert that a CDN probe uses the selected numeric IP,
// rather than asking the proxy to resolve the hostname to a different server.
class TestSocksRelay : public QTcpServer {
public:
    QByteArray destination, tlsBytes;
    void incomingConnection(qintptr descriptor) override {
        auto *peer = new QTcpSocket(this);
        peer->setSocketDescriptor(descriptor);
        auto *upstream = new QTcpSocket(peer);
        auto buffer = std::make_shared<QByteArray>();
        auto stage = std::make_shared<int>(0);
        connect(peer, &QTcpSocket::disconnected, peer, &QObject::deleteLater);
        connect(upstream, &QTcpSocket::readyRead, peer, [peer, upstream] {
            peer->write(upstream->readAll());
        });
        connect(upstream, &QTcpSocket::disconnected, peer, &QTcpSocket::disconnectFromHost);
        connect(upstream, &QTcpSocket::connected, peer, [peer, buffer, stage] {
            peer->write(QByteArray::fromHex("050000017f0000010000"));
            *stage = 2;
        });
        connect(peer, &QTcpSocket::readyRead, peer, [this, peer, upstream, buffer, stage] {
            *buffer += peer->readAll();
            if (*stage == 0) {
                if (buffer->size() < 2 || buffer->size() < 2 + quint8(buffer->at(1))) return;
                buffer->remove(0, 2 + quint8(buffer->at(1)));
                peer->write(QByteArray::fromHex("0500"));
                *stage = 1;
            }
            if (*stage == 1) {
                if (buffer->size() < 10) return;
                destination = buffer->left(10);
                if (quint8(destination.at(3)) != 1) { peer->abort(); return; }
                quint32 ip = 0;
                for (int i = 4; i < 8; ++i) ip = (ip << 8) | quint8(destination.at(i));
                const quint16 port = (quint16(quint8(destination.at(8))) << 8) | quint8(destination.at(9));
                buffer->remove(0, 10);
                *stage = 3;
                upstream->connectToHost(QHostAddress(ip), port);
            }
            if (*stage == 2) {
                tlsBytes += *buffer;
                upstream->write(*buffer);
                buffer->clear();
            }
        });
    }
};

class OutputSource : public IpcProcessInterfaceSimpleSource {
public:
    QByteArray bytes;
    int reads = 0;
    int delayMs = 0;
    bool running = false, ignoreKill = false;
    int kills = 0, closes = 0;
    void kill() override { ++kills; if (!ignoreKill) QTimer::singleShot(150, this, [this] { running = false; }); }
    void close() override { ++closes; }
    bool waitForFinished(int) override { return !running; }
    QByteArray readAllStandardOutput() override {
        if (delayMs) QThread::msleep(delayMs);
        ++reads;
        QByteArray result; result.swap(bytes); return result;
    }
    void append(const QByteArray &chunk) { bytes += chunk; emit readyReadStandardOutput(); }
};

class RoutingSource : public IpcInterfaceSimpleSource {
public:
    QStringList calls;
    QString fail;
    int processId = 7;
    int processRequests = 0;
    int createPrivilegedProcess() override { ++processRequests; return processId; }
    bool record(const QString &name) { calls.append(name); return fail != name; }
    bool createTun(const QString &, const QString &) override { return record("createTun"); }
    bool updateResolvers(const QString &, const QList<QHostAddress> &) override { return record("updateResolvers"); }
    bool enableKillSwitch(const QJsonObject &, int) override { return record("enableKillSwitch"); }
    int routeAddList(const QString &, const QStringList &ips) override {
        return record(ips.size() == 1 ? "excludeServer" : "routeAddList") ? ips.size() : 0;
    }
    bool StopRoutingIpv6() override { return record("StopRoutingIpv6"); }
    bool StartRoutingIpv6() override { return record("StartRoutingIpv6"); }
    bool enablePeerTraffic(const QJsonObject &) override { return record("enablePeerTraffic"); }
    bool disableKillSwitch() override { return record("disableKillSwitch"); }
    bool restoreResolvers() override { return record("restoreResolvers"); }
    bool deleteTun(const QString &) override { return record("deleteTun"); }
    bool xrayStop() override { return record("xrayStop"); }
};

class TransportTests : public QObject {
    Q_OBJECT
private slots:
    void cdnPolicyRejectsLanAndRequiresRepeatedPairedEvidence() {
        using namespace CdnRecovery;
        for (const auto &ip : {"0.0.0.0", "10.0.0.1", "127.0.0.1", "192.168.0.1",
                              "172.31.0.1", "100.64.0.1", "169.254.1.1", "192.0.2.1",
                              "198.18.0.1", "198.51.100.1", "203.0.113.1", "224.0.0.1",
                              "255.255.255.255", "::1", "::ffff:8.8.8.8"}) {
            QVERIFY(!publicIpv4(QHostAddress(ip)));
            Policy rejected(1, QHostAddress(ip), 443);
            for (int i = 0; i < 4; ++i)
                QCOMPARE(rejected.observe(1, i * 10000, Probe::Timeout, Probe::Success), Change::None);
        }
        Policy policy(1, QHostAddress("8.8.8.8"), 443);
        QCOMPARE(policy.observe(1, 0, Probe::Timeout, Probe::Success), Change::None);
        QCOMPARE(policy.observe(1, 1000, Probe::Timeout, Probe::Success), Change::None);
        QCOMPARE(policy.observe(1, 10000, Probe::Timeout, Probe::OtherFailure), Change::None);
        QCOMPARE(policy.observe(1, 20000, Probe::OtherFailure, Probe::Success), Change::None);
        QCOMPARE(policy.observe(1, 30000, Probe::Timeout, Probe::Success), Change::None);
        QCOMPARE(policy.observe(2, 40000, Probe::Timeout, Probe::Success), Change::None);
        QCOMPARE(policy.observe(1, 40000, Probe::Timeout, Probe::Success), Change::None);
        QCOMPARE(policy.observe(1, 50000, Probe::Timeout, Probe::Success), Change::Install);
        QVERIFY(!policy.active()); // a decision is not an installed rule
        policy.acknowledge(1, Change::Install, false, 50000);
        QVERIFY(policy.drained());
        QCOMPARE(policy.observe(1, 120000, Probe::Timeout, Probe::Success), Change::None);
        QCOMPARE(policy.observe(1, 200000, Probe::Timeout, Probe::Success), Change::None);
        QCOMPARE(policy.observe(1, 210000, Probe::Timeout, Probe::Success), Change::None);
        QCOMPARE(policy.observe(1, 220000, Probe::Timeout, Probe::Success), Change::Install);
    }
    void cdnLeaseWaitsForCleanupAndIgnoresPreviousSession() {
        using namespace CdnRecovery;
        Policy policy(7, QHostAddress("8.8.4.4"), 443);
        for (int i = 0; i < 3; ++i) policy.observe(7, i * 10000, Probe::Timeout, Probe::Success);
        QCOMPARE(policy.stop(), Change::None); // install reply still outstanding
        QCOMPARE(policy.acknowledge(6, Change::Install, true, 20000), Change::None);
        QVERIFY(!policy.drained());
        QCOMPARE(policy.acknowledge(7, Change::Install, true, 20000), Change::Remove);
        QVERIFY(policy.active());
        QCOMPARE(policy.acknowledge(7, Change::Remove, false, 30000), Change::None);
        QVERIFY(!policy.drained()); // retain ownership after failed deletion
        QCOMPARE(policy.stop(), Change::Remove);
        policy.acknowledge(7, Change::Remove, true, 40000);
        QVERIFY(policy.drained());
        QCOMPARE(policy.observe(7, 100000, Probe::Timeout, Probe::Success), Change::None);
    }
    void cdnRecoveryUsesHoldTimeSuccessStreakAndHardExpiry() {
        using namespace CdnRecovery;
        Policy policy(1, QHostAddress("8.8.8.8"), 443);
        for (int i = 0; i < 3; ++i) policy.observe(1, i * 10000, Probe::Timeout, Probe::Success);
        policy.acknowledge(1, Change::Install, true, 20000);
        for (int i = 3; i < 6; ++i)
            QCOMPARE(policy.observe(1, i * 10000, Probe::Success, Probe::Success), Change::None);
        QCOMPARE(policy.observe(1, 130000, Probe::Success, Probe::Success), Change::None);
        QCOMPARE(policy.observe(1, 140000, Probe::Timeout, Probe::Success), Change::None);
        QCOMPARE(policy.observe(1, 150000, Probe::Success, Probe::Success), Change::None);
        QCOMPARE(policy.observe(1, 160000, Probe::Success, Probe::Success), Change::None);
        QCOMPARE(policy.observe(1, 170000, Probe::Success, Probe::Success), Change::Remove);
        policy.acknowledge(1, Change::Remove, false, 170000);
        QVERIFY(policy.active());
        QCOMPARE(policy.expire(2, 999999), Change::None);
        QCOMPARE(policy.expire(1, 619999), Change::None);
        QCOMPARE(policy.expire(1, 620000), Change::Remove);
        policy.acknowledge(1, Change::Remove, true, 620000);
        QVERIFY(policy.drained());
    }
    void pinnedCdnProbePreservesTlsIdentityAndNumericSocksDestination() {
        TestCertificate cert;
        TestHttpsServer server;
        server.ssl = QSslConfiguration::defaultConfiguration();
        server.ssl.setLocalCertificate(cert.certificate);
        server.ssl.setPrivateKey(cert.privateKey);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        TestSocksRelay relay;
        QVERIFY(relay.listen(QHostAddress::LocalHost));
        auto trusted = QSslConfiguration::defaultConfiguration();
        trusted.setCaCertificates({cert.certificate});
        const auto proxy = QNetworkProxy(QNetworkProxy::Socks5Proxy, "127.0.0.1", relay.serverPort());
        const auto probe = [&](QString name, QSslConfiguration ssl) {
            auto future = QtConcurrent::run([&, name, ssl] {
                return TunnelDataProbe::runAtAddress(name, QHostAddress::LocalHost,
                                                     server.serverPort(), proxy, 3000, {}, ssl);
            });
            while (!future.isFinished()) QTest::qWait(5);
            return future.result();
        };
        QVERIFY(probe("localhost", trusted).success);
        QCOMPARE(relay.destination.mid(3, 5), QByteArray::fromHex("017f000001"));
        QVERIFY(relay.tlsBytes.contains("localhost")); // ClientHello SNI
        QVERIFY(server.request.contains("Host: localhost:"));
        QCOMPARE(probe("wrong-host.example", trusted).failure, TunnelDataProbe::Failure::Tls);
        QCOMPARE(probe("localhost", QSslConfiguration::defaultConfiguration()).failure,
                 TunnelDataProbe::Failure::Tls);
        const auto direct = QNetworkProxy(QNetworkProxy::NoProxy);
        QCOMPARE(TunnelDataProbe::runAtAddress("localhost", QHostAddress::LocalHost,
                 server.serverPort(), direct, 100).failure, TunnelDataProbe::Failure::Path);
        QCOMPARE(TunnelDataProbe::runAtAddress("localhost", QHostAddress::LocalHost,
                 server.serverPort(), direct, 100, [](QSslSocket &) { return false; }).failure,
                 TunnelDataProbe::Failure::Path);
    }
    void stopWaitsForExactHelperAndFailureBlocksAdapterCleanup() {
        QTemporaryDir dir;
        RoutingSource service;
        OutputSource process; process.running = true;
        QRemoteObjectHost host(QUrl("local:" + dir.path() + "/stop"));
        QVERIFY(host.enableRemoting(&service)); QVERIFY(host.enableRemoting(&process));
        QRemoteObjectNode node; QVERIFY(node.connectToNode(host.hostUrl()));
        QSharedPointer<IpcInterfaceReplica> iface(node.acquire<IpcInterfaceReplica>());
        AsyncProcessRequest::Replica replica(node.acquire<IpcProcessInterfaceReplica>());
        QTRY_VERIFY(iface->isReplicaValid() && replica->isReplicaValid());
        QSet<QString> completed;
        int callbacks = 0, ticks = 0; bool success = false;
        QTimer heartbeat; connect(&heartbeat, &QTimer::timeout, [&] { ++ticks; }); heartbeat.start(10);
        service.fail = "deleteTun";
        new AsyncTunnelStop(this, replica, nullptr, iface, "fixture-tun", true, completed,
            [&](bool ok, const QString &) { ++callbacks; success = ok; });
        QTest::qWait(50); QVERIFY(service.calls.isEmpty());
        QTRY_COMPARE(callbacks, 1); QVERIFY(!success); QVERIFY(ticks >= 5);
        QVERIFY(!service.calls.contains("xrayStop")); QCOMPARE(process.closes, 1);
        const int policyCalls = service.calls.count("disableKillSwitch");
        service.fail.clear();
        new AsyncTunnelStop(this, replica, nullptr, iface, "fixture-tun", true, completed,
            [&](bool ok, const QString &) { ++callbacks; success = ok; });
        QTRY_COMPARE(callbacks, 2); QVERIFY(success);
        QCOMPARE(service.calls.count("disableKillSwitch"), policyCalls);
        QVERIFY(service.calls.contains("xrayStop"));
        service.calls.clear(); completed.clear(); process.running = true; process.ignoreKill = true;
        new AsyncTunnelStop(this, replica, nullptr, iface, "fixture-tun", true, completed,
            [&](bool ok, const QString &) { ++callbacks; success = ok; }, 200);
        QTRY_COMPARE(callbacks, 3); QVERIFY(!success); QVERIFY(service.calls.isEmpty());
    }
    void processObserverReportsUnexpectedCleanExitAndFailedStartOnce() {
        QTemporaryDir dir;
        QRemoteObjectHost host(QUrl("local:" + dir.path() + "/observer"));
        OutputSource source;
        QVERIFY(host.enableRemoting(&source));
        QRemoteObjectNode node;
        QVERIFY(node.connectToNode(host.hostUrl()));
        QScopedPointer<IpcProcessInterfaceReplica> replica(node.acquire<IpcProcessInterfaceReplica>());
        QTRY_VERIFY(replica->isReplicaValid());
        bool active = true;
        int failures = 0;
        auto *observer = new Tun2SocksProcessObserver(replica.data(), this,
            [&] { return active; }, [&] { ++failures; });
        emit source.finished(0, QProcess::NormalExit);
        QTRY_COMPARE(failures, 1);
        emit source.finished(1, QProcess::CrashExit);
        emit source.errorOccurred(QProcess::FailedToStart);
        QTest::qWait(30);
        QCOMPARE(failures, 1);
        delete observer;

        observer = new Tun2SocksProcessObserver(replica.data(), this,
            [&] { return active; }, [&] { ++failures; });
        active = false;
        emit source.finished(0, QProcess::NormalExit);
        QTest::qWait(30);
        QCOMPARE(failures, 1);
        active = true;
        emit source.errorOccurred(QProcess::FailedToStart);
        QTRY_COMPARE(failures, 2);
        delete observer;
    }
    void processRequestWaitsWithoutBlockingAndCancelSuppressesLateDiscovery() {
        QTemporaryDir dir;
        RoutingSource service;
        QRemoteObjectHost host(QUrl("local:" + dir.path() + "/service"));
        QVERIFY(host.enableRemoting(&service));
        QRemoteObjectNode node;
        QVERIFY(node.connectToNode(host.hostUrl()));
        QSharedPointer<IpcInterfaceReplica> iface(node.acquire<IpcInterfaceReplica>());
        const auto endpoint = [&](int id) { return QUrl("local:" + dir.path() + "/process" + QString::number(id)); };
        int callbacks = 0, ticks = 0;
        AsyncProcessRequest::Replica result;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; });
        heartbeat.start(5);
        new AsyncProcessRequest(this, iface, endpoint, [&](auto replica) { ++callbacks; result = replica; }, 1000);
        QTRY_COMPARE(service.processRequests, 1);
        QTest::qWait(40);
        QCOMPARE(callbacks, 0);
        QVERIFY(ticks >= 3);
        OutputSource source;
        QRemoteObjectHost processHost(endpoint(7));
        QVERIFY(processHost.enableRemoting(&source));
        QTRY_COMPARE(callbacks, 1);
        QVERIFY(result && result->isReplicaValid());
        result.reset();

        service.processId = 8;
        QPointer<AsyncProcessRequest> canceled = new AsyncProcessRequest(this, iface, endpoint,
            [&](auto) { ++callbacks; }, 500);
        QTRY_COMPARE(service.processRequests, 2);
        canceled->cancel();
        QRemoteObjectHost lateHost(endpoint(8));
        QVERIFY(lateHost.enableRemoting(&source));
        QTest::qWait(80);
        QCOMPARE(callbacks, 1);
        QVERIFY(canceled.isNull());

        service.processId = -1;
        new AsyncProcessRequest(this, iface, endpoint, [&](auto replica) { ++callbacks; result = replica; }, 300);
        QTRY_COMPARE(callbacks, 2);
        QVERIFY(!result);

        service.processId = 9;
        new AsyncProcessRequest(this, iface, endpoint, [&](auto replica) { ++callbacks; result = replica; }, 60);
        QTRY_COMPARE(callbacks, 3);
        QVERIFY(!result);
        QRemoteObjectHost tooLateHost(endpoint(9));
        QVERIFY(tooLateHost.enableRemoting(&source));
        QTest::qWait(100);
        QCOMPARE(callbacks, 3);
    }
    void replicaDiscoveryAndSocksCancellationKeepEventLoopResponsive() {
        QRemoteObjectNode node;
        QSharedPointer<IpcInterfaceReplica> missing(node.acquire<IpcInterfaceReplica>());
        int calls = 0;
        new AsyncReplicaReady(this, missing.data(), 40, [&](bool ready) { QVERIFY(!ready); ++calls; });
        QTRY_COMPARE(calls, 1);
        auto *waiter = new AsyncReplicaReady(this, missing.data(), 40, [&](bool) { ++calls; });
        waiter->cancel();
        QTest::qWait(70);
        QCOMPARE(calls, 1);

        QTcpServer silent;
        QVERIFY(silent.listen(QHostAddress::LocalHost));
        QList<QTcpSocket *> peers;
        connect(&silent, &QTcpServer::newConnection, this, [&] { peers.append(silent.nextPendingConnection()); });
        auto *probe = new AsyncSocksProbe(this, "localhost", 443, 4000, silent.serverPort(), {}, {},
            [&](bool) { ++calls; });
        QTRY_VERIFY(!peers.isEmpty());
        QElapsedTimer elapsed;
        elapsed.start();
        probe->cancel();
        QTRY_COMPARE(peers.first()->state(), QAbstractSocket::UnconnectedState);
        QVERIFY(elapsed.elapsed() < 1000);
        QCOMPARE(calls, 1);
        qDeleteAll(peers);
    }
    void tunnelProbeRequiresTrustedTlsAndHttpDataWithinDeadline() {
        TestCertificate cert;
        QVERIFY(!cert.certificate.isNull());
        QVERIFY(!cert.privateKey.isNull());
        TestHttpsServer server;
        server.ssl = QSslConfiguration::defaultConfiguration();
        server.ssl.setLocalCertificate(cert.certificate);
        server.ssl.setPrivateKey(cert.privateKey);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        auto trusted = QSslConfiguration::defaultConfiguration();
        trusted.setCaCertificates({cert.certificate});
        auto probe = [&](const QSslConfiguration &ssl, int timeout) {
            const quint16 port = server.serverPort();
            auto future = QtConcurrent::run([port, ssl, timeout] {
                return TunnelDataProbe::run("localhost", port, QNetworkProxy(QNetworkProxy::NoProxy), timeout, ssl);
            });
            while (!future.isFinished()) QTest::qWait(5);
            return future.result();
        };
        QVERIFY(probe(trusted, 3000).success);
        QCOMPARE(probe(QSslConfiguration::defaultConfiguration(), 3000).failure, TunnelDataProbe::Failure::Tls);
        server.response = "this is not an HTTP status\r\n";
        QCOMPARE(probe(trusted, 3000).failure, TunnelDataProbe::Failure::Response);
        server.response = QByteArray(9000, 'x');
        QCOMPARE(probe(trusted, 3000).failure, TunnelDataProbe::Failure::Response);
        server.silent = true;
        const auto timeout = probe(trusted, 250);
        QCOMPARE(timeout.failure, TunnelDataProbe::Failure::Timeout);
        QVERIFY(timeout.elapsedMs < 1500);
        QCOMPARE(TunnelDataProbe::run("localhost\r\nInjected: value", 443,
                 QNetworkProxy(QNetworkProxy::NoProxy), 250).failure, TunnelDataProbe::Failure::InvalidTarget);
    }
    void routingIsSerialAndDoesNotInstallCatchAllAfterExclusionFailure() {
        QTemporaryDir dir;
        QRemoteObjectHost host(QUrl("local:" + dir.path() + "/routing"));
        RoutingSource source;
        QVERIFY(host.enableRemoting(&source));
        QRemoteObjectNode node;
        QVERIFY(node.connectToNode(host.hostUrl()));
        QSharedPointer<IpcInterfaceReplica> iface(node.acquire<IpcInterfaceReplica>());
        QTRY_VERIFY(iface->isInitialized());
        SocksRoutingSetup::Parameters p;
        p.device = "fixture"; p.localAddress = p.vpnAddress = p.vpnGateway = "192.0.2.2";
        p.serverAddress = "198.51.100.10"; p.externalGateway = "203.0.113.1";
        p.allSites = true; p.appSplit = true; p.killSwitch = true;
        const QStringList expected {"createTun", "updateResolvers", "excludeServer", "routeAddList",
                                    "enablePeerTraffic", "StartRoutingIpv6"};
        for (const QString &fail : QStringList{"", "createTun", "excludeServer", "enablePeerTraffic"}) {
            source.calls.clear(); source.fail = fail;
            int completed = 0;
            AsyncIpcSequence::Result result = AsyncIpcSequence::Result::Timeout;
            new AsyncIpcSequence(this, SocksRoutingSetup::steps(iface, p, [] { return 56; },
                                [](const QHostAddress &) { return 2; }),
                [&](AsyncIpcSequence::Result r, const QString &) { result = r; ++completed; });
            QTRY_COMPARE(completed, 1);
            const int count = fail.isEmpty() ? expected.size() : expected.indexOf(fail) + 1;
            QCOMPARE(source.calls, expected.mid(0, count));
            QCOMPARE(result, fail.isEmpty() ? AsyncIpcSequence::Result::Success : AsyncIpcSequence::Result::Rejected);
        }
        source.calls.clear(); source.fail.clear();
        p.appSplit = false;
        int completed = 0;
        new AsyncIpcSequence(this, SocksRoutingSetup::steps(iface, p, [] { return 56; },
                            [](const QHostAddress &) { return 2; }),
                            [&](AsyncIpcSequence::Result, const QString &) { ++completed; });
        QTRY_COMPARE(completed, 1);
        QCOMPARE(source.calls, QStringList({"createTun", "updateResolvers", "enableKillSwitch", "excludeServer",
                                            "routeAddList", "StopRoutingIpv6", "enablePeerTraffic"}));
    }
    void ipcSequenceStopsOnFailureAndCancel() {
        using Sequence = AsyncIpcSequence;
        int requests = 0, completions = 0;
        Sequence::Result result = Sequence::Result::Success;
        QString failedStep;
        auto request = [&](bool value) {
            ++requests;
            return QRemoteObjectPendingCall::fromCompletedCall(value);
        };
        new Sequence(this, {{"first", [&] { return request(true); }},
                            {"second", [&] { return request(false); }},
                            {"must-not-run", [&] { return request(true); }}},
                     [&](Sequence::Result r, const QString &step) { ++completions; result = r; failedStep = step; });
        QTRY_COMPARE(completions, 1);
        QCOMPARE(requests, 2);
        QCOMPARE(result, Sequence::Result::Rejected);
        QCOMPARE(failedStep, QString("second"));
        auto *cancelled = new Sequence(this, {{"must-not-run", [&] { return request(true); }}},
                                       [&](Sequence::Result, const QString &) { ++completions; });
        cancelled->cancel();
        QTest::qWait(20);
        QCOMPARE(requests, 2);
        QCOMPARE(completions, 1);
    }
    void ipcTimeoutKeepsUiAliveAndIgnoresLateReply() {
        QTemporaryDir dir;
        QThread thread;
        QObject worker;
        worker.moveToThread(&thread);
        thread.start();
        QRemoteObjectHost *host = nullptr;
        OutputSource *source = nullptr;
        const QUrl address("local:" + dir.path() + "/slow-output");
        QMetaObject::invokeMethod(&worker, [&] {
            host = new QRemoteObjectHost(address);
            source = new OutputSource;
            source->delayMs = 150;
            source->bytes = "reply";
            host->enableRemoting(source);
        }, Qt::BlockingQueuedConnection);
        // Ensure worker objects and thread are cleaned up even on a failed assertion.
        const auto cleanup = qScopeGuard([&] {
            QMetaObject::invokeMethod(&worker, [&] { delete host; delete source; }, Qt::BlockingQueuedConnection);
            thread.quit(); thread.wait();
        });
        QRemoteObjectNode node;
        QVERIFY(node.connectToNode(address));
        QScopedPointer<IpcProcessInterfaceReplica> replica(node.acquire<IpcProcessInterfaceReplica>());
        QTRY_VERIFY(replica->isInitialized());
        int completed = 0, later = 0, ticks = 0;
        AsyncIpcSequence::Result result = AsyncIpcSequence::Result::Success;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; });
        heartbeat.start(1);
        new AsyncIpcSequence(this,
            {{"slow", [&] { return replica->readAllStandardOutput(); },
              [](const QVariant &) { return true; }, 20},
             {"must-not-run", [&] { ++later; return QRemoteObjectPendingCall::fromCompletedCall(true); }}},
            [&](AsyncIpcSequence::Result r, const QString &) { ++completed; result = r; });
        QTRY_COMPARE(completed, 1);
        QCOMPARE(result, AsyncIpcSequence::Result::Timeout);
        QVERIFY(ticks > 0);
        QTest::qWait(200);
        QCOMPARE(completed, 1);
        QCOMPARE(later, 0);
    }
    void defaultRouteUsesCombinedMetricsAndActiveLinks() {
        using namespace DefaultRouteSelection;
        Route wired {2, 0xc0000201, 10, 25, 0, true, false, true, true};
        Route wifi {5, 0xc6336401, 5, 50, 0, true, false, true, true};
        QCOMPARE(select({wifi, wired})->interfaceIndex, std::uint32_t(2));
        QCOMPARE(select({wired, wifi})->interfaceIndex, std::uint32_t(2));
        wifi.interfaceMetric = 1;
        QCOMPARE(select({wired, wifi})->interfaceIndex, std::uint32_t(5));
        for (int failure = 0; failure < 7; ++failure) {
            Route invalid = wifi;
            switch (failure) {
            case 0: invalid.up = false; break;
            case 1: invalid.connected = false; break;
            case 2: invalid.valid = false; break;
            case 3: invalid.loopback = true; break;
            case 4: invalid.gateway = 0; break;
            case 5: invalid.interfaceIndex = 0; break;
            case 6: invalid.prefixLength = 32; break;
            }
            QCOMPARE(select({invalid, wired})->interfaceIndex, std::uint32_t(2));
            QVERIFY(!select({invalid}));
        }
        wired.routeMetric = UINT32_MAX;
        wired.interfaceMetric = UINT32_MAX;
        QCOMPARE(select({wired, wifi})->interfaceIndex, std::uint32_t(5));
        wired.routeMetric = wifi.routeMetric;
        wired.interfaceMetric = wifi.interfaceMetric;
        QCOMPARE(select({wifi, wired})->interfaceIndex, std::uint32_t(2));
        QVERIFY(!select({}));
        // Local connected routes (including a Radmin virtual LAN) must not be
        // mistaken for the external default gateway; this code never edits them.
        for (const unsigned prefix : {8u, 16u, 24u}) {
            Route local = wifi;
            local.prefixLength = prefix;
            local.routeMetric = local.interfaceMetric = 0;
            wired.routeMetric = 256; wired.interfaceMetric = 25;
            QCOMPARE(select({local, wired})->interfaceIndex, std::uint32_t(2));
        }
    }
    void readerDrainsAfterReadyWithoutBlockingAndStopsCallbacks() {
        QTemporaryDir dir;
        QRemoteObjectHost host(QUrl("local:" + dir.path() + "/output"));
        OutputSource source;
        QVERIFY(host.enableRemoting(&source));
        QRemoteObjectNode node;
        QVERIFY(node.connectToNode(host.hostUrl()));
        QScopedPointer<IpcProcessInterfaceReplica> replica(node.acquire<IpcProcessInterfaceReplica>());
        QTRY_VERIFY(replica->isInitialized());
        bool active = true;
        int ready = 0, ticks = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; });
        heartbeat.start(1);
        auto *reader = new Tun2SocksOutputReader(replica.data(), this, [&] { return active; }, [&] { ++ready; });
        source.append("[STACK] tun://tun2 <-> socks5://user:");
        QTRY_VERIFY(source.bytes.isEmpty());
        QCOMPARE(ready, 0);
        source.append("secret@localhost:123\n");
        QTRY_COMPARE(ready, 1);
        for (int round = 0; round < 10; ++round) {
            for (int burst = 0; burst < 100; ++burst)
                source.append("[TCP] flow\n[UDP] flow\n");
            QTRY_VERIFY(source.bytes.isEmpty());
        }
        QVERIFY(ticks > 0);
        QVERIFY(source.reads > 10);
        source.append("[STACK] tun://tun2 <-> socks5://localhost:1\n");
        QTRY_VERIFY(source.bytes.isEmpty());
        QCOMPARE(ready, 1);
        active = false;
        source.append("[STACK] tun://tun2 <-> socks5://localhost:1\n");
        QTest::qWait(30);
        QCOMPARE(ready, 1);
        delete reader;
        // Outstanding notifications/replies must be harmless after deletion.
        source.append("[UDP] flow\n");
        QTest::qWait(30);
    }
};
QTEST_GUILESS_MAIN(TransportTests)
#include "main.moc"

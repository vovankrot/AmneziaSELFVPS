#include <QtTest>
#include "common/logger/logRedaction.h"
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <future>
#include <QTcpServer>
#include "client/core/socksProbe.h"
#include "client/core/controllers/configSnapshotManager.h"
#include "core/controllers/serverController.h"

QString amnezia::ContainerProps::containerToString(DockerContainer container) {
    return container == DockerContainer::XrayReality ? "amnezia-xray-reality" : "amnezia-xray";
}
#include "client/core/listValidation.h"
#include "client/core/wgShowParser.h"
#include "client/core/configFormat.h"
#include "client/core/protectedBlob.h"
#include "client/core/sshHostTrust.h"
#include "ipc/ipc.h"
#include "ipc/localPeerAuth.h"

struct Peer { QString clientId, latestHandshake, dataReceived, dataSent, allowedIps, endpoint; };
class SecurityTests : public QObject {
    Q_OBJECT
private slots:
    void proxyCredentialsNeverReachLogs() {
        QCOMPARE(LogRedaction::hideProxyCredentials("[STACK] tun://tun2 <-> socks5://test-user:test-password@127.0.0.1:63780"),
                 QString("[STACK] tun://tun2 <-> socks5://[REDACTED]@127.0.0.1:63780"));
        QCOMPARE(LogRedaction::hideProxyCredentials("HTTPS://user:p%40ss@example.test/path socks5h://second:other@localhost:99"),
                 QString("HTTPS://[REDACTED]@example.test/path socks5h://[REDACTED]@localhost:99"));
        QCOMPARE(LogRedaction::hideProxyCredentials("socks5://127.0.0.1:63780 timeout 1460"),
                 QString("socks5://127.0.0.1:63780 timeout 1460"));
    }
    void snapshotFailuresStopBeforeDestructiveChanges() {
        auto settings = std::make_shared<Settings>();
        auto remote = QSharedPointer<ServerController>::create();
        for (const char *path : {amnezia::protocols::xray::PublicKeyPath, amnezia::protocols::xray::PrivateKeyPath,
            amnezia::protocols::xray::shortidPath, amnezia::protocols::xray::uuidPath, amnezia::protocols::xray::xhttpPathPath}) remote->remote[path] = "test-key";
        remote->remote[amnezia::protocols::xray::serverConfigPath] = "{\"inbounds\":[{\"port\":443}]}";
        ConfigSnapshotManager manager(settings);
        const QString hash = manager.serverHash(settings->credentials), base = manager.snapshotBasePath(hash);
        QDir(base).removeRecursively();
        remote->failedRead = amnezia::protocols::xray::PrivateKeyPath;
        QVERIFY(!manager.saveSnapshot(settings->credentials, DockerContainer::Xray, settings->current, remote));
        QVERIFY(manager.listSnapshots(hash).isEmpty());
        remote->failedRead.clear();
        settings->key.clear();
        QVERIFY(!manager.saveSnapshot(settings->credentials, DockerContainer::Xray, settings->current, remote));
        QVERIFY(manager.listSnapshots(hash).isEmpty());
        settings->key = QByteArray(32, 's');
        remote->remote[amnezia::protocols::xray::serverConfigPath] = "{\"inbounds\":[{\"port\":443,\"streamSettings\":{\"network\":\"xhttp\"}}]}";
        remote->failedRead = amnezia::protocols::xray::xhttpPathPath;
        QVERIFY(!manager.saveSnapshot(settings->credentials, DockerContainer::Xray, settings->current, remote));
        QVERIFY(manager.listSnapshots(hash).isEmpty());
        remote->failedRead.clear();
        remote->remote[amnezia::protocols::xray::serverConfigPath] = "{\"inbounds\":[{\"port\":443}]}";
        auto differentPort = settings->credentials; differentPort.port = 2222;
        QVERIFY(manager.serverHash(differentPort) != hash);
        QVERIFY(manager.saveSnapshot(settings->credentials, DockerContainer::Xray, settings->current, remote));
        const auto snapshots = manager.listSnapshots(hash); QCOMPARE(snapshots.size(), 1);
        QJsonObject restored;
        QVERIFY(!manager.restoreSnapshot(settings->credentials, DockerContainer::Xray, "../bad", restored, remote));
        remote->failedUpload = amnezia::protocols::xray::PrivateKeyPath;
        QVERIFY(!manager.restoreSnapshot(settings->credentials, DockerContainer::Xray, snapshots[0].id, restored, remote));
        QVERIFY(remote->scripts.isEmpty()); QVERIFY(restored.isEmpty());
        remote->failedUpload.clear(); remote->failSwap = true;
        QVERIFY(!manager.restoreSnapshot(settings->credentials, DockerContainer::Xray, snapshots[0].id, restored, remote));
        QVERIFY(remote->scripts.last().contains(".previous")); QVERIFY(!remote->scripts.last().contains("docker restart"));
        remote->scripts.clear(); remote->failSwap = false;
        QVERIFY(manager.restoreSnapshot(settings->credentials, DockerContainer::Xray, snapshots[0].id, restored, remote));
        QCOMPARE(restored, settings->current);
        QVERIFY(remote->scripts.join('\n').contains("xray run -test"));
        QVERIFY(remote->scripts.join('\n').contains("sudo docker restart 'amnezia-xray'"));
        QVERIFY(!remote->scripts.join('\n').contains("killall xray"));
        remote->scripts.clear(); remote->failRestart = true;
        QVERIFY(!manager.restoreSnapshot(settings->credentials, DockerContainer::Xray, snapshots[0].id, restored, remote));
        QVERIFY(remote->scripts.join('\n').contains("sudo docker cp")); QVERIFY(restored.isEmpty());
        QFile file(base + "/" + snapshots[0].id + "/snapshot.bin");
        QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.seek(40));
        auto corruptedByte = file.read(1); QCOMPARE(corruptedByte.size(), 1);
        corruptedByte[0] = char(corruptedByte.at(0) ^ 0x01);
        QVERIFY(file.seek(40)); QCOMPARE(file.write(corruptedByte), 1); file.close();
        remote->scripts.clear(); remote->uploads.clear();
        QVERIFY(!manager.restoreSnapshot(settings->credentials, DockerContainer::Xray, snapshots[0].id, restored, remote));
        QVERIFY(remote->scripts.isEmpty()); QVERIFY(remote->uploads.isEmpty());
        QVERIFY(QDir(base).removeRecursively());
    }
    void socksProbeChecksCompleteReplyAndAuthentication() {
        for (bool complete : {false, true}) {
            std::promise<quint16> port;
            auto worker = std::async(std::launch::async, [&] {
                QTcpServer server;
                if (!server.listen(QHostAddress::LocalHost, 0)) { port.set_value(0); return; }
                port.set_value(server.serverPort());
                if (!server.waitForNewConnection(1500)) return;
                auto socket = std::unique_ptr<QTcpSocket>(server.nextPendingConnection());
                if (!socket->waitForReadyRead(1000)) return;
                socket->readAll(); socket->write(QByteArray::fromHex("0500")); socket->waitForBytesWritten(1000);
                if (!socket->waitForReadyRead(1000)) return;
                socket->readAll(); socket->write(QByteArray::fromHex("05000001")); socket->waitForBytesWritten(1000);
                if (complete) {
                    QThread::msleep(70);
                    socket->write(QByteArray::fromHex("7f00000101bb")); socket->waitForBytesWritten(1000);
                    QThread::msleep(70);
                }
            });
            const auto socksPort = port.get_future().get(); QVERIFY(socksPort != 0);
            QCOMPARE(SocksProbe::connect("example.com", 443, 1000, socksPort), complete);
            worker.get();
        }
        QVERIFY(!SocksProbe::connect("example.com", 443, 100, 1080, "user", ""));
    }
    void peerStatsDoNotShift() {
        const auto peers = parseWgShow<Peer>("interface: wg0\npeer: idle\n  allowed ips: 10.0.0.2/32\n"
            "peer: active\n  endpoint: [2001:db8::1]:51820\n  latest handshake: 2 minutes, 3 seconds ago\n"
            "  transfer: 3 KiB received, 9 KiB sent\npeer: third\n  transfer: 1 KiB sent\n");
        QCOMPARE(peers.size(), size_t(3));
        QVERIFY(peers[0].latestHandshake.isEmpty()); QVERIFY(peers[0].dataReceived.isEmpty());
        QCOMPARE(peers[1].endpoint, QString("[2001:db8::1]:51820"));
        QCOMPARE(peers[1].dataReceived, QString("9 KiB")); QCOMPARE(peers[1].dataSent, QString("3 KiB"));
        QCOMPARE(peers[2].clientId, QString("third")); QVERIFY(peers[2].dataSent.isEmpty());
    }
    void validateListsAndPreserveCache() {
        QByteArray bytes; int count;
        QVERIFY(ListValidation::normalize("10.1.2.3/24\n10.1.2.0/24\n0.0.0.0/0\n", true, bytes, count));
        QCOMPARE(count, 2); QCOMPARE(bytes, QByteArray("10.1.2.0/24\n0.0.0.0/0\n"));
        for (const QByteArray invalid : {QByteArray("999.2.3.4/24"), QByteArray("1.2.3.4/99"), QByteArray("1.2.3.4/+1")})
            QVERIFY(!ListValidation::normalize(invalid, true, bytes, count));
        QVERIFY(ListValidation::normalize(QString::fromUtf8("*.EXAMPLE.COM.\nпример.рф\nexample.com\n").toUtf8(), false, bytes, count));
        QCOMPARE(count, 2); QVERIFY(bytes.contains("xn--e1afmkfd.xn--p1ai"));
        for (const QByteArray invalid : {QByteArray("<html>page.example</html>"), QByteArray("https://example.com/path"), QByteArray("a..com"), QByteArray("-bad.com"), QByteArray("bad\xff.com")})
            QVERIFY(!ListValidation::normalize(invalid, false, bytes, count));
        QVERIFY(!ListValidation::normalize(QByteArray(ListValidation::maxDownloadBytes + 1, 'x'), false, bytes, count));
        QTemporaryDir dir; const QString cache = dir.filePath("list");
        QVERIFY(ListValidation::save(cache, "old\n"));
        QVERIFY(!ListValidation::save(cache + "/impossible", "new\n"));
        QFile file(cache); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("old\n"));
    }
    void encryptedSnapshotsRejectTampering() {
        const QByteArray plain("server private keys and client credentials");
        const QByteArray key(32, 'k');
        auto first = ProtectedBlob::seal(plain, key), second = ProtectedBlob::seal(plain, key);
        QVERIFY(!first.isEmpty()); QVERIFY(first != second); QVERIFY(!first.contains(plain));
        QCOMPARE(ProtectedBlob::open(first, key), plain);
        QVERIFY(ProtectedBlob::open(first, QByteArray(32, 'x')).isEmpty());
        for (int index : {8, 20, int(first.size()-1)}) {
            auto altered = first; altered[index] = altered[index] ^ 1;
            QVERIFY(ProtectedBlob::open(altered, key).isEmpty());
        }
        QVERIFY(ProtectedBlob::open(first.left(35), key).isEmpty());
        QVERIFY(ProtectedBlob::seal(plain, "wrong-sized-key").isEmpty());
    }
    void rejectUnknownConfigVersions() {
        QVERIFY(ConfigFormat::supported({{"containers", QJsonArray()}}));
        QVERIFY(ConfigFormat::supported(ConfigFormat::stamp({})));
        QVERIFY(!ConfigFormat::supported({{"awg", QJsonObject{{"RandomTrailers", true}}}}));
        QVERIFY(!ConfigFormat::supported({{"last_config", "{\"DisableCookies\":false}"}}));
        QVERIFY(!ConfigFormat::backendCompatible("[Interface]\nRandomTrailers = true\n"));
        for (const QJsonValue value : {QJsonValue(1), QJsonValue(-1), QJsonValue(0.5), QJsonValue("0"), QJsonValue(true), QJsonValue(QJsonValue::Null)})
            QVERIFY(!ConfigFormat::supported({{"formatVersion", value}}));
    }
    void certutilAllowsOnlyCertificateImport() {
        QTemporaryDir dir; QFile cert(dir.filePath("cert.p12")); QVERIFY(cert.open(QIODevice::WriteOnly)); cert.write("test"); cert.close();
        QStringList accepted {"-f", "-importpfx", "-p", "test password", cert.fileName(), "NoExport"};
        QCOMPARE(amnezia::sanitizeArguments(amnezia::CertUtil, accepted), accepted);
        QVERIFY(amnezia::sanitizeArguments(amnezia::CertUtil, {"-urlcache", "-split", "-f", "https://example.com/payload", cert.fileName()}).isEmpty());
        accepted << "-user"; QVERIFY(amnezia::sanitizeArguments(amnezia::CertUtil, accepted).isEmpty());
        QVERIFY(amnezia::sanitizeArguments(amnezia::Tun2Socks, {"--device", "tun://tun0", "--proxy", "socks5://127.0.0.1:1080", "--unknown"}) !=
                QStringList({"--device", "tun://tun0", "--proxy", "socks5://127.0.0.1:1080", "--unknown"}));
    }
    void sshTrustAcceptRejectAndChangedKey() {
        QTemporaryDir dir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        SshHostTrust trust;
        int prompts = 0; bool accept = true;
        connect(&trust, &SshHostTrust::confirmationRequested, &trust, [&](const QString &, const QString &) { ++prompts; trust.answer(accept); });
        bool changed;
        QVERIFY(SshHostTrust::verify("test.example", 22, "SHA256:one", changed)); QVERIFY(!changed); QCOMPARE(prompts, 1);
        QVERIFY(SshHostTrust::verify("test.example", 22, "SHA256:one", changed)); QCOMPARE(prompts, 1);
        QVERIFY(!SshHostTrust::verify("test.example", 22, "SHA256:two", changed)); QVERIFY(changed); QCOMPARE(prompts, 1);
        accept = false;
        QVERIFY(!SshHostTrust::verify("other.example", 22, "SHA256:two", changed)); QVERIFY(!changed); QCOMPARE(prompts, 2);
        accept = true;
        auto worker = std::async(std::launch::async, [&] { bool c; return SshHostTrust::verify("test.example", 2222, "SHA256:two", c); });
        QTRY_VERIFY_WITH_TIMEOUT(worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 3000);
        QVERIFY(worker.get()); QCOMPARE(prompts, 3);
    }
    void namedPipeChecksExecutableIdentity() {
        const QString executable = QCoreApplication::applicationFilePath();
        const QString trusted = QCoreApplication::applicationDirPath() + "/AmneziaVPN.exe";
        QVERIFY(QFile::copy(executable, trusted));
        for (bool expected : {false, true}) {
            QLocalServer server; const QString name = "selfvps-peer-test-" + QString::number(QCoreApplication::applicationPid()) + QString::number(expected);
            QVERIFY(server.listen(name));
            QProcess child; child.start(expected ? trusted : executable, {"--peer", name}); QVERIFY(child.waitForStarted(2000));
            QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 3000);
            std::unique_ptr<QLocalSocket> socket(server.nextPendingConnection());
            QCOMPARE(amnezia::isTrustedLocalPeer(socket.get()), expected);
            socket->write("done"); socket->flush(); QVERIFY(child.waitForFinished(3000));
        }
        QVERIFY(QFile::remove(trusted));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--peer")) {
        QLocalSocket socket; socket.connectToServer(app.arguments().last());
        if (!socket.waitForConnected(3000)) return 2;
        return socket.waitForReadyRead(5000) ? 0 : 3;
    }
    app.setOrganizationName("SELFVPS-Security-Tests"); app.setApplicationName("isolated");
    SecurityTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "main.moc"

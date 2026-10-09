#include <QtTest>
#include "common/logger/logRedaction.h"
#include "client/core/tun2socksOutput.h"
#include "client/core/localSocksUrl.h"
#include <QFile>
#include <QRegExp>
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
#include "client/core/sshSessionPolicy.h"
#include "ipc/ipc.h"
#include "ipc/localPeerAuth.h"

struct Peer { QString clientId, latestHandshake, dataReceived, dataSent, allowedIps, endpoint; };
#include "client/core/api/apiDefs.h"
#include <QJsonArray>
namespace config_key = amnezia::config_key;
class PasswordModelFixture {
public:
    QJsonArray m_servers;
    QJsonObject getServerConfig(int index) const { return index >= 0 && index < m_servers.size() ? m_servers.at(index).toObject() : QJsonObject{}; }
    int m_processedServerIndex = 0, writes = 0;
    bool canEditProcessedServerPassword() const;
    bool updateProcessedServerPassword(int expectedIndex, const QString &password);
    void editServer(const QJsonObject &server, int index) { m_servers.replace(index, server); ++writes; }
};
#include "production-password-model.inc"
namespace NetworkUtilities {
QRegExp ipPortRegExp();
QString netMaskFromIpWithSubnet(const QString);
bool checkIPv4Format(const QString &s) { return QHostAddress(s).protocol() == QAbstractSocket::IPv4Protocol; }
}
#include "production-port-validator.inc"
#include "production-subnet-mask.inc"
DockerContainer fixtureDnsContainer(const QString &s) { return s == "amnezia-dns" ? DockerContainer::Dns : DockerContainer::None; }
struct DnsSettingsFixture {
    QString primary = "9.9.9.9", secondary = "149.112.112.112";
    QString primaryDns() const { return primary; } QString secondaryDns() const { return secondary; }
};
class DnsModelFixture {
public:
    QJsonArray m_servers;
    QJsonObject getServerConfig(int index) const { return index >= 0 && index < m_servers.size() ? m_servers.at(index).toObject() : QJsonObject{}; }
    bool m_isAmneziaDnsEnabled = false;
    std::shared_ptr<DnsSettingsFixture> m_settings = std::make_shared<DnsSettingsFixture>();
    QPair<QString, QString> getDnsPair(int);
};
#include "production-dns-model.inc"
QString testApplicationFilePath() { return "C:/Program Files/AmneziaVPN/AmneziaVPN.exe"; }
struct AutostartFixture { static QString appPath(); };
#include "production-autostart-path.inc"
class SecurityTests : public QObject {
    Q_OBJECT
private slots:
    void portValidatorAcceptsExactlyTheEntirePortRange() {
        auto validator = NetworkUtilities::ipPortRegExp();
        for (int port = 1; port <= 65535; ++port)
            QVERIFY2(validator.exactMatch(QString::number(port)), qPrintable(QString::number(port)));
        for (const auto &bad : {"0", "65536", "70000", "-1", "1.5", "01", "1x", ""})
            QVERIFY(!validator.exactMatch(bad));
    }
    void subnetMaskHasDefinedBoundaryBehavior() {
        QCOMPARE(NetworkUtilities::netMaskFromIpWithSubnet("192.0.2.1/0"), QString("0.0.0.0"));
        QCOMPARE(NetworkUtilities::netMaskFromIpWithSubnet("192.0.2.1/1"), QString("128.0.0.0"));
        QCOMPARE(NetworkUtilities::netMaskFromIpWithSubnet("192.0.2.1/24"), QString("255.255.255.0"));
        QCOMPARE(NetworkUtilities::netMaskFromIpWithSubnet("192.0.2.1/32"), QString("255.255.255.255"));
        for (const auto &bad : {"192.0.2.1/-1", "192.0.2.1/33", "192.0.2.1/x", "192.0.2.1/24/8"})
            QCOMPARE(NetworkUtilities::netMaskFromIpWithSubnet(bad), QString("255.255.255.255"));
    }
    void windowsAutostartQuotesPathWithSpaces() {
        const auto command=AutostartFixture::appPath();
        QVERIFY(command.startsWith('"'));
        QCOMPARE(QProcess::splitCommand(command), QStringList({QDir::toNativeSeparators(testApplicationFilePath()),"--autostart"}));
    }
    void selectedDnsHonorsLocalSwitchAndRequestedServer() {
        DnsModelFixture model;
        model.m_servers = {QJsonObject{{config_key::dns1, protocols::dns::amneziaDnsIp}},
            QJsonObject{{config_key::dns1, "8.8.8.8"}},
            QJsonObject{{config_key::containers, QJsonArray{QJsonObject{{config_key::container,"amnezia-dns"}}}}}};
        QVERIFY(model.getDnsPair(-1).first.isEmpty()); QVERIFY(model.getDnsPair(3).first.isEmpty());
        QCOMPARE(model.getDnsPair(0), qMakePair(QString("9.9.9.9"), QString("149.112.112.112")));
        model.m_isAmneziaDnsEnabled = true;
        QCOMPARE(model.getDnsPair(0), qMakePair(protocols::dns::amneziaDnsIp, QString()));
        QCOMPARE(model.getDnsPair(1).first, QString("9.9.9.9"));
        QCOMPARE(model.getDnsPair(2).first, protocols::dns::amneziaDnsIp);
        model.m_isAmneziaDnsEnabled = false;
        model.m_settings->secondary = model.m_settings->primary;
        QVERIFY(model.getDnsPair(0).second.isEmpty());
        model.m_settings->primary = "invalid"; model.m_settings->secondary = "invalid";
        QCOMPARE(model.getDnsPair(0), qMakePair(QString("1.1.1.1"), QString()));
    }
    void sshPortAndCredentialIdentityAndUtf8Passphrase() {
        QCOMPARE(SshSessionPolicy::effectivePort(0), 22);
        QCOMPARE(SshSessionPolicy::effectivePort(2222), 2222);
        QCOMPARE(SshSessionPolicy::effectivePort(65536), -1);
        QCOMPARE(SshSessionPolicy::effectivePort(-1), -1);
        const auto key = SshSessionPolicy::identity("host", "user", 0, "secret");
        QCOMPARE(key, SshSessionPolicy::identity("host", "user", 22, "secret"));
        QVERIFY(key != SshSessionPolicy::identity("host", "user", 2222, "secret"));
        QVERIFY(key != SshSessionPolicy::identity("host", "user", 22, "changed"));
        QVERIFY(key != SshSessionPolicy::identity("other", "user", 22, "secret"));
        QVERIFY(key != SshSessionPolicy::identity("host", "other", 22, "secret"));
        char buffer[5] = {'?', '?', '?', '?', '!'};
        const QString text = QString::fromUtf8("юя");
        QVERIFY(!SshSessionPolicy::copyPassphrase(text, buffer, 4));
        QCOMPARE(buffer[0], '\0'); QCOMPARE(buffer[4], '!');
        QVERIFY(SshSessionPolicy::copyPassphrase(text, buffer, 5));
        QCOMPARE(QByteArray(buffer), text.toUtf8());
        QVERIFY(!SshSessionPolicy::copyPassphrase(text, nullptr, 5));
        QVERIFY(!SshSessionPolicy::copyPassphrase(text, buffer, 0));
    }
    void savedServerPasswordPreservesProfileAndRejectsWrongTarget() {
        QJsonObject original {{"hostName", "fixture.invalid"}, {"userName", "root"},
            {"password", "old-fixture"}, {"port", 2222}, {"containers", QJsonArray{QJsonObject{{"config", "preserved"}}}}};
        // Use the actual production key names, including future schema changes.
        original.insert(amnezia::config_key::hostName, "fixture.invalid");
        original.insert(amnezia::config_key::userName, "root");
        original.insert(amnezia::config_key::password, "old-fixture");
        PasswordModelFixture model;
        model.m_servers = {original};
        QVERIFY(model.canEditProcessedServerPassword());
        QVERIFY(!model.updateProcessedServerPassword(1, "wrong-target"));
        QVERIFY(!model.updateProcessedServerPassword(0, ""));
        QCOMPARE(model.writes, 0);
        const QString password = QString::fromUtf8("  p@ss:ю  ");
        QVERIFY(model.updateProcessedServerPassword(0, password));
        auto expected = original; expected.insert(amnezia::config_key::password, password);
        QCOMPARE(model.m_servers.at(0).toObject(), expected);
        QCOMPARE(model.writes, 1);
        auto key = original; key.insert(amnezia::config_key::password, "-----BEGIN PRIVATE KEY-----fixture");
        model.m_servers = {key};
        QVERIFY(!model.updateProcessedServerPassword(0, "replacement"));
        model.m_servers = {original};
        model.m_processedServerIndex = -1;
        QVERIFY(!model.updateProcessedServerPassword(-1, "replacement"));
    }
    void localProxyUrlPreservesCredentialsAndRedactsThem() {
        const QString user = QString::fromUtf8("a:b@c/?#% +ю");
        const QString password = QString::fromUtf8("p@:/?#% +\tя");
        const auto encoded = LocalSocksUrl::make(10808, user, password);
        const QUrl parsed(encoded, QUrl::StrictMode);
        QVERIFY(parsed.isValid());
        QCOMPARE(parsed.host(), QString("127.0.0.1"));
        QCOMPARE(parsed.port(), 10808);
        QCOMPARE(parsed.userName(QUrl::FullyDecoded), user);
        QCOMPARE(parsed.password(QUrl::FullyDecoded), password);
        QVERIFY(parsed.query().isEmpty() && parsed.fragment().isEmpty());
        const auto safe = LogRedaction::hideProxyCredentials(encoded);
        QCOMPARE(safe, QString("socks5://[REDACTED]@127.0.0.1:10808"));
        QCOMPARE(LocalSocksUrl::make(10809), QString("socks5://127.0.0.1:10809"));
    }
    void tunOutputHandlesFragmentsAndKeepsWarnings() {
        Tun2SocksOutput parser;
        QVERIFY(!parser.feed("[STACK] tun://tun2 <-> socks5://user:").ready);
        auto first = parser.feed("secret@localhost:1000\n[UDP] normal flow\n");
        QVERIFY(first.ready);
        QCOMPARE(first.diagnostics.size(), 1);
        QVERIFY(!first.diagnostics.first().contains("secret"));
        QVERIFY(!parser.feed("[STACK] tun://tun2 <-> socks5://localhost:1000\n").ready);
        auto after = parser.feed("[UDP] normal\nlevel=warning [UDP] symmetric NAT drop packet\nlevel=error [TCP] connection reset\n");
        QCOMPARE(after.diagnostics.size(), 2);
        QVERIFY(after.diagnostics.first().contains("drop packet"));
        for (int i = 0; i < 1000; ++i)
            QVERIFY(parser.feed("[TCP] normal\n[UDP] normal\n").diagnostics.isEmpty());
        QCOMPARE(parser.bufferedBytes(), qsizetype(0));
    }
    void tunOutputDiscardsWholeOversizedLines() {
        Tun2SocksOutput parser;
        QVERIFY(parser.feed(QByteArray(20000, 'x') + "socks5://user:").diagnostics.isEmpty());
        QVERIFY(parser.bufferedBytes() <= 16384);
        auto result = parser.feed("secret@localhost\nlevel=error [UDP] failure\n");
        QCOMPARE(result.diagnostics.size(), 1);
        QVERIFY(!result.diagnostics.first().contains("secret"));
        QCOMPARE(parser.bufferedBytes(), qsizetype(0));
        QVERIFY(parser.feed("[STACK] tun://tun2 <-> socks5://localhost:1\n").ready);
    }
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
        const QJsonObject toggles{{"awg", QJsonObject{{"RandomTrailers", true}}}};
        const QJsonObject embedded{{"last_config", "{\"DisableCookies\":false}"}};
        const QJsonValue text("[Interface]\nRandomTrailers = on\n");
        QVERIFY(!ConfigFormat::backendCompatible(toggles, false));
        QVERIFY(!ConfigFormat::backendCompatible(embedded, false));
        QVERIFY(!ConfigFormat::backendCompatible(text, false));
        QVERIFY(ConfigFormat::backendCompatible(toggles, true));
        QVERIFY(ConfigFormat::backendCompatible(embedded, true));
        QVERIFY(ConfigFormat::backendCompatible(text, true));
        QCOMPARE(ConfigFormat::supported(toggles), ConfigFormat::nativeAwg31);
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

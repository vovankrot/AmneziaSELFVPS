#include <QtTest>
#include <QTemporaryDir>
#include <QSettings>
#include <QJsonDocument>
#include "client/core/appUpdater.h"
#include "client/core/splitTunnelAddress.h"

class FakeReply : public QNetworkReply {
public:
    QByteArray bytes;
    qint64 offset = 0;
    bool done = false;
    int aborts = 0;
    FakeReply(const QNetworkRequest &request, QByteArray content, QObject *parent, int status = 200) : QNetworkReply(parent), bytes(content) {
        setRequest(request); setUrl(request.url()); setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(5, this, [this] {
            if (done) return;
            emit readyRead();
            if (done) return;
            done = true; setFinished(true); emit finished();
        });
    }
    void abort() override {
        ++aborts;
        if (done) return;
        done = true; setError(QNetworkReply::OperationCanceledError, "cancelled"); setFinished(true); emit finished();
    }
    qint64 bytesAvailable() const override { return bytes.size() - offset + QIODevice::bytesAvailable(); }
protected:
    qint64 readData(char *out, qint64 max) override {
        auto count = qMin(max, qint64(bytes.size()) - offset);
        if (!count) return -1;
        memcpy(out, bytes.constData() + offset, count); offset += count; return count;
    }
};
class FakeNetwork : public QNetworkAccessManager {
public:
    QByteArray payload;
    int requests = 0;
    int status = 200;
    QPointer<FakeReply> lastReply;
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override {
        ++requests; lastReply = new FakeReply(request, payload, this, status);
        return lastReply;
    }
};
class UpdateTests : public QObject {
    Q_OBJECT
    const QString repo = "vovankrot/AmneziaSELFVPS";
    QJsonObject release() {
        return {{"tag_name", "v5.0.0.9"}, {"draft", false}, {"prerelease", false},
            {"html_url", "https://github.com/" + repo + "/releases/tag/v5.0.0.9"},
            {"assets", QJsonArray{QJsonObject{{"name", "AmneziaVPN_5.0.0.9_x64_setup.exe"},
                {"state", "uploaded"}, {"size", 7}, {"digest", "sha256:" + QString(64, 'a')},
                {"browser_download_url", "https://github.com/" + repo + "/releases/download/v5.0.0.9/AmneziaVPN_5.0.0.9_x64_setup.exe"}}}}};
    }
private slots:
    void manifestRejectsUnsafeAndOldReleases() {
        auto json = release(); QVERIFY(SelfVpsRelease::parse(json, repo, "5.0.0.8").valid());
        QVERIFY(!SelfVpsRelease::parse(json, repo, "5.0.0.9").valid());
        QVERIFY(!SelfVpsRelease::parse(json, repo, "5.0.0.10").valid());
        QVERIFY(!SelfVpsRelease::parse(json, "evil/other", "5.0.0.8").valid());
        json["prerelease"] = true; QVERIFY(!SelfVpsRelease::parse(json, repo, "5.0.0.8").valid()); json["prerelease"] = false;
        auto assets = json["assets"].toArray(); auto asset = assets[0].toObject();
        for (const auto &url : {"http://github.com/evil/setup.exe", "https://evil.example/setup.exe", "https://github.com/vovankrot/selfvps/releases/download/v5.0.0.9/setup.exe"}) {
            auto changed = asset; changed["browser_download_url"] = url; json["assets"] = QJsonArray{changed};
            QVERIFY(!SelfVpsRelease::parse(json, repo, "5.0.0.8").valid());
        }
        auto changed = asset; changed.remove("digest"); json["assets"] = QJsonArray{changed}; QVERIFY(!SelfVpsRelease::parse(json, repo, "5.0.0.8").valid());
        json["assets"] = QJsonArray{asset, asset}; QVERIFY(!SelfVpsRelease::parse(json, repo, "5.0.0.8").valid());
        QVERIFY(SelfVpsRelease::version("5.0.0.10") > SelfVpsRelease::version("5.0.0.9"));
        QVERIFY(SelfVpsRelease::version("v5.0.0.9-beta").isNull());
    }
    void downloadVerifiedAndCorruptionCancelled() {
        FakeNetwork network; AppUpdater updater(nullptr, &network);
        updater.m_downloadRoot = QCoreApplication::instance()->property("testRoot").toString();
        updater.m_release = SelfVpsRelease::parse(release(), repo, "5.0.0.8");
        network.payload = "payload";
        updater.m_release.sha256 = QString::fromLatin1(QCryptographicHash::hash(network.payload, QCryptographicHash::Sha256).toHex());
        updater.download(); QTRY_VERIFY(!updater.busy()); QVERIFY(updater.ready());
        QFile file(updater.m_readyPath); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("payload")); file.close();
        network.payload = "corrupt"; updater.download(); QTRY_VERIFY(!updater.busy()); QVERIFY(!updater.ready());
        QFile previous(file.fileName()); QVERIFY(previous.open(QIODevice::ReadOnly)); QCOMPARE(previous.readAll(), QByteArray("payload"));
        network.payload = "oversized payload"; updater.download(); QTRY_VERIFY(!updater.busy()); QVERIFY(!updater.ready());
        updater.download(); updater.cancel(); QTRY_VERIFY(!updater.busy()); QVERIFY(!updater.ready());
        previous.close();
        QFile tampered(file.fileName()); QVERIFY(tampered.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(tampered.write("changed"), qint64(7)); tampered.close();
        updater.m_readyPath = file.fileName(); updater.install();
        QTRY_VERIFY(!updater.busy());
        QVERIFY(!updater.ready()); // Returns before invoking UAC or an installer.
    }
    void installerValidationIsAsyncSingleFlightAndCancelable() {
        FakeNetwork network;
        AppUpdater updater(nullptr, &network);
        QTemporaryDir dir;
        const auto path = dir.path() + "/test-installer.exe";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray chunk(1024 * 1024, 't');
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for (int i = 0; i < 64; ++i) { QCOMPARE(file.write(chunk), qint64(chunk.size())); hash.addData(chunk); }
        file.close();
        updater.m_readyPath = path;
        updater.m_release.size = 64 * qint64(chunk.size());
        updater.m_release.sha256 = QString::fromLatin1(hash.result().toHex());
        int launches = 0, ticksWhileBusy = 0;
        updater.m_verifiedInstaller = [&](const QString &verified) { QCOMPARE(verified, path); ++launches; };
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&] { if (updater.busy()) ++ticksWhileBusy; });
        heartbeat.start(1);
        updater.install();
        QVERIFY(updater.busy());
        updater.install(); // A second click must not launch a second validation.
        QTRY_VERIFY(!updater.busy());
        QCOMPARE(launches, 1);
        QVERIFY(ticksWhileBusy > 0);

        updater.install();
        QVERIFY(updater.busy());
        updater.cancel();
        QVERIFY(!updater.busy());
        QTest::qWait(100);
        QCOMPARE(launches, 1);

        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("x"), qint64(1)); file.close();
        updater.install(); QTRY_VERIFY(!updater.busy());
        QVERIFY(!updater.ready());
        QCOMPARE(launches, 1);

        auto *destroyed = new AppUpdater(nullptr, &network);
        destroyed->m_readyPath = path;
        destroyed->m_release = updater.m_release;
        destroyed->m_verifiedInstaller = [&](const QString &) { ++launches; };
        destroyed->install();
        delete destroyed;
        QTest::qWait(100);
        QCOMPARE(launches, 1);
    }
    void checkIsSingleFlightAndCanBeDisabled() {
        FakeNetwork network; AppUpdater updater(nullptr, &network);
        network.payload = QJsonDocument(release()).toJson();
        QSignalSpy offers(&updater, &AppUpdater::updateAvailable);
        updater.setAutomatic(false); updater.check(false); QCOMPARE(network.requests, 0);
        updater.check(); updater.check(); QTRY_VERIFY(!updater.busy()); QCOMPARE(network.requests, 1); QCOMPARE(offers.count(), 1);
        updater.check(); QTRY_VERIFY(!updater.busy()); QCOMPARE(offers.count(), 1);
    }
    void releaseChangesRefreshOfferAndWithdrawInvalidInstaller() {
        FakeNetwork network; AppUpdater updater(nullptr, &network);
        auto json = release(); network.payload = QJsonDocument(json).toJson();
        QSignalSpy offers(&updater, &AppUpdater::updateAvailable);
        updater.check(); QTRY_VERIFY(!updater.busy()); QCOMPARE(offers.count(), 1);
        json["body"] = "Updated release notes";
        network.payload = QJsonDocument(json).toJson();
        updater.check(); QTRY_VERIFY(!updater.busy());
        QCOMPARE(updater.releaseNotes(), QString("Updated release notes")); QCOMPARE(offers.count(), 1);
        // A new tag can reuse the same binary digest, but still has new metadata.
        auto asset = json["assets"].toArray()[0].toObject();
        json["tag_name"] = "v5.0.0.10";
        json["html_url"] = "https://github.com/" + repo + "/releases/tag/v5.0.0.10";
        asset["name"] = "AmneziaVPN_5.0.0.10_x64_setup.exe";
        asset["browser_download_url"] = "https://github.com/" + repo + "/releases/download/v5.0.0.10/AmneziaVPN_5.0.0.10_x64_setup.exe";
        json["assets"] = QJsonArray{asset}; network.payload = QJsonDocument(json).toJson();
        updater.m_readyPath = "previous-installer.exe";
        updater.check(); QTRY_VERIFY(!updater.busy());
        QCOMPARE(updater.availableVersion(), QString("5.0.0.10")); QCOMPARE(offers.count(), 2); QVERIFY(!updater.ready());
        json["assets"] = QJsonArray{}; network.payload = QJsonDocument(json).toJson();
        updater.m_readyPath = "withdrawn-installer.exe";
        updater.check(); QTRY_VERIFY(!updater.busy());
        QVERIFY(updater.availableVersion().isEmpty()); QVERIFY(!updater.ready());
        updater.m_release = SelfVpsRelease::parse(release(), repo, "5.0.0.8");
        updater.m_readyPath = "deleted-release.exe"; network.status = 404;
        updater.check(); QTRY_VERIFY(!updater.busy());
        QVERIFY(!updater.m_release.valid()); QVERIFY(!updater.ready());
    }
    void offerSignalCanStartDownloadAndDestructionAbortsReply() {
        FakeNetwork network; AppUpdater updater(nullptr, &network);
        updater.m_downloadRoot = QCoreApplication::instance()->property("testRoot").toString();
        auto json = release(); auto asset = json["assets"].toArray()[0].toObject();
        asset["digest"] = "sha256:" + QString::fromLatin1(QCryptographicHash::hash("payload", QCryptographicHash::Sha256).toHex());
        json["assets"] = QJsonArray{asset}; network.payload = QJsonDocument(json).toJson();
        connect(&updater, &AppUpdater::updateAvailable, this, [&] {
            QVERIFY(!updater.busy()); network.payload = "payload"; updater.download();
        });
        updater.check(); QTRY_VERIFY(updater.ready()); QCOMPARE(network.requests, 2);
        auto *destroyed = new AppUpdater(nullptr, &network);
        destroyed->check(); const QPointer<FakeReply> pending = network.lastReply;
        QVERIFY(pending && !pending->done); delete destroyed;
        QVERIFY(pending && pending->done); QCOMPARE(pending->aborts, 1);
    }
    void addressSelectionIgnoresScopedLinkLocal() {
        const QList<QHostAddress> list = {QHostAddress("fe80::abcd%12"), QHostAddress("169.254.1.2"), QHostAddress("192.168.1.12"), QHostAddress("fd00::2"), QHostAddress("2001:db8::8")};
        QCOMPARE(SplitTunnelAddress::ipv4(list), QHostAddress("192.168.1.12"));
        QCOMPARE(SplitTunnelAddress::ipv6(list), QHostAddress("2001:db8::8"));
        QVERIFY(SplitTunnelAddress::ipv6({QHostAddress("fe80::abcd%12")}).isNull());
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("SELFVPS-Tests"); QCoreApplication::setApplicationName("UpdateTests");
    QTemporaryDir temp; if (!temp.isValid()) return 1;
    qputenv("LOCALAPPDATA", temp.path().toUtf8());
    app.setProperty("testRoot", temp.path());
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
    UpdateTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "main.moc"

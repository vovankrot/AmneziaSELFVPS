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
    FakeReply(const QNetworkRequest &request, QByteArray content, QObject *parent) : QNetworkReply(parent), bytes(content) {
        setRequest(request); setUrl(request.url()); setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(5, this, [this] {
            if (done) return;
            emit readyRead();
            if (done) return;
            done = true; setFinished(true); emit finished();
        });
    }
    void abort() override {
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
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override {
        ++requests; return new FakeReply(request, payload, this);
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
        QVERIFY(!updater.ready()); // Returns before invoking UAC or an installer.
    }
    void checkIsSingleFlightAndCanBeDisabled() {
        FakeNetwork network; AppUpdater updater(nullptr, &network);
        network.payload = QJsonDocument(release()).toJson();
        QSignalSpy offers(&updater, &AppUpdater::updateAvailable);
        updater.setAutomatic(false); updater.check(false); QCOMPARE(network.requests, 0);
        updater.check(); updater.check(); QTRY_VERIFY(!updater.busy()); QCOMPARE(network.requests, 1); QCOMPARE(offers.count(), 1);
        updater.check(); QTRY_VERIFY(!updater.busy()); QCOMPARE(offers.count(), 1);
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

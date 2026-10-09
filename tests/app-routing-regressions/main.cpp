#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include "client/core/appFolderScan.h"
#include "client/ui/models/appSplitTunnelingModel.h"

class AppRoutingTests : public QObject {
    Q_OBJECT
    static void file(const QString &path) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile out(path); QVERIFY(out.open(QIODevice::WriteOnly)); out.write("fixture");
    }
private slots:
    void scannerCollectsRelatedExecutablesWithoutPartialOversizedResults() {
        QTemporaryDir dir;
        file(dir.path() + "/Riot Client/RiotClientServices.exe");
        file(dir.path() + "/League of Legends/LeagueClient.exe");
        file(dir.path() + "/League of Legends/helper.EXE");
        file(dir.path() + "/notes.txt");
        const auto result = AppFolderScan::scan(dir.path());
        QCOMPARE(result.error, AppFolderScan::Error::None);
        QCOMPARE(result.apps.size(), 3);
        QCOMPARE(AppFolderScan::scan(dir.path(), 3).apps.size(), 3);
        for (const auto &app : result.apps) {
            QCOMPARE(app.groupFolder, QFileInfo(dir.path()).canonicalFilePath());
            QVERIFY(app.appPath.startsWith(app.groupFolder + '/'));
        }
        const auto overflow = AppFolderScan::scan(dir.path(), 2);
        QCOMPARE(overflow.error, AppFolderScan::Error::LimitExceeded);
        QVERIFY(overflow.apps.isEmpty());
        QCOMPARE(AppFolderScan::scan(dir.path(), 4096, 0).error, AppFolderScan::Error::LimitExceeded);
        QCOMPARE(AppFolderScan::scan(dir.path() + "/missing").error, AppFolderScan::Error::MissingFolder);
    }
    void batchPersistsOnceAndPreservesGroups() {
        QTemporaryDir dir;
        for (int i = 0; i < 100; ++i) file(dir.path() + "/components/app" + QString::number(i) + ".exe");
        const auto scanned = AppFolderScan::scan(dir.path());
        auto settings = std::make_shared<Settings>();
        AppSplitTunnelingModel model(settings);
        settings->writes = 0;
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        QCOMPARE(model.addAppsBatch(scanned.apps), 100);
        QCOMPARE(model.rowCount(), 100);
        QCOMPARE(reset.size(), 1);
        QCOMPARE(settings->writes, 2); // exactly one write per stored routing list
        QCOMPARE(model.groupCount(dir.path()), 100);
        QCOMPARE(model.addAppsBatch(scanned.apps), 0);
        QCOMPARE(reset.size(), 1);
        QCOMPARE(settings->writes, 2);
        auto invalid = scanned.apps.first(); invalid.appPath += ".missing";
        QCOMPARE(model.addAppsBatch({invalid}), 0);
        auto existing = scanned.apps.first(); existing.groupFolder = dir.path() + "/components";
        QCOMPARE(model.addAppsBatch({existing}), 0);
        QCOMPARE(settings->writes, 4);
        QCOMPARE(model.removeGroup(dir.path()), 100);
        QCOMPARE(model.rowCount(), 0);
    }
};
QTEST_GUILESS_MAIN(AppRoutingTests)
#include "main.moc"

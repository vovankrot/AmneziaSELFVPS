#include <QtTest>
#include <QProcess>
#include <QTemporaryDir>
#include <windows.h>
#include <tlhelp32.h>
#include "client/core/processImagePolicy.h"
QString printErrorMessage(DWORD error) { return QString::number(error); }
class Utils {
public:
    static bool killProcessByName(const QString &);
};
#include "production-kill-process.inc"
class IpcServerProcess {
public:
    QSharedPointer<QProcess> m_process {new QProcess};
    bool m_argumentsAccepted = true;
    int rejectedStarts = 0;
    void errorOccurred(QProcess::ProcessError error) { if (error == QProcess::FailedToStart) ++rejectedStarts; }
    void start();
    bool waitForFinished();
    bool waitForFinished(int);
};
#include "production-start-process.inc"
#include "production-wait-process.inc"

class ProcessPolicyTests : public QObject {
    Q_OBJECT
    QString fixture() const { return QCoreApplication::applicationDirPath() + "/selfvps-process-policy-fixture.exe"; }
private slots:
    void sameNameInAnotherDirectoryIsNeverTerminated() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir().mkpath(dir.path() + "/a"));
        QVERIFY(QDir().mkpath(dir.path() + "/b"));
        const auto a = dir.path() + "/a/selfvps-process-policy-fixture.exe";
        const auto b = dir.path() + "/b/selfvps-process-policy-fixture.exe";
        QVERIFY(QFile::copy(fixture(), a));
        QVERIFY(QFile::copy(fixture(), b));
        QVERIFY(ProcessImagePolicy::matches(ProcessImagePolicy::expectedPath(a), a));
        QVERIFY(!ProcessImagePolicy::matches(ProcessImagePolicy::expectedPath(a), b));
        QVERIFY(!ProcessImagePolicy::matches({}, a));
        QVERIFY(!ProcessImagePolicy::matches(ProcessImagePolicy::expectedPath(a), dir.path() + "/missing.exe"));
        QProcess first, second;
        first.start(a); second.start(b);
        QVERIFY(first.waitForStarted(2000));
        QVERIFY(second.waitForStarted(2000));
        QVERIFY(Utils::killProcessByName(a));
        QVERIFY(first.waitForFinished(2000));
        QCOMPARE(second.state(), QProcess::Running);
        QVERIFY(!Utils::killProcessByName(dir.path() + "/missing.exe"));
        QCOMPARE(second.state(), QProcess::Running);
        second.kill(); QVERIFY(second.waitForFinished(2000));
    }
    void processStartsAsynchronouslyAndDuplicateStartDoesNotKillIt() {
        IpcServerProcess rejected;
        rejected.m_process->setProgram(fixture());
        rejected.m_argumentsAccepted = false;
        rejected.start();
        QCOMPARE(rejected.rejectedStarts, 1);
        QCOMPARE(rejected.m_process->state(), QProcess::NotRunning);
        IpcServerProcess source;
        source.m_process->setProgram(fixture());
        QSignalSpy started(source.m_process.data(), &QProcess::started);
        QElapsedTimer timer; timer.start();
        source.start();
        QVERIFY(timer.elapsed() < 1000);
        QTRY_COMPARE(started.size(), 1);
        const auto id = source.m_process->processId();
        source.start();
        QTest::qWait(40);
        QCOMPARE(source.m_process->processId(), id);
        QCOMPARE(source.m_process->state(), QProcess::Running);
        source.m_process->kill();
        QVERIFY(source.waitForFinished(2000));
        // Qt's waitForFinished alone returns false once the process is already gone.
        QVERIFY(source.waitForFinished(20));
        QVERIFY(source.waitForFinished());

        IpcServerProcess invalid;
        invalid.m_process->setProgram(fixture() + ".missing");
        QSignalSpy errors(invalid.m_process.data(), &QProcess::errorOccurred);
        invalid.start();
        QTRY_VERIFY(!errors.isEmpty());
        QCOMPARE(invalid.m_process->error(), QProcess::FailedToStart);
    }
};
QTEST_GUILESS_MAIN(ProcessPolicyTests)
#include "main.moc"

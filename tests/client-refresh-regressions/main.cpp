#include <QObject>
#include <QCoreApplication>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QTest>
#include <QTimer>
#include <atomic>
#include <memory>
#include <functional>
#include <stdexcept>
enum class DockerContainer { Xray, WireGuard };
enum class ErrorCode { NoError, Failed };
struct ServerCredentials { QString hostName, userName, secretData; int port = 22; };
struct Settings {};
std::atomic<int> jobs {0}, active {0}, peak {0};
class ServerController { public: ServerController(std::shared_ptr<Settings>) {} };
class ClientManagementModel : public QObject {
    QJsonArray rows;
public:
    ClientManagementModel(std::shared_ptr<Settings>) {}
    void replaceSnapshot(const QJsonArray &r) {
        if (thread() != QThread::currentThread()) qFatal("Cross-thread model mutation");
        rows = r;
    }
    QJsonArray snapshot() const { return rows; }
    ErrorCode updateModel(DockerContainer container, const ServerCredentials &creds,
            const QSharedPointer<ServerController> &, const std::function<void(const QJsonArray &)> &initial) {
        jobs++; peak.store(qMax(peak.load(), ++active));
        QThread::msleep(20);
        if (creds.hostName == "fail") { active--; return ErrorCode::Failed; }
        rows = {creds.hostName + "/names"}; initial(rows);
        QThread::msleep(100);
        rows = {creds.hostName + (container == DockerContainer::Xray ? "/xray" : "/wg")};
        active--;
        return ErrorCode::NoError;
    }
};
class ExportController : public QObject {
    Q_OBJECT
public:
    std::shared_ptr<Settings> m_settings = std::make_shared<Settings>();
    QSharedPointer<ClientManagementModel> m_clientManagementModel {new ClientManagementModel(m_settings)};
    bool m_clientsLoading = false;
    QByteArray m_clientsCacheKey, m_clientsActiveKey, m_clientsRequestedKey;
    DockerContainer m_pendingClientsContainer {};
    ServerCredentials m_pendingClientsCredentials;
    QElapsedTimer m_clientsCacheAge;
    void updateClientManagementModel(DockerContainer, ServerCredentials, bool forceRefresh = false);
signals:
    void clientsLoadingChanged();
    void exportErrorOccurred(ErrorCode);
};
#include "production-refresh.inc"
static void check(bool ok, const char *message) { if (!ok) qFatal("%s", message); }
static void finish(ExportController &c) {
    QElapsedTimer t; t.start();
    while (c.m_clientsLoading && t.elapsed() < 2000) QTest::qWait(5);
    check(!c.m_clientsLoading, "Refresh did not finish");
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    ExportController c;
    int ticks = 0, errors = 0;
    QTimer timer; timer.setInterval(2);
    QObject::connect(&timer, &QTimer::timeout, [&] { ticks++; }); timer.start();
    QObject::connect(&c, &ExportController::exportErrorOccurred, [&] { errors++; });
    ServerCredentials a {"A", "root", "secret-a"};
    c.updateClientManagementModel(DockerContainer::Xray, a);
    for (int i = 0; i < 15 && c.m_clientManagementModel->snapshot().isEmpty(); ++i) QTest::qWait(5);
    check(c.m_clientsLoading && c.m_clientManagementModel->snapshot().first().toString() == "A/names", "Names did not arrive before runtime data");
    finish(c); check(ticks > 10, "GUI event loop was blocked");
    const int cachedJobs = jobs;
    c.updateClientManagementModel(DockerContainer::Xray, a);
    check(!c.m_clientsLoading && jobs == cachedJobs, "Fresh cache performed SSH work");
    c.updateClientManagementModel(DockerContainer::Xray, a, true); finish(c);
    check(jobs == cachedJobs + 1, "Force refresh reused cache");
    c.m_clientsCacheAge.invalidate();
    c.updateClientManagementModel(DockerContainer::Xray, a); finish(c);
    check(jobs == cachedJobs + 2, "Invalidated cache was reused");
    a.secretData = "changed";
    c.updateClientManagementModel(DockerContainer::Xray, a); finish(c);
    check(jobs == cachedJobs + 3, "Authentication change reused cache");
    c.updateClientManagementModel(DockerContainer::WireGuard, a); finish(c);
    check(c.m_clientManagementModel->snapshot().first().toString() == "A/wg", "Protocol selection reused another list");
    const int beforeSwitch = jobs;
    c.updateClientManagementModel(DockerContainer::Xray, a, true);
    c.updateClientManagementModel(DockerContainer::Xray, {"B", "root", "b"});
    c.updateClientManagementModel(DockerContainer::Xray, {"C", "root", "c"});
    check(c.m_clientManagementModel->snapshot().isEmpty(), "Another server's clients remained visible");
    finish(c);
    check(jobs == beforeSwitch + 2 && peak == 1 && c.m_clientManagementModel->snapshot().first().toString() == "C/xray", "Requests were not serialized to the latest server");
    c.updateClientManagementModel(DockerContainer::Xray, {"fail", "root", "f"}); finish(c);
    check(errors == 1 && c.m_clientManagementModel->snapshot().isEmpty(), "Failed target displayed stale clients");
    c.updateClientManagementModel(DockerContainer::Xray, {"C", "root", "c"}); finish(c);
    check(c.m_clientManagementModel->snapshot().first().toString() == "C/xray", "Cleared list incorrectly reused an empty cache");
    auto *temporary = new ExportController;
    temporary->updateClientManagementModel(DockerContainer::Xray, a);
    delete temporary;
    QTest::qWait(200);
    qInfo("PASS: early names, responsive event loop, cache/force/invalidation/auth identity, protocol isolation, latest selection, serial jobs, error recovery and destruction during refresh");
}
#include "main.moc"

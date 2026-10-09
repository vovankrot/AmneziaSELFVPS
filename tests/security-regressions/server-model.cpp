#include <QtTest>
#include <QAbstractListModel>
#include <QJsonArray>
#include <QJsonObject>
#include <memory>
#include "core/defs.h"
#include "protocols/protocols_defs.h"
using namespace amnezia;

// Storage fixture never creates QSettings or accesses real profiles.
class Settings {
public:
    QJsonArray stored;
    int selected = 0, removedSignals = 0;
    QJsonArray serversArray() const { return stored; }
    void setServersArray(const QJsonArray &array) { stored = array; }
    int defaultServerIndex() const { return selected; }
    void setDefaultServer(int index) { selected = index; }
    bool useAmneziaDns() const { return false; }
    void serverRemoved(int) { ++removedSignals; }
    QJsonObject server(int) const;
    void addServer(const QJsonObject &);
    void removeServer(int);
    bool editServer(int, const QJsonObject &);
};
#include "production-settings-servers.inc"

class ServersModel : public QAbstractListModel {
    Q_OBJECT
public:
    std::shared_ptr<Settings> m_settings = std::make_shared<Settings>();
    QJsonArray m_servers;
    int m_defaultServerIndex = 0, m_processedServerIndex = 0;
    bool m_isAmneziaDnsEnabled = false;
    enum { IsServerFromGatewayApiRole, IsCountrySelectionAvailableRole };
    int rowCount(const QModelIndex & = {}) const override { return m_servers.size(); }
    QVariant data(const QModelIndex &, int = Qt::DisplayRole) const override { return {}; }
    QVariant data(int, int) const { return false; }
    void resetModel();
    void setDefaultServerIndex(int);
    void setProcessedServerIndex(int);
    void removeServer();
    void removeServer(int);
    ServerCredentials serverCredentials(int) const;
    void updateContainersModel();
    void updateDefaultServerContainersModel();
    QJsonObject getServerConfig(int) const;
signals:
    void defaultServerIndexChanged(int);
    void processedServerIndexChanged(int);
    void containersUpdated(QJsonArray);
    void defaultServerContainersUpdated(QJsonArray);
    void updateApiCountryModel();
    void updateApiServicesModel();
};
#include "production-server-read.inc"
#include "production-server-reset.inc"
#include "production-server-select.inc"
#include "production-server-remove.inc"

class ServerModelTests : public QObject {
    Q_OBJECT
private slots:
    void invalidStorageIndicesCannotReadOrMutate() {
        Settings settings;
        settings.addServer({{"hostName", "server-one.invalid"}});
        const auto original = settings.serversArray();
        for (int index : {-2, -1, 1, 200}) {
            QVERIFY(settings.server(index).isEmpty());
            QVERIFY(!settings.editServer(index, {{"hostName", "wrong.invalid"}}));
            settings.removeServer(index);
            QCOMPARE(settings.serversArray(), original);
        }
        QCOMPARE(settings.removedSignals, 0);
    }
    void deletingLastServerClearsSelectionBeforeNotifications() {
        ServersModel model;
        model.m_settings->addServer({{"hostName", "server-one.invalid"}});
        model.resetModel();
        bool coherent = false;
        connect(&model, &ServersModel::defaultServerIndexChanged, this, [&](int index) {
            coherent = index == -1 && model.m_servers.isEmpty() && model.m_processedServerIndex == -1
                && model.getServerConfig(index).isEmpty();
            model.updateDefaultServerContainersModel();
        });
        QSignalSpy containers(&model, &ServersModel::containersUpdated);
        model.removeServer();
        QVERIFY(coherent); QCOMPARE(model.m_settings->defaultServerIndex(), -1);
        QVERIFY(model.serverCredentials(-1).hostName.isEmpty());
        QVERIFY(!containers.isEmpty()); QVERIFY(containers.last()[0].toJsonArray().isEmpty());
        model.removeServer(); // Stale UI invocation must be harmless.
        QCOMPARE(model.m_settings->removedSignals, 1);
    }
    void removingEarlierServerPreservesSelectedIdentity() {
        ServersModel model;
        for (const auto &host : {"one.invalid", "two.invalid", "three.invalid"})
            model.m_settings->addServer({{"hostName", host}});
        model.m_settings->setDefaultServer(2); model.resetModel();
        model.removeServer(0);
        QCOMPARE(model.m_defaultServerIndex, 1);
        QCOMPARE(model.serverCredentials(model.m_defaultServerIndex).hostName, QString("three.invalid"));
        model.removeServer(1);
        QCOMPARE(model.m_defaultServerIndex, 0);
        QCOMPARE(model.serverCredentials(0).hostName, QString("two.invalid"));
    }
    void resetRepairsStaleSelectionAndRejectsInvalidDefault() {
        ServersModel model;
        model.m_settings->setDefaultServer(100); model.resetModel();
        QCOMPARE(model.m_defaultServerIndex, -1);
        model.m_settings->addServer({{"hostName", "one.invalid"}}); model.resetModel();
        QCOMPARE(model.m_defaultServerIndex, 0);
        model.setDefaultServerIndex(-1); model.setDefaultServerIndex(10);
        QCOMPARE(model.m_defaultServerIndex, 0);
        model.setProcessedServerIndex(10); QCOMPARE(model.m_processedServerIndex, -1);
        model.updateContainersModel(); QVERIFY(model.getServerConfig(10).isEmpty());
    }
};
QTEST_GUILESS_MAIN(ServerModelTests)
#include "server-model.moc"

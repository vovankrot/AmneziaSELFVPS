#ifndef CONFIGSNAPSHOTMANAGER_H
#define CONFIGSNAPSHOTMANAGER_H

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>

#include "containers/containers_defs.h"
#include "core/defs.h"
#include "settings.h"

class ServerController;

class ConfigSnapshotManager : public QObject
{
    Q_OBJECT
public:
    explicit ConfigSnapshotManager(const std::shared_ptr<Settings> &settings, QObject *parent = nullptr);

    struct SnapshotInfo {
        QString id;
        QString timestamp;
        QString containerType;
        QString clientVersion;
    };

    // Save a snapshot of current server+client configs before update
    bool saveSnapshot(const ServerCredentials &credentials, DockerContainer container,
                      const QJsonObject &clientConfig, const QSharedPointer<ServerController> &serverController);

    // List available snapshots for a server
    QList<SnapshotInfo> listSnapshots(const QString &serverHash) const;

    // Restore a snapshot to the server
    bool restoreSnapshot(const ServerCredentials &credentials, DockerContainer container,
                         const QString &snapshotId, QJsonObject &restoredClientConfig,
                         const QSharedPointer<ServerController> &serverController);

    // Get the snapshot directory path for a server
    static QString snapshotBasePath(const QString &serverHash);
    static QString serverHash(const ServerCredentials &credentials);

    static constexpr int MAX_SNAPSHOTS_PER_SERVER = 5;

private:
    void pruneOldSnapshots(const QString &serverHash);

    std::shared_ptr<Settings> m_settings;
};

#endif // CONFIGSNAPSHOTMANAGER_H

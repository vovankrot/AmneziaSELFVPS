#include "configSnapshotManager.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>

#include "containers/containers_defs.h"
#include "core/controllers/serverController.h"
#include "protocols/protocols_defs.h"
#include "logger.h"
#include "version.h"

namespace {
Logger logger("ConfigSnapshotManager");
}

ConfigSnapshotManager::ConfigSnapshotManager(const std::shared_ptr<Settings> &settings, QObject *parent)
    : QObject(parent), m_settings(settings)
{
}

QString ConfigSnapshotManager::serverHash(const ServerCredentials &credentials)
{
    QByteArray data = (credentials.hostName + credentials.userName).toUtf8();
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex().left(12);
}

QString ConfigSnapshotManager::snapshotBasePath(const QString &serverHash)
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + "/snapshots/" + serverHash;
}

bool ConfigSnapshotManager::saveSnapshot(const ServerCredentials &credentials, DockerContainer container,
                                          const QJsonObject &clientConfig,
                                          const QSharedPointer<ServerController> &serverController)
{
    QString hash = serverHash(credentials);
    QString timestamp = QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss");
    QString snapshotDir = snapshotBasePath(hash) + "/" + timestamp;

    QDir dir;
    if (!dir.mkpath(snapshotDir)) {
        logger.error() << "Failed to create snapshot directory:" << snapshotDir;
        return false;
    }

    // Save client config
    QFile clientFile(snapshotDir + "/client_config.json");
    if (clientFile.open(QIODevice::WriteOnly)) {
        clientFile.write(QJsonDocument(clientConfig).toJson());
        clientFile.close();
    }

    // Download server config from VDS
    ErrorCode errorCode;
    QByteArray serverConfig = serverController->getTextFileFromContainer(
        container, credentials, amnezia::protocols::xray::serverConfigPath, errorCode);
    if (errorCode == ErrorCode::NoError && !serverConfig.isEmpty()) {
        QFile serverFile(snapshotDir + "/server_config.json");
        if (serverFile.open(QIODevice::WriteOnly)) {
            serverFile.write(serverConfig);
            serverFile.close();
        }
    }

    // Download keys
    QDir keysDir(snapshotDir + "/keys");
    keysDir.mkpath(".");

    auto saveKey = [&](const char *remotePath, const QString &localName) {
        ErrorCode err;
        QByteArray content = serverController->getTextFileFromContainer(container, credentials, remotePath, err);
        if (err == ErrorCode::NoError && !content.isEmpty()) {
            QFile f(keysDir.filePath(localName));
            if (f.open(QIODevice::WriteOnly)) {
                f.write(content);
                f.close();
            }
        }
    };

    saveKey(amnezia::protocols::xray::PublicKeyPath, "xray_public.key");
    saveKey(amnezia::protocols::xray::PrivateKeyPath, "xray_private.key");
    saveKey(amnezia::protocols::xray::shortidPath, "xray_short_id.key");
    saveKey(amnezia::protocols::xray::uuidPath, "xray_uuid.key");
    saveKey(amnezia::protocols::xray::xhttpPathPath, "xray_xhttp_path.key");

    // Download version.json
    QJsonObject versionInfo = serverController->readServerVersion(credentials);
    if (!versionInfo.isEmpty()) {
        QFile versionFile(snapshotDir + "/version.json");
        if (versionFile.open(QIODevice::WriteOnly)) {
            versionFile.write(QJsonDocument(versionInfo).toJson());
            versionFile.close();
        }
    }

    // Save metadata
    QJsonObject metadata;
    metadata["timestamp"] = timestamp;
    metadata["clientVersion"] = QString(APP_VERSION);
    metadata["containerType"] = ContainerProps::containerToString(container);
    metadata["serverHost"] = credentials.hostName;

    QFile metaFile(snapshotDir + "/metadata.json");
    if (metaFile.open(QIODevice::WriteOnly)) {
        metaFile.write(QJsonDocument(metadata).toJson());
        metaFile.close();
    }

    pruneOldSnapshots(hash);

    logger.info() << "Snapshot saved:" << snapshotDir;
    return true;
}

QList<ConfigSnapshotManager::SnapshotInfo> ConfigSnapshotManager::listSnapshots(const QString &srvHash) const
{
    QList<SnapshotInfo> result;
    QDir baseDir(snapshotBasePath(srvHash));
    if (!baseDir.exists()) return result;

    QStringList entries = baseDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
    for (const QString &entry : entries) {
        QFile metaFile(baseDir.filePath(entry + "/metadata.json"));
        if (!metaFile.open(QIODevice::ReadOnly)) continue;

        QJsonDocument doc = QJsonDocument::fromJson(metaFile.readAll());
        metaFile.close();

        if (!doc.isObject()) continue;

        QJsonObject meta = doc.object();
        SnapshotInfo info;
        info.id = entry;
        info.timestamp = meta.value("timestamp").toString();
        info.containerType = meta.value("containerType").toString();
        info.clientVersion = meta.value("clientVersion").toString();
        result.append(info);
    }

    return result;
}

bool ConfigSnapshotManager::restoreSnapshot(const ServerCredentials &credentials, DockerContainer container,
                                             const QString &snapshotId, QJsonObject &restoredClientConfig,
                                             const QSharedPointer<ServerController> &serverController)
{
    // Validate snapshotId: must be exactly yyyyMMdd_HHmmss format — prevents path traversal
    static const QRegularExpression validId(QStringLiteral("^\\d{8}_\\d{6}$"));
    if (!validId.match(snapshotId).hasMatch()) {
        logger.error() << "Invalid snapshot ID format (possible path traversal):" << snapshotId;
        return false;
    }

    QString hash = serverHash(credentials);
    QString snapshotDir = snapshotBasePath(hash) + "/" + snapshotId;

    // Double-check: resolved path must stay under base
    QString basePath = QDir(snapshotBasePath(hash)).absolutePath();
    QString resolvedPath = QDir(snapshotDir).absolutePath();
    if (!resolvedPath.startsWith(basePath)) {
        logger.error() << "Path traversal detected:" << snapshotDir;
        return false;
    }

    if (!QDir(snapshotDir).exists()) {
        logger.error() << "Snapshot not found:" << snapshotDir;
        return false;
    }

    // Read client config from snapshot
    QFile clientFile(snapshotDir + "/client_config.json");
    if (clientFile.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(clientFile.readAll());
        clientFile.close();
        if (doc.isObject()) {
            restoredClientConfig = doc.object();
        }
    }

    // Upload server config back to VDS
    QFile serverFile(snapshotDir + "/server_config.json");
    if (serverFile.open(QIODevice::ReadOnly)) {
        QString serverConfig = QString::fromUtf8(serverFile.readAll());
        serverFile.close();

        ErrorCode err = serverController->uploadTextFileToContainer(
            container, credentials, serverConfig,
            amnezia::protocols::xray::serverConfigPath,
            libssh::ScpOverwriteMode::ScpOverwriteExisting);

        if (err != ErrorCode::NoError) {
            logger.error() << "Failed to upload server config from snapshot";
            return false;
        }
    }

    // Upload keys back
    QDir keysDir(snapshotDir + "/keys");
    auto uploadKey = [&](const QString &localName, const char *remotePath) {
        QFile f(keysDir.filePath(localName));
        if (!f.open(QIODevice::ReadOnly)) return;
        QString content = QString::fromUtf8(f.readAll()).trimmed();
        f.close();

        serverController->uploadTextFileToContainer(
            container, credentials, content, remotePath,
            libssh::ScpOverwriteMode::ScpOverwriteExisting);
    };

    uploadKey("xray_public.key", amnezia::protocols::xray::PublicKeyPath);
    uploadKey("xray_private.key", amnezia::protocols::xray::PrivateKeyPath);
    uploadKey("xray_short_id.key", amnezia::protocols::xray::shortidPath);
    uploadKey("xray_uuid.key", amnezia::protocols::xray::uuidPath);
    uploadKey("xray_xhttp_path.key", amnezia::protocols::xray::xhttpPathPath);

    const QString repairXrayPublicKeyScript = QStringLiteral(R"SH(
set -e
XRAY_PRIVATE_KEY=$(cat /opt/amnezia/xray/xray_private.key 2>/dev/null | tr -d '[:space:]')
XRAY_PUBLIC_KEY=$(xray x25519 -i "$XRAY_PRIVATE_KEY" 2>/dev/null | sed -n \
    -e 's/^Password (PublicKey):[[:space:]]*//p' \
    -e 's/^PublicKey:[[:space:]]*//p' \
    -e 's/^Public key:[[:space:]]*//p' | head -n 1 | tr -d '[:space:]')

[ -n "$XRAY_PRIVATE_KEY" ]
[ -n "$XRAY_PUBLIC_KEY" ]

printf '%s\n' "$XRAY_PUBLIC_KEY" > /opt/amnezia/xray/xray_public.key
)SH");

    ErrorCode repairErr = serverController->runContainerScript(credentials, container, repairXrayPublicKeyScript);
    if (repairErr != ErrorCode::NoError) {
        logger.error() << "Failed to repair xray_public.key after snapshot restore";
        return false;
    }

    // Upload version.json back — use uploadTextFileToContainer instead of echo to avoid injection
    QFile versionFile(snapshotDir + "/version.json");
    if (versionFile.open(QIODevice::ReadOnly)) {
        QString versionContent = QString::fromUtf8(versionFile.readAll());
        versionFile.close();

        // Validate it's valid JSON before uploading
        QJsonDocument vDoc = QJsonDocument::fromJson(versionContent.toUtf8());
        if (vDoc.isObject()) {
            serverController->uploadTextFileToContainer(
                container, credentials, versionContent.trimmed(),
                "/opt/amnezia/version.json",
                libssh::ScpOverwriteMode::ScpOverwriteExisting);
        }
    }

    // Restart xray
    QString restartScript = "killall -KILL xray 2>/dev/null; "
                            "nohup xray -config /opt/amnezia/xray/server.json >/dev/null 2>&1 &";
    serverController->runScript(credentials, restartScript);

    logger.info() << "Snapshot restored:" << snapshotId;
    return true;
}

void ConfigSnapshotManager::pruneOldSnapshots(const QString &srvHash)
{
    QDir baseDir(snapshotBasePath(srvHash));
    if (!baseDir.exists()) return;

    QStringList entries = baseDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    while (entries.size() > MAX_SNAPSHOTS_PER_SERVER) {
        QString oldest = entries.takeFirst();
        QDir oldDir(baseDir.filePath(oldest));
        oldDir.removeRecursively();
        logger.info() << "Pruned old snapshot:" << oldest;
    }
}

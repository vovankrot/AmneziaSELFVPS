#include "configSnapshotManager.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include "core/controllers/serverController.h"
#include "protocols/protocols_defs.h"
#include "logger.h"
#include "version.h"

namespace {
Logger logger("ConfigSnapshotManager");
const QMap<QString, QString> remoteFiles = {
    {"server_config.json", amnezia::protocols::xray::serverConfigPath},
    {"xray_private.key", amnezia::protocols::xray::PrivateKeyPath},
    {"xray_public.key", amnezia::protocols::xray::PublicKeyPath},
    {"xray_short_id.key", amnezia::protocols::xray::shortidPath},
    {"xray_uuid.key", amnezia::protocols::xray::uuidPath},
    {"xray_xhttp_path.key", amnezia::protocols::xray::xhttpPathPath},
};
bool xrayContainer(DockerContainer container) {
    return container == DockerContainer::Xray || container == DockerContainer::XrayReality;
}
bool writeAtomic(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    return file.open(QIODevice::WriteOnly) && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        && file.write(bytes) == bytes.size() && file.commit();
}
QByteArray digest(const QByteArray &bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(); }
QStringList inboundPorts(const QByteArray &config) {
    QStringList ports;
    for (const auto &inbound : QJsonDocument::fromJson(config).object().value("inbounds").toArray()) {
        bool valid = false;
        const int port = inbound.toObject().value("port").toVariant().toInt(&valid);
        if (!valid || port < 1 || port > 65535) return {};
        ports.append(QString::number(port));
    }
    ports.removeDuplicates();
    return ports;
}
QString restartAndVerify(const QString &container, const QStringList &ports) {
    QString probe = "set -e; pidof xray >/dev/null;";
    for (const auto &port : ports) probe += QString(" netstat -ln | grep -Eq \"[:.]%1[[:space:]]\";").arg(port);
    return QString("set -e\nsudo docker restart '%1' >/dev/null\nsleep 2\nsudo docker exec '%1' sh -c '%2'\n").arg(container, probe);
}
}

ConfigSnapshotManager::ConfigSnapshotManager(const std::shared_ptr<Settings> &settings, QObject *parent)
    : QObject(parent), m_settings(settings) {}

QString ConfigSnapshotManager::serverHash(const ServerCredentials &credentials)
{
    QJsonArray identity {credentials.hostName.trimmed().toLower(), credentials.port, credentials.userName};
    return QString::fromLatin1(digest(QJsonDocument(identity).toJson(QJsonDocument::Compact)).left(24));
}
QString ConfigSnapshotManager::snapshotBasePath(const QString &hash)
{
#ifdef SELFVPS_SNAPSHOT_TEST_ROOT
    return QStringLiteral(SELFVPS_SNAPSHOT_TEST_ROOT) + "/" + hash;
#else
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/snapshots/" + hash;
#endif
}

bool ConfigSnapshotManager::saveSnapshot(const ServerCredentials &credentials, DockerContainer container,
    const QJsonObject &clientConfig, const QSharedPointer<ServerController> &serverController)
{
    if (!xrayContainer(container) || clientConfig.isEmpty()) return false;
    QJsonObject files, hashes;
    bool requiresXhttpPath = false;
    for (auto it = remoteFiles.begin(); it != remoteFiles.end(); ++it) {
        ErrorCode error;
        const QByteArray bytes = serverController->getTextFileFromContainer(container, credentials, it.value(), error);
        // Older non-XHTTP servers have no path file. All keys required for REALITY
        // and the actual configuration are mandatory; no partial snapshot is saved.
        if (it.key() == "xray_xhttp_path.key" && !requiresXhttpPath && (error != ErrorCode::NoError || bytes.isEmpty())) continue;
        if (error != ErrorCode::NoError || bytes.trimmed().isEmpty()) return false;
        if (it.key() == "server_config.json" && !QJsonDocument::fromJson(bytes).isObject()) return false;
        if (it.key() == "server_config.json") {
            for (const auto &inbound : QJsonDocument::fromJson(bytes).object().value("inbounds").toArray())
                if (inbound.toObject().value("streamSettings").toObject().value("network").toString() == "xhttp") requiresXhttpPath = true;
        }
        files[it.key()] = QString::fromLatin1(bytes.toBase64());
        hashes[it.key()] = QString::fromLatin1(digest(bytes));
    }
    const QString id = QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss_zzz")
        + "_" + QUuid::createUuid().toString(QUuid::Id128).left(8);
    QJsonObject metadata {{"timestamp", id}, {"clientVersion", QString(APP_VERSION)},
        {"containerType", ContainerProps::containerToString(container)}, {"snapshotFormat", 1}};
    QJsonObject bundle {{"metadata", metadata}, {"clientConfig", clientConfig},
        {"files", files}, {"sha256", hashes}};
    const QByteArray sealed = m_settings->protectSnapshot(QJsonDocument(bundle).toJson(QJsonDocument::Compact));
    if (sealed.isEmpty()) { logger.error() << "Snapshot encryption unavailable"; return false; }
    const QString base = snapshotBasePath(serverHash(credentials));
    if (!QDir().mkpath(base)) return false;
    QTemporaryDir staged(base + "/.pending-XXXXXX");
    if (!staged.isValid()
        || !writeAtomic(staged.filePath("snapshot.bin"), sealed)
        || !writeAtomic(staged.filePath("metadata.json"), QJsonDocument(metadata).toJson())) return false;
    if (!QDir().rename(staged.path(), base + "/" + id)) return false;
    staged.setAutoRemove(false);
    pruneOldSnapshots(serverHash(credentials));
    return true;
}

QList<ConfigSnapshotManager::SnapshotInfo> ConfigSnapshotManager::listSnapshots(const QString &hash) const
{
    QList<SnapshotInfo> result;
    QDir base(snapshotBasePath(hash));
    for (const QString &id : base.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed)) {
        QFile file(base.filePath(id + "/metadata.json"));
        if (!QFile::exists(base.filePath(id + "/snapshot.bin")) || !file.open(QIODevice::ReadOnly)) continue;
        const QJsonObject metadata = QJsonDocument::fromJson(file.readAll()).object();
        if (metadata.value("snapshotFormat").toInt() != 1) continue;
        result.append({id, metadata.value("timestamp").toString(), metadata.value("containerType").toString(),
            metadata.value("clientVersion").toString()});
    }
    return result;
}

bool ConfigSnapshotManager::restoreSnapshot(const ServerCredentials &credentials, DockerContainer container,
    const QString &snapshotId, QJsonObject &restoredClientConfig, const QSharedPointer<ServerController> &serverController)
{
    restoredClientConfig = {};
    static const QRegularExpression validId("^[0-9]{8}_[0-9]{6}_[0-9]{3}_[0-9a-f]{8}$");
    if (!xrayContainer(container) || !validId.match(snapshotId).hasMatch()) return false;
    QFile file(snapshotBasePath(serverHash(credentials)) + "/" + snapshotId + "/snapshot.bin");
    if (file.size() > 32 * 1024 * 1024 || !file.open(QIODevice::ReadOnly)) return false;
    const QJsonObject bundle = QJsonDocument::fromJson(m_settings->openSnapshot(file.readAll())).object();
    const QJsonObject metadata = bundle.value("metadata").toObject();
    const QJsonObject config = bundle.value("clientConfig").toObject();
    const QJsonObject files = bundle.value("files").toObject(), hashes = bundle.value("sha256").toObject();
    if (metadata.value("snapshotFormat").toInt() != 1
        || metadata.value("containerType").toString() != ContainerProps::containerToString(container)
        || config.isEmpty()) return false;
    QMap<QString, QByteArray> contents;
    for (auto it = remoteFiles.begin(); it != remoteFiles.end(); ++it) {
        if (it.key() == "xray_xhttp_path.key" && !files.contains(it.key())) continue;
        const QByteArray bytes = QByteArray::fromBase64(files.value(it.key()).toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (bytes.isEmpty() || QString::fromLatin1(digest(bytes)) != hashes.value(it.key()).toString()) return false;
        contents[it.value()] = bytes;
    }
    if (!QJsonDocument::fromJson(contents.value(amnezia::protocols::xray::serverConfigPath)).isObject()) return false;
    const auto targetPorts = inboundPorts(contents.value(amnezia::protocols::xray::serverConfigPath));
    if (targetPorts.isEmpty()) return false;
    // Preserve a verified checkpoint before touching the running configuration.
    int serverIndex = -1;
    for (int i = 0; i < m_settings->serversCount(); ++i) {
        if (serverHash(m_settings->serverCredentials(i)) == serverHash(credentials)) { serverIndex = i; break; }
    }
    if (serverIndex < 0) return false;
    const auto current = m_settings->containerConfig(serverIndex, container);
    ErrorCode currentRead;
    const auto currentPorts = inboundPorts(serverController->getTextFileFromContainer(container, credentials,
        amnezia::protocols::xray::serverConfigPath, currentRead));
    if (currentRead != ErrorCode::NoError || currentPorts.isEmpty()) return false;
    if (current.isEmpty() || !saveSnapshot(credentials, container, current, serverController)) return false;
    const QString suffix = ".restore-" + QUuid::createUuid().toString(QUuid::Id128);
    QString swap = "set -euo pipefail\numask 077\n";
    QString rollback = "set -euo pipefail\n";
    QString cleanup = "set -euo pipefail\n";
    const QString containerName = ContainerProps::containerToString(container);
    QString hostRollback = "set -e\numask 077\nrestore_dir=$(mktemp -d)\ntrap 'rm -rf -- \"$restore_dir\"' EXIT\n";
    for (auto it = contents.begin(); it != contents.end(); ++it) {
        if (serverController->uploadTextFileToContainer(container, credentials, QString::fromUtf8(it.value()),
            it.key() + suffix, libssh::ScpOverwriteMode::ScpOverwriteExisting) != ErrorCode::NoError) return false;
        const QString path = it.key(); // Only the fixed, compile-time allowlist above is used in shell text.
        swap += QString("cp -p '%1' '%1%2.previous'\n").arg(path, suffix);
        rollback += QString("[ ! -f '%1%2.previous' ] || cp -p '%1%2.previous' '%1'\n").arg(path, suffix);
        cleanup += QString("rm -f '%1%2' '%1%2.previous'\n").arg(path, suffix);
        const QString localName = QFileInfo(path).fileName();
        hostRollback += QString("sudo docker cp '%1:%2%3.previous' \"$restore_dir/%4\"\n"
            "sudo docker cp \"$restore_dir/%4\" '%1:%2'\n").arg(containerName, path, suffix, localName);
    }
    swap += QString("xray run -test -config '%1%2'\n").arg(amnezia::protocols::xray::serverConfigPath, suffix);
    swap += "rollback() {\n" + rollback.mid(QString("set -euo pipefail\n").size()) + "}\ntrap rollback ERR\n";
    for (auto it = contents.begin(); it != contents.end(); ++it)
        swap += QString("chmod 600 '%1%2'\nmv -f '%1%2' '%1'\n").arg(it.key(), suffix);
    swap += "trap - ERR\n";
    if (serverController->runContainerScript(credentials, container, swap) != ErrorCode::NoError) {
        serverController->runContainerScript(credentials, container, rollback);
        return false;
    }
    const QString restart = restartAndVerify(containerName, targetPorts);
    if (serverController->runScript(credentials, restart) != ErrorCode::NoError) {
        // docker cp also works when an invalid config prevents the container from
        // starting, unlike docker exec. Check every recovery operation.
        const ErrorCode recovered = serverController->runScript(credentials, hostRollback);
        const ErrorCode restarted = recovered == ErrorCode::NoError
            ? serverController->runScript(credentials, restartAndVerify(containerName, currentPorts)) : recovered;
        logger.error() << "Snapshot startup failed; recovery:" << recovered << restarted;
        return false;
    }
    serverController->runContainerScript(credentials, container, cleanup);
    restoredClientConfig = config;
    return true;
}

void ConfigSnapshotManager::pruneOldSnapshots(const QString &hash)
{
    const auto snapshots = listSnapshots(hash);
    for (int i = MAX_SNAPSHOTS_PER_SERVER; i < snapshots.size(); ++i)
        QDir(snapshotBasePath(hash) + "/" + snapshots[i].id).removeRecursively();
}

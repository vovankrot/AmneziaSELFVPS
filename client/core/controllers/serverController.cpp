#include "serverController.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QPointer>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QThread>
#include <QTimer>
#include <QtConcurrent>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>

#include <chrono>
#include <thread>

#include <QJsonArray>

#include "containers/containers_defs.h"
#include "core/errorstrings.h"
#include "core/networkUtilities.h"
#include "core/scripts_registry.h"
#include "core/server_defs.h"
#include "protocols/protocols_defs.h"
#include "logger.h"
#include "settings.h"
#include "utilities.h"
#include "version.h"
#include "vpnConfigurationController.h"

namespace
{
    Logger logger("ServerController");
    constexpr int kQuickRemoteIdleTimeoutMs = 15000;
    constexpr int kQuickRemoteTotalTimeoutMs = 30000;
    constexpr int kBusyProbeIdleTimeoutMs = 10000;
    constexpr int kBusyProbeTotalTimeoutMs = 15000;
    constexpr int kDockerInstallIdleTimeoutMs = 120000;
    constexpr int kDockerInstallTotalTimeoutMs = 1800000;
    constexpr int kPrepareHostIdleTimeoutMs = 60000;
    constexpr int kPrepareHostTotalTimeoutMs = 300000;

    QString sshTargetLabel(const ServerCredentials &credentials)
    {
        return QStringLiteral("%1@%2:%3").arg(credentials.userName, credentials.hostName).arg(credentials.port);
    }

    bool usesPrivateKeyAuthentication(const ServerCredentials &credentials)
    {
        return credentials.secretData.contains(QStringLiteral("BEGIN"), Qt::CaseInsensitive)
               && credentials.secretData.contains(QStringLiteral("PRIVATE KEY"), Qt::CaseInsensitive);
    }

    QString sshConnectionLogLine(const ServerCredentials &credentials)
    {
        const QString target = sshTargetLabel(credentials);
        if (usesPrivateKeyAuthentication(credentials)) {
            return ServerController::tr("SSH connection: %1 (private key authentication, key hidden)").arg(target);
        }
        return ServerController::tr("SSH connection: %1 (password authentication, secret hidden)").arg(target);
    }

    QString shellSingleQuote(const QString &value)
    {
        QString escaped = value;
        escaped.replace(QChar('\''), QStringLiteral("'\"'\"'"));
        return QStringLiteral("'") + escaped + QStringLiteral("'");
    }

    QString compactLogText(const QString &value, int maxLength = 200)
    {
        QString compact = value;
        compact.replace('\r', ' ');
        compact.replace('\n', ' ');
        compact = compact.simplified();
        if (compact.length() > maxLength) {
            compact = compact.left(maxLength - 3) + QStringLiteral("...");
        }
        return compact;
    }

    QString shortSha256Hex(const QByteArray &value, int length = 12)
    {
        return QString::fromLatin1(QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex().left(length));
    }
}

ServerController::ServerController(std::shared_ptr<Settings> settings, QObject *parent) : m_settings(settings)
{
    connect(this, &ServerController::logLineReady, this, [](const QString &line) {
        logger.info() << compactLogText(line, 512);
    });
}

ServerController::~ServerController()
{
    m_sshClient.disconnectFromHost();
}

ErrorCode ServerController::runScript(const ServerCredentials &credentials, QString script,
                                      const std::function<ErrorCode(const QString &, libssh::Client &)> &cbReadStdOut,
                                      const std::function<ErrorCode(const QString &, libssh::Client &)> &cbReadStdErr,
                                      int idleTimeoutMs,
                                      int totalTimeoutMs)
{
    if (m_cancelInstallation.load()) {
        return ErrorCode::ServerCancelInstallation;
    }

    const QString currentTarget = sshTargetLabel(credentials);
    if (!m_loggedSshConnection || m_loggedSshTarget != currentTarget) {
        emit logLineReady(sshConnectionLogLine(credentials));
    }

    auto error = m_sshClient.connectToHost(credentials);
    if (error != ErrorCode::NoError) {
        m_loggedSshConnection = false;
        m_loggedSshTarget.clear();
        emit logLineReady(tr("SSH connection failed: %1").arg(errorString(error)));
        return error;
    }

    m_loggedSshConnection = true;
    m_loggedSshTarget = currentTarget;

    script.replace("\r", "");

    qDebug() << "ServerController::Run script";

    QString totalLine;
    const QStringList &lines = script.split("\n", Qt::SkipEmptyParts);
    for (int i = 0; i < lines.count(); i++) {
        QString currentLine = lines.at(i);

        if (totalLine.isEmpty()) {
            totalLine = currentLine;
        } else {
            totalLine = totalLine + "\n" + currentLine;
        }

        QString lineToExec;
        if (currentLine.endsWith("\\")) {
            continue;
        } else {
            lineToExec = totalLine;
            totalLine.clear();
        }

        if (lineToExec.startsWith("#")) {
            continue;
        }

        if (m_cancelInstallation.load()) {
            return ErrorCode::ServerCancelInstallation;
        }

        auto wrappedStdOut = [&cbReadStdOut](const QString &output, libssh::Client &client) -> ErrorCode {
            if (cbReadStdOut) return cbReadStdOut(output, client);
            return ErrorCode::NoError;
        };
        auto wrappedStdErr = [&cbReadStdErr](const QString &output, libssh::Client &client) -> ErrorCode {
            if (cbReadStdErr) return cbReadStdErr(output, client);
            return ErrorCode::NoError;
        };

        error = m_sshClient.executeCommand(lineToExec, wrappedStdOut, wrappedStdErr, idleTimeoutMs, totalTimeoutMs);
        if (error != ErrorCode::NoError) {
            if (error == ErrorCode::ServerCommandFailedError) {
                emit logLineReady(tr("Remote command exited with code %1.").arg(m_sshClient.lastExitStatus()));
            }
            if (error == ErrorCode::SshCommandTimeoutError) {
                emit logLineReady(tr("Remote command timed out while waiting for server response."));
            }
            emit logLineReady(tr("Command failed: %1").arg(errorString(error)));
            return error;
        }
    }

    qDebug().noquote() << "ServerController::runScript finished\n";
    return ErrorCode::NoError;
}

ErrorCode ServerController::runHostScript(const ServerCredentials &credentials, const QString &script,
                                          const std::function<ErrorCode(const QString &, libssh::Client &)> &cbReadStdOut,
                                          const std::function<ErrorCode(const QString &, libssh::Client &)> &cbReadStdErr,
                                          int idleTimeoutMs,
                                          int totalTimeoutMs)
{
    if (m_cancelInstallation.load()) {
        return ErrorCode::ServerCancelInstallation;
    }

    const QString currentTarget = sshTargetLabel(credentials);
    if (!m_loggedSshConnection || m_loggedSshTarget != currentTarget) {
        emit logLineReady(sshConnectionLogLine(credentials));
    }

    auto error = m_sshClient.connectToHost(credentials);
    if (error != ErrorCode::NoError) {
        m_loggedSshConnection = false;
        m_loggedSshTarget.clear();
        emit logLineReady(tr("SSH connection failed: %1").arg(errorString(error)));
        return error;
    }

    m_loggedSshConnection = true;
    m_loggedSshTarget = currentTarget;

    // Upload script to /tmp as a file (SCP works without sudo for /tmp)
    QString tmpPath = QString("/tmp/amnezia_%1.sh").arg(QDateTime::currentMSecsSinceEpoch());
    QString normalizedScript = script;
    normalizedScript.replace("\r", "");
    const QByteArray scriptBytes = normalizedScript.toUtf8();

    qDebug() << "ServerController::runHostScript uploading to" << tmpPath;

    error = uploadFileToHost(credentials, scriptBytes, tmpPath);
    if (error != ErrorCode::NoError) {
        emit logLineReady(tr("Failed to upload script to host: %1").arg(errorString(error)));
        return error;
    }

    // Execute as a single atomic command: chmod + run + cleanup
    QString execCmd = QString("chmod +x %1 && bash %1; _rc=$?; rm -f %1; exit $_rc").arg(tmpPath);

    auto wrappedStdOut = [&cbReadStdOut](const QString &output, libssh::Client &client) -> ErrorCode {
        if (cbReadStdOut) return cbReadStdOut(output, client);
        return ErrorCode::NoError;
    };
    auto wrappedStdErr = [&cbReadStdErr](const QString &output, libssh::Client &client) -> ErrorCode {
        if (cbReadStdErr) return cbReadStdErr(output, client);
        return ErrorCode::NoError;
    };

    error = m_sshClient.executeCommand(execCmd, wrappedStdOut, wrappedStdErr, idleTimeoutMs, totalTimeoutMs);

    qDebug().noquote() << "ServerController::runHostScript finished";
    return error;
}

ErrorCode ServerController::runContainerScript(const ServerCredentials &credentials, DockerContainer container, QString script,
                                               const std::function<ErrorCode(const QString &, libssh::Client &)> &cbReadStdOut,
                                               const std::function<ErrorCode(const QString &, libssh::Client &)> &cbReadStdErr,
                                               int idleTimeoutMs,
                                               int totalTimeoutMs)
{
    const QString containerName = ContainerProps::containerToString(container);
    QString fileName = "/opt/amnezia/" + Utils::getRandomString(16) + ".sh";
    script.replace("\r", "");

    // Ensure script ends with explicit exit 0 — some Alpine/bash combos
    // return exit code 2 from docker exec despite script completing successfully
    if (!script.trimmed().endsWith("exit 0")) {
        script.append("\nexit 0\n");
    }

    const QByteArray scriptBytes = script.toUtf8();
    const QString scriptHash = shortSha256Hex(scriptBytes);

    emit logLineReady(tr("Uploading container script to %1 in %2 (%3 bytes, sha256 %4...)")
                      .arg(fileName, containerName)
                      .arg(scriptBytes.size())
                      .arg(scriptHash));

    ErrorCode e = uploadTextFileToContainer(container, credentials, script, fileName);
    if (e) {
        emit logLineReady(tr("Container script upload failed for %1 in %2: %3")
                          .arg(fileName, containerName, errorString(e)));
        return e;
    }

    emit logLineReady(tr("Container script uploaded to %1 in %2.").arg(fileName, containerName));

    QString runner =
            QString("sudo docker exec $CONTAINER_NAME %2 %1 ").arg(fileName, (container == DockerContainer::Socks5Proxy ? "sh" : "bash"));

    auto defaultStdErrLogger = [this](const QString &data, libssh::Client &) -> ErrorCode {
        const QString trimmed = data.trimmed();
        if (!trimmed.isEmpty()) {
            emit logLineReady(trimmed);
        }
        return ErrorCode::NoError;
    };

    std::function<ErrorCode(const QString &, libssh::Client &)> stderrCb = cbReadStdErr;
    if (!stderrCb) {
        stderrCb = defaultStdErrLogger;
    }

    emit logLineReady(tr("Launching container script in %1: %2").arg(containerName, fileName));
    e = runScript(credentials, replaceVars(runner, genVarsForScript(credentials, container)), cbReadStdOut, stderrCb, idleTimeoutMs, totalTimeoutMs);
    if (e) {
        emit logLineReady(tr("Container script failed in %1: %2").arg(containerName, errorString(e)));
    }

    // Cleanup: remove the uploaded script file. Ignore errors — container may have
    // stopped or restarted, and the cleanup failure is not actionable.
    QString remover = QString("sudo docker exec $CONTAINER_NAME rm %1 ").arg(fileName);
    runScript(credentials, replaceVars(remover, genVarsForScript(credentials, container)));

    return e;
}

ErrorCode ServerController::uploadTextFileToContainer(DockerContainer container, const ServerCredentials &credentials, const QString &file,
                                                      const QString &path, libssh::ScpOverwriteMode overwriteMode)
{
    ErrorCode e = ErrorCode::NoError;
    QString tmpFileName = QString("/tmp/%1.tmp").arg(Utils::getRandomString(16));
    QString normalizedFile = file;
    normalizedFile.replace("\r", "");
    e = uploadFileToHost(credentials, normalizedFile.toUtf8(), tmpFileName);
    if (e)
        return e;

    QString stdOut;
    auto cbReadStd = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };

    // mkdir
    QString mkdir = QString("sudo docker exec $CONTAINER_NAME mkdir -p  \"$(dirname %1)\"").arg(path);

    e = runScript(credentials, replaceVars(mkdir, genVarsForScript(credentials, container)));
    if (e)
        return e;

    if (overwriteMode == libssh::ScpOverwriteMode::ScpOverwriteExisting) {
        e = runScript(credentials,
                      replaceVars(QStringLiteral("sudo docker cp %1 $CONTAINER_NAME:/%2").arg(tmpFileName, path),
                                  genVarsForScript(credentials, container)),
                      cbReadStd, cbReadStd);

        if (e)
            return e;
    } else if (overwriteMode == libssh::ScpOverwriteMode::ScpAppendToExisting) {
        e = runScript(credentials,
                      replaceVars(QStringLiteral("sudo docker cp %1 $CONTAINER_NAME:/%2").arg(tmpFileName, tmpFileName),
                                  genVarsForScript(credentials, container)),
                      cbReadStd, cbReadStd);

        if (e)
            return e;

        e = runScript(credentials,
                      replaceVars(QStringLiteral("sudo docker exec $CONTAINER_NAME sh -c \"cat %1 >> %2\"").arg(tmpFileName, path),
                                  genVarsForScript(credentials, container)),
                      cbReadStd, cbReadStd);

        if (e)
            return e;
    } else
        return ErrorCode::NotImplementedError;

    if (stdOut.contains("Error") && stdOut.contains("No such container")) {
        return ErrorCode::ServerContainerMissingError;
    }

    runScript(credentials, replaceVars(QString("sudo shred -u %1").arg(tmpFileName), genVarsForScript(credentials, container)));
    return e;
}

QByteArray ServerController::getTextFileFromContainer(DockerContainer container, const ServerCredentials &credentials, const QString &path,
                                                      ErrorCode &errorCode)
{
    errorCode = ErrorCode::NoError;

    const QString containerName = ContainerProps::containerToString(container);
    const QString quotedPath = shellSingleQuote(path);
    const QString script = QStringLiteral(
                               "sudo docker exec %1 sh -c \"if [ -r %2 ]; then xxd -p %2; else printf '__AMNEZIA_FILE_READ_ERROR__:%s\\n' %2 >&2; fi\"")
                               .arg(containerName, quotedPath);

    emit logLineReady(tr("Reading %1 from %2...").arg(path, containerName));

    QString stdOut;
    QString stdErr;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data;
        return ErrorCode::NoError;
    };
    auto cbReadStdErr = [&](const QString &data, libssh::Client &) {
        stdErr += data;
        return ErrorCode::NoError;
    };

    const QString currentTarget = sshTargetLabel(credentials);
    if (!m_loggedSshConnection || m_loggedSshTarget != currentTarget) {
        emit logLineReady(sshConnectionLogLine(credentials));
    }

    errorCode = m_sshClient.connectToHost(credentials);
    if (errorCode != ErrorCode::NoError) {
        m_loggedSshConnection = false;
        m_loggedSshTarget.clear();
        emit logLineReady(tr("Read failed for %1 in %2: %3").arg(path, containerName, errorString(errorCode)));
        return {};
    }

    m_loggedSshConnection = true;
    m_loggedSshTarget = currentTarget;

    errorCode = m_sshClient.executeCommand(script, cbReadStdOut, cbReadStdErr);
    if (errorCode != ErrorCode::NoError) {
        emit logLineReady(tr("Read failed for %1 in %2: %3").arg(path, containerName, errorString(errorCode)));
        return {};
    }

    const QString trimmedErr = stdErr.trimmed();
    if (!trimmedErr.isEmpty()) {
        const bool isReadFailure = trimmedErr.contains(QStringLiteral("__AMNEZIA_FILE_READ_ERROR__"))
                || trimmedErr.contains(QStringLiteral("No such file or directory"), Qt::CaseInsensitive)
                || trimmedErr.contains(QStringLiteral("can't open"), Qt::CaseInsensitive)
                || trimmedErr.contains(QStringLiteral("Permission denied"), Qt::CaseInsensitive);
        if (isReadFailure) {
            errorCode = ErrorCode::ServerCheckFailed;
            emit logLineReady(tr("Read failed for %1 in %2: %3")
                              .arg(path, containerName, compactLogText(trimmedErr)));
            return {};
        }

        emit logLineReady(tr("Read warning for %1 in %2: %3")
                          .arg(path, containerName, compactLogText(trimmedErr)));
    }

    if (stdOut.trimmed().isEmpty()) {
        emit logLineReady(tr("Read completed for %1 in %2 (empty file).").arg(path, containerName));
    } else {
        emit logLineReady(tr("Read completed for %1 in %2.").arg(path, containerName));
    }

    return QByteArray::fromHex(stdOut.toUtf8());
}

ErrorCode ServerController::uploadFileToHost(const ServerCredentials &credentials, const QByteArray &data, const QString &remotePath,
                                             libssh::ScpOverwriteMode overwriteMode)
{
    const QString currentTarget = sshTargetLabel(credentials);
    if (!m_loggedSshConnection || m_loggedSshTarget != currentTarget) {
        emit logLineReady(sshConnectionLogLine(credentials));
    }

    auto error = m_sshClient.connectToHost(credentials);
    if (error != ErrorCode::NoError) {
        m_loggedSshConnection = false;
        m_loggedSshTarget.clear();
        return error;
    }

    m_loggedSshConnection = true;
    m_loggedSshTarget = currentTarget;

    QTemporaryFile localFile;
    localFile.open();
    localFile.write(data);
    localFile.close();

    error = m_sshClient.scpFileCopy(overwriteMode, localFile.fileName(), remotePath, "non_desc");

    if (error != ErrorCode::NoError) {
        return error;
    }
    return ErrorCode::NoError;
}

ErrorCode ServerController::rebootServer(const ServerCredentials &credentials)
{
    QString script = QString("sudo reboot");

    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data;
        return ErrorCode::NoError;
    };

    auto cbReadStdErr = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };

    return runScript(credentials, script, cbReadStdOut, cbReadStdErr);
}

ErrorCode ServerController::removeAllContainers(const ServerCredentials &credentials)
{
    return runScript(credentials, amnezia::scriptData(SharedScriptType::remove_all_containers));
}

ErrorCode ServerController::cleanupServer(const ServerCredentials &credentials, bool removeDocker)
{
    // First remove all containers
    ErrorCode e = removeAllContainers(credentials);
    if (e != ErrorCode::NoError) return e;

    // Then run host-level cleanup (uses runHostScript because the script
    // contains multi-line constructs that break with line-by-line execution)
    QString script = amnezia::scriptData(SharedScriptType::cleanup_host);
    if (removeDocker) {
        script.replace(QLatin1String("REMOVE_DOCKER=0"), QLatin1String("REMOVE_DOCKER=1"));
    }
    return runHostScript(credentials, script);
}

ErrorCode ServerController::removeContainer(const ServerCredentials &credentials, DockerContainer container)
{
    return runScript(credentials,
                     replaceVars(amnezia::scriptData(SharedScriptType::remove_container), genVarsForScript(credentials, container)));
}

ErrorCode ServerController::setupContainer(const ServerCredentials &credentials, DockerContainer container, QJsonObject &config, bool isUpdate)
{
    qDebug().noquote() << "ServerController::setupContainer" << ContainerProps::containerToString(container);
    ErrorCode e = ErrorCode::NoError;

    e = logServerEnvironment(credentials);
    if (e) {
        emit logLineReady(tr("Server preflight failed: %1").arg(errorString(e)));
        return e;
    }

    if (m_cancelInstallation.load()) {
        emit logLineReady(tr("Cancelled before sudo check"));
        return ErrorCode::ServerCancelInstallation;
    }
    emit logLineReady(tr("Checking sudo access..."));
    e = isUserInSudo(credentials, container);
    if (e) {
        emit logLineReady(tr("Sudo check failed: %1").arg(errorString(e)));
        return e;
    }
    emit logLineReady(tr("Sudo check passed."));

    if (m_cancelInstallation.load()) {
        emit logLineReady(tr("Cancelled before package manager check"));
        return ErrorCode::ServerCancelInstallation;
    }
    emit logLineReady(tr("Checking if package manager is busy..."));
    e = isServerDpkgBusy(credentials, container);
    if (e) {
        emit logLineReady(tr("Package manager check failed: %1").arg(errorString(e)));
        return e;
    }
    emit logLineReady(tr("Package manager is free."));

    emit logLineReady(tr("Installing Docker..."));
    e = installDockerWorker(credentials, container);
    if (e) {
        emit logLineReady(tr("Docker install failed: %1").arg(errorString(e)));
        return e;
    }
    if (m_cancelInstallation.load()) {
        emit logLineReady(tr("Cancelled after: install Docker"));
        return ErrorCode::ServerCancelInstallation;
    }
    emit logLineReady(tr("Step completed: install Docker"));

    if (!isUpdate) {
        emit logLineReady(tr("Checking server port availability..."));
        e = isServerPortBusy(credentials, container, config);
        if (e) {
            emit logLineReady(tr("Port check failed: %1").arg(errorString(e)));
            return e;
        }
    }

    emit logLineReady(tr("Preparing host..."));
    e = prepareHostWorker(credentials, container, config);
    if (e) {
        emit logLineReady(tr("Prepare host failed: %1").arg(errorString(e)));
        return e;
    }
    if (m_cancelInstallation.load()) {
        emit logLineReady(tr("Cancelled after: prepare host"));
        return ErrorCode::ServerCancelInstallation;
    }
    emit logLineReady(tr("Step completed: prepare host"));

    // ===== Backup existing config before container removal =====
    struct {
        // WG/AWG
        QByteArray wgConfig;
        QByteArray wgPrivateKey;
        QByteArray wgPublicKey;
        QByteArray wgPsk;
        bool hasWg = false;
        // Xray
        QByteArray xrayConfig;
        QByteArray xrayPublicKey;
        QByteArray xrayPrivateKey;
        QByteArray xrayShortId;
        QByteArray xrayUuid;
        QByteArray xrayXhttpPath;
        bool hasXray = false;
        // OpenVPN-based (docker cp to host)
        bool hasOvpn = false;
    } backup;

    bool isWgLike = (container == DockerContainer::WireGuard
                     || container == DockerContainer::Awg
                     || container == DockerContainer::Awg2);
    bool isXrayLike = (container == DockerContainer::Xray);
    bool isOvpnLike = (container == DockerContainer::OpenVpn
                       || container == DockerContainer::ShadowSocks
                       || container == DockerContainer::Cloak);

    // Check if the target container is running before attempting backup.
    // On first install the container doesn't exist, so docker exec would fail
    // with error 215 and produce a confusing log message.
    bool containerRunningForBackup = false;
    {
        const QString cName = ContainerProps::containerToString(container);
        QString psOut;
        auto cb = [&](const QString &data, libssh::Client &) { psOut += data; return ErrorCode::NoError; };
        auto cbIgnore = [](const QString &, libssh::Client &) { return ErrorCode::NoError; };
        runScript(credentials,
                  QStringLiteral("sudo docker ps -q --filter 'name=^%1$'").arg(cName),
                  cb, cbIgnore);
        containerRunningForBackup = !psOut.trimmed().isEmpty();
    }

    if (isWgLike && containerRunningForBackup) {
        ErrorCode be;
        QString cfgPath;
        QString keyDir;
        if (container == DockerContainer::WireGuard) {
            cfgPath = amnezia::protocols::wireguard::serverConfigPath;
            keyDir = QStringLiteral("/opt/amnezia/wireguard");
        } else if (container == DockerContainer::Awg) {
            cfgPath = amnezia::protocols::awg::serverLegacyConfigPath;
            keyDir = QStringLiteral("/opt/amnezia/awg");
        } else {
            cfgPath = amnezia::protocols::awg::serverConfigPath;
            keyDir = QStringLiteral("/opt/amnezia/awg");
        }
        backup.wgConfig = getTextFileFromContainer(container, credentials, cfgPath, be);
        if (be == ErrorCode::NoError && !backup.wgConfig.isEmpty() && backup.wgConfig.contains("[Peer]")) {
            backup.wgPrivateKey = getTextFileFromContainer(container, credentials,
                keyDir + "/wireguard_server_private_key.key", be);
            backup.wgPublicKey = getTextFileFromContainer(container, credentials,
                (container == DockerContainer::WireGuard)
                    ? amnezia::protocols::wireguard::serverPublicKeyPath
                    : amnezia::protocols::awg::serverPublicKeyPath, be);
            backup.wgPsk = getTextFileFromContainer(container, credentials,
                (container == DockerContainer::WireGuard)
                    ? amnezia::protocols::wireguard::serverPskKeyPath
                    : amnezia::protocols::awg::serverPskKeyPath, be);
            backup.hasWg = !backup.wgPrivateKey.isEmpty();
            if (backup.hasWg) {
                qDebug().noquote() << "Backed up WG/AWG config with peers before reinstall";
            }
        }
    }

    if (isXrayLike && containerRunningForBackup) {
        ErrorCode be;
        backup.xrayConfig = getTextFileFromContainer(container, credentials,
            amnezia::protocols::xray::serverConfigPath, be);
        if (be == ErrorCode::NoError && !backup.xrayConfig.isEmpty()) {
            backup.xrayPublicKey = getTextFileFromContainer(container, credentials,
                amnezia::protocols::xray::PublicKeyPath, be);
            backup.xrayPrivateKey = getTextFileFromContainer(container, credentials,
                amnezia::protocols::xray::PrivateKeyPath, be);
            backup.xrayShortId = getTextFileFromContainer(container, credentials,
                amnezia::protocols::xray::shortidPath, be);
            backup.xrayUuid = getTextFileFromContainer(container, credentials,
                amnezia::protocols::xray::uuidPath, be);
            backup.xrayXhttpPath = getTextFileFromContainer(container, credentials,
                amnezia::protocols::xray::xhttpPathPath, be);
            backup.hasXray = !backup.xrayPrivateKey.isEmpty();
            if (backup.hasXray) {
                qDebug().noquote() << "Backed up Xray config before reinstall";
            }
        }
    }

    if (isOvpnLike && containerRunningForBackup) {
        QString cName = ContainerProps::containerToString(container);
        // Backup entire /opt/amnezia/ from container to host /tmp/ via docker cp
        QString backupScript = QStringLiteral(
            "sudo rm -rf /tmp/amnezia_backup_%1 2>/dev/null; "
            "sudo docker cp %1:/opt/amnezia /tmp/amnezia_backup_%1 2>/dev/null && echo BACKUP_OK || echo BACKUP_FAIL"
        ).arg(cName);
        QString backupOut;
        auto cbBackup = [&](const QString &data, libssh::Client &) { backupOut += data; return ErrorCode::NoError; };
        runScript(credentials, backupScript, cbBackup);
        backup.hasOvpn = backupOut.contains("BACKUP_OK");
        if (backup.hasOvpn) {
            qDebug().noquote() << "Backed up OpenVPN PKI to host before reinstall";
        }
    }
    // ===== End backup =====

    emit logLineReady(tr("Removing old container..."));
    removeContainer(credentials, container);
    if (m_cancelInstallation.load()) {
        emit logLineReady(tr("Cancelled after: remove container"));
        return ErrorCode::ServerCancelInstallation;
    }
    emit logLineReady(tr("Step completed: remove old container"));

    emit logLineReady(tr("Building container..."));
    e = buildContainerWorker(credentials, container, config);
    if (e) {
        emit logLineReady(tr("Build container failed: %1").arg(errorString(e)));
        return e;
    }
    if (m_cancelInstallation.load()) {
        emit logLineReady(tr("Cancelled after: build container"));
        return ErrorCode::ServerCancelInstallation;
    }
    emit logLineReady(tr("Step completed: build container"));

    emit logLineReady(tr("Starting container..."));
    e = runContainerWorker(credentials, container, config);
    if (e) {
        emit logLineReady(tr("Start container failed: %1").arg(errorString(e)));
        return e;
    }
    if (m_cancelInstallation.load()) {
        emit logLineReady(tr("Cancelled after: start container"));
        return ErrorCode::ServerCancelInstallation;
    }
    emit logLineReady(tr("Step completed: start container"));

    // ===== Restore OpenVPN PKI (after runContainerWorker generated new PKI) =====
    if (backup.hasOvpn) {
        QString cName = ContainerProps::containerToString(container);
        // Restore backed-up files over newly generated PKI
        QString restoreScript = QStringLiteral(
            "sudo docker cp /tmp/amnezia_backup_%1/. %1:/opt/amnezia/ 2>/dev/null; "
            "sudo rm -rf /tmp/amnezia_backup_%1"
        ).arg(cName);
        runScript(credentials, restoreScript);
        qDebug().noquote() << "Restored OpenVPN PKI after reinstall";
    }

    emit logLineReady(tr("Configuring container..."));
    e = configureContainerWorker(credentials, container, config);
    if (e) {
        emit logLineReady(tr("Configure container failed: %1").arg(errorString(e)));
        // Self-heal: never leave a running-but-configless zombie behind — it would
        // poison the container scan (ServerCheckFailed/200) on every future install.
        emit logLineReady(tr("Removing incomplete container to keep the server clean..."));
        removeContainer(credentials, container);
        return e;
    }
    if (m_cancelInstallation.load()) {
        emit logLineReady(tr("Cancelled after: configure container"));
        return ErrorCode::ServerCancelInstallation;
    }
    emit logLineReady(tr("Step completed: configure container"));

    // ===== Restore WG/AWG peers and keys =====
    if (backup.hasWg) {
        QString cfgPath;
        QString keyDir;
        if (container == DockerContainer::WireGuard) {
            cfgPath = amnezia::protocols::wireguard::serverConfigPath;
            keyDir = QStringLiteral("/opt/amnezia/wireguard");
        } else if (container == DockerContainer::Awg) {
            cfgPath = amnezia::protocols::awg::serverLegacyConfigPath;
            keyDir = QStringLiteral("/opt/amnezia/awg");
        } else {
            cfgPath = amnezia::protocols::awg::serverConfigPath;
            keyDir = QStringLiteral("/opt/amnezia/awg");
        }

        // Restore key files (overwrite newly generated ones)
        uploadTextFileToContainer(container, credentials,
            QString::fromUtf8(backup.wgPrivateKey).trimmed(),
            keyDir + "/wireguard_server_private_key.key",
            libssh::ScpOverwriteMode::ScpOverwriteExisting);
        uploadTextFileToContainer(container, credentials,
            QString::fromUtf8(backup.wgPublicKey).trimmed(),
            (container == DockerContainer::WireGuard)
                ? amnezia::protocols::wireguard::serverPublicKeyPath
                : amnezia::protocols::awg::serverPublicKeyPath,
            libssh::ScpOverwriteMode::ScpOverwriteExisting);
        uploadTextFileToContainer(container, credentials,
            QString::fromUtf8(backup.wgPsk).trimmed(),
            (container == DockerContainer::WireGuard)
                ? amnezia::protocols::wireguard::serverPskKeyPath
                : amnezia::protocols::awg::serverPskKeyPath,
            libssh::ScpOverwriteMode::ScpOverwriteExisting);

        // Read fresh config (has new [Interface] with potentially updated port/junk params)
        ErrorCode re;
        QByteArray freshConfig = getTextFileFromContainer(container, credentials, cfgPath, re);
        if (re == ErrorCode::NoError && !freshConfig.isEmpty()) {
            QString freshStr = QString::fromUtf8(freshConfig);
            QString backupStr = QString::fromUtf8(backup.wgConfig);
            QString oldPrivKey = QString::fromUtf8(backup.wgPrivateKey).trimmed();

            // Replace PrivateKey in fresh [Interface] with the backed-up one
            QRegularExpression privKeyRe(QStringLiteral("PrivateKey\\s*=\\s*.+"));
            freshStr.replace(privKeyRe, "PrivateKey = " + oldPrivKey);

            // Append old [Peer] sections from backup
            int peerIdx = backupStr.indexOf(QStringLiteral("[Peer]"));
            if (peerIdx >= 0) {
                QString peers = backupStr.mid(peerIdx);
                if (!freshStr.endsWith('\n')) freshStr += '\n';
                freshStr += '\n' + peers;
            }

            // Upload merged config
            uploadTextFileToContainer(container, credentials, freshStr, cfgPath,
                libssh::ScpOverwriteMode::ScpOverwriteExisting);

            // Apply with syncconf
            QString bin = (container == DockerContainer::Awg2) ? "awg" : "wg";
            QString iface = (container == DockerContainer::Awg2) ? "awg0" : "wg0";
            QString syncScript = QStringLiteral(
                "%1 syncconf %2 <(%1-quick strip %3)"
            ).arg(bin, iface, cfgPath);
            runContainerScript(credentials, container, syncScript);

            qDebug().noquote() << "Restored WG/AWG peers after reinstall";
        }
    }

    // ===== Restore Xray clients and keys =====
    if (backup.hasXray) {
        // Restore key files
        uploadTextFileToContainer(container, credentials,
            QString::fromUtf8(backup.xrayPublicKey).trimmed(),
            amnezia::protocols::xray::PublicKeyPath,
            libssh::ScpOverwriteMode::ScpOverwriteExisting);
        uploadTextFileToContainer(container, credentials,
            QString::fromUtf8(backup.xrayPrivateKey).trimmed(),
            amnezia::protocols::xray::PrivateKeyPath,
            libssh::ScpOverwriteMode::ScpOverwriteExisting);
        uploadTextFileToContainer(container, credentials,
            QString::fromUtf8(backup.xrayShortId).trimmed(),
            amnezia::protocols::xray::shortidPath,
            libssh::ScpOverwriteMode::ScpOverwriteExisting);
        if (!backup.xrayUuid.isEmpty()) {
            uploadTextFileToContainer(container, credentials,
                QString::fromUtf8(backup.xrayUuid).trimmed(),
                amnezia::protocols::xray::uuidPath,
                libssh::ScpOverwriteMode::ScpOverwriteExisting);
        }
        if (!backup.xrayXhttpPath.isEmpty()) {
            uploadTextFileToContainer(container, credentials,
                QString::fromUtf8(backup.xrayXhttpPath).trimmed(),
                amnezia::protocols::xray::xhttpPathPath,
                libssh::ScpOverwriteMode::ScpOverwriteExisting);
        }

        // Merge old clients into the new server config
        ErrorCode re;
        QByteArray freshXray = getTextFileFromContainer(container, credentials,
            amnezia::protocols::xray::serverConfigPath, re);
        if (re == ErrorCode::NoError) {
            QJsonDocument freshDoc = QJsonDocument::fromJson(freshXray);
            QJsonDocument backupDoc = QJsonDocument::fromJson(backup.xrayConfig);

            if (freshDoc.isObject() && backupDoc.isObject()) {
                QJsonObject freshObj = freshDoc.object();
                QJsonObject backupObj = backupDoc.object();

                QJsonArray freshInbounds = freshObj["inbounds"].toArray();
                QJsonArray backupInbounds = backupObj["inbounds"].toArray();

                if (!freshInbounds.isEmpty() && !backupInbounds.isEmpty()) {
                    QJsonObject freshInbound = freshInbounds[0].toObject();
                    QJsonObject freshSettings = freshInbound["settings"].toObject();
                    QJsonArray freshClients = freshSettings["clients"].toArray();

                    // Collect existing client IDs for dedup
                    QSet<QString> existingIds;
                    for (const auto &c : freshClients) {
                        existingIds.insert(c.toObject()["id"].toString());
                    }

                    // Add backup clients that don't already exist
                    QJsonObject backupSettings = backupInbounds[0].toObject()["settings"].toObject();
                    QJsonArray backupClients = backupSettings["clients"].toArray();
                    for (const auto &c : backupClients) {
                        QString id = c.toObject()["id"].toString();
                        if (!id.isEmpty() && !existingIds.contains(id)) {
                            freshClients.append(c);
                            existingIds.insert(id);
                        }
                    }
                    freshSettings["clients"] = freshClients;
                    freshInbound["settings"] = freshSettings;

                    // Restore reality privateKey and shortIds from backup
                    QJsonObject freshStream = freshInbound["streamSettings"].toObject();
                    QJsonObject backupStream = backupInbounds[0].toObject()["streamSettings"].toObject();
                    if (backupStream.contains("realitySettings")) {
                        QJsonObject freshReality = freshStream["realitySettings"].toObject();
                        QJsonObject backupReality = backupStream["realitySettings"].toObject();
                        freshReality["privateKey"] = backupReality["privateKey"];
                        if (backupReality.contains("shortIds")) {
                            freshReality["shortIds"] = backupReality["shortIds"];
                        }
                        freshStream["realitySettings"] = freshReality;
                        freshInbound["streamSettings"] = freshStream;
                    }

                    // Restore XHTTP path from backup
                    if (backupStream.contains("xhttpSettings")) {
                        QJsonObject freshXhttp = freshStream.contains("xhttpSettings")
                            ? freshStream["xhttpSettings"].toObject() : QJsonObject();
                        QJsonObject backupXhttp = backupStream["xhttpSettings"].toObject();
                        if (backupXhttp.contains("path")) {
                            freshXhttp["path"] = backupXhttp["path"];
                            freshStream["xhttpSettings"] = freshXhttp;
                            freshInbound["streamSettings"] = freshStream;
                        }
                    }

                    freshInbounds[0] = freshInbound;
                    freshObj["inbounds"] = freshInbounds;

                    // Upload merged config
                    uploadTextFileToContainer(container, credentials,
                        QString::fromUtf8(QJsonDocument(freshObj).toJson(QJsonDocument::Indented)),
                        amnezia::protocols::xray::serverConfigPath,
                        libssh::ScpOverwriteMode::ScpOverwriteExisting);

                    // Restart xray to apply
                    runContainerScript(credentials, container,
                        QStringLiteral("killall -KILL xray 2>/dev/null; "
                                       "nohup xray -config /opt/amnezia/xray/server.json >/dev/null 2>&1 &"));

                    qDebug().noquote() << "Restored Xray clients after reinstall";
                }
            }
        }
    }
    // ===== End restore =====

    emit logLineReady(tr("Setting up firewall..."));
    setupServerFirewall(credentials);
    emit logLineReady(tr("Step completed: firewall setup"));

    emit logLineReady(tr("Running startup script..."));
    return startupContainerWorker(credentials, container, config);
}

ErrorCode ServerController::updateContainer(const ServerCredentials &credentials, DockerContainer container, const QJsonObject &oldConfig,
                                            QJsonObject &newConfig)
{
    bool reinstallRequired = isReinstallContainerRequired(container, oldConfig, newConfig);
    qDebug() << "ServerController::updateContainer for container" << container << "reinstall required is" << reinstallRequired;

    if (reinstallRequired) {
        return setupContainer(credentials, container, newConfig, true);
    } else {
        ErrorCode e = configureContainerWorker(credentials, container, newConfig);
        if (e)
            return e;

        return startupContainerWorker(credentials, container, newConfig);
    }
}

bool ServerController::isReinstallContainerRequired(DockerContainer container, const QJsonObject &oldConfig, const QJsonObject &newConfig)
{
    Proto mainProto = ContainerProps::defaultProtocol(container);

    const QJsonObject &oldProtoConfig = oldConfig.value(ProtocolProps::protoToString(mainProto)).toObject();
    const QJsonObject &newProtoConfig = newConfig.value(ProtocolProps::protoToString(mainProto)).toObject();

    if (container == DockerContainer::OpenVpn) {
        if (oldProtoConfig.value(config_key::transport_proto).toString(protocols::openvpn::defaultTransportProto)
            != newProtoConfig.value(config_key::transport_proto).toString(protocols::openvpn::defaultTransportProto))
            return true;

        if (oldProtoConfig.value(config_key::port).toString(protocols::openvpn::defaultPort)
            != newProtoConfig.value(config_key::port).toString(protocols::openvpn::defaultPort))
            return true;
    }

    if (container == DockerContainer::Cloak) {
        if (oldProtoConfig.value(config_key::port).toString(protocols::cloak::defaultPort)
            != newProtoConfig.value(config_key::port).toString(protocols::cloak::defaultPort))
            return true;
    }

    if (container == DockerContainer::ShadowSocks) {
        if (oldProtoConfig.value(config_key::port).toString(protocols::shadowsocks::defaultPort)
            != newProtoConfig.value(config_key::port).toString(protocols::shadowsocks::defaultPort))
            return true;
    }

    if (ContainerProps::isAwgContainer(container)) {
        if ((oldProtoConfig.value(config_key::subnet_address).toString(protocols::wireguard::defaultSubnetAddress)
             != newProtoConfig.value(config_key::subnet_address).toString(protocols::wireguard::defaultSubnetAddress))
            || (oldProtoConfig.value(config_key::port).toString(protocols::awg::defaultPort)
                != newProtoConfig.value(config_key::port).toString(protocols::awg::defaultPort))
            || (oldProtoConfig.value(config_key::junkPacketCount).toString(protocols::awg::defaultJunkPacketCount)
                != newProtoConfig.value(config_key::junkPacketCount).toString(protocols::awg::defaultJunkPacketCount))
            || (oldProtoConfig.value(config_key::junkPacketMinSize).toString(protocols::awg::defaultJunkPacketMinSize)
                != newProtoConfig.value(config_key::junkPacketMinSize).toString(protocols::awg::defaultJunkPacketMinSize))
            || (oldProtoConfig.value(config_key::junkPacketMaxSize).toString(protocols::awg::defaultJunkPacketMaxSize)
                != newProtoConfig.value(config_key::junkPacketMaxSize).toString(protocols::awg::defaultJunkPacketMaxSize))
            || (oldProtoConfig.value(config_key::initPacketJunkSize).toString(protocols::awg::defaultInitPacketJunkSize)
                != newProtoConfig.value(config_key::initPacketJunkSize).toString(protocols::awg::defaultInitPacketJunkSize))
            || (oldProtoConfig.value(config_key::responsePacketJunkSize).toString(protocols::awg::defaultResponsePacketJunkSize)
                != newProtoConfig.value(config_key::responsePacketJunkSize).toString(protocols::awg::defaultResponsePacketJunkSize))
            || (oldProtoConfig.value(config_key::initPacketMagicHeader).toString(protocols::awg::defaultInitPacketMagicHeader)
                != newProtoConfig.value(config_key::initPacketMagicHeader).toString(protocols::awg::defaultInitPacketMagicHeader))
            || (oldProtoConfig.value(config_key::responsePacketMagicHeader).toString(protocols::awg::defaultResponsePacketMagicHeader)
                != newProtoConfig.value(config_key::responsePacketMagicHeader).toString(protocols::awg::defaultResponsePacketMagicHeader))
            || (oldProtoConfig.value(config_key::underloadPacketMagicHeader).toString(protocols::awg::defaultUnderloadPacketMagicHeader)
                != newProtoConfig.value(config_key::underloadPacketMagicHeader).toString(protocols::awg::defaultUnderloadPacketMagicHeader))
            || (oldProtoConfig.value(config_key::transportPacketMagicHeader).toString(protocols::awg::defaultTransportPacketMagicHeader))
                    != newProtoConfig.value(config_key::transportPacketMagicHeader).toString(protocols::awg::defaultTransportPacketMagicHeader)
            || (oldProtoConfig.value(config_key::cookieReplyPacketJunkSize).toString(protocols::awg::defaultCookieReplyPacketJunkSize)
                != newProtoConfig.value(config_key::cookieReplyPacketJunkSize).toString(protocols::awg::defaultCookieReplyPacketJunkSize))
            || (oldProtoConfig.value(config_key::transportPacketJunkSize).toString(protocols::awg::defaultTransportPacketJunkSize)
                != newProtoConfig.value(config_key::transportPacketJunkSize).toString(protocols::awg::defaultTransportPacketJunkSize)))

            return true;
    }

    if (container == DockerContainer::WireGuard) {
        if ((oldProtoConfig.value(config_key::subnet_address).toString(protocols::wireguard::defaultSubnetAddress)
             != newProtoConfig.value(config_key::subnet_address).toString(protocols::wireguard::defaultSubnetAddress))
            || (oldProtoConfig.value(config_key::port).toString(protocols::wireguard::defaultPort)
                != newProtoConfig.value(config_key::port).toString(protocols::wireguard::defaultPort)))
            return true;
    }

    if (container == DockerContainer::Socks5Proxy) {
        return true;
    }

    if (container == DockerContainer::Xray) {
        if (oldProtoConfig.value(config_key::port).toString(protocols::xray::defaultPort)
            != newProtoConfig.value(config_key::port).toString(protocols::xray::defaultPort)) {
            return true;
        }
    }

    return false;
}

ErrorCode ServerController::installDockerWorker(const ServerCredentials &credentials, DockerContainer container)
{
    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &client) {
        stdOut += data + "\n";

        if (data.contains("Automatically restart Docker daemon?")) {
            return client.writeResponse("yes");
        }
        return ErrorCode::NoError;
    };
    auto cbReadStdErr = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };

    emit logLineReady(tr("Running Docker install script on server..."));

    ErrorCode error =
            runScript(credentials, replaceVars(amnezia::scriptData(SharedScriptType::install_docker), genVarsForScript(credentials)),
                  cbReadStdOut, cbReadStdErr, kDockerInstallIdleTimeoutMs, kDockerInstallTotalTimeoutMs);

    qDebug().noquote() << "ServerController::installDockerWorker" << stdOut;
    if (container == DockerContainer::Awg2) {
        QRegularExpression regex(R"(Linux\s+(\d+)\.(\d+)[^\d]*)");
        QRegularExpressionMatch match = regex.match(stdOut);
        if (match.hasMatch()) {
            int majorVersion = match.captured(1).toInt();
            int minorVersion = match.captured(2).toInt();

            if (majorVersion < 4 || (majorVersion == 4 && minorVersion < 14)) {
                return ErrorCode::ServerLinuxKernelTooOld;
            }
        }
    }
    if (stdOut.contains("lock"))
        return ErrorCode::ServerPacketManagerError;
    if (stdOut.contains("command not found"))
        return ErrorCode::ServerDockerFailedError;

    return error;
}

ErrorCode ServerController::prepareHostWorker(const ServerCredentials &credentials, DockerContainer container, const QJsonObject &config)
{
    // create folder on host
    emit logLineReady(tr("Running host preparation script for %1...").arg(ContainerProps::containerToString(container)));
    return runScript(credentials,
                     replaceVars(amnezia::scriptData(SharedScriptType::prepare_host), genVarsForScript(credentials, container)),
                     nullptr,
                     nullptr,
                     kPrepareHostIdleTimeoutMs,
                     kPrepareHostTotalTimeoutMs);
}

ErrorCode ServerController::buildContainerWorker(const ServerCredentials &credentials, DockerContainer container, const QJsonObject &config)
{
    const QString containerName = ContainerProps::containerToString(container);
    QString dockerfileFolder = amnezia::server::getDockerfileFolder(container);
    QString dockerFilePath = dockerfileFolder + "/Dockerfile";

    QString dockerfileText = amnezia::scriptData(ProtocolScriptType::dockerfile, container);
    dockerfileText.replace("\r", "");
    const QByteArray dockerfileData = dockerfileText.toUtf8();
    const QString dockerfileHash = shortSha256Hex(dockerfileData);

    emit logLineReady(tr("Uploading Dockerfile for %1 to %2 (%3 bytes, sha256 %4...)")
                      .arg(containerName, dockerFilePath)
                      .arg(dockerfileData.size())
                      .arg(dockerfileHash));

    // Use base64 + sudo to write file (SCP doesn't have sudo privileges)
    QString base64Data = dockerfileData.toBase64();
    QString writeScript = QString("sudo mkdir -p %1 && echo '%2' | base64 -d | sudo tee %3 > /dev/null")
                          .arg(dockerfileFolder, base64Data, dockerFilePath);
    
    ErrorCode errorCode = runScript(credentials, writeScript);

    if (errorCode) {
        emit logLineReady(tr("Dockerfile upload failed for %1: %2").arg(containerName, errorString(errorCode)));
        return errorCode;
    }

    emit logLineReady(tr("Dockerfile uploaded for %1.").arg(containerName));

    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };
    auto cbReadStdErr = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };

    emit logLineReady(tr("Launching docker build for %1...").arg(containerName));

    // idle timeout stays 0: `docker build` is silent for long stretches (e.g. the
    // Dockerfile's `curl` xray download has no progress output) — that silence is
    // normal, not a hang, so an idle timeout would false-trip. The TCP session is
    // kept alive by enableTcpKeepalive() so NAT/sshd can't drop it mid-build. A
    // generous total timeout is just a backstop against a genuinely stuck build.
    ErrorCode error =
            runScript(credentials,
                      replaceVars(amnezia::scriptData(SharedScriptType::build_container), genVarsForScript(credentials, container, config)),
                      cbReadStdOut, cbReadStdErr,
                      0 /* idleTimeoutMs */, kDockerInstallTotalTimeoutMs /* totalTimeoutMs */);

    if (stdOut.contains("doesn't work on cgroups v2"))
        return ErrorCode::ServerDockerOnCgroupsV2;
    if (stdOut.contains("cgroup mountpoint does not exist"))
        return ErrorCode::ServerCgroupMountpoint;
    if (stdOut.contains("have reached") && stdOut.contains("pull rate limit"))
        return ErrorCode::DockerPullRateLimit;

    return error;
}

ErrorCode ServerController::verifyContainerIsRunning(const ServerCredentials &credentials, DockerContainer container)
{
    const QString containerName = ContainerProps::containerToString(container);
    QString stdOut;
    QString stdErr;

    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data;
        return ErrorCode::NoError;
    };
    auto cbReadStdErr = [&](const QString &data, libssh::Client &) {
        stdErr += data;
        return ErrorCode::NoError;
    };

    const QString inspectScript = replaceVars(
        QStringLiteral("sudo docker inspect -f '{{.State.Running}}' $CONTAINER_NAME"),
        genVarsForScript(credentials, container));

    ErrorCode error = runScript(credentials, inspectScript, cbReadStdOut, cbReadStdErr);
    const QString combinedOutput = stdOut + stdErr;

    if (combinedOutput.contains("No such object") || combinedOutput.contains("No such container")) {
        emit logLineReady(tr("Container %1 is missing after start.").arg(containerName));
        return ErrorCode::ServerContainerMissingError;
    }

    if (error != ErrorCode::NoError) {
        return error;
    }

    if (!stdOut.trimmed().startsWith("true")) {
        emit logLineReady(tr("Container %1 is not running after start.").arg(containerName));

        QString logsOut;
        auto cbReadLogs = [&](const QString &data, libssh::Client &) {
            logsOut += data;
            return ErrorCode::NoError;
        };

        runScript(credentials,
                  replaceVars(QStringLiteral("sudo docker logs --tail 50 $CONTAINER_NAME"), genVarsForScript(credentials, container)),
                  cbReadLogs,
                  cbReadLogs);

        if (!logsOut.trimmed().isEmpty()) {
            emit logLineReady(tr("Recent logs for %1:").arg(containerName));
            emit logLineReady(logsOut.trimmed());
        }

        return ErrorCode::ServerDockerFailedError;
    }

    return ErrorCode::NoError;
}

ErrorCode ServerController::runContainerWorker(const ServerCredentials &credentials, DockerContainer container, QJsonObject &config)
{
    QString stdOut;
    QString stdErr;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };
    auto cbReadStdErr = [&](const QString &data, libssh::Client &) {
        stdErr += data + "\n";
        return ErrorCode::NoError;
    };

    ErrorCode e = runScript(credentials,
                            replaceVars(amnezia::scriptData(ProtocolScriptType::run_container, container),
                                        genVarsForScript(credentials, container, config)),
                            cbReadStdOut,
                            cbReadStdErr);

    const QString combinedOutput = stdOut + stdErr;

    if (combinedOutput.contains("address already in use", Qt::CaseInsensitive))
        return ErrorCode::ServerPortAlreadyAllocatedError;
    if (combinedOutput.contains("port is already allocated", Qt::CaseInsensitive))
        return ErrorCode::ServerPortAlreadyAllocatedError;
    if (combinedOutput.contains("Bind for", Qt::CaseInsensitive) && combinedOutput.contains("failed", Qt::CaseInsensitive))
        return ErrorCode::ServerPortAlreadyAllocatedError;
    if (combinedOutput.contains("is already in use by container", Qt::CaseInsensitive))
        return ErrorCode::ServerPortAlreadyAllocatedError;
    if (combinedOutput.contains("invalid publish", Qt::CaseInsensitive))
        return ErrorCode::ServerDockerFailedError;
    if (combinedOutput.contains("No such container", Qt::CaseInsensitive))
        return ErrorCode::ServerContainerMissingError;

    if (e != ErrorCode::NoError) {
        const QString details = compactLogText(combinedOutput, 1000);
        if (!details.isEmpty()) {
            emit logLineReady(tr("Container start output: %1").arg(details));
        }
        return e;
    }

    return verifyContainerIsRunning(credentials, container);
}

ErrorCode ServerController::configureContainerWorker(const ServerCredentials &credentials, DockerContainer container, QJsonObject &config)
{
    const QString containerName = ContainerProps::containerToString(container);
    const QString renderedScript = replaceVars(amnezia::scriptData(ProtocolScriptType::configure_container, container),
                                               genVarsForScript(credentials, container, config));
    const QByteArray scriptBytes = renderedScript.toUtf8();

    emit logLineReady(tr("Prepared configure script for %1 (%2 bytes, sha256 %3...)")
                      .arg(containerName)
                      .arg(scriptBytes.size())
                      .arg(shortSha256Hex(scriptBytes)));

    QString stdOut;
    auto cbReadStdOut = [this, &stdOut](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        emit logLineReady(data);
        return ErrorCode::NoError;
    };
    auto cbReadStdErr = [this, &stdOut](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        const QString trimmed = data.trimmed();
        if (!trimmed.isEmpty()) {
            emit logLineReady(QStringLiteral("[stderr] %1").arg(trimmed));
        }
        return ErrorCode::NoError;
    };

    ErrorCode e = runContainerScript(credentials, container, renderedScript,
                                     cbReadStdOut, cbReadStdErr,
                                     kDockerInstallIdleTimeoutMs /* 120s idle */,
                                     kPrepareHostTotalTimeoutMs /* 300s total */);

    VpnConfigurationsController::updateContainerConfigAfterInstallation(container, config, stdOut);

    if (e == ErrorCode::NoError) {
        writeServerVersion(credentials, container);
    }

    return e;
}

ErrorCode ServerController::startupContainerWorker(const ServerCredentials &credentials, DockerContainer container, const QJsonObject &config)
{
    QString script = amnezia::scriptData(ProtocolScriptType::container_startup, container);
    const QString containerName = ContainerProps::containerToString(container);

    if (script.isEmpty()) {
        return ErrorCode::NoError;
    }

    const QString renderedScript = replaceVars(script, genVarsForScript(credentials, container, config));
    const QByteArray scriptBytes = renderedScript.toUtf8();

    emit logLineReady(tr("Uploading startup script to /opt/amnezia/start.sh in %1 (%2 bytes, sha256 %3...)")
                      .arg(containerName)
                      .arg(scriptBytes.size())
                      .arg(shortSha256Hex(scriptBytes)));

    ErrorCode e = uploadTextFileToContainer(container, credentials, renderedScript, "/opt/amnezia/start.sh");
    if (e) {
        emit logLineReady(tr("Startup script upload failed for %1: %2").arg(containerName, errorString(e)));
        return e;
    }

    emit logLineReady(tr("Startup script uploaded for %1.").arg(containerName));
    emit logLineReady(tr("Launching startup script in %1.").arg(containerName));

    return runScript(credentials,
                     replaceVars("sudo docker exec -d $CONTAINER_NAME sh -c \"chmod a+x /opt/amnezia/start.sh && "
                                 "/opt/amnezia/start.sh\"",
                                 genVarsForScript(credentials, container, config)));
}

ServerController::Vars ServerController::genVarsForScript(const ServerCredentials &credentials, DockerContainer container,
                                                          const QJsonObject &config)
{
    const QJsonObject &openvpnConfig = config.value(ProtocolProps::protoToString(Proto::OpenVpn)).toObject();
    const QJsonObject &cloakConfig = config.value(ProtocolProps::protoToString(Proto::Cloak)).toObject();
    const QJsonObject &ssConfig = config.value(ProtocolProps::protoToString(Proto::ShadowSocks)).toObject();
    const QJsonObject &wireguarConfig = config.value(ProtocolProps::protoToString(Proto::WireGuard)).toObject();
    const QJsonObject &amneziaWireguarConfig = config.value(ProtocolProps::protoToString(Proto::Awg)).toObject();
    const QJsonObject &xrayConfig = config.value(ProtocolProps::protoToString(Proto::Xray)).toObject();
    const QJsonObject &ssxrayConfig = config.value(ProtocolProps::protoToString(Proto::SSXray)).toObject();
    const QJsonObject &hysteria2Config = config.value(ProtocolProps::protoToString(Proto::Hysteria2)).toObject();
    const QJsonObject &anytlsConfig = config.value(ProtocolProps::protoToString(Proto::AnyTls)).toObject();
    const QJsonObject &sftpConfig = config.value(ProtocolProps::protoToString(Proto::Sftp)).toObject();
    const QJsonObject &socks5ProxyConfig = config.value(ProtocolProps::protoToString(Proto::Socks5Proxy)).toObject();

    Vars vars;

    vars.append({ { "$REMOTE_HOST", credentials.hostName } });

    // OpenVPN vars
    vars.append({ { "$OPENVPN_SUBNET_IP",
                    openvpnConfig.value(config_key::subnet_address).toString(protocols::openvpn::defaultSubnetAddress) } });
    vars.append({ { "$OPENVPN_SUBNET_CIDR", openvpnConfig.value(config_key::subnet_cidr).toString(protocols::openvpn::defaultSubnetCidr) } });
    vars.append({ { "$OPENVPN_SUBNET_MASK", openvpnConfig.value(config_key::subnet_mask).toString(protocols::openvpn::defaultSubnetMask) } });

    vars.append({ { "$OPENVPN_PORT", openvpnConfig.value(config_key::port).toString(protocols::openvpn::defaultPort) } });
    vars.append({ { "$OPENVPN_TRANSPORT_PROTO",
                    openvpnConfig.value(config_key::transport_proto).toString(protocols::openvpn::defaultTransportProto) } });

    bool isNcpDisabled = openvpnConfig.value(config_key::ncp_disable).toBool(protocols::openvpn::defaultNcpDisable);
    vars.append({ { "$OPENVPN_NCP_DISABLE", isNcpDisabled ? protocols::openvpn::ncpDisableString : "" } });

    vars.append({ { "$OPENVPN_CIPHER", openvpnConfig.value(config_key::cipher).toString(protocols::openvpn::defaultCipher) } });
    vars.append({ { "$OPENVPN_HASH", openvpnConfig.value(config_key::hash).toString(protocols::openvpn::defaultHash) } });

    bool isTlsAuth = openvpnConfig.value(config_key::tls_auth).toBool(protocols::openvpn::defaultTlsAuth);
    vars.append({ { "$OPENVPN_TLS_AUTH", isTlsAuth ? protocols::openvpn::tlsAuthString : "" } });
    if (!isTlsAuth) {
        // erase $OPENVPN_TA_KEY, so it will not set in OpenVpnConfigurator::genOpenVpnConfig
        vars.append({ { "$OPENVPN_TA_KEY", "" } });
    }

    vars.append({ { "$OPENVPN_ADDITIONAL_CLIENT_CONFIG",
                    openvpnConfig.value(config_key::additional_client_config).toString(protocols::openvpn::defaultAdditionalClientConfig) } });
    vars.append({ { "$OPENVPN_ADDITIONAL_SERVER_CONFIG",
                    openvpnConfig.value(config_key::additional_server_config).toString(protocols::openvpn::defaultAdditionalServerConfig) } });

    // ShadowSocks vars
    vars.append({ { "$SHADOWSOCKS_SERVER_PORT", ssConfig.value(config_key::port).toString(protocols::shadowsocks::defaultPort) } });
    vars.append({ { "$SHADOWSOCKS_LOCAL_PORT",
                    ssConfig.value(config_key::local_port).toString(protocols::shadowsocks::defaultLocalProxyPort) } });
    vars.append({ { "$SHADOWSOCKS_CIPHER", ssConfig.value(config_key::cipher).toString(protocols::shadowsocks::defaultCipher) } });

    vars.append({ { "$CONTAINER_NAME", ContainerProps::containerToString(container) } });
    vars.append({ { "$DOCKERFILE_FOLDER", "/opt/amnezia/" + ContainerProps::containerToString(container) } });

    // Cloak vars
    vars.append({ { "$CLOAK_SERVER_PORT", cloakConfig.value(config_key::port).toString(protocols::cloak::defaultPort) } });
    vars.append({ { "$FAKE_WEB_SITE_ADDRESS", cloakConfig.value(config_key::site).toString(protocols::cloak::defaultRedirSite) } });

    // Xray vars
    vars.append({ { "$XRAY_SITE_NAME", xrayConfig.value(config_key::site).toString(protocols::xray::defaultSite) } });
    vars.append({ { "$XRAY_SERVER_PORT", xrayConfig.value(config_key::port).toString(protocols::xray::defaultPort) } });

    // SSXray vars (Shadowsocks via XRay)
    // NOTE: $SSXRAY_PASSWORD is NOT set here — it is populated by SSXrayConfigurator::createConfig()
    // after reading ss_password.key from the server. genVarsForScript() runs before that,
    // so including it here would replace the placeholder with an empty string.
    vars.append({ { "$SSXRAY_SERVER_PORT", ssxrayConfig.value(config_key::port).toString(protocols::ssxray::defaultPort) } });
    vars.append({ { "$SSXRAY_CIPHER", ssxrayConfig.value(config_key::cipher).toString(protocols::ssxray::defaultCipher) } });

    // Hysteria 2 vars.
    // NOTE: $HYSTERIA_PASSWORD is populated by Hysteria2Configurator::createConfig() after
    // reading hysteria2_password.key from the server. Same pattern as SSXray.
    vars.append({ { "$HYSTERIA_SERVER_PORT",
                    hysteria2Config.value(config_key::port).toString(protocols::hysteria2::defaultPort) } });
    vars.append({ { "$HYSTERIA_LOCAL_PROXY_PORT",
                    hysteria2Config.value(config_key::local_port).toString(protocols::hysteria2::defaultLocalProxyPort) } });
    vars.append({ { "$HYSTERIA_MASQUERADE_HOST",
                    hysteria2Config.value(config_key::site).toString(protocols::hysteria2::defaultMasqueradeHost) } });

    // AnyTLS vars. $ANYTLS_PASSWORD populated by AnyTlsConfigurator.
    vars.append({ { "$ANYTLS_SERVER_PORT",
                    anytlsConfig.value(config_key::port).toString(protocols::anytls::defaultPort) } });
    vars.append({ { "$ANYTLS_LOCAL_PROXY_PORT",
                    anytlsConfig.value(config_key::local_port).toString(protocols::anytls::defaultLocalProxyPort) } });
    vars.append({ { "$ANYTLS_SNI",
                    anytlsConfig.value(config_key::site).toString(protocols::anytls::defaultSni) } });

    // Wireguard vars
    vars.append({ { "$WIREGUARD_SUBNET_IP",
                    wireguarConfig.value(config_key::subnet_address).toString(protocols::wireguard::defaultSubnetAddress) } });
    vars.append({ { "$WIREGUARD_SUBNET_CIDR",
                    wireguarConfig.value(config_key::subnet_cidr).toString(protocols::wireguard::defaultSubnetCidr) } });
    vars.append({ { "$WIREGUARD_SUBNET_MASK",
                    wireguarConfig.value(config_key::subnet_mask).toString(protocols::wireguard::defaultSubnetMask) } });

    vars.append({ { "$WIREGUARD_SERVER_PORT", wireguarConfig.value(config_key::port).toString(protocols::wireguard::defaultPort) } });

    // IPsec vars
    vars.append({ { "$IPSEC_VPN_L2TP_NET", "192.168.42.0/24" } });
    vars.append({ { "$IPSEC_VPN_L2TP_POOL", "192.168.42.10-192.168.42.250" } });
    vars.append({ { "$IPSEC_VPN_L2TP_LOCAL", "192.168.42.1" } });

    vars.append({ { "$IPSEC_VPN_XAUTH_NET", "192.168.43.0/24" } });
    vars.append({ { "$IPSEC_VPN_XAUTH_POOL", "192.168.43.10-192.168.43.250" } });

    vars.append({ { "$IPSEC_VPN_SHA2_TRUNCBUG", "yes" } });

    vars.append({ { "$IPSEC_VPN_VPN_ANDROID_MTU_FIX", "yes" } });
    vars.append({ { "$IPSEC_VPN_DISABLE_IKEV2", "no" } });
    vars.append({ { "$IPSEC_VPN_DISABLE_L2TP", "no" } });
    vars.append({ { "$IPSEC_VPN_DISABLE_XAUTH", "no" } });

    vars.append({ { "$IPSEC_VPN_C2C_TRAFFIC", "no" } });

    vars.append({ { "$PRIMARY_SERVER_DNS", m_settings->primaryDns() } });
    vars.append({ { "$SECONDARY_SERVER_DNS", m_settings->secondaryDns() } });

    // Sftp vars
    vars.append({ { "$SFTP_PORT", sftpConfig.value(config_key::port).toString(QString::number(ProtocolProps::defaultPort(Proto::Sftp))) } });
    vars.append({ { "$SFTP_USER", sftpConfig.value(config_key::userName).toString() } });
    vars.append({ { "$SFTP_PASSWORD", sftpConfig.value(config_key::password).toString() } });

    // Amnezia wireguard vars
    vars.append({ { "$AWG_SUBNET_IP",
                    amneziaWireguarConfig.value(config_key::subnet_address).toString(protocols::wireguard::defaultSubnetAddress) } });
    vars.append({ { "$AWG_SERVER_PORT", amneziaWireguarConfig.value(config_key::port).toString(protocols::awg::defaultPort) } });

    vars.append({ { "$JUNK_PACKET_COUNT", amneziaWireguarConfig.value(config_key::junkPacketCount).toString() } });
    vars.append({ { "$JUNK_PACKET_MIN_SIZE", amneziaWireguarConfig.value(config_key::junkPacketMinSize).toString() } });
    vars.append({ { "$JUNK_PACKET_MAX_SIZE", amneziaWireguarConfig.value(config_key::junkPacketMaxSize).toString() } });
    vars.append({ { "$INIT_PACKET_JUNK_SIZE", amneziaWireguarConfig.value(config_key::initPacketJunkSize).toString() } });
    vars.append({ { "$RESPONSE_PACKET_JUNK_SIZE", amneziaWireguarConfig.value(config_key::responsePacketJunkSize).toString() } });
    vars.append({ { "$INIT_PACKET_MAGIC_HEADER", amneziaWireguarConfig.value(config_key::initPacketMagicHeader).toString() } });
    vars.append({ { "$RESPONSE_PACKET_MAGIC_HEADER", amneziaWireguarConfig.value(config_key::responsePacketMagicHeader).toString() } });
    vars.append({ { "$UNDERLOAD_PACKET_MAGIC_HEADER", amneziaWireguarConfig.value(config_key::underloadPacketMagicHeader).toString() } });
    vars.append({ { "$TRANSPORT_PACKET_MAGIC_HEADER", amneziaWireguarConfig.value(config_key::transportPacketMagicHeader).toString() } });

    vars.append({ { "$COOKIE_REPLY_PACKET_JUNK_SIZE", amneziaWireguarConfig.value(config_key::cookieReplyPacketJunkSize).toString() } });
    vars.append({ { "$TRANSPORT_PACKET_JUNK_SIZE", amneziaWireguarConfig.value(config_key::transportPacketJunkSize).toString() } });
    vars.append({ { "$SPECIAL_JUNK_1", amneziaWireguarConfig.value(config_key::specialJunk1).toString() } });
    vars.append({ { "$SPECIAL_JUNK_2", amneziaWireguarConfig.value(config_key::specialJunk2).toString() } });
    vars.append({ { "$SPECIAL_JUNK_3", amneziaWireguarConfig.value(config_key::specialJunk3).toString() } });
    vars.append({ { "$SPECIAL_JUNK_4", amneziaWireguarConfig.value(config_key::specialJunk4).toString() } });
    vars.append({ { "$SPECIAL_JUNK_5", amneziaWireguarConfig.value(config_key::specialJunk5).toString() } });

    // Socks5 proxy vars
    vars.append({ { "$SOCKS5_PROXY_PORT", socks5ProxyConfig.value(config_key::port).toString(protocols::socks5Proxy::defaultPort) } });
    auto username = socks5ProxyConfig.value(config_key::userName).toString();
    auto password = socks5ProxyConfig.value(config_key::password).toString();
    QString socks5user = (!username.isEmpty() && !password.isEmpty()) ? QString("users %1:CL:%2").arg(username, password) : "";
    vars.append({ { "$SOCKS5_USER", socks5user } });
    vars.append({ { "$SOCKS5_AUTH_TYPE", socks5user.isEmpty() ? "none" : "strong" } });

    QString serverIp = (!ContainerProps::isAwgContainer(container) && 
        container != DockerContainer::WireGuard && container != DockerContainer::Xray)
            ? NetworkUtilities::getIPAddress(credentials.hostName)
            : credentials.hostName;
    if (!serverIp.isEmpty()) {
        vars.append({ { "$SERVER_IP_ADDRESS", serverIp } });
    } else {
        qWarning() << "ServerController::genVarsForScript unable to resolve address for credentials.hostName";
    }

    return vars;
}

QString ServerController::checkSshConnection(const ServerCredentials &credentials, ErrorCode &errorCode)
{
    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };
    auto cbReadStdErr = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };

    errorCode = runScript(credentials,
                          amnezia::scriptData(SharedScriptType::check_connection),
                          cbReadStdOut,
                          cbReadStdErr,
                          kQuickRemoteIdleTimeoutMs,
                          kQuickRemoteTotalTimeoutMs);

    return stdOut;
}

ErrorCode ServerController::logServerEnvironment(const ServerCredentials &credentials)
{
    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data;
        return ErrorCode::NoError;
    };
    auto cbReadStdErr = [&](const QString &data, libssh::Client &) {
        stdOut += data;
        return ErrorCode::NoError;
    };

    emit logLineReady(tr("Detecting server environment..."));

    const ErrorCode error = runScript(credentials,
                                      amnezia::scriptData(SharedScriptType::check_connection),
                                      cbReadStdOut,
                                      cbReadStdErr,
                                      kQuickRemoteIdleTimeoutMs,
                                      kQuickRemoteTotalTimeoutMs);
    if (error != ErrorCode::NoError) {
        return error;
    }

    const QStringList lines = stdOut.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    if (lines.isEmpty()) {
        emit logLineReady(tr("Server preflight returned no output."));
        return ErrorCode::NoError;
    }

    for (const QString &line : lines) {
        emit logLineReady(tr("Server probe: %1").arg(compactLogText(line.trimmed(), 512)));
    }

    return ErrorCode::NoError;
}

void ServerController::cancelInstallation()
{
    m_cancelInstallation.store(true);
    m_sshClient.requestCancel();
    emit logLineReady(tr("Cancel requested, interrupting current SSH operation..."));
}

ErrorCode ServerController::setupServerFirewall(const ServerCredentials &credentials)
{
    return runScript(credentials, replaceVars(amnezia::scriptData(SharedScriptType::setup_host_firewall), genVarsForScript(credentials)));
}

static const int CURRENT_CONFIG_SCHEMA_VERSION = 2;

ErrorCode ServerController::writeServerVersion(const ServerCredentials &credentials, DockerContainer container)
{
    QString containerStr = ContainerProps::containerToString(container).remove("amnezia-");
    QJsonObject versionInfo = readServerVersion(credentials);
    QJsonObject containerInfo = versionInfo.value(containerStr).toObject();

    if (container == DockerContainer::Xray) {
        containerInfo["transport"] = "xhttp";
    } else if (container == DockerContainer::Awg2 || container == DockerContainer::Awg) {
        containerInfo["junkPackets"] = true;
    }

    const QString nowUtc = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ssZ"));
    containerInfo["configuredAt"] = nowUtc;

    versionInfo["clientVersion"] = QStringLiteral(APP_VERSION);
    versionInfo["configSchemaVersion"] = CURRENT_CONFIG_SCHEMA_VERSION;
    versionInfo["lastUpdated"] = nowUtc;
    versionInfo[containerStr] = containerInfo;

    const QByteArray versionJson = QJsonDocument(versionInfo).toJson(QJsonDocument::Indented);
    const QString versionJsonB64 = QString::fromLatin1(versionJson.toBase64());

    const QString writeScript = QStringLiteral(
        "sudo mkdir -p /opt/amnezia && echo '%1' | base64 -d | sudo tee /opt/amnezia/version.json > /dev/null")
        .arg(versionJsonB64);

    return runScript(credentials, writeScript);
}

QJsonObject ServerController::readServerVersion(const ServerCredentials &credentials)
{
    QString stdOut;
    auto cbRead = [&](const QString &data, libssh::Client &) {
        stdOut += data;
        return ErrorCode::NoError;
    };

    ErrorCode e = runScript(credentials, amnezia::scriptData(SharedScriptType::read_server_version), cbRead, cbRead);
    if (e != ErrorCode::NoError) {
        return QJsonObject();
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(stdOut.trimmed().toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return QJsonObject();
    }

    return doc.object();
}

QString ServerController::replaceVars(const QString &script, const Vars &vars)
{
    QString s = script;
    for (const QPair<QString, QString> &var : vars) {
        s.replace(var.first, var.second);
    }
    return s;
}

ErrorCode ServerController::isServerPortBusy(const ServerCredentials &credentials, DockerContainer container, const QJsonObject &config)
{
    if (container == DockerContainer::Dns) {
        return ErrorCode::NoError;
    }

    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };
    auto cbReadStdErr = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };

    const Proto protocol = ContainerProps::defaultProtocol(container);
    const QString containerString = ProtocolProps::protoToString(protocol);
    const QJsonObject containerConfig = config.value(containerString).toObject();

    QStringList fixedPorts = ContainerProps::fixedPortsForContainer(container);

    QString defaultPort("%1");
    QString port = containerConfig.value(config_key::port).toString(defaultPort.arg(ProtocolProps::defaultPort(protocol)));
    QString defaultTransportProto = ProtocolProps::transportProtoToString(ProtocolProps::defaultTransportProto(protocol), protocol);
    QString transportProto = containerConfig.value(config_key::transport_proto).toString(defaultTransportProto);

    QStringList allPorts{port};
    for (const QString &fixedPort : fixedPorts) {
        if (!fixedPort.isEmpty() && !allPorts.contains(fixedPort)) {
            allPorts.append(fixedPort);
        }
    }

    const QString grepPortPattern = allPorts.join("|");
    QStringList ssPortSelectors;
    for (const QString &currentPort : allPorts) {
        ssPortSelectors.append(QString("sport = :%1").arg(currentPort));
    }
    const QString ssPortSelector = ssPortSelectors.join(" or ");

    auto buildPortCheckScript = [&](const QString &transport) {
        const bool isTcp = transport.compare(QStringLiteral("tcp"), Qt::CaseInsensitive) == 0;
        const QString ssCommand = isTcp
                ? QString("sudo ss -lntpH '(%1)' 2>/dev/null").arg(ssPortSelector)
                : QString("sudo ss -lnupH '(%1)' 2>/dev/null").arg(ssPortSelector);
        const QString lsofCommand = isTcp
                ? QString("sudo lsof -i -P -n 2>/dev/null | grep -E ':(%1) ' | grep -i tcp | grep LISTEN")
                          .arg(grepPortPattern)
                : QString("sudo lsof -i -P -n 2>/dev/null | grep -E ':(%1) ' | grep -i udp")
                          .arg(grepPortPattern);
        const QString netstatCommand = isTcp
                ? QString("sudo netstat -lntp 2>/dev/null | grep -E ':(%1)([^0-9]|$)'").arg(grepPortPattern)
                : QString("sudo netstat -lnup 2>/dev/null | grep -E ':(%1)([^0-9]|$)'").arg(grepPortPattern);
        const QString dockerPortsCommand = QString("sudo docker ps --format '{{.Ports}}' 2>/dev/null | "
                               "grep -E '(^|[ ,])([^ ]*:)?(%1)->[0-9]+/%2'")
                           .arg(grepPortPattern, isTcp ? QStringLiteral("tcp") : QStringLiteral("udp"));

        return QString("{ if command -v ss >/dev/null 2>&1; then %1; "
                       "elif command -v lsof >/dev/null 2>&1; then %2; "
                       "elif command -v netstat >/dev/null 2>&1; then %3; "
                   "fi; "
                   "if command -v docker >/dev/null 2>&1; then %4; fi; } || true")
            .arg(ssCommand, lsofCommand, netstatCommand, dockerPortsCommand);
    };

    if (transportProto == "tcpandudp") {
        const QString tcpProtoScript = buildPortCheckScript(QStringLiteral("tcp"));
        const QString udpProtoScript = buildPortCheckScript(QStringLiteral("udp"));

        ErrorCode errorCode =
                runScript(credentials, replaceVars(tcpProtoScript, genVarsForScript(credentials, container)), cbReadStdOut, cbReadStdErr);
        if (errorCode != ErrorCode::NoError) {
            return errorCode;
        }

        errorCode = runScript(credentials, replaceVars(udpProtoScript, genVarsForScript(credentials, container)), cbReadStdOut, cbReadStdErr);
        if (errorCode != ErrorCode::NoError) {
            return errorCode;
        }

        if (!stdOut.isEmpty()) {
            return ErrorCode::ServerPortAlreadyAllocatedError;
        }
        return ErrorCode::NoError;
    }

    const QString script = buildPortCheckScript(transportProto);

    ErrorCode errorCode = runScript(credentials, replaceVars(script, genVarsForScript(credentials, container)), cbReadStdOut, cbReadStdErr);
    if (errorCode != ErrorCode::NoError) {
        return errorCode;
    }

    if (!stdOut.isEmpty()) {
        return ErrorCode::ServerPortAlreadyAllocatedError;
    }
    return ErrorCode::NoError;
}

ErrorCode ServerController::isUserInSudo(const ServerCredentials &credentials, DockerContainer container)
{
    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };
    auto cbReadStdErr = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };

    const QString scriptData = amnezia::scriptData(SharedScriptType::check_user_in_sudo);
    ErrorCode error = runScript(credentials,
                                replaceVars(scriptData, genVarsForScript(credentials)),
                                cbReadStdOut,
                                cbReadStdErr,
                                kQuickRemoteIdleTimeoutMs,
                                kQuickRemoteTotalTimeoutMs);

    if (credentials.userName != "root" && stdOut.contains("sudo:") && !stdOut.contains("uname:") && stdOut.contains("not found"))
        return ErrorCode::ServerSudoPackageIsNotPreinstalled;
    if (credentials.userName != "root" && !stdOut.contains("sudo") && !stdOut.contains("wheel"))
        return ErrorCode::ServerUserNotInSudo;
    if (stdOut.contains("can't cd to") || stdOut.contains("Permission denied") || stdOut.contains("No such file or directory"))
        return ErrorCode::ServerUserDirectoryNotAccessible;
    if (stdOut.contains("sudoers") || stdOut.contains("is not allowed to run sudo on"))
        return ErrorCode::ServerUserNotAllowedInSudoers;
    if (stdOut.contains("password is required"))
        return ErrorCode::ServerUserPasswordRequired;

    return error;
}

ErrorCode ServerController::isServerDpkgBusy(const ServerCredentials &credentials, DockerContainer container)
{
    // NOTE: Do NOT reset m_cancelInstallation or m_sshClient cancel here!
    // A pending cancel request from a previous step must be respected.
    // NOTE: This method is called from a worker thread (QtConcurrent::run in installController),
    // so we execute synchronously here - no nested QtConcurrent or QEventLoop!
    if (m_cancelInstallation.load()) {
        return ErrorCode::ServerCancelInstallation;
    }
    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };
    auto cbReadStdErr = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };

    for (int i = 0; i < 30; ++i) {
        if (m_cancelInstallation.load()) {
            return ErrorCode::ServerCancelInstallation;
        }
        stdOut.clear();
        const ErrorCode checkError = runScript(
            credentials,
            replaceVars(amnezia::scriptData(SharedScriptType::check_server_is_busy), genVarsForScript(credentials)),
            cbReadStdOut,
            cbReadStdErr,
            kBusyProbeIdleTimeoutMs,
            kBusyProbeTotalTimeoutMs);
        if (checkError != ErrorCode::NoError) {
            if (checkError == ErrorCode::SshCommandTimeoutError) {
                emit logLineReady(tr("Package manager probe timed out. The remote command stopped responding."));
            }
            return checkError;
        }

        if (stdOut.contains("Package manager not found"))
            return ErrorCode::ServerPacketManagerError;
        if (stdOut.contains("ps not installed"))
            return ErrorCode::NoError;

        if (stdOut.trimmed().isEmpty()) {
            return ErrorCode::NoError;
        } else {
            qDebug().noquote() << "dpkg busy, fuser output:" << stdOut;
            emit logLineReady(tr("Package manager is busy (attempt %1/30), waiting 10s... Output: %2")
                              .arg(i + 1).arg(stdOut.trimmed()));
            emit serverIsBusy(true);
            for (int waitChunk = 0; waitChunk < 100; ++waitChunk) {
                if (m_cancelInstallation.load()) {
                    emit serverIsBusy(false);
                    return ErrorCode::ServerCancelInstallation;
                }
                QThread::msleep(100);
            }
        }
    }
    emit logLineReady(tr("Package manager is still busy after 30 attempts, giving up."));
    emit serverIsBusy(false);
    return ErrorCode::ServerPacketManagerError;
}

ErrorCode ServerController::getDecryptedPrivateKey(const ServerCredentials &credentials, QString &decryptedPrivateKey,
                                                   const std::function<QString()> &callback)
{
    auto error = m_sshClient.getDecryptedPrivateKey(credentials, decryptedPrivateKey, callback);
    return error;
}

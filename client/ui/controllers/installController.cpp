#include "installController.h"

#include <QDesktopServices>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QtConcurrent>

#include "core/api/apiUtils.h"
#include "core/controllers/serverController.h"
#include "core/controllers/vpnConfigurationController.h"
#include "core/controllers/configSnapshotManager.h"
#include "core/errorstrings.h"
#include "core/scripts_registry.h"
#include "core/networkUtilities.h"
#include "logger.h"
#include "ui/models/protocols/awgConfigModel.h"
#include "ui/models/protocols/wireguardConfigModel.h"
#include "utilities.h"

#include <QFile>
#include <QDateTime>

namespace
{
    Logger logger("InstallController");

    QString sshTargetLabel(const ServerCredentials &credentials)
    {
        return QStringLiteral("%1@%2:%3").arg(credentials.userName, credentials.hostName).arg(credentials.port);
    }

    bool usesPrivateKeyAuthentication(const ServerCredentials &credentials)
    {
        return credentials.secretData.contains(QStringLiteral("BEGIN"), Qt::CaseInsensitive)
               && credentials.secretData.contains(QStringLiteral("PRIVATE KEY"), Qt::CaseInsensitive);
    }

    QString installSshConnectionLogLine(const ServerCredentials &credentials)
    {
        const QString target = sshTargetLabel(credentials);
        if (usesPrivateKeyAuthentication(credentials)) {
            return InstallController::tr("SSH install connection: %1 (private key authentication, key hidden)").arg(target);
        }
        return InstallController::tr("SSH install connection: %1 (password authentication, secret hidden)").arg(target);
    }

    QString setupSshConnectionLogLine(const ServerCredentials &credentials)
    {
        const QString target = sshTargetLabel(credentials);
        if (usesPrivateKeyAuthentication(credentials)) {
            return InstallController::tr("Setting up container on server via SSH: %1 (private key authentication, key hidden)").arg(target);
        }
        return InstallController::tr("Setting up container on server via SSH: %1 (password authentication, secret hidden)").arg(target);
    }

    QString scanSshConnectionLogLine(const ServerCredentials &credentials)
    {
        const QString target = sshTargetLabel(credentials);
        if (usesPrivateKeyAuthentication(credentials)) {
            return InstallController::tr("Checking server for installed containers via SSH: %1 (private key authentication, key hidden)").arg(target);
        }
        return InstallController::tr("Checking server for installed containers via SSH: %1 (password authentication, secret hidden)").arg(target);
    }

    namespace configKey
    {
        constexpr char serviceInfo[] = "service_info";
        constexpr char serviceType[] = "service_type";
        constexpr char serviceProtocol[] = "service_protocol";
        constexpr char userCountryCode[] = "user_country_code";

        constexpr char serverCountryCode[] = "server_country_code";
        constexpr char serverCountryName[] = "server_country_name";
        constexpr char availableCountries[] = "available_countries";

        constexpr char apiConfig[] = "api_config";
        constexpr char authData[] = "auth_data";
    }

    bool hasActualXhttpSchema2Config(const QByteArray &serverConfigRaw)
    {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(serverConfigRaw, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            return false;
        }

        const QJsonObject serverConfig = doc.object();
        const QJsonArray inbounds = serverConfig.value("inbounds").toArray();
        if (inbounds.isEmpty()) {
            return false;
        }

        const QJsonObject streamSettings = inbounds.at(0).toObject().value("streamSettings").toObject();
        if (streamSettings.value("network").toString() != QStringLiteral("xhttp")) {
            return false;
        }

        const QJsonObject xhttpSettings = streamSettings.value("xhttpSettings").toObject();
        if (xhttpSettings.value("path").toString().trimmed().isEmpty()) {
            return false;
        }
        if (xhttpSettings.value("mode").toString() != QStringLiteral("auto")) {
            return false;
        }

        const QJsonObject realitySettings = streamSettings.value("realitySettings").toObject();
        if (realitySettings.value("privateKey").toString().trimmed().isEmpty()) {
            return false;
        }

        const QJsonArray shortIds = realitySettings.value("shortIds").toArray();
        if (shortIds.isEmpty() || shortIds.at(0).toString().trimmed().isEmpty()) {
            return false;
        }

        return true;
    }
}

InstallController::InstallController(const QSharedPointer<ServersModel> &serversModel, const QSharedPointer<ContainersModel> &containersModel,
                                     const QSharedPointer<ProtocolsModel> &protocolsModel,
                                     const QSharedPointer<ClientManagementModel> &clientManagementModel,
                                     const std::shared_ptr<Settings> &settings, QObject *parent)
    : QObject(parent),
      m_serversModel(serversModel),
      m_containersModel(containersModel),
      m_protocolModel(protocolsModel),
      m_clientManagementModel(clientManagementModel),
      m_settings(settings)
{
    m_snapshotManager = std::make_unique<ConfigSnapshotManager>(settings);

    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this]() {
        emit cancelInstallation();
    });

    // Write every install log line to %TEMP%/amnezia_install.log
    connect(this, &InstallController::installLogMessage, this, [](const QString &line) {
        static const QString logPath = QDir::tempPath() + "/amnezia_install.log";
        QFile f(logPath);
        if (f.open(QIODevice::Append | QIODevice::Text)) {
            QTextStream ts(&f);
            ts << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz") << " " << line << "\n";
        }

        const QString trimmedLine = line.trimmed();
        if (!trimmedLine.isEmpty()) {
            logger.info() << trimmedLine;
        }
    });
}

InstallController::~InstallController()
{
#ifdef Q_OS_WINDOWS
    for (QSharedPointer<QProcess> process : m_sftpMountProcesses) {
        Utils::signalCtrl(process->processId(), CTRL_C_EVENT);
        process->kill();
        process->waitForFinished();
    }
#endif
}

bool InstallController::shouldUseAnyTlsVariant() const
{
    return m_shouldUseAnyTlsVariant;
}

void InstallController::setShouldUseAnyTlsVariant(bool shouldUseAnyTlsVariant)
{
    if (m_shouldUseAnyTlsVariant == shouldUseAnyTlsVariant) {
        return;
    }
    m_shouldUseAnyTlsVariant = shouldUseAnyTlsVariant;
    emit shouldUseAnyTlsVariantChanged();
}

void InstallController::install(DockerContainer container, int port, TransportProto transportProto, bool useAnyTlsVariant)
{
    setShouldUseAnyTlsVariant(false);

    // UX: "Use AnyTLS variant" is presented as a toggle inside the XRay setup
    // page instead of a separate wizard card. Internally AnyTLS is a distinct
    // Docker container, so we swap the target here before building the config.
    if (useAnyTlsVariant && container == DockerContainer::Xray) {
        container = DockerContainer::AnyTls;
        emit installLogMessage(tr("AnyTLS variant selected — installing AnyTLS container instead of XRay"));
    }

    QJsonObject config;
    auto mainProto = ContainerProps::defaultProtocol(container);
    for (auto protocol : ContainerProps::protocolsForContainer(container)) {
        QJsonObject containerConfig;

        if (protocol == mainProto) {
            containerConfig.insert(config_key::port, QString::number(port));
            containerConfig.insert(config_key::transport_proto, ProtocolProps::transportProtoToString(transportProto, protocol));
            containerConfig.insert(config_key::subnet_address, protocols::wireguard::defaultSubnetAddress);

            if (container == DockerContainer::Awg2) {
                containerConfig[config_key::protocolVersion] = "2";

                QString junkPacketCount = QString::number(QRandomGenerator::global()->bounded(4, 7));
                QString junkPacketMinSize = QString::number(10);
                QString junkPacketMaxSize = QString::number(50);

                int s1 = QRandomGenerator::global()->bounded(15, 150);
                int s2 = QRandomGenerator::global()->bounded(15, 150);
                int s3 = QRandomGenerator::global()->bounded(1, 64);
                int s4 = QRandomGenerator::global()->bounded(1, 20);

                // Ensure all values are unique and don't create equal packet sizes
                QSet<int> usedValues;
                usedValues.insert(s1);

                while (usedValues.contains(s2) || s1 + AwgConstant::messageInitiationSize == s2 + AwgConstant::messageResponseSize) {
                    s2 = QRandomGenerator::global()->bounded(15, 150);
                }
                usedValues.insert(s2);

                while (usedValues.contains(s3) || s1 + AwgConstant::messageInitiationSize == s3 + AwgConstant::messageCookieReplySize
                       || s2 + AwgConstant::messageResponseSize == s3 + AwgConstant::messageCookieReplySize) {
                    s3 = QRandomGenerator::global()->bounded(1, 64);
                }
                usedValues.insert(s3);

                while (usedValues.contains(s4)) {
                    s4 = QRandomGenerator::global()->bounded(1, 20);
                }

                QString initPacketJunkSize = QString::number(s1);
                QString responsePacketJunkSize = QString::number(s2);
                QString cookieReplyPacketJunkSize = QString::number(s3);
                QString transportPacketJunkSize = QString::number(s4);

                QVector<QPair<QString, QString>> headersValue;
                int min = 5;
                auto max = (std::numeric_limits<qint32>::max)();
                while (headersValue.size() != 4) {
                    auto first = QRandomGenerator::global()->bounded(min, max);
                    auto second = QRandomGenerator::global()->bounded(first, max);
                    min = second;

                    headersValue.push_back(QPair<QString, QString>(QString::number(first), QString::number(second)));
                }

                QString initPacketMagicHeader = headersValue.at(0).first + "-" + headersValue.at(0).second;
                QString responsePacketMagicHeader = headersValue.at(1).first + "-" + headersValue.at(1).second;
                QString underloadPacketMagicHeader = headersValue.at(2).first + "-" + headersValue.at(2).second;
                QString transportPacketMagicHeader = headersValue.at(3).first + "-" + headersValue.at(3).second;

                containerConfig[config_key::junkPacketCount] = junkPacketCount;
                containerConfig[config_key::junkPacketMinSize] = junkPacketMinSize;
                containerConfig[config_key::junkPacketMaxSize] = junkPacketMaxSize;
                containerConfig[config_key::initPacketJunkSize] = initPacketJunkSize;
                containerConfig[config_key::responsePacketJunkSize] = responsePacketJunkSize;
                containerConfig[config_key::initPacketMagicHeader] = initPacketMagicHeader;
                containerConfig[config_key::responsePacketMagicHeader] = responsePacketMagicHeader;
                containerConfig[config_key::underloadPacketMagicHeader] = underloadPacketMagicHeader;
                containerConfig[config_key::transportPacketMagicHeader] = transportPacketMagicHeader;

                containerConfig[config_key::cookieReplyPacketJunkSize] = cookieReplyPacketJunkSize;
                containerConfig[config_key::transportPacketJunkSize] = transportPacketJunkSize;

                containerConfig[config_key::specialJunk1] = protocols::awg::defaultSpecialJunk1;
                containerConfig[config_key::specialJunk2] = protocols::awg::defaultSpecialJunk2;
                containerConfig[config_key::specialJunk3] = protocols::awg::defaultSpecialJunk3;
                containerConfig[config_key::specialJunk4] = protocols::awg::defaultSpecialJunk4;
                containerConfig[config_key::specialJunk5] = protocols::awg::defaultSpecialJunk5;

            } else if (container == DockerContainer::Sftp) {
                containerConfig.insert(config_key::userName, protocols::sftp::defaultUserName);
                containerConfig.insert(config_key::password, Utils::getRandomString(16));
            } else if (container == DockerContainer::Socks5Proxy) {
                containerConfig.insert(config_key::userName, protocols::socks5Proxy::defaultUserName);
                containerConfig.insert(config_key::password, Utils::getRandomString(16));
            }

            config.insert(config_key::container, ContainerProps::containerToString(container));
        }
        config.insert(ProtocolProps::protoToString(protocol), containerConfig);
    }

    ServerCredentials serverCredentials;
    if (m_shouldCreateServer) {
        if (isServerAlreadyExists()) {
            return;
        }
        serverCredentials = m_processedServerCredentials;
    } else {
        int serverIndex = m_serversModel->getProcessedServerIndex();
        serverCredentials = qvariant_cast<ServerCredentials>(m_serversModel->data(serverIndex, ServersModel::Roles::CredentialsRole));
    }

    // ServerController is a QObject living on the GUI thread; the install
    // lambda runs on a worker thread (QtConcurrent::run). When the QSharedPointer
    // copy inside the lambda goes out of scope on the worker, naive delete from
    // the wrong thread breaks Qt and can hang the UI on subsequent
    // cancelInstallation emits. Route destruction through the owning thread's
    // event loop via deleteLater so queued signals/slots stay valid until drained.
    QSharedPointer<ServerController> serverController(new ServerController(m_settings), &QObject::deleteLater);
    connect(serverController.get(), &ServerController::serverIsBusy, this, &InstallController::serverIsBusy);
    connect(serverController.get(), &ServerController::logLineReady, this, &InstallController::installLogMessage);
    connect(this, &InstallController::cancelInstallation, serverController.get(), &ServerController::cancelInstallation);

    bool shouldCreateServer = m_shouldCreateServer;
    ServerCredentials processedCreds = m_processedServerCredentials;
    QSet<DockerContainer> knownContainers;
    if (!shouldCreateServer) {
        for (DockerContainer currentContainer : ContainerProps::allContainers()) {
            if (!m_containersModel->getContainerConfig(currentContainer).isEmpty()) {
                knownContainers.insert(currentContainer);
            }
        }
    }

    [[maybe_unused]] auto installFuture = QtConcurrent::run([this, container, config, serverCredentials, shouldCreateServer, processedCreds, serverController,
                       knownContainers]() mutable {
      try {
        // --- Background thread: ALL SSH operations ---
                emit installLogMessage(installSshConnectionLogLine(serverCredentials));
        emit installLogMessage(tr("Checking installed containers..."));
        QMap<DockerContainer, QJsonObject> installedContainers;
        ErrorCode errorCode = getAlreadyInstalledContainers(serverCredentials, serverController, installedContainers);
        if (errorCode) {
            emit installLogMessage(tr("Error checking containers: %1").arg(errorString(errorCode)));
            emit installationErrorOccurred(errorCode);
            return;
        }

        QString finishMessage;

        if (!installedContainers.contains(container)) {
            emit installLogMessage(setupSshConnectionLogLine(serverCredentials));
            errorCode = serverController->setupContainer(serverCredentials, container, config);
            if (errorCode) {
                emit installLogMessage(tr("Setup failed: %1").arg(errorString(errorCode)));
                emit installationErrorOccurred(errorCode);
                return;
            }

            installedContainers.insert(container, config);
            finishMessage = tr("%1 installed successfully. ").arg(ContainerProps::containerHumanNames().value(container));
        } else {
            emit installLogMessage(tr("Container already exists on server."));
            finishMessage = tr("%1 is already installed on the server. ").arg(ContainerProps::containerHumanNames().value(container));
        }

        // Pre-create protocol configs (SSH) while still on bg thread, so
        // installServer/installContainer don't block the UI with SSH calls.
        emit installLogMessage(tr("Creating protocol configurations..."));
        VpnConfigurationsController vpnConfigurationController(m_settings, serverController);
        auto buildLocalProfile = [&](DockerContainer currentContainer, QJsonObject &containerConfig, bool required) {
            if (!ContainerProps::isSupportedByCurrentPlatform(currentContainer)) {
                return true;
            }

            const QString containerName = ContainerProps::containerToString(currentContainer);
            emit installLogMessage(tr("Building local profile for %1...").arg(containerName));
            ErrorCode profileError = vpnConfigurationController.createProtocolConfigForContainer(
                serverCredentials, currentContainer, containerConfig);
            if (profileError) {
                emit installLogMessage(tr("Config creation failed for %1: %2")
                                       .arg(containerName, errorString(profileError)));
                if (required) {
                    errorCode = profileError;
                }
                return false;
            }

            emit installLogMessage(tr("Profile ready for %1.").arg(containerName));
            return true;
        };

        if (installedContainers.contains(container) && !knownContainers.contains(container)) {
            QJsonObject targetConfig = installedContainers.value(container);
            if (!buildLocalProfile(container, targetConfig, true)) {
                emit installationErrorOccurred(errorCode);
                return;
            }
            installedContainers[container] = targetConfig;
        }

        QList<DockerContainer> failedExistingContainers;
        for (auto it = installedContainers.begin(); it != installedContainers.end(); ++it) {
            if (it.key() == container) {
                continue;
            }
            if (knownContainers.contains(it.key())) {
                continue;
            }

            QJsonObject cfg = it.value();
            if (!buildLocalProfile(it.key(), cfg, false)) {
                failedExistingContainers.append(it.key());
                continue;
            }
            it.value() = cfg;
        }

        for (DockerContainer failedContainer : failedExistingContainers) {
            installedContainers.remove(failedContainer);
        }

        emit installLogMessage(tr("Finalizing..."));

        // --- Main thread: model updates + appendClient (lightweight SSH) ---
        // Qt models (beginResetModel, dataChanged, beginInsertRows) MUST run
        // on the GUI thread. appendClient does brief SSH for clientsTable sync.
        QMetaObject::invokeMethod(this, [this, container, installedContainers, serverCredentials,
                                         shouldCreateServer, processedCreds, serverController, finishMessage]() mutable {
            if (shouldCreateServer) {
                installServer(container, installedContainers, processedCreds, serverController, finishMessage);
            } else {
                installContainer(container, installedContainers, serverCredentials, serverController, finishMessage);
            }
        }, Qt::QueuedConnection);
      } catch (const std::exception &ex) {
        qCritical() << "Installation thread exception:" << ex.what();
        emit installLogMessage(tr("Internal error: %1").arg(QString::fromUtf8(ex.what())));
        emit installationErrorOccurred(ErrorCode::InternalError);
      } catch (...) {
        qCritical() << "Installation thread unknown exception";
        emit installLogMessage(tr("Internal error (unknown)"));
        emit installationErrorOccurred(ErrorCode::InternalError);
      }
    });
}

void InstallController::installServer(const DockerContainer container, const QMap<DockerContainer, QJsonObject> &installedContainers,
                                      const ServerCredentials &serverCredentials, const QSharedPointer<ServerController> &serverController,
                                      QString &finishMessage)
{
    if (installedContainers.size() > 1) {
        finishMessage += tr("\nAdded containers that were already installed on the server");
    }

    QJsonObject server;
    server.insert(config_key::hostName, serverCredentials.hostName);
    server.insert(config_key::userName, serverCredentials.userName);
    server.insert(config_key::password, serverCredentials.secretData);
    server.insert(config_key::port, serverCredentials.port);
    server.insert(config_key::description, m_settings->nextAvailableServerName());

    QJsonArray containerConfigs;
    for (auto iterator = installedContainers.begin(); iterator != installedContainers.end(); iterator++) {
        auto containerConfig = iterator.value();
        // Protocol configs already prepared by bg thread (createProtocolConfigForContainer)
        containerConfigs.append(containerConfig);

        if (ContainerProps::isSupportedByCurrentPlatform(iterator.key())) {
            auto errorCode = m_clientManagementModel->appendClient(iterator.key(), serverCredentials, containerConfig,
                                                              QString("Admin [%1]").arg(QSysInfo::prettyProductName()), serverController);
            if (errorCode) {
                emit installationErrorOccurred(errorCode);
                return;
            }
        }
    }

    server.insert(config_key::containers, containerConfigs);
    server.insert(config_key::defaultContainer, ContainerProps::containerToString(container));

    QMetaObject::invokeMethod(this, [this, server, finishMessage]() {
        m_serversModel->addServer(server);
        emit installServerFinished(finishMessage);
    }, Qt::QueuedConnection);
}

void InstallController::installContainer(const DockerContainer container, const QMap<DockerContainer, QJsonObject> &installedContainers,
                                         const ServerCredentials &serverCredentials,
                                         const QSharedPointer<ServerController> &serverController, QString &finishMessage)
{
    bool isInstalledContainerAddedToGui = false;

    for (auto iterator = installedContainers.begin(); iterator != installedContainers.end(); iterator++) {
        QJsonObject containerConfig = m_containersModel->getContainerConfig(iterator.key());
        if (containerConfig.isEmpty()) {
            // Protocol configs already prepared by bg thread (createProtocolConfigForContainer)
            containerConfig = iterator.value();

            m_serversModel->addContainerConfig(iterator.key(), containerConfig);

            if (ContainerProps::isSupportedByCurrentPlatform(iterator.key())) {
                auto errorCode = m_clientManagementModel->appendClient(iterator.key(), serverCredentials, containerConfig,
                                                                  QString("Admin [%1]").arg(QSysInfo::prettyProductName()), serverController);
                if (errorCode) {
                    emit installationErrorOccurred(errorCode);
                    return;
                }
            }

            if (container != iterator.key()) { // skip the newly installed container
                isInstalledContainerAddedToGui = true;
            }
        }
    }
    if (isInstalledContainerAddedToGui) {
        finishMessage += tr("\nAlready installed containers were found on the server. "
                            "All installed containers have been added to the application");
    }

    QMetaObject::invokeMethod(this, [this, finishMessage, container]() {
        emit installContainerFinished(finishMessage, ContainerProps::containerService(container) == ServiceType::Other, static_cast<int>(container));
    }, Qt::QueuedConnection);
}

bool InstallController::isServerAlreadyExists()
{
    for (int i = 0; i < m_serversModel->getServersCount(); i++) {
        auto modelIndex = m_serversModel->index(i);
        const ServerCredentials credentials =
                qvariant_cast<ServerCredentials>(m_serversModel->data(modelIndex, ServersModel::Roles::CredentialsRole));
        if (m_processedServerCredentials.hostName == credentials.hostName && m_processedServerCredentials.port == credentials.port) {
            emit serverAlreadyExists(i);
            return true;
        }
    }
    return false;
}

void InstallController::scanServerForInstalledContainers()
{
    int serverIndex = m_serversModel->getProcessedServerIndex();
    ServerCredentials serverCredentials =
            qvariant_cast<ServerCredentials>(m_serversModel->data(serverIndex, ServersModel::Roles::CredentialsRole));

    QMap<DockerContainer, QJsonObject> installedContainers;
    QSharedPointer<ServerController> serverController(new ServerController(m_settings));
    ErrorCode errorCode = getAlreadyInstalledContainers(serverCredentials, serverController, installedContainers);

    if (errorCode == ErrorCode::NoError) {
        bool isInstalledContainerAddedToGui = false;
        VpnConfigurationsController vpnConfigurationController(m_settings, serverController);

        for (auto iterator = installedContainers.begin(); iterator != installedContainers.end(); iterator++) {
            auto container = iterator.key();
            QJsonObject containerConfig = m_containersModel->getContainerConfig(container);
            if (containerConfig.isEmpty()) {
                containerConfig = iterator.value();

                if (ContainerProps::isSupportedByCurrentPlatform(container)) {
                    auto errorCode =
                            vpnConfigurationController.createProtocolConfigForContainer(serverCredentials, container, containerConfig);
                    if (errorCode) {
                        emit installationErrorOccurred(errorCode);
                        return;
                    }
                    m_serversModel->addContainerConfig(container, containerConfig);

                    errorCode = m_clientManagementModel->appendClient(container, serverCredentials, containerConfig,
                                                                      QString("Admin [%1]").arg(QSysInfo::prettyProductName()),
                                                                      serverController);
                    if (errorCode) {
                        emit installationErrorOccurred(errorCode);
                        return;
                    }
                } else {
                    m_serversModel->addContainerConfig(container, containerConfig);
                }

                isInstalledContainerAddedToGui = true;
            }
        }

        emit scanServerFinished(isInstalledContainerAddedToGui);
        return;
    }

    emit installationErrorOccurred(errorCode);
}

void InstallController::checkServerContainers()
{
    ServerCredentials creds;

    int serverIndex = m_serversModel->getProcessedServerIndex();
    if (serverIndex >= 0) {
        creds = qvariant_cast<ServerCredentials>(m_serversModel->data(serverIndex, ServersModel::Roles::CredentialsRole));
    } else if (m_processedServerCredentials.isValid()) {
        creds = m_processedServerCredentials;
    } else {
        emit installLogMessage(tr("Server check: no credentials available, skipping"));
        m_serverInstalledContainers.clear();
        emit serverContainersChecked();
        return;
    }

    emit installLogMessage(scanSshConnectionLogLine(creds));

    [[maybe_unused]] auto scanFuture = QtConcurrent::run([this, creds]() {
        QSharedPointer<ServerController> sc(new ServerController(m_settings));
        connect(sc.get(), &ServerController::logLineReady, this, &InstallController::installLogMessage);

        QMap<DockerContainer, QJsonObject> installed;
        ErrorCode err = getAlreadyInstalledContainers(creds, sc, installed);

        QSet<int> result;
        if (err == ErrorCode::NoError) {
            for (auto it = installed.begin(); it != installed.end(); ++it) {
                result.insert(static_cast<int>(it.key()));
            }
            QStringList names;
            for (auto it = installed.begin(); it != installed.end(); ++it) {
                names << ContainerProps::containerToString(it.key());
            }
            if (names.isEmpty()) {
                emit installLogMessage(tr("Server check: no Amnezia containers found on server"));
            } else {
                emit installLogMessage(tr("Server check: found %1 container(s): %2")
                                           .arg(names.size())
                                           .arg(names.join(", ")));
            }
        } else {
            emit installLogMessage(tr("Server check: failed to query containers (error %1)")
                                       .arg(static_cast<int>(err)));
        }

        QMetaObject::invokeMethod(this, [this, result]() {
            m_serverInstalledContainers = result;
            emit serverContainersChecked();
        }, Qt::QueuedConnection);
    });
}

bool InstallController::isContainerInstalledOnServer(int container) const
{
    return m_serverInstalledContainers.contains(container);
}

ErrorCode InstallController::getAlreadyInstalledContainers(const ServerCredentials &credentials,
                                                           const QSharedPointer<ServerController> &serverController,
                                                           QMap<DockerContainer, QJsonObject> &installedContainers)
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

    QString script = QString("sudo docker ps --format '{{.Names}} {{.Ports}}'");

    ErrorCode errorCode = serverController->runScript(credentials, script, cbReadStdOut, cbReadStdErr);
    if (errorCode != ErrorCode::NoError) {
        return errorCode;
    }

    auto containersInfo = stdOut.split("\n");
    for (auto &containerInfo : containersInfo) {
        if (containerInfo.isEmpty()) {
            continue;
        }
        const static QRegularExpression containerAndPortRegExp("(amnezia[-a-z0-9]*).*?:([0-9]*)->[0-9]*/(udp|tcp).*");
        QRegularExpressionMatch containerAndPortMatch = containerAndPortRegExp.match(containerInfo);
        if (containerAndPortMatch.hasMatch()) {
            QString name = containerAndPortMatch.captured(1);
            QString port = containerAndPortMatch.captured(2);
            QString transportProto = containerAndPortMatch.captured(3);
            DockerContainer container = ContainerProps::containerFromString(name);

            emit installLogMessage(tr("Inspecting installed container: %1").arg(name));

            QJsonObject config;
            bool skipBrokenContainer = false;
            Proto mainProto = ContainerProps::defaultProtocol(container);
            const auto &protocols = ContainerProps::protocolsForContainer(container);

            for (const auto &protocol : protocols) {
                QJsonObject containerConfig;

                // for Multiprotocols (OpenVPN over SS, OpenVPN over Cloak)
                bool shouldProcessProtocol = false;
                if (container == DockerContainer::ShadowSocks || container == DockerContainer::Cloak) {
                    shouldProcessProtocol = true;
                } else {
                    shouldProcessProtocol = (protocol == mainProto);
                }

                if (shouldProcessProtocol) {
                    containerConfig.insert(config_key::port, port);
                    containerConfig.insert(config_key::transport_proto, transportProto);

                    if (protocol == Proto::Awg) {
                        QString configPath = amnezia::protocols::awg::serverConfigPath;
                        if (container == DockerContainer::Awg) {
                            configPath = amnezia::protocols::awg::serverLegacyConfigPath;
                        }
                        QString serverConfig = serverController->getTextFileFromContainer(container, credentials, configPath, errorCode);
                        if (errorCode != ErrorCode::NoError) {
                            return errorCode;
                        }

                        QMap<QString, QString> serverConfigMap;
                        auto serverConfigLines = serverConfig.split("\n");
                        for (auto &line : serverConfigLines) {
                            auto trimmedLine = line.trimmed();
                            if (trimmedLine.startsWith("[") && trimmedLine.endsWith("]")) {
                                continue;
                            } else {
                                QStringList parts = trimmedLine.split(" = ");
                                if (parts.count() == 2) {
                                    serverConfigMap.insert(parts[0].trimmed(), parts[1].trimmed());
                                }
                            }
                        }

                        containerConfig[config_key::subnet_address] = serverConfigMap.value("Address").remove("/24");
                        containerConfig[config_key::junkPacketCount] = serverConfigMap.value(config_key::junkPacketCount);
                        containerConfig[config_key::junkPacketMinSize] = serverConfigMap.value(config_key::junkPacketMinSize);
                        containerConfig[config_key::junkPacketMaxSize] = serverConfigMap.value(config_key::junkPacketMaxSize);
                        containerConfig[config_key::initPacketJunkSize] = serverConfigMap.value(config_key::initPacketJunkSize);
                        containerConfig[config_key::responsePacketJunkSize] = serverConfigMap.value(config_key::responsePacketJunkSize);
                        containerConfig[config_key::initPacketMagicHeader] = serverConfigMap.value(config_key::initPacketMagicHeader);
                        containerConfig[config_key::responsePacketMagicHeader] = serverConfigMap.value(config_key::responsePacketMagicHeader);
                        containerConfig[config_key::underloadPacketMagicHeader] =
                                serverConfigMap.value(config_key::underloadPacketMagicHeader);
                        containerConfig[config_key::transportPacketMagicHeader] =
                                serverConfigMap.value(config_key::transportPacketMagicHeader);

                        // hack to parse i1-i5 from commented lines in server config
                        containerConfig[config_key::specialJunk1] = serverConfigMap.value(QString("# ") + config_key::specialJunk1);
                        containerConfig[config_key::specialJunk2] = serverConfigMap.value(QString("# ") + config_key::specialJunk2);
                        containerConfig[config_key::specialJunk3] = serverConfigMap.value(QString("# ") + config_key::specialJunk3);
                        containerConfig[config_key::specialJunk4] = serverConfigMap.value(QString("# ") + config_key::specialJunk4);
                        containerConfig[config_key::specialJunk5] = serverConfigMap.value(QString("# ") + config_key::specialJunk5);

                        if (container == DockerContainer::Awg2) {
                            containerConfig[config_key::protocolVersion] = "2";
                            containerConfig[config_key::cookieReplyPacketJunkSize] =
                                    serverConfigMap.value(config_key::cookieReplyPacketJunkSize);
                            containerConfig[config_key::transportPacketJunkSize] = serverConfigMap.value(config_key::transportPacketJunkSize);
                        }

                    } else if (protocol == Proto::WireGuard) {
                        QString serverConfig = serverController->getTextFileFromContainer(container, credentials,
                                                                                          protocols::wireguard::serverConfigPath, errorCode);
                        if (errorCode != ErrorCode::NoError) {
                            return errorCode;
                        }

                        QMap<QString, QString> serverConfigMap;
                        auto serverConfigLines = serverConfig.split("\n");
                        for (auto &line : serverConfigLines) {
                            auto trimmedLine = line.trimmed();
                            if (trimmedLine.startsWith("[") && trimmedLine.endsWith("]")) {
                                continue;
                            } else {
                                QStringList parts = trimmedLine.split(" = ");
                                if (parts.count() == 2) {
                                    serverConfigMap.insert(parts[0].trimmed(), parts[1].trimmed());
                                }
                            }
                        }
                        containerConfig[config_key::subnet_address] = serverConfigMap.value("Address").remove("/24");
                    } else if (protocol == Proto::Sftp) {
                        stdOut.clear();
                        script = QString("sudo docker inspect --format '{{.Config.Cmd}}' %1").arg(name);

                        ErrorCode errorCode = serverController->runScript(credentials, script, cbReadStdOut, cbReadStdErr);
                        if (errorCode != ErrorCode::NoError) {
                            return errorCode;
                        }

                        auto sftpInfo = stdOut.split(":");
                        if (sftpInfo.size() < 2) {
                            logger.error() << "Key parameters for the sftp container are missing";
                            continue;
                        }
                        auto userName = sftpInfo.at(0);
                        userName = userName.remove(0, 1);
                        auto password = sftpInfo.at(1);

                        containerConfig.insert(config_key::userName, userName);
                        containerConfig.insert(config_key::password, password);
                    } else if (protocol == Proto::Socks5Proxy) {
                        QString proxyConfig = serverController->getTextFileFromContainer(container, credentials,
                                                                                         protocols::socks5Proxy::proxyConfigPath, errorCode);
                        if (errorCode != ErrorCode::NoError) {
                            return errorCode;
                        }

                        const static QRegularExpression usernameAndPasswordRegExp("users (\\w+):CL:(\\w+)");
                        QRegularExpressionMatch usernameAndPasswordMatch = usernameAndPasswordRegExp.match(proxyConfig);

                        if (usernameAndPasswordMatch.hasMatch()) {
                            QString userName = usernameAndPasswordMatch.captured(1);
                            QString password = usernameAndPasswordMatch.captured(2);

                            containerConfig.insert(config_key::userName, userName);
                            containerConfig.insert(config_key::password, password);
                        }
                    } else if (protocol == Proto::Xray) {
                        QString currentConfig = serverController->getTextFileFromContainer(
                                container, credentials, amnezia::protocols::xray::serverConfigPath, errorCode);
                        if (errorCode != ErrorCode::NoError) {
                            if (errorCode == ErrorCode::ServerCheckFailed) {
                                // Zombie container with no server.json (left by a failed install):
                                // skip it instead of aborting the whole scan with error 200.
                                skipBrokenContainer = true;
                                errorCode = ErrorCode::NoError;
                                break;
                            }
                            return errorCode;
                        }

                        QJsonDocument doc = QJsonDocument::fromJson(currentConfig.toUtf8());
                        if (doc.isNull() || !doc.isObject()) {
                            logger.error() << "Failed to parse server config JSON";
                            errorCode = ErrorCode::InternalError;
                            return errorCode;
                        }
                        QJsonObject serverConfig = doc.object();

                        if (!serverConfig.contains("inbounds")) {
                            logger.error() << "Server config missing 'inbounds' field";
                            errorCode = ErrorCode::InternalError;
                            return errorCode;
                        }

                        QJsonArray inbounds = serverConfig["inbounds"].toArray();
                        if (inbounds.isEmpty()) {
                            logger.error() << "Server config has empty 'inbounds' array";
                            errorCode = ErrorCode::InternalError;
                            return errorCode;
                        }

                        QJsonObject inbound = inbounds[0].toObject();
                        if (!inbound.contains("streamSettings")) {
                            logger.error() << "Inbound missing 'streamSettings' field";
                            errorCode = ErrorCode::InternalError;
                            return errorCode;
                        }

                        QJsonObject streamSettings = inbound["streamSettings"].toObject();
                        QJsonObject realitySettings = streamSettings["realitySettings"].toObject();
                        // Reality/TCP exposes a serverNames site; mKCP does NOT. Only pull the
                        // site when reality is actually present — a kcp xray has no serverNames,
                        // and that must NOT abort the whole container scan. This hard-require was
                        // throwing InternalError (101) on EVERY install while xray runs on mKCP. by vovankrot
                        if (realitySettings.contains("serverNames")) {
                            QString siteName = realitySettings["serverNames"][0].toString();
                            containerConfig.insert(config_key::site, siteName);
                        }
                    } else if (protocol == Proto::OpenVpn) {
                        QString serverConfig = serverController->getTextFileFromContainer(container, credentials,
                                                                                          protocols::openvpn::serverConfigPath, errorCode);
                        if (errorCode != ErrorCode::NoError) {
                            return errorCode;
                        }

                        QMap<QString, QString> serverConfigMap;
                        auto serverConfigLines = serverConfig.split("\n");
                        for (auto &line : serverConfigLines) {
                            auto trimmedLine = line.trimmed();
                            if (trimmedLine.startsWith("#") || trimmedLine.isEmpty()) {
                                continue;
                            } else {
                                QStringList parts = trimmedLine.split(" ");
                                if (parts.count() >= 2) {
                                    QString key = parts[0];
                                    QString value = parts.mid(1).join(" ");
                                    serverConfigMap.insert(key, value);
                                }
                            }
                        }

                        QString serverValue = serverConfigMap.value("server");

                        if (!serverValue.isEmpty()) {
                            QStringList serverParts = serverValue.split(" ");
                            if (serverParts.count() >= 1) {
                                containerConfig[config_key::subnet_address] = serverParts[0];
                            }
                        }

                        bool ncpDisable = serverConfig.contains("ncp-disable");
                        containerConfig[config_key::ncp_disable] = ncpDisable;

                        bool tlsAuth = serverConfig.contains("tls-auth");
                        containerConfig[config_key::tls_auth] = tlsAuth;

                        bool blockOutsideDns = serverConfig.contains("block-outside-dns");

                        containerConfig[config_key::block_outside_dns] = blockOutsideDns;

                        QString cipher = serverConfigMap.value("cipher");
                        if (!cipher.isEmpty()) {
                            containerConfig[config_key::cipher] = cipher;
                        }

                        QString hash = serverConfigMap.value("auth");
                        if (!hash.isEmpty()) {
                            containerConfig[config_key::hash] = hash;
                        }
                    } else if (protocol == Proto::Cloak) {
                        QString cloakConfig = serverController->getTextFileFromContainer(container, credentials,
                                                                                         "/opt/amnezia/cloak/ck-config.json", errorCode);
                        if (errorCode != ErrorCode::NoError) {
                            return errorCode;
                        }

                        QJsonDocument doc = QJsonDocument::fromJson(cloakConfig.toUtf8());

                        if (!doc.isNull() && doc.isObject()) {
                            QJsonObject cloakConfigObj = doc.object();

                            QString site = cloakConfigObj.value("RedirAddr").toString();
                            if (!site.isEmpty()) {
                                containerConfig[config_key::site] = site;
                            }
                        } else {
                            emit installLogMessage(tr("Failed to parse Cloak config for %1.").arg(name));
                            qDebug() << "Failed to parse main loop Cloak JSON config";
                        }

                    } else if (protocol == Proto::ShadowSocks) {
                        QString shadowsocksConfig = serverController->getTextFileFromContainer(
                                container, credentials, "/opt/amnezia/shadowsocks/ss-config.json", errorCode);
                        if (errorCode != ErrorCode::NoError) {
                            return errorCode;
                        }

                        QJsonDocument doc = QJsonDocument::fromJson(shadowsocksConfig.toUtf8());

                        if (!doc.isNull() && doc.isObject()) {
                            QJsonObject ssConfigObj = doc.object();
                            QString cipher = ssConfigObj.value("method").toString();
                            if (!cipher.isEmpty()) {
                                containerConfig[config_key::cipher] = cipher;
                            }
                        } else {
                            emit installLogMessage(tr("Failed to parse Shadowsocks config for %1.").arg(name));
                            qDebug() << "Failed to parse main loop Shadowsocks JSON config";
                        }
                    }

                    config.insert(config_key::container, ContainerProps::containerToString(container));
                }
                if (shouldProcessProtocol) {
                    config.insert(ProtocolProps::protoToString(protocol), containerConfig);
                }
            }
            if (skipBrokenContainer) {
                emit installLogMessage(tr("Skipped container %1: no readable config (zombie from a failed install); it will be reinstalled cleanly").arg(name));
            } else {
                installedContainers.insert(container, config);
                emit installLogMessage(tr("Finished inspecting container: %1").arg(name));
            }
        }

        const static QRegularExpression torOrDnsRegExp("(amnezia-(?:torwebsite|dns)).*?([0-9]*)/(udp|tcp).*");
        QRegularExpressionMatch torOrDnsRegMatch = torOrDnsRegExp.match(containerInfo);
        if (torOrDnsRegMatch.hasMatch()) {
            QString name = torOrDnsRegMatch.captured(1);
            QString port = torOrDnsRegMatch.captured(2);
            QString transportProto = torOrDnsRegMatch.captured(3);
            DockerContainer container = ContainerProps::containerFromString(name);

            emit installLogMessage(tr("Inspecting installed container: %1").arg(name));

            QJsonObject config;
            Proto mainProto = ContainerProps::defaultProtocol(container);
            for (auto protocol : ContainerProps::protocolsForContainer(container)) {
                QJsonObject containerConfig;
                if (protocol == mainProto) {
                    containerConfig.insert(config_key::port, port);
                    containerConfig.insert(config_key::transport_proto, transportProto);

                    if (protocol == Proto::TorWebSite) {
                        stdOut.clear();
                        script = QString("sudo docker exec %1 sh -c 'cat /var/lib/tor/hidden_service/hostname'").arg(name);

                        ErrorCode errorCode = serverController->runScript(credentials, script, cbReadStdOut, cbReadStdErr);
                        if (errorCode != ErrorCode::NoError) {
                            return errorCode;
                        }

                        if (stdOut.isEmpty()) {
                            logger.error() << "Key parameters for the tor container are missing";
                            continue;
                        }

                        QString onion = stdOut;
                        onion.replace("\n", "");
                        containerConfig.insert(config_key::site, onion);
                    }

                    config.insert(config_key::container, ContainerProps::containerToString(container));
                }
                config.insert(ProtocolProps::protoToString(protocol), containerConfig);
            }
            installedContainers.insert(container, config);
            emit installLogMessage(tr("Finished inspecting container: %1").arg(name));
        }
    }

    return ErrorCode::NoError;
}

void InstallController::updateContainer(QJsonObject config)
{
    int serverIndex = m_serversModel->getProcessedServerIndex();
    ServerCredentials serverCredentials =
            qvariant_cast<ServerCredentials>(m_serversModel->data(serverIndex, ServersModel::Roles::CredentialsRole));

    const DockerContainer container = ContainerProps::containerFromString(config.value(config_key::container).toString());
    QJsonObject oldContainerConfig = m_containersModel->getContainerConfig(container);
    ErrorCode errorCode = ErrorCode::NoError;

    if (isUpdateDockerContainerRequired(container, oldContainerConfig, config)) {
        QSharedPointer<ServerController> serverController(new ServerController(m_settings), &QObject::deleteLater);
        connect(serverController.get(), &ServerController::serverIsBusy, this, &InstallController::serverIsBusy);
        connect(this, &InstallController::cancelInstallation, serverController.get(), &ServerController::cancelInstallation);

        errorCode = serverController->updateContainer(serverCredentials, container, oldContainerConfig, config);
        clearCachedProfile(serverController);
    }

    if (errorCode == ErrorCode::NoError) {
        m_serversModel->updateContainerConfig(container, config);
        m_protocolModel->updateModel(config);

        auto defaultContainer = qvariant_cast<DockerContainer>(m_serversModel->data(serverIndex, ServersModel::Roles::DefaultContainerRole));
        if ((serverIndex == m_serversModel->getDefaultServerIndex()) && (container == defaultContainer)) {
            emit currentContainerUpdated();
        } else {
            emit updateContainerFinished(tr("Settings updated successfully"));
        }

        return;
    }

    emit installationErrorOccurred(errorCode);
}

void InstallController::rebootProcessedServer()
{
    int serverIndex = m_serversModel->getProcessedServerIndex();
    QString serverName = m_serversModel->data(serverIndex, ServersModel::Roles::NameRole).toString();

    QSharedPointer<ServerController> serverController(new ServerController(m_settings));
    const auto errorCode = m_serversModel->rebootServer(serverController);
    if (errorCode == ErrorCode::NoError) {
        emit rebootProcessedServerFinished(tr("Server '%1' was rebooted").arg(serverName));
    } else {
        emit installationErrorOccurred(errorCode);
    }
}

void InstallController::removeProcessedServer()
{
    int serverIndex = m_serversModel->getProcessedServerIndex();
    QString serverName = m_serversModel->data(serverIndex, ServersModel::Roles::NameRole).toString();

    m_serversModel->removeServer();
    emit removeProcessedServerFinished(tr("Server '%1' was removed").arg(serverName));
}

void InstallController::removeAllContainers()
{
    int serverIndex = m_serversModel->getProcessedServerIndex();
    QString serverName = m_serversModel->data(serverIndex, ServersModel::Roles::NameRole).toString();

    QSharedPointer<ServerController> serverController(new ServerController(m_settings));
    ErrorCode errorCode = m_serversModel->removeAllContainers(serverController);
    if (errorCode == ErrorCode::NoError) {
        emit removeAllContainersFinished(tr("All containers from server '%1' have been removed").arg(serverName));
        return;
    }
    emit installationErrorOccurred(errorCode);
}

void InstallController::cleanupServer(bool removeDocker)
{
    int serverIndex = m_serversModel->getProcessedServerIndex();
    QString serverName = m_serversModel->data(serverIndex, ServersModel::Roles::NameRole).toString();

    QSharedPointer<ServerController> serverController(new ServerController(m_settings));

    // First remove containers via model (clears local config too)
    ErrorCode errorCode = m_serversModel->removeAllContainers(serverController);
    if (errorCode != ErrorCode::NoError) {
        emit installationErrorOccurred(errorCode);
        return;
    }

    // Then cleanup host-level changes
    auto credentials = m_settings->serverCredentials(serverIndex);
    errorCode = serverController->cleanupServer(credentials, removeDocker);
    if (errorCode == ErrorCode::NoError) {
        emit cleanupServerFinished(tr("Server '%1' has been fully cleaned up").arg(serverName));
        return;
    }
    emit installationErrorOccurred(errorCode);
}

void InstallController::removeProcessedContainer()
{
    int serverIndex = m_serversModel->getProcessedServerIndex();
    QString serverName = m_serversModel->data(serverIndex, ServersModel::Roles::NameRole).toString();

    int container = m_containersModel->getProcessedContainerIndex();
    QString containerName = m_containersModel->getProcessedContainerName();

    // Guard against an uninitialised/None/out-of-range processed container.
    // Otherwise a garbage index is cast to a DockerContainer, the server-side
    // "docker rm <wrong-name>" fails, the script still reported success, and the
    // client dropped the container from its config while the real container kept
    // running on the server (re-add then reused the stale container/port). by vovankrot
    if (container <= static_cast<int>(DockerContainer::None)
        || container >= ContainerProps::allContainers().size()) {
        emit installationErrorOccurred(ErrorCode::InternalError);
        return;
    }

    QSharedPointer<ServerController> serverController(new ServerController(m_settings));
    ErrorCode errorCode = m_serversModel->removeContainer(serverController, container);
    if (errorCode == ErrorCode::NoError) {

        emit removeProcessedContainerFinished(tr("%1 has been removed from the server '%2'").arg(containerName, serverName));
        return;
    }
    emit installationErrorOccurred(errorCode);
}

void InstallController::removeApiConfig(const int serverIndex)
{
    m_serversModel->removeApiConfig(serverIndex);
    emit apiConfigRemoved(tr("Api config removed"));
}

void InstallController::clearCachedProfile(QSharedPointer<ServerController> serverController)
{
    if (serverController.isNull()) {
        serverController.reset(new ServerController(m_settings));
    }

    int serverIndex = m_serversModel->getProcessedServerIndex();
    DockerContainer container = static_cast<DockerContainer>(m_containersModel->getProcessedContainerIndex());
    if (ContainerProps::containerService(container) == ServiceType::Other) {
        return;
    }

    QJsonObject containerConfig = m_containersModel->getContainerConfig(container);
    ServerCredentials serverCredentials =
            qvariant_cast<ServerCredentials>(m_serversModel->data(serverIndex, ServersModel::Roles::CredentialsRole));

    m_serversModel->clearCachedProfile(container);
    m_clientManagementModel->revokeClient(containerConfig, container, serverCredentials, serverIndex, serverController);

    emit cachedProfileCleared(tr("%1 cached profile cleared").arg(ContainerProps::containerHumanNames().value(container)));
    QJsonObject updatedConfig = m_settings->containerConfig(serverIndex, container);
    emit profileCleared(updatedConfig);
}

QRegularExpression InstallController::ipAddressPortRegExp()
{
    return NetworkUtilities::ipAddressPortRegExp();
}

QRegularExpression InstallController::ipAddressRegExp()
{
    return NetworkUtilities::ipAddressRegExp();
}

void InstallController::setProcessedServerCredentials(const QString &hostName, const QString &userName, const QString &secretData)
{
    m_processedServerCredentials.hostName = hostName;
    if (m_processedServerCredentials.hostName.contains(":")) {
        m_processedServerCredentials.port = m_processedServerCredentials.hostName.split(":").at(1).toInt();
        m_processedServerCredentials.hostName = m_processedServerCredentials.hostName.split(":").at(0);
    }
    m_processedServerCredentials.userName = userName;
    m_processedServerCredentials.secretData = secretData;
}

void InstallController::setShouldCreateServer(bool shouldCreateServer)
{
    m_shouldCreateServer = shouldCreateServer;
}

void InstallController::mountSftpDrive(const QString &port, const QString &password, const QString &username)
{
    QString mountPath;
    QString cmd;

    int serverIndex = m_serversModel->getProcessedServerIndex();
    ServerCredentials serverCredentials =
            qvariant_cast<ServerCredentials>(m_serversModel->data(serverIndex, ServersModel::Roles::CredentialsRole));
    QString hostname = serverCredentials.hostName;

#ifdef Q_OS_WINDOWS
    mountPath = Utils::getNextDriverLetter() + ":";
    //    QString cmd = QString("net use \\\\sshfs\\%1@x.x.x.x!%2 /USER:%1 %3")
    //            .arg(labelTftpUserNameText())
    //            .arg(labelTftpPortText())
    //            .arg(labelTftpPasswordText());

    cmd = "C:\\Program Files\\SSHFS-Win\\bin\\sshfs.exe";
#elif defined AMNEZIA_DESKTOP
    mountPath = QString("%1/sftp:%2:%3").arg(QStandardPaths::writableLocation(QStandardPaths::HomeLocation), hostname, port);
    QDir dir(mountPath);
    if (!dir.exists()) {
        dir.mkpath(mountPath);
    }

    cmd = "/usr/local/bin/sshfs";
#endif

#ifdef AMNEZIA_DESKTOP
    QSharedPointer<QProcess> process;
    process.reset(new QProcess());
    m_sftpMountProcesses.append(process);
    process->setProcessChannelMode(QProcess::MergedChannels);

    connect(process.get(), &QProcess::readyRead, this, [this, process, mountPath]() {
        QString s = process->readAll();
        if (s.contains("The service sshfs has been started")) {
            QDesktopServices::openUrl(QUrl("file:///" + mountPath));
        }
        qDebug() << s;
    });

    process->setProgram(cmd);

    QString args = QString("%1@%2:/ %3 "
                           "-o port=%4 "
                           "-f "
                           "-o reconnect "
                           "-o rellinks "
                           "-o fstypename=SSHFS "
                           "-o ssh_command=/usr/bin/ssh.exe "
                           "-o UserKnownHostsFile=/dev/null "
                           "-o StrictHostKeyChecking=no "
                           "-o password_stdin")
                           .arg(username, hostname, mountPath, port);

    //    args.replace("\n", " ");
    //    args.replace("\r", " ");
    // #ifndef Q_OS_WIN
    //    args.replace("reconnect-orellinks", "");
    // #endif
    process->setArguments(args.split(" ", Qt::SkipEmptyParts));
    process->start();
    process->waitForStarted(50);
    if (process->state() != QProcess::Running) {
        qDebug() << "onPushButtonSftpMountDriveClicked process not started";
        qDebug() << args;
    } else {
        process->write((password + "\n").toUtf8());
    }

#endif
}

bool InstallController::checkSshConnection(QSharedPointer<ServerController> serverController)
{
    if (serverController.isNull()) {
        serverController.reset(new ServerController(m_settings));
    }

    ErrorCode errorCode = ErrorCode::NoError;
    m_privateKeyPassphrase = "";

    if (m_processedServerCredentials.secretData.contains("BEGIN") && m_processedServerCredentials.secretData.contains("PRIVATE KEY")) {
        auto passphraseCallback = [this]() {
            emit passphraseRequestStarted();
            QEventLoop loop;
            QObject::connect(this, &InstallController::passphraseRequestFinished, &loop, &QEventLoop::quit);
            loop.exec();

            return m_privateKeyPassphrase;
        };

        QString decryptedPrivateKey;
        errorCode = serverController->getDecryptedPrivateKey(m_processedServerCredentials, decryptedPrivateKey, passphraseCallback);
        if (errorCode == ErrorCode::NoError) {
            m_processedServerCredentials.secretData = decryptedPrivateKey;
        } else {
            emit installationErrorOccurred(errorCode);
            return false;
        }
    }

    QString output;
    output = serverController->checkSshConnection(m_processedServerCredentials, errorCode);

    if (errorCode != ErrorCode::NoError) {
        emit installationErrorOccurred(errorCode);
        return false;
    } else {
        if (output.contains(tr("Please login as the user"))) {
            output.replace("\n", "");
            emit wrongInstallationUser(output);
            return false;
        }
    }
    return true;
}

void InstallController::setEncryptedPassphrase(QString passphrase)
{
    m_privateKeyPassphrase = passphrase;
    emit passphraseRequestFinished();
}

void InstallController::addEmptyServer()
{
    QJsonObject server;
    server.insert(config_key::hostName, m_processedServerCredentials.hostName);
    server.insert(config_key::userName, m_processedServerCredentials.userName);
    server.insert(config_key::password, m_processedServerCredentials.secretData);
    server.insert(config_key::port, m_processedServerCredentials.port);
    server.insert(config_key::description, m_settings->nextAvailableServerName());

    server.insert(config_key::defaultContainer, ContainerProps::containerToString(DockerContainer::None));

    m_serversModel->addServer(server);

    emit installServerFinished(tr("Server added successfully"));
}

void InstallController::validateConfig()
{
    int serverIndex = m_serversModel->getDefaultServerIndex();
    QJsonObject serverConfigObject = m_serversModel->getServerConfig(serverIndex);

    if (apiUtils::isServerFromApi(serverConfigObject)) {
        emit configValidated(true);
        return;
    }

    if (!m_serversModel->data(serverIndex, ServersModel::Roles::HasInstalledContainers).toBool()) {
        emit noInstalledContainers();
        emit configValidated(false);
        return;
    }

    DockerContainer container = qvariant_cast<DockerContainer>(m_serversModel->data(serverIndex, ServersModel::Roles::DefaultContainerRole));

    if (container == DockerContainer::None) {
        emit installationErrorOccurred(ErrorCode::NoInstalledContainersError);
        emit configValidated(false);
        return;
    }

    QJsonObject containerConfig = m_containersModel->getContainerConfig(container);
    ServerCredentials credentials = m_serversModel->getServerCredentials(serverIndex);
    QSharedPointer<ServerController> serverController(new ServerController(m_settings));

    auto isProtocolConfigExists = [](const QJsonObject &containerConfig, const DockerContainer container) {
        for (Proto protocol : ContainerProps::protocolsForContainer(container)) {
            QString protocolConfig =
                    containerConfig.value(ProtocolProps::protoToString(protocol)).toObject().value(config_key::last_config).toString();

            if (protocolConfig.isEmpty()) {
                return false;
            }
        }
        return true;
    };

    if (isProtocolConfigExists(containerConfig, container)) {
        emit configValidated(true);
        return;
    }

    struct ValidationResult {
        ErrorCode errorCode = ErrorCode::NoError;
        QJsonObject containerConfig;
    };

    QFuture<ValidationResult> future =
            QtConcurrent::run([settings = m_settings, serverController, credentials, containerConfig, container]() mutable {
                ValidationResult result;
                result.containerConfig = containerConfig;

                VpnConfigurationsController vpnConfigurationController(settings, serverController);
                result.errorCode = vpnConfigurationController.createProtocolConfigForContainer(credentials, container,
                                                                                               result.containerConfig);
                return result;
            });

    auto *watcher = new QFutureWatcher<ValidationResult>(this);
    connect(watcher, &QFutureWatcher<ValidationResult>::finished, this,
            [this, watcher, container, credentials, serverController]() {
                auto result = watcher->result();
                watcher->deleteLater();

                if (result.errorCode != ErrorCode::NoError) {
                    emit installationErrorOccurred(result.errorCode);
                    emit configValidated(false);
                    return;
                }

                m_serversModel->updateContainerConfig(container, result.containerConfig);

                ErrorCode appendError = m_clientManagementModel->appendClient(
                        container, credentials, result.containerConfig,
                        QString("Admin [%1]").arg(QSysInfo::prettyProductName()), serverController);

                if (appendError != ErrorCode::NoError) {
                    emit installationErrorOccurred(appendError);
                    emit configValidated(false);
                    return;
                }

                emit configValidated(true);
            });
    watcher->setFuture(future);
}

bool InstallController::isUpdateDockerContainerRequired(const DockerContainer container, const QJsonObject &oldConfig,
                                                        const QJsonObject &newConfig)
{
    Proto mainProto = ContainerProps::defaultProtocol(container);

    const QJsonObject &oldProtoConfig = oldConfig.value(ProtocolProps::protoToString(mainProto)).toObject();
    const QJsonObject &newProtoConfig = newConfig.value(ProtocolProps::protoToString(mainProto)).toObject();

    if (ContainerProps::isAwgContainer(container)) {
        const AwgConfig oldConfig(oldProtoConfig);
        const AwgConfig newConfig(newProtoConfig);

        if (oldConfig.hasEqualServerSettings(newConfig)) {
            return false;
        }
    } else if (container == DockerContainer::WireGuard) {
        const WgConfig oldConfig(oldProtoConfig);
        const WgConfig newConfig(newProtoConfig);

        if (oldConfig.hasEqualServerSettings(newConfig)) {
            return false;
        }
    }

    return true;
}

void InstallController::checkServerConfigUpdate(int serverIndex)
{
    // XHTTP "schema 2" upgrade detection removed: this fork runs XRay over plain
    // mKCP (no XHTTP path/schema), so there is nothing to upgrade to and no SSH probe
    // is needed. The server is always reported up to date — no "outdated config"
    // banner. by vovankrot
    QMetaObject::invokeMethod(this, [this, serverIndex]() {
        emit serverConfigUpToDate(serverIndex);
    }, Qt::QueuedConnection);
}

void InstallController::hotReconfigureContainer(int serverIndex)
{
    DockerContainer container = DockerContainer::Xray;

    ServerCredentials credentials = m_serversModel->getServerCredentials(serverIndex);
    QSharedPointer<ServerController> serverController(new ServerController(m_settings));
    connect(serverController.get(), &ServerController::logLineReady, this, &InstallController::installLogMessage);

    QJsonObject containerConfig = m_containersModel->getContainerConfig(container);
    QString portHint = containerConfig.value(config_key::port).toString().trimmed();
    QString siteHint = containerConfig.value(config_key::site).toString().trimmed();

    // Local values are hints only. If they are missing/stale, the script must reuse
    // the current server.json so hot reconfigure stays portable across imported servers.
    if (!portHint.isEmpty()) {
        bool portValid = false;
        const int portNum = portHint.toInt(&portValid);
        if (!portValid || portNum < 1 || portNum > 65535) {
            emit installLogMessage(tr("Local XRay port hint is invalid (%1). Falling back to the current server config.").arg(portHint));
            portHint.clear();
        } else {
            portHint = QString::number(portNum);
        }
    }

    // Validate site hint: must be a valid domain name (letters, digits, hyphens, dots only)
    static const QRegularExpression validDomain(
        QStringLiteral("^[a-zA-Z0-9]([a-zA-Z0-9\\-]{0,61}[a-zA-Z0-9])?(\\.[a-zA-Z0-9]([a-zA-Z0-9\\-]{0,61}[a-zA-Z0-9])?)*$"));
    if (!siteHint.isEmpty() && (!validDomain.match(siteHint).hasMatch() || siteHint.length() > 253)) {
        emit installLogMessage(tr("Local XRay site hint is invalid (%1). Falling back to the current server config.").arg(siteHint));
        siteHint.clear();
    }

    emit installLogMessage(tr("Starting hot reconfigure..."));

    [[maybe_unused]] auto hotReconfigureFuture = QtConcurrent::run([this, serverIndex, credentials, serverController, container, portHint, siteHint, containerConfig]() {
        // Verify container is running before trying to exec into it
        {
            QString checkStdOut;
            auto cbCheck = [&checkStdOut](const QString &data, libssh::Client &) {
                checkStdOut += data;
                return ErrorCode::NoError;
            };
            QString checkCmd = QStringLiteral("sudo docker inspect -f '{{.State.Running}}' %1 2>/dev/null || echo 'false'")
                               .arg(ContainerProps::containerToString(container));
            serverController->runScript(credentials, checkCmd, cbCheck);
            if (!checkStdOut.trimmed().contains("true")) {
                QMetaObject::invokeMethod(this, [this]() {
                    QString detail = tr("Container is not running. Please reinstall the protocol first.");
                    emit installLogMessage(tr("Hot reconfigure failed: %1").arg(detail));
                    emit hotReconfigureFinished(detail, false);
                }, Qt::QueuedConnection);
                return;
            }
        }

        // Save snapshot before reconfigure (containerConfig already read on main thread)
        QMetaObject::invokeMethod(this, [this]() {
            emit installLogMessage(tr("Saving configuration snapshot..."));
        }, Qt::QueuedConnection);

        bool snapshotOk = m_snapshotManager->saveSnapshot(credentials, container, containerConfig, serverController);

        QMetaObject::invokeMethod(this, [this, snapshotOk]() {
            if (snapshotOk) {
                emit installLogMessage(tr("Snapshot saved successfully"));
            } else {
                emit installLogMessage(tr("Warning: snapshot save failed, continuing anyway"));
            }
        }, Qt::QueuedConnection);

        QMetaObject::invokeMethod(this, [this]() {
            emit installLogMessage(tr("Running hot_reconfigure_xray.sh script..."));
        }, Qt::QueuedConnection);

        QString stdOut;
        auto cbRead = [this, &stdOut](const QString &data, libssh::Client &) {
            stdOut += data + "\n";
            // Log script output line by line
            QMetaObject::invokeMethod(this, [this, data]() {
                emit installLogMessage(data);
            }, Qt::QueuedConnection);
            return ErrorCode::NoError;
        };

        QString script = amnezia::scriptData(SharedScriptType::hot_reconfigure_xray);
        script.replace("$XRAY_SERVER_PORT", portHint);
        script.replace("$XRAY_SITE_NAME", siteHint);

        // This script expects Xray files and binary inside the container, not on the host.
        ErrorCode e = serverController->runContainerScript(credentials, container, script, cbRead, cbRead);

        ErrorCode verifyError = ErrorCode::InternalError;
        const QByteArray updatedServerConfig = serverController->getTextFileFromContainer(
            container,
            credentials,
            amnezia::protocols::xray::serverConfigPath,
            verifyError);
        const bool actualSchema2 = (verifyError == ErrorCode::NoError && hasActualXhttpSchema2Config(updatedServerConfig));
        const bool recoveredAfterExecDrop = (e == ErrorCode::ServerCommandFailedError && actualSchema2);

        if ((e == ErrorCode::NoError && stdOut.contains("HOT_RECONFIGURE_OK") && actualSchema2)
            || recoveredAfterExecDrop) {
            if (recoveredAfterExecDrop) {
                QMetaObject::invokeMethod(this, [this]() {
                    emit installLogMessage(tr("Hot reconfigure applied successfully after XRay restarted and closed the container exec session"));
                }, Qt::QueuedConnection);
            }

            ErrorCode versionWriteError = serverController->writeServerVersion(credentials, container);

            if (versionWriteError != ErrorCode::NoError) {
                QMetaObject::invokeMethod(this, [this, versionWriteError]() {
                    emit installLogMessage(tr("Warning: server version metadata update failed: %1")
                                           .arg(errorString(versionWriteError)));
                }, Qt::QueuedConnection);
            }

            QMetaObject::invokeMethod(this, [this, serverIndex]() {
                emit installLogMessage(tr("Hot reconfigure completed successfully"));
                emit hotReconfigureFinished(tr("Server configuration updated successfully"), true);
                emit serverConfigUpToDate(serverIndex);
            }, Qt::QueuedConnection);
        } else {
            QString errorDetail = stdOut.trimmed();
            if (errorDetail.isEmpty()) {
                if (verifyError != ErrorCode::NoError) {
                    errorDetail = errorString(verifyError);
                } else if (e != ErrorCode::NoError) {
                    errorDetail = errorString(e);
                } else {
                    errorDetail = tr("Updated XRay config could not be verified on the server");
                }
            }
            QMetaObject::invokeMethod(this, [this, errorDetail]() {
                emit installLogMessage(tr("Hot reconfigure failed: %1").arg(errorDetail));
                emit hotReconfigureFinished(errorDetail, false);
            }, Qt::QueuedConnection);
        }
    });
}

QJsonArray InstallController::listSnapshots(int serverIndex)
{
    ServerCredentials credentials = m_serversModel->getServerCredentials(serverIndex);
    QString hash = ConfigSnapshotManager::serverHash(credentials);

    QJsonArray result;
    auto snapshots = m_snapshotManager->listSnapshots(hash);
    for (const auto &snap : snapshots) {
        QJsonObject obj;
        obj["id"] = snap.id;
        obj["timestamp"] = snap.timestamp;
        obj["containerType"] = snap.containerType;
        obj["clientVersion"] = snap.clientVersion;
        result.append(obj);
    }
    return result;
}

void InstallController::restoreSnapshot(int serverIndex, int containerIndex, const QString &snapshotId)
{
    DockerContainer container = qvariant_cast<DockerContainer>(
        m_containersModel->data(m_containersModel->index(containerIndex), ContainersModel::Roles::DockerContainerRole));

    ServerCredentials credentials = m_serversModel->getServerCredentials(serverIndex);
    QSharedPointer<ServerController> serverController(new ServerController(m_settings));

    [[maybe_unused]] auto snapshotRestoreFuture = QtConcurrent::run([this, credentials, container, serverController, snapshotId]() {
        QJsonObject restoredClientConfig;
        bool ok = m_snapshotManager->restoreSnapshot(credentials, container, snapshotId,
                                                       restoredClientConfig, serverController);

        QMetaObject::invokeMethod(this, [this, ok]() {
            if (ok) {
                emit snapshotRestoreFinished(tr("Configuration restored from snapshot"), true);
            } else {
                emit snapshotRestoreFinished(tr("Failed to restore snapshot"), false);
            }
        }, Qt::QueuedConnection);
    });
}

void InstallController::recoverRealityDns()
{
    // Get the default server
    int serverIndex = m_serversModel->getDefaultServerIndex();
    if (serverIndex < 0) {
        logger.error() << "No default server configured for Reality DNS recovery";
        return;
    }

    ServerCredentials credentials = m_serversModel->getServerCredentials(serverIndex);
    QSharedPointer<ServerController> serverController(new ServerController(m_settings));
    connect(serverController.get(), &ServerController::logLineReady, this, &InstallController::installLogMessage);

    emit installLogMessage(tr("Starting Reality DNS recovery..."));

    [[maybe_unused]] auto realityDnsFuture = QtConcurrent::run([this, credentials, serverController]() {
        QString result;

        auto callback = [&result](const QString &data, libssh::Client &) {
            result += data;
            return ErrorCode::NoError;
        };

        const QString dnsRecoveryScript = QStringLiteral(R"AMNEZIA(
set -eu

log() {
    printf '%s\n' "$*"
}

if ! command -v docker >/dev/null 2>&1; then
    log "Docker is not installed on the server"
    exit 1
fi

log "Configuring Docker DNS resolvers"
sudo mkdir -p /etc/docker
if [ -f /etc/docker/daemon.json ]; then
    sudo cp -f /etc/docker/daemon.json "/etc/docker/daemon.json.amnezia-backup-$(date +%Y%m%d%H%M%S)"
fi

if command -v python3 >/dev/null 2>&1; then
    sudo python3 - <<'PY'
import json
import os
import shutil
import tempfile
import time

path = "/etc/docker/daemon.json"
data = {}
if os.path.exists(path) and os.path.getsize(path) > 0:
    try:
        with open(path, "r", encoding="utf-8") as handle:
            loaded = json.load(handle)
        if isinstance(loaded, dict):
            data = loaded
    except Exception:
        shutil.copy2(path, path + ".invalid-" + str(int(time.time())))

data["dns"] = ["1.1.1.1", "9.9.9.9"]
directory = os.path.dirname(path)
fd, tmp_path = tempfile.mkstemp(prefix="daemon.", dir=directory)
with os.fdopen(fd, "w", encoding="utf-8") as handle:
    json.dump(data, handle, indent=2, sort_keys=True)
    handle.write("\n")
os.replace(tmp_path, path)
PY
else
    log "python3 is not available; replacing Docker daemon.json with DNS-only recovery config"
    printf '{"dns":["1.1.1.1","9.9.9.9"]}\n' | sudo tee /etc/docker/daemon.json >/dev/null
fi

log "Restarting Docker"
if command -v systemctl >/dev/null 2>&1; then
    sudo systemctl restart docker
else
    sudo service docker restart
fi

for attempt in 1 2 3 4 5 6 7 8 9 10; do
    if sudo docker info >/dev/null 2>&1; then
        break
    fi
    sleep 1
done
sudo docker info >/dev/null

log "Restarting Amnezia proxy containers"
restarted=0
for container in amnezia-xray amnezia-ssxray amnezia-hysteria2 amnezia-anytls; do
    if sudo docker ps -a --format '{{.Names}}' | grep -qx "$container"; then
        sudo docker restart "$container" >/dev/null
        log "Restarted $container"
        restarted=1
    fi
done

if [ "$restarted" -eq 0 ]; then
    log "No Amnezia proxy containers were found"
    exit 1
fi

checked=0
for container in amnezia-xray amnezia-ssxray; do
    if sudo docker ps --format '{{.Names}}' | grep -qx "$container"; then
        checked=1
        if sudo docker exec "$container" sh -c 'getent hosts www.microsoft.com >/dev/null 2>&1 || nslookup www.microsoft.com 1.1.1.1 >/dev/null 2>&1'; then
            log "DNS smoke-test passed in $container"
        else
            log "DNS smoke-test failed in $container"
            exit 1
        fi
    fi
done

if [ "$checked" -eq 0 ]; then
    log "No running XRay containers were available for DNS smoke-test"
fi

log "RECOVERY_OK"
)AMNEZIA");

        ErrorCode error = serverController->runHostScript(credentials, dnsRecoveryScript, callback, callback, 60000, 240000);
        bool success = (error == ErrorCode::NoError) && result.contains(QStringLiteral("RECOVERY_OK"));

        QMetaObject::invokeMethod(this, [this, success, error, result]() {
            if (success) {
                emit installLogMessage(tr("Reality DNS recovery completed successfully"));
                emit hotReconfigureFinished(tr("Reality DNS has been recovered on the server"), true);
            } else {
                QString detail = result.right(800).trimmed();
                if (error != ErrorCode::NoError) {
                    const QString errorDetail = errorString(error);
                    detail = detail.isEmpty() ? errorDetail : detail + QStringLiteral("\n") + errorDetail;
                }
                if (detail.isEmpty()) {
                    detail = tr("Failed to recover DNS");
                }
                emit installLogMessage(tr("Reality DNS recovery failed: %1").arg(detail));
                emit hotReconfigureFinished(detail, false);
            }
        }, Qt::QueuedConnection);
    });
}

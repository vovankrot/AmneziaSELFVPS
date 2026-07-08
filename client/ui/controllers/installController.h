#ifndef INSTALLCONTROLLER_H
#define INSTALLCONTROLLER_H

#include <QObject>
#include <QProcess>
#include <QSet>

#include "containers/containers_defs.h"
#include "core/defs.h"
#include "ui/models/clientManagementModel.h"
#include "ui/models/containers_model.h"
#include "ui/models/protocols_model.h"
#include "ui/models/servers_model.h"

class ConfigSnapshotManager;

class InstallController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool shouldUseAnyTlsVariant READ shouldUseAnyTlsVariant WRITE setShouldUseAnyTlsVariant NOTIFY shouldUseAnyTlsVariantChanged)
public:
    explicit InstallController(const QSharedPointer<ServersModel> &serversModel, const QSharedPointer<ContainersModel> &containersModel,
                               const QSharedPointer<ProtocolsModel> &protocolsModel,
                               const QSharedPointer<ClientManagementModel> &clientManagementModel,
                               const std::shared_ptr<Settings> &settings, QObject *parent = nullptr);
    ~InstallController();

    bool shouldUseAnyTlsVariant() const;

public slots:
    // useAnyTlsVariant: when true and `container == Xray`, the install is
    // transparently routed to the AnyTLS container. This is the UX-level
    // "AnyTLS is a XRay variant" toggle wired from PageSetupWizardProtocolSettings.
    void install(DockerContainer container, int port, TransportProto transportProto, bool useAnyTlsVariant = false);
    void setProcessedServerCredentials(const QString &hostName, const QString &userName, const QString &secretData);
    void setShouldCreateServer(bool shouldCreateServer);
    void setShouldUseAnyTlsVariant(bool shouldUseAnyTlsVariant);

    void scanServerForInstalledContainers();

    void updateContainer(QJsonObject config);

    void removeProcessedServer();
    void rebootProcessedServer();
    void removeAllContainers();
    void cleanupServer(bool removeDocker = false);
    void removeProcessedContainer();

    void removeApiConfig(const int serverIndex);

    void clearCachedProfile(QSharedPointer<ServerController> serverController = nullptr);

    QRegularExpression ipAddressPortRegExp();
    QRegularExpression ipAddressRegExp();

    void mountSftpDrive(const QString &port, const QString &password, const QString &username);

    bool checkSshConnection(QSharedPointer<ServerController> serverController = nullptr);

    void setEncryptedPassphrase(QString passphrase);

    void addEmptyServer();

    void validateConfig();

    Q_INVOKABLE void checkServerConfigUpdate(int serverIndex);
    Q_INVOKABLE void hotReconfigureContainer(int serverIndex);
    Q_INVOKABLE QJsonArray listSnapshots(int serverIndex);
    Q_INVOKABLE void restoreSnapshot(int serverIndex, int containerIndex, const QString &snapshotId);

    Q_INVOKABLE void checkServerContainers();
    Q_INVOKABLE bool isContainerInstalledOnServer(int container) const;
    Q_INVOKABLE void recoverRealityDns();

signals:
    void configValidated(bool isValid);
    void installContainerFinished(const QString &finishMessage, bool isServiceInstall, int container);
    void installServerFinished(const QString &finishMessage);

    void updateContainerFinished(const QString &message);

    void scanServerFinished(bool isInstalledContainerFound);

    void rebootProcessedServerFinished(const QString &finishedMessage);
    void removeProcessedServerFinished(const QString &finishedMessage);
    void removeAllContainersFinished(const QString &finishedMessage);
    void cleanupServerFinished(const QString &finishedMessage);
    void removeProcessedContainerFinished(const QString &finishedMessage);

    void installationErrorOccurred(ErrorCode errorCode);
    void wrongInstallationUser(const QString &message);

    void serverAlreadyExists(int serverIndex);

    void passphraseRequestStarted();
    void passphraseRequestFinished();

    void serverIsBusy(const bool isBusy);
    void cancelInstallation();

    void currentContainerUpdated();

    void cachedProfileCleared(const QString &message);
    void apiConfigRemoved(const QString &message);

    void noInstalledContainers();

    void profileCleared(const QJsonObject &config);

    void serverConfigUpdateAvailable(int serverIndex, int currentSchema, int requiredSchema);
    void serverConfigUpToDate(int serverIndex);
    void hotReconfigureFinished(const QString &message, bool success);

    void installLogMessage(const QString &line);
    void snapshotRestoreFinished(const QString &message, bool success);
    void serverContainersChecked();
    void shouldUseAnyTlsVariantChanged();

private:
    void installServer(const DockerContainer container, const QMap<DockerContainer, QJsonObject> &installedContainers,
                       const ServerCredentials &serverCredentials, const QSharedPointer<ServerController> &serverController,
                       QString &finishMessage);
    void installContainer(const DockerContainer container, const QMap<DockerContainer, QJsonObject> &installedContainers,
                          const ServerCredentials &serverCredentials, const QSharedPointer<ServerController> &serverController,
                          QString &finishMessage);
    bool isServerAlreadyExists();

    ErrorCode getAlreadyInstalledContainers(const ServerCredentials &credentials, const QSharedPointer<ServerController> &serverController,
                                            QMap<DockerContainer, QJsonObject> &installedContainers);
    bool isUpdateDockerContainerRequired(const DockerContainer container, const QJsonObject &oldConfig, const QJsonObject &newConfig);

    QSharedPointer<ServersModel> m_serversModel;
    QSharedPointer<ContainersModel> m_containersModel;
    QSharedPointer<ProtocolsModel> m_protocolModel;
    QSharedPointer<ClientManagementModel> m_clientManagementModel;

    std::shared_ptr<Settings> m_settings;
    std::unique_ptr<ConfigSnapshotManager> m_snapshotManager;

    ServerCredentials m_processedServerCredentials;

    bool m_shouldCreateServer;
    bool m_shouldUseAnyTlsVariant = false;

    QString m_privateKeyPassphrase;

    QSet<int> m_serverInstalledContainers;

#ifndef Q_OS_IOS
    QList<QSharedPointer<QProcess>> m_sftpMountProcesses;
#endif
};

#endif // INSTALLCONTROLLER_H

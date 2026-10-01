#ifndef EXPORTCONTROLLER_H
#define EXPORTCONTROLLER_H

#include <QObject>
#include <QElapsedTimer>

#include "ui/models/clientManagementModel.h"
#include "ui/models/containers_model.h"
#include "ui/models/servers_model.h"

class ExportController : public QObject
{
    Q_OBJECT
public:
    explicit ExportController(const QSharedPointer<ServersModel> &serversModel, const QSharedPointer<ContainersModel> &containersModel,
                              const QSharedPointer<ClientManagementModel> &clientManagementModel, const std::shared_ptr<Settings> &settings,
                              QObject *parent = nullptr);

    Q_PROPERTY(bool clientsLoading READ clientsLoading NOTIFY clientsLoadingChanged)
    bool clientsLoading() const { return m_clientsLoading; }

    Q_PROPERTY(QList<QString> qrCodes READ getQrCodes NOTIFY exportConfigChanged)
    Q_PROPERTY(int qrCodesCount READ getQrCodesCount NOTIFY exportConfigChanged)
    Q_PROPERTY(QString config READ getConfig NOTIFY exportConfigChanged)
    Q_PROPERTY(QString nativeConfigString READ getNativeConfigString NOTIFY exportConfigChanged)

public slots:
    void generateFullAccessConfig();
    void generateConnectionConfig(const QString &clientName);
    void generateOpenVpnConfig(const QString &clientName);
    void generateWireGuardConfig(const QString &clientName);
    void generateAwgConfig(const QString &clientName);
    void generateShadowSocksConfig();
    void generateCloakConfig();
    void generateXrayConfig(const QString &clientName);

    QString getConfig();
    QString getNativeConfigString();
    QList<QString> getQrCodes();

    void exportConfig(const QString &fileName);

    void updateClientManagementModel(const DockerContainer container, ServerCredentials credentials, bool forceRefresh = false);
    void revokeConfig(const int row, const DockerContainer container, ServerCredentials credentials);
    void renameClient(const int row, const QString &clientName, const DockerContainer container, ServerCredentials credentials);

signals:
    void clientsLoadingChanged();
    void generateConfig(int type);
    void revokeConfigCompleted();
    void exportErrorOccurred(const QString &errorMessage);
    void exportErrorOccurred(ErrorCode errorCode);

    void exportConfigChanged();

    void saveFile(const QString &fileName, const QString &data);

private:
    int getQrCodesCount();

    void clearPreviousConfig();

    ErrorCode generateNativeConfig(const DockerContainer container, const QString &clientName, const Proto &protocol,
                                   QJsonObject &jsonNativeConfig);

    QSharedPointer<ServersModel> m_serversModel;
    QSharedPointer<ContainersModel> m_containersModel;
    QSharedPointer<ClientManagementModel> m_clientManagementModel;
    std::shared_ptr<Settings> m_settings;

    bool m_clientsLoading = false;
    QByteArray m_clientsCacheKey;
    QByteArray m_clientsActiveKey;
    QByteArray m_clientsRequestedKey;
    DockerContainer m_pendingClientsContainer {};
    ServerCredentials m_pendingClientsCredentials;
    QElapsedTimer m_clientsCacheAge;

    QString m_config;
    QString m_nativeConfigString;
    QList<QString> m_qrCodes;
};

#endif // EXPORTCONTROLLER_H

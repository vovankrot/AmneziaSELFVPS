#ifndef SSXRAY_CONFIGURATOR_H
#define SSXRAY_CONFIGURATOR_H

#include "configurator_base.h"

class SSXrayConfigurator : public ConfiguratorBase
{
    Q_OBJECT
public:
    SSXrayConfigurator(std::shared_ptr<Settings> settings, const QSharedPointer<ServerController> &serverController,
                       QObject *parent = nullptr);

    QString createConfig(const ServerCredentials &credentials, DockerContainer container, const QJsonObject &containerConfig,
                         ErrorCode &errorCode) override;
};

#endif // SSXRAY_CONFIGURATOR_H

#ifndef HYSTERIA2_CONFIGURATOR_H
#define HYSTERIA2_CONFIGURATOR_H

#include "configurator_base.h"

// Generates a client-side Hysteria 2 YAML config from the server's
// hysteria2_password.key + container settings. Pattern mirrors SSXrayConfigurator:
// pull a secret off the server, substitute into the template shipped via qrc.
class Hysteria2Configurator : public ConfiguratorBase
{
    Q_OBJECT
public:
    Hysteria2Configurator(std::shared_ptr<Settings> settings, const QSharedPointer<ServerController> &serverController,
                          QObject *parent = nullptr);

    QString createConfig(const ServerCredentials &credentials, DockerContainer container, const QJsonObject &containerConfig,
                         ErrorCode &errorCode) override;
};

#endif // HYSTERIA2_CONFIGURATOR_H

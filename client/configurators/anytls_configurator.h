#ifndef ANYTLS_CONFIGURATOR_H
#define ANYTLS_CONFIGURATOR_H

#include "configurator_base.h"

// Generates a client-side AnyTLS JSON config from the server's
// anytls_password.key plus container settings. Same pattern as
// SSXrayConfigurator / Hysteria2Configurator.
class AnyTlsConfigurator : public ConfiguratorBase
{
    Q_OBJECT
public:
    AnyTlsConfigurator(std::shared_ptr<Settings> settings, const QSharedPointer<ServerController> &serverController,
                       QObject *parent = nullptr);

    QString createConfig(const ServerCredentials &credentials, DockerContainer container, const QJsonObject &containerConfig,
                         ErrorCode &errorCode) override;
};

#endif // ANYTLS_CONFIGURATOR_H

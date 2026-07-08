#include "anytls_configurator.h"

#include "containers/containers_defs.h"
#include "core/controllers/serverController.h"
#include "core/scripts_registry.h"
#include "logger.h"

namespace {
Logger logger("AnyTlsConfigurator");
}

AnyTlsConfigurator::AnyTlsConfigurator(std::shared_ptr<Settings> settings,
                                        const QSharedPointer<ServerController> &serverController, QObject *parent)
    : ConfiguratorBase(settings, serverController, parent)
{
}

QString AnyTlsConfigurator::createConfig(const ServerCredentials &credentials, DockerContainer container,
                                          const QJsonObject &containerConfig, ErrorCode &errorCode)
{
    QString password = m_serverController->getTextFileFromContainer(
        container, credentials, amnezia::protocols::anytls::passwordPath, errorCode);
    if (errorCode != ErrorCode::NoError || password.isEmpty()) {
        logger.error() << "Failed to read anytls_password.key from server";
        if (errorCode == ErrorCode::NoError) {
            errorCode = ErrorCode::InternalError;
        }
        return "";
    }
    password.replace("\n", "").replace("\r", "");
    password = password.trimmed();

    // genVarsForScript already injects $ANYTLS_SERVER_PORT,
    // $ANYTLS_LOCAL_PROXY_PORT, $ANYTLS_SNI and $SERVER_IP_ADDRESS.
    ServerController::Vars vars = m_serverController->genVarsForScript(credentials, container, containerConfig);
    vars.append({ QStringLiteral("$ANYTLS_PASSWORD"), password });

    QString config = m_serverController->replaceVars(
        amnezia::scriptData(ProtocolScriptType::anytls_template, container), vars);

    if (config.isEmpty()) {
        logger.error() << "AnyTLS template rendered to empty config";
        errorCode = ErrorCode::InternalError;
        return "";
    }

    logger.info() << "AnyTLS client config created";
    return config;
}

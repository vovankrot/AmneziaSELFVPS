#include "ssxray_configurator.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include "containers/containers_defs.h"
#include "core/controllers/serverController.h"
#include "core/scripts_registry.h"
#include "logger.h"

namespace {
Logger logger("SSXrayConfigurator");
}

SSXrayConfigurator::SSXrayConfigurator(std::shared_ptr<Settings> settings, const QSharedPointer<ServerController> &serverController,
                                       QObject *parent)
    : ConfiguratorBase(settings, serverController, parent)
{
}

QString SSXrayConfigurator::createConfig(const ServerCredentials &credentials, DockerContainer container,
                                         const QJsonObject &containerConfig, ErrorCode &errorCode)
{
    // Read the Shadowsocks password from server
    QString ssPassword = m_serverController->getTextFileFromContainer(
        container, credentials, amnezia::protocols::ssxray::passwordPath, errorCode);
    
    if (errorCode != ErrorCode::NoError || ssPassword.isEmpty()) {
        logger.error() << "Failed to get SSXray password from server";
        return "";
    }
    ssPassword.replace("\n", "");
    ssPassword.replace("\r", "");

    // Get the XRay client config template
    QString config = m_serverController->replaceVars(
        amnezia::scriptData(ProtocolScriptType::xray_template, container),
        m_serverController->genVarsForScript(credentials, container, containerConfig));
    
    if (config.isEmpty()) {
        logger.error() << "Failed to get SSXray config template";
        errorCode = ErrorCode::InternalError;
        return "";
    }

    // Replace password placeholder
    config.replace("$SSXRAY_PASSWORD", ssPassword);

    logger.info() << "SSXray client config created successfully";
    return config;
}

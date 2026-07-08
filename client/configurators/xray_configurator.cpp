#include "xray_configurator.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include "logger.h"

#include "containers/containers_defs.h"
#include "core/controllers/serverController.h"
#include "core/scripts_registry.h"

namespace {
Logger logger("XrayConfigurator");

struct XrayServerState {
    bool parsed = false;
    bool usesXhttp = false;
    bool usesKcp = false;
    QString xhttpPath;
    QString kcpSeed;
    QString siteName;
    QString serverPort;
};

QString trimFileValue(QString value)
{
    value.replace("\r", "");
    value.replace("\n", "");
    return value.trimmed();
}

QString extractSiteName(const QJsonObject &streamSettings)
{
    const QJsonObject realitySettings = streamSettings.value("realitySettings").toObject();

    const QString dest = realitySettings.value("dest").toString().trimmed();
    if (!dest.isEmpty()) {
        const int portSeparator = dest.lastIndexOf(':');
        if (portSeparator > 0) {
            return dest.left(portSeparator).trimmed();
        }
        return dest;
    }

    const QJsonArray serverNames = realitySettings.value("serverNames").toArray();
    for (const QJsonValue &serverNameValue : serverNames) {
        const QString serverName = serverNameValue.toString().trimmed();
        if (!serverName.isEmpty()) {
            return serverName;
        }
    }

    return {};
}

XrayServerState parseServerState(const QString &serverConfigRaw)
{
    XrayServerState state;

    const QJsonDocument serverDoc = QJsonDocument::fromJson(serverConfigRaw.toUtf8());
    if (!serverDoc.isObject()) {
        return state;
    }

    const QJsonArray inbounds = serverDoc.object().value("inbounds").toArray();
    if (inbounds.isEmpty()) {
        return state;
    }

    const QJsonObject inbound = inbounds.at(0).toObject();
    const QJsonObject streamSettings = inbound.value("streamSettings").toObject();
    state.parsed = true;
    state.usesXhttp = (streamSettings.value("network").toString() == QStringLiteral("xhttp"));
    state.serverPort = QString::number(inbound.value("port").toInt(0));
    state.siteName = extractSiteName(streamSettings);

    if (state.usesXhttp) {
        state.xhttpPath = streamSettings.value("xhttpSettings").toObject().value("path").toString().trimmed();
    }

    state.usesKcp = (streamSettings.value("network").toString() == QStringLiteral("kcp"));
    if (state.usesKcp) {
        state.kcpSeed = streamSettings.value("kcpSettings").toObject().value("seed").toString().trimmed();
    }

    return state;
}

void replaceOrAppendVar(ServerController::Vars &vars, const QString &key, const QString &value)
{
    for (auto &var : vars) {
        if (var.first == key) {
            var.second = value;
            return;
        }
    }

    vars.append({ key, value });
}
}

XrayConfigurator::XrayConfigurator(std::shared_ptr<Settings> settings, const QSharedPointer<ServerController> &serverController, QObject *parent)
    : ConfiguratorBase(settings, serverController, parent)
{
}

QString XrayConfigurator::prepareServerConfig(const ServerCredentials &credentials, DockerContainer container,
                                               const QJsonObject &containerConfig, ErrorCode &errorCode)
{
    // Get current server config
    QString currentConfig = m_serverController->getTextFileFromContainer(
        container, credentials, amnezia::protocols::xray::serverConfigPath, errorCode);
    
    if (errorCode != ErrorCode::NoError) {
        logger.error() << "Failed to get server config file";
        return "";
    }

    // Parse current config as JSON
    QJsonDocument doc = QJsonDocument::fromJson(currentConfig.toUtf8());
    if (doc.isNull() || !doc.isObject()) {
        logger.error() << "Failed to parse server config JSON";
        errorCode = ErrorCode::InternalError;
        return "";
    }

    QJsonObject serverConfig = doc.object();
    QJsonObject logConfig = serverConfig.value("log").toObject();
    if (logConfig.value("access").toString() != QString::fromLatin1(amnezia::protocols::xray::accessLogPath)) {
        logConfig["access"] = QString::fromLatin1(amnezia::protocols::xray::accessLogPath);
    }
    if (logConfig.value("loglevel").toString().isEmpty() || logConfig.value("loglevel").toString() == QStringLiteral("error")) {
        logConfig["loglevel"] = QStringLiteral("warning");
    }
    serverConfig["log"] = logConfig;
    
    // Validate server config structure
    if (!serverConfig.contains("inbounds")) {
        logger.error() << "Server config missing 'inbounds' field";
        errorCode = ErrorCode::InternalError;
        return "";
    }

    QJsonArray inbounds = serverConfig["inbounds"].toArray();
    if (inbounds.isEmpty()) {
        logger.error() << "Server config has empty 'inbounds' array";
        errorCode = ErrorCode::InternalError;
        return "";
    }
    
    QJsonObject inbound = inbounds[0].toObject();
    if (!inbound.contains("settings")) {
        logger.error() << "Inbound missing 'settings' field";
        errorCode = ErrorCode::InternalError;
        return "";
    }

    QJsonObject settings = inbound["settings"].toObject();
    if (!settings.contains("clients")) {
        logger.error() << "Settings missing 'clients' field";
        errorCode = ErrorCode::InternalError;
        return "";
    }

    QJsonArray clients = settings["clients"].toArray();
    if (clients.isEmpty()) {
        logger.error() << "Server config has no clients";
        errorCode = ErrorCode::InternalError;
        return "";
    }

    // Share ONE persisted client UUID across all devices instead of each device minting
    // and appending its own. configure_container.sh stores the UUID in the host volume
    // /opt/amnezia-xray-keys, so a container reinstall reuses it -- reinstalls no longer
    // wipe other devices' clients and break them. Adopt the server's existing (default)
    // client id and backfill its email if missing. The default client's flow is already
    // set correctly server-side (empty for mKCP/XHTTP, xtls-rprx-vision for TCP+Reality),
    // so we don't touch it here. by vovankrot
    QString clientId;
    for (int i = 0; i < clients.size(); ++i) {
        QJsonObject existingClient = clients.at(i).toObject();
        const QString existingClientId = existingClient.value("id").toString();
        if (existingClientId.isEmpty()) {
            continue;
        }
        if (existingClient.value("email").toString().isEmpty()) {
            existingClient["email"] = existingClientId;
            clients[i] = existingClient;
        }
        if (clientId.isEmpty()) {
            clientId = existingClientId;
        }
    }
    if (clientId.isEmpty()) {
        logger.error() << "Server config has no usable client id";
        errorCode = ErrorCode::InternalError;
        return "";
    }

    // Update config
    settings["clients"] = clients;
    inbound["settings"] = settings;
    inbounds[0] = inbound;
    serverConfig["inbounds"] = inbounds;
    
    // Save updated config to server
    QString updatedConfig = QJsonDocument(serverConfig).toJson();
    errorCode = m_serverController->uploadTextFileToContainer(
        container, 
        credentials, 
        updatedConfig,
        amnezia::protocols::xray::serverConfigPath,
        libssh::ScpOverwriteMode::ScpOverwriteExisting
    );
    if (errorCode != ErrorCode::NoError) {
        logger.error() << "Failed to upload updated config";
        return "";
    }

    // Restart container
    QString restartScript = QString("sudo docker restart $CONTAINER_NAME");
    errorCode = m_serverController->runScript(
        credentials, 
        m_serverController->replaceVars(restartScript, m_serverController->genVarsForScript(credentials, container))
    );

    if (errorCode != ErrorCode::NoError) {
        logger.error() << "Failed to restart container";
        return "";
    }

    return clientId;
}

QString XrayConfigurator::createConfig(const ServerCredentials &credentials, DockerContainer container,
                                       const QJsonObject &containerConfig, ErrorCode &errorCode)
{
    // Get client ID from prepareServerConfig
    QString xrayClientId = prepareServerConfig(credentials, container, containerConfig, errorCode);
    if (errorCode != ErrorCode::NoError || xrayClientId.isEmpty()) {
        logger.error() << "Failed to prepare server config";
        if (errorCode == ErrorCode::NoError) {
            errorCode = ErrorCode::InternalError;
        }
        return "";
    }

    QString serverConfigRaw = m_serverController->getTextFileFromContainer(
        container, credentials, amnezia::protocols::xray::serverConfigPath, errorCode);
    const XrayServerState serverState = (errorCode == ErrorCode::NoError && !serverConfigRaw.isEmpty())
        ? parseServerState(serverConfigRaw)
        : XrayServerState();
    errorCode = ErrorCode::NoError;

    QString resolvedServerPort = serverState.serverPort;
    if (resolvedServerPort.isEmpty() || resolvedServerPort == QStringLiteral("0")) {
        resolvedServerPort = containerConfig.value(config_key::port).toString(protocols::xray::defaultPort);
    }

    QString resolvedSiteName = serverState.siteName;
    if (resolvedSiteName.isEmpty()) {
        resolvedSiteName = containerConfig.value(config_key::site).toString(protocols::xray::defaultSite);
    }

    QString xrayXhttpPath;
    bool serverUsesXhttp = serverState.parsed ? serverState.usesXhttp : false;
    if (serverUsesXhttp) {
        xrayXhttpPath = serverState.xhttpPath;
    }

    // mKCP (UDP) transport: no reality/vision. xray 26.x wraps it in FinalMask
    // "salamander" obfuscation (same masking as Hysteria). The client mirrors the
    // server's shared salamander password, read from xray_salamander.key below.
    // by vovankrot
    bool serverUsesKcp = serverState.parsed ? serverState.usesKcp : false;
    QString xraySalamanderPassword;

    // xhttp is dead on this fork — do NOT probe xray_xhttp_path.key. The probe
    // only spammed __AMNEZIA_FILE_READ_ERROR__ for a key that never exists on a
    // kcp/tcp server, and served no purpose. by vovankrot

    if (serverUsesXhttp && xrayXhttpPath.isEmpty()) {
        xrayXhttpPath = QStringLiteral("/api/v1/data");
    }

    ServerController::Vars vars = m_serverController->genVarsForScript(credentials, container, containerConfig);
    replaceOrAppendVar(vars, QStringLiteral("$XRAY_SERVER_PORT"), resolvedServerPort);
    replaceOrAppendVar(vars, QStringLiteral("$XRAY_SITE_NAME"), resolvedSiteName);

    QString config = m_serverController->replaceVars(
        amnezia::scriptData(ProtocolScriptType::xray_template, container),
        vars);
    
    if (config.isEmpty()) {
        logger.error() << "Failed to get config template";
        errorCode = ErrorCode::InternalError;
        return "";
    }

    QString xrayPublicKey;
    QString xrayShortId;
    if (!serverUsesKcp) {
        xrayPublicKey =
            m_serverController->getTextFileFromContainer(container, credentials, amnezia::protocols::xray::PublicKeyPath, errorCode);
        if (errorCode != ErrorCode::NoError || xrayPublicKey.isEmpty()) {
            logger.error() << "Failed to get public key";
            if (errorCode == ErrorCode::NoError) {
                errorCode = ErrorCode::InternalError;
            }
            return "";
        }
        xrayPublicKey.replace("\n", "");
    }
    
    if (!serverUsesKcp) {
        xrayShortId =
            m_serverController->getTextFileFromContainer(container, credentials, amnezia::protocols::xray::shortidPath, errorCode);
        if (errorCode != ErrorCode::NoError || xrayShortId.isEmpty()) {
            logger.error() << "Failed to get short ID";
            if (errorCode == ErrorCode::NoError) {
                errorCode = ErrorCode::InternalError;
            }
            return "";
        }
        xrayShortId.replace("\n", "");
    }

    if (serverUsesKcp) {
        xraySalamanderPassword =
            m_serverController->getTextFileFromContainer(container, credentials, amnezia::protocols::xray::salamanderKeyPath, errorCode);
        if (errorCode != ErrorCode::NoError || xraySalamanderPassword.isEmpty()) {
            logger.error() << "Failed to get xray salamander password";
            if (errorCode == ErrorCode::NoError) {
                errorCode = ErrorCode::InternalError;
            }
            return "";
        }
        xraySalamanderPassword.replace("\n", "");
    }

    // Validate all required variables are present (transport-dependent)
    const bool missingCommon = !config.contains("$XRAY_CLIENT_ID");
    const bool missingReality = !serverUsesKcp
            && (!config.contains("$XRAY_PUBLIC_KEY") || !config.contains("$XRAY_SHORT_ID"));
    // mKCP + FinalMask salamander: the client needs the shared salamander password.
    const bool missingKcp = serverUsesKcp && !config.contains("$XRAY_SALAMANDER_PASSWORD");
    if (missingCommon || missingReality || missingKcp) {
        logger.error() << "Config template missing required variables:"
                      << "kcp:" << serverUsesKcp
                      << "XRAY_CLIENT_ID:" << !config.contains("$XRAY_CLIENT_ID")
                      << "XRAY_PUBLIC_KEY:" << !config.contains("$XRAY_PUBLIC_KEY")
                      << "XRAY_SHORT_ID:" << !config.contains("$XRAY_SHORT_ID")
                      << "XRAY_SALAMANDER_PASSWORD:" << !config.contains("$XRAY_SALAMANDER_PASSWORD");
        errorCode = ErrorCode::InternalError;
        return "";
    }

    config.replace("$XRAY_CLIENT_ID", xrayClientId);
    config.replace("$XRAY_XHTTP_PATH", xrayXhttpPath);
    if (serverUsesKcp) {
        config.replace("$XRAY_SALAMANDER_PASSWORD", xraySalamanderPassword);
    } else {
        config.replace("$XRAY_PUBLIC_KEY", xrayPublicKey);
        config.replace("$XRAY_SHORT_ID", xrayShortId);
    }

    // If server uses TCP (stock Amnezia), rewrite client streamSettings to match.
    // mKCP servers keep the kcp template as-is (no tcp/vision rewrite).
    if (!serverUsesXhttp && !serverUsesKcp) {
        QJsonDocument configDoc = QJsonDocument::fromJson(config.toUtf8());
        if (configDoc.isObject()) {
            QJsonObject configObj = configDoc.object();
            QJsonArray outbounds = configObj["outbounds"].toArray();
            if (!outbounds.isEmpty()) {
                QJsonObject proxyOutbound = outbounds[0].toObject();
                QJsonObject streamSettings = proxyOutbound["streamSettings"].toObject();

                // Switch to TCP transport with xtls-rprx-vision flow
                streamSettings["network"] = QString("tcp");
                streamSettings.remove("xhttpSettings");

                proxyOutbound["streamSettings"] = streamSettings;

                // Set flow on user
                QJsonObject settings = proxyOutbound["settings"].toObject();
                QJsonArray vnext = settings["vnext"].toArray();
                if (!vnext.isEmpty()) {
                    QJsonObject vnextEntry = vnext[0].toObject();
                    QJsonArray users = vnextEntry["users"].toArray();
                    if (!users.isEmpty()) {
                        QJsonObject user = users[0].toObject();
                        user["flow"] = QString("xtls-rprx-vision");
                        users[0] = user;
                    }
                    vnextEntry["users"] = users;
                    vnext[0] = vnextEntry;
                }
                settings["vnext"] = vnext;
                proxyOutbound["settings"] = settings;

                outbounds[0] = proxyOutbound;
            }
            configObj["outbounds"] = outbounds;
            config = QString::fromUtf8(QJsonDocument(configObj).toJson());

            logger.info() << "Using TCP fallback for stock Amnezia server";
        }
    }

    return config;
}

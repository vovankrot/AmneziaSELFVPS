#include "vpnConfigurationController.h"

#include "configurators/awg_configurator.h"
#include "configurators/cloak_configurator.h"
#include "configurators/openvpn_configurator.h"
#include "configurators/shadowsocks_configurator.h"
#include "configurators/ssxray_configurator.h"
#include "configurators/wireguard_configurator.h"
#include "configurators/xray_configurator.h"
#include "configurators/hysteria2_configurator.h"
#include "configurators/anytls_configurator.h"

VpnConfigurationsController::VpnConfigurationsController(const std::shared_ptr<Settings> &settings,
                                                         QSharedPointer<ServerController> serverController, QObject *parent)
    : QObject { parent }, m_settings(settings), m_serverController(serverController)
{
}

QScopedPointer<ConfiguratorBase> VpnConfigurationsController::createConfigurator(const Proto protocol)
{
    switch (protocol) {
    case Proto::OpenVpn: return QScopedPointer<ConfiguratorBase>(new OpenVpnConfigurator(m_settings, m_serverController));
    case Proto::ShadowSocks: return QScopedPointer<ConfiguratorBase>(new ShadowSocksConfigurator(m_settings, m_serverController));
    case Proto::Cloak: return QScopedPointer<ConfiguratorBase>(new CloakConfigurator(m_settings, m_serverController));
    case Proto::WireGuard: return QScopedPointer<ConfiguratorBase>(new WireguardConfigurator(m_settings, m_serverController, false));
    case Proto::Awg: return QScopedPointer<ConfiguratorBase>(new AwgConfigurator(m_settings, m_serverController));
    case Proto::Xray: return QScopedPointer<ConfiguratorBase>(new XrayConfigurator(m_settings, m_serverController));
    case Proto::SSXray: return QScopedPointer<ConfiguratorBase>(new SSXrayConfigurator(m_settings, m_serverController));
    case Proto::Hysteria2: return QScopedPointer<ConfiguratorBase>(new Hysteria2Configurator(m_settings, m_serverController));
    case Proto::AnyTls: return QScopedPointer<ConfiguratorBase>(new AnyTlsConfigurator(m_settings, m_serverController));
    default: return QScopedPointer<ConfiguratorBase>();
    }
}

ErrorCode VpnConfigurationsController::createProtocolConfigForContainer(const ServerCredentials &credentials,
                                                                        const DockerContainer container, QJsonObject &containerConfig)
{
    ErrorCode errorCode = ErrorCode::NoError;

    if (ContainerProps::containerService(container) == ServiceType::Other) {
        return errorCode;
    }

    for (Proto protocol : ContainerProps::protocolsForContainer(container)) {
        QJsonObject protocolConfig = containerConfig.value(ProtocolProps::protoToString(protocol)).toObject();

        auto configurator = createConfigurator(protocol);
        QString protocolConfigString = configurator->createConfig(credentials, container, containerConfig, errorCode);
        if (errorCode != ErrorCode::NoError) {
            return errorCode;
        }

        protocolConfig.insert(config_key::last_config, protocolConfigString);
        containerConfig.insert(ProtocolProps::protoToString(protocol), protocolConfig);
    }

    return errorCode;
}

ErrorCode VpnConfigurationsController::createProtocolConfigString(const bool isApiConfig, const QPair<QString, QString> &dns,
                                                                  const ServerCredentials &credentials, const DockerContainer container,
                                                                  const QJsonObject &containerConfig, const Proto protocol,
                                                                  QString &protocolConfigString)
{
    ErrorCode errorCode = ErrorCode::NoError;

    if (ContainerProps::containerService(container) == ServiceType::Other) {
        return errorCode;
    }

    auto configurator = createConfigurator(protocol);

    protocolConfigString = configurator->createConfig(credentials, container, containerConfig, errorCode);
    if (errorCode != ErrorCode::NoError) {
        return errorCode;
    }
    protocolConfigString = configurator->processConfigWithExportSettings(dns, isApiConfig, protocolConfigString);

    return errorCode;
}

QJsonObject VpnConfigurationsController::createVpnConfiguration(const QPair<QString, QString> &dns, const QJsonObject &serverConfig,
                                                                const QJsonObject &containerConfig, const DockerContainer container)
{
    QJsonObject vpnConfiguration {};

    if (ContainerProps::containerService(container) == ServiceType::Other) {
        return vpnConfiguration;
    }

    bool isApiConfig = serverConfig.value(config_key::configVersion).toInt();

    for (ProtocolEnumNS::Proto proto : ContainerProps::protocolsForContainer(container)) {
        if (isApiConfig && container == DockerContainer::Cloak && proto == ProtocolEnumNS::Proto::ShadowSocks) {
            continue;
        }

        QString protocolConfigString =
                containerConfig.value(ProtocolProps::protoToString(proto)).toObject().value(config_key::last_config).toString();

        auto configurator = createConfigurator(proto);
        protocolConfigString = configurator->processConfigWithLocalSettings(dns, isApiConfig, protocolConfigString);

        QJsonObject vpnConfigData;
        if (proto == ProtocolEnumNS::Proto::Hysteria2) {
            // Hysteria2Configurator produces a YAML payload (not JSON).
            // Wrap it in a JSON envelope alongside relevant sub-config fields
            // so Hysteria2Protocol can recover both the rendered YAML and the
            // numeric port/site values it needs without re-parsing YAML.
            const QJsonObject subCfg = containerConfig.value(ProtocolProps::protoToString(proto)).toObject();
            vpnConfigData = QJsonObject {
                { QStringLiteral("yaml_config"), protocolConfigString },
                { QStringLiteral("local_port"), subCfg.value(config_key::local_port).toString() },
                { QStringLiteral("port"),       subCfg.value(config_key::port).toString() },
                { QStringLiteral("site"),       subCfg.value(config_key::site).toString() },
            };
        } else {
            vpnConfigData = QJsonDocument::fromJson(protocolConfigString.toUtf8()).object();
        }
        if (ContainerProps::isAwgContainer(container) || container == DockerContainer::WireGuard) {
            // add mtu for old configs
            if (vpnConfigData[config_key::mtu].toString().isEmpty()) {
                vpnConfigData[config_key::mtu] =
                        ContainerProps::isAwgContainer(container) ? protocols::awg::defaultMtu :
                        protocols::wireguard::defaultMtu;
            }
        }

        vpnConfiguration.insert(ProtocolProps::key_proto_config_data(proto), vpnConfigData);
    }

    Proto proto = ContainerProps::defaultProtocol(container);
    vpnConfiguration[config_key::vpnproto] = ProtocolProps::protoToString(proto);

    vpnConfiguration[config_key::dns1] = dns.first;
    vpnConfiguration[config_key::dns2] = dns.second;

    vpnConfiguration[config_key::hostName] = serverConfig.value(config_key::hostName).toString();
    vpnConfiguration[config_key::description] = serverConfig.value(config_key::description).toString();

    vpnConfiguration[config_key::configVersion] = serverConfig.value(config_key::configVersion).toInt();
    // TODO: try to get hostName, port, description for 3rd party configs
    // vpnConfiguration[config_key::port] = ...;

    return vpnConfiguration;
}

void VpnConfigurationsController::updateContainerConfigAfterInstallation(const DockerContainer container, QJsonObject &containerConfig,
                                                                         const QString &stdOut)
{
    Proto mainProto = ContainerProps::defaultProtocol(container);

    if (container == DockerContainer::TorWebSite) {
        QJsonObject protocol = containerConfig.value(ProtocolProps::protoToString(mainProto)).toObject();

        qDebug() << "amnezia-tor onions" << stdOut;

        QString onion = stdOut;
        onion.replace("\n", "");
        protocol.insert(config_key::site, onion);

        containerConfig.insert(ProtocolProps::protoToString(mainProto), protocol);
    }
}

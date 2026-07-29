#include "awg_configurator.h"
#include "protocols/protocols_defs.h"

#include <QJsonDocument>
#include <QJsonObject>

AwgConfigurator::AwgConfigurator(std::shared_ptr<Settings> settings, const QSharedPointer<ServerController> &serverController, QObject *parent)
    : WireguardConfigurator(settings, serverController, true, parent)
{
}

QString AwgConfigurator::createConfig(const ServerCredentials &credentials, DockerContainer container, const QJsonObject &containerConfig,
                                      ErrorCode &errorCode)
{
    QString config = WireguardConfigurator::createConfig(credentials, container, containerConfig, errorCode);

    QJsonObject jsonConfig = QJsonDocument::fromJson(config.toUtf8()).object();
    QString awgConfig = jsonConfig.value(config_key::config).toString();

    QMap<QString, QString> configMap;
    auto configLines = awgConfig.split("\n");
    for (auto &line : configLines) {
        auto trimmedLine = line.trimmed();
        if (trimmedLine.startsWith("[") && trimmedLine.endsWith("]")) {
            continue;
        } else {
            QStringList parts = trimmedLine.split(" = ");
            if (parts.count() == 2) {
                configMap.insert(parts[0].trimmed(), parts[1].trimmed());
            }
        }
    }

    jsonConfig[config_key::junkPacketCount] = configMap.value(config_key::junkPacketCount);
    jsonConfig[config_key::junkPacketMinSize] = configMap.value(config_key::junkPacketMinSize);
    jsonConfig[config_key::junkPacketMaxSize] = configMap.value(config_key::junkPacketMaxSize);
    jsonConfig[config_key::initPacketJunkSize] = configMap.value(config_key::initPacketJunkSize);
    jsonConfig[config_key::responsePacketJunkSize] = configMap.value(config_key::responsePacketJunkSize);
    jsonConfig[config_key::initPacketMagicHeader] = configMap.value(config_key::initPacketMagicHeader);
    jsonConfig[config_key::responsePacketMagicHeader] = configMap.value(config_key::responsePacketMagicHeader);
    jsonConfig[config_key::underloadPacketMagicHeader] = configMap.value(config_key::underloadPacketMagicHeader);
    jsonConfig[config_key::transportPacketMagicHeader] = configMap.value(config_key::transportPacketMagicHeader);

    if (container == DockerContainer::Awg2) {
        jsonConfig[config_key::cookieReplyPacketJunkSize] = configMap.value(config_key::cookieReplyPacketJunkSize);
        jsonConfig[config_key::transportPacketJunkSize] = configMap.value(config_key::transportPacketJunkSize);
    }

    jsonConfig[config_key::specialJunk1] = configMap.value(amnezia::config_key::specialJunk1);
    jsonConfig[config_key::specialJunk2] = configMap.value(amnezia::config_key::specialJunk2);
    jsonConfig[config_key::specialJunk3] = configMap.value(amnezia::config_key::specialJunk3);
    jsonConfig[config_key::specialJunk4] = configMap.value(amnezia::config_key::specialJunk4);
    jsonConfig[config_key::specialJunk5] = configMap.value(amnezia::config_key::specialJunk5);

    // AmneziaWG 3. Lift only the keys the generated .conf actually carries -- the
    // header protection line is absent whenever the server container predates AWG3,
    // and writing an empty value into the JSON would make the daemon emit a
    // valueless config line that amneziawg rejects. by vovankrot
    for (const char *awg3Key : { config_key::headerProtectionKey, config_key::contentPaddingAddition,
                                 config_key::rekeyAfterTime, config_key::rekeyTimeout,
                                 config_key::rejectAfterTime, config_key::keepaliveTimeout,
                                 config_key::maxHandshakeAttempts }) {
        const QString value = configMap.value(QString::fromLatin1(awg3Key));
        if (!value.isEmpty()) {
            jsonConfig[awg3Key] = value;
        }
    }

    jsonConfig[config_key::mtu] =
            containerConfig.value(ProtocolProps::protoToString(Proto::Awg)).toObject().value(config_key::mtu).toString(protocols::awg::defaultMtu);

    return QJsonDocument(jsonConfig).toJson();
}

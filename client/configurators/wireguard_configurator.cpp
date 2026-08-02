#include "wireguard_configurator.h"

#include <QDebug>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QString>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include "containers/containers_defs.h"
#include "core/controllers/serverController.h"
#include "core/scripts_registry.h"
#include "core/server_defs.h"
#include "settings.h"
#include "utilities.h"

WireguardConfigurator::WireguardConfigurator(std::shared_ptr<Settings> settings,
                                             const QSharedPointer<ServerController> &serverController, bool isAwg,
                                             QObject *parent)
    : ConfiguratorBase(settings, serverController, parent), m_isAwg(isAwg)
{
    m_serverConfigPath =
            m_isAwg ? amnezia::protocols::awg::serverConfigPath : amnezia::protocols::wireguard::serverConfigPath;
    m_serverPublicKeyPath =
            m_isAwg ? amnezia::protocols::awg::serverPublicKeyPath : amnezia::protocols::wireguard::serverPublicKeyPath;
    m_serverPskKeyPath =
            m_isAwg ? amnezia::protocols::awg::serverPskKeyPath : amnezia::protocols::wireguard::serverPskKeyPath;
    // Only AWG has header protection; plain WireGuard leaves this empty.
    m_serverHeaderProtectionKeyPath =
            m_isAwg ? amnezia::protocols::awg::serverHeaderProtectionKeyPath : QString();
    m_configTemplate = m_isAwg ? ProtocolScriptType::awg_template : ProtocolScriptType::wireguard_template;

    m_protocolName = m_isAwg ? config_key::awg : config_key::wireguard;
    m_defaultPort = m_isAwg ? protocols::wireguard::defaultPort : protocols::awg::defaultPort;
}

WireguardConfigurator::ConnectionData WireguardConfigurator::genClientKeys()
{
    // TODO review
    constexpr size_t EDDSA_KEY_LENGTH = 32;

    ConnectionData connData;

    unsigned char buff[EDDSA_KEY_LENGTH];
    int ret = RAND_priv_bytes(buff, EDDSA_KEY_LENGTH);
    if (ret <= 0)
        return connData;

    EVP_PKEY *pKey = EVP_PKEY_new();
    q_check_ptr(pKey);
    pKey = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, &buff[0], EDDSA_KEY_LENGTH);

    size_t keySize = EDDSA_KEY_LENGTH;

    // save private key
    unsigned char priv[EDDSA_KEY_LENGTH];
    EVP_PKEY_get_raw_private_key(pKey, priv, &keySize);
    connData.clientPrivKey = QByteArray::fromRawData((char *)priv, keySize).toBase64();

    // save public key
    unsigned char pub[EDDSA_KEY_LENGTH];
    EVP_PKEY_get_raw_public_key(pKey, pub, &keySize);
    connData.clientPubKey = QByteArray::fromRawData((char *)pub, keySize).toBase64();

    return connData;
}

QList<QHostAddress> WireguardConfigurator::getIpsFromConf(const QString &input)
{
    QRegularExpression regex("AllowedIPs = (\\d+\\.\\d+\\.\\d+\\.\\d+)");
    QRegularExpressionMatchIterator matchIterator = regex.globalMatch(input);

    QList<QHostAddress> ips;

    while (matchIterator.hasNext()) {
        QRegularExpressionMatch match = matchIterator.next();
        const QString address_string { match.captured(1) };
        const QHostAddress address { address_string };
        if (address.isNull()) {
            qWarning() << "Couldn't recognize the ip address: " << address_string;
        } else {
            ips << address;
        }
    }

    return ips;
}

WireguardConfigurator::ConnectionData WireguardConfigurator::prepareWireguardConfig(const ServerCredentials &credentials,
                                                                                    DockerContainer container,
                                                                                    const QJsonObject &containerConfig,
                                                                                    ErrorCode &errorCode)
{
    WireguardConfigurator::ConnectionData connData = WireguardConfigurator::genClientKeys();
    connData.host = credentials.hostName;
    connData.port = containerConfig.value(m_protocolName).toObject().value(config_key::port).toString(m_defaultPort);

    if (connData.clientPrivKey.isEmpty() || connData.clientPubKey.isEmpty()) {
        errorCode = ErrorCode::InternalError;
        return connData;
    }

    QString configPath = m_serverConfigPath;
    if (container == DockerContainer::Awg) {
        configPath = amnezia::protocols::awg::serverLegacyConfigPath;
    }
    QString getIpsScript = QString("cat %1 | grep AllowedIPs").arg(configPath);
    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };

    errorCode = m_serverController->runContainerScript(credentials, container, getIpsScript, cbReadStdOut);
    if (errorCode != ErrorCode::NoError) {
        return connData;
    }
    auto ips = getIpsFromConf(stdOut);

    QHostAddress nextIp = [&] {
        QHostAddress result;
        QHostAddress lastIp;
        if (ips.empty()) {
            lastIp.setAddress(containerConfig.value(m_protocolName)
                                      .toObject()
                                      .value(config_key::subnet_address)
                                      .toString(protocols::wireguard::defaultSubnetAddress));
        } else {
            lastIp = ips.last();
        }
        quint8 lastOctet = static_cast<quint8>(lastIp.toIPv4Address());
        switch (lastOctet) {
        case 254: result.setAddress(lastIp.toIPv4Address() + 3); break;
        case 255: result.setAddress(lastIp.toIPv4Address() + 2); break;
        default: result.setAddress(lastIp.toIPv4Address() + 1); break;
        }

        return result;
    }();

    connData.clientIP = nextIp.toString();

    // Get keys
    connData.serverPubKey =
            m_serverController->getTextFileFromContainer(container, credentials, m_serverPublicKeyPath, errorCode);
    connData.serverPubKey.replace("\n", "");
    if (errorCode != ErrorCode::NoError) {
        return connData;
    }

    connData.pskKey = m_serverController->getTextFileFromContainer(container, credentials, m_serverPskKeyPath, errorCode);
    connData.pskKey.replace("\n", "");

    if (errorCode != ErrorCode::NoError) {
        return connData;
    }

    // AmneziaWG 3 header protection key -- optional by design.
    // configure_container.sh writes this file only when header protection was both
    // requested and accepted by the installed amneziawg-go, so a missing file is the
    // normal state for every container built before AWG3 and for anyone who left the
    // option off. Read it opportunistically and swallow the error: the tunnel is
    // perfectly valid without it, and failing the whole config build here would break
    // every existing installation. by vovankrot
    if (!m_serverHeaderProtectionKeyPath.isEmpty()) {
        ErrorCode headerKeyError = ErrorCode::NoError;
        QString headerKey = m_serverController->getTextFileFromContainer(
                container, credentials, m_serverHeaderProtectionKeyPath, headerKeyError);
        headerKey.replace("\n", "");
        headerKey = headerKey.trimmed();
        if (headerKeyError == ErrorCode::NoError && !headerKey.isEmpty()) {
            // The key file existing is NOT proof the server can use it. Some images
            // ship an AWG3 daemon alongside older userspace tools, and awg-quick /
            // awg setconf are what actually parse awg0.conf -- they reject
            // HeaderProtectionKey as an unrecognised line. Once such a key is on the
            // server, EVERY later operation fails, because adding a client runs
            // `awg syncconf` over that same config. Trusting the file alone is how a
            // server ends up permanently stuck reporting "Server command failed".
            //
            // So verify the tools themselves, and if they cannot parse it, undo the
            // damage rather than propagating it into a client config: drop the line,
            // remove the key file, resync. by vovankrot
            const QString probe = QStringLiteral(
                    "sudo docker exec $CONTAINER_NAME sh -c "
                    "'grep -aq HeaderProtectionKey \"$(command -v awg || echo /usr/bin/awg)\"'");
            ErrorCode probeError = m_serverController->runScript(
                    credentials, m_serverController->replaceVars(
                                         probe, m_serverController->genVarsForScript(credentials, container)));

            if (probeError == ErrorCode::NoError) {
                connData.headerProtectionKey = headerKey;
                qDebug() << "AWG3: server provides header protection";
            } else {
                // Not a reason to quietly settle for generation 2. The base image gained
                // AWG3-capable amneziawg-tools on 2026-07-30; a container built before
                // that carries tools from 2021 that do not know the key. Since
                // build_container.sh already runs `docker build --no-cache --pull`,
                // simply reinstalling AmneziaWG pulls a current image and AWG3 works.
                // So say that out loud in the install log instead of leaving the user to
                // wonder why the switch is on and nothing changed. by vovankrot
                emit m_serverController->logLineReady(
                        tr("This AmneziaWG container is too old for AmneziaWG 3 — its amneziawg tools cannot "
                           "read the header protection key. Reinstall AmneziaWG on this server to get it: the "
                           "container will be rebuilt from a current image. Falling back to generation 2 for now."));
                qWarning() << "AWG3: server has a header protection key but its amneziawg tools cannot parse it;"
                           << "removing it so the tunnel keeps working -- reinstall the container for real AWG3";
                // Bring the interface back with awg-quick, not syncconf. syncconf takes a
                // FILE PATH and needs an interface that already exists -- and it does not:
                // awg-quick up failed at container start on the very key we just removed,
                // so awg0 was never created and syncconf answers "Unable to retrieve
                // current interface configuration: Protocol not supported". down-then-up
                // recreates it from the now-clean config, which is what start.sh does.
                // by vovankrot
                const QString repair = QString("sudo docker exec $CONTAINER_NAME sh -c "
                                               "\"sed -i '/^HeaderProtectionKey/d' %1; rm -f %2; "
                                               "awg-quick down %1 >/dev/null 2>&1; awg-quick up %1\"")
                                               .arg(m_serverConfigPath, m_serverHeaderProtectionKeyPath);
                m_serverController->runScript(
                        credentials,
                        m_serverController->replaceVars(repair, m_serverController->genVarsForScript(credentials, container)));
            }
        } else {
            qDebug() << "AWG3: no header protection on this server, building plain AWG2 config";
        }
    }


    // Add client to config
    QString configPart = QString("[Peer]\n"
                                 "PublicKey = %1\n"
                                 "PresharedKey = %2\n"
                                 "AllowedIPs = %3/32\n\n")
                                 .arg(connData.clientPubKey, connData.pskKey, connData.clientIP);

    errorCode = m_serverController->uploadTextFileToContainer(container, credentials, configPart, configPath,
                                                              libssh::ScpOverwriteMode::ScpAppendToExisting);

    if (errorCode != ErrorCode::NoError) {
        return connData;
    }

    bool isAwg = (container == DockerContainer::Awg2);
    QString bin = isAwg ? QStringLiteral("awg") : QStringLiteral("wg");
    QString iface = isAwg ? QStringLiteral("awg0") : QStringLiteral("wg0");
    QString script = QString(
        "sudo docker exec $CONTAINER_NAME bash -c '%1 syncconf %2 <(%1-quick strip %3)'").arg(bin, iface, configPath);

    errorCode = m_serverController->runScript(
            credentials,
            m_serverController->replaceVars(script, m_serverController->genVarsForScript(credentials, container)));

    return connData;
}

QString WireguardConfigurator::createConfig(const ServerCredentials &credentials, DockerContainer container,
                                            const QJsonObject &containerConfig, ErrorCode &errorCode)
{
    QString scriptData = amnezia::scriptData(m_configTemplate, container);
    QString config = m_serverController->replaceVars(
            scriptData, m_serverController->genVarsForScript(credentials, container, containerConfig));

    ConnectionData connData = prepareWireguardConfig(credentials, container, containerConfig, errorCode);
    if (errorCode != ErrorCode::NoError) {
        return "";
    }

    config.replace("$WIREGUARD_CLIENT_PRIVATE_KEY", connData.clientPrivKey);
    config.replace("$WIREGUARD_CLIENT_IP", connData.clientIP);
    config.replace("$WIREGUARD_SERVER_PUBLIC_KEY", connData.serverPubKey);
    config.replace("$WIREGUARD_PSK", connData.pskKey);

    // AWG3 header protection: substitute the key, or drop the line outright.
    // An empty "HeaderProtectionKey = " line is NOT equivalent to no line at all --
    // amneziawg parses it as a zero-length key and refuses the whole config, so the
    // no-key path has to remove the line rather than blank it out. Same reasoning
    // for ContentPaddingAddition below. by vovankrot
    if (connData.headerProtectionKey.isEmpty()) {
        static const QRegularExpression headerProtectionLine(
                QStringLiteral("^[ \\t]*HeaderProtectionKey[ \\t]*=.*(?:\\r?\\n|$)"),
                QRegularExpression::MultilineOption);
        config.remove(headerProtectionLine);
    } else {
        config.replace("$AWG_HEADER_PROTECTION_KEY", connData.headerProtectionKey);
    }

    {
        static const QRegularExpression emptyContentPaddingLine(
                QStringLiteral("^[ \\t]*ContentPaddingAddition[ \\t]*=[ \\t]*(?:\\r?\\n|$)"),
                QRegularExpression::MultilineOption);
        config.remove(emptyContentPaddingLine);
    }

    const QJsonObject &wireguarConfig = containerConfig.value(ProtocolProps::protoToString(Proto::WireGuard)).toObject();
    QJsonObject jConfig;
    jConfig[config_key::config] = config;

    jConfig[config_key::hostName] = connData.host;
    jConfig[config_key::port] = connData.port.toInt();
    jConfig[config_key::client_priv_key] = connData.clientPrivKey;
    jConfig[config_key::client_ip] = connData.clientIP;
    jConfig[config_key::client_pub_key] = connData.clientPubKey;
    jConfig[config_key::psk_key] = connData.pskKey;
    jConfig[config_key::server_pub_key] = connData.serverPubKey;
    jConfig[config_key::mtu] = wireguarConfig.value(config_key::mtu).toString(protocols::wireguard::defaultMtu);

    jConfig[config_key::persistent_keep_alive] = "25";
    QJsonArray allowedIps { "0.0.0.0/0", "::/0" };
    jConfig[config_key::allowed_ips] = allowedIps;

    jConfig[config_key::clientId] = connData.clientPubKey;

    return QJsonDocument(jConfig).toJson();
}

QString WireguardConfigurator::processConfigWithLocalSettings(const QPair<QString, QString> &dns,
                                                              const bool isApiConfig, QString &protocolConfigString)
{
    processConfigWithDnsSettings(dns, protocolConfigString);

    return protocolConfigString;
}

QString WireguardConfigurator::processConfigWithExportSettings(const QPair<QString, QString> &dns,
                                                               const bool isApiConfig, QString &protocolConfigString)
{
    processConfigWithDnsSettings(dns, protocolConfigString);

    return protocolConfigString;
}

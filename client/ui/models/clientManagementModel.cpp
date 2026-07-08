#include "clientManagementModel.h"

#include <algorithm>

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include "core/controllers/serverController.h"
#include "logger.h"

namespace
{
    Logger logger("ClientManagementModel");

    namespace configKey
    {
        constexpr char clientId[] = "clientId";
        constexpr char clientName[] = "clientName";
        constexpr char container[] = "container";
        constexpr char userData[] = "userData";
        constexpr char creationDate[] = "creationDate";
        constexpr char latestHandshake[] = "latestHandshake";
        constexpr char dataReceived[] = "dataReceived";
        constexpr char dataSent[] = "dataSent";
        constexpr char allowedIps[] = "allowedIps";
        constexpr char isOnline[] = "isOnline";
        constexpr char latestActivity[] = "latestActivity";
        constexpr char latestClientIp[] = "latestClientIp";
        constexpr char latestDestination[] = "latestDestination";
        constexpr char visitHistory[] = "visitHistory";
        constexpr char xrayEmail[] = "xrayEmail";
    }

    struct XrayVisit
    {
        QString identity;
        QString clientIp;
        QString destination;
        QString route;
        QString timestampText;
        QDateTime timestamp;
    };

    QString clientsTableFilePath(DockerContainer container)
    {
        QString clientsTableFile = QStringLiteral("/opt/amnezia/%1/clientsTable");
        if (container == DockerContainer::OpenVpn || container == DockerContainer::ShadowSocks || container == DockerContainer::Cloak) {
            return clientsTableFile.arg(ContainerProps::containerTypeToString(DockerContainer::OpenVpn));
        }
        return clientsTableFile.arg(ContainerProps::containerTypeToString(container));
    }

    QString searchTextForClient(const QJsonObject &client)
    {
        const QJsonObject userData = client.value(configKey::userData).toObject();
        return QStringList {
            client.value(configKey::clientId).toString(),
            userData.value(configKey::clientName).toString(),
            userData.value(configKey::allowedIps).toString(),
            userData.value(configKey::latestClientIp).toString(),
            userData.value(configKey::latestDestination).toString(),
            userData.value(configKey::visitHistory).toString()
        }.join(QLatin1Char(' '));
    }

    QDateTime parseXrayTimestamp(const QString &line, QString &timestampText)
    {
        static const QRegularExpression dockerTimestampRe(
            QStringLiteral(R"((\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:\d{2})))"));
        static const QRegularExpression slashTimestampRe(
            QStringLiteral(R"((\d{4}/\d{2}/\d{2}\s+\d{2}:\d{2}:\d{2}))"));

        auto dockerMatch = dockerTimestampRe.match(line);
        if (dockerMatch.hasMatch()) {
            timestampText = dockerMatch.captured(1);
            QDateTime dt = QDateTime::fromString(timestampText, Qt::ISODateWithMs);
            if (!dt.isValid()) {
                dt = QDateTime::fromString(timestampText, Qt::ISODate);
            }
            return dt;
        }

        auto slashMatch = slashTimestampRe.match(line);
        if (slashMatch.hasMatch()) {
            timestampText = slashMatch.captured(1);
            QDateTime dt = QDateTime::fromString(timestampText, QStringLiteral("yyyy/MM/dd HH:mm:ss"));
            dt.setTimeSpec(Qt::UTC);
            return dt;
        }

        timestampText.clear();
        return QDateTime();
    }

    QString xrayEmailForClientId(const QString &clientId)
    {
        return clientId;
    }

    QString shellQuote(const QString &value)
    {
        QString escaped = value;
        escaped.replace(QLatin1Char('\''), QStringLiteral("'\"'\"'"));
        return QLatin1Char('\'') + escaped + QLatin1Char('\'');
    }

    QString destinationHost(const QString &destination)
    {
        const QString value = destination.trimmed();
        if (value.startsWith(QLatin1Char('['))) {
            const int closingBracketIndex = value.indexOf(QLatin1Char(']'));
            if (closingBracketIndex > 1) {
                return value.mid(1, closingBracketIndex - 1);
            }
        }

        if (value.count(QLatin1Char(':')) == 1) {
            return value.section(QLatin1Char(':'), 0, 0);
        }

        return value;
    }

    bool isIpv4Address(const QString &host)
    {
        const QStringList octets = host.split(QLatin1Char('.'));
        if (octets.size() != 4) {
            return false;
        }

        for (const QString &octet : octets) {
            if (octet.isEmpty()) {
                return false;
            }

            bool ok = false;
            const int value = octet.toInt(&ok);
            if (!ok || value < 0 || value > 255) {
                return false;
            }
        }

        return true;
    }

    QString cleanReverseDnsName(QString name)
    {
        name = name.trimmed();
        while (name.endsWith(QLatin1Char('.'))) {
            name.chop(1);
        }
        return name;
    }

    QString destinationWithReverseDns(const QString &destination, const QHash<QString, QString> &reverseDnsNames)
    {
        const QString host = destinationHost(destination);
        const QString name = reverseDnsNames.value(host);
        if (name.isEmpty() || name == host) {
            return destination;
        }

        return QStringLiteral("%1 (%2)").arg(name, destination);
    }

    QHash<QString, QString> resolveReverseDnsNames(const QStringList &rawIps, const ServerCredentials &credentials,
                                                   const QSharedPointer<ServerController> &serverController)
    {
        QStringList ips;
        for (const QString &ip : rawIps) {
            if (isIpv4Address(ip) && !ips.contains(ip)) {
                ips.append(ip);
            }
        }

        ips.sort();
        const int maxReverseDnsLookups = 64;
        if (ips.size() > maxReverseDnsLookups) {
            ips = ips.mid(0, maxReverseDnsLookups);
        }

        if (ips.isEmpty()) {
            return {};
        }

        QStringList quotedIps;
        for (const QString &ip : ips) {
            quotedIps.append(shellQuote(ip));
        }

        const QString script = QStringLiteral(
            "lookup_timeout=\"\"\n"
            "if command -v timeout >/dev/null 2>&1; then lookup_timeout=\"timeout 1\"; fi\n"
            "for ip in %1; do\n"
            "  name=\"\"\n"
            "  if command -v getent >/dev/null 2>&1; then name=$($lookup_timeout getent hosts \"$ip\" 2>/dev/null | awk '{print $2; exit}'); fi\n"
            "  if [ -z \"$name\" ] && command -v host >/dev/null 2>&1; then name=$($lookup_timeout host \"$ip\" 2>/dev/null | awk '/domain name pointer/ {print $5; exit}'); fi\n"
            "  if [ -z \"$name\" ] && command -v nslookup >/dev/null 2>&1; then name=$($lookup_timeout nslookup \"$ip\" 2>/dev/null | awk '/name =/ {print $4; exit} /^Name:/ {print $2; exit}'); fi\n"
            "  if [ -n \"$name\" ]; then name=${name%.}; printf 'AMNEZIA_RDNS\\t%s\\t%s\\n' \"$ip\" \"$name\"; fi\n"
            "done\n").arg(quotedIps.join(QLatin1Char(' ')));

        QString output;
        auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
            output += data + QLatin1Char('\n');
            return ErrorCode::NoError;
        };

        const ErrorCode error = serverController->runScript(credentials, script, cbReadStdOut, cbReadStdOut, 8000, 15000);
        if (error != ErrorCode::NoError) {
            logger.warning() << "Reverse DNS lookup for XRay history failed" << error;
            return {};
        }

        QHash<QString, QString> reverseDnsNames;
        const QSet<QString> requestedIps(ips.begin(), ips.end());
        const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            if (!line.startsWith(QStringLiteral("AMNEZIA_RDNS\t"))) {
                continue;
            }

            const QStringList parts = line.split(QLatin1Char('\t'));
            if (parts.size() < 3 || !requestedIps.contains(parts.at(1))) {
                continue;
            }

            const QString name = cleanReverseDnsName(parts.at(2));
            if (!name.isEmpty()) {
                reverseDnsNames.insert(parts.at(1), name);
            }
        }

        return reverseDnsNames;
    }

    bool isRecentWireGuardHandshake(const QString &latestHandshake)
    {
        const QString value = latestHandshake.toLower();
        if (value.isEmpty() || value.contains(QStringLiteral("never"))) {
            return false;
        }
        if (value.contains(QLatin1Char('s'))) {
            return true;
        }

        static const QRegularExpression minutesRe(QStringLiteral(R"((\d+)m)"));
        const auto minutesMatch = minutesRe.match(value);
        return minutesMatch.hasMatch() && minutesMatch.captured(1).toInt() <= 5;
    }
}

ClientManagementModel::ClientManagementModel(std::shared_ptr<Settings> settings, QObject *parent)
    : m_settings(settings), QAbstractListModel(parent)
{
}

int ClientManagementModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return static_cast<int>(m_clientsTable.size());
}

QVariant ClientManagementModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_clientsTable.size())) {
        return QVariant();
    }

    auto client = m_clientsTable.at(index.row()).toObject();
    auto userData = client.value(configKey::userData).toObject();

    switch (role) {
    case ClientNameRole: return userData.value(configKey::clientName).toString();
    case CreationDateRole: return userData.value(configKey::creationDate).toString();
    case LatestHandshakeRole: return userData.value(configKey::latestHandshake).toString();
    case DataReceivedRole: return userData.value(configKey::dataReceived).toString();
    case DataSentRole: return userData.value(configKey::dataSent).toString();
    case AllowedIpsRole: return userData.value(configKey::allowedIps).toString();
    case ClientIdRole: return client.value(configKey::clientId).toString();
    case IsOnlineRole: return userData.value(configKey::isOnline).toBool(false);
    case LatestActivityRole: return userData.value(configKey::latestActivity).toString();
    case LatestClientIpRole: return userData.value(configKey::latestClientIp).toString();
    case LatestDestinationRole: return userData.value(configKey::latestDestination).toString();
    case VisitHistoryRole: return userData.value(configKey::visitHistory).toString();
    case SearchTextRole: return searchTextForClient(client);
    }

    return QVariant();
}

void ClientManagementModel::migration(const QByteArray &clientsTableString)
{
    QJsonObject clientsTable = QJsonDocument::fromJson(clientsTableString).object();

    for (auto &clientId : clientsTable.keys()) {
        QJsonObject client;
        client[configKey::clientId] = clientId;

        QJsonObject userData;
        userData[configKey::clientName] = clientsTable.value(clientId).toObject().value(configKey::clientName);
        client[configKey::userData] = userData;

        m_clientsTable.push_back(client);
    }
}

ErrorCode ClientManagementModel::updateModel(const DockerContainer container, const ServerCredentials &credentials,
                                             const QSharedPointer<ServerController> &serverController)
{
    beginResetModel();
    m_clientsTable = QJsonArray();
    endResetModel();

    if (!ContainerProps::supportsUserManagement(container)) {
        return ErrorCode::NoError;
    }

    ErrorCode error = ErrorCode::NoError;

    const QString clientsTableFile = clientsTableFilePath(container);

    QByteArray clientsTableString = serverController->getTextFileFromContainer(container, credentials, clientsTableFile, error);
    if (error != ErrorCode::NoError) {
        if (container == DockerContainer::Xray && error == ErrorCode::ServerCheckFailed) {
            logger.warning() << "Xray clientsTable is missing; using empty clients table and rebuilding from server.json";
            clientsTableString = QByteArrayLiteral("[]");
            error = ErrorCode::NoError;
        } else {
            logger.error() << "Failed to get the clientsTable file from the server";
            return error;
        }
    }

    beginResetModel();
    m_clientsTable = QJsonDocument::fromJson(clientsTableString).array();

    if (m_clientsTable.isEmpty()) {
        migration(clientsTableString);

        int count = 0;

        if (container == DockerContainer::OpenVpn || container == DockerContainer::ShadowSocks || container == DockerContainer::Cloak) {
            error = getOpenVpnClients(container, credentials, serverController, count);
        } else if (container == DockerContainer::WireGuard || ContainerProps::isAwgContainer(container)) {
            error = getWireGuardClients(container, credentials, serverController, count);
        } else if (container == DockerContainer::Xray) {
            error = getXrayClients(container, credentials, serverController, count);
        }
        if (error != ErrorCode::NoError) {
            endResetModel();
            return error;
        }

        const QByteArray newClientsTableString = QJsonDocument(m_clientsTable).toJson();
        if (clientsTableString != newClientsTableString) {
            error = serverController->uploadTextFileToContainer(container, credentials, newClientsTableString, clientsTableFile);
            if (error != ErrorCode::NoError) {
                logger.error() << "Failed to upload the clientsTable file to the server";
            }
        }
    }

    if (container == DockerContainer::Xray) {
        const ErrorCode runtimeError = refreshXrayRuntimeData(container, credentials, serverController);
        if (runtimeError != ErrorCode::NoError) {
            logger.warning() << "Failed to refresh XRay runtime user data" << runtimeError;
        }
    }

    if (container == DockerContainer::WireGuard || ContainerProps::isAwgContainer(container)) {
        for (int i = 0; i < m_clientsTable.size(); ++i) {
            QJsonObject tableClient = m_clientsTable.at(i).toObject();
            QJsonObject userData = tableClient.value(configKey::userData).toObject();
            userData[configKey::isOnline] = false;
            userData.remove(configKey::latestActivity);
            userData.remove(configKey::latestClientIp);
            tableClient[configKey::userData] = userData;
            m_clientsTable.replace(i, tableClient);
        }
    }

    std::vector<WgShowData> data;
    wgShow(container, credentials, serverController, data);

    for (const auto &client : data) {
        int i = 0;
        for (const auto &it : std::as_const(m_clientsTable)) {
            if (it.isObject()) {
                QJsonObject obj = it.toObject();
                if (obj.contains(configKey::clientId) && obj[configKey::clientId].toString() == client.clientId) {
                    QJsonObject userData = obj[configKey::userData].toObject();

                    if (!client.latestHandshake.isEmpty()) {
                        userData[configKey::latestHandshake] = client.latestHandshake;
                    }

                    if (!client.dataReceived.isEmpty()) {
                        userData[configKey::dataReceived] = client.dataReceived;
                    }

                    if (!client.dataSent.isEmpty()) {
                        userData[configKey::dataSent] = client.dataSent;
                    }

                    if (!client.allowedIps.isEmpty()) {
                        userData[configKey::allowedIps] = client.allowedIps;
                    }

                    userData[configKey::isOnline] = isRecentWireGuardHandshake(client.latestHandshake);
                    if (!client.endpoint.isEmpty()) {
                        userData[configKey::latestClientIp] = client.endpoint;
                    }
                    if (!client.latestHandshake.isEmpty()) {
                        userData[configKey::latestActivity] = client.latestHandshake;
                    }

                    obj[configKey::userData] = userData;
                    m_clientsTable.replace(i, obj);
                    break;
                }
            }
            ++i;
        }
    }

    endResetModel();
    return error;
}

ErrorCode ClientManagementModel::getOpenVpnClients(const DockerContainer container, const ServerCredentials &credentials,
                                                   const QSharedPointer<ServerController> &serverController, int &count)
{
    ErrorCode error = ErrorCode::NoError;
    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };

    const QString getOpenVpnClientsList = "sudo docker exec $CONTAINER_NAME bash -c 'ls /opt/amnezia/openvpn/pki/issued'";
    QString script = serverController->replaceVars(getOpenVpnClientsList, serverController->genVarsForScript(credentials, container));
    error = serverController->runScript(credentials, script, cbReadStdOut);
    if (error != ErrorCode::NoError) {
        logger.error() << "Failed to retrieve the list of issued certificates on the server";
        return error;
    }

    if (!stdOut.isEmpty()) {
        QStringList certsIds = stdOut.split("\n", Qt::SkipEmptyParts);
        certsIds.removeAll("AmneziaReq.crt");

        for (auto &openvpnCertId : certsIds) {
            openvpnCertId.replace(".crt", "");
            if (!isClientExists(openvpnCertId)) {
                QJsonObject client;
                client[configKey::clientId] = openvpnCertId;

                QJsonObject userData;
                userData[configKey::clientName] = QString("Client %1").arg(count);
                client[configKey::userData] = userData;

                m_clientsTable.push_back(client);

                count++;
            }
        }
    }
    return error;
}

ErrorCode ClientManagementModel::getWireGuardClients(const DockerContainer container, const ServerCredentials &credentials,
                                                     const QSharedPointer<ServerController> &serverController, int &count)
{
    ErrorCode error = ErrorCode::NoError;

    QString configPath;
    if (container == DockerContainer::Awg) {
        configPath = QString::fromLatin1(amnezia::protocols::awg::serverLegacyConfigPath);
    } else if (container == DockerContainer::Awg2) {
        configPath = QString::fromLatin1(amnezia::protocols::awg::serverConfigPath);
    } else {
        configPath = QString::fromLatin1(amnezia::protocols::wireguard::serverConfigPath);
    }
    const QString wireguardConfigString = serverController->getTextFileFromContainer(container, credentials, configPath, error);
    if (error != ErrorCode::NoError) {
        logger.error() << "Failed to get the wg conf file from the server";
        return error;
    }

    auto configLines = wireguardConfigString.split("\n", Qt::SkipEmptyParts);
    QStringList wireguardKeys;
    for (const auto &line : configLines) {
        auto configPair = line.split(" = ", Qt::SkipEmptyParts);
        if (configPair.front() == "PublicKey") {
            wireguardKeys.push_back(configPair.back());
        }
    }

    for (auto &wireguardKey : wireguardKeys) {
        if (!isClientExists(wireguardKey)) {
            QJsonObject client;
            client[configKey::clientId] = wireguardKey;

            QJsonObject userData;
            userData[configKey::clientName] = QString("Client %1").arg(count);
            client[configKey::userData] = userData;

            m_clientsTable.push_back(client);

            count++;
        }
    }
    return error;
}
ErrorCode ClientManagementModel::getXrayClients(const DockerContainer container, const ServerCredentials& credentials,
                                                const QSharedPointer<ServerController> &serverController, int &count)
{
    ErrorCode error = ErrorCode::NoError;

    const QString serverConfigPath = amnezia::protocols::xray::serverConfigPath;
    const QString configString = serverController->getTextFileFromContainer(container, credentials, serverConfigPath, error);
    if (error != ErrorCode::NoError) {
        logger.error() << "Failed to get the xray server config file from the server";
        return error;
    }

    QJsonDocument serverConfig = QJsonDocument::fromJson(configString.toUtf8());
    if (serverConfig.isNull()) {
        logger.error() << "Failed to parse xray server config JSON";
        return ErrorCode::InternalError;
    }

    if (!serverConfig.object().contains("inbounds") || serverConfig.object()["inbounds"].toArray().isEmpty()) {
        logger.error() << "Invalid xray server config structure";
        return ErrorCode::InternalError;
    }

    const QJsonObject inbound = serverConfig.object()["inbounds"].toArray()[0].toObject();
    if (!inbound.contains("settings")) {
        logger.error() << "Missing settings in xray inbound config";
        return ErrorCode::InternalError;
    }

    const QJsonObject settings = inbound["settings"].toObject();
    if (!settings.contains("clients")) {
        logger.error() << "Missing clients in xray settings config"; 
        return ErrorCode::InternalError;
    }

    const QJsonArray clients = settings["clients"].toArray();
    for (const auto &clientValue : clients) {
        const QJsonObject clientObj = clientValue.toObject();
        if (!clientObj.contains("id")) {
            logger.error() << "Missing id in xray client config";
            continue;
        }
        QString clientId = clientObj["id"].toString();
        
        QString xrayDefaultUuid = serverController->getTextFileFromContainer(container, credentials, amnezia::protocols::xray::uuidPath, error);
        xrayDefaultUuid.replace("\n", "");

        if (!isClientExists(clientId) && clientId != xrayDefaultUuid) {
            QJsonObject client;
            client[configKey::clientId] = clientId;

            QJsonObject userData;
            userData[configKey::clientName] = QString("Client %1").arg(count);
            userData[configKey::xrayEmail] = clientObj.value(QStringLiteral("email")).toString(xrayEmailForClientId(clientId));
            client[configKey::userData] = userData;

            m_clientsTable.push_back(client);
            count++;
        }
    }

    return error;
}

ErrorCode ClientManagementModel::refreshXrayRuntimeData(const DockerContainer container, const ServerCredentials &credentials,
                                                        const QSharedPointer<ServerController> &serverController)
{
    ErrorCode error = ErrorCode::NoError;
    const QString serverConfigPath = amnezia::protocols::xray::serverConfigPath;
    const QString configString = serverController->getTextFileFromContainer(container, credentials, serverConfigPath, error);
    if (error != ErrorCode::NoError) {
        return error;
    }

    const QJsonDocument serverConfig = QJsonDocument::fromJson(configString.toUtf8());
    if (!serverConfig.isObject()) {
        return ErrorCode::InternalError;
    }

    QHash<QString, QString> identityToClientId;
    const QJsonArray inbounds = serverConfig.object().value(QStringLiteral("inbounds")).toArray();
    if (!inbounds.isEmpty()) {
        const QJsonArray clients = inbounds.first().toObject()
            .value(QStringLiteral("settings")).toObject()
            .value(QStringLiteral("clients")).toArray();

        for (const QJsonValue &clientValue : clients) {
            const QJsonObject xrayClient = clientValue.toObject();
            const QString clientId = xrayClient.value(QStringLiteral("id")).toString();
            if (clientId.isEmpty()) {
                continue;
            }

            const QString email = xrayClient.value(QStringLiteral("email")).toString(xrayEmailForClientId(clientId));
            identityToClientId.insert(clientId, clientId);
            if (!email.isEmpty()) {
                identityToClientId.insert(email, clientId);
            }

            for (int i = 0; i < m_clientsTable.size(); ++i) {
                QJsonObject tableClient = m_clientsTable.at(i).toObject();
                if (tableClient.value(configKey::clientId).toString() != clientId) {
                    continue;
                }

                QJsonObject userData = tableClient.value(configKey::userData).toObject();
                userData[configKey::xrayEmail] = email;
                tableClient[configKey::userData] = userData;
                m_clientsTable.replace(i, tableClient);
                break;
            }
        }
    }

    for (int i = 0; i < m_clientsTable.size(); ++i) {
        QJsonObject tableClient = m_clientsTable.at(i).toObject();
        QJsonObject userData = tableClient.value(configKey::userData).toObject();
        userData[configKey::isOnline] = false;
        userData.remove(configKey::latestActivity);
        userData.remove(configKey::latestClientIp);
        userData.remove(configKey::latestDestination);
        userData.remove(configKey::visitHistory);
        tableClient[configKey::userData] = userData;
        m_clientsTable.replace(i, tableClient);
    }

    QString logs;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        logs += data + QLatin1Char('\n');
        return ErrorCode::NoError;
    };

    const QString script = QStringLiteral(
        "sudo docker exec $CONTAINER_NAME sh -c 'for f in /opt/amnezia/xray/access.log /var/log/xray/access.log; do if [ -r \"$f\" ]; then tail -n 1000 \"$f\"; fi; done' 2>/dev/null || true\n"
        "sudo docker logs --timestamps --since 24h --tail 1000 $CONTAINER_NAME 2>&1 || true");

    error = serverController->runScript(credentials,
                                        serverController->replaceVars(script, serverController->genVarsForScript(credentials, container)),
                                        cbReadStdOut,
                                        cbReadStdOut,
                                        10000,
                                        20000);
    if (error != ErrorCode::NoError) {
        return error;
    }

    QHash<QString, QVector<XrayVisit>> visitsByClientId;
    QSet<QString> destinationIps;
    const QString fallbackClientId = (m_clientsTable.size() == 1)
        ? m_clientsTable.first().toObject().value(configKey::clientId).toString()
        : QString();
    static const QRegularExpression acceptedRe(
        QStringLiteral(R"(from\s+(?:tcp|udp):([^\s:]+|\[[^\]]+\]):\d+\s+accepted\s+(tcp|udp):([^\s\[]+)\s+\[([^\]]+)\])"));
    static const QRegularExpression identityRe(
        QStringLiteral(R"((?:email|user):\s*([^\s\]]+))"), QRegularExpression::CaseInsensitiveOption);

    const QStringList lines = logs.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const auto acceptedMatch = acceptedRe.match(line);
        if (!acceptedMatch.hasMatch()) {
            continue;
        }

        QString identity;
        QString clientId;
        const auto identityMatch = identityRe.match(line);
        if (identityMatch.hasMatch()) {
            identity = identityMatch.captured(1).trimmed();
            clientId = identityToClientId.value(identity);
            if (clientId.isEmpty()) {
                continue;
            }
        } else if (!fallbackClientId.isEmpty()) {
            clientId = fallbackClientId;
        } else {
            continue;
        }

        const QString acceptedProto = acceptedMatch.captured(2).toLower();
        const QString destination = acceptedMatch.captured(3).trimmed();
        if (acceptedProto == QStringLiteral("udp") && destination.endsWith(QStringLiteral(":53"))) {
            continue;
        }

        const QString host = destinationHost(destination);
        if (isIpv4Address(host)) {
            destinationIps.insert(host);
        }

        QString timestampText;
        QDateTime timestamp = parseXrayTimestamp(line, timestampText);

        XrayVisit visit;
        visit.identity = identity;
        visit.clientIp = acceptedMatch.captured(1).trimmed();
        visit.destination = destination;
        visit.route = acceptedMatch.captured(4).trimmed();
        visit.timestampText = timestampText;
        visit.timestamp = timestamp;
        visitsByClientId[clientId].append(visit);
    }

    const QHash<QString, QString> reverseDnsNames = resolveReverseDnsNames(destinationIps.values(), credentials, serverController);

    const QDateTime onlineThreshold = QDateTime::currentDateTimeUtc().addSecs(-300);
    for (int i = 0; i < m_clientsTable.size(); ++i) {
        QJsonObject client = m_clientsTable.at(i).toObject();
        const QString clientId = client.value(configKey::clientId).toString();
        QVector<XrayVisit> visits = visitsByClientId.value(clientId);
        if (visits.isEmpty()) {
            continue;
        }

        std::sort(visits.begin(), visits.end(), [](const XrayVisit &left, const XrayVisit &right) {
            if (left.timestamp.isValid() && right.timestamp.isValid()) {
                return left.timestamp > right.timestamp;
            }
            return left.timestampText > right.timestampText;
        });

        const XrayVisit &latest = visits.first();
        QStringList historyLines;
        const int visitsCount = static_cast<int>(visits.size());
        const int historyLimit = visitsCount < 25 ? visitsCount : 25;
        for (int j = 0; j < historyLimit; ++j) {
            const XrayVisit &visit = visits.at(j);
            const QString timeText = visit.timestampText.isEmpty() ? QStringLiteral("-") : visit.timestampText;
            const QString destinationText = destinationWithReverseDns(visit.destination, reverseDnsNames);
            historyLines << QStringLiteral("%1 | %2 -> %3 [%4]")
                                .arg(timeText, visit.clientIp, destinationText, visit.route);
        }

        QJsonObject userData = client.value(configKey::userData).toObject();
        userData[configKey::latestClientIp] = latest.clientIp;
        userData[configKey::latestDestination] = destinationWithReverseDns(latest.destination, reverseDnsNames);
        userData[configKey::latestActivity] = latest.timestampText;
        userData[configKey::visitHistory] = historyLines.join(QLatin1Char('\n'));
        userData[configKey::isOnline] = latest.timestamp.isValid() && latest.timestamp.toUTC() >= onlineThreshold;
        client[configKey::userData] = userData;
        m_clientsTable.replace(i, client);
    }

    return ErrorCode::NoError;
}

ErrorCode ClientManagementModel::wgShow(const DockerContainer container, const ServerCredentials &credentials,
                                        const QSharedPointer<ServerController> &serverController, std::vector<WgShowData> &data)
{
    if (container != DockerContainer::WireGuard && !ContainerProps::isAwgContainer(container)) {
        return ErrorCode::NoError;
    }

    ErrorCode error = ErrorCode::NoError;
    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };

    QString showBin = (container == DockerContainer::Awg2)
                       ? QStringLiteral("awg")
                       : QStringLiteral("wg");
    const QString command = QString("sudo docker exec $CONTAINER_NAME bash -c '%1 show all'")
                             .arg(showBin);

    QString script = serverController->replaceVars(command, serverController->genVarsForScript(credentials, container));
    error = serverController->runScript(credentials, script, cbReadStdOut);
    if (error != ErrorCode::NoError) {
        logger.error() << "Failed to execute wg show command";
        return error;
    }

    if (stdOut.isEmpty()) {
        return error;
    }

    const auto getStrValue = [](const auto str) { return str.mid(str.indexOf(":") + 1).trimmed(); };

    const auto parts = stdOut.split('\n');
    const auto peerList = parts.filter("peer:");
    const auto latestHandshakeList = parts.filter("latest handshake:");
    const auto transferredDataList = parts.filter("transfer:");
    const auto allowedIpsList = parts.filter("allowed ips:");
    const auto endpointList = parts.filter("endpoint:");

    if (allowedIpsList.isEmpty() || latestHandshakeList.isEmpty() || transferredDataList.isEmpty() || peerList.isEmpty()) {
        return error;
    }

    const auto changeHandshakeFormat = [](QString &latestHandshake) {
        const std::vector<std::pair<QString, QString>> replaceMap = { { " days", "d" },    { " hours", "h" }, { " minutes", "m" },
                                                                      { " seconds", "s" }, { " day", "d" },   { " hour", "h" },
                                                                      { " minute", "m" },  { " second", "s" } };

        for (const auto &item : replaceMap) {
            latestHandshake.replace(item.first, item.second);
        }
    };

    for (int i = 0; i < peerList.size() && i < transferredDataList.size() && i < latestHandshakeList.size() && i < allowedIpsList.size(); ++i) {

        const auto transferredData = getStrValue(transferredDataList[i]).split(",");
        auto latestHandshake = getStrValue(latestHandshakeList[i]);
        auto serverBytesReceived = transferredData.front().trimmed();
        auto serverBytesSent = transferredData.back().trimmed();
        auto allowedIps = getStrValue(allowedIpsList[i]);
        QString endpoint;
        if (i < endpointList.size()) {
            endpoint = getStrValue(endpointList[i]);
        }

        changeHandshakeFormat(latestHandshake);

        serverBytesReceived.chop(QStringLiteral(" received").length());
        serverBytesSent.chop(QStringLiteral(" sent").length());

        data.push_back({ getStrValue(peerList[i]), latestHandshake, serverBytesSent, serverBytesReceived, allowedIps, endpoint });
    }

    return error;
}

bool ClientManagementModel::isClientExists(const QString &clientId)
{
    for (const QJsonValue &value : std::as_const(m_clientsTable)) {
        if (value.isObject()) {
            QJsonObject obj = value.toObject();
            if (obj.contains(configKey::clientId) && obj[configKey::clientId].toString() == clientId) {
                return true;
            }
        }
    }
    return false;
}

ErrorCode ClientManagementModel::appendClient(const DockerContainer container, const ServerCredentials &credentials,
                                              const QJsonObject &containerConfig, const QString &clientName,
                                              const QSharedPointer<ServerController> &serverController)
{
    Proto protocol;
    switch (container) {
        case DockerContainer::ShadowSocks:
        case DockerContainer::Cloak:
            protocol = Proto::OpenVpn;
            break;
        case DockerContainer::OpenVpn:
        case DockerContainer::WireGuard:
        case DockerContainer::Awg2:
        case DockerContainer::Awg:
        case DockerContainer::Xray:
            protocol = ContainerProps::defaultProtocol(container);
            break;
        default:
            return ErrorCode::NoError;
    }

    auto protocolConfig = ContainerProps::getProtocolConfigFromContainer(protocol, containerConfig);
    return appendClient(protocolConfig, clientName, container, credentials, serverController);
}

ErrorCode ClientManagementModel::appendClient(QJsonObject &protocolConfig, const QString &clientName, const DockerContainer container,
                                              const ServerCredentials &credentials, const QSharedPointer<ServerController> &serverController)
{
    QString clientId;
    if (container == DockerContainer::Xray) {
        if (!protocolConfig.contains("outbounds")) {
            return ErrorCode::InternalError;
        }
        QJsonArray outbounds = protocolConfig.value("outbounds").toArray();
        if (outbounds.isEmpty()) {
            return ErrorCode::InternalError;
        }
        QJsonObject outbound = outbounds[0].toObject();
        if (!outbound.contains("settings")) {
            return ErrorCode::InternalError;
        }
        QJsonObject settings = outbound["settings"].toObject();
        if (!settings.contains("vnext")) {
            return ErrorCode::InternalError;
        }
        QJsonArray vnext = settings["vnext"].toArray();
        if (vnext.isEmpty()) {
            return ErrorCode::InternalError;
        }
        QJsonObject vnextObj = vnext[0].toObject();
        if (!vnextObj.contains("users")) {
            return ErrorCode::InternalError;
        }
        QJsonArray users = vnextObj["users"].toArray();
        if (users.isEmpty()) {
            return ErrorCode::InternalError;
        }
        QJsonObject user = users[0].toObject();
        if (!user.contains("id")) {
            return ErrorCode::InternalError;
        }
        clientId = user["id"].toString();
    } else {
        clientId = protocolConfig.value(config_key::clientId).toString();
    }
    
    return appendClient(clientId, clientName, container, credentials, serverController);
}

ErrorCode ClientManagementModel::appendClient(const QString &clientId, const QString &clientName, const DockerContainer container,
                                              const ServerCredentials &credentials, const QSharedPointer<ServerController> &serverController)
{
    ErrorCode error = ErrorCode::NoError;

    error = updateModel(container, credentials, serverController);
    if (error != ErrorCode::NoError) {
        return error;
    }

    for (int i = 0; i < m_clientsTable.size(); i++) {
        if (m_clientsTable.at(i).toObject().value(configKey::clientId) == clientId) {
            return renameClient(i, clientName, container, credentials, serverController, true);
        }
    }

    beginInsertRows(QModelIndex(), rowCount(), rowCount() + 1);
    QJsonObject client;
    client[configKey::clientId] = clientId;

    QJsonObject userData;
    userData[configKey::clientName] = clientName;
    userData[configKey::creationDate] = QDateTime::currentDateTime().toString();
    client[configKey::userData] = userData;
    m_clientsTable.push_back(client);
    endInsertRows();

    const QByteArray clientsTableString = QJsonDocument(m_clientsTable).toJson();

    const QString clientsTableFile = clientsTableFilePath(container);

    error = serverController->uploadTextFileToContainer(container, credentials, clientsTableString, clientsTableFile);
    if (error != ErrorCode::NoError) {
        logger.error() << "Failed to upload the clientsTable file to the server";
    }

    return error;
}

ErrorCode ClientManagementModel::renameClient(const int row, const QString &clientName,
                                              const DockerContainer container,
                                              const ServerCredentials &credentials,
                                              const QSharedPointer<ServerController> &serverController, bool addTimeStamp)
{
    auto client = m_clientsTable.at(row).toObject();
    auto userData = client[configKey::userData].toObject();
    userData[configKey::clientName] = clientName;
    if (addTimeStamp) {
        userData[configKey::creationDate] = QDateTime::currentDateTime().toString();
    }
    client[configKey::userData] = userData;

    m_clientsTable.replace(row, client);
    emit dataChanged(index(row, 0), index(row, 0));

    const QByteArray clientsTableString = QJsonDocument(m_clientsTable).toJson();

    const QString clientsTableFile = clientsTableFilePath(container);

    ErrorCode error = serverController->uploadTextFileToContainer(container, credentials, clientsTableString, clientsTableFile);
    if (error != ErrorCode::NoError) {
        logger.error() << "Failed to upload the clientsTable file to the server";
    }

    return error;
}

ErrorCode ClientManagementModel::revokeClient(const int row, const DockerContainer container,
                                              const ServerCredentials &credentials,
                                              const int serverIndex, const QSharedPointer<ServerController> &serverController)
{
    ErrorCode errorCode = ErrorCode::NoError;
    auto client = m_clientsTable.at(row).toObject();
    QString clientId = client.value(configKey::clientId).toString();

    switch(container)
    {
        case DockerContainer::OpenVpn:
        case DockerContainer::ShadowSocks:
        case DockerContainer::Cloak: {
            errorCode = revokeOpenVpn(row, container, credentials, serverIndex, serverController);
            break;
        }
        case DockerContainer::WireGuard:
        case DockerContainer::Awg2:
        case DockerContainer::Awg: {
            errorCode = revokeWireGuard(row, container, credentials, serverController);
            break;
        }
        case DockerContainer::Xray: {
            errorCode = revokeXray(row, container, credentials, serverController);
            break;
        }
        default: {
            logger.error() << "Internal error: received unexpected container type";
            return ErrorCode::InternalError;
        }
    }

    if (errorCode == ErrorCode::NoError) {
        const auto server = m_settings->server(serverIndex);
        QJsonArray containers = server.value(config_key::containers).toArray();
        for (auto i = 0; i < containers.size(); i++) {
            auto containerConfig = containers.at(i).toObject();
            auto containerType = ContainerProps::containerFromString(containerConfig.value(config_key::container).toString());
            if (containerType == container) {
                QJsonObject protocolConfig;
                if (container == DockerContainer::ShadowSocks || container == DockerContainer::Cloak) {
                    protocolConfig = containerConfig.value(ContainerProps::containerTypeToString(DockerContainer::OpenVpn)).toObject();
                } else {
                    protocolConfig = containerConfig.value(ContainerProps::containerTypeToString(containerType)).toObject();
                }

                if (protocolConfig.value(config_key::last_config).toString().contains(clientId)) {
                    emit adminConfigRevoked(container);
                }
            }
        }
    }

    return errorCode;
}

ErrorCode ClientManagementModel::revokeClient(const QJsonObject &containerConfig, const DockerContainer container,
                                              const ServerCredentials &credentials, const int serverIndex,
                                              const QSharedPointer<ServerController> &serverController)
{
    ErrorCode errorCode = ErrorCode::NoError;
    errorCode = updateModel(container, credentials, serverController);
    if (errorCode != ErrorCode::NoError) {
        return errorCode;
    }

    Proto protocol;

    switch(container)
    {
        case DockerContainer::ShadowSocks:
        case DockerContainer::Cloak: {
            protocol = Proto::OpenVpn;
            break;
        }
        case DockerContainer::OpenVpn:
        case DockerContainer::WireGuard:
        case DockerContainer::Awg2:
        case DockerContainer::Awg:
        case DockerContainer::Xray: {
            protocol = ContainerProps::defaultProtocol(container);
            break;
        }
        default: {
            logger.error() << "Internal error: received unexpected container type";
            return ErrorCode::InternalError;
        }
    }

    auto protocolConfig = ContainerProps::getProtocolConfigFromContainer(protocol, containerConfig);

    QString clientId;
    if (container == DockerContainer::Xray) {
        if (!protocolConfig.contains("outbounds")) {
            return ErrorCode::InternalError;
        }
        QJsonArray outbounds = protocolConfig.value("outbounds").toArray();
        if (outbounds.isEmpty()) {
            return ErrorCode::InternalError;
        }
        QJsonObject outbound = outbounds[0].toObject();
        if (!outbound.contains("settings")) {
            return ErrorCode::InternalError;
        }
        QJsonObject settings = outbound["settings"].toObject();
        if (!settings.contains("vnext")) {
            return ErrorCode::InternalError;
        }
        QJsonArray vnext = settings["vnext"].toArray();
        if (vnext.isEmpty()) {
            return ErrorCode::InternalError;
        }
        QJsonObject vnextObj = vnext[0].toObject();
        if (!vnextObj.contains("users")) {
            return ErrorCode::InternalError;
        }
        QJsonArray users = vnextObj["users"].toArray();
        if (users.isEmpty()) {
            return ErrorCode::InternalError;
        }
        QJsonObject user = users[0].toObject();
        if (!user.contains("id")) {
            return ErrorCode::InternalError;
        }
        clientId = user["id"].toString();
    } else {
        clientId = protocolConfig.value(config_key::clientId).toString();
    }

    int row;
    bool clientExists = false;
    for (row = 0; row < rowCount(); row++) {
        auto client = m_clientsTable.at(row).toObject();
        if (clientId == client.value(configKey::clientId).toString()) {
            clientExists = true;
            break;
        }
    }
    if (!clientExists) {
        return errorCode;
    }

    switch (container)
    {
    case DockerContainer::OpenVpn:
    case DockerContainer::ShadowSocks:
    case DockerContainer::Cloak: {
        errorCode = revokeOpenVpn(row, container, credentials, serverIndex, serverController);
        break;
    }
    case DockerContainer::WireGuard:
    case DockerContainer::Awg:
    case DockerContainer::Awg2: {
        errorCode = revokeWireGuard(row, container, credentials, serverController);
        break;
    }
    case DockerContainer::Xray: {
        errorCode = revokeXray(row, container, credentials, serverController);
        break;
    }
    default:
        logger.error() << "Internal error: received unexpected container type";
        return ErrorCode::InternalError;
    }

    return errorCode;
}

ErrorCode ClientManagementModel::revokeOpenVpn(const int row, const DockerContainer container, const ServerCredentials &credentials,
                                               const int serverIndex, const QSharedPointer<ServerController> &serverController)
{
    auto client = m_clientsTable.at(row).toObject();
    QString clientId = client.value(configKey::clientId).toString();

    const QString getOpenVpnCertData = QString("sudo docker exec $CONTAINER_NAME bash -c '"
                                               "cd /opt/amnezia/openvpn ;\\"
                                               "easyrsa revoke %1 ;\\"
                                               "easyrsa gen-crl ;\\"
                                               "chmod 666 pki/crl.pem ;\\"
                                               "cp pki/crl.pem .'")
                                               .arg(clientId);

    const QString script = serverController->replaceVars(getOpenVpnCertData, serverController->genVarsForScript(credentials, container));
    ErrorCode error = serverController->runScript(credentials, script);
    if (error != ErrorCode::NoError) {
        logger.error() << "Failed to revoke the certificate";
        return error;
    }

    beginRemoveRows(QModelIndex(), row, row);
    m_clientsTable.removeAt(row);
    endRemoveRows();

    const QByteArray clientsTableString = QJsonDocument(m_clientsTable).toJson();

    QString clientsTableFile = QString("/opt/amnezia/%1/clientsTable");
    clientsTableFile = clientsTableFile.arg(ContainerProps::containerTypeToString(DockerContainer::OpenVpn));
    error = serverController->uploadTextFileToContainer(container, credentials, clientsTableString, clientsTableFile);
    if (error != ErrorCode::NoError) {
        logger.error() << "Failed to upload the clientsTable file to the server";
        return error;
    }

    return ErrorCode::NoError;
}

ErrorCode ClientManagementModel::revokeWireGuard(const int row, const DockerContainer container, const ServerCredentials &credentials,
                                                 const QSharedPointer<ServerController> &serverController)
{
    ErrorCode error = ErrorCode::NoError;

    QString configPath;
    if (container == DockerContainer::Awg) {
        configPath = QString::fromLatin1(amnezia::protocols::awg::serverLegacyConfigPath);
    } else if (container == DockerContainer::Awg2) {
        configPath = QString::fromLatin1(amnezia::protocols::awg::serverConfigPath);
    } else {
        configPath = QString::fromLatin1(amnezia::protocols::wireguard::serverConfigPath);
    }
    const QString wireguardConfigString = serverController->getTextFileFromContainer(container, credentials, configPath, error);
    if (error != ErrorCode::NoError) {
        logger.error() << "Failed to get the wg conf file from the server";
        return error;
    }

    auto client = m_clientsTable.at(row).toObject();
    QString clientId = client.value(configKey::clientId).toString();

    auto configSections = wireguardConfigString.split("[", Qt::SkipEmptyParts);
    for (auto &section : configSections) {
        if (section.contains(clientId)) {
            configSections.removeOne(section);
            break;
        }
    }
    QString newWireGuardConfig = configSections.join("[");
    newWireGuardConfig.insert(0, "[");
    error = serverController->uploadTextFileToContainer(container, credentials, newWireGuardConfig, configPath);
    if (error != ErrorCode::NoError) {
        logger.error() << "Failed to upload the wg conf file to the server";
        return error;
    }

    beginRemoveRows(QModelIndex(), row, row);
    m_clientsTable.removeAt(row);
    endRemoveRows();

    const QByteArray clientsTableString = QJsonDocument(m_clientsTable).toJson();

    const QString clientsTableFile = clientsTableFilePath(container);
    error = serverController->uploadTextFileToContainer(container, credentials, clientsTableString, clientsTableFile);
    if (error != ErrorCode::NoError) {
        logger.error() << "Failed to upload the clientsTable file to the server";
        return error;
    }

    bool isAwg = (container == DockerContainer::Awg2);
    QString command = isAwg ? QStringLiteral("awg") : QStringLiteral("wg");
    QString iface   = isAwg ? QStringLiteral("awg0") : QStringLiteral("wg0");
    QString script  = QString(
        "sudo docker exec $CONTAINER_NAME bash -c '%1 syncconf %2 <(%1-quick strip %3)'"
    ).arg(command, iface, configPath);
    error = serverController->runScript(
        credentials,
        serverController->replaceVars(script, serverController->genVarsForScript(credentials, container))
    );
    if (error != ErrorCode::NoError) {
        logger.error() << QString("Failed to execute command '%1 syncconf %2' on the server").arg(command, iface);
        return error;
    }

    return ErrorCode::NoError;
}

ErrorCode ClientManagementModel::revokeXray(const int row,
                                            const DockerContainer container,
                                            const ServerCredentials &credentials,
                                            const QSharedPointer<ServerController> &serverController)
{
    ErrorCode error = ErrorCode::NoError;

    // Get server config
    const QString serverConfigPath = amnezia::protocols::xray::serverConfigPath;
    const QString configString = serverController->getTextFileFromContainer(container, credentials, serverConfigPath, error);
    if (error != ErrorCode::NoError) {
        logger.error() << "Failed to get the xray server config file";
        return error;
    }

    QJsonDocument serverConfig = QJsonDocument::fromJson(configString.toUtf8());
    if (serverConfig.isNull()) {
        logger.error() << "Failed to parse xray server config JSON";
        return ErrorCode::InternalError;
    }

    // Get client ID to remove
    auto client = m_clientsTable.at(row).toObject();
    QString clientId = client.value(configKey::clientId).toString();

    // Remove client from server config
    QJsonObject configObj = serverConfig.object();
    if (!configObj.contains("inbounds")) {
        logger.error() << "Missing inbounds in xray config";
        return ErrorCode::InternalError;
    }

    QJsonArray inbounds = configObj["inbounds"].toArray();
    if (inbounds.isEmpty()) {
        logger.error() << "Empty inbounds array in xray config";
        return ErrorCode::InternalError;
    }

    QJsonObject inbound = inbounds[0].toObject();
    if (!inbound.contains("settings")) {
        logger.error() << "Missing settings in xray inbound config";
        return ErrorCode::InternalError;
    }

    QJsonObject settings = inbound["settings"].toObject();
    if (!settings.contains("clients")) {
        logger.error() << "Missing clients in xray settings";
        return ErrorCode::InternalError;
    }

    QJsonArray clients = settings["clients"].toArray();
    if (clients.isEmpty()) {
        logger.error() << "Empty clients array in xray config";
        return ErrorCode::InternalError;
    }

    for (int i = 0; i < clients.size(); ++i) {
        QJsonObject clientObj = clients[i].toObject();
        if (clientObj.contains("id") && clientObj["id"].toString() == clientId) {
            clients.removeAt(i);
            break;
        }
    }

    // Update server config
    settings["clients"] = clients;
    inbound["settings"] = settings;
    inbounds[0] = inbound;
    configObj["inbounds"] = inbounds;

    // Upload updated config
    error = serverController->uploadTextFileToContainer(
        container, 
        credentials,
        QJsonDocument(configObj).toJson(),
        serverConfigPath
    );
    if (error != ErrorCode::NoError) {
        logger.error() << "Failed to upload updated xray config";
        return error;
    }

    // Remove from local table
    beginRemoveRows(QModelIndex(), row, row);
    m_clientsTable.removeAt(row);
    endRemoveRows();

    // Update clients table file on server
    const QByteArray clientsTableString = QJsonDocument(m_clientsTable).toJson();
    QString clientsTableFile = QString("/opt/amnezia/%1/clientsTable")
        .arg(ContainerProps::containerTypeToString(container));

    error = serverController->uploadTextFileToContainer(container, credentials, clientsTableString, clientsTableFile);
    if (error != ErrorCode::NoError) {
        logger.error() << "Failed to upload the clientsTable file";
    }

    // Restart container
    QString restartScript = QString("sudo docker restart $CONTAINER_NAME");
    error = serverController->runScript(
        credentials, 
        serverController->replaceVars(restartScript, serverController->genVarsForScript(credentials, container))
    );
    if (error != ErrorCode::NoError) {
        logger.error() << "Failed to restart xray container";
        return error;
    }

    return error;
}

QHash<int, QByteArray> ClientManagementModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[ClientNameRole] = "clientName";
    roles[CreationDateRole] = "creationDate";
    roles[LatestHandshakeRole] = "latestHandshake";
    roles[DataReceivedRole] = "dataReceived";
    roles[DataSentRole] = "dataSent";
    roles[AllowedIpsRole] = "allowedIps";
    roles[ClientIdRole] = "clientId";
    roles[IsOnlineRole] = "isOnline";
    roles[LatestActivityRole] = "latestActivity";
    roles[LatestClientIpRole] = "latestClientIp";
    roles[LatestDestinationRole] = "latestDestination";
    roles[VisitHistoryRole] = "visitHistory";
    roles[SearchTextRole] = "searchText";
    return roles;
}

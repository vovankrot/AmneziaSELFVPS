#include "settings.h"

#include "QCoreApplication"
#include "QThread"

#include <algorithm>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include "core/networkUtilities.h"
#include "version.h"

#include "containers/containers_defs.h"
#include "logger.h"

namespace
{
    const char cloudFlareNs1[] = "1.1.1.1";
    const char cloudFlareNs2[] = "1.0.0.1";
    constexpr char siteRulesKey[] = "Conf/siteRules";

    constexpr char gatewayEndpoint[] = "http://gw.amnezia.org:80/";

    QString normalizedExecutablePath(const QString &rawPath)
    {
        const QString cleanedPath = QDir::fromNativeSeparators(QDir::cleanPath(rawPath.trimmed()));
        if (cleanedPath.isEmpty()) {
            return QString();
        }

        QFileInfo fileInfo(cleanedPath);
        if (!fileInfo.exists() || !fileInfo.isFile()) {
            return QString();
        }

#ifdef Q_OS_WIN
        if (fileInfo.suffix().compare("exe", Qt::CaseInsensitive) != 0) {
            return QString();
        }
#endif

        const QString canonicalPath = fileInfo.canonicalFilePath();
        return QDir::fromNativeSeparators(canonicalPath.isEmpty() ? fileInfo.absoluteFilePath() : canonicalPath);
    }

    QString normalizedSiteRuleKey(const QString &hostname)
    {
        return hostname.trimmed().toLower();
    }

    QVector<Settings::SiteSplitRule> normalizeSiteSplitRules(const QVector<Settings::SiteSplitRule> &inputRules)
    {
        QVector<Settings::SiteSplitRule> normalizedRules;
        normalizedRules.reserve(inputRules.size());

        QHash<QString, int> indexByKey;

        for (int index = inputRules.size() - 1; index >= 0; --index) {
            Settings::SiteSplitRule rule = inputRules.at(index);
            rule.hostname = rule.hostname.trimmed();
            rule.ip = rule.ip.trimmed();

            if (rule.hostname.isEmpty()) {
                continue;
            }

            const QString lookupKey = normalizedSiteRuleKey(rule.hostname);
            const auto existingIt = indexByKey.constFind(lookupKey);
            if (existingIt != indexByKey.constEnd()) {
                auto &newerRule = normalizedRules[existingIt.value()];
                if (newerRule.ip.isEmpty() && !rule.ip.isEmpty()) {
                    newerRule.ip = rule.ip;
                }
                continue;
            }

            indexByKey.insert(lookupKey, normalizedRules.size());
            normalizedRules.append(rule);
        }

        std::reverse(normalizedRules.begin(), normalizedRules.end());
        return normalizedRules;
    }

    QVariantList serializeSiteSplitRules(const QVector<Settings::SiteSplitRule> &rules)
    {
        QVariantList serializedRules;
        serializedRules.reserve(rules.size());

        for (const auto &rule : rules) {
            QVariantMap serializedRule;
            serializedRule.insert("hostname", rule.hostname);
            serializedRule.insert("ip", rule.ip);
            serializedRule.insert("useVpn", rule.useVpn);
            serializedRules.append(serializedRule);
        }

        return serializedRules;
    }

    QVector<Settings::SiteSplitRule> deserializeSiteSplitRules(const QVariant &rawValue)
    {
        QVector<Settings::SiteSplitRule> rules;

        const QVariantList serializedRules = rawValue.toList();
        rules.reserve(serializedRules.size());

        for (const auto &serializedRule : serializedRules) {
            const QVariantMap ruleMap = serializedRule.toMap();
            Settings::SiteSplitRule rule;
            rule.hostname = ruleMap.value("hostname").toString();
            rule.ip = ruleMap.value("ip").toString();
            rule.useVpn = ruleMap.value("useVpn").toBool();
            rules.append(rule);
        }

        return normalizeSiteSplitRules(rules);
    }
}

Settings::Settings(QObject *parent) : QObject(parent), m_settings(ORGANIZATION_NAME, APPLICATION_NAME, this)
{
    // Import old settings
    if (serversCount() == 0) {
        QString user = m_settings.value("Server/userName").toString();
        QString password = m_settings.value("Server/password").toString();
        QString serverName = m_settings.value("Server/serverName").toString();
        int port = m_settings.value("Server/serverPort").toInt();

        if (!user.isEmpty() && !password.isEmpty() && !serverName.isEmpty()) {
            QJsonObject server;
            server.insert(config_key::userName, user);
            server.insert(config_key::password, password);
            server.insert(config_key::hostName, serverName);
            server.insert(config_key::port, port);
            server.insert(config_key::description, tr("Server #1"));

            addServer(server);

            m_settings.remove("Server/userName");
            m_settings.remove("Server/password");
            m_settings.remove("Server/serverName");
            m_settings.remove("Server/serverPort");
        }
    }

    m_gatewayEndpoint = gatewayEndpoint;
}

int Settings::serversCount() const
{
    return serversArray().size();
}

QJsonObject Settings::server(int index) const
{
    const QJsonArray &servers = serversArray();
    if (index >= servers.size())
        return QJsonObject();

    return servers.at(index).toObject();
}

void Settings::addServer(const QJsonObject &server)
{
    QJsonArray servers = serversArray();
    servers.append(server);
    setServersArray(servers);
}

void Settings::removeServer(int index)
{
    QJsonArray servers = serversArray();
    if (index >= servers.size())
        return;

    servers.removeAt(index);
    setServersArray(servers);
    emit serverRemoved(index);
}

bool Settings::editServer(int index, const QJsonObject &server)
{
    QJsonArray servers = serversArray();
    if (index >= servers.size())
        return false;

    servers.replace(index, server);
    setServersArray(servers);
    return true;
}

void Settings::setDefaultContainer(int serverIndex, DockerContainer container)
{
    QJsonObject s = server(serverIndex);
    s.insert(config_key::defaultContainer, ContainerProps::containerToString(container));
    editServer(serverIndex, s);
}

DockerContainer Settings::defaultContainer(int serverIndex) const
{
    return ContainerProps::containerFromString(defaultContainerName(serverIndex));
}

QString Settings::defaultContainerName(int serverIndex) const
{
    QString name = server(serverIndex).value(config_key::defaultContainer).toString();
    if (name.isEmpty()) {
        return ContainerProps::containerToString(DockerContainer::None);
    } else
        return name;
}

QMap<DockerContainer, QJsonObject> Settings::containers(int serverIndex) const
{
    const QJsonArray &containers = server(serverIndex).value(config_key::containers).toArray();

    QMap<DockerContainer, QJsonObject> containersMap;
    for (const QJsonValue &val : containers) {
        containersMap.insert(ContainerProps::containerFromString(val.toObject().value(config_key::container).toString()), val.toObject());
    }

    return containersMap;
}

void Settings::setContainers(int serverIndex, const QMap<DockerContainer, QJsonObject> &containers)
{
    QJsonObject s = server(serverIndex);
    QJsonArray c;
    for (const QJsonObject &o : containers) {
        c.append(o);
    }
    s.insert(config_key::containers, c);
    editServer(serverIndex, s);
}

QJsonObject Settings::containerConfig(int serverIndex, DockerContainer container)
{
    if (container == DockerContainer::None)
        return QJsonObject();
    return containers(serverIndex).value(container);
}

void Settings::setContainerConfig(int serverIndex, DockerContainer container, const QJsonObject &config)
{
    if (container == DockerContainer::None) {
        qCritical() << "Settings::setContainerConfig trying to set config for container == DockerContainer::None";
        return;
    }
    auto c = containers(serverIndex);
    c[container] = config;
    c[container][config_key::container] = ContainerProps::containerToString(container);
    setContainers(serverIndex, c);
}

void Settings::removeContainerConfig(int serverIndex, DockerContainer container)
{
    if (container == DockerContainer::None) {
        qCritical() << "Settings::removeContainerConfig trying to remove config for container == DockerContainer::None";
        return;
    }

    auto c = containers(serverIndex);
    c.remove(container);
    setContainers(serverIndex, c);
}

QJsonObject Settings::protocolConfig(int serverIndex, DockerContainer container, Proto proto)
{
    const QJsonObject &c = containerConfig(serverIndex, container);
    return c.value(ProtocolProps::protoToString(proto)).toObject();
}

void Settings::setProtocolConfig(int serverIndex, DockerContainer container, Proto proto, const QJsonObject &config)
{
    QJsonObject c = containerConfig(serverIndex, container);
    c.insert(ProtocolProps::protoToString(proto), config);

    setContainerConfig(serverIndex, container, c);
}

void Settings::clearLastConnectionConfig(int serverIndex, DockerContainer container, Proto proto)
{
    // recursively remove
    if (proto == Proto::Any) {
        for (Proto p : ContainerProps::protocolsForContainer(container)) {
            clearLastConnectionConfig(serverIndex, container, p);
        }
        return;
    }

    QJsonObject c = protocolConfig(serverIndex, container, proto);
    c.remove(config_key::last_config);
    setProtocolConfig(serverIndex, container, proto, c);
}

bool Settings::haveAuthData(int serverIndex) const
{
    if (serverIndex < 0)
        return false;
    ServerCredentials cred = serverCredentials(serverIndex);
    return (!cred.hostName.isEmpty() && !cred.userName.isEmpty() && !cred.secretData.isEmpty());
}

QString Settings::nextAvailableServerName() const
{
    int i = 0;
    bool nameExist = false;

    do {
        i++;
        nameExist = false;
        for (const QJsonValue &server : serversArray()) {
            if (server.toObject().value(config_key::description).toString() == tr("Server") + " " + QString::number(i)) {
                nameExist = true;
                break;
            }
        }
    } while (nameExist);

    return tr("Server") + " " + QString::number(i);
}

void Settings::setSaveLogs(bool enabled)
{
    m_settings.setValue("Conf/saveLogs", enabled);
    // Client file logging is always on; only service logs are toggled
    Logger::setServiceLogsEnabled(enabled);

    if (enabled) {
        setLogEnableDate(QDateTime::currentDateTime());
    }
    emit saveLogsChanged(enabled);
}

QDateTime Settings::getLogEnableDate()
{
    return m_settings.value("Conf/logEnableDate").toDateTime();
}

void Settings::setLogEnableDate(QDateTime date)
{
    m_settings.setValue("Conf/logEnableDate", date);
}

QString Settings::routeModeString(RouteMode mode) const
{
    switch (mode) {
    case VpnAllSites: return "AllSites";
    case VpnOnlyForwardSites: return "ForwardSites";
    case VpnAllExceptSites: return "ExceptSites";
    }
}

Settings::RouteMode Settings::routeMode() const
{
    return static_cast<RouteMode>(m_settings.value("Conf/routeMode", 0).toInt());
}

bool Settings::isSitesSplitTunnelingEnabled() const
{
    return m_settings.value("Conf/sitesSplitTunnelingEnabled", false).toBool();
}

void Settings::setSitesSplitTunnelingEnabled(bool enabled)
{
    m_settings.setValue("Conf/sitesSplitTunnelingEnabled", enabled);
}

QVector<Settings::SiteSplitRule> Settings::getVpnSiteRules() const
{
    const QVariant storedRulesValue = m_settings.value(siteRulesKey);
    if (storedRulesValue.isValid()) {
        return deserializeSiteSplitRules(storedRulesValue);
    }

    QVector<SiteSplitRule> rules;

    const auto appendLegacySites = [this, &rules](RouteMode mode, bool useVpn) {
        const QVariantMap storedSites = m_settings.value("Conf/" + routeModeString(mode)).toMap();
        for (auto it = storedSites.constBegin(); it != storedSites.constEnd(); ++it) {
            SiteSplitRule rule;
            rule.hostname = it.key();
            rule.ip = it.value().toString();
            rule.useVpn = useVpn;
            rules.append(rule);
        }
    };

    if (routeMode() == VpnOnlyForwardSites) {
        appendLegacySites(VpnAllExceptSites, false);
        appendLegacySites(VpnOnlyForwardSites, true);
    } else {
        appendLegacySites(VpnOnlyForwardSites, true);
        appendLegacySites(VpnAllExceptSites, false);
    }

    return normalizeSiteSplitRules(rules);
}

void Settings::setVpnSiteRules(const QVector<SiteSplitRule> &rules)
{
    const QVector<SiteSplitRule> normalizedRules = normalizeSiteSplitRules(rules);
    m_settings.setValue(siteRulesKey, serializeSiteSplitRules(normalizedRules));
    m_settings.remove("Conf/ForwardSites");
    m_settings.remove("Conf/ExceptSites");
}

QVariantMap Settings::vpnSites(RouteMode mode) const
{
    if (mode == VpnAllSites) {
        return {};
    }

    QVariantMap filteredRules;
    const bool useVpn = mode == VpnOnlyForwardSites;

    for (const auto &rule : getVpnSiteRules()) {
        if (rule.useVpn == useVpn) {
            filteredRules.insert(rule.hostname, rule.ip);
        }
    }

    return filteredRules;
}

void Settings::setVpnSites(RouteMode mode, const QVariantMap &sites)
{
    if (mode == VpnAllSites) {
        return;
    }

    const QVector<SiteSplitRule> currentRules = getVpnSiteRules();
    QVector<SiteSplitRule> updatedRules;
    updatedRules.reserve(currentRules.size() + sites.size());

    const bool useVpn = mode == VpnOnlyForwardSites;

    for (const auto &rule : currentRules) {
        if (rule.useVpn != useVpn) {
            updatedRules.append(rule);
        }
    }

    for (auto it = sites.constBegin(); it != sites.constEnd(); ++it) {
        SiteSplitRule rule;
        rule.hostname = it.key();
        rule.ip = it.value().toString();
        rule.useVpn = useVpn;
        updatedRules.append(rule);
    }

    setVpnSiteRules(updatedRules);
}

bool Settings::addVpnSite(RouteMode mode, const QString &site, const QString &ip)
{
    if (mode == VpnAllSites) {
        return false;
    }

    QVector<SiteSplitRule> rules = getVpnSiteRules();
    const QString lookupKey = normalizedSiteRuleKey(site);
    const bool useVpn = mode == VpnOnlyForwardSites;

    for (auto &rule : rules) {
        if (normalizedSiteRuleKey(rule.hostname) != lookupKey) {
            continue;
        }

        const bool hasModeChanged = rule.useVpn != useVpn;
        const bool hasIpChanged = !ip.isEmpty() && rule.ip != ip;
        if (!hasModeChanged && !hasIpChanged) {
            return false;
        }

        rule.useVpn = useVpn;
        if (hasIpChanged) {
            rule.ip = ip;
        }

        setVpnSiteRules(rules);
        return true;
    }

    SiteSplitRule rule;
    rule.hostname = site;
    rule.ip = ip;
    rule.useVpn = useVpn;
    rules.append(rule);
    setVpnSiteRules(rules);
    return true;
}

void Settings::addVpnSites(RouteMode mode, const QMap<QString, QString> &sites)
{
    if (mode == VpnAllSites) {
        return;
    }

    QVector<SiteSplitRule> rules = getVpnSiteRules();
    const bool useVpn = mode == VpnOnlyForwardSites;

    for (auto it = sites.constBegin(); it != sites.constEnd(); ++it) {
        const QString lookupKey = normalizedSiteRuleKey(it.key());
        bool updatedExistingRule = false;

        for (auto &rule : rules) {
            if (normalizedSiteRuleKey(rule.hostname) != lookupKey) {
                continue;
            }

            rule.useVpn = useVpn;
            rule.ip = it.value();
            updatedExistingRule = true;
            break;
        }

        if (!updatedExistingRule) {
            SiteSplitRule rule;
            rule.hostname = it.key();
            rule.ip = it.value();
            rule.useVpn = useVpn;
            rules.append(rule);
        }
    }

    setVpnSiteRules(rules);
}

QStringList Settings::getVpnIps(RouteMode mode) const
{
    QStringList ips;
    const QVariantMap &m = vpnSites(mode);
    for (auto i = m.constBegin(); i != m.constEnd(); ++i) {
        if (NetworkUtilities::checkIpSubnetFormat(i.key())) {
            ips.append(i.key());
        } else if (NetworkUtilities::checkIpSubnetFormat(i.value().toString())) {
            ips.append(i.value().toString());
        }
    }
    ips.removeDuplicates();
    return ips;
}

void Settings::removeVpnSite(RouteMode mode, const QString &site)
{
    if (mode == VpnAllSites) {
        return;
    }

    QVector<SiteSplitRule> rules = getVpnSiteRules();
    const QString lookupKey = normalizedSiteRuleKey(site);
    const bool useVpn = mode == VpnOnlyForwardSites;

    bool removed = false;
    for (int index = rules.size() - 1; index >= 0; --index) {
        if (normalizedSiteRuleKey(rules[index].hostname) == lookupKey
            && rules[index].useVpn == useVpn) {
            rules.removeAt(index);
            removed = true;
        }
    }

    if (removed) {
        setVpnSiteRules(rules);
    }
}

void Settings::addVpnIps(RouteMode mode, const QStringList &ips)
{
    if (mode == VpnAllSites) {
        return;
    }

    QVector<SiteSplitRule> rules = getVpnSiteRules();
    const bool useVpn = mode == VpnOnlyForwardSites;

    for (const QString &ip : ips) {
        if (ip.isEmpty()) {
            continue;
        }

        const QString lookupKey = normalizedSiteRuleKey(ip);
        bool updatedExistingRule = false;

        for (auto &rule : rules) {
            if (normalizedSiteRuleKey(rule.hostname) != lookupKey) {
                continue;
            }

            rule.useVpn = useVpn;
            updatedExistingRule = true;
            break;
        }

        if (!updatedExistingRule) {
            SiteSplitRule rule;
            rule.hostname = ip;
            rule.useVpn = useVpn;
            rules.append(rule);
        }
    }

    setVpnSiteRules(rules);
}

void Settings::removeVpnSites(RouteMode mode, const QStringList &sites)
{
    if (mode == VpnAllSites) {
        return;
    }

    QVector<SiteSplitRule> rules = getVpnSiteRules();
    const bool useVpn = mode == VpnOnlyForwardSites;
    QSet<QString> lookupKeys;

    for (const QString &site : sites) {
        if (!site.isEmpty()) {
            lookupKeys.insert(normalizedSiteRuleKey(site));
        }
    }

    if (lookupKeys.isEmpty()) {
        return;
    }

    bool removed = false;
    for (int index = rules.size() - 1; index >= 0; --index) {
        if (rules[index].useVpn == useVpn
            && lookupKeys.contains(normalizedSiteRuleKey(rules[index].hostname))) {
            rules.removeAt(index);
            removed = true;
        }
    }

    if (removed) {
        setVpnSiteRules(rules);
    }
}

void Settings::removeAllVpnSites(RouteMode mode)
{
    if (mode == VpnAllSites) {
        return;
    }

    QVector<SiteSplitRule> rules = getVpnSiteRules();
    const bool useVpn = mode == VpnOnlyForwardSites;

    rules.erase(std::remove_if(rules.begin(), rules.end(), [useVpn](const SiteSplitRule &rule) {
        return rule.useVpn == useVpn;
    }), rules.end());

    setVpnSiteRules(rules);
}

QString Settings::primaryDns() const
{
    return m_settings.value("Conf/primaryDns", cloudFlareNs1).toString();
}

QString Settings::secondaryDns() const
{
    return m_settings.value("Conf/secondaryDns", cloudFlareNs2).toString();
}

void Settings::clearSettings()
{
    auto uuid = getInstallationUuid(false);
    m_settings.clearSettings();
    setInstallationUuid(uuid);
    emit settingsCleared();
}

QString Settings::appsRouteModeString(AppsRouteMode mode) const
{
    switch (mode) {
    case VpnAllApps: return "AllApps";
    case VpnOnlyForwardApps: return "ForwardApps";
    case VpnAllExceptApps: return "ExceptApps";
    }
}

Settings::AppsRouteMode Settings::getAppsRouteMode() const
{
    return static_cast<AppsRouteMode>(m_settings.value("Conf/appsRouteMode", 0).toInt());
}

void Settings::setAppsRouteMode(AppsRouteMode mode)
{
    m_settings.setValue("Conf/appsRouteMode", mode);
}

QVector<InstalledAppInfo> Settings::getVpnApps(AppsRouteMode mode) const
{
    QVector<InstalledAppInfo> apps;
    QSet<QString> seenEntries;
    auto appsArray = m_settings.value("Conf/" + appsRouteModeString(mode)).toJsonArray();
    for (const auto &app : appsArray) {
        InstalledAppInfo appInfo;
        appInfo.appName = app.toObject().value("appName").toString();
        appInfo.packageName = app.toObject().value("packageName").toString();
        appInfo.appPath = app.toObject().value("appPath").toString();
        appInfo.groupFolder = app.toObject().value("groupFolder").toString();

        if (!appInfo.packageName.isEmpty()) {
            const QString dedupeKey = QString("pkg:%1").arg(appInfo.packageName);
            if (seenEntries.contains(dedupeKey)) {
                continue;
            }

            seenEntries.insert(dedupeKey);
            apps.push_back(appInfo);
            continue;
        }

        appInfo.appPath = normalizedExecutablePath(appInfo.appPath);
        if (appInfo.appPath.isEmpty()) {
            continue;
        }

        if (appInfo.appName.isEmpty()) {
            appInfo.appName = QFileInfo(appInfo.appPath).fileName();
        }

        const QString dedupeKey = QString("path:%1").arg(appInfo.appPath.toLower());
        if (seenEntries.contains(dedupeKey)) {
            continue;
        }

        seenEntries.insert(dedupeKey);

        apps.push_back(appInfo);
    }
    return apps;
}

void Settings::setVpnApps(AppsRouteMode mode, const QVector<InstalledAppInfo> &apps)
{
    QJsonArray appsArray;
    for (const auto &app : apps) {
        QJsonObject appInfo;
        appInfo.insert("appName", app.appName);
        appInfo.insert("packageName", app.packageName);
        appInfo.insert("appPath", app.appPath);
        appInfo.insert("groupFolder", app.groupFolder);
        appsArray.push_back(appInfo);
    }
    m_settings.setValue("Conf/" + appsRouteModeString(mode), appsArray);
}

bool Settings::isAppsSplitTunnelingEnabled() const
{
    return m_settings.value("Conf/appsSplitTunnelingEnabled", false).toBool();
}

void Settings::setAppsSplitTunnelingEnabled(bool enabled)
{
    m_settings.setValue("Conf/appsSplitTunnelingEnabled", enabled);
}

bool Settings::isKillSwitchEnabled() const
{
    return m_settings.value("Conf/killSwitchEnabled", true).toBool();
}

void Settings::setKillSwitchEnabled(bool enabled)
{
    m_settings.setValue("Conf/killSwitchEnabled", enabled);
}

bool Settings::isStrictKillSwitchEnabled() const
{
    return m_settings.value("Conf/strictKillSwitchEnabled", false).toBool();
}

void Settings::setStrictKillSwitchEnabled(bool enabled)
{
    m_settings.setValue("Conf/strictKillSwitchEnabled", enabled);
}

QString Settings::getInstallationUuid(const bool needCreate)
{
    auto uuid = m_settings.value("Conf/installationUuid", "").toString();
    if (needCreate && uuid.isEmpty()) {
        uuid = QUuid::createUuid().toString();

        //remove {} from uuid
        uuid.remove(0, 1);
        uuid.chop(1);

        setInstallationUuid(uuid);
    } else if (uuid.contains("{") && uuid.contains("}")) {
        //remove {} from old uuid
        uuid.remove(0, 1);
        uuid.chop(1);

        setInstallationUuid(uuid);
    }
    return uuid;
}

void Settings::setInstallationUuid(const QString &uuid)
{
    m_settings.setValue("Conf/installationUuid", uuid);
}

ServerCredentials Settings::defaultServerCredentials() const
{
    return serverCredentials(defaultServerIndex());
}

ServerCredentials Settings::serverCredentials(int index) const
{
    const QJsonObject &s = server(index);

    ServerCredentials credentials;
    credentials.hostName = s.value(config_key::hostName).toString();
    credentials.userName = s.value(config_key::userName).toString();
    credentials.secretData = s.value(config_key::password).toString();
    credentials.port = s.value(config_key::port).toInt();

    return credentials;
}

void Settings::resetGatewayEndpoint()
{
    m_gatewayEndpoint = gatewayEndpoint;
}

void Settings::setGatewayEndpoint(const QString &endpoint)
{
    m_gatewayEndpoint = endpoint;
}

void Settings::setDevGatewayEndpoint()
{
    m_gatewayEndpoint = DEV_AGW_ENDPOINT;
}

QString Settings::getGatewayEndpoint(bool isTestPurchase)
{
    return isTestPurchase ? DEV_AGW_ENDPOINT : m_gatewayEndpoint;
}

bool Settings::isDevGatewayEnv(bool isTestPurchase)
{
    return isTestPurchase ? true : m_settings.value("Conf/devGatewayEnv", false).toBool();
}

void Settings::toggleDevGatewayEnv(bool enabled)
{
    m_settings.setValue("Conf/devGatewayEnv", enabled);
}

bool Settings::isHomeAdLabelVisible()
{
    return m_settings.value("Conf/homeAdLabelVisible", true).toBool();
}

void Settings::disableHomeAdLabel()
{
    m_settings.setValue("Conf/homeAdLabelVisible", false);
}

bool Settings::isPremV1MigrationReminderActive()
{
    return m_settings.value("Conf/premV1MigrationReminderActive", true).toBool();
}

void Settings::disablePremV1MigrationReminder()
{
    m_settings.setValue("Conf/premV1MigrationReminderActive", false);
}

QStringList Settings::allowedDnsServers() const
{
    return m_settings.value("Conf/allowedDnsServers").toStringList();
}

void Settings::setAllowedDnsServers(const QStringList &servers)
{
    m_settings.setValue("Conf/allowedDnsServers", servers);
}

QStringList Settings::readNewsIds() const
{
    return m_settings.value("News/readIds").toStringList();
}

void Settings::setReadNewsIds(const QStringList &ids)
{
    m_settings.setValue("News/readIds", ids);
}

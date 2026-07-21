#ifndef SETTINGS_H
#define SETTINGS_H

#include <QObject>
#include <QSettings>
#include <QString>
#include <QDateTime>
#include <QVector>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "containers/containers_defs.h"
#include "core/defs.h"
#include "secure_qsettings.h"

using namespace amnezia;

class QSettings;

class Settings : public QObject
{
    Q_OBJECT

public:
    explicit Settings(QObject *parent = nullptr);

    ServerCredentials defaultServerCredentials() const;
    ServerCredentials serverCredentials(int index) const;

    QJsonArray serversArray() const
    {
        return QJsonDocument::fromJson(m_settings.value("Servers/serversList").toByteArray()).array();
    }
    void setServersArray(const QJsonArray &servers)
    {
        m_settings.setValue("Servers/serversList", QJsonDocument(servers).toJson());
    }

    // Servers section
    int serversCount() const;
    QJsonObject server(int index) const;
    void addServer(const QJsonObject &server);
    void removeServer(int index);
    bool editServer(int index, const QJsonObject &server);

    int defaultServerIndex() const
    {
        return m_settings.value("Servers/defaultServerIndex", 0).toInt();
    }
    void setDefaultServer(int index)
    {
        m_settings.setValue("Servers/defaultServerIndex", index);
    }
    QJsonObject defaultServer() const
    {
        return server(defaultServerIndex());
    }

    void setDefaultContainer(int serverIndex, DockerContainer container);
    DockerContainer defaultContainer(int serverIndex) const;
    QString defaultContainerName(int serverIndex) const;

    QMap<DockerContainer, QJsonObject> containers(int serverIndex) const;
    void setContainers(int serverIndex, const QMap<DockerContainer, QJsonObject> &containers);

    QJsonObject containerConfig(int serverIndex, DockerContainer container);
    void setContainerConfig(int serverIndex, DockerContainer container, const QJsonObject &config);
    void removeContainerConfig(int serverIndex, DockerContainer container);

    QJsonObject protocolConfig(int serverIndex, DockerContainer container, Proto proto);
    void setProtocolConfig(int serverIndex, DockerContainer container, Proto proto, const QJsonObject &config);

    void clearLastConnectionConfig(int serverIndex, DockerContainer container, Proto proto = Proto::Any);

    bool haveAuthData(int serverIndex) const;
    QString nextAvailableServerName() const;

    // App settings section
    bool isAutoConnect() const
    {
        return m_settings.value("Conf/autoConnect", false).toBool();
    }
    void setAutoConnect(bool enabled)
    {
        m_settings.setValue("Conf/autoConnect", enabled);
    }

    bool isStartMinimized() const
    {
        return m_settings.value("Conf/startMinimized", false).toBool();
    }
    void setStartMinimized(bool enabled)
    {
        m_settings.setValue("Conf/startMinimized", enabled);
    }

    bool isNewsNotifications() const
    {
        return m_settings.value("Conf/newsNotifications", true).toBool();
    }
    void setNewsNotifications(bool enabled)
    {
        m_settings.setValue("Conf/newsNotifications", enabled);
    }

    bool isSaveLogs() const
    {
        return m_settings.value("Conf/saveLogs", false).toBool();
    }
    void setSaveLogs(bool enabled);

    QDateTime getLogEnableDate();
    void setLogEnableDate(QDateTime date);

    enum RouteMode {
        VpnAllSites,
        VpnOnlyForwardSites,
        VpnAllExceptSites
    };
    Q_ENUM(RouteMode)

    struct SiteSplitRule {
        QString hostname;
        QString ip;
        bool useVpn = false;
    };

    QString routeModeString(RouteMode mode) const;

    RouteMode routeMode() const;
    void setRouteMode(RouteMode mode) { m_settings.setValue("Conf/routeMode", mode); }

    bool bypassRuSites() const
    {
        return bypassRuGeoSites() || bypassRuGeoIp();
    }
    void setBypassRuSites(bool enabled)
    {
        m_settings.setValue("Conf/bypassRuSites", enabled);
        m_settings.setValue("Conf/bypassRuGeoSites", enabled);
        m_settings.setValue("Conf/bypassRuGeoIp", enabled);
    }

    bool bypassRuGeoSites() const
    {
        const QVariant geoSitesValue = m_settings.value("Conf/bypassRuGeoSites");
        return geoSitesValue.isValid()
                   ? geoSitesValue.toBool()
                   : m_settings.value("Conf/bypassRuSites", true).toBool();
    }
    void setBypassRuGeoSites(bool enabled)
    {
        m_settings.setValue("Conf/bypassRuGeoSites", enabled);

        const QVariant geoIpValue = m_settings.value("Conf/bypassRuGeoIp");
        const bool geoIpEnabled = geoIpValue.isValid()
                                      ? geoIpValue.toBool()
                                      : m_settings.value("Conf/bypassRuSites", true).toBool();
        m_settings.setValue("Conf/bypassRuSites", enabled || geoIpEnabled);
    }

    bool bypassRuGeoIp() const
    {
        const QVariant geoIpValue = m_settings.value("Conf/bypassRuGeoIp");
        return geoIpValue.isValid()
                   ? geoIpValue.toBool()
                   : m_settings.value("Conf/bypassRuSites", true).toBool();
    }
    void setBypassRuGeoIp(bool enabled)
    {
        m_settings.setValue("Conf/bypassRuGeoIp", enabled);

        const QVariant geoSitesValue = m_settings.value("Conf/bypassRuGeoSites");
        const bool geoSitesEnabled = geoSitesValue.isValid()
                                         ? geoSitesValue.toBool()
                                         : m_settings.value("Conf/bypassRuSites", true).toBool();
        m_settings.setValue("Conf/bypassRuSites", enabled || geoSitesEnabled);
    }

    bool isAdvancedMode() const
    {
        return m_settings.value("Conf/advancedMode", false).toBool();
    }
    void setAdvancedMode(bool enabled)
    {
        m_settings.setValue("Conf/advancedMode", enabled);
    }

    bool isSitesSplitTunnelingEnabled() const;
    void setSitesSplitTunnelingEnabled(bool enabled);

    QVector<SiteSplitRule> getVpnSiteRules() const;
    void setVpnSiteRules(const QVector<SiteSplitRule> &rules);
    QVariantMap vpnSites(RouteMode mode) const;
    void setVpnSites(RouteMode mode, const QVariantMap &sites);
    bool addVpnSite(RouteMode mode, const QString &site, const QString &ip = "");
    void addVpnSites(RouteMode mode, const QMap<QString, QString> &sites); // map <site, ip>
    QStringList getVpnIps(RouteMode mode) const;
    void removeVpnSite(RouteMode mode, const QString &site);

    void addVpnIps(RouteMode mode, const QStringList &ip);
    void removeVpnSites(RouteMode mode, const QStringList &sites);
    void removeAllVpnSites(RouteMode mode);

    bool useAmneziaDns() const
    {
        return m_settings.value("Conf/useAmneziaDns", true).toBool();
    }
    void setUseAmneziaDns(bool enabled)
    {
        m_settings.setValue("Conf/useAmneziaDns", enabled);
    }

    QString primaryDns() const;
    QString secondaryDns() const;

    // QString primaryDns() const { return m_primaryDns; }
    void setPrimaryDns(const QString &primaryDns)
    {
        m_settings.setValue("Conf/primaryDns", primaryDns);
    }

    // QString secondaryDns() const { return m_secondaryDns; }
    void setSecondaryDns(const QString &secondaryDns)
    {
        m_settings.setValue("Conf/secondaryDns", secondaryDns);
    }

    //    static constexpr char openNicNs5[] = "94.103.153.176";
    //    static constexpr char openNicNs13[] = "144.76.103.143";

    QByteArray backupAppConfig() const
    {
        return m_settings.backupAppConfig();
    }
    bool restoreAppConfig(const QByteArray &cfg)
    {
        return m_settings.restoreAppConfig(cfg);
    }

    QLocale getAppLanguage()
    {
        QString localeStr = m_settings.value("Conf/appLanguage", QLocale::system().name()).toString();
        return QLocale(localeStr);
    };
    void setAppLanguage(QLocale locale)
    {
        m_settings.setValue("Conf/appLanguage", locale.name());
    };

    bool isScreenshotsEnabled() const
    {
        return m_settings.value("Conf/screenshotsEnabled", true).toBool();
    }
    void setScreenshotsEnabled(bool enabled)
    {
        m_settings.setValue("Conf/screenshotsEnabled", enabled);
        emit screenshotsEnabledChanged(enabled);
    }

    void clearSettings();

    enum AppsRouteMode {
        VpnAllApps,
        VpnOnlyForwardApps,
        VpnAllExceptApps
    };
    Q_ENUM(AppsRouteMode)

    QString appsRouteModeString(AppsRouteMode mode) const;

    AppsRouteMode getAppsRouteMode() const;
    void setAppsRouteMode(AppsRouteMode mode);

    QVector<InstalledAppInfo> getVpnApps(AppsRouteMode mode) const;
    void setVpnApps(AppsRouteMode mode, const QVector<InstalledAppInfo> &apps);

    bool isAppsSplitTunnelingEnabled() const;
    void setAppsSplitTunnelingEnabled(bool enabled);

    bool isKillSwitchEnabled() const;
    void setKillSwitchEnabled(bool enabled);

    bool isStrictKillSwitchEnabled() const;
    void setStrictKillSwitchEnabled(bool enabled);

    // Auto-failover (beta): automatically switch to fallback connection on degradation
    bool isAutoFailoverEnabled() const
    {
        return m_settings.value("Conf/autoFailover", false).toBool();
    }
    void setAutoFailoverEnabled(bool enabled)
    {
        m_settings.setValue("Conf/autoFailover", enabled);
    }

    QString getInstallationUuid(const bool needCreate);

    void resetGatewayEndpoint();
    void setGatewayEndpoint(const QString &endpoint);
    void setDevGatewayEndpoint();
    QString getGatewayEndpoint(bool isTestPurchase = false);
    bool isDevGatewayEnv(bool isTestPurchase = false);
    void toggleDevGatewayEnv(bool enabled);

    bool isHomeAdLabelVisible();
    void disableHomeAdLabel();

    bool isPremV1MigrationReminderActive();
    void disablePremV1MigrationReminder();
    
    QStringList allowedDnsServers() const;
    void setAllowedDnsServers(const QStringList &servers);

    QStringList readNewsIds() const;
    void setReadNewsIds(const QStringList &ids);

    QDateTime geoIpLastUpdate() const
    {
        return m_settings.value("GeoIp/lastUpdate").toDateTime();
    }
    void setGeoIpLastUpdate(const QDateTime &dt)
    {
        m_settings.setValue("GeoIp/lastUpdate", dt);
    }

    // ---- GeoIP list source / schedule: user-configurable, not hardcoded ----
    // The old default (herrbischoff/country-ip-blocks) now 404s, so every update silently
    // failed and the bundled snapshot was used forever. This one is regenerated daily from
    // RIR delegation data; verified 8629 RU subnets. by vovankrot
    static constexpr const char *defaultGeoIpSourceUrl =
        "https://raw.githubusercontent.com/ipverse/rir-ip/master/country/ru/ipv4-aggregated.txt";

    QString geoIpSourceUrl() const
    {
        const QString url = m_settings.value("GeoIp/sourceUrl").toString().trimmed();
        return url.isEmpty() ? QString::fromLatin1(defaultGeoIpSourceUrl) : url;
    }
    void setGeoIpSourceUrl(const QString &url)
    {
        m_settings.setValue("GeoIp/sourceUrl", url.trimmed());
    }

    // How often the list is refreshed. Minimum 1 hour.
    int geoIpUpdateIntervalHours() const
    {
        return qMax(1, m_settings.value("GeoIp/updateIntervalHours", 24).toInt());
    }
    void setGeoIpUpdateIntervalHours(int hours)
    {
        m_settings.setValue("GeoIp/updateIntervalHours", qMax(1, hours));
    }

    // Number of CIDRs in the last successfully downloaded list (0 = never downloaded).
    int geoIpLastCount() const
    {
        return m_settings.value("GeoIp/lastCount", 0).toInt();
    }
    void setGeoIpLastCount(int count)
    {
        m_settings.setValue("GeoIp/lastCount", count);
    }

    bool isAutoBypassRknEnabled() const
    {
        return m_settings.value("Conf/autoBypassRkn", false).toBool();
    }
    void setAutoBypassRknEnabled(bool enabled)
    {
        m_settings.setValue("Conf/autoBypassRkn", enabled);
    }

    QDateTime rknBlocklistLastUpdate() const
    {
        return m_settings.value("Rkn/blocklistLastUpdate").toDateTime();
    }
    void setRknBlocklistLastUpdate(const QDateTime &dt)
    {
        m_settings.setValue("Rkn/blocklistLastUpdate", dt);
    }

    bool autoDisableLoopbackProxyOnConnect() const
    {
        return m_settings.value("Conf/autoDisableLoopbackProxyOnConnect", true).toBool();
    }
    void setAutoDisableLoopbackProxyOnConnect(bool enabled)
    {
        m_settings.setValue("Conf/autoDisableLoopbackProxyOnConnect", enabled);
    }

signals:
    void saveLogsChanged(bool enabled);
    void screenshotsEnabledChanged(bool enabled);
    void serverRemoved(int serverIndex);
    void settingsCleared();

private:
    void setInstallationUuid(const QString &uuid);

    mutable SecureQSettings m_settings;

    QString m_gatewayEndpoint;
};

#endif // SETTINGS_H

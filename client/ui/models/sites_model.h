#ifndef SITESMODEL_H
#define SITESMODEL_H

#include <QAbstractListModel>

#include "settings.h"

class SitesModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        UrlRole = Qt::UserRole + 1,
        IpRole,
        UseVpnRole
    };

    struct SiteEntry {
        QString hostname;
        QString ip;
        bool useVpn = false;
    };

    explicit SitesModel(std::shared_ptr<Settings> settings, QObject *parent = nullptr);

    Q_INVOKABLE QString stateSignature() const;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    Q_PROPERTY(int routeMode READ getRouteMode WRITE setRouteMode NOTIFY routeModeChanged)
    Q_PROPERTY(bool isTunnelingEnabled READ isSplitTunnelingEnabled NOTIFY splitTunnelingToggled)
    Q_PROPERTY(bool bypassRuSites READ isBypassRuSites WRITE setBypassRuSites NOTIFY bypassRuSitesChanged)
    Q_PROPERTY(bool bypassRuGeoSites READ isBypassRuGeoSites WRITE setBypassRuGeoSites NOTIFY bypassRuGeoSitesChanged)
    Q_PROPERTY(bool bypassRuGeoIp READ isBypassRuGeoIp WRITE setBypassRuGeoIp NOTIFY bypassRuGeoIpChanged)
    Q_PROPERTY(bool autoBypassRkn READ isAutoBypassRknEnabled WRITE setAutoBypassRknEnabled NOTIFY autoBypassRknChanged)

public slots:
    bool addSite(const QString &hostname, const QString &ip);
    bool addSiteWithVpnPreference(const QString &hostname, const QString &ip, bool useVpn);
    void addSites(const QVector<SiteEntry> &sites, bool replaceExisting);
    void removeSite(QModelIndex index);
    void removeSites();
    void toggleSiteVpn(int row);

    int getRouteMode();
    void setRouteMode(int routeMode);

    bool isSplitTunnelingEnabled();
    void toggleSplitTunneling(bool enabled);

    bool isBypassRuSites();
    void setBypassRuSites(bool enabled);
    bool isBypassRuGeoSites();
    void setBypassRuGeoSites(bool enabled);
    bool isBypassRuGeoIp();
    void setBypassRuGeoIp(bool enabled);

    bool isAutoBypassRknEnabled();
    void setAutoBypassRknEnabled(bool enabled);

    QVector<SiteEntry> getCurrentSites();

signals:
    void routeModeChanged();
    void splitTunnelingToggled();
    void bypassRuSitesChanged();
    void bypassRuGeoSitesChanged();
    void bypassRuGeoIpChanged();
    void autoBypassRknChanged();
    void sitesChanged();

protected:
    QHash<int, QByteArray> roleNames() const override;

private:
    void loadAllSites();
    void persistSites();
    int findSiteRow(const QString &hostname) const;

    std::shared_ptr<Settings> m_settings;

    bool m_isSplitTunnelingEnabled;
    Settings::RouteMode m_currentRouteMode;

    QVector<SiteEntry> m_sites;
};

#endif // SITESMODEL_H

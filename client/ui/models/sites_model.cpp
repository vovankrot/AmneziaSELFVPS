#include "sites_model.h"

namespace {
QString siteLookupKey(const QString &hostname)
{
    return hostname.trimmed().toLower();
}

Settings::RouteMode normalizeRouteMode(int routeMode)
{
    const auto mode = static_cast<Settings::RouteMode>(routeMode);
    return mode == Settings::VpnAllSites ? Settings::VpnOnlyForwardSites : mode;
}
}

SitesModel::SitesModel(std::shared_ptr<Settings> settings, QObject *parent)
    : QAbstractListModel(parent), m_settings(settings)
{
    m_isSplitTunnelingEnabled = m_settings->isSitesSplitTunnelingEnabled();
    m_currentRouteMode = normalizeRouteMode(m_settings->routeMode());
    if (m_currentRouteMode != m_settings->routeMode()) {
        m_settings->setRouteMode(m_currentRouteMode);
    }

    loadAllSites();
}

int SitesModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return m_sites.size();
}

QString SitesModel::stateSignature() const
{
    QStringList siteKeys;
    siteKeys.reserve(m_sites.size());

    for (const auto &site : m_sites) {
        siteKeys.append(QString("%1|%2|%3")
                            .arg(site.hostname, site.ip)
                            .arg(site.useVpn ? 1 : 0));
    }

    siteKeys.sort(Qt::CaseInsensitive);

    return QString("%1|%2|%3|%4|%5|%6|%7")
        .arg(m_isSplitTunnelingEnabled ? 1 : 0)
        .arg(static_cast<int>(m_currentRouteMode))
        .arg(m_settings->bypassRuGeoSites() ? 1 : 0)
        .arg(m_settings->bypassRuGeoIp() ? 1 : 0)
        .arg(m_settings->bypassRuSites() ? 1 : 0)
        .arg(m_settings->isAutoBypassRknEnabled() ? 1 : 0)
        .arg(siteKeys.join("||"));
}

QVariant SitesModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rowCount()))
        return QVariant();

    switch (role) {
    case UrlRole:
        return m_sites.at(index.row()).hostname;
    case IpRole:
        return m_sites.at(index.row()).ip;
    case UseVpnRole:
        return m_sites.at(index.row()).useVpn;
    default:
        return QVariant();
    }
}

bool SitesModel::addSite(const QString &hostname, const QString &ip)
{
    const int existingRow = findSiteRow(hostname);
    if (existingRow >= 0) {
        auto &existingSite = m_sites[existingRow];
        if (existingSite.ip.isEmpty() && !ip.isEmpty()) {
            existingSite.ip = ip;
            persistSites();

            const QModelIndex changedIndex = index(existingRow, 0);
            emit dataChanged(changedIndex, changedIndex, { IpRole });
            emit sitesChanged();
            return true;
        }

        return false;
    }

    SiteEntry entry;
    entry.hostname = hostname;
    entry.ip = ip;
    entry.useVpn = m_currentRouteMode == Settings::VpnOnlyForwardSites;

    beginInsertRows(QModelIndex(), rowCount(), rowCount());
    m_sites.append(entry);
    persistSites();
    endInsertRows();
    emit sitesChanged();

    return true;
}

bool SitesModel::addSiteWithVpnPreference(const QString &hostname, const QString &ip, bool useVpn)
{
    const int existingRow = findSiteRow(hostname);
    if (existingRow >= 0) {
        auto &existingSite = m_sites[existingRow];

        bool changed = false;
        if (existingSite.useVpn != useVpn) {
            existingSite.useVpn = useVpn;
            changed = true;
        }
        if (!ip.isEmpty() && existingSite.ip != ip) {
            existingSite.ip = ip;
            changed = true;
        }

        if (!changed) {
            return false;
        }

        persistSites();

        const QModelIndex changedIndex = index(existingRow, 0);
        emit dataChanged(changedIndex, changedIndex, { IpRole, UseVpnRole });
        emit sitesChanged();
        return true;
    }

    SiteEntry entry;
    entry.hostname = hostname;
    entry.ip = ip;
    entry.useVpn = useVpn;

    beginInsertRows(QModelIndex(), rowCount(), rowCount());
    m_sites.append(entry);
    persistSites();
    endInsertRows();
    emit sitesChanged();

    return true;
}

void SitesModel::addSites(const QVector<SiteEntry> &sites, bool replaceExisting)
{
    const QString previousSignature = stateSignature();

    beginResetModel();

    if (replaceExisting) {
        m_sites.clear();
    }

    for (const auto &site : sites) {
        if (site.hostname.trimmed().isEmpty()) {
            continue;
        }

        const int existingRow = findSiteRow(site.hostname);
        if (existingRow >= 0) {
            auto &existingSite = m_sites[existingRow];
            existingSite.useVpn = site.useVpn;
            if (!site.ip.isEmpty()) {
                existingSite.ip = site.ip;
            }
            continue;
        }

        m_sites.append(site);
    }

    persistSites();

    endResetModel();

    if (previousSignature != stateSignature()) {
        emit sitesChanged();
    }
}

void SitesModel::removeSite(QModelIndex index)
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return;
    }

    beginRemoveRows(QModelIndex(), index.row(), index.row());
    m_sites.removeAt(index.row());
    persistSites();
    endRemoveRows();
    emit sitesChanged();
}

void SitesModel::removeSites()
{
    if (m_sites.isEmpty()) {
        return;
    }

    beginResetModel();

    m_sites.clear();
    persistSites();

    endResetModel();
    emit sitesChanged();
}

void SitesModel::toggleSiteVpn(int row)
{
    if (row < 0 || row >= rowCount()) {
        return;
    }

    m_sites[row].useVpn = !m_sites[row].useVpn;
    persistSites();

    const QModelIndex changedIndex = index(row, 0);
    emit dataChanged(changedIndex, changedIndex, { UseVpnRole });
    emit sitesChanged();
}

int SitesModel::getRouteMode()
{
    return m_currentRouteMode;
}

void SitesModel::setRouteMode(int routeMode)
{
    const Settings::RouteMode normalizedMode = normalizeRouteMode(routeMode);
    if (m_currentRouteMode == normalizedMode) {
        return;
    }

    m_settings->setRouteMode(normalizedMode);
    m_currentRouteMode = normalizedMode;

    const bool newUseVpn = (normalizedMode == Settings::VpnOnlyForwardSites);
    bool anyChanged = false;
    for (auto &site : m_sites) {
        if (site.useVpn != newUseVpn) {
            site.useVpn = newUseVpn;
            anyChanged = true;
        }
    }
    if (anyChanged) {
        persistSites();
        emit dataChanged(index(0, 0), index(rowCount() - 1, 0), { UseVpnRole });
        emit sitesChanged();
    }

    emit routeModeChanged();
}

bool SitesModel::isSplitTunnelingEnabled()
{
    return m_isSplitTunnelingEnabled;
}

void SitesModel::toggleSplitTunneling(bool enabled)
{
    m_settings->setSitesSplitTunnelingEnabled(enabled);
    m_isSplitTunnelingEnabled = enabled;
    emit splitTunnelingToggled();
}

QVector<SitesModel::SiteEntry> SitesModel::getCurrentSites()
{
    return m_sites;
}

bool SitesModel::isBypassRuSites()
{
    return m_settings->bypassRuSites();
}

void SitesModel::setBypassRuSites(bool enabled)
{
    m_settings->setBypassRuSites(enabled);
    emit bypassRuSitesChanged();
    emit bypassRuGeoSitesChanged();
    emit bypassRuGeoIpChanged();
}

bool SitesModel::isBypassRuGeoSites()
{
    return m_settings->bypassRuGeoSites();
}

void SitesModel::setBypassRuGeoSites(bool enabled)
{
    m_settings->setBypassRuGeoSites(enabled);
    emit bypassRuGeoSitesChanged();
    emit bypassRuSitesChanged();
}

bool SitesModel::isBypassRuGeoIp()
{
    return m_settings->bypassRuGeoIp();
}

void SitesModel::setBypassRuGeoIp(bool enabled)
{
    m_settings->setBypassRuGeoIp(enabled);
    emit bypassRuGeoIpChanged();
    emit bypassRuSitesChanged();
}

bool SitesModel::isAutoBypassRknEnabled()
{
    return m_settings->isAutoBypassRknEnabled();
}

void SitesModel::setAutoBypassRknEnabled(bool enabled)
{
    m_settings->setAutoBypassRknEnabled(enabled);
    emit autoBypassRknChanged();
}

QHash<int, QByteArray> SitesModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[UrlRole] = "url";
    roles[IpRole] = "ip";
    roles[UseVpnRole] = "useVpn";
    return roles;
}

void SitesModel::loadAllSites()
{
    m_sites.clear();

    const auto storedRules = m_settings->getVpnSiteRules();
    m_sites.reserve(storedRules.size());

    for (const auto &rule : storedRules) {
        SiteEntry entry;
        entry.hostname = rule.hostname.trimmed();
        entry.ip = rule.ip.trimmed();
        entry.useVpn = rule.useVpn;
        if (!entry.hostname.isEmpty()) {
            m_sites.append(entry);
        }
    }
}

void SitesModel::persistSites()
{
    QVector<Settings::SiteSplitRule> rules;
    rules.reserve(m_sites.size());

    for (const auto &site : m_sites) {
        if (site.hostname.isEmpty()) {
            continue;
        }

        Settings::SiteSplitRule rule;
        rule.hostname = site.hostname;
        rule.ip = site.ip;
        rule.useVpn = site.useVpn;
        rules.append(rule);
    }

    m_settings->setVpnSiteRules(rules);
}

int SitesModel::findSiteRow(const QString &hostname) const
{
    const QString lookupKey = siteLookupKey(hostname);
    for (int row = 0; row < m_sites.size(); ++row) {
        if (siteLookupKey(m_sites[row].hostname) == lookupKey) {
            return row;
        }
    }

    return -1;
}

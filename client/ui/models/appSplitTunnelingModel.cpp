#include "appSplitTunnelingModel.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

namespace {
QString normalizeStoredAppPath(const QString &path)
{
    return QDir::fromNativeSeparators(QDir::cleanPath(path.trimmed()));
}

bool isExcludeOnlyAppSplitTunnelMode()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

bool isValidStoredSplitTunnelApp(const amnezia::InstalledAppInfo &appInfo)
{
    if (!appInfo.packageName.isEmpty()) {
        return true;
    }

    const QString normalizedPath = normalizeStoredAppPath(appInfo.appPath);
    if (normalizedPath.isEmpty()) {
        return false;
    }

#ifdef Q_OS_WIN
    const QFileInfo fileInfo(normalizedPath);
    return fileInfo.exists() && fileInfo.isFile()
           && fileInfo.suffix().compare("exe", Qt::CaseInsensitive) == 0;
#else
    const QFileInfo fileInfo(normalizedPath);
    return fileInfo.exists() && fileInfo.isFile();
#endif
}

QVector<amnezia::InstalledAppInfo> sanitizeStoredSplitTunnelApps(const QVector<amnezia::InstalledAppInfo> &apps, bool *didChange = nullptr)
{
    QVector<amnezia::InstalledAppInfo> sanitizedApps;
    sanitizedApps.reserve(apps.size());

    bool changed = false;

    for (amnezia::InstalledAppInfo appInfo : apps) {
        if (!appInfo.packageName.isEmpty()) {
            if (sanitizedApps.contains(appInfo)) {
                changed = true;
                continue;
            }

            sanitizedApps.append(appInfo);
            continue;
        }

        const QString originalPath = appInfo.appPath;
        appInfo.appPath = QDir::fromNativeSeparators(QDir::cleanPath(appInfo.appPath.trimmed()));
        if (appInfo.appPath != originalPath) {
            changed = true;
        }

        if (!isValidStoredSplitTunnelApp(appInfo)) {
            changed = true;
            continue;
        }

        if (appInfo.appName.isEmpty()) {
            appInfo.appName = QFileInfo(appInfo.appPath).fileName();
            changed = true;
        }

        if (sanitizedApps.contains(appInfo)) {
            changed = true;
            continue;
        }

        sanitizedApps.append(appInfo);
    }

    if (didChange) {
        *didChange = changed || sanitizedApps.size() != apps.size();
    }

    return sanitizedApps;
}

}

AppSplitTunnelingModel::AppSplitTunnelingModel(std::shared_ptr<Settings> settings, QObject *parent)
    : QAbstractListModel(parent), m_settings(settings)
{
    m_isSplitTunnelingEnabled = m_settings->isAppsSplitTunnelingEnabled();

    // Migrate: always use VpnAllExceptApps mode internally
    if (m_settings->getAppsRouteMode() != Settings::VpnAllExceptApps) {
        m_settings->setAppsRouteMode(Settings::VpnAllExceptApps);
    }

    loadAllApps();
}

void AppSplitTunnelingModel::loadAllApps()
{
    m_apps.clear();
    invalidateGroupCache();

    // Load "except" apps (bypass VPN) — useVpn = false
    bool didSanitizeExcept = false;
    auto exceptApps = sanitizeStoredSplitTunnelApps(
        m_settings->getVpnApps(Settings::VpnAllExceptApps), &didSanitizeExcept);
    if (didSanitizeExcept) {
        m_settings->setVpnApps(Settings::VpnAllExceptApps, exceptApps);
    }

    for (const auto &app : exceptApps) {
        AppEntry entry;
        entry.info = app;
        entry.useVpn = false;
        m_apps.append(entry);
    }

    bool didSanitizeForward = false;
    auto forwardApps = sanitizeStoredSplitTunnelApps(
        m_settings->getVpnApps(Settings::VpnOnlyForwardApps), &didSanitizeForward);

    if (isExcludeOnlyAppSplitTunnelMode()) {
        if (didSanitizeForward || !forwardApps.isEmpty()) {
            m_settings->setVpnApps(Settings::VpnOnlyForwardApps, {});
        }
        return;
    }

    // Load "forward" apps (through VPN) — useVpn = true
    if (didSanitizeForward) {
        m_settings->setVpnApps(Settings::VpnOnlyForwardApps, forwardApps);
    }

    for (const auto &app : forwardApps) {
        // Skip duplicates (app already in except list takes precedence)
        if (containsApp(app)) {
            continue;
        }
        AppEntry entry;
        entry.info = app;
        entry.useVpn = true;
        m_apps.append(entry);
    }
}

bool AppSplitTunnelingModel::containsApp(const amnezia::InstalledAppInfo &appInfo) const
{
    for (const auto &entry : m_apps) {
        if (entry.info == appInfo) {
            return true;
        }
    }
    return false;
}

void AppSplitTunnelingModel::persistApps()
{
    QVector<amnezia::InstalledAppInfo> exceptApps;
    QVector<amnezia::InstalledAppInfo> forwardApps;

    for (const auto &entry : m_apps) {
        if (!isExcludeOnlyAppSplitTunnelMode() && entry.useVpn) {
            forwardApps.append(entry.info);
        } else {
            exceptApps.append(entry.info);
        }
    }

    m_settings->setVpnApps(Settings::VpnAllExceptApps, exceptApps);
    m_settings->setVpnApps(Settings::VpnOnlyForwardApps, forwardApps);
}

int AppSplitTunnelingModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return m_apps.size();
}

QString AppSplitTunnelingModel::stateSignature() const
{
    QStringList appKeys;
    appKeys.reserve(m_apps.size());

    for (const auto &entry : m_apps) {
        QString key;
        if (!entry.info.packageName.isEmpty()) {
            key = QString("pkg:%1").arg(entry.info.packageName);
        } else {
            key = QString("path:%1").arg(
                QDir::fromNativeSeparators(QDir::cleanPath(entry.info.appPath)));
        }
        key += entry.useVpn ? ":vpn" : ":direct";
        appKeys.append(key);
    }

    appKeys.sort(Qt::CaseInsensitive);

    return QString("%1|%2")
        .arg(m_isSplitTunnelingEnabled ? 1 : 0)
        .arg(appKeys.join("||"));
}

QVariant AppSplitTunnelingModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rowCount()))
        return QVariant();

    const auto &entry = m_apps.at(index.row());

    switch (role) {
        case AppPathRole:
            return entry.info.appPath;
        case PackageAppNameRole:
            return appDisplayName(entry.info);
        case UseVpnRole:
            return entry.useVpn;
        case GroupFolderRole:
            return effectiveGroupForRow(index.row());
        default:
            return QVariant();
    }
}

bool AppSplitTunnelingModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
        return false;

    if (role == UseVpnRole) {
        m_apps[index.row()].useVpn = value.toBool();
        persistApps();
        emit dataChanged(index, index, { UseVpnRole });
        return true;
    }
    return false;
}

bool AppSplitTunnelingModel::addApp(const amnezia::InstalledAppInfo &appInfo)
{
    if (!isValidStoredSplitTunnelApp(appInfo)) {
        return false;
    }

    // If the app is already present and the caller provides a (different) source folder
    // — e.g. a folder-add that re-encounters an exe previously added individually or from
    // another folder — adopt it into the new group. Otherwise "Remove folder" would leave
    // an app that physically lives in that folder behind, breaking the "…and all its apps"
    // promise. An individual add (empty groupFolder) never strips an existing group.
    for (int row = 0; row < m_apps.size(); ++row) {
        if (!(m_apps.at(row).info == appInfo)) {
            continue;
        }

        if (!appInfo.groupFolder.isEmpty() && m_apps.at(row).info.groupFolder != appInfo.groupFolder) {
            m_apps[row].info.groupFolder = appInfo.groupFolder;
            invalidateGroupCache();
            persistApps();
            if (!m_apps.isEmpty()) {
                emit dataChanged(index(0, 0), index(m_apps.size() - 1, 0), { GroupFolderRole });
            }
        }

        return false;
    }

    AppEntry entry;
    entry.info = appInfo;
    entry.useVpn = false; // new apps default to "bypass VPN"

    beginInsertRows(QModelIndex(), rowCount(), rowCount());
    m_apps.append(entry);
    invalidateGroupCache();
    persistApps();
    endInsertRows();

    // A new entry can change the effective group of existing rows (a parent folder
    // appearing collapses former subfolder groups into it) — refresh them all.
    if (m_apps.size() > 1) {
        emit dataChanged(index(0, 0), index(m_apps.size() - 2, 0), { GroupFolderRole });
    }

    return true;
}

void AppSplitTunnelingModel::removeApp(QModelIndex index)
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_apps.size()) {
        return;
    }

    beginRemoveRows(QModelIndex(), index.row(), index.row());
    m_apps.removeAt(index.row());
    invalidateGroupCache();
    persistApps();
    endRemoveRows();

    if (!m_apps.isEmpty()) {
        emit dataChanged(this->index(0, 0), this->index(m_apps.size() - 1, 0), { GroupFolderRole });
    }
}

void AppSplitTunnelingModel::clearAppsList() {
    beginResetModel();
    m_apps.clear();
    invalidateGroupCache();
    persistApps();
    endResetModel();
}

void AppSplitTunnelingModel::toggleAppVpn(int row)
{
    if (row < 0 || row >= rowCount())
        return;

    if (isExcludeOnlyAppSplitTunnelMode()) {
        removeApp(index(row, 0));
        return;
    }

    m_apps[row].useVpn = !m_apps[row].useVpn;
    persistApps();

    QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx, { UseVpnRole });
}

bool AppSplitTunnelingModel::isSplitTunnelingEnabled()
{
    return m_isSplitTunnelingEnabled;
}

bool AppSplitTunnelingModel::supportsPerAppVpnToggle() const
{
    return !isExcludeOnlyAppSplitTunnelMode();
}

int AppSplitTunnelingModel::removeGroup(const QString &groupFolder)
{
    // Guard the raw input first: an empty/whitespace folder must never match
    // anything (that would sweep the whole list).
    if (groupFolder.trimmed().isEmpty()) {
        return 0;
    }

    const QString normalizedGroup = normalizeStoredAppPath(groupFolder);
    if (normalizedGroup.isEmpty()) {
        return 0;
    }

    QVector<AppEntry> remainingApps;
    remainingApps.reserve(m_apps.size());

    int removedCount = 0;
    for (int row = 0; row < m_apps.size(); ++row) {
        // Match on the same effective group the UI displays, so removing a header
        // removes exactly the rows shown under it (including merged subfolders).
        if (effectiveGroupForRow(row).compare(normalizedGroup, Qt::CaseInsensitive) == 0) {
            ++removedCount;
            continue;
        }
        remainingApps.append(m_apps.at(row));
    }

    if (removedCount == 0) {
        return 0;
    }

    beginResetModel();
    m_apps = remainingApps;
    invalidateGroupCache();
    persistApps();
    endResetModel();

    return removedCount;
}

void AppSplitTunnelingModel::toggleSplitTunneling(bool enabled)
{
    m_settings->setAppsSplitTunnelingEnabled(enabled);
    m_isSplitTunnelingEnabled = enabled;
    emit splitTunnelingToggled();
}

QHash<int, QByteArray> AppSplitTunnelingModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[AppPathRole] = "appPath";
    roles[PackageAppNameRole] = "appName";
    roles[UseVpnRole] = "useVpn";
    roles[GroupFolderRole] = "groupFolder";
    return roles;
}

QString AppSplitTunnelingModel::effectiveGroupForRow(int row) const
{
    if (m_groupCacheDirty) {
        rebuildGroupCache();
    }

    if (row < 0 || row >= m_effectiveGroupByRow.size()) {
        return QString();
    }

    return m_effectiveGroupByRow.at(row);
}

void AppSplitTunnelingModel::rebuildGroupCache() const
{
    m_effectiveGroupByRow.clear();
    m_effectiveGroupByRow.reserve(m_apps.size());

    // Base folder per row: the stored source folder (set by "Add folder"), or the
    // executable's own directory for individually-added and legacy entries — so the
    // whole list collapses into folder groups.
    QVector<QString> bases;
    bases.reserve(m_apps.size());

    QStringList roots;
    QSet<QString> seenRoots;

    for (const auto &entry : m_apps) {
        QString base = entry.info.groupFolder;
        if (base.isEmpty() && !entry.info.appPath.isEmpty()) {
            base = QFileInfo(entry.info.appPath).absolutePath();
        }
        base = normalizeStoredAppPath(base);
        bases.append(base);

        const QString rootKey = base.toLower();
        if (!base.isEmpty() && !seenRoots.contains(rootKey)) {
            seenRoots.insert(rootKey);
            roots.append(base);
        }
    }

    // Shortest-first, so the first ancestor hit is the shallowest existing group —
    // a subfolder scan (e.g. ".../App/Applications") merges under its parent group
    // (".../App") instead of showing up as a stray sibling section.
    std::sort(roots.begin(), roots.end(),
              [](const QString &a, const QString &b) { return a.size() < b.size(); });

    for (const QString &base : bases) {
        QString group = base;

        for (const QString &root : roots) {
            if (root.size() > base.size()) {
                break;
            }

            const QString rootPrefix = root.endsWith('/') ? root : root + QStringLiteral("/");
            if (base.compare(root, Qt::CaseInsensitive) == 0
                || base.startsWith(rootPrefix, Qt::CaseInsensitive)) {
                group = root;
                break;
            }
        }

        m_effectiveGroupByRow.append(group);
    }

    m_groupCacheDirty = false;
}

void AppSplitTunnelingModel::invalidateGroupCache()
{
    m_groupCacheDirty = true;
}

int AppSplitTunnelingModel::groupCount(const QString &groupFolder) const
{
    const QString normalizedGroup = normalizeStoredAppPath(groupFolder);
    if (normalizedGroup.isEmpty()) {
        return 0;
    }

    int count = 0;
    for (int row = 0; row < m_apps.size(); ++row) {
        if (effectiveGroupForRow(row).compare(normalizedGroup, Qt::CaseInsensitive) == 0) {
            ++count;
        }
    }
    return count;
}

QString AppSplitTunnelingModel::appDisplayName(const amnezia::InstalledAppInfo &appInfo) const
{
    if (!appInfo.appName.isEmpty()) {
        return appInfo.appName;
    }

    if (!appInfo.appPath.isEmpty()) {
        return QFileInfo(appInfo.appPath).fileName();
    }

    return appInfo.packageName;
}

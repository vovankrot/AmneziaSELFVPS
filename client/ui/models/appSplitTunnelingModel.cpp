#include "appSplitTunnelingModel.h"

#include <QDir>
#include <QProcessEnvironment>
#include <QFileInfo>

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

#ifdef Q_OS_WIN
QStringList windowsAppInstallRoots()
{
    QStringList roots;
    const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();

    const auto appendRoot = [&roots, &environment](const QString &variableName) {
        const QString rawPath = environment.value(variableName).trimmed();
        if (rawPath.isEmpty()) {
            return;
        }

        roots.append(normalizeStoredAppPath(rawPath));
    };

    appendRoot(QStringLiteral("LOCALAPPDATA"));
    appendRoot(QStringLiteral("APPDATA"));
    appendRoot(QStringLiteral("ProgramFiles"));
    appendRoot(QStringLiteral("ProgramFiles(x86)"));

    roots.removeAll(QString());
    roots.removeDuplicates();
    return roots;
}

QString windowsAppFamilyScope(const QString &appPath)
{
    const QString normalizedPath = normalizeStoredAppPath(appPath);
    if (normalizedPath.isEmpty()) {
        return QString();
    }

    for (const auto &rootPath : windowsAppInstallRoots()) {
        const QString scopedPrefix = rootPath + QStringLiteral("/");
        if (!normalizedPath.startsWith(scopedPrefix, Qt::CaseInsensitive)) {
            continue;
        }

        const QString relativePath = normalizedPath.mid(scopedPrefix.size());
        const QString familyName = relativePath.section('/', 0, 0).trimmed();
        if (familyName.isEmpty()) {
            return QString();
        }

        return scopedPrefix + familyName;
    }

    return QString();
}

bool belongsToSameWindowsAppFamily(const QString &candidatePath, const QString &referencePath)
{
    const QString normalizedCandidatePath = normalizeStoredAppPath(candidatePath);
    const QString normalizedReferencePath = normalizeStoredAppPath(referencePath);

    if (normalizedCandidatePath.isEmpty() || normalizedReferencePath.isEmpty()) {
        return false;
    }

    if (normalizedCandidatePath.compare(normalizedReferencePath, Qt::CaseInsensitive) == 0) {
        return true;
    }

    const QString familyScope = windowsAppFamilyScope(normalizedReferencePath);
    if (familyScope.isEmpty()) {
        return false;
    }

    return normalizedCandidatePath.startsWith(familyScope + QStringLiteral("/"), Qt::CaseInsensitive);
}
#endif
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

    if (containsApp(appInfo)) {
        return false;
    }

    AppEntry entry;
    entry.info = appInfo;
    entry.useVpn = false; // new apps default to "bypass VPN"

    beginInsertRows(QModelIndex(), rowCount(), rowCount());
    m_apps.append(entry);
    persistApps();
    endInsertRows();

    return true;
}

void AppSplitTunnelingModel::removeApp(QModelIndex index)
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_apps.size()) {
        return;
    }

    const QString appPath = m_apps.at(index.row()).info.appPath;
    if (removeAppsByPath(appPath) > 0) {
        return;
    }

    beginRemoveRows(QModelIndex(), index.row(), index.row());
    m_apps.removeAt(index.row());
    persistApps();
    endRemoveRows();
}

void AppSplitTunnelingModel::clearAppsList() {
    beginResetModel();
    m_apps.clear();
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

int AppSplitTunnelingModel::removeAppsByPath(const QString &appPath)
{
    const QString normalizedAppPath = normalizeStoredAppPath(appPath);
    if (normalizedAppPath.isEmpty()) {
        return 0;
    }

    QVector<int> rowsToRemove;
    rowsToRemove.reserve(m_apps.size());

    for (int row = 0; row < m_apps.size(); ++row) {
        const QString candidatePath = m_apps.at(row).info.appPath;

#ifdef Q_OS_WIN
        if (belongsToSameWindowsAppFamily(candidatePath, normalizedAppPath)) {
            rowsToRemove.append(row);
        }
#else
        if (normalizeStoredAppPath(candidatePath) == normalizedAppPath) {
            rowsToRemove.append(row);
        }
#endif
    }

    if (rowsToRemove.isEmpty()) {
        return 0;
    }

    if (rowsToRemove.size() == 1) {
        const int row = rowsToRemove.constFirst();
        beginRemoveRows(QModelIndex(), row, row);
        m_apps.removeAt(row);
        persistApps();
        endRemoveRows();
        return 1;
    }

    std::sort(rowsToRemove.begin(), rowsToRemove.end());

    QVector<AppEntry> updatedApps;
    updatedApps.reserve(m_apps.size() - rowsToRemove.size());

    int nextRowToRemoveIndex = 0;
    for (int row = 0; row < m_apps.size(); ++row) {
        if (nextRowToRemoveIndex < rowsToRemove.size() && row == rowsToRemove.at(nextRowToRemoveIndex)) {
            ++nextRowToRemoveIndex;
            continue;
        }

        updatedApps.append(m_apps.at(row));
    }

    beginResetModel();
    m_apps = updatedApps;
    persistApps();
    endResetModel();

    return rowsToRemove.size();
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
    return roles;
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

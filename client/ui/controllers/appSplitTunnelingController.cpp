#include "appSplitTunnelingController.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QMetaObject>

#ifdef Q_OS_WIN
    #include <Windows.h>
    #include <shellapi.h>
#endif

#include "core/defs.h"
#include "ui/controllers/connectionController.h"
#include "ui/controllers/pageController.h"
#include "ui/models/servers_model.h"

namespace {
QString normalizeAppPath(const QString &path)
{
    const QString trimmedPath = path.trimmed();
    if (trimmedPath.isEmpty()) {
        return "";
    }

    QFileInfo fileInfo(QDir::fromNativeSeparators(trimmedPath));
    QString normalizedPath = fileInfo.canonicalFilePath();
    if (normalizedPath.isEmpty()) {
        normalizedPath = fileInfo.absoluteFilePath();
    }

    return QDir::fromNativeSeparators(QDir::cleanPath(normalizedPath));
}

bool isSupportedSplitTunnelAppPath(const QString &path)
{
    const QFileInfo fileInfo(path);

#ifdef Q_OS_WIN
    return fileInfo.exists() && fileInfo.isFile()
           && fileInfo.suffix().compare("exe", Qt::CaseInsensitive) == 0;
#else
    return fileInfo.exists() && fileInfo.isFile();
#endif
}

bool siteSplitTunnelingCanAffectRemovedApp(const std::shared_ptr<Settings> &settings)
{
    if (!settings || !settings->isSitesSplitTunnelingEnabled()) {
        return false;
    }

    if (settings->bypassRuGeoSites() || settings->bypassRuGeoIp()
        || settings->isAutoBypassRknEnabled()) {
        return true;
    }

    const Settings::RouteMode routeMode = settings->routeMode();
    if (routeMode == Settings::VpnOnlyForwardSites) {
        return !settings->vpnSites(Settings::VpnOnlyForwardSites).isEmpty();
    }

    if (routeMode == Settings::VpnAllExceptSites) {
        return !settings->vpnSites(Settings::VpnAllExceptSites).isEmpty();
    }

    return false;
}

QString buildRemovedAppNotificationMessage(const std::shared_ptr<Settings> &settings,
                                          const QString &appName)
{
    const QString baseMessage = QCoreApplication::translate(
        "AppSplitTunnelingController",
        "Application removed: %1").arg(appName);

#ifdef Q_OS_WIN
    if (siteSplitTunnelingCanAffectRemovedApp(settings)) {
        return QCoreApplication::translate(
            "AppSplitTunnelingController",
            "Application removed: %1. Site split tunneling is still active, so some traffic from this application may still bypass VPN.")
            .arg(appName);
    }
#else
    Q_UNUSED(settings)
#endif

    return baseMessage;
}

#ifdef Q_OS_WIN
QString normalizeLaunchTargetPath(const QString &path)
{
    const QString normalizedPath = normalizeAppPath(path);
    QFileInfo fileInfo(normalizedPath);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return "";
    }

    return normalizedPath;
}

QString resolveLaunchExecutablePath(const QString &targetPath)
{
    const QFileInfo targetInfo(targetPath);
    if (targetInfo.suffix().compare("exe", Qt::CaseInsensitive) == 0) {
        return targetPath;
    }

    wchar_t resolvedExecutable[MAX_PATH] = {};
    const QString nativeTargetPath = QDir::toNativeSeparators(targetPath);
    const HINSTANCE result = FindExecutableW(reinterpret_cast<LPCWSTR>(nativeTargetPath.utf16()), nullptr, resolvedExecutable);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        return "";
    }

    QFileInfo executableInfo(QString::fromWCharArray(resolvedExecutable));
    QString normalizedExecutablePath = executableInfo.canonicalFilePath();
    if (normalizedExecutablePath.isEmpty()) {
        normalizedExecutablePath = executableInfo.absoluteFilePath();
    }

    return QDir::fromNativeSeparators(QDir::cleanPath(normalizedExecutablePath));
}

bool launchTargetWithShell(const QString &targetPath)
{
    const QString nativeTargetPath = QDir::toNativeSeparators(targetPath);
    const QString nativeWorkingDirectory = QDir::toNativeSeparators(QFileInfo(targetPath).absolutePath());

    std::wstring target = nativeTargetPath.toStdWString();
    std::wstring workingDirectory = nativeWorkingDirectory.toStdWString();

    SHELLEXECUTEINFOW executeInfo = {};
    executeInfo.cbSize = sizeof(executeInfo);
    executeInfo.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    executeInfo.lpVerb = L"open";
    executeInfo.lpFile = target.c_str();
    executeInfo.lpDirectory = workingDirectory.c_str();
    executeInfo.nShow = SW_SHOWNORMAL;

    return ShellExecuteExW(&executeInfo) == TRUE;
}
#endif
}

AppSplitTunnelingController::AppSplitTunnelingController(const std::shared_ptr<Settings> &settings,
                                                         const QSharedPointer<AppSplitTunnelingModel> &appSplitTunnelingModel,
                                                         ConnectionController *connectionController,
                                                         PageController *pageController,
                                                         ServersModel *serversModel,
                                                         QObject *parent)
    : QObject(parent),
      m_settings(settings),
      m_appSplitTunnelingModel(appSplitTunnelingModel),
      m_connectionController(connectionController),
      m_pageController(pageController),
      m_serversModel(serversModel)
{
}

void AppSplitTunnelingController::addApp(const QString &appPath)
{
    const QString normalizedAppPath = normalizeAppPath(appPath);
    if (normalizedAppPath.isEmpty()) {
        emit errorOccurred(tr("The selected application path is invalid"));
        return;
    }

    if (!isSupportedSplitTunnelAppPath(normalizedAppPath)) {
        emit errorOccurred(tr("Please select an executable (.exe) file"));
        return;
    }

    InstalledAppInfo appInfo { "", "", normalizedAppPath };
    if (!normalizedAppPath.isEmpty()) {
        QFileInfo fileInfo(normalizedAppPath);
        appInfo.appName = fileInfo.fileName();
    }

    if (m_appSplitTunnelingModel->addApp(appInfo)) {
        emit finished(tr("Application added: %1").arg(appInfo.appName));

    } else {
        emit errorOccurred(tr("The application has already been added"));
    }
}

void AppSplitTunnelingController::addApps(QVector<QPair<QString, QString>> apps)
{
    for (const auto &app : apps) {
        InstalledAppInfo appInfo { app.first, app.second, "" };

        m_appSplitTunnelingModel->addApp(appInfo);
    }
    emit finished(tr("The selected applications have been added"));
}

void AppSplitTunnelingController::addAppsFromFolder(const QString &folderPath)
{
    const QString normalizedFolderPath = normalizeAppPath(folderPath);
    QDir folder(normalizedFolderPath);
    if (!folder.exists()) {
        emit errorOccurred(tr("The selected folder does not exist"));
        return;
    }

    QDirIterator iterator(normalizedFolderPath,
                          QStringList() << "*.exe",
                          QDir::Files | QDir::NoSymLinks,
                          QDirIterator::Subdirectories);

    int addedCount = 0;
    QString lastAddedAppName;
    bool foundExecutable = false;
    while (iterator.hasNext()) {
        foundExecutable = true;

        const QString normalizedAppPath = normalizeAppPath(iterator.next());
        if (normalizedAppPath.isEmpty()) {
            continue;
        }

        InstalledAppInfo appInfo { "", "", normalizedAppPath };
        QFileInfo fileInfo(normalizedAppPath);
        appInfo.appName = fileInfo.fileName();
        appInfo.groupFolder = normalizedFolderPath; // tag so the UI can group and remove the whole folder at once

        if (m_appSplitTunnelingModel->addApp(appInfo)) {
            ++addedCount;
            lastAddedAppName = appInfo.appName;
        }
    }

    if (!foundExecutable) {
        emit errorOccurred(tr("No executable files were found in the selected folder"));
        return;
    }

    if (addedCount == 0) {
        emit errorOccurred(tr("All applications from the selected folder have already been added"));
        return;
    }

    if (addedCount == 1) {
        emit finished(tr("Application added: %1").arg(lastAddedAppName));
        return;
    }

    emit finished(tr("The selected applications have been added"));
}

void AppSplitTunnelingController::launchTargetBypassingVpn(const QString &targetPath)
{
#ifndef Q_OS_WIN
    showGlobalError(tr("Launching applications outside VPN is only supported on Windows"));
    return;
#else
    if (!m_pendingLaunchTargetPath.isEmpty()) {
        showGlobalError(tr("Another launch outside VPN request is already in progress"));
        return;
    }

    const QString normalizedTargetPath = normalizeLaunchTargetPath(targetPath);
    if (normalizedTargetPath.isEmpty()) {
        showGlobalError(tr("The selected file path is invalid"));
        return;
    }

    const QString launchExecutablePath = resolveLaunchExecutablePath(normalizedTargetPath);
    if (launchExecutablePath.isEmpty()) {
        showGlobalError(tr("Unable to determine which application should be launched for the selected file"));
        return;
    }

    if (!m_connectionController || !m_connectionController->isConnected()) {
        showGlobalError(tr("AmneziaVPN has no active VPN connection"));
        return;
    }

    if (m_serversModel && m_serversModel->getServersCount() > 0 &&
        m_serversModel->getDefaultServerIndex() >= 0 &&
        m_serversModel->isDefaultServerDefaultContainerHasSplitTunneling()) {
        showGlobalError(tr("Default server does not support split tunneling function"));
        return;
    }

    const bool splitTunnelingWasDisabled = !m_appSplitTunnelingModel->isSplitTunnelingEnabled();
    if (splitTunnelingWasDisabled) {
        m_appSplitTunnelingModel->toggleSplitTunneling(true);
    }

    InstalledAppInfo appInfo { QFileInfo(launchExecutablePath).fileName(), "", launchExecutablePath };
    const bool appAdded = m_appSplitTunnelingModel->addApp(appInfo);

    if (!splitTunnelingWasDisabled && !appAdded) {
        if (!launchTargetWithShell(normalizedTargetPath)) {
            showGlobalError(tr("The selected file could not be started outside the VPN"));
        }
        return;
    }

    m_pendingLaunchTargetPath = normalizedTargetPath;
    m_pendingLaunchExecutablePath = launchExecutablePath;

    m_pendingBypassLaunchConnection = connect(
        m_connectionController,
        &ConnectionController::connectionStateChanged,
        this,
        [this]() {
            if (m_pendingLaunchTargetPath.isEmpty() || !m_connectionController) {
                return;
            }

            if (m_connectionController->isConnected()) {
                finishPendingBypassLaunch();
                return;
            }

            if (m_connectionController->isConnectionInProgress()) {
                return;
            }

            clearPendingBypassLaunch();
            showGlobalError(tr("Failed to apply split tunneling settings. The selected file was not launched outside the VPN"));
        });

    m_connectionController->reconnectToVpn();
#endif
}

void AppSplitTunnelingController::removeApp(const int index)
{
    auto modelIndex = m_appSplitTunnelingModel->index(index);
    auto appPath = m_appSplitTunnelingModel->data(modelIndex, AppSplitTunnelingModel::Roles::AppPathRole).toString();
    const QString appName = QFileInfo(appPath).fileName();
    m_appSplitTunnelingModel->removeApp(modelIndex);

    emit finished(buildRemovedAppNotificationMessage(m_settings, appName));
}

void AppSplitTunnelingController::removeGroup(const QString &groupFolder)
{
    const int removedCount = m_appSplitTunnelingModel->removeGroup(groupFolder);
    if (removedCount <= 0) {
        return;
    }

    const QString folderName = QFileInfo(groupFolder).fileName();
    const QString folderLabel = folderName.isEmpty() ? groupFolder : folderName;

    emit finished(tr("Folder removed: %1 (%n application(s))", "", removedCount).arg(folderLabel));
}

void AppSplitTunnelingController::clearPendingBypassLaunch()
{
    if (m_pendingBypassLaunchConnection) {
        disconnect(m_pendingBypassLaunchConnection);
        m_pendingBypassLaunchConnection = QMetaObject::Connection();
    }

    m_pendingLaunchTargetPath.clear();
    m_pendingLaunchExecutablePath.clear();
}

void AppSplitTunnelingController::showGlobalError(const QString &errorMessage, bool raiseWindow)
{
    if (m_pageController) {
        if (raiseWindow) {
            QMetaObject::invokeMethod(m_pageController, "raiseMainWindow", Qt::QueuedConnection);
        }

        QMetaObject::invokeMethod(
            m_pageController,
            "showErrorMessage",
            Qt::QueuedConnection,
            Q_ARG(QString, errorMessage));
        return;
    }

    emit errorOccurred(errorMessage);
}

bool AppSplitTunnelingController::finishPendingBypassLaunch()
{
#ifndef Q_OS_WIN
    clearPendingBypassLaunch();
    return false;
#else
    const QString targetPath = m_pendingLaunchTargetPath;
    clearPendingBypassLaunch();

    if (!launchTargetWithShell(targetPath)) {
        showGlobalError(tr("Split tunneling settings were updated, but the selected file could not be started"));
        return false;
    }

    return true;
#endif
}

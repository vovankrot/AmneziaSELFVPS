#ifndef APPSPLITTUNNELINGCONTROLLER_H
#define APPSPLITTUNNELINGCONTROLLER_H

#include <QMetaObject>
#include <QObject>
#include <QPointer>

#include "settings.h"
#include "ui/models/appSplitTunnelingModel.h"

class ConnectionController;
class PageController;
class ServersModel;

class AppSplitTunnelingController : public QObject
{
    Q_OBJECT
public:
    Q_PROPERTY(bool folderScanBusy READ folderScanBusy NOTIFY folderScanBusyChanged)
    bool folderScanBusy() const { return m_folderScanBusy; }
    explicit AppSplitTunnelingController(const std::shared_ptr<Settings> &settings,
                                         const QSharedPointer<AppSplitTunnelingModel> &sitesModel,
                                         ConnectionController *connectionController,
                                         PageController *pageController,
                                         ServersModel *serversModel,
                                         QObject *parent = nullptr);

public slots:
    void addApp(const QString &appPath);
    void addApps(QVector<QPair<QString, QString>> apps);
    void addAppsFromFolder(const QString &folderPath);
    void confirmFolderApps(int token);
    void discardFolderApps(int token);
    void launchTargetBypassingVpn(const QString &targetPath);
    void removeApp(const int index);
    void removeGroup(const QString &groupFolder);

signals:
    void folderScanBusyChanged();
    void folderScanReady(int token, const QString &folder, const QStringList &names, int count);
    void errorOccurred(const QString &errorMessage);
    void finished(const QString &message);

private:
    bool m_folderScanBusy = false;
    int m_folderScanToken = 0;
    QVector<amnezia::InstalledAppInfo> m_pendingFolderApps;
    void clearPendingBypassLaunch();
    void showGlobalError(const QString &errorMessage, bool raiseWindow = true);
    bool finishPendingBypassLaunch();

    std::shared_ptr<Settings> m_settings;

    QSharedPointer<AppSplitTunnelingModel> m_appSplitTunnelingModel;
    QPointer<ConnectionController> m_connectionController;
    QPointer<PageController> m_pageController;
    QPointer<ServersModel> m_serversModel;

    QMetaObject::Connection m_pendingBypassLaunchConnection;
    QString m_pendingLaunchTargetPath;
    QString m_pendingLaunchExecutablePath;
};

#endif // APPSPLITTUNNELINGCONTROLLER_H

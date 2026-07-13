#ifndef APPSPLITTUNNELINGMODEL_H
#define APPSPLITTUNNELINGMODEL_H

#include <QAbstractListModel>

#include "settings.h"
#include "core/defs.h"

class AppSplitTunnelingModel: public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        AppPathRole = Qt::UserRole + 1,
        PackageAppNameRole,
        PackageAppIconRole,
        UseVpnRole,
        GroupFolderRole
    };

    struct AppEntry {
        amnezia::InstalledAppInfo info;
        bool useVpn = false; // false = bypass VPN (default for newly added)
    };

    explicit AppSplitTunnelingModel(std::shared_ptr<Settings> settings, QObject *parent = nullptr);

    Q_INVOKABLE QString stateSignature() const;
    Q_PROPERTY(bool supportsPerAppVpnToggle READ supportsPerAppVpnToggle CONSTANT)

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;

    Q_PROPERTY(bool isTunnelingEnabled READ isSplitTunnelingEnabled NOTIFY splitTunnelingToggled)

public slots:
    bool addApp(const amnezia::InstalledAppInfo &appInfo);
    void removeApp(QModelIndex index);
    int removeGroup(const QString &groupFolder);
    int groupCount(const QString &groupFolder) const;
    void clearAppsList();

    void toggleAppVpn(int row);

    bool isSplitTunnelingEnabled();
    bool supportsPerAppVpnToggle() const;
    void toggleSplitTunneling(bool enabled);

signals:
    void splitTunnelingToggled();

protected:
    QHash<int, QByteArray> roleNames() const override;

private:
    void loadAllApps();
    void persistApps();
    QString effectiveGroupForRow(int row) const;
    void rebuildGroupCache() const;
    void invalidateGroupCache();
    QString appDisplayName(const amnezia::InstalledAppInfo &appInfo) const;
    bool containsApp(const amnezia::InstalledAppInfo &appInfo) const;

    std::shared_ptr<Settings> m_settings;

    bool m_isSplitTunnelingEnabled;

    QVector<AppEntry> m_apps;

    // Effective display group per row: the stored source folder (or the exe's own
    // directory for legacy/single adds), collapsed into the shallowest ancestor
    // folder that is itself a group — so subfolder scans merge under their parent.
    mutable QVector<QString> m_effectiveGroupByRow;
    mutable bool m_groupCacheDirty = true;
};

#endif // APPSPLITTUNNELINGMODEL_H

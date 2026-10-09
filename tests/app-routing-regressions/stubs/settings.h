#pragma once
#include <memory>
#include <QMap>
#include "core/defs.h"

// In-memory persistence double: tests never read or change the user's settings.
class Settings {
public:
    enum AppsRouteMode { VpnAllApps, VpnOnlyForwardApps, VpnAllExceptApps };
    int writes = 0;
    QMap<int, QVector<amnezia::InstalledAppInfo>> apps;
    bool isAppsSplitTunnelingEnabled() const { return true; }
    AppsRouteMode getAppsRouteMode() const { return VpnAllExceptApps; }
    void setAppsRouteMode(AppsRouteMode) {}
    void setAppsSplitTunnelingEnabled(bool) {}
    QVector<amnezia::InstalledAppInfo> getVpnApps(AppsRouteMode mode) const { return apps.value(mode); }
    void setVpnApps(AppsRouteMode mode, const QVector<amnezia::InstalledAppInfo> &value) { ++writes; apps[mode] = value; }
};

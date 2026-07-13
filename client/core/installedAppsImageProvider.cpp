#include "installedAppsImageProvider.h"

#include "platforms/android/android_controller.h"

InstalledAppsImageProvider::InstalledAppsImageProvider() : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage InstalledAppsImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    return AndroidController::instance()->getAppIcon(id, size, requestedSize);
}

#ifndef INSTALLEDAPPSIMAGEPROVIDER_H
#define INSTALLEDAPPSIMAGEPROVIDER_H

#include <QImage>
#include <QObject>
#include <QQuickImageProvider>

class InstalledAppsImageProvider : public QQuickImageProvider
{
public:
    InstalledAppsImageProvider();

    // Image (not Pixmap) provider: QML calls requestImage() on its image-reader thread
    // (when the Image element is asynchronous), so decoding an app icon never blocks the
    // UI thread — fixes the app-split picker freeze/jank.
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};

#endif // INSTALLEDAPPSIMAGEPROVIDER_H

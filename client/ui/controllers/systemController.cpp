#include "systemController.h"

#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QQuickItem>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent>

#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
    #include <QFileDialog>
#endif

#ifdef Q_OS_ANDROID
    #include "platforms/android/android_controller.h"
#endif

#if defined(Q_OS_IOS) || defined(MACOS_NE)
    #include "platforms/ios/ios_controller.h"
    #include <CoreFoundation/CoreFoundation.h>
#endif

SystemController::SystemController(const std::shared_ptr<Settings> &settings, QObject *parent)
    : QObject(parent), m_settings(settings)
{
}

void SystemController::saveFile(const QString &fileName, const QString &data)
{
#if defined Q_OS_ANDROID
    AndroidController::instance()->saveFile(fileName, data);
    return;
#endif

#ifdef Q_OS_IOS
    QUrl fileUrl = QDir::tempPath() + "/" + fileName;
    QFile file(fileUrl.toString());
#else
    QFile file(fileName);
#endif

    // todo check if save successful
    file.open(QIODevice::WriteOnly);
    file.write(data.toUtf8());
    file.close();

#ifdef Q_OS_IOS
    QStringList filesToSend;
    filesToSend.append(fileUrl.toString());
    // todo check if save successful
    IosController::Instance()->shareText(filesToSend);
    return;
#else
    QFileInfo fi(fileName);

#ifdef Q_OS_MAC
    const auto url = "file://" + fi.absoluteDir().absolutePath();
#else
    const auto url = fi.absoluteDir().absolutePath();
#endif

#ifndef MACOS_NE
    QDesktopServices::openUrl(url);
#endif
#endif
}

bool SystemController::readFile(const QString &fileName, QByteArray &data)
{
#ifdef Q_OS_ANDROID
    int fd = AndroidController::instance()->getFd(fileName);
    if (fd == -1) return false;
    QFile file;
    if(!file.open(fd, QIODevice::ReadOnly)) return false;
    data = file.readAll();
    AndroidController::instance()->closeFd();
#else
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) return false;
    data = file.readAll();
#endif
    return true;
}

bool SystemController::readFile(const QString &fileName, QString &data)
{
    QByteArray byteArray;
    if(!readFile(fileName, byteArray)) return false;
    data = byteArray;
    return true;
}

QString SystemController::getFileName(const QString &acceptLabel, const QString &nameFilter,
                                      const QString &selectedFile, const bool isSaveMode, const QString &defaultSuffix)
{
    QString fileName;
#ifdef Q_OS_ANDROID
    Q_ASSERT(!isSaveMode);
    return AndroidController::instance()->openFile(nameFilter);
#endif

#ifdef Q_OS_IOS

    fileName = IosController::Instance()->openFile();
    if (fileName.isEmpty()) {
        return fileName;
    }
    
    CFURLRef url = CFURLCreateWithFileSystemPath(
            kCFAllocatorDefault,
            CFStringCreateWithCharacters(0, reinterpret_cast<const UniChar *>(fileName.unicode()), fileName.length()),
            kCFURLPOSIXPathStyle, 0);

    if (!CFURLStartAccessingSecurityScopedResource(url)) {
        qDebug() << "Could not access path " << QUrl::fromLocalFile(fileName).toString();
    }

    return fileName;
#endif

    QObject *mainFileDialog = m_qmlRoot->findChild<QObject*>("mainFileDialog");
    if (!mainFileDialog) {
        return "";
    }

    mainFileDialog->setProperty("acceptLabel", QVariant::fromValue(acceptLabel));
    mainFileDialog->setProperty("nameFilters", QVariant::fromValue(QStringList(nameFilter)));
    mainFileDialog->setProperty("defaultSuffix", QVariant::fromValue(defaultSuffix));
    mainFileDialog->setProperty("isSaveMode", QVariant::fromValue(isSaveMode));
    if (!selectedFile.isEmpty()) {
        mainFileDialog->setProperty("selectedFile", QVariant::fromValue(QUrl::fromLocalFile(selectedFile)));
    } else {
        mainFileDialog->setProperty("selectedFile", QVariant::fromValue(QUrl()));
    }

    bool isFileDialogAccepted = false;
    QEventLoop wait;
    QObject::connect(this, &SystemController::fileDialogClosed, [&wait, &isFileDialogAccepted](const bool isAccepted) {
        isFileDialogAccepted = isAccepted;
        wait.quit();
    });
    QObject::connect(mainFileDialog, &QObject::destroyed, &wait, &QEventLoop::quit);

    if (!QMetaObject::invokeMethod(mainFileDialog, "open")) {
        QObject::disconnect(this, &SystemController::fileDialogClosed, nullptr, nullptr);
        return "";
    }

    wait.exec();
    QObject::disconnect(this, &SystemController::fileDialogClosed, nullptr, nullptr);

    if (!isFileDialogAccepted) {
        return "";
    }

    fileName = mainFileDialog->property("selectedFile").toString();
    return QUrl(fileName).toLocalFile();
}

void SystemController::openFileDialogAsync(const QString &acceptLabel, const QString &nameFilter,
                                           const QString &selectedFile, const bool isSaveMode, const QString &defaultSuffix)
{
#ifdef Q_OS_ANDROID
    Q_ASSERT(!isSaveMode);
    emit fileDialogFinished(AndroidController::instance()->openFile(nameFilter));
    return;
#endif

#ifdef Q_OS_IOS
    QString fileName = IosController::Instance()->openFile();
    if (!fileName.isEmpty()) {
        CFURLRef url = CFURLCreateWithFileSystemPath(
                kCFAllocatorDefault,
                CFStringCreateWithCharacters(0, reinterpret_cast<const UniChar *>(fileName.unicode()), fileName.length()),
                kCFURLPOSIXPathStyle, 0);

        if (!CFURLStartAccessingSecurityScopedResource(url)) {
            qDebug() << "Could not access path " << QUrl::fromLocalFile(fileName).toString();
        }
    }
    emit fileDialogFinished(fileName);
    return;
#endif

    QObject *mainFileDialog = m_qmlRoot->findChild<QObject*>("mainFileDialog");
    if (!mainFileDialog) {
        emit fileDialogFinished("");
        return;
    }

    mainFileDialog->setProperty("acceptLabel", QVariant::fromValue(acceptLabel));
    mainFileDialog->setProperty("nameFilters", QVariant::fromValue(QStringList(nameFilter)));
    mainFileDialog->setProperty("defaultSuffix", QVariant::fromValue(defaultSuffix));
    mainFileDialog->setProperty("isSaveMode", QVariant::fromValue(isSaveMode));
    if (!selectedFile.isEmpty()) {
        mainFileDialog->setProperty("selectedFile", QVariant::fromValue(QUrl::fromLocalFile(selectedFile)));
    } else {
        mainFileDialog->setProperty("selectedFile", QVariant::fromValue(QUrl()));
    }

    QObject::connect(this, &SystemController::fileDialogClosed, this,
                     [this, mainFileDialog](const bool isAccepted) {
                         if (!isAccepted) {
                             emit fileDialogFinished("");
                             return;
                         }

                         const QString fileName = mainFileDialog->property("selectedFile").toString();
                         emit fileDialogFinished(QUrl(fileName).toLocalFile());
                     },
                     static_cast<Qt::ConnectionType>(Qt::SingleShotConnection));

    QTimer::singleShot(0, mainFileDialog, [mainFileDialog]() {
        QMetaObject::invokeMethod(mainFileDialog, "open");
    });
}

void SystemController::openDirectoryDialogAsync(const QString &acceptLabel, const QString &selectedDirectory)
{
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    emit fileDialogFinished("");
    return;

#else

    auto *dialog = new QFileDialog(nullptr, acceptLabel);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setFileMode(QFileDialog::Directory);
    dialog->setOption(QFileDialog::ShowDirsOnly, true);
    dialog->setAcceptMode(QFileDialog::AcceptOpen);

    if (!selectedDirectory.isEmpty()) {
        dialog->setDirectory(selectedDirectory);
    }

    QObject::connect(dialog, &QFileDialog::finished, this,
                     [this, dialog](int result) {
                         if (result != QDialog::Accepted) {
                             emit fileDialogFinished("");
                             return;
                         }

                         const QStringList selectedFiles = dialog->selectedFiles();
                         emit fileDialogFinished(selectedFiles.isEmpty() ? "" : selectedFiles.constFirst());
                     },
                     static_cast<Qt::ConnectionType>(Qt::SingleShotConnection));

    dialog->open();
#endif
}

void SystemController::setQmlRoot(QObject *qmlRoot)
{
    m_qmlRoot = qmlRoot;
}

bool SystemController::isAuthenticated()
{
#ifdef Q_OS_ANDROID
    return AndroidController::instance()->requestAuthentication();
#else
    return true;
#endif
}

void SystemController::sendTouch(float x, float y)
{
#ifdef Q_OS_ANDROID
    AndroidController::instance()->sendTouch(x, y);
#endif
}

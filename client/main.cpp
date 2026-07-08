#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTimer>

#include "amnezia_application.h"
#include "core/osSignalHandler.h"
#include "migrations.h"
#include "version.h"

#include <QTimer>

#ifdef Q_OS_WIN
    #include "Windows.h"
#endif

#if defined(Q_OS_IOS)
    #include "platforms/ios/QtAppDelegate-C-Interface.h"
#endif

#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS) && !defined(MACOS_NE)
namespace {
constexpr auto kSingleInstanceServerName = "AmneziaVPNInstance";
constexpr auto kSingleInstanceCommandKey = "command";
constexpr auto kSingleInstanceCommandRaise = "raise";
constexpr auto kSingleInstanceCommandLaunchBypassed = "launch-bypassed";

QByteArray buildSingleInstanceMessage(const QStringList &arguments)
{
    QJsonObject message;

    if (arguments.size() >= 3 && arguments.at(1) == "--launch-bypassed") {
        message.insert(kSingleInstanceCommandKey, kSingleInstanceCommandLaunchBypassed);
        message.insert("targetPath", arguments.at(2));
    } else {
        message.insert(kSingleInstanceCommandKey, kSingleInstanceCommandRaise);
    }

    return QJsonDocument(message).toJson(QJsonDocument::Compact);
}
}

bool isAnotherInstanceRunning(const QStringList &arguments)
{
    QLocalSocket socket;
    socket.connectToServer(kSingleInstanceServerName);
    if (socket.waitForConnected(500)) {
        const QByteArray message = buildSingleInstanceMessage(arguments);
        socket.write(message);
        socket.flush();
        socket.waitForBytesWritten(500);
        socket.disconnectFromServer();
        qWarning() << "AmneziaVPN is already running";
        return true;
    }
    return false;
}
#endif

int main(int argc, char *argv[])
{
    Migrations migrationsManager;
    migrationsManager.doMigrations();

#ifdef Q_OS_WIN
    AllowSetForegroundWindow(ASFW_ANY);
    // Ensure DLLs in app dir are found when loading QML plugins from subdirs
    {
        wchar_t exePath[MAX_PATH];
        if (GetModuleFileNameW(NULL, exePath, MAX_PATH)) {
            wchar_t *lastSlash = wcsrchr(exePath, L'\\');
            if (lastSlash) {
                *lastSlash = L'\0';
                SetDllDirectoryW(exePath);
            }
        }
    }
#endif

#ifdef Q_OS_ANDROID
    // QTBUG-95974 QTBUG-95764 QTBUG-102168
    qputenv("QT_ANDROID_DISABLE_ACCESSIBILITY", "1");
    qputenv("ANDROID_OPENSSL_SUFFIX", "_3");
#endif

    AmneziaApplication app(argc, argv);
    OsSignalHandler::setup();

#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS) && !defined(MACOS_NE)
    if (isAnotherInstanceRunning(app.arguments())) {
        QTimer::singleShot(1000, &app, [&]() { app.quit(); });
        return app.exec();
    }
    app.startLocalServer();
#endif

// Allow to raise app window if secondary instance launched
#ifdef Q_OS_WIN
    AllowSetForegroundWindow(0);
#endif

    app.registerTypes();

    app.setApplicationName(APPLICATION_NAME);
    app.setOrganizationName(ORGANIZATION_NAME);
    app.setApplicationDisplayName(APPLICATION_NAME);

    app.loadFonts();

    bool doExec = app.parseCommands();

    if (doExec) {
        app.init();

        qInfo().noquote() << QString("Started %1 version %2 %3").arg(APPLICATION_NAME, APP_VERSION, GIT_COMMIT_HASH);
        qInfo().noquote() << QString("%1 (%2)").arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture());
        qInfo().noquote() << QString("SSL backend: %1").arg(QSslSocket::sslLibraryVersionString());

        return app.exec();
    }
    return 0;
}

#include "amnezia_application.h"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMimeData>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QResource>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QTextDocument>
#include <QTimer>
#include <QTranslator>

#include "logger.h"
#include "ui/controllers/pageController.h"
#include "ui/models/installedAppsModel.h"
#include "version.h"

#include "protocols/qml_register_protocols.h"
#include <QtQuick/QQuickWindow>
#include <QWindow>

#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS) && !defined(MACOS_NE)
namespace {
constexpr auto kSingleInstanceServerName = "AmneziaVPNInstance";
constexpr auto kSingleInstanceCommandKey = "command";
constexpr auto kSingleInstanceCommandLaunchBypassed = "launch-bypassed";
}
#endif

bool AmneziaApplication::m_forceQuit = false;

AmneziaApplication::AmneziaApplication(int &argc, char *argv[])
    : AMNEZIA_BASE_CLASS(argc, argv),
      m_optAutostart({QStringLiteral("a"), QStringLiteral("autostart")}, QStringLiteral("System autostart")),
      m_optCleanup({QStringLiteral("c"), QStringLiteral("cleanup")}, QStringLiteral("Cleanup logs")),
      m_optConnect({QStringLiteral("connect")}, QStringLiteral("Connect to server by index on startup"), QStringLiteral("index")),
      m_optImport({QStringLiteral("import")}, QStringLiteral("Import configuration from data string"), QStringLiteral("data")),
      m_optLaunchBypass({QStringLiteral("launch-bypassed")}, QStringLiteral("Launch the selected file outside the VPN"), QStringLiteral("path"))
{
    setDesktopFileName(QStringLiteral(APPLICATION_NAME));
    setQuitOnLastWindowClosed(false);

    // Fix config file permissions
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    {
        QSettings s(ORGANIZATION_NAME, APPLICATION_NAME);
        s.setValue("permFixed", true);
    }

    QString configLoc1 = QStandardPaths::standardLocations(QStandardPaths::ConfigLocation).first() + "/" + ORGANIZATION_NAME + "/"
        + APPLICATION_NAME + ".conf";
    QFile::setPermissions(configLoc1, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    QString configLoc2 = QStandardPaths::standardLocations(QStandardPaths::ConfigLocation).first() + "/" + ORGANIZATION_NAME + "/"
        + APPLICATION_NAME + "/" + APPLICATION_NAME + ".conf";
    QFile::setPermissions(configLoc2, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#endif

    m_settings = std::shared_ptr<Settings>(new Settings);
    m_nam = new QNetworkAccessManager(this);
}

AmneziaApplication::~AmneziaApplication()
{
#ifdef AMNEZIA_DESKTOP
    if (m_vpnConnection && m_vpnConnectionThread.isRunning()) {
        QMetaObject::invokeMethod(m_vpnConnection.get(), "disconnectSlots", Qt::BlockingQueuedConnection);
        QMetaObject::invokeMethod(m_vpnConnection.get(), "disconnectFromVpn", Qt::BlockingQueuedConnection);
    }
#endif

    m_vpnConnectionThread.requestInterruption();
    m_vpnConnectionThread.quit();

    if (!m_vpnConnectionThread.wait(3000)) {
        m_vpnConnectionThread.terminate();
        m_vpnConnectionThread.wait(500);
    }

    if (m_engine) {
        delete m_engine;
    }
}

#ifdef Q_OS_ANDROID
namespace {
static void clearQtCaches()
{
    const QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (!cacheRoot.isEmpty()) {
        QDir(cacheRoot + "/QtShaderCache").removeRecursively();
        QDir(cacheRoot + "/qmlcache").removeRecursively();
    }
}
}
#endif

void AmneziaApplication::init()
{
    m_engine = new QQmlApplicationEngine;

    const QUrl url(QStringLiteral("qrc:/ui/qml/main2.qml"));
    QObject::connect(
        m_engine, &QQmlApplicationEngine::objectCreated, this,
        [this, url](QObject *obj, const QUrl &objUrl) {
            QString diagPath = QDir::tempPath() + "/AmneziaVPN_startup_diag.log";

            if (!obj && url == objUrl) {
                QFile diag(diagPath);
                if (diag.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    QTextStream ts(&diag);
                    ts << "QML load FAILED for: " << objUrl.toString() << "\n";
                    ts << "Timestamp: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
                    ts << "App dir: " << QCoreApplication::applicationDirPath() << "\n";
                    ts << "qt.conf exists: " << QFileInfo(QCoreApplication::applicationDirPath() + "/qt.conf").exists() << "\n";
                    ts << "Library paths: " << QCoreApplication::libraryPaths().join("; ") << "\n";
                    ts << "QML import paths: " << m_engine->importPathList().join("; ") << "\n";
                    ts << "QML plugin paths: " << m_engine->pluginPathList().join("; ") << "\n";
                    QDir appDir(QCoreApplication::applicationDirPath());
                    ts << "App dir listing: " << appDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot).join("; ") << "\n";
                    QDir qmlDir(QCoreApplication::applicationDirPath() + "/qml");
                    if (qmlDir.exists()) {
                        ts << "qml/ listing: " << qmlDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot).join("; ") << "\n";
                    } else {
                        ts << "qml/ directory: DOES NOT EXIST\n";
                    }
                    ts << "AppDataLocation: " << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) << "\n";
                    ts << "\n=== QML Warnings/Errors ===\n";
                    for (const auto &warning : m_qmlWarnings) {
                        ts << warning << "\n";
                    }
                    diag.close();
                }
                QCoreApplication::exit(-1);
                return;
            }

            auto win = qobject_cast<QQuickWindow *>(obj);
            if (win) {
                win->installEventFilter(this);
#ifdef Q_OS_ANDROID
                QObject::connect(win, &QQuickWindow::sceneGraphError,
                                 [](QQuickWindow::SceneGraphError, const QString &msg) {
                                     qWarning() << "Scene graph error (suppressed):" << msg;
                                 });
                win->setPersistentSceneGraph(true);
                win->setPersistentGraphics(true);
#endif
                win->show();
            }

            QFile diag(diagPath);
            if (diag.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                QTextStream ts(&diag);
                ts << "Timestamp: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
                ts << "QML loaded OK: " << objUrl.toString() << "\n";
                ts << "Object class: " << (obj ? obj->metaObject()->className() : "null") << "\n";
                ts << "Is QQuickWindow: " << (win != nullptr) << "\n";
                if (win) {
                    ts << "Window visible: " << win->isVisible() << "\n";
                    ts << "Window geometry: " << win->x() << "," << win->y() << " " << win->width() << "x" << win->height() << "\n";
                    ts << "Window opacity: " << win->opacity() << "\n";
                    ts << "Window flags: 0x" << Qt::hex << (int)win->flags() << Qt::dec << "\n";
                    ts << "Screen: " << (win->screen() ? win->screen()->name() : "null") << "\n";
                    if (win->screen()) {
                        ts << "Screen geometry: " << win->screen()->geometry().x() << "," << win->screen()->geometry().y() << " "
                           << win->screen()->geometry().width() << "x" << win->screen()->geometry().height() << "\n";
                    }
                }
                ts << "App dir: " << QCoreApplication::applicationDirPath() << "\n";
                ts << "qt.conf exists: " << QFileInfo(QCoreApplication::applicationDirPath() + "/qt.conf").exists() << "\n";
                ts << "Library paths: " << QCoreApplication::libraryPaths().join("; ") << "\n";
                ts << "Root objects: " << m_engine->rootObjects().size() << "\n";
                ts << "AppDataLocation: " << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) << "\n";
                diag.close();
            }
        },
        Qt::QueuedConnection);

    m_engine->rootContext()->setContextProperty("Debug", &Logger::Instance());

#ifdef MACOS_NE
    m_engine->rootContext()->setContextProperty("IsMacOsNeBuild", true);
#else
    m_engine->rootContext()->setContextProperty("IsMacOsNeBuild", false);
#endif

    m_vpnConnection.reset(new VpnConnection(m_settings));
    m_vpnConnection->moveToThread(&m_vpnConnectionThread);
    m_vpnConnectionThread.start();

    m_coreController.reset(new CoreController(m_vpnConnection, m_settings, m_engine));

    m_geoipUpdater = new GeoipUpdater(m_nam, m_settings, this);
    m_geoipUpdater->startPeriodicUpdates();

    m_blocklistUpdater = new BlocklistUpdater(m_nam, m_settings, this);
    m_blocklistUpdater->startPeriodicUpdates();

    m_engine->addImportPath("qrc:/ui/qml/Modules/");

    QObject::connect(m_engine, &QQmlEngine::warnings, this, [this](const QList<QQmlError> &warnings) {
        QString warnPath = QDir::tempPath() + "/AmneziaVPN_qml_warnings.log";
        QFile warningFile(warnPath);
        warningFile.open(QIODevice::WriteOnly | QIODevice::Append);
        QTextStream warningStream(&warningFile);
        for (const auto &warning : warnings) {
            const QString line = warning.toString();
            m_qmlWarnings.append(line);
            warningStream << line << "\n";
        }
        warningFile.close();
    });

    if (m_parser.isSet(m_optImport)) {
        const QString data = m_parser.value(m_optImport);
        if (!data.isEmpty() && m_coreController) {
            m_coreController->importConfigFromData(data);
        }
    }

    m_engine->load(url);

    m_coreController->setQmlRoot();

#ifndef Q_OS_ANDROID
    if (!Logger::init(false)) {
        qWarning() << "Initialization of debug subsystem failed";
    }
#endif
    Logger::setServiceLogsEnabled(m_settings->isSaveLogs());

#ifdef Q_OS_WIN
    if (m_parser.isSet(m_optAutostart)) {
        m_coreController->pageController()->showOnStartup();
    } else {
        emit m_coreController->pageController()->raiseMainWindow();
    }
#else
    m_coreController->pageController()->showOnStartup();
#endif

#ifdef Q_OS_ANDROID
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, []() {
        auto clipboard = QGuiApplication::clipboard();
        if (clipboard->mimeData()->hasHtml()) {
            clipboard->setText(clipboard->text());
        }
    });
#endif

    if (m_parser.isSet(m_optConnect)) {
        bool ok = false;
        const int index = m_parser.value(m_optConnect).toInt(&ok);
        if (ok) {
            QTimer::singleShot(0, this, [this, index]() {
                if (m_coreController) {
                    m_coreController->openConnectionByIndex(index);
                }
            });
        }
    }

    if (m_parser.isSet(m_optLaunchBypass)) {
        const QString targetPath = m_parser.value(m_optLaunchBypass).trimmed();
        if (!targetPath.isEmpty()) {
            m_pendingLaunchBypassedTargets.append(targetPath);
        }
    }

    m_isInitialized = true;
#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS) && !defined(MACOS_NE)
    processPendingSingleInstanceMessages();
#endif
}

void AmneziaApplication::registerTypes()
{
    qRegisterMetaType<ServerCredentials>("ServerCredentials");

    qRegisterMetaType<DockerContainer>("DockerContainer");
    qRegisterMetaType<TransportProto>("TransportProto");
    qRegisterMetaType<Proto>("Proto");
    qRegisterMetaType<ServiceType>("ServiceType");
    // ErrorCode is emitted across threads by ServerController/InstallController
    // signals; without runtime registration Qt drops the queued emit with
    // "Cannot queue arguments of type 'ErrorCode'" and the install UI hangs.
    qRegisterMetaType<amnezia::ErrorCode>("ErrorCode");
    qRegisterMetaType<amnezia::ErrorCode>("amnezia::ErrorCode");

    declareQmlProtocolEnum();
    declareQmlContainerEnum();

    m_containerProps.reset(new ContainerProps());
    qmlRegisterSingletonInstance("ContainerProps", 1, 0, "ContainerProps", m_containerProps.get());

    m_protocolProps.reset(new ProtocolProps());
    qmlRegisterSingletonInstance("ProtocolProps", 1, 0, "ProtocolProps", m_protocolProps.get());

    qmlRegisterSingletonType(QUrl("qrc:/ui/qml/Filters/ContainersModelFilters.qml"), "ContainersModelFilters", 1, 0,
                             "ContainersModelFilters");

    qmlRegisterType<InstalledAppsModel>("InstalledAppsModel", 1, 0, "InstalledAppsModel");

    Vpn::declareQmlVpnConnectionStateEnum();
    PageLoader::declareQmlPageEnum();
}

void AmneziaApplication::loadFonts()
{
    QQuickStyle::setStyle("Basic");
    // Inter is the redesign UI font (by vovankrot); pt-root-ui stays registered
    // as a fallback for any glyphs Inter may lack.
    QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");
    QFontDatabase::addApplicationFont(":/fonts/pt-root-ui_vf.ttf");
}

bool AmneziaApplication::parseCommands()
{
    m_parser.setApplicationDescription(APPLICATION_NAME);
    m_parser.addHelpOption();
    m_parser.addVersionOption();

    m_parser.addOption(m_optAutostart);
    m_parser.addOption(m_optCleanup);
    m_parser.addOption(m_optConnect);
    m_parser.addOption(m_optImport);
    m_parser.addOption(m_optLaunchBypass);

    m_parser.process(*this);

    if (m_parser.isSet(m_optCleanup)) {
        Logger::cleanUp();
        QTimer::singleShot(100, this, [this] { quit(); });
        exec();
        return false;
    }

    return true;
}

#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS) && !defined(MACOS_NE)
void AmneziaApplication::startLocalServer()
{
    const QString serverName(kSingleInstanceServerName);
    QLocalServer::removeServer(serverName);

    QLocalServer *server = new QLocalServer(this);
    server->listen(serverName);

    QObject::connect(server, &QLocalServer::newConnection, this, [server, this]() {
        if (!server) {
            return;
        }

        QLocalSocket *clientConnection = server->nextPendingConnection();
        if (!clientConnection) {
            return;
        }

        QObject::connect(clientConnection, &QLocalSocket::disconnected, this, [this, clientConnection]() {
            handleSingleInstanceMessage(clientConnection->readAll());
            clientConnection->deleteLater();
        });
    });
}

void AmneziaApplication::handleSingleInstanceMessage(const QByteArray &payload)
{
    const QJsonDocument document = QJsonDocument::fromJson(payload);
    const QJsonObject message = document.isObject() ? document.object() : QJsonObject();
    const QString command = message.value(kSingleInstanceCommandKey).toString();

    if (command == kSingleInstanceCommandLaunchBypassed) {
        const QString targetPath = message.value("targetPath").toString().trimmed();
        if (!targetPath.isEmpty()) {
            if (m_isInitialized) {
                launchTargetBypassingVpn(targetPath);
            } else {
                m_pendingLaunchBypassedTargets.append(targetPath);
            }
            return;
        }
    }

    if (m_isInitialized && m_coreController && m_coreController->pageController()) {
        emit m_coreController->pageController()->raiseMainWindow();
    } else {
        m_pendingRaiseMainWindow = true;
    }
}

void AmneziaApplication::processPendingSingleInstanceMessages()
{
    if (!m_isInitialized || !m_coreController || !m_coreController->pageController()) {
        return;
    }

    if (m_pendingRaiseMainWindow) {
        m_pendingRaiseMainWindow = false;
        emit m_coreController->pageController()->raiseMainWindow();
    }

    const QStringList pendingTargets = m_pendingLaunchBypassedTargets;
    m_pendingLaunchBypassedTargets.clear();

    for (const QString &targetPath : pendingTargets) {
        launchTargetBypassingVpn(targetPath);
    }
}

void AmneziaApplication::launchTargetBypassingVpn(const QString &targetPath)
{
    if (!m_coreController) {
        m_pendingLaunchBypassedTargets.append(targetPath);
        return;
    }

    QTimer::singleShot(0, this, [this, targetPath]() {
        if (m_coreController) {
            m_coreController->launchTargetBypassingVpn(targetPath);
        }
    });
}
#endif

bool AmneziaApplication::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Close) {
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
        quit();
#else
        if (m_forceQuit) {
            quit();
        } else if (m_coreController && m_coreController->pageController()) {
            m_coreController->pageController()->hideMainWindow();
        }
#endif
        return true;
    }

    return QObject::eventFilter(watched, event);
}

void AmneziaApplication::forceQuit()
{
    m_forceQuit = true;
    quit();
}

QQmlApplicationEngine *AmneziaApplication::qmlEngine() const
{
    return m_engine;
}

QNetworkAccessManager *AmneziaApplication::networkManager()
{
    return m_nam;
}

QClipboard *AmneziaApplication::getClipboard()
{
    return this->clipboard();
}

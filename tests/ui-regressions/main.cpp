#include <cmath>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQuickItem>
#include <QTest>
#include <QSignalSpy>
#include <QImage>
#include <QDir>
#include <QDebug>
#include <QQuickStyle>
#include <QFontDatabase>
#include <QQuickItemGrabResult>
#include <QTranslator>
#include <QQmlPropertyMap>
#include <functional>

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Basic");
    QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");
    QTranslator translator;
    if (!translator.load(QStringLiteral(TRANSLATION_DIR "/amneziavpn_ru_RU.qm"))) qFatal("Russian translation catalog missing");
    app.installTranslator(&translator);
    qmlRegisterModule("PageEnum", 1, 0);
    qmlRegisterModule("ConnectionState", 1, 0);
    qmlRegisterModule("ContainerProps", 1, 0);
    qmlRegisterModule("ContainerEnum", 1, 0);
    qmlRegisterModule("ProtocolEnum", 1, 0);
    qmlRegisterSingletonType(QUrl::fromLocalFile(QStringLiteral(SOURCE_ROOT "/Filters/ContainersModelFilters.qml")),
                            "ContainersModelFilters", 1, 0, "ContainersModelFilters");
    QQmlEngine engine;
    QStringList runtimeErrors;
    QObject::connect(&engine, &QQmlEngine::warnings, [&](const QList<QQmlError> &errors) {
        for (const auto &error : errors) {
            const auto text = error.toString();
            if (text.contains("ReferenceError") || text.contains("TypeError") || text.contains("Binding loop"))
                runtimeErrors.append(text);
        }
    });
    engine.addImportPath(QStringLiteral(SOURCE_ROOT "/Modules"));
    auto mock = [&](const char *name, const QByteArray &body) {
        QQmlComponent c(&engine);
        c.setData("import QtQuick\nQtObject { " + body + " }", QUrl());
        QObject *o = c.create();
        if (!o) qFatal("%s", qPrintable(c.errorString()));
        o->setParent(&engine);
        engine.rootContext()->setContextProperty(name, o);
        return o;
    };
    QObject *connection = mock("ConnectionController",
        "property bool isConnected: true; property bool isConnectionInProgress: false;"
        "property string actionButtonText: 'Отключиться'; property int clicks: 0;"
        "property string connectionStateText: isConnected ? 'Подключено' : 'Отключено';"
        "signal connectionStateChanged(); function reconnectToVpn() {}"
        "signal reconnectWithUpdatedContainer(string message);"
        "signal preparingConfig(); function connectButtonClicked() { clicks++; isConnected = !isConnected }");
    QObject *serversMock = mock("ServersModel", "property int defaultIndex: 0; property bool hasServersFromGatewayApi: false;"
        "property int processedIndex: 3; signal processedServerIndexChanged(); signal dataChanged();"
        "function canEditProcessedServerPassword() { return true } function isProcessedServerHasWriteAccess() { return true }"
        "function getProcessedServerData(k) { return k === 'credentialsLogin' ? 'fixture-user' : false }"
        "property int passwordWrites: 0; property string savedPassword: ''; property int savedIndex: -1;"
        "function updateProcessedServerPassword(i, p) { passwordWrites++; savedIndex=i; savedPassword=p; return true }"
        "function setProcessedServerIndex(i) {}");
    mock("SettingsController",
        "signal changeSettingsFinished(string message);"
        "property int safeAreaTopMargin: 0; property int safeAreaBottomMargin: 0; property int imeHeight: 0; property bool isAdvancedMode: true;"
        "property bool isDevModeEnabled: false; property bool startMinimized: false;"
        "property bool isLoggingEnabled: false; function isOnTv() { return false }"
        "property bool isDevGatewayEnv: false; property bool isAutoFailoverEnabled: false; property bool isAwgHeaderProtectionEnabled: false;"
        "function getAppVersion() { return '5.0.0' }"
        "function isAutoStartEnabled() { return false } function isAutoConnectEnabled() { return false }"
        "function isAutoDisableLoopbackProxyEnabled() { return false } function isAmneziaDnsEnabled() { return false }");
    QQmlComponent languages(&engine);
    languages.setData("import QtQuick\nListModel { property string currentLanguageName: 'Русский';"
        "property int currentLanguageIndex: 0; function getLineHeightAppend() { return 0 }"
        "ListElement { languageName: 'Русский'; languageCode: 'ru' } }", QUrl());
    QObject *languageModel = languages.create();
    languageModel->setParent(&engine);
    engine.rootContext()->setContextProperty("LanguageModel", languageModel);
    mock("NewsModel", "property bool hasUnread: false;");
    mock("ApiNewsController", "signal fetchNewsFinished(); signal errorOccurred(int errorCode, bool showError);");
    mock("FocusController", "function resetRootObject() {} function setFocusOnDefaultItem() {} function pushRootObject(o) {} function dropRootObject(o) {}");
    mock("PageController", "signal closeTopDrawer(); signal restorePageHomeState(); property int depth: 0; function getDrawerDepth() { return depth } function incrementDrawerDepth() { return ++depth } function decrementDrawerDepth() { --depth } function getInitialPageNavigationBarColor() { return 0xFF1C1D21 } function updateNavigationBarColor(c) {} function showNotificationMessage(message) {} function showBusyIndicator(b) {}");
    QQuickWindow window;
    window.resize(750, 680);
    window.setColor(QColor("#071019"));
    window.show();
    auto create = [&](const QString &file) {
        QQmlComponent c(&engine, QUrl::fromLocalFile(QStringLiteral(SOURCE_ROOT "/") + file));
        auto *item = qobject_cast<QQuickItem *>(c.create());
        if (!item) qFatal("%s", qPrintable(c.errorString()));
        item->setParentItem(window.contentItem());
        return item;
    };
    std::function<QQuickItem *(QQuickItem *, const QString &)> findVisualItem =
        [&](QQuickItem *parent, const QString &objectName) -> QQuickItem * {
            for (QQuickItem *child : parent->childItems()) {
                if (child->objectName() == objectName) return child;
                if (auto *nested = findVisualItem(child, objectName)) return nested;
            }
            return nullptr;
        };
    // A rejected selection must not move the radio; accepted model changes
    // update it even when the stored container is read through an invokable.
    QQmlComponent pickerFixture(&engine);
    pickerFixture.setData(R"(
        import QtQuick
        import "."
        Item {
            id: fixture
            width: parent ? parent.width : 800; height: parent ? parent.height : 670
            property var stored: ({container: 0})
            property int xrayRealitySwitcherRefresh: 0
            property bool acceptSwitch: false
            property int switchCalls: 0
            function currentContainer() {
                fixture.xrayRealitySwitcherRefresh
                return stored.container
            }
            ListModel {
                id: protocols
                ListElement { name: "AmneziaWG"; description: "Fixture"; dockerContainer: 0 }
                ListElement { name: "XRay"; description: "Fixture"; dockerContainer: 1 }
            }
            HomeProtocolListDrawer {
                objectName: "protocolTestDrawer"
                protocolsModel: protocols
                currentContainer: fixture.currentContainer()
                switchFunction: function(value) {
                    fixture.switchCalls++
                    if (fixture.acceptSwitch) {
                        fixture.stored.container=value
                        fixture.xrayRealitySwitcherRefresh++
                    }
                }
            }
        }
    )", QUrl::fromLocalFile(QStringLiteral(SOURCE_ROOT "/Components/protocol-fixture.qml")));
    auto *pickerRoot=qobject_cast<QQuickItem *>(pickerFixture.create());
    if (!pickerRoot) qFatal("%s",qPrintable(pickerFixture.errorString()));
    pickerRoot->setParentItem(window.contentItem());
    auto *picker=findVisualItem(pickerRoot,"protocolTestDrawer");
    auto openPicker=[&] { QMetaObject::invokeMethod(picker,"openTriggered"); QTest::qWait(400); };
    auto clickProtocol=[&](int value) {
        auto *option=findVisualItem(pickerRoot,"homeProtocolOption_"+QString::number(value));
        if (!option || !option->isVisible()) qFatal("Protocol option missing");
        QTest::mouseClick(&window,Qt::LeftButton,Qt::NoModifier,
                         option->mapToScene(QPointF(option->width()/2,option->height()/2)).toPoint());
        QTest::qWait(400);
    };
    openPicker(); clickProtocol(1);
    if (pickerRoot->property("switchCalls").toInt()!=1 || picker->property("currentContainer").toInt()!=0)
        qFatal("Rejected switch changed the protocol selection");
    pickerRoot->setProperty("acceptSwitch",true);
    openPicker(); clickProtocol(1); openPicker();
    auto *chosen=findVisualItem(pickerRoot,"homeProtocolOption_1");
    auto *previous=findVisualItem(pickerRoot,"homeProtocolOption_0");
    if (picker->property("currentContainer").toInt()!=1 || !chosen->property("checked").toBool()
        || previous->property("checked").toBool()) qFatal("Accepted switch did not redraw the protocol selection");
    connection->setProperty("isConnectionInProgress",true);
    clickProtocol(0);
    if (pickerRoot->property("switchCalls").toInt()!=2) qFatal("Busy protocol picker accepted a switch");
    connection->setProperty("isConnectionInProgress",false);
    delete pickerRoot;
    qInfo()<<"PASS: protocol selection follows accepted model updates and rejects busy clicks";
    auto *button = create("Components/ConnectButton.qml");
    button->setX(40); button->setY(40);
    button->setWidth(260);
    QTest::qWait(100);
    if (button->height() < 36) qFatal("Connect button has no click area");
    auto *background = button->findChild<QQuickItem *>("connectButtonBackground");
    if (!background || background->width() < 250 || background->height() < 36)
        qFatal("Connect button background collapsed");
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(170, 60));
    QTest::qWait(50);
    if (connection->property("clicks").toInt() != 1 || connection->property("isConnected").toBool())
        qFatal("Disconnect mouse click did not reach controller exactly once");
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(170, 60));
    QTest::qWait(50);
    if (connection->property("clicks").toInt() != 2 || !connection->property("isConnected").toBool())
        qFatal("Connect mouse click did not reach controller");
    qInfo() << "PASS: real ConnectButton pointer events, connected and disconnected states";
    delete button;

    auto *navigation = create("Components/DesktopTopNavigation.qml");
    navigation->setSize(QSizeF(750, 104));
    QSignalSpy settingsNavigationSpy(navigation, SIGNAL(settingsClicked()));
    QTest::qWait(100);
    auto *settingsNavigation = navigation->findChild<QQuickItem *>("desktopSettingsNavigation");
    if (!settingsNavigation || settingsNavigation->width() < 90) qFatal("Desktop navigation collapsed");
    auto navigationPoint = settingsNavigation->mapToScene(QPointF(settingsNavigation->width()/2, settingsNavigation->height()/2)).toPoint();
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, navigationPoint);
    QTest::qWait(50);
    if (settingsNavigationSpy.count() != 1) qFatal("Desktop navigation click was not dispatched");
    navigation->setProperty("currentIndex", 2);
    if (!settingsNavigation->property("selected").toBool()) qFatal("Desktop navigation selection did not update");
    auto navigationShot = window.contentItem()->grabToImage();
    bool navigationSaved = false;
    QObject::connect(navigationShot.data(), &QQuickItemGrabResult::ready, [&]() { navigationSaved = navigationShot->saveToFile("navigation-runtime.png"); });
    for (int i = 0; i < 20 && !navigationSaved; ++i) QTest::qWait(50);
    if (!navigationSaved) qFatal("Could not render desktop navigation image");
    qInfo() << "PASS: desktop top navigation geometry, pointer event and selection";
    delete navigation;

    auto *settings = create("Pages2/PageSettings.qml");
    settings->setSize(QSizeF(750, 680));
    QTest::qWait(150);
    auto *panel = settings->findChild<QQuickItem *>("generalSettingsPanel");
    if (!panel || panel->width() < 300)
        qFatal("Settings content collapsed");
    auto *serversSettingsNavigation = settings->findChild<QQuickItem *>("desktopServersSettingsNavigation");
    if (!serversSettingsNavigation || !serversSettingsNavigation->isVisible()
            || serversSettingsNavigation->width() < 180 || serversSettingsNavigation->height() < 36)
        qFatal("Desktop server settings entry is missing");
    qInfo() << "PASS: settings panel geometry" << panel->width() << panel->height();
    auto shot = window.contentItem()->grabToImage();
    bool saved = false;
    QObject::connect(shot.data(), &QQuickItemGrabResult::ready, [&]() { saved = shot->saveToFile("settings-runtime.png"); });
    for (int i = 0; i < 20 && !saved; ++i) QTest::qWait(50);
    if (!saved) qFatal("Could not render settings image");
    settings->setWidth(650);
    QTest::qWait(100);
    if (panel->width() < 300) qFatal("Settings panel collapsed at minimum width");
    qInfo() << "PASS: minimum-window settings layout";
    auto *networkSettings = settings->findChild<QQuickItem *>("settingsNetworkNavigation");
    const auto networkPoint = networkSettings->mapToScene(QPointF(networkSettings->width()/2, networkSettings->height()/2)).toPoint();
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, networkPoint);
    QTest::qWait(100);
    if (settings->property("settingsSection").toInt() != 1 || panel->property("contentHeight").toReal() < 250)
        qFatal("Network settings did not open in place");
    auto networkShot = window.contentItem()->grabToImage();
    bool networkSaved = false;
    QObject::connect(networkShot.data(), &QQuickItemGrabResult::ready, [&]() { networkSaved = networkShot->saveToFile("settings-network-runtime.png"); });
    for (int i = 0; i < 20 && !networkSaved; ++i) QTest::qWait(50);
    if (!networkSaved) qFatal("Network settings screenshot failed");
    settings->setProperty("settingsSection", 3);
    QTest::qWait(50);
    if (panel->property("contentY").toReal() != 0) qFatal("Settings section did not reset its scroll position");
    settings->setProperty("settingsSection", 0);
    QTest::qWait(50);
    qInfo() << "PASS: grouped settings switch in place and reset scrolling at minimum width";
    delete settings;
    auto model = [&](const char *name, const QByteArray &body) {
        QQmlComponent c(&engine);
        c.setData("import QtQuick\nListModel { " + body + " }", QUrl());
        auto *o = c.create();
        if (!o) qFatal("%s", qPrintable(c.errorString()));
        o->setParent(&engine);
        engine.rootContext()->setContextProperty(name, o);
    };
    for (const auto *name : {"ContainerEnum", "ProtocolEnum"}) {
        auto *values = new QQmlPropertyMap(&engine);
        int n = 0;
        for (const auto *key : {"Xray", "AnyTls", "Hysteria2", "Vpn", "Other"}) values->insert(key, n++);
        engine.rootContext()->setContextProperty(name, values);
    }
    mock("ContainerProps", "function defaultProtocol(c) { return 0 } function supportsSiteSplitTunneling(c) { return true } function containerFromString(s) { return 0 }"
         "function isSupportedConfigSharing(c) { return true } function supportsUserManagement(c) { return true }");
    QObject *installController = mock("InstallController",
         "signal scanServerFinished(bool found); signal rebootProcessedServerFinished(string message);"
         "signal removeAllContainersFinished(string message); signal cleanupServerFinished(string message);"
         "signal removeProcessedContainerFinished(string message);"
         "property bool shouldUseAnyTlsVariant: false; property bool hysteria2Updating: false;"
         "property bool hysteria2VersionChecking: false; property bool hysteria2UpdateAvailable: true;"
         "property string hysteria2InstalledVersion: 'v2.11.0'; property int hysteria2UpdateCalls: 0;"
         "function checkServerConfigUpdate(i) {} function checkHysteria2Version() {}"
         "function updateHysteria2() { hysteria2UpdateCalls++ }"
         "signal installLogMessage(string line); signal hotReconfigureFinished(string message, bool success);"
         "signal hysteria2UpdateFinished(bool success, string message);");
    mock("DiagnosticsController", "property bool isResolving: false; property bool hasIssues: false; property int issueCount: 0;"
         "property var currentIssue: ({}); signal issueResolved(string issueId, bool success, string message);");
    mock("SpeedTestController", "property bool isRunning: false; property string statusText: ''; property real downloadSpeed: 0;"
         "property real uploadSpeed: 0; property string serverPingText: ''; property string moscowPingText: '';");
    mock("SitesModel", "property bool isTunnelingEnabled: false;");
    mock("AppSplitTunnelingModel", "property bool isTunnelingEnabled: false;");
    QObject *exportController = mock("ExportController", "signal revokeConfigCompleted(); signal generateConfig(int type); signal exportErrorOccurred(int error);"
         "property bool clientsLoading: false; property int refreshes: 0; function updateClientManagementModel(c, s, force) { refreshes++ }");
    model("ServersModel", R"(
        property int defaultIndex: 0; property int processedIndex: 0
        property bool hasServersFromGatewayApi: false; property bool isDefaultServerFromApi: false
        property bool isDefaultServerDefaultContainerHasSplitTunneling: false
        property string defaultServerName: 'My VPS'; property string defaultServerDefaultContainerName: 'Hysteria 2'
        property string defaultServerObfuscationName: 'Salamander'; property string adHeader: ''; property string adDescription: ''
        property string defaultServerDescriptionCollapsed: 'My VPS'; property string defaultServerDescriptionExpanded: 'My VPS'
        property string defaultServerImagePathCollapsed: ''
        signal defaultServerContainersUpdated(var containers); signal defaultServerDefaultContainerChanged(int containerIndex)
        signal defaultServerIndexChanged()
        function getServersCount() { return count }
        function getDefaultServerData(k) { return k === 'defaultContainer' ? 0 : true }
        function getProcessedServerData(k) { return k === 'defaultContainer' ? 0 : false }
        function getProcessedServerCredentials() { return {} }
        function isDefaultServerHasWriteAccess() { return true }
        function isProcessedServerHasWriteAccess() { return true }
        ListElement { name: 'My VPS'; serverName: 'My VPS'; serverDescription: 'My VPS'; isServerFromGatewayApi: false; hasWriteAccess: true; hasInstalledContainers: true }
    )");
    const QByteArray containers = R"(
        function isInstalled(c) { return true }
        function getProcessedContainerIndex() { return 0 }
        function setProcessedContainerIndex(i) {}
        ListElement { name: 'Hysteria 2'; containerName: 'Hysteria 2'; description: 'Hysteria 2'; dockerContainer: 0; isInstalled: true; isShareable: true; serviceType: 3; isSupported: true; isInstallationAllowed: true }
    )";
    model("ContainersModel", containers);
    model("DefaultServerContainersModel", containers);
    model("Hysteria2ConfigModel", "ListElement { site: 'example.com'; port: '45474' }");
    model("ClientManagementModel", "ListElement { clientName: 'Laptop'; isOnline: true; latestClientIp: '192.0.2.5'; clientId: 'demo-client'; latestDestination: 'example.com'; searchText: 'Laptop'; latestActivity: ''; creationDate: ''; latestHandshake: ''; dataReceived: ''; dataSent: ''; allowedIps: ''; visitHistory: '' }");

    connection->setProperty("isConnected", false);
    auto *hysteriaSettings = create("Pages2/PageProtocolHysteria2Settings.qml");
    hysteriaSettings->setSize(QSizeF(750, 680));
    QTest::qWait(150);
    auto *hysteriaUpdateButton = findVisualItem(hysteriaSettings, "hysteriaUpdateButton");
    if (!hysteriaUpdateButton || !hysteriaUpdateButton->isVisible())
        qFatal("Hysteria update button is missing for an old server version");
    auto hysteriaUpdatePoint = hysteriaUpdateButton->mapToScene(
        QPointF(hysteriaUpdateButton->width()/2, hysteriaUpdateButton->height()/2)).toPoint();
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, hysteriaUpdatePoint);
    QTest::qWait(50);
    if (installController->property("hysteria2UpdateCalls").toInt() != 1)
        qFatal("Hysteria update click was not dispatched");
    QMetaObject::invokeMethod(installController, "installLogMessage", Q_ARG(QString, QStringLiteral("Downloading Hysteria v2.12.2")));
    QTest::qWait(50);
    auto *hysteriaLogPanel = findVisualItem(hysteriaSettings, "hysteriaUpdateLogPanel");
    if (!hysteriaLogPanel || !hysteriaLogPanel->isVisible()
            || !hysteriaSettings->property("updateLogText").toString().contains("Downloading Hysteria"))
        qFatal("Live Hysteria server log was not rendered");
    installController->setProperty("hysteria2InstalledVersion", "v2.12.2");
    installController->setProperty("hysteria2UpdateAvailable", false);
    QMetaObject::invokeMethod(installController, "hysteria2UpdateFinished",
                              Q_ARG(bool, true), Q_ARG(QString, QStringLiteral("Hysteria updated")));
    QTest::qWait(50);
    if (hysteriaUpdateButton->isVisible())
        qFatal("Hysteria update button remained visible after successful verification");
    qInfo() << "PASS: Hysteria version state, live server log and completed update UI";
    delete hysteriaSettings;

    for (const auto &page : {QString("Home"), QString("Share")}) {
        auto *item = create("Pages2/Page" + page + ".qml");
        item->setSize(QSizeF(750, 680));
        QTest::qWait(300);
        auto *desktopContent = item->findChild<QQuickItem *>(page == "Home" ? "desktopHomeView" : "desktopClientsHeader");
        if (!desktopContent || desktopContent->width() < 300) qFatal("Desktop content geometry failed");
        if (page == "Home") {
            auto *details = item->findChild<QQuickItem *>("desktopDetailsToggle");
            auto point = details->mapToScene(QPointF(details->width()/2, details->height()/2)).toPoint();
            QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, point);
            QTest::qWait(100);
            if (!item->property("desktopAdvancedExpanded").toBool()) qFatal("Connection details did not expand");
        } else {
            auto *refresh = item->findChild<QQuickItem *>("clientsRefreshButton");
            auto *clientRows = item->findChild<QQuickItem *>("clientsListView");
            if (!refresh || !clientRows) qFatal("Client refresh controls missing");
            exportController->setProperty("clientsLoading", true);
            QTest::qWait(50);
            if (refresh->isEnabled() || clientRows->isEnabled() || !item->isEnabled())
                qFatal("Client refresh must gate row actions without blocking navigation");
            exportController->setProperty("clientsLoading", false);
            auto refreshPoint = refresh->mapToScene(QPointF(refresh->width()/2, refresh->height()/2)).toPoint();
            const auto previousRefreshes = exportController->property("refreshes").toInt();
            QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, refreshPoint);
            QTest::qWait(50);
            if (exportController->property("refreshes").toInt() != previousRefreshes + 1)
                qFatal("Client refresh click was not dispatched exactly once");
            qInfo() << "PASS: client refresh click and loading gate preserve navigation";
            auto *addClient = item->findChild<QQuickItem *>("desktopAddClientButton");
            auto point = addClient->mapToScene(QPointF(addClient->width()/2, addClient->height()/2)).toPoint();
            QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, point);
            QTest::qWait(100);
            auto *back = item->findChild<QQuickItem *>("desktopClientsBackButton");
            if (!back || !back->isVisible()) qFatal("Add-client action did not open the form");
            point = back->mapToScene(QPointF(back->width()/2, back->height()/2)).toPoint();
            QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, point);
            QTest::qWait(100);
            if (!addClient->isVisible()) qFatal("Back-to-clients action did not restore the list");
        }
        item->setWidth(650);
        QTest::qWait(100);
        if (desktopContent->width() < 300) qFatal("Desktop content collapsed at minimum width");
        item->setWidth(750);
        auto capture = window.contentItem()->grabToImage();
        bool ready = false;
        QObject::connect(capture.data(), &QQuickItemGrabResult::ready, [&]() { ready = capture->saveToFile(page.toLower() + "-runtime.png"); });
        for (int i=0; i<20 && !ready; ++i) QTest::qWait(50);
        if (!ready) qFatal("Page render failed");
        qInfo() << "PASS: production page render, narrow geometry and navigation click" << page;
        delete item;
    }
    if (!runtimeErrors.isEmpty()) qFatal("%s", qPrintable(runtimeErrors.join("\n")));
    QObject *updater = mock("AppUpdater",
        "property string status: 'Available'; property string availableVersion: '5.0.0.9';"
        "property string releaseNotes: 'Release notes'; property bool busy: false; property bool ready: false;"
        "property int progress: 0; property int downloads: 0; property int installs: 0; property int cancellations: 0;"
        "function download() { downloads++; } function install() { installs++; }"
        "function cancel() { cancellations++; busy = false; } function openRelease() {}");
    QQmlComponent updateComponent(&engine, QUrl::fromLocalFile(QStringLiteral(SOURCE_ROOT "/Components/AppUpdateDialog.qml")));
    QObject *dialog = updateComponent.create();
    if (!dialog) qFatal("%s", qPrintable(updateComponent.errorString()));
    dialog->setProperty("parent", QVariant::fromValue(window.contentItem()));
    if (!QMetaObject::invokeMethod(dialog, "open")) qFatal("Update dialog did not open");
    QTest::qWait(150);
    auto *primary = dialog->findChild<QQuickItem *>("selfVpsUpdatePrimary");
    if (!primary || !primary->isVisible()) qFatal("Update action missing");
    auto clickUpdate = [&] {
        auto point = primary->mapToScene(QPointF(primary->width()/2, primary->height()/2)).toPoint();
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, point); QTest::qWait(50);
    };
    clickUpdate();
    if (updater->property("downloads").toInt() != 1 || updater->property("installs").toInt()) qFatal("Download action prematurely installed update");
    updater->setProperty("busy", true); clickUpdate();
    if (updater->property("downloads").toInt() != 1) qFatal("Busy update accepted another download");
    updater->setProperty("busy", false); updater->setProperty("ready", true); clickUpdate();
    if (updater->property("installs").toInt() != 1) qFatal("Verified installer action failed");
    QMetaObject::invokeMethod(dialog, "close");
    QTest::qWait(200);
    updater->setProperty("busy", true);
    QMetaObject::invokeMethod(dialog, "open");
    QTest::qWait(150);
    QTest::keyClick(&window, Qt::Key_Escape);
    if (updater->property("cancellations").toInt() != 1)
        qFatal("Escape cancellation waited for the closing animation");
    QTest::qWait(250);
    if (dialog->property("opened").toBool() || updater->property("cancellations").toInt() != 1)
        qFatal("Escape did not cancel the pending updater operation");
    delete dialog;
    if (!runtimeErrors.isEmpty()) qFatal("%s", qPrintable(runtimeErrors.join("\n")));
    qInfo() << "PASS: update dialog download, busy gate, explicit installation and Escape cancellation";
    QObject *previousServersContext = engine.rootContext()->contextProperty("ServersModel").value<QObject *>();
    engine.rootContext()->setContextProperty("ServersModel", serversMock);
    auto *serverManagementPage = create("Pages2/PageSettingsServerData.qml");
    serverManagementPage->setSize(QSizeF(750, 680));
    QTest::qWait(100);
    if (!serverManagementPage->property("canEditPassword").toBool()) qFatal("Password action hidden for editable server");
    delete serverManagementPage;
    QQmlComponent passwordComponent(&engine, QUrl::fromLocalFile(QStringLiteral(SOURCE_ROOT "/Components/ServerPasswordDialog.qml")));
    QObject *passwordDialog = passwordComponent.create();
    if (!passwordDialog) qFatal("%s", qPrintable(passwordComponent.errorString()));
    passwordDialog->setProperty("parent", QVariant::fromValue(window.contentItem()));
    passwordDialog->setProperty("serverIndex", 3);
    passwordDialog->setProperty("login", "fixture-user");
    QMetaObject::invokeMethod(passwordDialog, "open");
    QTest::qWait(150);
    if (!window.grabWindow().save("server-password-runtime.png")) qFatal("Password dialog render failed");
    auto *passwordInput = passwordDialog->findChild<QObject *>("serverPasswordInput");
    auto *passwordConfirm = passwordDialog->findChild<QObject *>("serverPasswordConfirmation");
    auto *passwordSave = passwordDialog->findChild<QQuickItem *>("serverPasswordSave");
    if (!passwordInput || !passwordConfirm || !passwordSave || passwordSave->isEnabled()
        || !passwordInput->property("text").toString().isEmpty() || passwordInput->property("echoMode").toInt() != 2)
        qFatal("Password dialog leaked old text or enabled empty save");
    passwordInput->setProperty("text", "fixture password");
    passwordConfirm->setProperty("text", "different");
    QTest::qWait(20);
    if (passwordSave->isEnabled()) qFatal("Mismatched password accepted");
    passwordConfirm->setProperty("text", "fixture password");
    QTest::qWait(20);
    const auto passwordPoint = passwordSave->mapToScene(QPointF(passwordSave->width()/2, passwordSave->height()/2)).toPoint();
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, passwordPoint);
    QTest::qWait(150);
    if (serversMock->property("passwordWrites").toInt() != 1 || serversMock->property("savedIndex").toInt() != 3
        || serversMock->property("savedPassword").toString() != "fixture password" || !passwordInput->property("text").toString().isEmpty())
        qFatal("Password save lost target/value or failed to clear input");
    QMetaObject::invokeMethod(passwordDialog, "open");
    QTest::qWait(100);
    passwordInput->setProperty("text", "cancel fixture");
    QTest::keyClick(&window, Qt::Key_Escape);
    QTest::qWait(150);
    if (serversMock->property("passwordWrites").toInt() != 1 || !passwordInput->property("text").toString().isEmpty())
        qFatal("Password cancellation saved or retained secret");
    delete passwordDialog;
    engine.rootContext()->setContextProperty("ServersModel", previousServersContext);
    qInfo() << "PASS: saved SSH password masked, confirmed, scoped to server, cleared on save/Escape";
    QObject *trustController = mock("SshHostTrust",
        "property int answers: 0; property bool lastAnswer: false;"
        "function answer(accepted) { answers++; lastAnswer = accepted }");
    QQmlComponent trustFixtureComponent(&engine);
    trustFixtureComponent.setData(R"(
        import QtQuick
        import "Components"
        import "Controls2"
        Item {
            width: 750
            height: 680
            property bool loadingRequested: true
            BusyIndicatorType { objectName: "sshTestBusy"; z: 1; visible: parent.loadingRequested }
            SshHostTrustDialog {
                id: trust
                endpoint: "test.example:22"
                fingerprint: "SHA256:test-only-fingerprint"
            }
            function requestTrust() { trust.open() }
        }
    )", QUrl::fromLocalFile(QStringLiteral(SOURCE_ROOT "/ssh-trust-busy-fixture.qml")));
    auto *trustFixture = qobject_cast<QQuickItem *>(trustFixtureComponent.create());
    if (!trustFixture) qFatal("%s", qPrintable(trustFixtureComponent.errorString()));
    trustFixture->setParentItem(window.contentItem());
    trustFixture->setSize(QSizeF(750, 680));
    QTest::qWait(100);
    for (const bool accept : {true, false}) {
        QMetaObject::invokeMethod(trustFixture, "requestTrust");
        QTest::qWait(150);
        // Reopen the loader after the decision to reproduce late busy signals.
        trustFixture->setProperty("loadingRequested", false);
        QTest::qWait(50);
        trustFixture->setProperty("loadingRequested", true);
        QTest::qWait(100);
        auto *trustButton = findVisualItem(window.contentItem(),
            accept ? "sshTrustYesButton" : "sshTrustNoButton");
        if (!trustButton || !trustButton->isVisible()) qFatal("SSH decision button missing");
        if (accept && !window.grabWindow().save("ssh-trust-runtime.png")) qFatal("SSH confirmation render failed");
        const int previousAnswers = trustController->property("answers").toInt();
        const auto point = trustButton->mapToScene(QPointF(trustButton->width()/2, trustButton->height()/2)).toPoint();
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, point);
        QTest::qWait(100);
        if (trustController->property("answers").toInt() != previousAnswers + 1
            || trustController->property("lastAnswer").toBool() != accept)
            qFatal("Modal loader intercepted SSH decision click");
        auto *loader = trustFixture->findChild<QObject *>("sshTestBusy");
        if (!loader || !loader->property("visible").toBool()) qFatal("SSH decision lost pending loading state");
    }
    delete trustFixture;
    if (!runtimeErrors.isEmpty()) qFatal("%s", qPrintable(runtimeErrors.join("\n")));
    qInfo() << "PASS: SSH Yes/No mouse clicks above existing and reopened modal loader; pending loading preserved";
    QQmlComponent localizedTrustComponent(&engine, QUrl::fromLocalFile(QStringLiteral(SOURCE_ROOT "/Components/SshHostTrustDialog.qml")));
    QObject *localizedTrust = localizedTrustComponent.create();
    if (!localizedTrust) qFatal("%s", qPrintable(localizedTrustComponent.errorString()));
    localizedTrust->setProperty("parent", QVariant::fromValue(window.contentItem()));
    QMetaObject::invokeMethod(localizedTrust, "open");
    QTest::qWait(100);
    if (localizedTrust->property("yesText").toString() != QString::fromUtf8("Да")
        || QCoreApplication::translate("PageShare", "Online") != QString::fromUtf8("Онлайн"))
        qFatal("Russian SSH/client labels were not translated");
    app.removeTranslator(&translator);
    if (!translator.load(QStringLiteral(TRANSLATION_DIR "/amneziavpn_en_US.qm"))) qFatal("English translation catalog missing");
    app.installTranslator(&translator);
    engine.retranslate();
    QTest::qWait(100);
    if (localizedTrust->property("title").toString() != "First connection to the VPS"
        || localizedTrust->property("yesText").toString() != "Yes"
        || QCoreApplication::translate("AppUpdater", "Проверяем релизы GitHub…") != "Checking GitHub releases…")
        qFatal("English language left Russian UI strings");
    app.removeTranslator(&translator);
    if (!translator.load(QStringLiteral(TRANSLATION_DIR "/amneziavpn_ru_RU.qm"))) qFatal("Russian translation reload failed");
    app.installTranslator(&translator);
    engine.retranslate();
    QTest::qWait(100);
    if (localizedTrust->property("yesText").toString() != QString::fromUtf8("Да")) qFatal("Switching back to Russian did not retranslate QML");
    QMetaObject::invokeMethod(localizedTrust, "close");
    delete localizedTrust;
    qInfo() << "PASS: compiled Russian/English catalogs and live QML retranslation in both directions";
    if (QCoreApplication::translate("PageSettingsAppSplitTunneling", "Add") != QString::fromUtf8("Добавить"))
        qFatal("Folder confirmation action is not translated into Russian");
    QQmlComponent longQuestionComponent(&engine);
    longQuestionComponent.setData(R"(
        import QtQuick
        import "Components"
        Item {
            id: fixture
            property int yesClicks: 0
            property int noClicks: 0
            QuestionDrawer {
                id: question
                objectName: "longQuestionDrawer"
                anchors.fill: parent
                headerText: "Add applications from folder with a long name to VPN bypass?"
                descriptionText: Array(100).join("components/long-folder-name/RiotClientServices.exe\n")
                yesButtonText: "Add"
                noButtonText: "Cancel"
                yesButtonFunction: function() { fixture.yesClicks++ }
                noButtonFunction: function() { fixture.noClicks++ }
            }
            function showQuestion() { question.openTriggered() }
            function closeQuestion() { question.closeTriggered() }
        }
    )", QUrl::fromLocalFile(QStringLiteral(SOURCE_ROOT "/long-question-fixture.qml")));
    auto *longQuestion = qobject_cast<QQuickItem *>(longQuestionComponent.create());
    if (!longQuestion) qFatal("%s", qPrintable(longQuestionComponent.errorString()));
    longQuestion->setParentItem(window.contentItem());
    for (const QSize size : {QSize(400, 360), QSize(750, 680)}) {
        window.resize(size);
        longQuestion->setSize(size);
        QMetaObject::invokeMethod(longQuestion, "showQuestion");
        QTest::qWait(300);
        auto *drawer = longQuestion->findChild<QObject *>("longQuestionDrawer");
        if (!std::isfinite(drawer->property("expandedHeight").toDouble())) qFatal("Question height is not finite");
        for (const QString &name : {QString("questionYesButton"), QString("questionNoButton")}) {
            auto *item = findVisualItem(longQuestion, name);
            if (!item || !item->isVisible()) qFatal("Question button missing");
            const QPointF top = item->mapToScene(QPointF(0, 0));
            if (top.y() < 0 || top.y() + item->height() > size.height()) { qInfo() << size << name << top << item->height(); qFatal("Question button outside viewport"); }
            const auto point = item->mapToScene(QPointF(item->width()/2, item->height()/2)).toPoint();
            QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, point);
        }
        if (!window.grabWindow().save("folder-preview-scroll.png")) qFatal("Question preview render failed");
        QMetaObject::invokeMethod(longQuestion, "closeQuestion");
        QTest::qWait(250);
    }
    if (longQuestion->property("yesClicks").toInt() != 2 || longQuestion->property("noClicks").toInt() != 2)
        qFatal("Long question buttons did not receive clicks");
    delete longQuestion;
    qInfo() << "PASS: long folder preview scrolls while Add/Cancel remain clickable at minimum sizes";
    mock("GeoipController", "property bool usingBundledList: true; property bool updating: false;"
        "signal statusChanged();"
        "property int cidrCount: 1; property int intervalHours: 24; property string lastError: '';"
        "property string sourceUrl: ''; property string lastUpdateText: ''; property string listPath: '';"
        "function updateNow() {} function resetSourceToDefault() {}");
    mock("SitesController", "signal finished(string message); signal errorOccurred(string message);");
    model("IpIntervalsModel", "property int length: 0;");
    model("SitesModel", "property bool isTunnelingEnabled: true; property int routeMode: 1;"
        "property bool bypassRuGeoIp: false; property bool bypassRuGeoSites: false; property bool autoBypassRkn: false;"
        "signal sitesChanged(); signal splitTunnelingToggled();"
        "function stateSignature() { return routeMode + ':' + isTunnelingEnabled }"
        "function toggleSplitTunneling(enabled) { isTunnelingEnabled=enabled; if (enabled && routeMode===0) routeMode=1; splitTunnelingToggled() }"
        "ListElement { url: '*.fixture.invalid'; ip: '' }");
    QObject *sites = engine.rootContext()->contextProperty("SitesModel").value<QObject *>();
    auto *sitePage = create("Pages2/PageSettingsSplitTunneling.qml");
    sitePage->setSize(QSizeF(750, 680)); QTest::qWait(100);
    auto *siteSelector = findVisualItem(sitePage, "siteRoutingModeSelector");
    auto *siteExplanation = findVisualItem(sitePage, "siteRoutingModeExplanation");
    if (!siteSelector || !siteExplanation || siteSelector->mapToScene(QPointF()).y() > 300 || siteSelector->mapToScene(QPointF()).y() < 0)
        qFatal("Site mode choice is not visible near the page header");
    const auto modeOneText = siteSelector->property("text").toString();
    sites->setProperty("routeMode", 2); QTest::qWait(30);
    if (siteSelector->property("text").toString() == modeOneText
        || siteExplanation->property("textString").toString() != QCoreApplication::translate("PageSettingsSplitTunneling", "Sites in the list below will bypass VPN. Everything else goes through VPN."))
        qFatal("Site mode selector and explanation disagree after external mode change");
    QMetaObject::invokeMethod(siteSelector, "openTriggered"); QTest::qWait(250);
    auto *options = findVisualItem(window.contentItem(), "siteRoutingModeOptions");
    QQuickItem *choice = nullptr;
    if (!options || !QMetaObject::invokeMethod(options, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, choice), Q_ARG(int, 0)) || !choice)
        qFatal("Site routing radio choices are missing");
    auto *radio = choice->property("selectable").value<QQuickItem *>();
    if (!radio) qFatal("Site routing radio is missing");
    const auto point = radio->mapToScene(QPointF(radio->width()/2, radio->height()/2)).toPoint();
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, point); QTest::qWait(250);
    if (sites->property("routeMode").toInt() != 1 || siteSelector->property("text").toString() != modeOneText)
        qFatal("Site mode mouse click did not select VPN for listed sites");
    if (siteSelector->mapToScene(QPointF()).y() < 0 || !siteSelector->isVisible())
        qFatal("Site mode choice scrolled out of view after selection");
    if (findVisualItem(sitePage, "siteSplitListView")->height() < 200) qFatal("Site list viewport collapsed");
    if (!window.grabWindow().save("site-mode-runtime.png")) qFatal("Site mode render failed");
    sites->setProperty("isTunnelingEnabled", false); QTest::qWait(30);
    if (siteSelector->isEnabled() || siteExplanation->property("textString").toString() != QCoreApplication::translate("PageSettingsSplitTunneling", "Раздельное туннелирование сайтов выключено. Список не применяется."))
        qFatal("Disabled split tunneling still advertises an active list mode");
    sitePage->setProperty("isApplicationQuitting", true); delete sitePage;
    if (!runtimeErrors.isEmpty()) qFatal("%s", qPrintable(runtimeErrors.join("\n")));
    qInfo() << "PASS: site mode choice at top, live explanation, radio click and disabled state";
    return 0;
}

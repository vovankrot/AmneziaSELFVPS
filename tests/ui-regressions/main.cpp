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
        "signal reconnectWithUpdatedContainer(string message);"
        "signal preparingConfig(); function connectButtonClicked() { clicks++; isConnected = !isConnected }");
    mock("ServersModel", "property int defaultIndex: 0; property bool hasServersFromGatewayApi: false;"
        "function setProcessedServerIndex(i) {}");
    mock("SettingsController",
        "property int safeAreaTopMargin: 0; property bool isAdvancedMode: true;"
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
    mock("FocusController", "function resetRootObject() {} function setFocusOnDefaultItem() {}");
    mock("PageController", "signal closeTopDrawer(); signal restorePageHomeState(); function showNotificationMessage(message) {} function showBusyIndicator(b) {}");
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
    mock("ContainerProps", "function supportsSiteSplitTunneling(c) { return true } function containerFromString(s) { return 0 }"
         "function isSupportedConfigSharing(c) { return true } function supportsUserManagement(c) { return true }");
    QObject *installController = mock("InstallController",
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
        "property int progress: 0; property int downloads: 0; property int installs: 0;"
        "function download() { downloads++; } function install() { installs++; }"
        "function cancel() {} function openRelease() {}");
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
    QMetaObject::invokeMethod(dialog, "close"); delete dialog;
    if (!runtimeErrors.isEmpty()) qFatal("%s", qPrintable(runtimeErrors.join("\n")));
    qInfo() << "PASS: update dialog download, busy gate and explicit installation action";
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
    return 0;
}

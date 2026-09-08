#include <QGuiApplication>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQuickItem>
#include <QTest>
#include <QImage>
#include <QDir>
#include <QDebug>
#include <QQuickStyle>
#include <QFontDatabase>
#include <QQuickItemGrabResult>
#include <QTranslator>

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Basic");
    QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");
    QTranslator translator;
    if (translator.load("../client/amneziavpn_ru_RU.qm")) app.installTranslator(&translator);
    qmlRegisterModule("PageEnum", 1, 0);
    qmlRegisterModule("ConnectionState", 1, 0);
    qmlRegisterModule("ContainerProps", 1, 0);
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
        "signal preparingConfig(); function connectButtonClicked() { clicks++; isConnected = !isConnected }");
    mock("ServersModel", "property int defaultIndex: 0; property bool hasServersFromGatewayApi: false;"
        "function setProcessedServerIndex(i) {}");
    mock("SettingsController",
        "property int safeAreaTopMargin: 0; property bool isAdvancedMode: true;"
        "property bool isDevModeEnabled: false; property bool startMinimized: false;"
        "property bool isLoggingEnabled: false; function isOnTv() { return false }"
        "function isAutoStartEnabled() { return false } function isAutoConnectEnabled() { return false }"
        "function isAutoDisableLoopbackProxyEnabled() { return false }");
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
    mock("PageController", "signal closeTopDrawer(); function showNotificationMessage(message) {}");
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
    auto *settings = create("Pages2/PageSettings.qml");
    settings->setSize(QSizeF(750, 680));
    QTest::qWait(150);
    auto *panel = settings->findChild<QQuickItem *>("generalSettingsPanel");
    if (!panel || panel->width() < 300 || panel->x() < 168)
        qFatal("Settings panel collapsed or overlapped");
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
    if (!runtimeErrors.isEmpty()) qFatal("%s", qPrintable(runtimeErrors.join("\n")));
    return 0;
}

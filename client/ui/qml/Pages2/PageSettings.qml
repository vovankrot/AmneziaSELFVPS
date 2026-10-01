import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import PageEnum 1.0
import ContainerProps 1.0
import Style 1.0

import "./"
import "../Controls2"
import "../Controls2/TextTypes"
import "../Config"
import "../Components"

PageType {
    id: root

    property int settingsSection: 0
    readonly property var sectionTitles: [qsTr("General"), qsTr("VPN & Connection"), qsTr("Servers"), qsTr("Maintenance")]

    function resetApplicationSettings() {
        var headerText = qsTr("Reset settings and remove all data from the application?")
        var descriptionText = qsTr("All settings will be reset to default. All installed AmneziaVPN services will still remain on the server.")
        var yesButtonFunction = function() {
            if (ServersModel.isDefaultServerCurrentlyProcessed() && ConnectionController.isConnected) {
                PageController.showNotificationMessage(qsTr("Cannot reset settings during active connection"))
                return
            }
            SettingsController.clearSettings()
            PageController.goToPageHome()
        }
        showQuestionDrawer(headerText, descriptionText, qsTr("Continue"), qsTr("Cancel"), yesButtonFunction, function() {})
    }

    onSettingsSectionChanged: generalSettingsFlickable.contentY = 0

    SelectLanguageDrawer {
        id: selectLanguageDrawer
        anchors.fill: parent
        z: 10
    }

    component SettingsNavButton: Rectangle {
        id: settingsNavButton
        property string text
        property string iconSource
        property bool selected: false
        property var clickedFunc: function() {}

        Layout.fillWidth: true
        Layout.preferredHeight: 44
        radius: 8
        color: selected ? AmneziaStyle.color.softGoldenApricot
                        : (settingsNavMouse.containsMouse ? AmneziaStyle.color.translucentWhite : "transparent")
        border.width: 0

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 11
            anchors.rightMargin: 11
            spacing: 9

            TintedIconType {
                Layout.preferredWidth: 15
                Layout.preferredHeight: 15
                source: settingsNavButton.iconSource
                tintColor: settingsNavButton.selected
                           ? AmneziaStyle.color.softViolet
                           : AmneziaStyle.color.mutedGray
                iconWidth: 15
                iconHeight: 15
            }
            Text {
                Layout.fillWidth: true
                text: settingsNavButton.text
                color: settingsNavButton.selected
                       ? AmneziaStyle.color.paleGray
                       : AmneziaStyle.color.mutedGray
                font.family: "Inter"
                font.pixelSize: 13
                font.weight: settingsNavButton.selected ? 600 : 500
                elide: Text.ElideRight
            }
        }

        MouseArea {
            id: settingsNavMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: settingsNavButton.clickedFunc()
        }
    }

    Connections {
        target: ApiNewsController
        function onFetchNewsFinished() {
            PageController.showBusyIndicator(false)
        }
        
        function onErrorOccurred(errorCode, showError) {
            if (showError) {
                PageController.showErrorMessage(errorCode)
                PageController.closePage()
                PageController.showBusyIndicator(false)
            }
        }
    }

    RowLayout {
        id: desktopSettingsView
        anchors.fill: parent
        anchors.topMargin: 20 + SettingsController.safeAreaTopMargin
        anchors.leftMargin: 28
        anchors.rightMargin: 28
        anchors.bottomMargin: 20
        spacing: 0
        visible: GC.isDesktop()

        Rectangle {
            Layout.minimumWidth: 232
            Layout.preferredWidth: 232
            Layout.maximumWidth: 232
            Layout.fillHeight: true
            color: "transparent"

            ColumnLayout {
                anchors.fill: parent
                anchors.rightMargin: 24
                spacing: 3

                SettingsNavButton { objectName: "settingsGeneralNavigation"; text: qsTr("General"); iconSource: "qrc:/images/controls/app.svg"; selected: root.settingsSection === 0; clickedFunc: function() { root.settingsSection = 0 } }
                SettingsNavButton { objectName: "settingsNetworkNavigation"; text: qsTr("VPN & Connection"); iconSource: "qrc:/images/controls/radio.svg"; selected: root.settingsSection === 1; clickedFunc: function() { root.settingsSection = 1 } }
                SettingsNavButton { objectName: "desktopServersSettingsNavigation"; text: qsTr("Servers"); iconSource: "qrc:/images/controls/server.svg"; clickedFunc: servers.clickedHandler }
                SettingsNavButton { objectName: "settingsMaintenanceNavigation"; text: qsTr("Maintenance"); iconSource: "qrc:/images/controls/bug.svg"; selected: root.settingsSection === 3; clickedFunc: function() { root.settingsSection = 3 } }

                Item { Layout.fillHeight: true }
                Text {
                    Layout.leftMargin: 12
                    text: "v" + SettingsController.getAppVersion()
                    color: AmneziaStyle.color.charcoalGray
                    font.family: "Inter"
                    font.pixelSize: 10
                }
            }

            Rectangle {
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                width: 1
                color: AmneziaStyle.color.slateGray
            }
        }

        Flickable {
            id: generalSettingsFlickable
            objectName: "generalSettingsPanel"
            Layout.minimumWidth: 300
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentHeight: (root.settingsSection === 0 ? generalSettingsColumn.implicitHeight : sectionColumn.implicitHeight) + 16
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            ColumnLayout {
                id: generalSettingsColumn
                visible: root.settingsSection === 0
                width: generalSettingsFlickable.width - 56
                x: 44
                spacing: 0

                Text {
                    Layout.fillWidth: true
                    Layout.bottomMargin: 22
                    text: qsTr("General")
                    color: AmneziaStyle.color.paleGray
                    font.family: "Inter"
                    font.pixelSize: 24
                    font.weight: 700
                }

                Text { Layout.fillWidth: true; Layout.bottomMargin: 8; text: qsTr("Startup"); color: AmneziaStyle.color.mutedGray; font.family: "Inter"; font.pixelSize: 13; font.weight: 600 }
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: AmneziaStyle.color.slateGray }
                SwitcherType {
                    id: desktopAutoStart
                    Layout.fillWidth: true
                    Layout.preferredHeight: 54
                    text: qsTr("Auto start")
                    descriptionText: qsTr("Launch AmneziaVPN when Windows starts")
                    checked: SettingsController.isAutoStartEnabled()
                    onToggled: function() { if (checked !== SettingsController.isAutoStartEnabled()) SettingsController.toggleAutoStart(checked) }
                }
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: AmneziaStyle.color.slateGray }
                SwitcherType {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 54
                    text: qsTr("Auto connect")
                    descriptionText: qsTr("Connect to VPN after the application starts")
                    checked: SettingsController.isAutoConnectEnabled()
                    onToggled: function() { if (checked !== SettingsController.isAutoConnectEnabled()) SettingsController.toggleAutoConnect(checked) }
                }

                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: AmneziaStyle.color.slateGray }
                SwitcherType {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 54
                    text: qsTr("Start minimized")
                    descriptionText: qsTr("Keep the app in the notification area on startup")
                    enabled: desktopAutoStart.checked
                    checked: SettingsController.startMinimized
                    onToggled: function() { if (checked !== SettingsController.startMinimized) SettingsController.toggleStartMinimized(checked) }
                }
                Text { Layout.fillWidth: true; Layout.topMargin: 22; Layout.bottomMargin: 8; text: qsTr("Language and interface"); color: AmneziaStyle.color.mutedGray; font.family: "Inter"; font.pixelSize: 13; font.weight: 600 }
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: AmneziaStyle.color.slateGray }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 48
                    color: languageMouse.containsMouse ? AmneziaStyle.color.translucentWhite : "transparent"
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        Text { Layout.fillWidth: true; text: qsTr("Language"); color: AmneziaStyle.color.paleGray; font.family: "Inter"; font.pixelSize: 13 }
                        Text { text: LanguageModel.currentLanguageName; color: AmneziaStyle.color.mutedGray; font.family: "Inter"; font.pixelSize: 12 }
                        TintedIconType { Layout.preferredWidth: 14; Layout.preferredHeight: 14; source: "qrc:/images/controls/chevron-right.svg"; tintColor: AmneziaStyle.color.charcoalGray; iconWidth: 14; iconHeight: 14 }
                    }
                    MouseArea { id: languageMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: selectLanguageDrawer.openTriggered() }
                }
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: AmneziaStyle.color.slateGray }
                SwitcherType {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 54
                    text: qsTr("Advanced mode")
                    descriptionText: qsTr("Show expert network settings")
                    checked: SettingsController.isAdvancedMode
                    onToggled: function() { if (checked !== SettingsController.isAdvancedMode) SettingsController.isAdvancedMode = checked }
                }
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: AmneziaStyle.color.slateGray }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 14
                    spacing: 8
                    BasicButtonType {
                        objectName: "resetSettingsButton"
                        text: qsTr("Reset settings")
                        defaultColor: AmneziaStyle.color.transparent
                        hoveredColor: Qt.rgba(239/255, 68/255, 68/255, 0.10)
                        pressedColor: Qt.rgba(239/255, 68/255, 68/255, 0.18)
                        textColor: AmneziaStyle.color.vibrantRed
                        borderWidth: 0
                        clickedFunc: root.resetApplicationSettings
                    }
                    BasicButtonType {
                        text: qsTr("Close application")
                        defaultColor: AmneziaStyle.color.transparent
                        hoveredColor: AmneziaStyle.color.translucentWhite
                        textColor: AmneziaStyle.color.mutedGray
                        borderWidth: 0
                        clickedFunc: function() { PageController.closeApplication() }
                    }
                    Item { Layout.fillWidth: true }
                }
            }
            ColumnLayout {
                id: sectionColumn
                objectName: "settingsSectionPanel"
                visible: root.settingsSection !== 0
                width: generalSettingsColumn.width
                x: generalSettingsColumn.x
                spacing: 12

                Text {
                    Layout.fillWidth: true
                    text: root.sectionTitles[root.settingsSection]
                    color: AmneziaStyle.color.paleGray
                    font.family: "Inter"
                    font.pixelSize: 24
                    font.weight: 700
                }
                ParagraphTextType {
                    Layout.fillWidth: true
                    Layout.bottomMargin: 10
                    color: AmneziaStyle.color.mutedGray
                    text: root.settingsSection === 1 ? qsTr("Choose where traffic goes and how the connection is protected. These settings apply to the selected VPN connection.")
                        : root.settingsSection === 2 ? qsTr("Manage your VPS, installed protocols and server services. Application preferences are in General.")
                        : qsTr("Updates, backups and logs in one place.")
                }
                SwitcherType {
                    Layout.fillWidth: true
                    visible: root.settingsSection === 1
                    text: qsTr("Use AmneziaDNS")
                    descriptionText: qsTr("If AmneziaDNS is installed on the server")
                    checked: SettingsController.isAmneziaDnsEnabled()
                    onToggled: function() { if (checked !== SettingsController.isAmneziaDnsEnabled()) SettingsController.toggleAmneziaDns(checked) }
                }
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: AmneziaStyle.color.slateGray; visible: root.settingsSection === 1 && Qt.platform.os === "windows" }
                SwitcherType {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 54
                    visible: root.settingsSection === 1 && Qt.platform.os === "windows"
                    text: qsTr("Disable local proxy on connect")
                    descriptionText: qsTr("Avoid routing conflicts with local proxy applications")
                    checked: SettingsController.isAutoDisableLoopbackProxyEnabled()
                    onToggled: function() { if (checked !== SettingsController.isAutoDisableLoopbackProxyEnabled()) SettingsController.toggleAutoDisableLoopbackProxy(checked) }
                }

                Repeater {
                    model: root.settingsSection === 1 ? [
                        { title: qsTr("App-based split tunneling"), description: qsTr("Choose which applications use VPN or connect directly"), handler: function() { PageController.goToPage(PageEnum.PageSettingsAppSplitTunneling) }, shown: Qt.platform.os === "windows" || Qt.platform.os === "android" },
                        { title: qsTr("Site-based split tunneling"), description: qsTr("Routing rules for websites and IP addresses"), handler: splitTunneling.clickedHandler, shown: true },
                        { title: qsTr("DNS servers"), description: qsTr("Addresses used when AmneziaDNS is unavailable"), handler: dns.clickedHandler, shown: true },
                        { title: qsTr("Kill Switch"), description: qsTr("Blocks network connections without VPN"), handler: killSwitch.clickedHandler, shown: true }
                    ] : root.settingsSection === 2 ? [
                        { title: qsTr("Servers"), description: qsTr("Select a VPS to manage protocols, services and access"), handler: servers.clickedHandler, shown: true }
                    ] : root.settingsSection === 3 ? [
                        { title: qsTr("Backup"), description: qsTr("Save or restore application settings and connections"), handler: backup.clickedHandler, shown: true },
                        { title: qsTr("Logging"), description: qsTr("View and export application logs"), handler: logging.clickedHandler, shown: true },
                        { title: qsTr("About AmneziaVPN"), description: qsTr("Application version and update checks"), handler: function() { PageController.goToPage(PageEnum.PageSettingsAbout) }, shown: true }
                    ] : []
                    delegate: Rectangle {
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: entryLabel.implicitHeight + 8
                        visible: modelData.shown
                        radius: 8
                        color: AmneziaStyle.color.onyxBlack
                        border.color: AmneziaStyle.color.slateGray
                        LabelWithButtonType {
                            id: entryLabel
                            width: parent.width
                            y: 4
                            text: modelData.title
                            descriptionText: modelData.description
                            rightImageSource: "qrc:/images/controls/chevron-right.svg"
                            clickedFunction: modelData.handler
                        }
                    }
                }
            }
        }
    }

    ListViewType {
        id: listView

        anchors.fill: parent
        visible: !GC.isDesktop()

        header: ColumnLayout {
            width: listView.width

            BaseHeaderType {
                id: header
                Layout.fillWidth: true
                Layout.topMargin: 24 + SettingsController.safeAreaTopMargin
                Layout.bottomMargin: 16
                Layout.rightMargin: 16
                Layout.leftMargin: 16

                headerText: qsTr("Settings")
            }
        }

        model: settingsEntries

        delegate: ColumnLayout {
            id: delegateItem
            width: listView.width

            required property QtObject modelData

            spacing: 0

            // Section header
            CaptionTextType {
                Layout.fillWidth: true
                Layout.topMargin: 20
                Layout.bottomMargin: 4
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                visible: delegateItem.modelData.sectionHeader !== undefined
                         && delegateItem.modelData.sectionHeader.length > 0

                text: delegateItem.modelData.sectionHeader || ""
                color: AmneziaStyle.color.paleGray
                font.pixelSize: 13
                font.weight: Font.DemiBold
                font.capitalization: Font.AllUppercase
            }

            // Toggle item (Advanced mode)
            SwitcherType {
                Layout.fillWidth: true
                visible: delegateItem.modelData.isToggle !== undefined
                         && delegateItem.modelData.isToggle
                         && delegateItem.modelData.isVisible

                text: delegateItem.modelData.title
                checked: SettingsController.isAdvancedMode
                onToggled: function() {
                    if (checked !== SettingsController.isAdvancedMode) {
                        SettingsController.isAdvancedMode = checked
                    }
                }
            }

            // Normal navigation item
            LabelWithButtonType {
                Layout.fillWidth: true

                visible: delegateItem.modelData.isVisible
                         && delegateItem.modelData.title.length > 0
                         && (delegateItem.modelData.isToggle === undefined || !delegateItem.modelData.isToggle)

                text: delegateItem.modelData.title
                rightImageSource: "qrc:/images/controls/chevron-right.svg"
                leftImageSource: delegateItem.modelData.leftImagePath

                clickedFunction: delegateItem.modelData.clickedHandler
            }

            DividerType {
                visible: delegateItem.modelData.isVisible && delegateItem.modelData.title.length > 0
            }
        }

        footer: ColumnLayout {
            width: listView.width

            LabelWithButtonType {
                id: close

                visible: GC.isDesktop()
                Layout.fillWidth: true

                text: qsTr("Close application")
                leftImageSource: "qrc:/images/controls/x-circle.svg"
                isLeftImageHoverEnabled: false

                clickedFunction: function() {
                    PageController.closeApplication()
                }
            }

            DividerType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                visible: GC.isDesktop()
            }
        }
    }

    property list<QtObject> settingsEntries: [
        // — VPN & Connection —
        sectionVpn,
        servers,
        connection,
        splitTunneling,
        killSwitch,
        dns,
        // — Application —
        sectionApp,
        application,
        news,
        logging,
        // — Data —
        sectionData,
        backup,
        // — Info —
        sectionInfo,
        about,
        devConsole,
        // — Mode toggle —
        sectionMode,
        advancedToggle
    ]

    // ── Section headers ──

    QtObject {
        id: sectionVpn
        property string title: ""
        property string sectionHeader: qsTr("VPN & Connection")
        readonly property string leftImagePath: ""
        property bool isVisible: true
        readonly property var clickedHandler: function() {}
    }

    QtObject {
        id: sectionApp
        property string title: ""
        property string sectionHeader: qsTr("Application")
        readonly property string leftImagePath: ""
        property bool isVisible: true
        readonly property var clickedHandler: function() {}
    }

    QtObject {
        id: sectionData
        property string title: ""
        property string sectionHeader: qsTr("Data")
        readonly property string leftImagePath: ""
        property bool isVisible: true
        readonly property var clickedHandler: function() {}
    }

    QtObject {
        id: sectionInfo
        property string title: ""
        property string sectionHeader: qsTr("Info")
        readonly property string leftImagePath: ""
        property bool isVisible: true
        readonly property var clickedHandler: function() {}
    }

    QtObject {
        id: sectionMode
        property string title: ""
        property string sectionHeader: ""
        readonly property string leftImagePath: ""
        property bool isVisible: false
        readonly property var clickedHandler: function() {}
    }

    // ── Items ──

    QtObject {
        id: servers

        property string title: qsTr("Servers")
        property string sectionHeader: ""
        readonly property string leftImagePath: "qrc:/images/controls/server.svg"
        property bool isVisible: true
        readonly property var clickedHandler: function() {
            PageController.goToPage(PageEnum.PageSettingsServersList)
        }
    }

    QtObject {
        id: connection

        property string title: qsTr("Connection")
        property string sectionHeader: ""
        readonly property string leftImagePath: "qrc:/images/controls/radio.svg"
        property bool isVisible: true
        readonly property var clickedHandler: function() {
            PageController.goToPage(PageEnum.PageSettingsConnection)
        }
    }

    QtObject {
        id: splitTunneling

        property string title: qsTr("Split tunneling")
        property string sectionHeader: ""
        readonly property string leftImagePath: "qrc:/images/controls/split-tunneling.svg"
        property bool isVisible: SettingsController.isAdvancedMode
        readonly property var clickedHandler: function() {
            if (!ContainerProps.supportsSiteSplitTunneling(ServersModel.getDefaultServerData("defaultContainer"))) {
                PageController.showNotificationMessage(qsTr("Site-based split tunneling is available only for XRay and Shadowsocks over XRay"))
                return
            }
            PageController.goToPage(PageEnum.PageSettingsSplitTunneling)
        }
    }

    QtObject {
        id: killSwitch

        property string title: qsTr("Kill Switch")
        property string sectionHeader: ""
        readonly property string leftImagePath: "qrc:/images/controls/settings-2.svg"
        property bool isVisible: SettingsController.isAdvancedMode
        readonly property var clickedHandler: function() {
            PageController.goToPage(PageEnum.PageSettingsKillSwitch)
        }
    }

    QtObject {
        id: dns

        property string title: qsTr("DNS")
        property string sectionHeader: ""
        readonly property string leftImagePath: "qrc:/images/controls/globe-2.svg"
        property bool isVisible: SettingsController.isAdvancedMode
        readonly property var clickedHandler: function() {
            PageController.goToPage(PageEnum.PageSettingsDns)
        }
    }

    QtObject {
        id: application

        property string title: qsTr("General")
        property string sectionHeader: ""
        readonly property string leftImagePath: "qrc:/images/controls/app.svg"
        property bool isVisible: true
        readonly property var clickedHandler: function() {
            PageController.goToPage(PageEnum.PageSettingsApplication)
        }
    }

    QtObject {
        id: news

        property string title: qsTr("Notifications")
        property string sectionHeader: ""
        readonly property string leftImagePath: NewsModel.hasUnread && SettingsController.isNewsNotificationsEnabled() ? "qrc:/images/controls/news-unread.svg" : "qrc:/images/controls/news.svg"
        property bool isVisible: ServersModel.hasServersFromGatewayApi
        readonly property var clickedHandler: function() {
            if (!ServersModel.hasServersFromGatewayApi) {
                return;
            }
            PageController.showBusyIndicator(true)
            ApiNewsController.fetchNews(true)
            PageController.goToPage(PageEnum.PageSettingsNewsNotifications)
        }
    }

    QtObject {
        id: logging

        property string title: qsTr("Logging")
        property string sectionHeader: ""
        readonly property string leftImagePath: "qrc:/images/controls/bug.svg"
        property bool isVisible: SettingsController.isAdvancedMode
        readonly property var clickedHandler: function() {
            PageController.goToPage(PageEnum.PageSettingsLogging)
        }
    }

    QtObject {
        id: backup

        property string title: qsTr("Backup")
        property string sectionHeader: ""
        readonly property string leftImagePath: "qrc:/images/controls/save.svg"
        property bool isVisible: true
        readonly property var clickedHandler: function() {
            PageController.goToPage(PageEnum.PageSettingsBackup)
        }
    }

    QtObject {
        id: about

        property string title: qsTr("About AmneziaVPN")
        property string sectionHeader: ""
        readonly property string leftImagePath: "qrc:/images/controls/amnezia.svg"
        property bool isVisible: true
        readonly property var clickedHandler: function() {
            PageController.goToPage(PageEnum.PageSettingsAbout)
        }
    }

    QtObject {
        id: devConsole

        property string title: qsTr("Dev console")
        property string sectionHeader: ""
        readonly property string leftImagePath: "qrc:/images/controls/bug.svg"
        property bool isVisible: SettingsController.isDevModeEnabled
        readonly property var clickedHandler: function() {
            PageController.goToPage(PageEnum.PageDevMenu)
        }
    }

    QtObject {
        id: advancedToggle

        property string title: qsTr("Advanced mode")
        property string sectionHeader: ""
        readonly property string leftImagePath: "qrc:/images/controls/settings.svg"
        property bool isVisible: true
        property bool isToggle: true
        readonly property var clickedHandler: function() {}
    }
}

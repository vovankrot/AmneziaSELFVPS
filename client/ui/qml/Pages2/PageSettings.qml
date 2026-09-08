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
        Layout.preferredHeight: 38
        radius: 8
        color: selected ? AmneziaStyle.color.deepBrown
                        : (settingsNavMouse.containsMouse ? AmneziaStyle.color.translucentWhite : "transparent")
        border.width: selected ? 1 : 0
        border.color: AmneziaStyle.color.slateGray

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
                           ? AmneziaStyle.color.goldenApricot
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
                font.pixelSize: 12
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

    ColumnLayout {
        id: desktopSettingsView
        anchors.fill: parent
        anchors.topMargin: 24 + SettingsController.safeAreaTopMargin
        anchors.leftMargin: 28
        anchors.rightMargin: 28
        anchors.bottomMargin: 24
        spacing: 16
        visible: GC.isDesktop()

        Text {
            Layout.fillWidth: true
            text: qsTr("Settings")
            color: AmneziaStyle.color.paleGray
            font.family: "Inter"
            font.pixelSize: 22
            font.weight: 600
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 16

            ColumnLayout {
                Layout.minimumWidth: 168
                Layout.preferredWidth: 168
                Layout.maximumWidth: 168
                Layout.fillHeight: true
                spacing: 3

                SettingsNavButton {
                    text: qsTr("General")
                    iconSource: "qrc:/images/controls/app.svg"
                    selected: true
                }
                SettingsNavButton {
                    text: qsTr("Connection")
                    iconSource: "qrc:/images/controls/radio.svg"
                    clickedFunc: function() { PageController.goToPage(PageEnum.PageSettingsConnection) }
                }
                SettingsNavButton {
                    text: qsTr("Servers")
                    iconSource: "qrc:/images/controls/server.svg"
                    clickedFunc: function() { PageController.goToPage(PageEnum.PageSettingsServersList) }
                }
                SettingsNavButton {
                    text: qsTr("Split tunneling")
                    iconSource: "qrc:/images/controls/split-tunneling.svg"
                    visible: SettingsController.isAdvancedMode
                    clickedFunc: splitTunneling.clickedHandler
                }
                SettingsNavButton {
                    text: qsTr("Kill Switch")
                    iconSource: "qrc:/images/controls/settings-2.svg"
                    visible: SettingsController.isAdvancedMode
                    clickedFunc: function() { PageController.goToPage(PageEnum.PageSettingsKillSwitch) }
                }
                SettingsNavButton {
                    text: qsTr("DNS")
                    iconSource: "qrc:/images/controls/globe-2.svg"
                    visible: SettingsController.isAdvancedMode
                    clickedFunc: function() { PageController.goToPage(PageEnum.PageSettingsDns) }
                }

                Item { Layout.fillHeight: true }

                SettingsNavButton {
                    text: qsTr("Application")
                    iconSource: "qrc:/images/controls/app.svg"
                    clickedFunc: application.clickedHandler
                }
                SettingsNavButton {
                    text: qsTr("Logging")
                    iconSource: "qrc:/images/controls/bug.svg"
                    clickedFunc: logging.clickedHandler
                }
                SettingsNavButton {
                    text: qsTr("Notifications")
                    iconSource: "qrc:/images/controls/news.svg"
                    visible: news.isVisible
                    clickedFunc: news.clickedHandler
                }
                SettingsNavButton {
                    text: qsTr("Dev console")
                    iconSource: "qrc:/images/controls/bug.svg"
                    visible: devConsole.isVisible
                    clickedFunc: devConsole.clickedHandler
                }

                SettingsNavButton {
                    text: qsTr("Backup")
                    iconSource: "qrc:/images/controls/save.svg"
                    clickedFunc: function() { PageController.goToPage(PageEnum.PageSettingsBackup) }
                }
                SettingsNavButton {
                    text: qsTr("About AmneziaVPN")
                    iconSource: "qrc:/images/controls/amnezia.svg"
                    clickedFunc: function() { PageController.goToPage(PageEnum.PageSettingsAbout) }
                }
            }

            Rectangle {
                objectName: "generalSettingsPanel"
                Layout.minimumWidth: 300
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 10
                color: AmneziaStyle.color.deepBrown
                border.width: 1
                border.color: AmneziaStyle.color.slateGray

                Flickable {
                    id: generalSettingsFlickable
                    anchors.fill: parent
                    anchors.margins: 18
                    contentHeight: generalSettingsColumn.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    ColumnLayout {
                        id: generalSettingsColumn
                        width: generalSettingsFlickable.width
                        spacing: 0

                        Text {
                            Layout.fillWidth: true
                            Layout.bottomMargin: 6
                            text: qsTr("General")
                            color: AmneziaStyle.color.paleGray
                            font.family: "Inter"
                            font.pixelSize: 17
                            font.weight: 600
                        }
                        Text {
                            Layout.fillWidth: true
                            Layout.bottomMargin: 14
                            text: qsTr("Application behavior and startup")
                            color: AmneziaStyle.color.mutedGray
                            font.family: "Inter"
                            font.pixelSize: 11
                        }

                        SwitcherType {
                            id: desktopAutoStart
                            Layout.fillWidth: true
                            Layout.preferredHeight: 46
                            text: qsTr("Auto start")
                            descriptionText: qsTr("Launch AmneziaVPN when Windows starts")
                            checked: SettingsController.isAutoStartEnabled()
                            onToggled: function() {
                                if (checked !== SettingsController.isAutoStartEnabled()) SettingsController.toggleAutoStart(checked)
                            }
                        }
                        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: AmneziaStyle.color.slateGray }
                        SwitcherType {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 46
                            text: qsTr("Auto connect")
                            descriptionText: qsTr("Connect to VPN after the application starts")
                            checked: SettingsController.isAutoConnectEnabled()
                            onToggled: function() {
                                if (checked !== SettingsController.isAutoConnectEnabled()) SettingsController.toggleAutoConnect(checked)
                            }
                        }
                        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: AmneziaStyle.color.slateGray }
                        SwitcherType {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 46
                            text: qsTr("Start minimized")
                            descriptionText: qsTr("Keep the app in the notification area on startup")
                            enabled: desktopAutoStart.checked
                            checked: SettingsController.startMinimized
                            onToggled: function() {
                                if (checked !== SettingsController.startMinimized) SettingsController.toggleStartMinimized(checked)
                            }
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            color: AmneziaStyle.color.slateGray
                            visible: Qt.platform.os === "windows"
                        }
                        SwitcherType {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 46
                            visible: Qt.platform.os === "windows"
                            text: qsTr("Disable local proxy on connect")
                            descriptionText: qsTr("Avoid routing conflicts with local proxy applications")
                            checked: SettingsController.isAutoDisableLoopbackProxyEnabled()
                            onToggled: function() {
                                if (checked !== SettingsController.isAutoDisableLoopbackProxyEnabled()) SettingsController.toggleAutoDisableLoopbackProxy(checked)
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            Layout.topMargin: 18
                            Layout.bottomMargin: 7
                            text: qsTr("INTERFACE")
                            color: AmneziaStyle.color.charcoalGray
                            font.family: "Inter"
                            font.pixelSize: 10
                            font.weight: 600
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 42
                            radius: 8
                            color: languageMouse.containsMouse
                                   ? AmneziaStyle.color.onyxBlack
                                   : AmneziaStyle.color.translucentOnyxBlack
                            border.width: 1
                            border.color: AmneziaStyle.color.slateGray

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

                        SwitcherType {
                            Layout.fillWidth: true
                            Layout.topMargin: 12
                            Layout.preferredHeight: 42
                            text: qsTr("Advanced mode")
                            descriptionText: qsTr("Show expert network settings")
                            checked: SettingsController.isAdvancedMode
                            onToggled: function() {
                                if (checked !== SettingsController.isAdvancedMode) SettingsController.isAdvancedMode = checked
                            }
                        }

                        BasicButtonType {
                            Layout.alignment: Qt.AlignLeft
                            Layout.topMargin: 16
                            implicitWidth: Math.max(150, buttonTextLabel.implicitWidth + 52)
                            text: qsTr("Close application")
                            leftImageSource: "qrc:/images/controls/x-circle.svg"
                            defaultColor: AmneziaStyle.color.transparent
                            hoveredColor: Qt.rgba(239/255, 68/255, 68/255, 0.12)
                            textColor: AmneziaStyle.color.vibrantRed
                            borderWidth: 1
                            borderColor: Qt.rgba(239/255, 68/255, 68/255, 0.45)
                            clickedFunc: function() { PageController.closeApplication() }
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

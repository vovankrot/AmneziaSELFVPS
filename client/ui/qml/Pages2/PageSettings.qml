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

PageType {
    id: root

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

    ListViewType {
        id: listView

        anchors.fill: parent

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

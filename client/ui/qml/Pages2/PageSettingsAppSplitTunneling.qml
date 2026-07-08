import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import QtCore

import SortFilterProxyModel 0.2

import PageEnum 1.0
import ProtocolEnum 1.0
import ContainerProps 1.0
import Style 1.0

import "./"
import "../Controls2"
import "../Controls2/TextTypes"
import "../Config"
import "../Components"

PageType {
    id: root

    pageBackgroundVisible: true

    readonly property bool pageEnabled: !ConnectionController.isConnectionInProgress
    readonly property bool siteSplitTunnelingOverridesAppList: SitesModel.isTunnelingEnabled
    property bool hasPendingChanges: false
    property string pendingDialogKind: ""
    property bool isApplicationQuitting: false
    property string initialStateSignature: ""

    Component.onCompleted: {
        root.initialStateSignature = AppSplitTunnelingModel.stateSignature()
        updatePendingChanges()
    }

    function updatePendingChanges() {
        if (!ConnectionController.isConnected) {
            root.hasPendingChanges = false
            return
        }

        root.hasPendingChanges = AppSplitTunnelingModel.stateSignature() !== root.initialStateSignature
    }

    function applyPendingChanges(notificationMessage) {
        updatePendingChanges()

        var shouldApply = !root.isApplicationQuitting
                && root.hasPendingChanges
                && ConnectionController.isConnected

        if (!shouldApply) {
            return false
        }

        root.initialStateSignature = AppSplitTunnelingModel.stateSignature()
        root.hasPendingChanges = false

        Qt.callLater(function() {
            if (!ConnectionController.isConnected) {
                return
            }

            ConnectionController.reconnectToVpn()
            if (notificationMessage !== "") {
                PageController.showNotificationMessage(notificationMessage)
            }
        })

        return true
    }

    function applyPendingChangesOnLeave() {
        applyPendingChanges(qsTr("Split tunneling settings were applied automatically"))
    }

    Component.onDestruction: {
        applyPendingChangesOnLeave()
    }

    Connections {
        target: Qt.application

        function onAboutToQuit() {
            root.isApplicationQuitting = true
        }
    }

    Connections {
        target: AppSplitTunnelingController

        function onFinished(message) {
            root.updatePendingChanges()

            if (root.applyPendingChanges(message)) {
                return
            }

            PageController.showNotificationMessage(message)
        }

        function onErrorOccurred(errorMessage) {
            PageController.showErrorMessage(errorMessage)
        }
    }

    Connections {
        target: SystemController

        function onFileDialogFinished(fileName) {
            if (root.pendingDialogKind === "") {
                return
            }

            var pendingDialogKind = root.pendingDialogKind
            root.pendingDialogKind = ""

            if (fileName === "") {
                return
            }

            if (pendingDialogKind === "app") {
                AppSplitTunnelingController.addApp(fileName)
            } else if (pendingDialogKind === "folder") {
                AppSplitTunnelingController.addAppsFromFolder(fileName)
            }
        }
    }

    Connections {
        target: ConnectionController

        function onConnectionStateChanged() {
            if (!ConnectionController.isConnected) {
                root.hasPendingChanges = false
            }
        }
    }

    ColumnLayout {
        id: header

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right

        anchors.topMargin: 20 + SettingsController.safeAreaTopMargin

        BackButtonType {
            id: backButton
        }

        HeaderTypeWithSwitcher {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16

            headerText: qsTr("App split tunneling")

            enabled: root.pageEnabled
            showSwitcher: true
            switcher {
                checked: AppSplitTunnelingModel.isTunnelingEnabled
                enabled: root.pageEnabled
            }
            switcherFunction: function(checked) {
                AppSplitTunnelingModel.toggleSplitTunneling(checked)
                root.updatePendingChanges()

                root.applyPendingChanges(qsTr("Split tunneling settings were applied automatically"))
            }
        }

        Rectangle {
            id: tipsToggle
            Layout.fillWidth: true
            Layout.topMargin: 8
            Layout.leftMargin: 16
            Layout.rightMargin: 16

            property bool expanded: false

            color: AmneziaStyle.color.onyxBlack
            radius: 8
            implicitHeight: tipsToggleRow.implicitHeight + 16

            visible: root.pageEnabled

            RowLayout {
                id: tipsToggleRow
                anchors.fill: parent
                anchors.margins: 8
                spacing: 8

                TintedIconType {
                    Layout.alignment: Qt.AlignVCenter
                    source: "qrc:/images/controls/info.svg"
                    tintColor: AmneziaStyle.color.paleGray
                    iconWidth: 16
                    iconHeight: 16
                }

                CaptionTextType {
                    Layout.fillWidth: true
                    text: qsTr("Tips and details")
                    color: AmneziaStyle.color.paleGray
                }

                TintedIconType {
                    Layout.alignment: Qt.AlignVCenter
                    source: "qrc:/images/controls/chevron-down.svg"
                    tintColor: AmneziaStyle.color.paleGray
                    iconWidth: 16
                    iconHeight: 16
                    rotation: tipsToggle.expanded ? 180 : 0

                    Behavior on rotation {
                        NumberAnimation { duration: 200; easing.type: Easing.InOutQuad }
                    }
                }
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: tipsToggle.expanded = !tipsToggle.expanded
            }
        }

        WarningType {
            Layout.fillWidth: true
            Layout.topMargin: 8
            Layout.leftMargin: 16
            Layout.rightMargin: 16

            textString: qsTr("Apps in this list will bypass VPN — their traffic goes directly without encryption.")
            iconPath: "qrc:/images/controls/info.svg"

            visible: root.pageEnabled && tipsToggle.expanded
        }

        WarningType {
            Layout.fillWidth: true
            Layout.topMargin: 8
            Layout.leftMargin: 16
            Layout.rightMargin: 16

            textString: qsTr("Changes to the app list are applied immediately. Restart the changed application if it is already running.")
            iconPath: "qrc:/images/controls/refresh-cw.svg"

            visible: ConnectionController.isConnected && tipsToggle.expanded
        }

        WarningType {
            Layout.fillWidth: true
            Layout.topMargin: 8
            Layout.leftMargin: 16
            Layout.rightMargin: 16

            textString: qsTr("Tip: site and app split tunneling work best when set to opposite modes.\n\n" +
                             "• If site mode is \"Sites not in the list will bypass VPN\" (only listed sites through VPN), then the app list is for apps that should bypass VPN.\n" +
                             "• If site mode is \"Sites not in the list will use VPN\" (all traffic through VPN except listed sites), then the app list is also for apps that should bypass VPN.\n\n" +
                             "Recommended: use \"Sites not in the list will use VPN\" together with this app bypass list — then all traffic goes through VPN except sites and apps you explicitly exclude.")
            iconPath: "qrc:/images/controls/alert-circle.svg"

            visible: root.siteSplitTunnelingOverridesAppList && tipsToggle.expanded
        }
    }

    ListViewType {
        id: listView

        ScrollBar.vertical: ScrollBarType { policy: ScrollBar.AlwaysOn }

        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.bottomMargin: bottomActions.implicitHeight + 48 + SettingsController.safeAreaBottomMargin + (searchField.textField.activeFocus ? 0 : SettingsController.imeHeight)
        anchors.left: parent.left
        anchors.right: parent.right
        clip: true

        model: SortFilterProxyModel {
            id: proxyAppSplitTunnelingModel
            sourceModel: AppSplitTunnelingModel
            filters: RegExpFilter {
                roleName: "appName"
                pattern: ".*" + searchField.textField.text + ".*"
                caseSensitivity: Qt.CaseInsensitive
            }
            sorters: [
                RoleSorter { roleName: "appName"; sortOrder: Qt.AscendingOrder }
            ]
        }

        delegate: ColumnLayout {
            width: listView.width

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.topMargin: 8
                Layout.bottomMargin: 8

                spacing: 12

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    ParagraphTextType {
                        Layout.fillWidth: true
                        text: appName
                        color: AmneziaStyle.color.paleGray
                        maximumLineCount: 2
                        elide: Qt.ElideRight
                    }

                    CaptionTextType {
                        Layout.fillWidth: true
                        text: appPath
                        color: AmneziaStyle.color.mutedGray
                        maximumLineCount: 1
                        elide: Qt.ElideMiddle
                        visible: appPath !== ""
                    }
                }



                ImageButtonType {
                    id: deleteButton

                    implicitWidth: 40
                    implicitHeight: 40

                    image: "qrc:/images/controls/trash.svg"
                    imageColor: AmneziaStyle.color.paleGray
                    hoveredColor: AmneziaStyle.color.barelyTranslucentWhite
                    pressedColor: AmneziaStyle.color.barelyTranslucentWhite

                    onClicked: {
                        var headerText = qsTr("Remove ") + appName + "?"
                        var yesButtonText = qsTr("Continue")
                        var noButtonText = qsTr("Cancel")

                        var yesButtonFunction = function() {
                            AppSplitTunnelingController.removeApp(proxyAppSplitTunnelingModel.mapToSource(index))
                        }
                        var noButtonFunction = function() {
                        }

                        showQuestionDrawer(headerText, "", yesButtonText, noButtonText, yesButtonFunction, noButtonFunction)
                    }
                }
            }

            DividerType {}
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        
        height: bottomActions.implicitHeight + 48 + SettingsController.safeAreaBottomMargin
        
        color: AmneziaStyle.color.midnightBlack

        ColumnLayout {
            id: bottomActions

            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 24
            anchors.rightMargin: 16
            anchors.leftMargin: 16
            anchors.bottomMargin: 24 + SettingsController.safeAreaBottomMargin

            RowLayout {
                id: addAppButton

                enabled: root.pageEnabled

                Layout.fillWidth: true

                TextFieldWithHeaderType {
                    id: searchField

                    Layout.fillWidth: true

                    textField.placeholderText: qsTr("application name")
                    buttonImageSource: "qrc:/images/controls/plus.svg"

                    rightButtonClickedOnEnter: true

                    clickedFunc: function() {
                        searchField.focus = false

                        if (Qt.platform.os === "windows") {
                            root.pendingDialogKind = "app"
                            SystemController.openFileDialogAsync(qsTr("Open executable file"),
                                                                 qsTr("Executable files (*.exe)"))
                        } else if (Qt.platform.os === "android"){
                            PageController.showBusyIndicator(true)
                            installedAppDrawer.openTriggered()
                            PageController.showBusyIndicator(false)
                        }
                    }
                }

                BasicButtonType {
                    Layout.preferredHeight: 56

                    visible: Qt.platform.os === "windows"
                    enabled: root.pageEnabled

                    text: qsTr("Folder")
                    leftImageSource: "qrc:/images/controls/folder-open.svg"

                    clickedFunc: function() {
                        searchField.focus = false
                        root.pendingDialogKind = "folder"
                        SystemController.openDirectoryDialogAsync(qsTr("Open application folder"))
                    }
                }
            }
        }
    }

    InstalledAppsDrawer {
        id: installedAppDrawer

        anchors.fill: parent
    }
}

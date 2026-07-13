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

    // Last path component for a folder header, tolerant of a trailing slash and of
    // drive/volume roots like "C:/" (whose last component is empty) — falls back to the
    // full path so the header title and the confirm dialog never show an empty name.
    function folderLeafName(path) {
        if (path === "")
            return ""
        var trimmed = path
        while (trimmed.length > 1 && trimmed.charAt(trimmed.length - 1) === '/')
            trimmed = trimmed.substring(0, trimmed.length - 1)
        var leaf = trimmed.substring(trimmed.lastIndexOf('/') + 1)
        return leaf === "" ? path : leaf
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
                RoleSorter { roleName: "groupFolder"; sortOrder: Qt.AscendingOrder },
                RoleSorter { roleName: "appName"; sortOrder: Qt.AscendingOrder }
            ]
        }

        section.property: "groupFolder"
        section.criteria: ViewSection.FullString
        section.delegate: ColumnLayout {
            width: listView.width

            // The "groupFolder" role is the effective folder: the stored source folder for
            // folder-adds (or the exe's own directory for legacy/single adds), with
            // subfolders collapsed into their parent group. Every folder gets a removable
            // header; only entries with no path at all (empty section) skip it.
            Rectangle {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.topMargin: 12

                visible: section !== ""
                Layout.preferredHeight: sectionRow.implicitHeight + 20

                color: AmneziaStyle.color.onyxBlack
                radius: 8

                // Accent stripe so the header reads as a group, not another app row
                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 3
                    radius: 1.5
                    color: AmneziaStyle.color.goldenApricot
                }

                RowLayout {
                    id: sectionRow
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 6
                    spacing: 10

                    TintedIconType {
                        Layout.alignment: Qt.AlignVCenter
                        source: "qrc:/images/controls/folder-open.svg"
                        tintColor: AmneziaStyle.color.goldenApricot
                        iconWidth: 20
                        iconHeight: 20
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            ParagraphTextType {
                                Layout.fillWidth: false
                                Layout.maximumWidth: sectionRow.width - 140
                                text: root.folderLeafName(section)
                                font.bold: true
                                color: AmneziaStyle.color.paleGray
                                maximumLineCount: 1
                                elide: Qt.ElideMiddle
                            }

                            Rectangle {
                                Layout.alignment: Qt.AlignVCenter
                                implicitWidth: countText.implicitWidth + 12
                                implicitHeight: countText.implicitHeight + 4
                                radius: height / 2
                                color: AmneziaStyle.color.charcoalGray

                                CaptionTextType {
                                    id: countText
                                    anchors.centerIn: parent
                                    // proxy count referenced so the badge re-evaluates on add/remove
                                    text: (proxyAppSplitTunnelingModel.count,
                                           AppSplitTunnelingModel.groupCount(section))
                                    color: AmneziaStyle.color.paleGray
                                }
                            }

                            Item { Layout.fillWidth: true }
                        }

                        CaptionTextType {
                            Layout.fillWidth: true
                            text: section
                            color: AmneziaStyle.color.mutedGray
                            maximumLineCount: 1
                            elide: Qt.ElideMiddle
                        }
                    }

                    ImageButtonType {
                        implicitWidth: 40
                        implicitHeight: 40

                        image: "qrc:/images/controls/trash.svg"
                        imageColor: AmneziaStyle.color.paleGray
                        hoveredColor: AmneziaStyle.color.barelyTranslucentWhite
                        pressedColor: AmneziaStyle.color.barelyTranslucentWhite

                        onClicked: {
                            var folder = section
                            var folderName = root.folderLeafName(folder)
                            var headerText = qsTr("Remove folder \"%1\" and all its apps?").arg(folderName)
                            var yesButtonText = qsTr("Continue")
                            var noButtonText = qsTr("Cancel")

                            var yesButtonFunction = function() {
                                AppSplitTunnelingController.removeGroup(folder)
                            }
                            var noButtonFunction = function() {
                            }

                            showQuestionDrawer(headerText, "", yesButtonText, noButtonText, yesButtonFunction, noButtonFunction)
                        }
                    }
                }
            }
        }

        delegate: ColumnLayout {
            width: listView.width

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: groupFolder !== "" ? 36 : 16
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
                        // Inside a group the full path just repeats the header —
                        // show the path relative to the group folder instead.
                        text: {
                            if (appPath === "")
                                return ""
                            if (groupFolder !== ""
                                    && appPath.toLowerCase().indexOf(groupFolder.toLowerCase() + "/") === 0)
                                return appPath.substring(groupFolder.length + 1)
                            return appPath
                        }
                        color: AmneziaStyle.color.mutedGray
                        maximumLineCount: 1
                        elide: Qt.ElideMiddle
                        visible: text !== ""
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

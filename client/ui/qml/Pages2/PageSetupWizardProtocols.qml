import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import SortFilterProxyModel 0.2

import PageEnum 1.0
import ContainerEnum 1.0
import ProtocolEnum 1.0
import Style 1.0

import "./"
import "../Controls2"
import "../Config"

PageType {
    id: root

    property bool serverCheckDone: false
    property int configCheckServerIndex: -1
    property bool xrayNeedsCurrentXhttpConfig: false

    Component.onCompleted: {
        InstallController.checkServerContainers()
        var idx = ServersModel.defaultIndex
        root.configCheckServerIndex = idx
        if (idx >= 0) {
            InstallController.checkServerConfigUpdate(idx)
        }
    }

    Connections {
        target: InstallController
        function onServerContainersChecked() {
            root.serverCheckDone = true
        }

        function onServerConfigUpdateAvailable(serverIndex, currentSchema, requiredSchema) {
            if (serverIndex === root.configCheckServerIndex) {
                root.xrayNeedsCurrentXhttpConfig = true
            }
        }

        function onServerConfigUpToDate(serverIndex) {
            if (serverIndex === root.configCheckServerIndex) {
                root.xrayNeedsCurrentXhttpConfig = false
            }
        }
    }

    SortFilterProxyModel {
        id: proxyContainersModel
        sourceModel: ContainersModel
        filters: [
            ValueFilter {
                roleName: "serviceType"
                value: ProtocolEnum.Vpn
            },
            ValueFilter {
                roleName: "isSupported"
                value: true
            },
            ValueFilter {
                roleName: "isInstallationAllowed"
                value: true
            }
        ]
        sorters: RoleSorter {
            roleName: "installPageOrder"
            sortOrder: Qt.AscendingOrder
        }
    }

    BackButtonType {
        id: backButton

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right

        anchors.topMargin: 20 + SettingsController.safeAreaTopMargin

        onActiveFocusChanged: {
            if(backButton.enabled && backButton.activeFocus) {
                listView.positionViewAtBeginning()
            }
        }
    }

    ListViewType {
        id: listView
        anchors.top: backButton.bottom
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.left: parent.left

        header: ColumnLayout {
            width: listView.width

            BaseHeaderType {
                id: header

                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.bottomMargin: 16

                headerText: qsTr("VPN protocol")
                descriptionText: qsTr("Choose the one with the highest priority for you. Later, you can install other protocols and additional services, such as DNS proxy and SFTP.")
            }
        }

        model: proxyContainersModel

        spacing: 0
        snapMode: ListView.SnapToItem

        delegate: ColumnLayout {
            width: listView.width

            property bool installedOnServer: root.serverCheckDone && InstallController.isContainerInstalledOnServer(dockerContainer)
            property string baseDescriptionText: installedOnServer
                ? qsTr("Already installed on server. Will reuse existing config.")
                : description

            LabelWithButtonType {
                Layout.fillWidth: true

                text: name
                descriptionText: baseDescriptionText
                badgeText: installedOnServer ? qsTr("Installed") : ""
                badgeColor: AmneziaStyle.color.vibrantGreen
                rightImageSource: "qrc:/images/controls/chevron-right.svg"

                clickedFunction: function () {
                    ContainersModel.setProcessedContainerIndex(proxyContainersModel.mapToSource(index));
                    PageController.goToPage(PageEnum.PageSetupWizardProtocolSettings);
                }
            }

            DividerType {}
        }
    }
}

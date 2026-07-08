import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import SortFilterProxyModel 0.2

import PageEnum 1.0
import ContainerEnum 1.0
import ContainerProps 1.0
import ProtocolProps 1.0
import ProtocolEnum 1.0
import Style 1.0

import "./"
import "../Controls2"
import "../Config"

PageType {
    id: root

    property bool isEasySetup: true
    property int configCheckServerIndex: -1
    property bool xrayNeedsCurrentXhttpConfig: false

    Component.onCompleted: {
        var idx = ServersModel.defaultIndex
        root.configCheckServerIndex = idx
        if (idx >= 0) {
            InstallController.checkServerConfigUpdate(idx)
        }
    }

    Connections {
        target: InstallController

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

    // Filtered & sorted list of installable VPN protocols
    SortFilterProxyModel {
        id: proxyContainersModel
        sourceModel: ContainersModel
        filters: [
            ValueFilter {
                roleName: "serviceType"
                value: ProtocolEnum.Vpn
            },
            ValueFilter {
                roleName: "isEasySetupContainer"
                value: true
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

    function installContainer(containerIndex) {
        var proto = ContainerProps.defaultProtocol(containerIndex)
        var port = ProtocolProps.getPortForInstall(proto)
        var transport = ProtocolProps.defaultTransportProto(proto)

        ContainersModel.setProcessedContainerIndex(containerIndex)
        PageController.goToPage(PageEnum.PageSetupWizardInstalling)
        InstallController.install(containerIndex, port, transport)
    }

    function openContainer(containerIndex, dockerContainer) {
        ContainersModel.setProcessedContainerIndex(containerIndex)
        if (dockerContainer === ContainerEnum.Xray) {
            PageController.goToPage(PageEnum.PageSetupWizardProtocolSettings)
            return
        }

        root.installContainer(containerIndex)
    }

    BackButtonType {
        id: backButton

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: 20 + SettingsController.safeAreaTopMargin

        onActiveFocusChanged: {
            if (backButton.enabled && backButton.activeFocus) {
                listView.positionViewAtBeginning()
            }
        }
    }

    ListViewType {
        id: listView

        anchors.top: backButton.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right

        spacing: 0
        snapMode: ListView.SnapToItem

        header: ColumnLayout {
            width: listView.width
            spacing: 0

            BaseHeaderType {
                id: header

                Layout.fillWidth: true
                Layout.rightMargin: 16
                Layout.leftMargin: 16
                Layout.bottomMargin: 8

                headerTextMaximumLineCount: 10
                headerText: qsTr("Install VPN on your server")
                descriptionText: qsTr("Choose a protocol and click to install.")
            }
        }

        model: proxyContainersModel

        delegate: ColumnLayout {
            width: listView.width
            spacing: 0
            property string baseDescriptionText: easySetupDescription ? easySetupDescription : description

            LabelWithButtonType {
                Layout.fillWidth: true

                text: easySetupHeader ? (easySetupHeader + ": " + name) : name
                descriptionText: baseDescriptionText
                badgeText: ""
                badgeColor: AmneziaStyle.color.vibrantGreen
                rightImageSource: "qrc:/images/controls/chevron-right.svg"

                clickedFunction: function () {
                    var srcIndex = proxyContainersModel.mapToSource(index)
                    root.openContainer(srcIndex, dockerContainer)
                }
            }

            DividerType {}
        }

        footer: ColumnLayout {
            width: listView.width
            spacing: 0

            BasicButtonType {
                id: setupLaterButton

                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.bottomMargin: 24
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                defaultColor: AmneziaStyle.color.transparent
                hoveredColor: AmneziaStyle.color.translucentWhite
                pressedColor: AmneziaStyle.color.sheerWhite
                disabledColor: AmneziaStyle.color.mutedGray
                textColor: AmneziaStyle.color.paleGray
                borderWidth: 0

                visible: {
                    if (PageController.isTriggeredByConnectButton()) {
                        PageController.setTriggeredByConnectButton(false)
                        return false
                    }
                    return true
                }

                text: qsTr("Skip setup")

                clickedFunc: function() {
                    PageController.goToPage(PageEnum.PageSetupWizardInstalling)
                    InstallController.addEmptyServer()
                }
            }
        }
    }
}

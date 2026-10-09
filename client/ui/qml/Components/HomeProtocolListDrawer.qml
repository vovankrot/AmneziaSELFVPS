import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Style 1.0

import "../Controls2"
import "../Controls2/TextTypes"
import "../Config"

// Bottom-sheet protocol picker for the home screen, mirroring HomeServerListDrawer.
// Replaces the old inline segmented control, which broke down into unreadable "..."
// pills once a server had more than 2-3 installed protocols (XRay, AmneziaWG,
// Hysteria2, Shadowsocks, ... all squeezed into one row). Switching while connected
// is allowed: root.switchXrayRealityVariant -> ConnectionController.switchToContainer
// already disconnects + auto-reconnects on the new protocol; only the brief
// connecting/disconnecting transition itself blocks a tap. by vovankrot
DrawerType2 {
    id: root

    property var protocolsModel
    property var switchFunction
    property int currentContainer: -1

    anchors.fill: parent
    expandedHeight: Math.min(parent.height * 0.75, 120 + (root.protocolsModel ? root.protocolsModel.count * 74 : 0))

    expandedStateContent: Item {
        implicitHeight: root.expandedHeight

        ColumnLayout {
            id: header
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 0

            Header2Type {
                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.bottomMargin: 8

                headerText: qsTr("Protocol")
                descriptionText: ConnectionController.isConnected
                                 ? qsTr("Tap a protocol to switch — VPN will reconnect automatically")
                                 : qsTr("Tap a protocol to make it the default")
            }
        }

        ListViewType {
            id: listView

            anchors.top: header.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.topMargin: 12
            anchors.bottomMargin: 8

            model: root.protocolsModel
            clip: true

            delegate: Item {
                implicitWidth: listView.width
                implicitHeight: delegateContent.implicitHeight

                ColumnLayout {
                    id: delegateContent

                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16
                    spacing: 0

                    VerticalRadioButton {
                        Layout.fillWidth: true

                        text: name
                        descriptionText: description

                        checked: dockerContainer === root.currentContainer
                        // Selection belongs to the model. A click must not move the
                        // indicator before the controller accepts the protocol switch.
                        checkable: false
                        autoExclusive: false
                        enabled: !ConnectionController.isConnectionInProgress
                        objectName: "homeProtocolOption_" + dockerContainer

                        onClicked: {
                            const drawer = root
                            const tapped = dockerContainer

                            if (tapped === drawer.currentContainer) {
                                return
                            }

                            drawer.closeTriggered()
                            if (drawer.switchFunction) {
                                drawer.switchFunction(tapped)
                            }
                        }

                        Keys.onEnterPressed: this.clicked()
                        Keys.onReturnPressed: this.clicked()
                    }

                    DividerType {
                        Layout.fillWidth: true
                    }
                }
            }
        }
    }
}

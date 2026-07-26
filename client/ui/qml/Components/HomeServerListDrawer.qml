import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import PageEnum 1.0
import Style 1.0

import "../Controls2"
import "../Controls2/TextTypes"
import "../Config"

// Bottom-sheet server picker for the home screen. The redesign moved server
// switching off the main screen entirely; this brings it back as a one-tap
// drawer so you can flip the default server without diving into settings.
// Switching while connected is ALLOWED (mirrors the protocol switcher): it
// disconnects, flips the default server, and auto-reconnects to the new one
// via ConnectionController.switchToServer -- only blocked during the brief
// connecting/disconnecting transition itself. by vovankrot
DrawerType2 {
    id: root

    anchors.fill: parent
    expandedHeight: parent.height * 0.75

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

                headerText: qsTr("Servers")
                descriptionText: ConnectionController.isConnected
                                 ? qsTr("Tap a server to switch — VPN will reconnect automatically")
                                 : qsTr("Tap a server to make it the default")
            }
        }

        ButtonGroup {
            id: serverPickerGroup
        }

        ListViewType {
            id: listView

            property int selectedIndex: ServersModel.defaultIndex

            anchors.top: header.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: manageButton.top
            anchors.topMargin: 12
            anchors.bottomMargin: 8

            model: ServersModel
            clip: true

            Connections {
                target: ServersModel
                function onDefaultServerIndexChanged(serverIndex) {
                    listView.selectedIndex = serverIndex
                }
            }

            delegate: Item {
                implicitWidth: listView.width
                implicitHeight: delegateContent.implicitHeight

                ColumnLayout {
                    id: delegateContent

                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16
                    spacing: 0

                    RowLayout {
                        Layout.fillWidth: true

                        VerticalRadioButton {
                            Layout.fillWidth: true

                            text: name
                            descriptionText: serverDescription

                            checked: index === listView.selectedIndex
                            checkable: true

                            ButtonGroup.group: serverPickerGroup

                            // Mirror the protocol drawer: don't refuse taps just because a
                            // previous switch is still Disconnecting/Connecting. switchToServer
                            // is idempotent about the same-target case and folds concurrent
                            // switches through m_reconnectAfterDisconnect. by vovankrot
                            onClicked: {
                                if (index === listView.selectedIndex) {
                                    return
                                }

                                listView.selectedIndex = index
                                ConnectionController.switchToServer(index)
                                root.closeTriggered()
                            }

                            Keys.onEnterPressed: this.clicked()
                            Keys.onReturnPressed: this.clicked()
                        }

                        ImageButtonType {
                            image: "qrc:/images/controls/settings.svg"
                            imageColor: AmneziaStyle.color.paleGray

                            implicitWidth: 56
                            implicitHeight: 56
                            z: 1

                            onClicked: function() {
                                ServersModel.processedIndex = index

                                if (ServersModel.getProcessedServerData("isServerFromGatewayApi")) {
                                    if (ServersModel.getProcessedServerData("isCountrySelectionAvailable")) {
                                        PageController.goToPage(PageEnum.PageSettingsApiAvailableCountries)
                                    } else {
                                        PageController.showBusyIndicator(true)
                                        let result = ApiSettingsController.getAccountInfo(false)
                                        PageController.showBusyIndicator(false)
                                        if (!result) {
                                            return
                                        }
                                        PageController.goToPage(PageEnum.PageSettingsApiServerInfo)
                                    }
                                } else {
                                    PageController.goToPage(PageEnum.PageSettingsServerInfo)
                                }

                                root.closeTriggered()
                            }
                        }
                    }

                    DividerType {
                        Layout.fillWidth: true
                    }
                }
            }
        }

        BasicButtonType {
            id: manageButton

            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            anchors.bottomMargin: 24

            implicitHeight: 44

            defaultColor: AmneziaStyle.color.transparent
            hoveredColor: AmneziaStyle.color.translucentWhite
            pressedColor: AmneziaStyle.color.sheerWhite
            textColor: AmneziaStyle.color.paleGray
            borderWidth: 1

            text: qsTr("Manage servers")
            leftImageSource: "qrc:/images/controls/settings.svg"

            onClicked: {
                root.closeTriggered()
                PageController.goToPage(PageEnum.PageSettingsServersList)
            }

            Keys.onEnterPressed: this.clicked()
            Keys.onReturnPressed: this.clicked()
        }
    }
}

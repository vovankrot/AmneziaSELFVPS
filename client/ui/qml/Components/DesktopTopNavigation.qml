import QtQuick
import QtQuick.Layouts

import Style 1.0

Item {
    id: root

    implicitHeight: 104 + SettingsController.safeAreaTopMargin
    height: implicitHeight

    property int currentIndex: 0
    property bool navigationEnabled: true
    property bool clientsVisible: true

    signal homeClicked
    signal clientsClicked
    signal settingsClicked

    component NavigationButton: Item {
        id: navigationButton

        property string text
        property bool selected: false
        property bool buttonEnabled: true
        property var clickedFunc: function() {}

        implicitWidth: Math.max(104, navigationLabel.implicitWidth + 34)
        implicitHeight: 48
        opacity: buttonEnabled ? 1.0 : 0.4

        Text {
            id: navigationLabel
            anchors.centerIn: parent
            text: navigationButton.text
            color: navigationButton.selected ? AmneziaStyle.color.paleGray : AmneziaStyle.color.mutedGray
            font.family: "Inter"
            font.pixelSize: 14
            font.weight: navigationButton.selected ? 600 : 500
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 2
            color: AmneziaStyle.color.goldenApricot
            visible: navigationButton.selected
        }

        Rectangle {
            anchors.fill: parent
            anchors.margins: 5
            z: -1
            radius: 7
            color: navigationMouse.containsMouse && !navigationButton.selected
                   ? AmneziaStyle.color.translucentWhite : "transparent"
        }

        MouseArea {
            id: navigationMouse
            anchors.fill: parent
            hoverEnabled: true
            enabled: navigationButton.buttonEnabled && root.navigationEnabled
            cursorShape: Qt.PointingHandCursor
            onClicked: navigationButton.clickedFunc()
        }
    }

    Rectangle {
        anchors.fill: parent
        color: AmneziaStyle.color.accentGradientBottom

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: AmneziaStyle.color.slateGray
        }

        Row {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.leftMargin: 30
            anchors.topMargin: 15 + SettingsController.safeAreaTopMargin
            height: 40
            spacing: 0

            Text {
                text: "SELF"
                color: AmneziaStyle.color.paleGray
                font.family: "Inter"
                font.pixelSize: 18
                font.weight: 700
            }
            Text {
                text: "VPS"
                color: AmneziaStyle.color.goldenApricot
                font.family: "Inter"
                font.pixelSize: 18
                font.weight: 700
            }

        }

        RowLayout {
            anchors.right: parent.right
            anchors.rightMargin: 30
            anchors.top: parent.top
            anchors.topMargin: 15 + SettingsController.safeAreaTopMargin
            height: 40
            spacing: 7

            Rectangle {
                Layout.preferredWidth: 8
                Layout.preferredHeight: 8
                radius: 4
                color: ConnectionController.isConnected
                       ? AmneziaStyle.color.vibrantGreen
                       : (ConnectionController.isConnectionInProgress
                          ? AmneziaStyle.color.goldenApricot
                          : AmneziaStyle.color.charcoalGray)
            }
            Text {
                text: ConnectionController.connectionStateText
                color: ConnectionController.isConnected
                       ? AmneziaStyle.color.vibrantGreen : AmneziaStyle.color.mutedGray
                font.family: "Inter"
                font.pixelSize: 12
                font.weight: 600
            }
        }

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 30
            anchors.bottom: parent.bottom
            height: 48
            spacing: 4

            NavigationButton {
                objectName: "desktopHomeNavigation"
                text: qsTr("Home")
                selected: root.currentIndex === 0
                clickedFunc: function() { root.homeClicked() }
            }
            NavigationButton {
                objectName: "desktopClientsNavigation"
                text: qsTr("Clients")
                visible: root.clientsVisible
                buttonEnabled: visible
                selected: root.currentIndex === 1
                clickedFunc: function() { root.clientsClicked() }
            }
            NavigationButton {
                objectName: "desktopSettingsNavigation"
                text: qsTr("Settings")
                selected: root.currentIndex === 2
                clickedFunc: function() { root.settingsClicked() }
            }
        }
    }
}

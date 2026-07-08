import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import ConnectionState 1.0
import PageEnum 1.0
import Style 1.0

import "../Controls2/TextTypes"

Button {
    id: root

    property string defaultButtonColor: AmneziaStyle.color.goldenApricot
    property string progressButtonColor: AmneziaStyle.color.charcoalGray
    property string connectedButtonColor: AmneziaStyle.color.goldenApricot
    property string disconnectButtonColor: AmneziaStyle.color.vibrantRed
    property bool buttonActiveFocus: activeFocus && (Qt.platform.os !== "android" || SettingsController.isOnTv())
    property color idleBorderColor: AmneziaStyle.color.transparent

    property bool isFocusable: true

    Keys.onTabPressed: FocusController.nextKeyTabItem()
    Keys.onBacktabPressed: FocusController.previousKeyTabItem()
    Keys.onUpPressed: FocusController.nextKeyUpItem()
    Keys.onDownPressed: FocusController.nextKeyDownItem()
    Keys.onLeftPressed: FocusController.nextKeyLeftItem()
    Keys.onRightPressed: FocusController.nextKeyRightItem()

    implicitWidth: 240
    implicitHeight: 56

    text: ConnectionController.actionButtonText

    Connections {
        target: ConnectionController

        function onPreparingConfig() {
            PageController.showNotificationMessage(qsTr("Unable to disconnect during configuration preparation"))
        }
    }

    background: Rectangle {
        id: bg
        anchors.fill: parent
        radius: 16
        clip: true
        color: {
            if (ConnectionController.isConnectionInProgress) {
                return root.progressButtonColor
            } else if (ConnectionController.isConnected) {
                return root.pressed ? AmneziaStyle.color.burntOrange : (root.hovered ? AmneziaStyle.color.burntOrange : root.disconnectButtonColor)
            } else {
                return root.pressed ? AmneziaStyle.color.burntOrange : (root.hovered ? AmneziaStyle.color.mutedBrown : root.defaultButtonColor)
            }
        }
        border.color: {
            if (ConnectionController.isConnectionInProgress) {
                return AmneziaStyle.color.goldenApricot
            }
            return root.buttonActiveFocus ? AmneziaStyle.color.paleGray : root.idleBorderColor
        }
        border.width: (ConnectionController.isConnectionInProgress || root.buttonActiveFocus) ? 2 : 0

        Behavior on color {
            PropertyAnimation { duration: 200 }
        }

        Rectangle {
            id: progressOutline
            visible: ConnectionController.isConnectionInProgress
            anchors.fill: parent
            anchors.margins: 3
            radius: 13
            color: AmneziaStyle.color.transparent
            border.width: 1
            border.color: AmneziaStyle.color.goldenApricot
            opacity: 0.25

            SequentialAnimation on opacity {
                running: ConnectionController.isConnectionInProgress
                loops: Animation.Infinite
                NumberAnimation { from: 0.25; to: 0.95; duration: 620; easing.type: Easing.InOutQuad }
                NumberAnimation { from: 0.95; to: 0.25; duration: 620; easing.type: Easing.InOutQuad }
            }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            enabled: false
        }
    }

    contentItem: ButtonTextType {
        height: 24

        font.weight: 700
        font.pixelSize: 18

        color: {
            if (ConnectionController.isConnectionInProgress) {
                return AmneziaStyle.color.paleGray
            } else if (ConnectionController.isConnected) {
                return AmneziaStyle.color.paleGray
            } else {
                return AmneziaStyle.color.midnightBlack
            }
        }
        text: root.text

        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    onClicked: {
        ServersModel.setProcessedServerIndex(ServersModel.defaultIndex)
        ConnectionController.connectButtonClicked()
    }

    Keys.onEnterPressed: this.clicked()
    Keys.onReturnPressed: this.clicked()
}

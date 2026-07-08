import QtQuick
import QtQuick.Controls

ToolButton {
    id: root

    property string source: ""
    property color tintColor: "white"
    property real iconWidth: 24
    property real iconHeight: 24

    enabled: false
    opacity: 1.0
    hoverEnabled: false
    focusPolicy: Qt.NoFocus
    display: AbstractButton.IconOnly
    padding: 0
    topPadding: 0
    bottomPadding: 0
    leftPadding: 0
    rightPadding: 0

    implicitWidth: iconWidth
    implicitHeight: iconHeight

    icon.source: source
    icon.color: tintColor
    icon.width: iconWidth
    icon.height: iconHeight

    background: Item {}
}
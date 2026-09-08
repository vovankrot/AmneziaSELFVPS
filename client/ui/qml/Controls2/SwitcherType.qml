import QtQuick
import "../Config"
import QtQuick.Controls
import QtQuick.Layouts

import Style 1.0

import "TextTypes"

Switch {
    id: root

    property alias descriptionText: description.text
    property string descriptionTextColor: AmneziaStyle.color.mutedGray
    property string descriptionTextDisabledColor: AmneziaStyle.color.charcoalGray

    property string textColor: AmneziaStyle.color.paleGray
    property string textDisabledColor: AmneziaStyle.color.mutedGray

    property string checkedIndicatorColor: AmneziaStyle.color.goldenApricot
    property string defaultIndicatorColor: AmneziaStyle.color.transparent
    property string checkedDisabledIndicatorColor: AmneziaStyle.color.deepBrown

    property string borderFocusedColor: AmneziaStyle.color.paleGray
    property int borderFocusedWidth: 1

    property string checkedIndicatorBorderColor: AmneziaStyle.color.goldenApricot
    property string defaultIndicatorBorderColor: AmneziaStyle.color.slateGray
    property string checkedDisabledIndicatorBorderColor: AmneziaStyle.color.deepBrown

    property string checkedInnerCircleColor: AmneziaStyle.color.pearlGray
    property string defaultInnerCircleColor: AmneziaStyle.color.paleGray
    property string checkedDisabledInnerCircleColor: AmneziaStyle.color.mutedBrown
    property string defaultDisabledInnerCircleColor: AmneziaStyle.color.charcoalGray

    property string hoveredIndicatorBackgroundColor: AmneziaStyle.color.translucentWhite
    property string defaultIndicatorBackgroundColor: AmneziaStyle.color.transparent

    property bool isFocusable: true

    Keys.onTabPressed: {
        FocusController.nextKeyTabItem()
    }

    Keys.onBacktabPressed: {
        FocusController.previousKeyTabItem()
    }

    Keys.onUpPressed: {
        FocusController.nextKeyUpItem()
    }
    
    Keys.onDownPressed: {
        FocusController.nextKeyDownItem()
    }
    
    Keys.onLeftPressed: {
        FocusController.nextKeyLeftItem()
    }

    Keys.onRightPressed: {
        FocusController.nextKeyRightItem()
    }

    hoverEnabled: enabled ? true : false
    focusPolicy: Qt.TabFocus

    indicator: Rectangle {
        id: switcher

        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter

        implicitWidth: GC.isDesktop() ? 34 : 52
        implicitHeight: GC.isDesktop() ? 20 : 32

        radius: height / 2
        color: root.checked ? (root.enabled ? root.checkedIndicatorColor : root.checkedDisabledIndicatorColor)
                            : root.defaultIndicatorColor

        border.color: root.activeFocus ? root.borderFocusedColor : (root.checked ? (root.enabled ? root.checkedIndicatorBorderColor : root.checkedDisabledIndicatorBorderColor)
                            : root.defaultIndicatorBorderColor)

        Behavior on color {
            PropertyAnimation { duration: 200 }
        }
        Behavior on border.color {
            PropertyAnimation { duration: 200 }
        }

        Rectangle {
            id: innerCircle

            anchors.verticalCenter: parent.verticalCenter
            x: root.checked ? parent.width - width - (GC.isDesktop() ? 3 : 4) : (GC.isDesktop() ? 3 : 8)
            width: GC.isDesktop() ? 14 : (root.checked ? 24 : 16)
            height: width
            radius: 23
            color: root.checked ? (root.enabled ? root.checkedInnerCircleColor : root.checkedDisabledInnerCircleColor)
                                : (root.enabled ? root.defaultInnerCircleColor : root.defaultDisabledInnerCircleColor)

            Behavior on x {
                PropertyAnimation { duration: 200 }
            }
        }

        Rectangle {
            anchors.centerIn: innerCircle
            width: GC.isDesktop() ? 30 : 40
            height: width
            radius: 23
            color: root.hovered ? root.hoveredIndicatorBackgroundColor : root.defaultIndicatorBackgroundColor

            Behavior on color {
                PropertyAnimation { duration: 200 }
            }
        }
    }

    contentItem: ColumnLayout {
        id: content

        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.right: parent.right

        ListItemTitleType {
            Layout.fillWidth: true
            rightPadding: indicator.width

            text: root.text
            color: root.enabled ? root.textColor : root.textDisabledColor
            font.pixelSize: GC.isDesktop() ? 13 : 18
        }

        CaptionTextType {
            id: description

            Layout.fillWidth: true
            rightPadding: indicator.width

            color: root.enabled ? root.descriptionTextColor : root.descriptionTextDisabledColor
            font.pixelSize: GC.isDesktop() ? 11 : 13

            visible: text !== ""
        }
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        enabled: false
    }

    Keys.onEnterPressed: event => handleSwitch(event)
    Keys.onReturnPressed: event => handleSwitch(event)
    Keys.onSpacePressed: event => handleSwitch(event)

    function handleSwitch(event) {
        if (root.enabled && !event.isAutoRepeat) {
            root.checked = !root.checked
            root.toggled()
        }
        event.accepted = true
    }
}

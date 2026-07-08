import QtQuick
import QtQuick.Controls

import Style 1.0

import "TextTypes"

TabButton {
    id: root

    property string hoveredColor: AmneziaStyle.color.richBrown
    property string defaultColor: AmneziaStyle.color.slateGray
    property string selectedColor: AmneziaStyle.color.goldenApricot

    property string textColor: AmneziaStyle.color.paleGray

    property bool isSelected: false
    property int baseFontPixelSize: Qt.platform.os === "android" ? 15 : 16
    property int minimumFontPixelSize: Qt.platform.os === "android" ? 11 : 16

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
    
    implicitHeight: 48

    hoverEnabled: true

    background: Rectangle {
        id: background

        anchors.fill: parent
        color: AmneziaStyle.color.transparent

        Rectangle {
            id: underline
            width: parent.width
            height: (root.isSelected || root.activeFocus) ? 2 : 1
            y: parent.height - height
            color: {
                if (root.isSelected || root.activeFocus) {
                    return selectedColor
                }
                return hovered ? hoveredColor : defaultColor
            }

            Behavior on color {
                PropertyAnimation { duration: 200 }
            }
        }
    }

    MouseArea {
        anchors.fill: background
        cursorShape: Qt.PointingHandCursor
        enabled: false
    }

    contentItem: ButtonTextType {
        anchors.fill: background
        height: 24
        clip: true

        font.styleName: "normal"
        font.weight: 500
        font.pixelSize: root.baseFontPixelSize
        minimumPixelSize: root.minimumFontPixelSize
        fontSizeMode: Qt.platform.os === "android" ? Text.Fit : Text.FixedSize
        color: {
            if (root.isSelected || root.activeFocus) {
                return selectedColor
            }
            return textColor
        }
        text: root.text
        elide: Text.ElideRight
        wrapMode: Text.NoWrap

        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}

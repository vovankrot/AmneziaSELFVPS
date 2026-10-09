pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Style 1.0

import "../Controls2"
import "../Controls2/TextTypes"

import "../Config"

DrawerType2 {
    id: root

    property string headerText
    property string descriptionText
    property string yesButtonText
    property string noButtonText

    property var yesButtonFunction
    property var noButtonFunction

    expandedStateContent: ColumnLayout {
        id: content

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right

        spacing: 8

        onImplicitHeightChanged: {
            root.expandedHeight = content.implicitHeight + 32 + SettingsController.safeAreaBottomMargin
        }

        Header2TextType {
            id: questionHeader
            Layout.fillWidth: true
            Layout.topMargin: 16
            Layout.rightMargin: 16
            Layout.leftMargin: 16

            text: root.headerText
            maximumLineCount: 3
            elide: Text.ElideRight
        }

        Flickable {
            id: descriptionScroll
            objectName: "questionDescriptionScroll"
            Layout.fillWidth: true
            Layout.topMargin: 8
            Layout.rightMargin: 16
            Layout.leftMargin: 16
            Layout.preferredHeight: Math.min(description.implicitHeight,
                Math.max(0, root.height - questionHeader.implicitHeight
                         - yesButton.implicitHeight - (noButton.visible ? noButton.implicitHeight : 0)
                         - 140 - SettingsController.safeAreaBottomMargin))
            contentWidth: width
            contentHeight: description.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBarType {}

            ParagraphTextType {
                id: description
                width: descriptionScroll.width
                wrapMode: Text.Wrap
                text: root.descriptionText
            }
        }

        BasicButtonType {
            id: yesButton
            objectName: "questionYesButton"
            Layout.fillWidth: true
            Layout.topMargin: 16
            Layout.rightMargin: 16
            Layout.leftMargin: 16

            text: root.yesButtonText

            clickedFunc: function() {
                if (root.yesButtonFunction && typeof root.yesButtonFunction === "function") {
                    root.yesButtonFunction()
                }
            }
        }

        BasicButtonType {
            id: noButton
            objectName: "questionNoButton"
            Layout.fillWidth: true
            Layout.rightMargin: 16
            Layout.leftMargin: 16

            defaultColor: AmneziaStyle.color.transparent
            hoveredColor: AmneziaStyle.color.translucentWhite
            pressedColor: AmneziaStyle.color.sheerWhite
            disabledColor: AmneziaStyle.color.mutedGray
            textColor: AmneziaStyle.color.paleGray
            borderWidth: 1

            visible: root.noButtonText !== ""

            text: root.noButtonText

            clickedFunc: function() {
                if (root.noButtonFunction && typeof root.noButtonFunction === "function") {
                    root.noButtonFunction()
                }
            }
        }
    }
}

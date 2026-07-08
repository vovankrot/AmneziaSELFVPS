import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Style 1.0

import "../Controls2"
import "../Controls2/TextTypes"

Item {
    id: root

    property var stackView
    readonly property bool isAndroidCompactLayout: Qt.platform.os === "android"
    readonly property color panelColor: AmneziaStyle.color.onyxBlack
    readonly property color panelBorderColor: AmneziaStyle.color.translucentWhite
    readonly property color panelMutedText: AmneziaStyle.color.mutedGray
    readonly property var quickActions: [
        { text: qsTr("XRay"), color: AmneziaStyle.color.goldenApricot, action: function() { ServerTerminalController.fetchXrayLogs() } },
        { text: qsTr("Docker"), color: AmneziaStyle.color.paleGray, action: function() { ServerTerminalController.fetchDockerLogs() } },
        { text: qsTr("Errors"), color: AmneziaStyle.color.vibrantRed, action: function() { ServerTerminalController.fetchSystemErrors() } },
        { text: qsTr("SSH"), color: AmneziaStyle.color.paleGray, action: function() { ServerTerminalController.fetchSshAuthLog() } }
    ]

    Connections {
        target: ServerTerminalController

        function onOutputChanged() {
            terminalOutput.text = ServerTerminalController.outputText
            outputFlick.contentY = Math.max(0, terminalOutput.contentHeight - outputFlick.height)
        }

        function onCommandFinished(success) {
            commandInput.enabled = true
            commandInput.forceActiveFocus()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        spacing: 12

        // Quick action buttons - pill style
        Flow {
            id: quickActionsFlow
            Layout.fillWidth: true
            spacing: root.isAndroidCompactLayout ? 10 : 8

            Repeater {
                model: root.quickActions

                Rectangle {
                    property var actionItem: (modelData && typeof modelData === "object")
                                             ? modelData
                                             : ({ text: "", color: AmneziaStyle.color.paleGray, action: null })

                    width: root.isAndroidCompactLayout
                           ? Math.max(96, Math.floor((quickActionsFlow.width - quickActionsFlow.spacing) / 2))
                           : pillText.implicitWidth + 24
                    height: root.isAndroidCompactLayout ? 40 : 32
                    radius: height / 2
                    color: pillMouse.containsMouse ? AmneziaStyle.color.richBrown : AmneziaStyle.color.charcoalGray
                    border.width: 1
                    border.color: AmneziaStyle.color.translucentWhite
                    opacity: ServerTerminalController.isBusy ? 0.5 : 1.0

                    CopyableTextType {
                        id: pillText
                        anchors.centerIn: parent
                        width: parent.width - (root.isAndroidCompactLayout ? 20 : 24)
                        text: actionItem.text || ""
                        color: actionItem.color || AmneziaStyle.color.paleGray
                        font.pixelSize: root.isAndroidCompactLayout ? 12 : 13
                        minimumPixelSize: root.isAndroidCompactLayout ? 10 : 13
                        fontSizeMode: root.isAndroidCompactLayout ? Text.Fit : Text.FixedSize
                        font.weight: Font.Medium
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        wrapMode: Text.NoWrap
                        elide: Text.ElideRight
                    }

                    MouseArea {
                        id: pillMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        enabled: !ServerTerminalController.isBusy
                        onClicked: {
                            if (actionItem.action) {
                                actionItem.action()
                            }
                        }
                    }
                }
            }
        }

        // Terminal output area
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: root.panelColor
            radius: 12
            border.width: 1
            border.color: root.panelBorderColor

            RowLayout {
                id: terminalHeader
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.topMargin: 12
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                height: 28

                CopyableTextType {
                    Layout.alignment: Qt.AlignVCenter
                    text: qsTr("Server terminal")
                    color: AmneziaStyle.color.paleGray
                    font.family: "Inter"
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }

                Item {
                    Layout.fillWidth: true
                }

                Rectangle {
                    Layout.alignment: Qt.AlignVCenter
                    width: statusText.implicitWidth + 18
                    height: 24
                    radius: 12
                    color: ServerTerminalController.isBusy ? AmneziaStyle.color.translucentRichBrown : AmneziaStyle.color.translucentSlateGray
                    border.width: 1
                    border.color: ServerTerminalController.isBusy ? AmneziaStyle.color.goldenApricot : AmneziaStyle.color.translucentWhite

                    CopyableTextType {
                        id: statusText
                        anchors.centerIn: parent
                        text: ServerTerminalController.isBusy ? qsTr("Busy") : qsTr("Ready")
                        color: ServerTerminalController.isBusy ? AmneziaStyle.color.goldenApricot : AmneziaStyle.color.paleGray
                        font.family: "Inter"
                        font.pixelSize: 11
                        font.weight: Font.Medium
                    }
                }
            }

            Flickable {
                id: outputFlick
                anchors.top: terminalHeader.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 12
                anchors.topMargin: 8
                contentWidth: width
                contentHeight: terminalOutput.contentHeight
                clip: true
                flickableDirection: Flickable.VerticalFlick
                boundsBehavior: Flickable.StopAtBounds

                TextEdit {
                    id: terminalOutput
                    width: outputFlick.width
                    readOnly: true
                    selectByMouse: true
                    color: AmneziaStyle.color.pearlGray
                    selectionColor: AmneziaStyle.color.translucentRichBrown
                    font.family: "Consolas"
                    font.pixelSize: 12
                    wrapMode: TextEdit.WrapAnywhere
                    text: ""
                    textFormat: TextEdit.PlainText
                }

                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                }
            }
        }

        // Command input row
        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Rectangle {
                Layout.fillWidth: true
                height: 44
                color: root.panelColor
                radius: 22
                border.color: commandInput.activeFocus ? AmneziaStyle.color.goldenApricot : root.panelBorderColor
                border.width: 1

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16
                    spacing: 10

                    CopyableTextType {
                        text: "›"
                        color: AmneziaStyle.color.goldenApricot
                        font.pixelSize: 18
                        font.bold: true
                    }

                    TextInput {
                        id: commandInput
                        Layout.fillWidth: true
                        color: AmneziaStyle.color.pearlGray
                        font.family: "Consolas"
                        font.pixelSize: 13
                        clip: true
                        enabled: !ServerTerminalController.isBusy

                        CopyableTextType {
                            anchors.fill: parent
                            anchors.leftMargin: 2
                            text: qsTr("Enter command...")
                            color: root.panelMutedText
                            font: parent.font
                            visible: !parent.text && !parent.activeFocus
                            verticalAlignment: Text.AlignVCenter
                        }

                        Keys.onReturnPressed: {
                            if (text.trim().length > 0) {
                                ServerTerminalController.executeCommand(text)
                                text = ""
                            }
                        }

                        Keys.onEnterPressed: {
                            if (text.trim().length > 0) {
                                ServerTerminalController.executeCommand(text)
                                text = ""
                            }
                        }
                    }
                }
            }

            Rectangle {
                width: clearText.implicitWidth + 24
                height: 44
                radius: 22
                color: clearMouse.containsMouse ? AmneziaStyle.color.richBrown : AmneziaStyle.color.charcoalGray
                border.width: 1
                border.color: root.panelBorderColor

                CopyableTextType {
                    id: clearText
                    anchors.centerIn: parent
                    text: qsTr("Clear")
                    color: AmneziaStyle.color.paleGray
                    font.family: "Inter"
                    font.pixelSize: 13
                    font.weight: Font.Medium
                }

                MouseArea {
                    id: clearMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: ServerTerminalController.clearOutput()
                }
            }
        }
    }
}

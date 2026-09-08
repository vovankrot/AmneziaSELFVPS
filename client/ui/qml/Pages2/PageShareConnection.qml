import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import QtCore

import Style 1.0

import "./"
import "../Controls2"
import "../Controls2/TextTypes"
import "../Config"

PageType {
    id: root

    property string headerText
    property string configContentHeaderText
    property string shareButtonText: qsTr("Save file")
    property string copyButtonText: qsTr("Copy connection")
    property bool isSelfHostedConfig: true
    property string configExtension: ".vpn"
    property string configCaption: qsTr("Save AmneziaVPN config")
    property string configFileName: "amnezia_config"

    readonly property bool hasQrCode: isSelfHostedConfig
                                      ? ExportController.qrCodesCount > 0
                                      : ApiConfigsController.qrCodesCount > 0

    function qrCodeAt(index) {
        if (!root.hasQrCode) return ""
        return root.isSelfHostedConfig
               ? ExportController.qrCodes[index]
               : ApiConfigsController.qrCodes[index]
    }

    function qrCodeCount() {
        return root.isSelfHostedConfig
               ? ExportController.qrCodesCount
               : ApiConfigsController.qrCodesCount
    }

    function saveConfig() {
        var fileName = ""
        if (GC.isMobile()) {
            fileName = configFileName + configExtension
        } else {
            fileName = SystemController.getFileName(
                        configCaption,
                        qsTr("Config files (*" + configExtension + ")"),
                        StandardPaths.standardLocations(StandardPaths.DocumentsLocation) + "/" + configFileName,
                        true,
                        configExtension)
        }
        if (fileName === "") return
        PageController.showBusyIndicator(true)
        ExportController.exportConfig(fileName)
        PageController.showBusyIndicator(false)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 22 + SettingsController.safeAreaTopMargin
        anchors.leftMargin: GC.isDesktop() ? 28 : 16
        anchors.rightMargin: GC.isDesktop() ? 28 : 16
        anchors.bottomMargin: 24
        spacing: 16

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            BackButtonType { Layout.preferredWidth: implicitWidth }

            Text {
                Layout.fillWidth: true
                text: root.headerText
                color: AmneziaStyle.color.paleGray
                font.family: "Inter"
                font.pixelSize: GC.isDesktop() ? 22 : 28
                font.weight: 600
                elide: Text.ElideRight
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            GridLayout {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                width: Math.min(parent.width, GC.isDesktop() ? 660 : parent.width)
                columns: GC.isDesktop() ? 2 : 1
                columnSpacing: 14
                rowSpacing: 14

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredWidth: GC.isDesktop() ? 290 : root.width - 32
                    Layout.preferredHeight: GC.isDesktop() ? 360 : 270
                    radius: 10
                    color: AmneziaStyle.color.deepBrown
                    border.width: 1
                    border.color: AmneziaStyle.color.slateGray

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 18
                        spacing: 10

                        Rectangle {
                            Layout.preferredWidth: 30
                            Layout.preferredHeight: 30
                            radius: 15
                            color: Qt.rgba(45/255, 206/255, 118/255, 0.15)
                            TintedIconType {
                                anchors.centerIn: parent
                                source: "qrc:/images/controls/check.svg"
                                tintColor: AmneziaStyle.color.vibrantGreen
                                iconWidth: 16
                                iconHeight: 16
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Client created")
                            color: AmneziaStyle.color.paleGray
                            font.family: "Inter"
                            font.pixelSize: 17
                            font.weight: 600
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Scan the QR code in AmneziaVPN or send the connection file to the client device.")
                            color: AmneziaStyle.color.mutedGray
                            font.family: "Inter"
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                        }

                        Item { Layout.fillHeight: true }

                        BasicButtonType {
                            Layout.fillWidth: true
                            text: root.copyButtonText
                            leftImageSource: "qrc:/images/controls/copy.svg"
                            defaultColor: AmneziaStyle.color.goldenApricot
                            hoveredColor: AmneziaStyle.color.softViolet
                            textColor: AmneziaStyle.color.pearlGray
                            clickedFunc: function() {
                                GC.copyToClipBoard(ExportController.config)
                                PageController.showNotificationMessage(qsTr("Copied"))
                            }
                        }
                        BasicButtonType {
                            Layout.fillWidth: true
                            text: root.shareButtonText
                            leftImageSource: "qrc:/images/controls/download.svg"
                            defaultColor: AmneziaStyle.color.onyxBlack
                            hoveredColor: AmneziaStyle.color.richBrown
                            textColor: AmneziaStyle.color.paleGray
                            borderWidth: 1
                            borderColor: AmneziaStyle.color.slateGray
                            clickedFunc: root.saveConfig
                        }
                        BasicButtonType {
                            Layout.fillWidth: true
                            visible: root.isSelfHostedConfig
                            text: qsTr("Connection settings")
                            defaultColor: AmneziaStyle.color.transparent
                            hoveredColor: AmneziaStyle.color.translucentWhite
                            textColor: AmneziaStyle.color.mutedGray
                            clickedFunc: function() { configContentDrawer.openTriggered() }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredWidth: GC.isDesktop() ? 300 : root.width - 32
                    Layout.preferredHeight: GC.isDesktop() ? 360 : 330
                    radius: 10
                    color: AmneziaStyle.color.deepBrown
                    border.width: 1
                    border.color: AmneziaStyle.color.slateGray

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 18
                        spacing: 12

                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Connection QR code")
                            color: AmneziaStyle.color.paleGray
                            font.family: "Inter"
                            font.pixelSize: 15
                            font.weight: 600
                        }

                        Rectangle {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: Math.min(parent.width, 238)
                            Layout.preferredHeight: Layout.preferredWidth
                            radius: 8
                            color: "white"
                            visible: root.hasQrCode

                            Image {
                                id: qrCodeImage
                                anchors.fill: parent
                                anchors.margins: 7
                                smooth: false
                                fillMode: Image.PreserveAspectFit
                                source: root.qrCodeAt(0)

                                Timer {
                                    property int qrIndex: 0
                                    interval: 1000
                                    running: root.hasQrCode
                                    repeat: true
                                    onTriggered: {
                                        var count = root.qrCodeCount()
                                        if (count <= 0) return
                                        qrIndex = (qrIndex + 1) % count
                                        qrCodeImage.source = root.qrCodeAt(qrIndex)
                                    }
                                }
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: root.hasQrCode
                                  ? qsTr("Open AmneziaVPN on the client device and scan this code.")
                                  : qsTr("QR code is not available for this format. Save the configuration file instead.")
                            color: AmneziaStyle.color.mutedGray
                            font.family: "Inter"
                            font.pixelSize: 10
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }
        }
    }

    DrawerType2 {
        id: configContentDrawer
        parent: root
        anchors.fill: parent
        expandedHeight: root.height * 0.9

        expandedStateContent: Item {
            implicitHeight: configContentDrawer.expandedHeight

            BackButtonType {
                id: configBackButton
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.topMargin: 16
                backButtonFunction: function() { configContentDrawer.closeTriggered() }
            }

            ColumnLayout {
                anchors.top: configBackButton.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 18

                Header2Type {
                    Layout.fillWidth: true
                    headerText: root.configContentHeaderText
                }
                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true

                    TextArea {
                        readOnly: true
                        selectByMouse: GC.isDesktop()
                        wrapMode: TextEdit.WrapAnywhere
                        text: ExportController.config
                        color: AmneziaStyle.color.paleGray
                        selectionColor: AmneziaStyle.color.richBrown
                        selectedTextColor: AmneziaStyle.color.paleGray
                        font.family: "Inter"
                        font.pixelSize: 12
                        background: Rectangle {
                            radius: 8
                            color: AmneziaStyle.color.onyxBlack
                            border.width: 1
                            border.color: AmneziaStyle.color.slateGray
                        }
                    }
                }
            }
        }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import SortFilterProxyModel 0.2

import PageEnum 1.0
import Style 1.0

import "./"
import "../Controls2"
import "../Controls2/TextTypes"
import "../Config"

PageType {
    id: root

    function escapeRegExp(value) {
        return String(value || "").replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 24 + SettingsController.safeAreaTopMargin
        anchors.leftMargin: 28
        anchors.rightMargin: 28
        anchors.bottomMargin: 22
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            BackButtonType {
                visible: !GC.isDesktop()
                Layout.preferredWidth: visible ? implicitWidth : 0
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("Servers")
                color: AmneziaStyle.color.paleGray
                font.family: "Inter"
                font.pixelSize: 22
                font.weight: 600
            }

            BasicButtonType {
                implicitWidth: Math.max(146, buttonTextLabel.implicitWidth + 52)
                implicitHeight: 38
                text: qsTr("Add server")
                leftImageSource: "qrc:/images/controls/plus.svg"
                defaultColor: AmneziaStyle.color.goldenApricot
                hoveredColor: AmneziaStyle.color.softViolet
                pressedColor: Qt.darker(AmneziaStyle.color.goldenApricot, 1.15)
                textColor: AmneziaStyle.color.pearlGray
                clickedFunc: function() {
                    PageController.goToPage(PageEnum.PageSetupWizardConfigSource)
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 42
            radius: 8
            color: AmneziaStyle.color.onyxBlack
            border.width: 1
            border.color: searchField.activeFocus
                          ? AmneziaStyle.color.goldenApricot
                          : AmneziaStyle.color.slateGray

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 13
                anchors.rightMargin: 13
                spacing: 9

                TintedIconType {
                    Layout.preferredWidth: 16
                    Layout.preferredHeight: 16
                    source: "qrc:/images/controls/search.svg"
                    tintColor: AmneziaStyle.color.mutedGray
                    iconWidth: 16
                    iconHeight: 16
                }

                TextField {
                    id: searchField
                    Layout.fillWidth: true
                    color: AmneziaStyle.color.paleGray
                    placeholderText: qsTr("Search servers")
                    placeholderTextColor: AmneziaStyle.color.charcoalGray
                    font.family: "Inter"
                    font.pixelSize: 13
                    selectByMouse: true
                    background: Item {}
                }
            }
        }

        ListView {
            id: serverList
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 9
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            model: SortFilterProxyModel {
                id: proxyServersModel
                sourceModel: ServersModel
                filters: RegExpFilter {
                    roleName: "name"
                    pattern: ".*" + root.escapeRegExp(searchField.text) + ".*"
                    caseSensitivity: Qt.CaseInsensitive
                }
            }

            delegate: Rectangle {
                id: serverCard
                required property int index
                required property string name
                required property string hostName
                required property string serverDescription
                required property bool isDefault
                required property bool isServerFromGatewayApi

                width: serverList.width
                height: 82
                radius: 10
                color: cardMouse.containsMouse
                       ? Qt.lighter(AmneziaStyle.color.deepBrown, 1.08)
                       : AmneziaStyle.color.deepBrown
                border.width: isDefault ? 1 : 1
                border.color: isDefault
                              ? AmneziaStyle.color.goldenApricot
                              : AmneziaStyle.color.slateGray

                Behavior on color { ColorAnimation { duration: 120 } }

                MouseArea {
                    id: cardMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        ServersModel.processedIndex = proxyServersModel.mapToSource(serverCard.index)
                        if (serverCard.isServerFromGatewayApi) {
                            PageController.showBusyIndicator(true)
                            var result = ApiSettingsController.getAccountInfo(false)
                            PageController.showBusyIndicator(false)
                            if (!result) return
                            PageController.goToPage(PageEnum.PageSettingsApiServerInfo)
                        } else {
                            PageController.goToPage(PageEnum.PageSettingsServerInfo)
                        }
                    }
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 14
                    spacing: 12

                    Rectangle {
                        Layout.preferredWidth: 34
                        Layout.preferredHeight: 34
                        radius: 9
                        color: AmneziaStyle.color.onyxBlack

                        TintedIconType {
                            anchors.centerIn: parent
                            source: "qrc:/images/controls/server.svg"
                            tintColor: serverCard.isDefault
                                       ? AmneziaStyle.color.goldenApricot
                                       : AmneziaStyle.color.mutedGray
                            iconWidth: 17
                            iconHeight: 17
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Text {
                                Layout.fillWidth: true
                                text: serverCard.name
                                color: AmneziaStyle.color.paleGray
                                font.family: "Inter"
                                font.pixelSize: 14
                                font.weight: 600
                                elide: Text.ElideRight
                            }

                            Rectangle {
                                visible: serverCard.isDefault
                                implicitWidth: defaultLabel.implicitWidth + 12
                                implicitHeight: 20
                                radius: 10
                                color: Qt.rgba(45/255, 206/255, 118/255, 0.13)

                                Text {
                                    id: defaultLabel
                                    anchors.centerIn: parent
                                    text: qsTr("Default")
                                    color: AmneziaStyle.color.vibrantGreen
                                    font.family: "Inter"
                                    font.pixelSize: 10
                                    font.weight: 600
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Text {
                                Layout.fillWidth: true
                                text: serverCard.serverDescription
                                color: AmneziaStyle.color.mutedGray
                                font.family: "Inter"
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                            Text {
                                text: serverCard.hostName
                                color: AmneziaStyle.color.charcoalGray
                                font.family: "Inter"
                                font.pixelSize: 11
                                elide: Text.ElideMiddle
                                Layout.maximumWidth: 180
                            }
                        }
                    }

                    TintedIconType {
                        Layout.preferredWidth: 16
                        Layout.preferredHeight: 16
                        source: "qrc:/images/controls/chevron-right.svg"
                        tintColor: AmneziaStyle.color.charcoalGray
                        iconWidth: 16
                        iconHeight: 16
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                visible: serverList.count === 0
                text: searchField.text.length > 0 ? qsTr("No matching servers") : qsTr("No servers yet")
                color: AmneziaStyle.color.mutedGray
                font.family: "Inter"
                font.pixelSize: 13
            }
        }
    }
}

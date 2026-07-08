import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Style 1.0

import "../Controls2"
import "../Controls2/TextTypes"

Item {
    id: root

    property var stackView

    Connections {
        target: ServerMonitorController
        function onMetricsUpdated() {
            cpuChart.dataModel = ServerMonitorController.cpuHistory
            ramChart.dataModel = ServerMonitorController.ramHistory
            netRxChart.dataModel = ServerMonitorController.netRxHistory
            netTxChart.dataModel = ServerMonitorController.netTxHistory
        }
    }

    Component.onCompleted: {
        ServerMonitorController.startMonitoring()
    }

    Component.onDestruction: {
        ServerMonitorController.stopMonitoring()
    }

    FlickableType {
        // FlickableType (Controls2/FlickableType.qml) already bakes in
        // anchors.bottom/left/right + width; it only needs a top anchor.
        // anchors.fill conflicts with those baked anchors -> broken layout.
        anchors.top: parent.top
        contentHeight: content.implicitHeight

        ColumnLayout {
            id: content

            // Anchor top/left/right (NOT fill) so the column grows to its
            // implicitHeight and the Flickable can actually scroll the content.
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            spacing: 16

            // Error banner
            Rectangle {
                Layout.fillWidth: true
                height: 32
                radius: 8
                color: AmneziaStyle.color.vibrantRed
                visible: ServerMonitorController.errorString.length > 0

                ParagraphTextType {
                    anchors.centerIn: parent
                    text: ServerMonitorController.errorString
                    color: AmneziaStyle.color.paleGray
                }
            }

            // CPU Chart
            Rectangle {
                Layout.fillWidth: true
                height: 136
                radius: 12
                color: AmneziaStyle.color.onyxBlack

                MetricChart {
                    id: cpuChart
                    anchors.fill: parent
                    anchors.margins: 8
                    title: qsTr("CPU")
                    maxValue: 100
                    unit: "%"
                }
            }

            // RAM Chart
            Rectangle {
                Layout.fillWidth: true
                height: 136
                radius: 12
                color: AmneziaStyle.color.onyxBlack

                MetricChart {
                    id: ramChart
                    anchors.fill: parent
                    anchors.margins: 8
                    title: qsTr("RAM (%1 / %2 MB)").arg(Math.round(ServerMonitorController.ramUsedMb)).arg(Math.round(ServerMonitorController.ramTotalMb))
                    maxValue: ServerMonitorController.ramTotalMb > 0 ? ServerMonitorController.ramTotalMb : 1024
                    unit: " MB"
                    lineColor: AmneziaStyle.color.vibrantGreen
                    fillColor: Qt.rgba(34/255, 197/255, 94/255, 0.15)
                }
            }

            // Network RX Chart
            Rectangle {
                Layout.fillWidth: true
                height: 136
                radius: 12
                color: AmneziaStyle.color.onyxBlack

                MetricChart {
                    id: netRxChart
                    anchors.fill: parent
                    anchors.margins: 8
                    title: qsTr("Network RX")
                    maxValue: 0
                    unit: " KB/s"
                    lineColor: AmneziaStyle.color.softViolet
                    fillColor: Qt.rgba(88/255, 166/255, 255/255, 0.15)
                }
            }

            // Network TX Chart
            Rectangle {
                Layout.fillWidth: true
                height: 136
                radius: 12
                color: AmneziaStyle.color.onyxBlack

                MetricChart {
                    id: netTxChart
                    anchors.fill: parent
                    anchors.margins: 8
                    title: qsTr("Network TX")
                    maxValue: 0
                    unit: " KB/s"
                    lineColor: AmneziaStyle.color.vibrantRed
                    fillColor: Qt.rgba(239/255, 68/255, 68/255, 0.15)
                }
            }

            // Info cards
            Rectangle {
                Layout.fillWidth: true
                height: infoColumn.implicitHeight + 24
                radius: 12
                color: AmneziaStyle.color.onyxBlack

                ColumnLayout {
                    id: infoColumn
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        ParagraphTextType {
                            text: qsTr("Disk usage")
                            color: AmneziaStyle.color.paleGray
                        }
                        Item { Layout.fillWidth: true }
                        ParagraphTextType {
                            text: ServerMonitorController.diskPercent + "%"
                            color: ServerMonitorController.diskPercent > 85
                                   ? AmneziaStyle.color.vibrantRed
                                   : AmneziaStyle.color.goldenApricot
                        }
                    }

                    DividerType {}

                    RowLayout {
                        Layout.fillWidth: true
                        ParagraphTextType {
                            text: qsTr("Uptime")
                            color: AmneziaStyle.color.paleGray
                        }
                        Item { Layout.fillWidth: true }
                        ParagraphTextType {
                            text: ServerMonitorController.uptime || "—"
                            color: AmneziaStyle.color.goldenApricot
                        }
                    }

                    DividerType {}

                    RowLayout {
                        Layout.fillWidth: true
                        ParagraphTextType {
                            text: qsTr("Docker containers")
                            color: AmneziaStyle.color.paleGray
                        }
                        Item { Layout.fillWidth: true }
                        ParagraphTextType {
                            text: ServerMonitorController.dockerContainers.toString()
                            color: AmneziaStyle.color.goldenApricot
                        }
                    }
                }
            }

            // Polling status
            ParagraphTextType {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: ServerMonitorController.isPolling
                      ? qsTr("Polling every 10 s via SSH")
                      : qsTr("Monitoring stopped")
                color: AmneziaStyle.color.slateGray
                font.pixelSize: 12
            }

            Item {
                Layout.fillWidth: true
                height: 16
            }
        }
    }
}

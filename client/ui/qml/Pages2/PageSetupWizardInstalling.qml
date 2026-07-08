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

    Component.onCompleted: PageController.disableTabBar(true)
    Component.onDestruction: PageController.disableTabBar(false)

    property bool isTimerRunning: true
    property string progressBarText: qsTr("Usually it takes no more than 5 minutes")
    property string logText: ""
    property int logLineCount: 0
    property int idleSeconds: 0
    property bool installFinished: false
    property bool cancelRequested: false

    function finishInstallWithError(message) {
        root.isTimerRunning = false
        root.installFinished = true
        idleWatchdog.stop()
        cancelWatchdog.stop()
        closeTimer.stop()
        closeServerTimer.stop()
        root.progressBarText = message
    }

    Timer {
        // Safety net: if the worker thread never reports back after Cancel
        // (e.g. signal lost due to unregistered metatype, or worker already
        // exited before Cancel arrived) — force-finish the page so the user
        // can leave instead of staring at a frozen progress bar.
        id: cancelWatchdog
        interval: 15000
        repeat: false
        onTriggered: {
            if (!root.installFinished) {
                root.logText += "\n\u26A0 " + qsTr("Cancel did not complete in time. You can close this page and try again.") + "\n"
                root.finishInstallWithError(qsTr("Installation canceled"))
            }
        }
    }

    Timer {
        id: idleWatchdog
        interval: 1000
        repeat: true
        running: true
        onTriggered: {
            root.idleSeconds++
            if (root.idleSeconds >= 300) {
                idleWatchdog.stop()
                root.isTimerRunning = false
                root.progressBarText = qsTr("Installation is taking too long. Something may have gone wrong.")
                root.logText += "\n⚠ " + qsTr("No response for 5 minutes. You may cancel and retry.") + "\n"
            }
        }
    }

    Timer {
        id: closeTimer
        property string finishedMessage: ""
        interval: 2000
        repeat: false
        onTriggered: {
            PageController.closePage() // close installing page
            PageController.closePage() // close protocol settings page

            if (stackView.currentItem.objectName === PageController.getPagePath(PageEnum.PageHome)) {
                PageController.restorePageHomeState(true)
            }
            if (stackView.currentItem.objectName === PageController.getPagePath(PageEnum.PageSetupWizardProtocols)) {
                PageController.goToPage(PageEnum.PageHome)
            }
            PageController.showNotificationMessage(finishedMessage)
        }
    }

    Timer {
        id: closeServerTimer
        property string finishedMessage: ""
        interval: 2000
        repeat: false
        onTriggered: {
            PageController.goToPageHome()
            PageController.showNotificationMessage(finishedMessage)
        }
    }

    Connections {
        target: InstallController

        function onInstallLogMessage(line) {
            root.logText += line + "\n"
            root.logLineCount++
            root.idleSeconds = 0
        }

        function onInstallationErrorOccurred(error) {
            if (root.installFinished) {
                return
            }

            root.finishInstallWithError(error === 204
                                        ? qsTr("Installation canceled")
                                        : qsTr("Installation failed"))
        }

        function onInstallContainerFinished(finishedMessage, isServiceInstall, container) {
            root.isTimerRunning = false
            root.installFinished = true
            idleWatchdog.stop()
            root.logText += "\n✓ " + finishedMessage + "\n"
            root.progressBarText = finishedMessage

            var containerIndex = (container !== undefined) ? container : ContainersModel.getProcessedContainerIndex()
            if (!ConnectionController.isConnected && !ContainersModel.isServiceContainer(containerIndex)) {
                ServersModel.setDefaultContainer(ServersModel.processedIndex, containerIndex)
            }

            closeTimer.finishedMessage = finishedMessage
            closeTimer.start()
        }

        function onInstallServerFinished(finishedMessage) {
            root.isTimerRunning = false
            root.installFinished = true
            idleWatchdog.stop()
            root.logText += "\n✓ " + finishedMessage + "\n"
            root.progressBarText = finishedMessage

            if (!ConnectionController.isConnected) {
                ServersModel.setDefaultServerIndex(ServersModel.getServersCount() - 1);
                ServersModel.processedIndex = ServersModel.defaultIndex
            }

            closeServerTimer.finishedMessage = finishedMessage
            closeServerTimer.start()
        }

        function onServerAlreadyExists(serverIndex) {
            PageController.goToStartPage()
            ServersModel.processedIndex = serverIndex
            PageController.goToPage(PageEnum.PageSettingsServerInfo, false)

            PageController.showErrorMessage(qsTr("The server has already been added to the application"))
        }

        function onServerIsBusy(isBusy) {
            if (root.installFinished) {
                return
            }

            if (isBusy) {
                root.progressBarText = qsTr("Amnezia has detected that your server is currently ") +
                                       qsTr("busy installing other software. Amnezia installation ") +
                                       qsTr("will pause until the server finishes installing other software")
                root.isTimerRunning = false
            } else {
                root.progressBarText = qsTr("Usually it takes no more than 5 minutes")
                root.isTimerRunning = true
            }
        }
    }

    SortFilterProxyModel {
        id: proxyContainersModel
        sourceModel: ContainersModel
        filters: [
            ValueFilter {
                roleName: "isCurrentlyProcessed"
                value: true
            }
        ]
    }

    ListViewType {
        id: listView

        anchors.fill: parent

        currentIndex: -1

        model: proxyContainersModel

        delegate: ColumnLayout {
            width: listView.width

            BaseHeaderType {
                Layout.fillWidth: true
                Layout.topMargin: 20 + SettingsController.safeAreaTopMargin
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                headerText: qsTr("Installing")
                descriptionText: name
            }

            ProgressBarType {
                id: progressBar

                Layout.fillWidth: true
                Layout.topMargin: 32
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                value: root.installFinished ? 1.0 : progressBar.value

                Behavior on value {
                    NumberAnimation { duration: 300 }
                }

                Timer {
                    id: timer

                    interval: 300
                    repeat: true
                    running: root.isTimerRunning
                    onTriggered: {
                        if (progressBar.value < 0.95)
                            progressBar.value += 0.003
                    }
                }
            }

            ParagraphTextType {
                id: progressText

                Layout.fillWidth: true
                Layout.topMargin: 8
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                text: root.progressBarText
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.minimumHeight: 200

                color: AmneziaStyle.color.onyxBlack
                radius: 12
                border.color: AmneziaStyle.color.charcoalGray
                border.width: 1

                ScrollView {
                    id: logScrollView
                    anchors.fill: parent
                    anchors.margins: 8
                    clip: true

                    TextArea {
                        id: logArea
                        readOnly: true
                        wrapMode: TextArea.Wrap
                        text: root.logText
                        color: AmneziaStyle.color.pearlGray
                        font.family: "Consolas"
                        font.pixelSize: 11
                        background: null
                        selectByMouse: true

                        onTextChanged: {
                            logArea.cursorPosition = logArea.length
                        }
                    }
                }
            }

            BasicButtonType {
                id: cancelIntallationButton

                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.bottomMargin: 24 + SettingsController.safeAreaBottomMargin
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                visible: !root.installFinished

                text: qsTr("Cancel installation")

                clickedFunc: function() {
                    root.logText += "\n⛔ " + qsTr("Cancel requested, interrupting current SSH operation...") + "\n"
                    root.progressBarText = qsTr("Cancelling now...")
                    root.isTimerRunning = false
                    root.cancelRequested = true
                    cancelIntallationButton.enabled = false
                    cancelWatchdog.restart()
                    InstallController.cancelInstallation()
                }
            }

            // Visible after cancel or error so the user can leave the page;
            // without it `installFinished=true` hides the cancel button and
            // leaves no way back to home (the tab bar is disabled on this page).
            BasicButtonType {
                id: closeInstallationButton

                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.bottomMargin: 24 + SettingsController.safeAreaBottomMargin
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                visible: (root.installFinished || root.cancelRequested) && !closeTimer.running && !closeServerTimer.running

                text: qsTr("Close")

                clickedFunc: function() {
                    PageController.goToPageHome()
                }
            }
        }
    }
}

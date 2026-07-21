import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import SortFilterProxyModel 0.2

import PageEnum 1.0
import ContainerEnum 1.0
import Style 1.0

import "./"
import "../Controls2"
import "../Controls2/TextTypes"
import "../Config"
import "../Components"

PageType {
    id: root

    // Lives on the page, not on the delegate: resetAdvancedToDefaults() resets the model,
    // which rebuilds the delegate and would otherwise collapse the section the user is
    // working in. by vovankrot
    property bool advancedShown: false

    BackButtonType {
        id: backButton

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: 20 + SettingsController.safeAreaTopMargin

        onFocusChanged: {
            if (this.activeFocus) {
                listView.positionViewAtBeginning()
            }
        }
    }

    ListViewType {
        id: listView

        anchors.top: backButton.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right

        enabled: ServersModel.isProcessedServerHasWriteAccess()
        model: XrayConfigModel

        delegate: ColumnLayout {
            width: listView.width

            property alias focusItemId: textFieldWithHeaderType.textField

            spacing: 0

            BaseHeaderType {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                headerText: qsTr("XRay settings")
            }

            TextFieldWithHeaderType {
                id: textFieldWithHeaderType

                Layout.fillWidth: true
                Layout.topMargin: 32
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                enabled: listView.enabled

                headerText: qsTr("Disguised as traffic from")
                textField.text: site

                textField.onEditingFinished: {
                    if (textField.text !== site) {
                        var tmpText = textField.text
                        tmpText = tmpText.toLocaleLowerCase()

                        if (tmpText.startsWith("https://")) {
                            tmpText = textField.text.substring(8)
                            site = tmpText
                        } else {
                            site = textField.text
                        }
                    }
                }

                checkEmptyText: true
            }

            TextFieldWithHeaderType {
                id: portTextField

                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                enabled: listView.enabled

                headerText: qsTr("Port")
                textField.text: port
                textField.maximumLength: 5
                textField.validator: IntValidator { bottom: 1; top: 65535 }

                textField.onEditingFinished: {
                    if (textField.text !== port) {
                        port = textField.text
                    }
                }

                checkEmptyText: true
            }

            // --- Advanced (mKCP + FinalMask) ------------------------------------
            // Hidden behind a switch on purpose: these values must match on BOTH ends,
            // and a wrong capacity silently caps throughput rather than failing loudly.
            // Everything here is applied to the server too (the container is reinstalled
            // on save), so what the UI shows is what actually runs. by vovankrot
            SwitcherType {
                id: advancedSwitch

                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                enabled: listView.enabled

                text: qsTr("Advanced XRay settings")
                descriptionText: qsTr("Masking and mKCP transport parameters. Default values are tuned for this fork — change them only if you understand the effect.")

                checked: root.advancedShown
                onToggled: function() {
                    root.advancedShown = checked
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0

                visible: root.advancedShown

                WarningType {
                    Layout.fillWidth: true
                    Layout.topMargin: 16
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16

                    textString: qsTr("Saving applies these settings to the server as well — the XRay container is reinstalled and every device using this server reconnects.")
                    iconPath: "qrc:/images/controls/alert-circle.svg"
                }

                SwitcherType {
                    id: maskSwitch

                    Layout.fillWidth: true
                    Layout.topMargin: 16
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16

                    enabled: listView.enabled

                    text: qsTr("Traffic masking (salamander)")
                    descriptionText: qsTr("Turns every packet into pseudo-random noise so DPI has nothing to fingerprint. Turning this off makes the connection faster but easily detectable — on a flagged IP it will most likely be blocked.")

                    checked: maskType !== "none"
                    onToggled: function() {
                        var newValue = checked ? "salamander" : "none"
                        if (newValue !== maskType) {
                            maskType = newValue
                        }
                    }
                }

                TextFieldWithHeaderType {
                    id: packetSizeField

                    Layout.fillWidth: true
                    Layout.topMargin: 16
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16

                    enabled: listView.enabled
                    visible: maskType !== "none"

                    headerText: qsTr("Packet size range (bytes)")
                    textField.text: packetSize

                    textField.onEditingFinished: {
                        if (textField.text !== packetSize) {
                            packetSize = textField.text
                        }
                    }

                    checkEmptyText: true
                }

                CaptionTextType {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16

                    visible: maskType !== "none"
                    color: AmneziaStyle.color.mutedGray
                    text: qsTr("Format: min-max, for example 512-1200. Padding packets to a random size within this range hides the real packet-length pattern.")
                }

                WarningType {
                    Layout.fillWidth: true
                    Layout.topMargin: 16
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16

                    textString: qsTr("Uplink/downlink capacity is a HARD limit in MB/s, not a hint. Lowering it to 12 capped a 300 Mbit/s link at 90 Mbit/s. Leave it at 100 unless you have a reason.")
                    iconPath: "qrc:/images/controls/alert-circle.svg"
                }

                TextFieldWithHeaderType {
                    id: uplinkField

                    Layout.fillWidth: true
                    Layout.topMargin: 16
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16

                    enabled: listView.enabled

                    headerText: qsTr("Uplink capacity (MB/s)")
                    textField.text: kcpUplinkCapacity
                    textField.maximumLength: 4
                    textField.validator: IntValidator { bottom: 1; top: 1000 }

                    textField.onEditingFinished: {
                        if (parseInt(textField.text) !== kcpUplinkCapacity) {
                            kcpUplinkCapacity = parseInt(textField.text)
                        }
                    }

                    checkEmptyText: true
                }

                TextFieldWithHeaderType {
                    id: downlinkField

                    Layout.fillWidth: true
                    Layout.topMargin: 16
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16

                    enabled: listView.enabled

                    headerText: qsTr("Downlink capacity (MB/s)")
                    textField.text: kcpDownlinkCapacity
                    textField.maximumLength: 4
                    textField.validator: IntValidator { bottom: 1; top: 1000 }

                    textField.onEditingFinished: {
                        if (parseInt(textField.text) !== kcpDownlinkCapacity) {
                            kcpDownlinkCapacity = parseInt(textField.text)
                        }
                    }

                    checkEmptyText: true
                }

                TextFieldWithHeaderType {
                    id: mtuField

                    Layout.fillWidth: true
                    Layout.topMargin: 16
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16

                    enabled: listView.enabled

                    headerText: qsTr("MTU")
                    textField.text: kcpMtu
                    textField.maximumLength: 4
                    textField.validator: IntValidator { bottom: 576; top: 1460 }

                    textField.onEditingFinished: {
                        if (parseInt(textField.text) !== kcpMtu) {
                            kcpMtu = parseInt(textField.text)
                        }
                    }

                    checkEmptyText: true
                }

                CaptionTextType {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16

                    color: AmneziaStyle.color.mutedGray
                    text: qsTr("Maximum packet size. Lower it (1200–1300) if the connection stalls on mobile networks that fragment large UDP packets.")
                }

                TextFieldWithHeaderType {
                    id: ttiField

                    Layout.fillWidth: true
                    Layout.topMargin: 16
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16

                    enabled: listView.enabled

                    headerText: qsTr("Send interval, ms (tti)")
                    textField.text: kcpTti
                    textField.maximumLength: 3
                    textField.validator: IntValidator { bottom: 10; top: 100 }

                    textField.onEditingFinished: {
                        if (parseInt(textField.text) !== kcpTti) {
                            kcpTti = parseInt(textField.text)
                        }
                    }

                    checkEmptyText: true
                }

                CaptionTextType {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16

                    color: AmneziaStyle.color.mutedGray
                    text: qsTr("How often packets are sent. Lower values reduce latency (useful for voice chat) at the cost of more overhead.")
                }

                SwitcherType {
                    id: congestionSwitch

                    Layout.fillWidth: true
                    Layout.topMargin: 16
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16

                    enabled: listView.enabled

                    text: qsTr("Congestion control")
                    descriptionText: qsTr("Slows down when the link starts losing packets. Keep it on for a lossy cross-border path.")

                    checked: kcpCongestion
                    onToggled: function() {
                        if (checked !== kcpCongestion) {
                            kcpCongestion = checked
                        }
                    }
                }

                TextFieldWithHeaderType {
                    id: readBufferField

                    Layout.fillWidth: true
                    Layout.topMargin: 16
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16

                    enabled: listView.enabled

                    headerText: qsTr("Read buffer (MB)")
                    textField.text: kcpReadBufferSize
                    textField.maximumLength: 2
                    textField.validator: IntValidator { bottom: 1; top: 64 }

                    textField.onEditingFinished: {
                        if (parseInt(textField.text) !== kcpReadBufferSize) {
                            kcpReadBufferSize = parseInt(textField.text)
                        }
                    }

                    checkEmptyText: true
                }

                TextFieldWithHeaderType {
                    id: writeBufferField

                    Layout.fillWidth: true
                    Layout.topMargin: 16
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16

                    enabled: listView.enabled

                    headerText: qsTr("Write buffer (MB)")
                    textField.text: kcpWriteBufferSize
                    textField.maximumLength: 2
                    textField.validator: IntValidator { bottom: 1; top: 64 }

                    textField.onEditingFinished: {
                        if (parseInt(textField.text) !== kcpWriteBufferSize) {
                            kcpWriteBufferSize = parseInt(textField.text)
                        }
                    }

                    checkEmptyText: true
                }

                WarningType {
                    Layout.fillWidth: true
                    Layout.topMargin: 16
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16

                    textString: qsTr("There is no TLS fingerprint (uTLS) setting here: this fork runs VLESS over mKCP with packet masking and performs no TLS handshake at all, so a fingerprint would have nothing to disguise. Reality — which does use one — is blocked on a flagged IP on any port, which is exactly why this transport was chosen.")
                    iconPath: "qrc:/images/controls/info.svg"
                }

                BasicButtonType {
                    Layout.fillWidth: true
                    Layout.topMargin: 16
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    implicitHeight: 44

                    defaultColor: AmneziaStyle.color.transparent
                    hoveredColor: AmneziaStyle.color.translucentWhite
                    pressedColor: AmneziaStyle.color.sheerWhite
                    textColor: AmneziaStyle.color.paleGray
                    borderWidth: 1

                    enabled: listView.enabled

                    text: qsTr("Reset to defaults")

                    clickedFunc: function() {
                        XrayConfigModel.resetAdvancedToDefaults()
                    }
                }
            }

            BasicButtonType {
                id: saveButton

                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.bottomMargin: 24
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                enabled: portTextField.errorText === ""
                         && packetSizeField.errorText === ""
                         && uplinkField.errorText === ""
                         && downlinkField.errorText === ""
                         && mtuField.errorText === ""
                         && ttiField.errorText === ""
                         && readBufferField.errorText === ""
                         && writeBufferField.errorText === ""

                text: qsTr("Save")

                onClicked: function() {
                    forceActiveFocus()

                    var headerText = qsTr("Save settings?")
                    var descriptionText = qsTr("All users with whom you shared a connection with will no longer be able to connect to it.")
                    var yesButtonText = qsTr("Continue")
                    var noButtonText = qsTr("Cancel")

                    var yesButtonFunction = function() {
                        if (ConnectionController.isConnected && ServersModel.getDefaultServerData("defaultContainer") === ContainersModel.getProcessedContainerIndex()) {
                            PageController.showNotificationMessage(qsTr("Unable change settings while there is an active connection"))
                            return
                        }

                        PageController.goToPage(PageEnum.PageSetupWizardInstalling);
                        InstallController.updateContainer(XrayConfigModel.getConfig())
                    }
                    var noButtonFunction = function() {
                        if (!GC.isMobile()) {
                            saveButton.forceActiveFocus()
                        }
                    }
                    showQuestionDrawer(headerText, descriptionText, yesButtonText, noButtonText, yesButtonFunction, noButtonFunction)
                }

                Keys.onEnterPressed: saveButton.clicked()
                Keys.onReturnPressed: saveButton.clicked()
            }
        }
    }
}

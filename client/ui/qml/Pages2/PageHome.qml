import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import SortFilterProxyModel 0.2

import PageEnum 1.0
import ContainerEnum 1.0
import ProtocolEnum 1.0
import ContainersModelFilters 1.0
import ContainerProps 1.0
import Style 1.0

import "./"
import "../Controls2"
import "../Controls2/TextTypes"
import "../Config"
import "../Components"

PageType {
    id: root

    // Legacy properties for backwards compatibility
    property bool serverUpdateAvailable: false
    property bool isHotReconfiguring: DiagnosticsController.isResolving
    property int xrayRealitySwitcherRefresh: 0

    Connections {
        target: InstallController

        function onHotReconfigureFinished(message, success) {
            if (success && message) {
                PageController.showNotificationMessage(message)
            }
        }
    }

    Connections {
        target: ConnectionController

        function onReconnectWithUpdatedContainer(message) {
            if (message) {
                PageController.showNotificationMessage(message)
            }
        }
    }
    
    Connections {
        target: DiagnosticsController
        
        function onIssueResolved(issueId, success, message) {
            if (success && message) {
                PageController.showNotificationMessage(message)
            }
        }
    }

    Connections {
        target: ServersModel

        function onDefaultServerContainersUpdated(containers) {
            root.xrayRealitySwitcherRefresh += 1
        }

        function onDefaultServerDefaultContainerChanged(containerIndex) {
            root.xrayRealitySwitcherRefresh += 1
        }

        function onDefaultServerIndexChanged() {
            root.xrayRealitySwitcherRefresh += 1
        }
    }

    Component.onCompleted: {
        var idx = ServersModel.defaultIndex
        if (idx >= 0) {
            InstallController.checkServerConfigUpdate(idx)
        }
    }

    function currentHomeHelpServerName() {
        return ServersModel.defaultServerName && ServersModel.defaultServerName.length > 0
               ? ServersModel.defaultServerName
               : qsTr("selected server")
    }

    function currentHomeHelpProtocolName() {
        return ServersModel.defaultServerDefaultContainerName && ServersModel.defaultServerDefaultContainerName.length > 0
               ? ServersModel.defaultServerDefaultContainerName
               : qsTr("selected protocol")
    }

    function buildHomeHelpDescription() {
        var serverName = currentHomeHelpServerName()
        var protocolName = currentHomeHelpProtocolName()

        if (!ServersModel.getServersCount()) {
            return qsTr("No server is configured yet. Press Connect and Amnezia will open the setup flow so you can add or import a server first.\n\nAfter that, the main button will use the default server and its selected VPN protocol.")
        }

        if (ConnectionController.isConnectionInProgress) {
            return qsTr("Connection is changing right now. Amnezia is preparing, connecting, or disconnecting the VPN. Wait until the current operation finishes before trying another action.\n\nCurrent target: %1 via %2.\n\nIf some apps or sites should stay outside the VPN, open split tunneling settings from the button below.")
                    .arg(serverName)
                    .arg(protocolName)
        }

        if (ConnectionController.isConnected) {
            return qsTr("VPN is active. The app is currently connected to %1 via %2.\n\nPress the red button to disconnect. Open split tunneling if some apps or sites should bypass the VPN.")
                    .arg(serverName)
                    .arg(protocolName)
        }

        return qsTr("VPN is currently disconnected. Press Connect to start VPN on %1 via %2.\n\nUse the server section below to switch the default server or protocol before connecting.\n\nIf some apps or sites should stay outside the VPN, open split tunneling settings from the button below.")
                .arg(serverName)
                .arg(protocolName)
    }

    function buildHomeHelpPrimaryActionText() {
        return ServersModel.getServersCount() > 0
               ? qsTr("Split tunneling")
               : qsTr("Open server list")
    }

    function openHomeHelpPrimaryAction() {
        if (ServersModel.getServersCount() > 0) {
            PageController.goToPage(PageEnum.PageSettingsSplitTunneling)
            return
        }

        PageController.goToPage(PageEnum.PageSettingsServersList)
    }

    function showHomeHelpDrawer() {
        showQuestionDrawer(
            qsTr("Main screen help"),
            buildHomeHelpDescription(),
            buildHomeHelpPrimaryActionText(),
            qsTr("Close"),
            function() {
                root.openHomeHelpPrimaryAction()
            },
            undefined)
    }

    function defaultContainer() {
        return ServersModel.getDefaultServerData("defaultContainer")
    }

    function defaultServerContainerInstalled(container) {
        root.xrayRealitySwitcherRefresh
        return ServersModel.getServersCount() > 0 && DefaultServerContainersModel.isInstalled(container)
    }

    function isXrayRealityVariant(container) {
        return container === ContainerEnum.Xray || container === ContainerEnum.AnyTls
    }

    function showXrayRealityVariantSwitcher() {
        root.xrayRealitySwitcherRefresh
        return !ServersModel.isDefaultServerFromApi
               && (root.isXrayRealityVariant(root.defaultContainer())
                   || root.defaultServerContainerInstalled(ContainerEnum.Xray)
                   || root.defaultServerContainerInstalled(ContainerEnum.AnyTls))
    }

    function switchXrayRealityVariant(container) {
        if (ConnectionController.isConnectionInProgress || root.defaultContainer() === container) {
            return
        }

        if (root.defaultServerContainerInstalled(container)) {
            ConnectionController.switchToContainer(container)
            return
        }

        ServersModel.processedIndex = ServersModel.defaultIndex
        ContainersModel.setProcessedContainerIndex(container === ContainerEnum.AnyTls ? ContainerEnum.Xray : container)
        InstallController.setShouldCreateServer(false)
        InstallController.shouldUseAnyTlsVariant = container === ContainerEnum.AnyTls
        PageController.goToPage(PageEnum.PageSetupWizardProtocolSettings)
    }

    function xrayRealityButtonColor(container) {
        return root.defaultContainer() === container ? AmneziaStyle.color.goldenApricot : AmneziaStyle.color.charcoalGray
    }

    function xrayRealityButtonTextColor(container) {
        return root.defaultContainer() === container ? AmneziaStyle.color.midnightBlack : AmneziaStyle.color.paleGray
    }

    // Installed VPN protocols on the default server — drives the home protocol selector.
    // Auto-updates when the default server's containers change.
    SortFilterProxyModel {
        id: installedProtocolsModel
        sourceModel: DefaultServerContainersModel
        filters: [
            ValueFilter { roleName: "serviceType"; value: ProtocolEnum.Vpn },
            ValueFilter { roleName: "isInstalled"; value: true }
        ]
    }

    Connections {
        target: Qt.application

        function onStateChanged() {
            if (Qt.application.state !== Qt.ApplicationActive) {
                if (drawer.isOpened) {
                    drawer.closeTriggered()
                }
                if (homeSplitTunnelingDrawer.isOpened) {
                    homeSplitTunnelingDrawer.closeTriggered()
                }
            }
        }
    }

    Connections {
        objectName: "pageControllerConnections"

        target: PageController

        function onRestorePageHomeState(isContainerInstalled) {
            drawer.openTriggered()
            if (isContainerInstalled) {
                // containersDropDown lives inside drawer.contentItem and may not
                // be instantiated yet on early restores (the drawer's content is
                // lazily created). Guard against ReferenceError and defer the
                // call until the QML object tree is fully constructed.
                Qt.callLater(function() {
                    try {
                        if (typeof containersDropDown !== "undefined" && containersDropDown !== null) {
                            containersDropDown.rootButtonClickedFunction()
                        }
                    } catch (e) {
                        console.warn("PageHome: containersDropDown not ready:", e)
                    }
                })
            }
        }
    }


    Flickable {
        id: homeFlickable
        objectName: "homeColumnItem"

        anchors.fill: parent
        anchors.bottomMargin: 0
        contentHeight: homeColumnLayout.height + homeColumnLayout.y + 16
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick

        ColumnLayout {
            id: homeColumnLayout
            objectName: "homeColumnLayout"

            y: 12 + SettingsController.safeAreaTopMargin
            width: homeFlickable.width
            height: Math.max(homeFlickable.height - y - 16, implicitHeight)

            BasicButtonType {
                id: loggingButton
                objectName: "loggingButton"

                property bool isLoggingEnabled: SettingsController.isLoggingEnabled

                Layout.alignment: Qt.AlignHCenter

                implicitHeight: 36

                defaultColor: AmneziaStyle.color.transparent
                hoveredColor: AmneziaStyle.color.translucentWhite
                pressedColor: AmneziaStyle.color.sheerWhite
                disabledColor: AmneziaStyle.color.mutedGray
                textColor: AmneziaStyle.color.mutedGray
                borderWidth: 0

                visible: isLoggingEnabled ? true : false
                text: qsTr("Logging enabled")

                Keys.onEnterPressed: this.clicked()
                Keys.onReturnPressed: this.clicked()

                onClicked: {
                    PageController.goToPage(PageEnum.PageSettingsLogging)
                }
            }

            BasicButtonType {
                id: devGatewayButton
                objectName: "devGatewayButton"

                property bool isDevGatewayEnabled: SettingsController.isDevGatewayEnv

                Layout.alignment: Qt.AlignHCenter

                implicitHeight: 36

                defaultColor: AmneziaStyle.color.transparent
                hoveredColor: AmneziaStyle.color.translucentWhite
                pressedColor: AmneziaStyle.color.sheerWhite
                disabledColor: AmneziaStyle.color.mutedGray
                textColor: AmneziaStyle.color.mutedGray
                borderWidth: 0

                visible: SettingsController.isDevModeEnabled && isDevGatewayEnabled
                text: qsTr("Dev gateway enabled")

                Keys.onEnterPressed: this.clicked()
                Keys.onReturnPressed: this.clicked()

                onClicked: {
                    PageController.goToPage(PageEnum.PageDevMenu)
                }
            }

            Item {
                id: connectionShieldBlock
                objectName: "connectionShieldBlock"

                // Tri-state derived from ConnectionController:
                //   state 0 = disconnected (grey, no glow)
                //   state 1 = in progress  (blue, pulsing ring)
                //   state 2 = connected    (green, soft glow + check)
                readonly property int shieldState: ConnectionController.isConnectionInProgress
                                                   ? 1
                                                   : (ConnectionController.isConnected ? 2 : 0)

                readonly property color shieldColor: shieldState === 2
                                                     ? AmneziaStyle.color.vibrantGreen
                                                     : (shieldState === 1
                                                        ? AmneziaStyle.color.goldenApricot
                                                        : AmneziaStyle.color.deepBrown)

                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 16
                Layout.bottomMargin: 4

                implicitWidth: 132
                implicitHeight: 132

                Behavior on implicitHeight { NumberAnimation { duration: 200 } }

                // ── Fake green glow (stacked low-opacity scaled copies) ──────────
                // QtQuick-only; no Qt5Compat graphical effects.
                Repeater {
                    model: 3
                    delegate: Item {
                        anchors.centerIn: parent
                        width: connectionShieldBlock.implicitWidth
                        height: connectionShieldBlock.implicitHeight

                        opacity: connectionShieldBlock.shieldState === 2
                                 ? (0.10 - index * 0.03)
                                 : 0.0
                        scale: 1.0 + (index + 1) * 0.05

                        Behavior on opacity { NumberAnimation { duration: 260 } }

                        Canvas {
                            id: glowCanvas
                            anchors.fill: parent
                            renderStrategy: Canvas.Cooperative

                            property color glowColor: AmneziaStyle.color.vibrantGreen
                            onGlowColorChanged: requestPaint()

                            onPaint: {
                                var ctx = getContext("2d")
                                ctx.reset()
                                connectionShieldBlock.drawShieldPath(ctx, width, height, 8)
                                ctx.fillStyle = glowColor
                                ctx.fill()
                            }
                        }
                    }
                }

                // ── Connecting indicator: dots spinner INSIDE the shield ─────────
                // Browser-style loader (rotating dots), per concept — replaces
                // the old pulsing ring. by vovankrot
                Item {
                    id: shieldSpinner
                    z: 5  // must sit ABOVE the opaque shield Canvas, else hidden
                    anchors.centerIn: parent
                    anchors.verticalCenterOffset: -6
                    width: connectionShieldBlock.implicitWidth * 0.5
                    height: width
                    visible: connectionShieldBlock.shieldState === 1

                    Repeater {
                        model: 8
                        delegate: Rectangle {
                            width: 6
                            height: 6
                            radius: 3
                            color: AmneziaStyle.color.paleGray
                            opacity: 1.0 - (index / 8.0) * 0.82
                            x: shieldSpinner.width / 2 - width / 2
                               + Math.cos(index * Math.PI / 4) * (shieldSpinner.width / 2 - 4)
                            y: shieldSpinner.height / 2 - height / 2
                               + Math.sin(index * Math.PI / 4) * (shieldSpinner.height / 2 - 4)
                        }
                    }

                    RotationAnimation on rotation {
                        running: connectionShieldBlock.shieldState === 1
                        loops: Animation.Infinite
                        from: 0
                        to: 360
                        duration: 900
                    }
                }

                // Shared shield-path builder (rounded crest shield).
                function drawShieldPath(ctx, w, h, pad) {
                    var x0 = pad
                    var x1 = w - pad
                    var y0 = pad
                    var cx = w / 2
                    var topR = (x1 - x0) * 0.12
                    var shoulderY = h * 0.42
                    var tipY = h - pad

                    ctx.beginPath()
                    ctx.moveTo(x0 + topR, y0)
                    ctx.lineTo(x1 - topR, y0)
                    ctx.quadraticCurveTo(x1, y0, x1, y0 + topR)
                    ctx.lineTo(x1, shoulderY)
                    ctx.quadraticCurveTo(x1, h * 0.78, cx, tipY)
                    ctx.quadraticCurveTo(x0, h * 0.78, x0, shoulderY)
                    ctx.lineTo(x0, y0 + topR)
                    ctx.quadraticCurveTo(x0, y0, x0 + topR, y0)
                    ctx.closePath()
                }

                // ── Main shield body ─────────────────────────────────────────────
                Canvas {
                    id: shieldCanvas
                    anchors.fill: parent
                    renderStrategy: Canvas.Cooperative

                    // Animatable fill color so the state change tweens smoothly.
                    property color fillColor: connectionShieldBlock.shieldColor
                    Behavior on fillColor { ColorAnimation { duration: 220 } }
                    onFillColorChanged: requestPaint()

                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.reset()
                        connectionShieldBlock.drawShieldPath(ctx, width, height, 8)
                        // Subtle vertical gradient gives the shield depth instead of
                        // a flat fill (especially the dark disconnected state).
                        var g = ctx.createLinearGradient(0, 0, 0, height)
                        g.addColorStop(0, Qt.lighter(fillColor, 1.35))
                        g.addColorStop(1, Qt.darker(fillColor, 1.10))
                        ctx.fillStyle = g
                        ctx.fill()
                        // Clearly visible thin contour (the concept's outlined shield).
                        ctx.lineJoin = "round"
                        ctx.lineWidth = 1.5
                        ctx.strokeStyle = Qt.rgba(1, 1, 1, 0.28)
                        ctx.stroke()
                    }
                }

                // ── CONNECTED check overlay (existing check.svg asset) ───────────
                Image {
                    id: shieldCheck
                    anchors.centerIn: parent
                    anchors.verticalCenterOffset: -2
                    width: 44
                    height: 44
                    source: "qrc:/images/controls/check.svg"
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    opacity: connectionShieldBlock.shieldState === 2 ? 1.0 : 0.0
                    scale: connectionShieldBlock.shieldState === 2 ? 1.0 : 0.6

                    Behavior on opacity { NumberAnimation { duration: 220 } }
                    Behavior on scale { NumberAnimation { duration: 220; easing.type: Easing.OutBack } }
                }
            }

            // ── Status + server name + ping (concept layout). by vovankrot ───
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 2
                spacing: 7

                Rectangle {
                    Layout.alignment: Qt.AlignVCenter
                    width: 8
                    height: 8
                    radius: 4
                    color: ConnectionController.isConnected
                           ? AmneziaStyle.color.vibrantGreen
                           : (ConnectionController.isConnectionInProgress
                              ? AmneziaStyle.color.goldenApricot
                              : AmneziaStyle.color.charcoalGray)
                }
                Text {
                    text: ConnectionController.connectionStateText
                    color: ConnectionController.isConnected
                           ? AmneziaStyle.color.vibrantGreen
                           : (ConnectionController.isConnectionInProgress
                              ? AmneziaStyle.color.goldenApricot
                              : AmneziaStyle.color.mutedGray)
                    font.family: "Inter"
                    font.pixelSize: 15
                    font.weight: 600
                }
            }

            Text {
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 6
                visible: ServersModel.getServersCount() > 0
                text: ServersModel.defaultServerName
                color: AmneziaStyle.color.paleGray
                font.family: "Inter"
                font.pixelSize: 20
                font.weight: 700
            }

            Text {
                Layout.alignment: Qt.AlignHCenter
                visible: SpeedTestController.serverPingText.length > 0
                text: qsTr("ping") + " " + SpeedTestController.serverPingText
                color: AmneziaStyle.color.mutedGray
                font.family: "Inter"
                font.pixelSize: 13
            }

            Item {
                id: connectButtonBlock
                objectName: "connectButtonBlock"

                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: Math.min(parent.width - 64, 324)
                Layout.topMargin: 8
                Layout.bottomMargin: 8

                implicitWidth: 324
                implicitHeight: Math.max(connectButton.implicitHeight, homeHelpButton.implicitHeight)

                ConnectButton {
                    id: connectButton
                    objectName: "connectButton"

                    width: Math.max(0, parent.width - homeHelpButton.implicitWidth - 8)
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                }

                ImageButtonType {
                    id: homeHelpButton
                    objectName: "homeHelpButton"

                    implicitWidth: 36
                    implicitHeight: 36
                    anchors.left: connectButton.right
                    anchors.leftMargin: 8
                    anchors.verticalCenter: connectButton.verticalCenter

                    image: "qrc:/images/controls/help-circle.svg"
                    imageColor: AmneziaStyle.color.goldenApricot
                    defaultColor: AmneziaStyle.color.charcoalGray
                    hoveredColor: AmneziaStyle.color.slateGray
                    pressedColor: AmneziaStyle.color.translucentWhite
                    backgroundRadius: 18

                    onClicked: {
                        root.showHomeHelpDrawer()
                    }

                    Keys.onEnterPressed: this.clicked()
                    Keys.onReturnPressed: this.clicked()
                }
            }

            // ── Info cards (concept): Protocol/Obfuscation + Traffic/IP ──────
            Rectangle {
                Layout.alignment: Qt.AlignHCenter
                Layout.fillWidth: true
                Layout.maximumWidth: 360
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.topMargin: 12
                implicitHeight: 62
                radius: 12
                color: AmneziaStyle.color.deepBrown
                visible: ServersModel.getServersCount() > 0

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text { text: qsTr("Protocol"); color: AmneziaStyle.color.mutedGray; font.family: "Inter"; font.pixelSize: 11 }
                        Text {
                            text: ServersModel.defaultServerDefaultContainerName
                            color: AmneziaStyle.color.paleGray; font.family: "Inter"; font.pixelSize: 14; font.weight: 600
                            elide: Text.ElideRight; Layout.fillWidth: true
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text { text: qsTr("Obfuscation"); color: AmneziaStyle.color.mutedGray; font.family: "Inter"; font.pixelSize: 11 }
                        Text {
                            text: ConnectionController.isConnected ? qsTr("Enabled") : qsTr("Off")
                            color: ConnectionController.isConnected ? AmneziaStyle.color.vibrantGreen : AmneziaStyle.color.mutedGray
                            font.family: "Inter"; font.pixelSize: 14; font.weight: 600
                        }
                    }
                }
            }

            Rectangle {
                Layout.alignment: Qt.AlignHCenter
                Layout.fillWidth: true
                Layout.maximumWidth: 360
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.topMargin: 8
                implicitHeight: 62
                radius: 12
                color: AmneziaStyle.color.deepBrown
                visible: ServersModel.getServersCount() > 0

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text { text: qsTr("Traffic"); color: AmneziaStyle.color.mutedGray; font.family: "Inter"; font.pixelSize: 11 }
                        Text {
                            text: (SpeedTestController.downloadSpeed > 0 || SpeedTestController.uploadSpeed > 0)
                                  ? ("↓ " + SpeedTestController.downloadSpeed.toFixed(1) + "  ↑ " + SpeedTestController.uploadSpeed.toFixed(1) + " Mbps")
                                  : "—"
                            color: AmneziaStyle.color.paleGray; font.family: "Inter"; font.pixelSize: 13; font.weight: 600
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text { text: qsTr("IP address"); color: AmneziaStyle.color.mutedGray; font.family: "Inter"; font.pixelSize: 11 }
                        RowLayout {
                            spacing: 6
                            Text {
                                id: ipValueText
                                Layout.fillWidth: true
                                text: ServersModel.getDefaultServerData("hostName")
                                color: AmneziaStyle.color.paleGray; font.family: "Inter"; font.pixelSize: 14; font.weight: 600
                                elide: Text.ElideRight
                            }
                            Image {
                                source: "qrc:/images/controls/copy.svg"
                                sourceSize.width: 14
                                sourceSize.height: 14
                                opacity: 0.6
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        GC.copyToClipBoard(ipValueText.text)
                                        PageController.showNotificationMessage(qsTr("Copied"))
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Reality DNS Recovery Button
            BasicButtonType {
                id: realityDnsRecoveryButton
                objectName: "realityDnsRecoveryButton"

                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 4
                Layout.bottomMargin: 2

                implicitHeight: 34

                defaultColor: AmneziaStyle.color.charcoalGray
                hoveredColor: AmneziaStyle.color.slateGray
                pressedColor: AmneziaStyle.color.translucentWhite
                disabledColor: AmneziaStyle.color.mutedGray
                textColor: AmneziaStyle.color.goldenApricot
                borderWidth: 0

                visible: {
                    var containerName = (ServersModel.defaultServerDefaultContainerName || "").toLowerCase()
                    return containerName.indexOf("xray") !== -1 || containerName.indexOf("ssxray") !== -1
                }

                text: qsTr("Recover Reality DNS")

                Keys.onEnterPressed: this.clicked()
                Keys.onReturnPressed: this.clicked()

                onClicked: {
                    InstallController.recoverRealityDns()
                    PageController.showNotificationMessage(qsTr("Recovering Reality DNS on server..."))
                }
            }

            CaptionTextType {
                id: realityDnsRecoveryHint
                objectName: "realityDnsRecoveryHint"

                // fillWidth is required so Layout.maximumWidth takes effect;
                // otherwise the Text uses its (one-line) implicit width and
                // visually overflows the column instead of wrapping centered.
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignHCenter
                Layout.maximumWidth: 320
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.bottomMargin: 2

                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: AmneziaStyle.color.mutedGray
                font.pixelSize: 11

                visible: realityDnsRecoveryButton.visible

                text: qsTr("Use this if the VPN is connected but sites do not open: the XRay container will be restarted and its DNS cache cleared.")
            }

            ColumnLayout {
                id: xrayRealityVariantBlock
                objectName: "xrayRealityVariantBlock"

                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 8
                Layout.bottomMargin: 2
                spacing: 4

                visible: !ServersModel.isDefaultServerFromApi && installedProtocolsModel.count > 1

                CaptionTextType {
                    Layout.alignment: Qt.AlignHCenter

                    text: qsTr("Protocol")
                    color: AmneziaStyle.color.mutedGray
                    font.pixelSize: 12
                    font.weight: 500
                }

                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: Math.max(188, installedProtocolsModel.count * 94)
                    Layout.preferredHeight: 34

                    radius: 8
                    color: AmneziaStyle.color.midnightBlack
                    border.width: 1
                    border.color: AmneziaStyle.color.charcoalGray

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 3
                        spacing: 3

                        Repeater {
                            model: installedProtocolsModel

                            delegate: BasicButtonType {
                                Layout.fillWidth: true
                                Layout.fillHeight: true

                                implicitHeight: 28
                                leftPadding: 8
                                rightPadding: 8

                                defaultColor: root.xrayRealityButtonColor(dockerContainer)
                                hoveredColor: root.defaultContainer() === dockerContainer ? AmneziaStyle.color.goldenApricot : AmneziaStyle.color.slateGray
                                pressedColor: AmneziaStyle.color.burntOrange
                                disabledColor: AmneziaStyle.color.charcoalGray
                                textColor: root.xrayRealityButtonTextColor(dockerContainer)
                                borderWidth: 0
                                text: name
                                enabled: !ConnectionController.isConnectionInProgress

                                buttonTextLabel.font.pixelSize: 12
                                buttonTextLabel.font.weight: 600

                                clickedFunc: function() {
                                    root.switchXrayRealityVariant(dockerContainer)
                                }
                            }
                        }
                    }
                }
            }

            // Auto-Failover toggle (Beta feature)
            RowLayout {
                id: autoFailoverRow
                Layout.alignment: Qt.AlignHCenter
                spacing: 8
                visible: ServersModel.getServersCount() > 1

                CaptionTextType {
                    text: qsTr("Auto-failover")
                    color: SettingsController.isAutoFailoverEnabled
                           ? AmneziaStyle.color.goldenApricot
                           : AmneziaStyle.color.mutedGray
                    font.pixelSize: 13
                }

                SwitcherType {
                    id: autoFailoverSwitch
                    implicitWidth: 42
                    implicitHeight: 24
                    checked: SettingsController.isAutoFailoverEnabled
                    onCheckedChanged: {
                        if (checked !== SettingsController.isAutoFailoverEnabled) {
                            SettingsController.isAutoFailoverEnabled = checked
                        }
                    }
                }

                Image {
                    source: "qrc:/images/controls/info.svg"
                    sourceSize.width: 16
                    sourceSize.height: 16
                    opacity: 0.5

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            PageController.showNotificationMessage(
                                qsTr("Beta: Auto-switch to backup server/protocol when connection degrades"))
                        }
                    }
                }
            }

            // Diagnostics banner - shows issues one at a time
            Rectangle {
                id: diagnosticsBanner
                visible: DiagnosticsController.hasIssues
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 8
                Layout.preferredWidth: Math.min(parent.width - 32, 360)
                Layout.maximumHeight: 220
                implicitHeight: Math.min(220, diagnosticsBannerCol.implicitHeight + 16)
                clip: true
                radius: 12
                // Warning card per redesign: dark surface + amber accent border.
                color: AmneziaStyle.color.deepBrown
                border.width: 1
                border.color: Qt.rgba(245/255, 158/255, 11/255, 0.55)  // amber

                ColumnLayout {
                    id: diagnosticsBannerCol
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 8
                    
                    property var currentIssue: DiagnosticsController.currentIssue

                    CaptionTextType {
                        Layout.fillWidth: true
                        text: DiagnosticsController.isResolving
                              ? qsTr("Applying fix...")
                              : (diagnosticsBannerCol.currentIssue.message || "")
                        color: {
                            var severity = diagnosticsBannerCol.currentIssue.severity || 0
                            if (severity >= 2) return AmneziaStyle.color.vibrantRed  // Error/Critical
                            return AmneziaStyle.color.burntOrange  // Info/Warning (amber)
                        }
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                        maximumLineCount: 3
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignHCenter
                    }

                    BusyIndicator {
                        Layout.alignment: Qt.AlignHCenter
                        running: DiagnosticsController.isResolving
                        visible: DiagnosticsController.isResolving
                        implicitWidth: 32
                        implicitHeight: 32
                        palette.dark: AmneziaStyle.color.goldenApricot
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        visible: !DiagnosticsController.isResolving
                                 && (diagnosticsBannerCol.currentIssue.details || "") !== ""
                        radius: 8
                        color: AmneziaStyle.color.midnightBlack
                        border.width: 1
                        border.color: AmneziaStyle.color.translucentWhite
                        implicitHeight: Math.min(72, diagnosticsErrorText.contentHeight + 16)

                        ScrollView {
                            anchors.fill: parent
                            anchors.margins: 8
                            clip: true

                            TextArea {
                                id: diagnosticsErrorText
                                width: diagnosticsBanner.width - 48
                                readOnly: true
                                selectByMouse: GC.isDesktop()
                                wrapMode: TextEdit.WrapAnywhere
                                text: diagnosticsBannerCol.currentIssue.details || ""
                                color: AmneziaStyle.color.paleGray
                                font.pixelSize: 11
                                leftPadding: 0
                                rightPadding: 0
                                topPadding: 0
                                bottomPadding: 0
                                background: null
                            }
                        }
                    }

                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        spacing: 8
                        visible: !DiagnosticsController.isResolving

                        BasicButtonType {
                            implicitHeight: 28
                            leftPadding: 16
                            rightPadding: 16
                            defaultColor: AmneziaStyle.color.burntOrange
                            hoveredColor: Qt.rgba(245/255, 158/255, 11/255, 0.82)
                            pressedColor: Qt.rgba(245/255, 158/255, 11/255, 0.70)
                            textColor: AmneziaStyle.color.midnightBlack
                            text: diagnosticsBannerCol.currentIssue.actionText || qsTr("Fix")
                            font.pixelSize: 12
                            enabled: !DiagnosticsController.isResolving

                            onClicked: {
                                DiagnosticsController.resolveCurrentIssue()
                            }
                        }
                        
                        // Skip button (only if more than one issue)
                        BasicButtonType {
                            implicitHeight: 28
                            leftPadding: 12
                            rightPadding: 12
                            visible: (diagnosticsBannerCol.currentIssue.details || "") !== ""
                            defaultColor: "transparent"
                            hoveredColor: AmneziaStyle.color.charcoalGray
                            pressedColor: AmneziaStyle.color.charcoalGray
                            textColor: AmneziaStyle.color.paleGray
                            borderWidth: 1
                            text: qsTr("Copy error")
                            font.pixelSize: 11

                            onClicked: {
                                GC.copyToClipBoard(diagnosticsBannerCol.currentIssue.details || "")
                                PageController.showNotificationMessage(qsTr("Copied"))
                            }
                        }

                        BasicButtonType {
                            implicitHeight: 28
                            leftPadding: 12
                            rightPadding: 12
                            visible: DiagnosticsController.issueCount > 1
                            defaultColor: "transparent"
                            hoveredColor: AmneziaStyle.color.charcoalGray
                            pressedColor: AmneziaStyle.color.charcoalGray
                            textColor: AmneziaStyle.color.mutedGray
                            borderWidth: 1
                            text: qsTr("Skip")
                            font.pixelSize: 11

                            onClicked: {
                                DiagnosticsController.skipCurrentIssue()
                            }
                        }
                    }
                    
                    // Issue counter
                    CaptionTextType {
                        Layout.alignment: Qt.AlignHCenter
                        visible: DiagnosticsController.issueCount > 1 && !DiagnosticsController.isResolving
                        text: qsTr("%1 of %2 issues").arg(1).arg(DiagnosticsController.issueCount)
                        color: AmneziaStyle.color.mutedGray
                        font.pixelSize: 10
                    }
                }
            }

            ColumnLayout {
                id: speedTestBlock
                objectName: "speedTestBlock"

                Layout.alignment: Qt.AlignHCenter
                spacing: 4

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 16
                    visible: SpeedTestController.serverPingText.length > 0 || SpeedTestController.moscowPingText.length > 0

                    CaptionTextType {
                        visible: SpeedTestController.serverPingText.length > 0
                        text: qsTr("VPN server") + ": " + SpeedTestController.serverPingText
                        color: AmneziaStyle.color.mutedGray
                        font.pixelSize: 12
                        font.weight: 500
                    }

                    CaptionTextType {
                        visible: SpeedTestController.moscowPingText.length > 0
                        text: qsTr("Moscow") + ": " + SpeedTestController.moscowPingText
                        color: AmneziaStyle.color.mutedGray
                        font.pixelSize: 12
                        font.weight: 500
                    }
                }

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 16
                    visible: SpeedTestController.downloadSpeed > 0 || SpeedTestController.uploadSpeed > 0

                    CaptionTextType {
                        text: "↓ " + SpeedTestController.downloadSpeed.toFixed(1) + " Mbps"
                        color: AmneziaStyle.color.goldenApricot
                        font.pixelSize: 14
                        font.weight: 500
                    }
                    CaptionTextType {
                        text: "↑ " + SpeedTestController.uploadSpeed.toFixed(1) + " Mbps"
                        color: AmneziaStyle.color.goldenApricot
                        font.pixelSize: 14
                        font.weight: 500
                        visible: SpeedTestController.uploadSpeed > 0
                    }
                }

                BasicButtonType {
                    id: speedTestButton
                    objectName: "speedTestButton"

                    Layout.alignment: Qt.AlignHCenter
                    leftPadding: 16
                    rightPadding: 16

                    implicitHeight: 36

                    defaultColor: AmneziaStyle.color.transparent
                    hoveredColor: AmneziaStyle.color.translucentWhite
                    pressedColor: AmneziaStyle.color.sheerWhite
                    disabledColor: AmneziaStyle.color.transparent
                    textColor: SpeedTestController.isRunning
                               ? AmneziaStyle.color.goldenApricot
                               : ((SpeedTestController.statusText.length > 0
                                   && SpeedTestController.downloadSpeed === 0
                                   && SpeedTestController.uploadSpeed === 0)
                                  ? AmneziaStyle.color.vibrantRed
                                  : AmneziaStyle.color.mutedGray)
                    borderWidth: 0

                    buttonTextLabel.lineHeight: 20
                    buttonTextLabel.font.pixelSize: 14
                    buttonTextLabel.font.weight: 500

                    enabled: !SpeedTestController.isRunning
                    text: (SpeedTestController.isRunning
                           || (SpeedTestController.statusText.length > 0
                               && SpeedTestController.downloadSpeed === 0
                               && SpeedTestController.uploadSpeed === 0))
                          ? SpeedTestController.statusText
                          : qsTr("Speed test")

                    leftImageSource: "qrc:/images/controls/gauge.svg"
                    leftImageColor: SpeedTestController.isRunning
                                    ? AmneziaStyle.color.goldenApricot
                                    : ((SpeedTestController.statusText.length > 0
                                        && SpeedTestController.downloadSpeed === 0
                                        && SpeedTestController.uploadSpeed === 0)
                                       ? AmneziaStyle.color.vibrantRed
                                       : AmneziaStyle.color.mutedGray)

                    Keys.onEnterPressed: this.clicked()
                    Keys.onReturnPressed: this.clicked()

                    onClicked: {
                        SpeedTestController.runSpeedTest()
                    }
                }
            }

            BasicButtonType {
                id: splitTunnelingButton
                objectName: "splitTunnelingButton"

                Layout.alignment: Qt.AlignHCenter | Qt.AlignBottom
                leftPadding: 16
                rightPadding: 16

                implicitHeight: 36

                defaultColor: AmneziaStyle.color.transparent
                hoveredColor: AmneziaStyle.color.translucentWhite
                pressedColor: AmneziaStyle.color.sheerWhite
                disabledColor: AmneziaStyle.color.mutedGray
                textColor: AmneziaStyle.color.mutedGray
                borderWidth: 0

                buttonTextLabel.lineHeight: 20
                buttonTextLabel.font.pixelSize: 14
                buttonTextLabel.font.weight: 500

                property bool siteSplitTunnelingSupported: {
                    ServersModel.defaultServerDefaultContainerName // reactive dep (defaultServerDefaultContainerChanged): refresh after a container reinstall. by vovankrot
                    return ContainerProps.supportsSiteSplitTunneling(ServersModel.getDefaultServerData("defaultContainer"))
                }
                property bool isSplitTunnelingEnabled: (siteSplitTunnelingSupported && SitesModel.isTunnelingEnabled) || AppSplitTunnelingModel.isTunnelingEnabled ||
                                                       ServersModel.isDefaultServerDefaultContainerHasSplitTunneling

                text: isSplitTunnelingEnabled ? qsTr("Split tunneling enabled") : qsTr("Split tunneling disabled")

                leftImageSource: isSplitTunnelingEnabled ? "qrc:/images/controls/split-tunneling.svg" : ""
                leftImageColor: ""
                rightImageSource: "qrc:/images/controls/chevron-down.svg"

                Keys.onEnterPressed: this.clicked()
                Keys.onReturnPressed: this.clicked()

                onClicked: {
                    homeSplitTunnelingDrawer.openTriggered()
                }

                HomeSplitTunnelingDrawer {
                    id: homeSplitTunnelingDrawer
                    objectName: "homeSplitTunnelingDrawer"

                    parent: root
                }
            }

            AdLabel {
                id: adLabel

                Layout.fillWidth: true
                Layout.preferredHeight: adLabel.contentHeight
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.topMargin: 22
            }

            // ── Signature — by vovankrot (clean, no glow halo) ───────────────
            Text {
                id: neonLabel
                objectName: "neonSignature"

                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 14
                Layout.bottomMargin: 6

                text: "by vovankrot"
                color: AmneziaStyle.color.softViolet
                font.family: "Inter"
                font.pixelSize: 13
                font.weight: 600
                font.italic: true
                opacity: 0.85
            }
        }
    }

    DrawerType2 {
        id: drawer
        objectName: "drawerProtocol"

        // Bottom server drawer removed per concept — server/protocol/IP are now
        // shown in the cards above; switch servers via the Servers page. by vovankrot
        visible: false

        anchors.fill: parent

        collapsedStateContent: Item {
            objectName: "ProtocolDrawerCollapsedContent"

            implicitHeight: Qt.platform.os !== "ios" ? root.height * 0.9 : screen.height * 0.77
            Component.onCompleted: {
                drawer.expandedHeight = implicitHeight
            }

            ColumnLayout {
                id: collapsed
                objectName: "collapsedColumnLayout"

                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 0

                Component.onCompleted: {
                    drawer.collapsedHeight = collapsed.implicitHeight
                }

                DividerType {
                    Layout.topMargin: 10
                    Layout.fillWidth: false
                    Layout.preferredWidth: 20
                    Layout.preferredHeight: 2
                    Layout.alignment: Qt.AlignHCenter | Qt.AlignVCenter
                }

                RowLayout {
                    objectName: "rowLayout"

                    Layout.topMargin: 14
                    Layout.leftMargin: 24
                    Layout.rightMargin: 24
                    Layout.alignment: Qt.AlignHCenter | Qt.AlignVCenter

                    spacing: 0

                    Connections {
                        objectName: "drawerConnections"

                        target: drawer
                        function onCursorEntered() {
                            if (drawer.isCollapsedStateActive) {
                                collapsedButtonChevron.backgroundColor = collapsedButtonChevron.hoveredColor
                                collapsedButtonHeader.opacity = 0.8
                            } else {
                                collapsedButtonHeader.opacity = 1
                            }
                        }

                        function onCursorExited() {
                            if (drawer.isCollapsedStateActive) {
                                collapsedButtonChevron.backgroundColor = collapsedButtonChevron.defaultColor
                                collapsedButtonHeader.opacity = 1
                            } else {
                                collapsedButtonHeader.opacity = 1
                            }
                        }

                        function onPressed(pressed, entered) {
                            if (drawer.isCollapsedStateActive) {
                                collapsedButtonChevron.backgroundColor = pressed ? collapsedButtonChevron.pressedColor : entered ? collapsedButtonChevron.hoveredColor : collapsedButtonChevron.defaultColor
                                collapsedButtonHeader.opacity = 0.7
                            } else {
                                collapsedButtonHeader.opacity = 1
                            }
                        }
                    }

                    Header1TextType {
                        id: collapsedButtonHeader
                        objectName: "collapsedButtonHeader"

                        Layout.maximumWidth: drawer.width - 48 - 18 - 12

                        maximumLineCount: 2
                        elide: Qt.ElideRight

                        text: ServersModel.defaultServerName
                        horizontalAlignment: Qt.AlignHCenter

                        Behavior on opacity {
                            PropertyAnimation { duration: 200 }
                        }
                    }

                    ImageButtonType {
                        id: collapsedButtonChevron
                        objectName: "collapsedButtonChevron"

                        Layout.leftMargin: 8

                        visible: drawer.isCollapsedStateActive()

                        hoverEnabled: false
                        image: "qrc:/images/controls/chevron-down.svg"
                        imageColor: AmneziaStyle.color.paleGray

                        icon.width: 18
                        icon.height: 18
                        backgroundRadius: 16
                        horizontalPadding: 4
                        topPadding: 4
                        bottomPadding: 3

                        Keys.onEnterPressed: this.clicked()
                        Keys.onReturnPressed: this.clicked()

                        onClicked: {
                            if (drawer.isCollapsedStateActive()) {
                                drawer.openTriggered()
                            }
                        }
                    }
                }

                RowLayout {
                    objectName: "rowLayoutLabel"
                    Layout.alignment: Qt.AlignHCenter | Qt.AlignVCenter
                    Layout.topMargin: 8
                    Layout.bottomMargin: drawer.isCollapsedStateActive ? 44 : ServersModel.isDefaultServerFromApi ? 61 : 16
                    spacing: 0

                    BasicButtonType {
                        enabled: (ServersModel.defaultServerImagePathCollapsed !== "") && drawer.isCollapsedStateActive
                        hoverEnabled: enabled

                        implicitHeight: 36

                        leftPadding: 16
                        rightPadding: 16

                        defaultColor: AmneziaStyle.color.transparent
                        hoveredColor: AmneziaStyle.color.translucentWhite
                        pressedColor: AmneziaStyle.color.sheerWhite
                        disabledColor: AmneziaStyle.color.transparent
                        textColor: AmneziaStyle.color.mutedGray

                        buttonTextLabel.lineHeight: 16
                        buttonTextLabel.font.pixelSize: 13
                        buttonTextLabel.font.weight: 400

                        text: drawer.isCollapsedStateActive ? ServersModel.defaultServerDescriptionCollapsed : ServersModel.defaultServerDescriptionExpanded
                        leftImageSource: ServersModel.defaultServerImagePathCollapsed
                        leftImageColor: ""
                        changeLeftImageSize: false

                        rightImageSource: hoverEnabled ? "qrc:/images/controls/chevron-down.svg" : ""

                        Keys.onEnterPressed: this.clicked()
                        Keys.onReturnPressed: this.clicked()

                        onClicked: {
                            ServersModel.processedIndex = ServersModel.defaultIndex

                            if (ServersModel.getProcessedServerData("isServerFromGatewayApi")) {
                                if (ServersModel.getProcessedServerData("isCountrySelectionAvailable")) {
                                    PageController.goToPage(PageEnum.PageSettingsApiAvailableCountries)
                                } else {
                                    PageController.showBusyIndicator(true)
                                    let result = ApiSettingsController.getAccountInfo(false)
                                    PageController.showBusyIndicator(false)
                                    if (!result) {
                                        return
                                    }

                                    PageController.goToPage(PageEnum.PageSettingsApiServerInfo)
                                }
                            } else {
                                PageController.goToPage(PageEnum.PageSettingsServerInfo)
                            }
                        }
                    }
                }
            }

            ColumnLayout {
                id: serversMenuHeader
                objectName: "serversMenuHeader"

                anchors.top: collapsed.bottom
                anchors.right: parent.right
                anchors.left: parent.left

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter | Qt.AlignVCenter
                    spacing: 8

                    visible: !ServersModel.isDefaultServerFromApi

                    DropDownType {
                        id: containersDropDown
                        objectName: "containersDropDown"

                        rootButtonImageColor: AmneziaStyle.color.midnightBlack
                        rootButtonBackgroundColor: AmneziaStyle.color.paleGray
                        rootButtonBackgroundHoveredColor: AmneziaStyle.color.mistyGray
                        rootButtonBackgroundPressedColor: AmneziaStyle.color.cloudyGray
                        rootButtonHoveredBorderColor: AmneziaStyle.color.transparent
                        rootButtonDefaultBorderColor: AmneziaStyle.color.transparent
                        rootButtonTextTopMargin: 8
                        rootButtonTextBottomMargin: 8

                        enabled: drawer.isOpened

                        text: ServersModel.defaultServerDefaultContainerName
                        textColor: AmneziaStyle.color.midnightBlack
                        headerText: qsTr("VPN protocol")
                        headerBackButtonImage: "qrc:/images/controls/arrow-left.svg"

                        rootButtonClickedFunction: function() {
                            containersDropDown.openTriggered()
                        }

                        drawerParent: root

                        listView: HomeContainersListView {
                            id: containersListView
                            objectName: "containersListView"

                            rootWidth: root.width

                            Connections {
                                objectName: "rowLayoutConnections"

                                target: ServersModel

                                function onDefaultServerIndexChanged() {
                                    updateContainersModelFilters()
                                }
                            }

                            function updateContainersModelFilters() {
                                if (ServersModel.isDefaultServerHasWriteAccess()) {
                                    proxyDefaultServerContainersModel.filters = ContainersModelFilters.getWriteAccessProtocolsListFilters()
                                } else {
                                    proxyDefaultServerContainersModel.filters = ContainersModelFilters.getReadAccessProtocolsListFilters()
                                }
                            }

                            model: SortFilterProxyModel {
                                id: proxyDefaultServerContainersModel
                                sourceModel: DefaultServerContainersModel

                                sorters: [
                                    RoleSorter { roleName: "isInstalled"; sortOrder: Qt.DescendingOrder }
                                ]
                            }

                            Component.onCompleted: updateContainersModelFilters()
                        }
                    }
                }

                Header2Type {
                    Layout.fillWidth: true
                    Layout.topMargin: 48
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16

                    headerText: qsTr("Servers")
                }
            }

            ButtonGroup {
                id: serversRadioButtonGroup
                objectName: "serversRadioButtonGroup"
            }

            ServersListView {
                id: serversMenuContent
                objectName: "serversMenuContent"

                isFocusable: false

                Connections {
                    target: drawer

                    // this item shouldn't be focused when drawer is closed
                    function onIsOpenedChanged() {
                        serversMenuContent.isFocusable = drawer.isOpened
                    }
                }
            }
        }
    }
}

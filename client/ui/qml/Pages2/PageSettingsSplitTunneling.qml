import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import QtCore

import SortFilterProxyModel 0.2

import PageEnum 1.0
import ProtocolEnum 1.0
import ContainerProps 1.0
import Style 1.0

import "./"
import "../Controls2"
import "../Controls2/TextTypes"
import "../Config"
import "../Components"

PageType {
    id: root

    pageBackgroundVisible: true

    property var isServerFromTelegramApi: ServersModel.getDefaultServerData("isServerFromTelegramApi")
    readonly property int defaultContainer: {
        // Reactive dep so this re-evaluates when the default container changes
        // (NOTIFY defaultServerDefaultContainerChanged); getDefaultServerData() has no
        // NOTIFY, which left site-split greyed after a reinstall flipped the container
        // None -> amnezia-xray without refreshing the binding. by vovankrot
        ServersModel.defaultServerDefaultContainerName
        return ServersModel.getDefaultServerData("defaultContainer")
    }
    readonly property int defaultProtocol: ContainerProps.defaultProtocol(defaultContainer)
    readonly property bool siteSplitTunnelingSupported: ContainerProps.supportsSiteSplitTunneling(defaultContainer)
    readonly property bool supportsGeoSiteRules: siteSplitTunnelingSupported
    readonly property string geoRoutingBadgeText: siteSplitTunnelingSupported ? qsTr("XRay routing mode") : qsTr("Unavailable")
    readonly property color geoRoutingAccentColor: siteSplitTunnelingSupported ? AmneziaStyle.color.softViolet : AmneziaStyle.color.goldenApricot
    readonly property color geoRoutingBadgeBackgroundColor: siteSplitTunnelingSupported ? AmneziaStyle.color.translucentRichBrown : AmneziaStyle.color.softGoldenApricot
    readonly property color geoRoutingInfoBackgroundColor: siteSplitTunnelingSupported ? AmneziaStyle.color.translucentSlateGray : Qt.rgba(255 / 255, 177 / 255, 94 / 255, 0.10)
    readonly property string geoRoutingMechanismText: siteSplitTunnelingSupported
                                                       ? qsTr("Domain masks, RKN rules and Russian site rules are handled inside XRay. Other protocols cannot enforce these rules reliably.")
                                                       : qsTr("Site-based split tunneling is available only for XRay and Shadowsocks over XRay. The current protocol will use normal full VPN routing.")

    readonly property bool pageEnabled: !ConnectionController.isConnectionInProgress && siteSplitTunnelingSupported
    property bool hasPendingChanges: false
    property bool isApplicationQuitting: false
    property string initialStateSignature: ""

    Timer {
        id: pendingReconnectTimer

        interval: 900
        repeat: false
        onTriggered: {
            // Run synchronously — wrapping in Qt.callLater() here used to fire on a
            // page that had already been popped from the StackView, producing
            //   "Object destroyed while one of its QML signal handlers is in progress"
            // (fatal). The Timer itself is owned by `root`, so by the time onTriggered
            // is called, `root` is guaranteed to be alive.
            root.applyPendingChangesNow()
        }
    }

    Component.onCompleted: {
        root.initialStateSignature = SitesModel.stateSignature()
        updatePendingChanges()
    }

    function escapeSearchPattern(text) {
        return text.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
    }

    function searchPattern(text) {
        if (text === "") {
            return ".*"
        }

        return ".*" + escapeSearchPattern(text) + ".*"
    }

    function updatePendingChanges() {
        if (!ConnectionController.isConnected) {
            root.hasPendingChanges = false
            return
        }

        root.hasPendingChanges = SitesModel.stateSignature() !== root.initialStateSignature
    }

    function schedulePendingReconnect() {
        updatePendingChanges()

        if (!ConnectionController.isConnected
                || ConnectionController.isConnectionInProgress
                || root.isApplicationQuitting
                || !root.hasPendingChanges) {
            return
        }

        pendingReconnectTimer.restart()
    }

    function applyPendingChangesNow(notificationMessage) {
        pendingReconnectTimer.stop()
        updatePendingChanges()

        if (!ConnectionController.isConnected
                || ConnectionController.isConnectionInProgress
                || root.isApplicationQuitting
                || !root.hasPendingChanges) {
            return false
        }

        root.hasPendingChanges = false
        root.initialStateSignature = SitesModel.stateSignature()

        reconnectWithNotification(notificationMessage || qsTr("Split tunneling settings applied, reconnecting..."))
        return true
    }

    function applyPendingChangesOnLeave() {
        pendingReconnectTimer.stop()
        updatePendingChanges()

        var shouldApply = !root.isApplicationQuitting
                && root.hasPendingChanges
                && ConnectionController.isConnected
                && !ConnectionController.isConnectionInProgress

        if (!shouldApply) {
            return
        }

        root.hasPendingChanges = false
        root.initialStateSignature = SitesModel.stateSignature()

        reconnectWithNotification(qsTr("Split tunneling settings applied, reconnecting..."))
    }

    function reconnectWithNotification(notificationMessage) {
        Qt.callLater(function() {
            if (!ConnectionController.isConnected || ConnectionController.isConnectionInProgress) {
                return
            }

            ConnectionController.reconnectToVpn()
            PageController.showNotificationMessage(notificationMessage)
        })
    }

    Component.onDestruction: {
        applyPendingChangesOnLeave()
    }

    Connections {
        target: SitesController

        function onFinished(message) {
            if (root.applyPendingChangesNow(qsTr("Split tunneling settings applied, reconnecting..."))) {
                return
            }

            PageController.showNotificationMessage(message)
        }

        function onErrorOccurred(errorMessage) {
            PageController.showErrorMessage(errorMessage)
        }
    }

    Connections {
        target: Qt.application

        function onAboutToQuit() {
            root.isApplicationQuitting = true
        }
    }

    Connections {
        target: SitesModel

        function onRouteModeChanged() {
            root.schedulePendingReconnect()
        }

        function onSplitTunnelingToggled() {
            root.schedulePendingReconnect()
        }

        function onBypassRuGeoSitesChanged() {
            root.schedulePendingReconnect()
        }

        function onBypassRuGeoIpChanged() {
            root.schedulePendingReconnect()
        }

        function onAutoBypassRknChanged() {
            root.schedulePendingReconnect()
        }

        function onSitesChanged() {
            root.schedulePendingReconnect()
        }
    }

    Connections {
        target: ConnectionController

        function onConnectionStateChanged() {
            if (!ConnectionController.isConnected) {
                pendingReconnectTimer.stop()
            }

            if (!ConnectionController.isConnected) {
                root.hasPendingChanges = false
                return
            }

            root.initialStateSignature = SitesModel.stateSignature()
        }
    }

    // --- GeoIP refresh interval choices -------------------------------------
    property list<QtObject> geoIpIntervalsModel: [
        geoIpEvery1h, geoIpEvery6h, geoIpEvery12h, geoIpEvery24h, geoIpEvery72h, geoIpEvery168h
    ]

    QtObject { id: geoIpEvery1h;   property string name: qsTr("Every hour");   property int hours: 1 }
    QtObject { id: geoIpEvery6h;   property string name: qsTr("Every 6 hours"); property int hours: 6 }
    QtObject { id: geoIpEvery12h;  property string name: qsTr("Every 12 hours"); property int hours: 12 }
    QtObject { id: geoIpEvery24h;  property string name: qsTr("Once a day");   property int hours: 24 }
    QtObject { id: geoIpEvery72h;  property string name: qsTr("Every 3 days"); property int hours: 72 }
    QtObject { id: geoIpEvery168h; property string name: qsTr("Once a week");  property int hours: 168 }

    function geoIpIntervalIndex(hours) {
        for (var i = 0; i < root.geoIpIntervalsModel.length; ++i) {
            if (root.geoIpIntervalsModel[i].hours === hours) {
                return i
            }
        }
        return 3 // once a day
    }

    function geoIpIntervalName(hours) {
        for (var i = 0; i < root.geoIpIntervalsModel.length; ++i) {
            if (root.geoIpIntervalsModel[i].hours === hours) {
                return root.geoIpIntervalsModel[i].name
            }
        }
        return qsTr("Every %1 h").arg(hours)
    }

    QtObject {
        id: routeMode
        property int allSites: 0
        property int onlyForwardSites: 1
        property int allExceptSites: 2
    }

    property list<QtObject> routeModesModel: [
        onlyForwardSites,
        allExceptSites
    ]

    QtObject {
        id: onlyForwardSites
        property string name: qsTr("Sites not in the list will bypass VPN")
        property int type: routeMode.onlyForwardSites
    }
    QtObject {
        id: allExceptSites
        property string name: qsTr("Sites not in the list will use VPN")
        property int type: routeMode.allExceptSites
    }

    function getRouteModesModelIndex() {
        var currentRouteMode = SitesModel.routeMode
        if ((routeMode.onlyForwardSites === currentRouteMode) || (routeMode.allSites === currentRouteMode)) {
            return 0
        } else if (routeMode.allExceptSites === currentRouteMode) {
            return 1
        }
    }

    BackButtonType {
        id: backButton

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right

        anchors.topMargin: 20 + SettingsController.safeAreaTopMargin
    }

    ListViewType {
        id: listView

        ScrollBar.vertical: ScrollBarType { policy: ScrollBar.AlwaysOn }

        anchors.top: backButton.bottom
        anchors.topMargin: 12
        anchors.bottom: parent.bottom
        anchors.bottomMargin: addSiteButton.implicitHeight + 48 + SettingsController.safeAreaBottomMargin + (searchField.textField.activeFocus ? 0 : SettingsController.imeHeight)
        anchors.left: parent.left
        anchors.right: parent.right

        enabled: root.pageEnabled
        clip: true

        header: ColumnLayout {
            width: listView.width

            HeaderTypeWithSwitcher {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                headerText: qsTr("Split tunneling")

                enabled: root.pageEnabled
                showSwitcher: true
                switcher {
                    checked: root.siteSplitTunnelingSupported && SitesModel.isTunnelingEnabled
                    enabled: root.pageEnabled
                }
                switcherFunction: function(checked) {
                    if (!root.siteSplitTunnelingSupported) {
                        return
                    }
                    SitesModel.toggleSplitTunneling(checked)
                    selector.text = root.routeModesModel[getRouteModesModelIndex()].name
                }
            }

            WarningType {
                Layout.fillWidth: true
                Layout.topMargin: 8
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                textString: root.geoRoutingMechanismText
                iconPath: "qrc:/images/controls/info.svg"
                visible: !root.siteSplitTunnelingSupported
            }

            WarningType {
                Layout.fillWidth: true
                Layout.topMargin: 8
                Layout.leftMargin: 16
                Layout.rightMargin: 16

            textString: SitesModel.routeMode === routeMode.onlyForwardSites
                        ? qsTr("Sites in the list below will go through VPN. Everything else bypasses VPN.")
                        : qsTr("Sites in the list below will bypass VPN. Everything else goes through VPN.")
                iconPath: "qrc:/images/controls/info.svg"

                visible: root.pageEnabled
            }

            WarningType {
                Layout.fillWidth: true
                Layout.topMargin: 8
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                textString: qsTr("Changes to site split tunneling are applied automatically a moment after editing")
                iconPath: "qrc:/images/controls/refresh-cw.svg"

                visible: ConnectionController.isConnected
            }

            SwitcherType {
                id: bypassRuSitesSwitch

                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.topMargin: 16

                text: qsTr("Russian sites without VPN")
                descriptionText: qsTr("Sites with Russian IP addresses will be accessed directly without VPN. For sites on foreign hosting, add them manually to the exclusion list.")

                enabled: root.pageEnabled && SitesModel.isTunnelingEnabled
                checked: SitesModel.bypassRuGeoIp
                opacity: SitesModel.isTunnelingEnabled ? 1.0 : 0.5
                onToggled: function() {
                    if (checked !== SitesModel.bypassRuGeoIp) {
                        SitesModel.bypassRuGeoIp = checked
                        SitesModel.bypassRuGeoSites = checked
                    }
                }
            }

            // GeoIP list status + settings. The switch above only works if a usable
            // RU CIDR list is present, so show which list is loaded, where it came from
            // and let the user point it elsewhere / change how often it refreshes.
            // by vovankrot
            ColumnLayout {
                id: geoIpCard

                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.topMargin: 8
                spacing: 8

                visible: SitesModel.isTunnelingEnabled && SitesModel.bypassRuGeoIp

                readonly property bool bundled: GeoipController.usingBundledList

                Rectangle {
                    Layout.fillWidth: true

                    radius: 16
                    color: AmneziaStyle.color.onyxBlack
                    border.width: 1
                    border.color: geoIpCard.bundled ? AmneziaStyle.color.goldenApricot
                                                    : AmneziaStyle.color.slateGray

                    implicitHeight: geoIpContent.implicitHeight + 32

                    ColumnLayout {
                        id: geoIpContent

                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 16

                        spacing: 8

                        LabelTextType {
                            Layout.fillWidth: true
                            text: qsTr("Russian IP list (GeoIP)")
                            color: AmneziaStyle.color.mutedGray
                        }

                        ParagraphTextType {
                            Layout.fillWidth: true
                            text: GeoipController.updating
                                  ? qsTr("Updating…")
                                  : (geoIpCard.bundled
                                     ? qsTr("Built-in list is in use — has not been downloaded yet")
                                     : qsTr("%n subnet(s) loaded", "", GeoipController.cidrCount))
                            color: geoIpCard.bundled ? AmneziaStyle.color.goldenApricot
                                                     : AmneziaStyle.color.paleGray
                            wrapMode: Text.WordWrap
                        }

                        CaptionTextType {
                            Layout.fillWidth: true
                            text: qsTr("Updated: %1").arg(GeoipController.lastUpdateText)
                            color: AmneziaStyle.color.mutedGray
                        }

                        CaptionTextType {
                            Layout.fillWidth: true
                            text: qsTr("File: %1").arg(GeoipController.listPath)
                            color: AmneziaStyle.color.mutedGray
                            wrapMode: Text.WrapAnywhere
                            maximumLineCount: 2
                            elide: Qt.ElideMiddle
                        }

                        CaptionTextType {
                            Layout.fillWidth: true
                            text: qsTr("Error: %1").arg(GeoipController.lastError)
                            color: AmneziaStyle.color.vibrantRed
                            wrapMode: Text.WordWrap
                            visible: GeoipController.lastError !== ""
                        }

                        TextFieldWithHeaderType {
                            id: geoIpUrlField

                            Layout.fillWidth: true
                            Layout.topMargin: 8

                            headerText: qsTr("Update source (URL of a CIDR list)")
                            textField.text: GeoipController.sourceUrl

                            // Commit only on focus loss / Enter so we don't rewrite
                            // settings on every keystroke.
                            textField.onEditingFinished: {
                                if (textField.text !== GeoipController.sourceUrl) {
                                    GeoipController.sourceUrl = textField.text
                                }
                            }

                            Connections {
                                target: GeoipController
                                function onStatusChanged() {
                                    if (!geoIpUrlField.textField.activeFocus
                                            && geoIpUrlField.textField.text !== GeoipController.sourceUrl) {
                                        geoIpUrlField.textField.text = GeoipController.sourceUrl
                                    }
                                }
                            }
                        }

                        DropDownType {
                            id: geoIpIntervalSelector

                            Layout.fillWidth: true
                            Layout.topMargin: 8

                            drawerHeight: 0.4375
                            drawerParent: root

                            headerText: qsTr("Check for updates")
                            text: root.geoIpIntervalName(GeoipController.intervalHours)

                            listView: ListViewWithRadioButtonType {
                                rootWidth: root.width

                                model: root.geoIpIntervalsModel

                                selectedIndex: root.geoIpIntervalIndex(GeoipController.intervalHours)

                                clickedFunction: function() {
                                    geoIpIntervalSelector.text = selectedText
                                    geoIpIntervalSelector.closeTriggered()
                                    GeoipController.intervalHours = root.geoIpIntervalsModel[selectedIndex].hours
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 8
                            spacing: 8

                            BasicButtonType {
                                Layout.fillWidth: true
                                implicitHeight: 44

                                enabled: !GeoipController.updating
                                text: GeoipController.updating ? qsTr("Updating…") : qsTr("Update now")

                                clickedFunc: function() {
                                    GeoipController.updateNow()
                                }
                            }

                            BasicButtonType {
                                Layout.fillWidth: true
                                implicitHeight: 44

                                defaultColor: AmneziaStyle.color.transparent
                                hoveredColor: AmneziaStyle.color.translucentWhite
                                pressedColor: AmneziaStyle.color.sheerWhite
                                textColor: AmneziaStyle.color.paleGray
                                borderWidth: 1

                                text: qsTr("Default source")

                                clickedFunc: function() {
                                    GeoipController.resetSourceToDefault()
                                }
                            }
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.topMargin: 8
                spacing: 8

                visible: SitesModel.isTunnelingEnabled

                Rectangle {
                    Layout.alignment: Qt.AlignLeft

                    radius: 10
                    color: root.geoRoutingBadgeBackgroundColor
                    border.width: 1
                    border.color: root.geoRoutingAccentColor

                    implicitHeight: geoRoutingBadgeLabel.implicitHeight + 8
                    implicitWidth: geoRoutingBadgeLabel.implicitWidth + 16
                    width: implicitWidth
                    height: implicitHeight

                    BadgeTextType {
                        id: geoRoutingBadgeLabel

                        anchors.centerIn: parent

                        text: root.geoRoutingBadgeText
                        color: root.geoRoutingAccentColor
                    }
                }

                WarningType {
                    Layout.fillWidth: true

                    textString: root.geoRoutingMechanismText
                    iconPath: "qrc:/images/controls/split-tunneling.svg"
                    imageColor: root.geoRoutingAccentColor
                    backGroundColor: root.geoRoutingInfoBackgroundColor

                    visible: SitesModel.isTunnelingEnabled
                }
            }

            SwitcherType {
                id: autoBypassRknSwitch

                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.topMargin: 16

                text: qsTr("Auto-bypass RKN blocks")
                descriptionText: qsTr("Sites blocked by Roskomnadzor will be automatically routed through VPN. The blocklist is updated from community sources every few days.")

                enabled: root.pageEnabled && SitesModel.isTunnelingEnabled
                checked: SitesModel.autoBypassRkn
                opacity: SitesModel.isTunnelingEnabled ? 1.0 : 0.5
                onToggled: function() {
                    if (checked !== SitesModel.autoBypassRkn) {
                        SitesModel.autoBypassRkn = checked
                    }
                }
            }

            DividerType {
                Layout.fillWidth: true
                Layout.topMargin: 8
            }

            DropDownType {
                id: selector

                Layout.fillWidth: true
                Layout.topMargin: 32
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                drawerHeight: 0.4375
                drawerParent: root

                enabled: root.pageEnabled

                headerText: qsTr("Default for sites not in the list")

                listView: ListViewWithRadioButtonType {
                    rootWidth: root.width

                    model: root.routeModesModel

                    selectedIndex: getRouteModesModelIndex()

                    clickedFunction: function() {
                        selector.text = selectedText
                        selector.closeTriggered()
                        if (SitesModel.routeMode !== root.routeModesModel[selectedIndex].type) {
                            SitesModel.routeMode = root.routeModesModel[selectedIndex].type
                            root.updatePendingChanges()
                        }
                    }

                    Component.onCompleted: {
                        if (root.routeModesModel[selectedIndex].type === SitesModel.routeMode) {
                            selector.text = selectedText
                        } else {
                            selector.text = root.routeModesModel[0].name
                        }
                    }

                    Connections {
                        target: SitesModel
                        function onRouteModeChanged() {
                            selectedIndex = getRouteModesModelIndex()
                        }
                    }
                }
            }

            Item {
                width: 1
                height: 16
            }
        }

        model: SortFilterProxyModel {
            id: proxySitesModel
            sourceModel: SitesModel
            filters: [
                AnyOf {
                    RegExpFilter {
                        roleName: "url"
                        pattern: root.searchPattern(searchField.textField.text)
                        caseSensitivity: Qt.CaseInsensitive
                    }
                    RegExpFilter {
                        roleName: "ip"
                        pattern: root.searchPattern(searchField.textField.text)
                        caseSensitivity: Qt.CaseInsensitive
                    }
                }
            ]
        }

        delegate: ColumnLayout {
            width: listView.width

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.topMargin: 8
                Layout.bottomMargin: 8

                spacing: 12

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    ParagraphTextType {
                        Layout.fillWidth: true
                        text: url
                        color: AmneziaStyle.color.paleGray
                        maximumLineCount: 2
                        elide: Qt.ElideRight
                    }

                    CaptionTextType {
                        Layout.fillWidth: true
                        text: ip
                        color: AmneziaStyle.color.mutedGray
                        maximumLineCount: 1
                        elide: Qt.ElideMiddle
                        visible: ip !== ""
                    }
                }

                ImageButtonType {
                    id: deleteButton

                    implicitWidth: 40
                    implicitHeight: 40

                    image: "qrc:/images/controls/trash.svg"
                    imageColor: AmneziaStyle.color.paleGray
                    hoveredColor: AmneziaStyle.color.barelyTranslucentWhite
                    pressedColor: AmneziaStyle.color.barelyTranslucentWhite

                    onClicked: {
                        var headerText = qsTr("Remove ") + url + "?"
                        var yesButtonText = qsTr("Continue")
                        var noButtonText = qsTr("Cancel")

                        var yesButtonFunction = function() {
                            SitesController.removeSite(proxySitesModel.mapToSource(index))
                        }
                        var noButtonFunction = function() {
                        }

                        showQuestionDrawer(headerText, "", yesButtonText, noButtonText, yesButtonFunction, noButtonFunction)
                    }
                }
            }

            DividerType {}
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        
        height: addSiteButton.implicitHeight + 48 + SettingsController.safeAreaBottomMargin
        
        color: AmneziaStyle.color.midnightBlack
        
        RowLayout {
            id: addSiteButton

            enabled: root.pageEnabled

            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 24
            anchors.rightMargin: 16
            anchors.leftMargin: 16
            anchors.bottomMargin: 24 + SettingsController.safeAreaBottomMargin

            TextFieldWithHeaderType {
                id: searchField

                Layout.fillWidth: true
                rightButtonClickedOnEnter: true

                textField.placeholderText: SitesModel.routeMode === routeMode.allExceptSites
                                           ? qsTr("website, IP or mask like *youtube* or *.domain.com")
                                           : qsTr("website or IP")
                buttonImageSource: "qrc:/images/controls/plus.svg"

                clickedFunc: function() {
                    PageController.showBusyIndicator(true)
                    SitesController.addSite(textField.text)
                    textField.text = ""
                    PageController.showBusyIndicator(false)
                }
            }

            ImageButtonType {
                id: addSiteButtonImage
                implicitWidth: 56
                implicitHeight: 56

                image: "qrc:/images/controls/more-vertical.svg"
                imageColor: AmneziaStyle.color.paleGray

                onClicked: function () {
                    moreActionsDrawer.openTriggered()
                }

                Keys.onReturnPressed: addSiteButtonImage.clicked()
                Keys.onEnterPressed: addSiteButtonImage.clicked()
            }
        }
    }

    DrawerType2 {
        id: moreActionsDrawer

        anchors.fill: parent
        expandedHeight: parent.height * 0.4375

        expandedStateContent: ColumnLayout {
            id: moreActionsDrawerContent

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right

            Header2Type {
                Layout.fillWidth: true
                Layout.margins: 16

                headerText: qsTr("Additional options")
            }

            LabelWithButtonType {
                id: importSitesButton
                Layout.fillWidth: true

                text: qsTr("Import")
                rightImageSource: "qrc:/images/controls/chevron-right.svg"

                clickedFunction: function() {
                    importSitesDrawer.openTriggered()
                }
            }

            DividerType {}

            LabelWithButtonType {
                id: exportSitesButton
                Layout.fillWidth: true
                text: qsTr("Save site list")

                clickedFunction: function() {
                    var fileName = ""
                    if (GC.isMobile()) {
                        fileName = "amnezia_sites.json"
                    } else {
                        fileName = SystemController.getFileName(qsTr("Save sites"),
                                                                qsTr("Sites files (*.json)"),
                                                                StandardPaths.standardLocations(StandardPaths.DocumentsLocation) + "/amnezia_sites",
                                                                true,
                                                                ".json")
                    }
                    if (fileName !== "") {
                        PageController.showBusyIndicator(true)
                        SitesController.exportSites(fileName)
                        moreActionsDrawer.closeTriggered()
                        PageController.showBusyIndicator(false)
                    }
                }
            }

            DividerType {}
            
            LabelWithButtonType {
                id: clearSitesButton
                Layout.fillWidth: true

                text: qsTr("Clear site list")

                clickedFunction: function() {
                    var headerText = qsTr("Clear site list?")
                    var descriptionText = qsTr("All sites will be removed from list.")
                    var yesButtonText = qsTr("Continue")
                    var noButtonText = qsTr("Cancel")

                    var yesButtonFunction = function() {
                        PageController.showBusyIndicator(true)
                        SitesController.removeSites()
                        PageController.showBusyIndicator(false)
                    }
                    var noButtonFunction = function() {
                        
                    }

                    showQuestionDrawer(headerText, descriptionText, yesButtonText, noButtonText, yesButtonFunction, noButtonFunction)
                }
            }

            DividerType {}
        }
    }

    DrawerType2 {
        id: importSitesDrawer

        anchors.fill: parent
        expandedHeight: parent.height * 0.4375

        expandedStateContent: Item {
            implicitHeight: importSitesDrawer.expandedHeight

            BackButtonType {
                id: importSitesDrawerBackButton

                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.topMargin: 16

                backButtonFunction: function() {
                    importSitesDrawer.closeTriggered()
                }
                
                onFocusChanged: {
                    if (this.activeFocus) {
                        importSitesDrawerListView.positionViewAtBeginning()
                    }
                }
            }

            ListViewType {
                id: importSitesDrawerListView

                anchors.top: importSitesDrawerBackButton.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom

                header: ColumnLayout {
                    width: importSitesDrawerListView.width

                    Header2Type {
                        Layout.fillWidth: true
                        Layout.margins: 16

                        headerText: qsTr("Import a list of sites")
                    }
                }

                model: importOptions

                delegate: ColumnLayout {
                    width: importSitesDrawerListView.width

                    LabelWithButtonType {
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16

                        text: title

                        clickedFunction: function() {
                            clickedHandler()
                        }
                    }

                    DividerType {}
                }
            }
        }
    }

    property list<QtObject> importOptions: [
        replaceOption,
        addOption,
    ]

    QtObject {
        id: replaceOption

        readonly property string title: qsTr("Replace site list")
        readonly property var clickedHandler: function() {
            var fileName = SystemController.getFileName(qsTr("Open sites file"),
                                                        qsTr("Sites files (*.json)"))
            if (fileName !== "") {
                root.importSites(fileName, true)
            }
        }
    }

    QtObject {
        id: addOption

        readonly property string title: qsTr("Add imported sites to existing ones")
        readonly property var clickedHandler: function() {
            var fileName = SystemController.getFileName(qsTr("Open sites file"),
                                                        qsTr("Sites files (*.json)"))
            if (fileName !== "") {
                root.importSites(fileName, false)
            }
        }
    }

    function importSites(fileName, replaceExistingSites) {
        PageController.showBusyIndicator(true)
        SitesController.importSites(fileName, replaceExistingSites)
        PageController.showBusyIndicator(false)
        importSitesDrawer.closeTriggered()
        moreActionsDrawer.closeTriggered()
    }
}

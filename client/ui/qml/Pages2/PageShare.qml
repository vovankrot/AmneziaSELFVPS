import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import SortFilterProxyModel 0.2

import PageEnum 1.0
import ContainerProps 1.0
import Style 1.0

import "./"
import "../Controls2"
import "../Controls2/TextTypes"
import "../Components"
import "../Config"


PageType {
    id: root

    property bool supportsUserManagement: true

    enum ConfigType {
        AmneziaConnection,
        OpenVpn,
        WireGuard,
        Awg,
        ShadowSocks,
        Cloak,
        Xray
    }

    Connections {
        target: ExportController

        function onRevokeConfigCompleted() {
            PageController.showBusyIndicator(false)
            PageController.showNotificationMessage(qsTr("Config revoked"))
        }

        function onGenerateConfig(type) {
            if (ExportController.clientsLoading) return
            PageController.showBusyIndicator(true)

            var configCaption
            var configExtension
            var configFileName

            switch (type) {
            case PageShare.ConfigType.AmneziaConnection: {
                ExportController.generateConnectionConfig(clientNameTextField.textField.text);
                configCaption = qsTr("Save AmneziaVPN config")
                configExtension = ".vpn"
                configFileName = "amnezia_config"
                break;
            }
            case PageShare.ConfigType.OpenVpn: {
                ExportController.generateOpenVpnConfig(clientNameTextField.textField.text)
                configCaption = qsTr("Save OpenVPN config")
                configExtension = ".ovpn"
                configFileName = "amnezia_for_openvpn"
                break
            }
            case PageShare.ConfigType.WireGuard: {
                ExportController.generateWireGuardConfig(clientNameTextField.textField.text)
                configCaption = qsTr("Save WireGuard config")
                configExtension = ".conf"
                configFileName = "amnezia_for_wireguard"
                break
            }
            case PageShare.ConfigType.Awg: {
                ExportController.generateAwgConfig(clientNameTextField.textField.text)
                configCaption = qsTr("Save AmneziaWG config")
                configExtension = ".conf"
                configFileName = "amnezia_for_awg"
                break
            }
            case PageShare.ConfigType.ShadowSocks: {
                ExportController.generateShadowSocksConfig()
                configCaption = qsTr("Save Shadowsocks config")
                configExtension = ".json"
                configFileName = "amnezia_for_shadowsocks"
                break
            }
            case PageShare.ConfigType.Cloak: {
                ExportController.generateCloakConfig()
                configCaption = qsTr("Save Cloak config")
                configExtension = ".json"
                configFileName = "amnezia_for_cloak"
                break
            }
            case PageShare.ConfigType.Xray: {
                ExportController.generateXrayConfig(clientNameTextField.textField.text)
                configCaption = qsTr("Save XRay config")
                configExtension = ".json"
                configFileName = "amnezia_for_xray"
                break
            }
            }

            PageController.showBusyIndicator(false)
            
            var headerText = qsTr("Connection to ") + serverSelector.text
            var configContentHeaderText = qsTr("File with connection settings to ") + serverSelector.text
            PageController.goToShareConnectionPage(headerText, configContentHeaderText, configCaption, configExtension, configFileName)
        }

        function onExportErrorOccurred(error) {
            PageController.showErrorMessage(error)
        }
    }

    property bool isSearchBarVisible: false
    property bool showContent: false
    property bool shareButtonEnabled: true
    property list<QtObject> connectionTypesModel: [
        amneziaConnectionFormat
    ]

    function hasText(value) {
        return value !== undefined && value !== null && String(value).length > 0
    }

    function shortKey(value) {
        var key = String(value || "")
        if (key.length <= 14) {
            return key
        }
        return key.slice(0, 8) + "..." + key.slice(-6)
    }

    function clientListSummary(online, ip, key, destination) {
        var parts = [online ? qsTr("Online") : qsTr("Offline")]
        if (hasText(ip)) {
            parts.push(qsTr("IP: %1").arg(ip))
        }
        if (hasText(key)) {
            parts.push(qsTr("Key: %1").arg(shortKey(key)))
        }
        if (hasText(destination)) {
            parts.push(qsTr("Last: %1").arg(destination))
        }
        return parts.join("  ")
    }

    function historyRows(history) {
        if (!hasText(history)) {
            return []
        }
        return String(history).split("\n").filter(function(row) { return row.length > 0 })
    }

    QtObject {
        id: amneziaConnectionFormat
        readonly property string name: qsTr("For the AmneziaVPN app")
        readonly property int type: PageShare.ConfigType.AmneziaConnection
    }
    QtObject {
        id: openVpnConnectionFormat
        readonly property string name: qsTr("OpenVPN native format")
        readonly property int type: PageShare.ConfigType.OpenVpn
    }
    QtObject {
        id: wireGuardConnectionFormat
        readonly property string name: qsTr("WireGuard native format")
        readonly property int type: PageShare.ConfigType.WireGuard
    }
    QtObject {
        id: awgConnectionFormat
        readonly property string name: qsTr("AmneziaWG native format")
        readonly property int type: PageShare.ConfigType.Awg
    }
    QtObject {
        id: shadowSocksConnectionFormat
        readonly property string name: qsTr("Shadowsocks native format")
        readonly property int type: PageShare.ConfigType.ShadowSocks
    }
    QtObject {
        id: cloakConnectionFormat
        readonly property string name: qsTr("Cloak native format")
        readonly property int type: PageShare.ConfigType.Cloak
    }
    QtObject {
        id: xrayConnectionFormat
        readonly property string name: qsTr("XRay native format")
        readonly property int type: PageShare.ConfigType.Xray
    }

    FlickableType {
        id: a

        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.leftMargin: GC.isDesktop() ? 64 : 0
        anchors.rightMargin: GC.isDesktop() ? 64 : 1
        anchors.topMargin: GC.isDesktop() ? 28 + SettingsController.safeAreaTopMargin : 0
        anchors.bottomMargin: GC.isDesktop() ? 28 : 0
        contentHeight: content.height + 10

        Rectangle {
            id: desktopCreateClientCard
            z: -1
            anchors.left: content.left
            anchors.right: content.right
            y: content.y + accessTypeSelector.y - 14
            height: Math.max(0, content.y + shareButton.y + shareButton.height - y + 14)
            radius: 10
            color: AmneziaStyle.color.deepBrown
            border.width: 1
            border.color: AmneziaStyle.color.slateGray
            visible: false
        }

        ColumnLayout {
            id: content

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right

            anchors.rightMargin: GC.isDesktop() ? 0 : 16
            anchors.leftMargin: GC.isDesktop() ? 0 : 16

            spacing: 0

            HeaderTypeWithButton {
                id: header
                Layout.fillWidth: true
                Layout.topMargin: 24 + SettingsController.safeAreaTopMargin
                visible: !GC.isDesktop()

                headerText: GC.isDesktop() ? (accessTypeSelector.currentIndex === 0 ? qsTr("New client") : qsTr("Clients")) : qsTr("Share VPN Access")

                actionButtonImage: "qrc:/images/controls/more-vertical.svg"
                actionButtonFunction: function() {
                    shareFullAccessDrawer.openTriggered()
                }

                DrawerType2 {
                    id: shareFullAccessDrawer

                    parent: root

                    anchors.fill: parent
                    expandedHeight: root.height

                    expandedStateContent: ColumnLayout {
                        id: shareFullAccessDrawerContent
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.topMargin: 16

                        spacing: 0

                        onImplicitHeightChanged: {
                            shareFullAccessDrawer.expandedHeight = shareFullAccessDrawerContent.implicitHeight + 32
                        }

                        Header2Type {
                            Layout.fillWidth: true
                            Layout.bottomMargin: 16
                            Layout.leftMargin: 16
                            Layout.rightMargin: 16

                            headerText: qsTr("Share full access to the server and VPN")
                            descriptionText: qsTr("Use for your own devices, or share with those you trust to manage the server.")
                        }

                        LabelWithButtonType {
                            id: shareFullAccessButton
                            Layout.fillWidth: true

                            text: qsTr("Share")
                            rightImageSource: "qrc:/images/controls/chevron-right.svg"

                            clickedFunction: function() {
                                PageController.goToPage(PageEnum.PageShareFullAccess)
                                shareFullAccessDrawer.closeTriggered()
                            }
                        }
                    }
                }
            }

            RowLayout {
                id: desktopClientsHeader
                objectName: "desktopClientsHeader"
                Layout.fillWidth: true
                visible: GC.isDesktop()
                spacing: 12

                Text {
                    Layout.fillWidth: true
                    text: accessTypeSelector.currentIndex === 0 ? qsTr("New client") : qsTr("Clients")
                    color: AmneziaStyle.color.paleGray
                    font.family: "Inter"
                    font.pixelSize: 24
                    font.weight: 700
                }

                BasicButtonType {
                    objectName: "desktopClientsBackButton"
                    visible: accessTypeSelector.currentIndex === 0 && root.supportsUserManagement
                    text: qsTr("Back to clients")
                    defaultColor: AmneziaStyle.color.transparent
                    hoveredColor: AmneziaStyle.color.translucentWhite
                    textColor: AmneziaStyle.color.mutedGray
                    borderWidth: 0
                    clickedFunc: function() { usersRadioButton.clicked(); a.contentY = 0 }
                }

                BasicButtonType {
                    objectName: "desktopAddClientButton"
                        enabled: !ExportController.clientsLoading
                    visible: accessTypeSelector.currentIndex === 1
                    text: qsTr("Add client")
                    leftImageSource: "qrc:/images/controls/plus.svg"
                    implicitHeight: 40
                    defaultColor: AmneziaStyle.color.goldenApricot
                    hoveredColor: AmneziaStyle.color.softViolet
                    pressedColor: AmneziaStyle.color.accentGlowViolet
                    textColor: AmneziaStyle.color.paleGray
                    clickedFunc: function() { connectionRadioButton.clicked(); a.contentY = 0 }
                }

                ImageButtonType {
                    objectName: "desktopClientsMenuButton"
                    image: "qrc:/images/controls/more-vertical.svg"
                    imageColor: AmneziaStyle.color.mutedGray
                    implicitWidth: 40
                    implicitHeight: 40
                    onClicked: shareFullAccessDrawer.openTriggered()
                }
            }

            Rectangle {
                id: accessTypeSelector

                property int currentIndex: GC.isDesktop() && root.supportsUserManagement ? 1 : 0
                visible: !GC.isDesktop()

                Layout.topMargin: GC.isDesktop() ? 22 : 32

                implicitWidth: accessTypeSelectorContent.implicitWidth
                implicitHeight: accessTypeSelectorContent.implicitHeight

                color: AmneziaStyle.color.onyxBlack
                radius: GC.isDesktop() ? 8 : 16

                RowLayout {
                    id: accessTypeSelectorContent

                    spacing: 0

                    HorizontalRadioButton {
                        id: connectionRadioButton
                        checked: accessTypeSelector.currentIndex === 0

                        implicitWidth: (a.width - 36) / (root.supportsUserManagement ? 2 : 1)
                        text: qsTr("Connection")

                        onClicked: {
                            accessTypeSelector.currentIndex = 0
                        }

                        Keys.onEnterPressed: this.clicked()
                        Keys.onReturnPressed: this.clicked()
                    }

                    HorizontalRadioButton {
                        id: usersRadioButton
                        checked: accessTypeSelector.currentIndex === 1
                        visible: root.supportsUserManagement

                        implicitWidth: (a.width - 36) / 2
                        text: qsTr("Users")

                        onClicked: {
                            accessTypeSelector.currentIndex = 1
                            ExportController.updateClientManagementModel(ContainersModel.getProcessedContainerIndex(),
                                                                         ServersModel.getProcessedServerCredentials())
                        }

                        Keys.onEnterPressed: this.clicked()
                        Keys.onReturnPressed: this.clicked()
                    }
                }
            }

            RowLayout {
                id: desktopClientFilters
                objectName: "desktopClientFilters"
                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.bottomMargin: 12
                visible: GC.isDesktop() && accessTypeSelector.currentIndex === 1
                spacing: 12

                Rectangle {
                    Layout.preferredWidth: Math.min(260, (desktopClientFilters.width - 164) / 2)
                    Layout.preferredHeight: 44
                    radius: 8
                    color: desktopServerFilterMouse.containsMouse ? AmneziaStyle.color.translucentWhite : "transparent"
                    border.width: 1
                    border.color: AmneziaStyle.color.slateGray
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 12
                        Text { Layout.fillWidth: true; text: serverSelector.text; color: AmneziaStyle.color.paleGray; font.family: "Inter"; font.pixelSize: 13; elide: Text.ElideRight }
                        TintedIconType { Layout.preferredWidth: 14; Layout.preferredHeight: 14; source: "qrc:/images/controls/chevron-down.svg"; tintColor: AmneziaStyle.color.mutedGray; iconWidth: 14; iconHeight: 14 }
                    }
                    MouseArea { id: desktopServerFilterMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: serverSelector.openTriggered() }
                }

                Rectangle {
                    Layout.preferredWidth: Math.min(210, (desktopClientFilters.width - 164) / 2)
                    Layout.preferredHeight: 44
                    radius: 8
                    color: desktopProtocolFilterMouse.containsMouse ? AmneziaStyle.color.translucentWhite : "transparent"
                    border.width: 1
                    border.color: AmneziaStyle.color.slateGray
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 12
                        Text { Layout.fillWidth: true; text: protocolSelector.text; color: AmneziaStyle.color.paleGray; font.family: "Inter"; font.pixelSize: 13; elide: Text.ElideRight }
                        TintedIconType { Layout.preferredWidth: 14; Layout.preferredHeight: 14; source: "qrc:/images/controls/chevron-down.svg"; tintColor: AmneziaStyle.color.mutedGray; iconWidth: 14; iconHeight: 14 }
                    }
                    MouseArea { id: desktopProtocolFilterMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: protocolSelector.openTriggered() }
                }
                BusyIndicator {
                    objectName: "clientsRefreshIndicator"
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    running: ExportController.clientsLoading
                    visible: running
                }
                BasicButtonType {
                    objectName: "clientsRefreshButton"
                    text: qsTr("Refresh")
                    enabled: !ExportController.clientsLoading
                    clickedFunc: function() {
                        ExportController.updateClientManagementModel(ContainersModel.getProcessedContainerIndex(),
                                                                     ServersModel.getProcessedServerCredentials(), true)
                    }
                }
                Item { Layout.fillWidth: true }
            }

            ParagraphTextType {
                Layout.fillWidth: true
                Layout.topMargin: GC.isDesktop() ? 16 : 24
                Layout.bottomMargin: GC.isDesktop() ? 8 : 24

                visible: accessTypeSelector.currentIndex === 0

                text: qsTr("Share VPN access without the ability to manage the server")
                color: AmneziaStyle.color.mutedGray
            }

            TextFieldWithHeaderType {
                id: clientNameTextField
                Layout.fillWidth: true
                Layout.topMargin: GC.isDesktop() ? 10 : 16

                visible: accessTypeSelector.currentIndex === 0

                headerText: qsTr("User name")
                textField.text: qsTr("New client")
                textField.maximumLength: 20

                checkEmptyText: true
            }

            DropDownType {
                id: serverSelector

                signal serverSelectorIndexChanged
                property int currentIndex: -1

                Layout.fillWidth: true
                Layout.topMargin: GC.isDesktop() ? 10 : 16
                visible: !GC.isDesktop() || accessTypeSelector.currentIndex === 0

                drawerHeight: 0.4375
                drawerParent: root

                descriptionText: qsTr("Server")
                headerText: qsTr("Server")

                listView: ListViewWithRadioButtonType {
                    id: serverSelectorListView
                    rootWidth: root.width
                    imageSource: "qrc:/images/controls/check.svg"

                    model: SortFilterProxyModel {
                        id: proxyServersModel
                        sourceModel: ServersModel
                        filters: [
                            ValueFilter {
                                roleName: "hasWriteAccess"
                                value: true
                            },
                            ValueFilter {
                                roleName: "hasInstalledContainers"
                                value: true
                            }
                        ]
                    }

                    clickedFunction: function() {
                        handler()

                        if (serverSelector.currentIndex !== serverSelectorListView.selectedIndex) {
                            serverSelector.currentIndex = serverSelectorListView.selectedIndex
                            serverSelector.serverSelectorIndexChanged()
                        }

                        serverSelector.closeTriggered()
                    }

                    Component.onCompleted: {
                        if (ServersModel.isDefaultServerHasWriteAccess() && ServersModel.getDefaultServerData("hasInstalledContainers")) {
                            serverSelectorListView.selectedIndex = proxyServersModel.mapFromSource(ServersModel.defaultIndex)
                        } else {
                            serverSelectorListView.selectedIndex = 0
                        }

                        serverSelectorListView.positionViewAtIndex(selectedIndex, ListView.Beginning)
                        serverSelectorListView.triggerCurrentItem()
                    }

                    function handler() {
                        serverSelector.text = selectedText
                        ServersModel.processedIndex = proxyServersModel.mapToSource(selectedIndex)
                    }
                }
            }

            DropDownType {
                id: protocolSelector

                signal protocolSelectorTextChanged

                Layout.fillWidth: true
                Layout.topMargin: GC.isDesktop() ? 10 : 16
                visible: !GC.isDesktop() || accessTypeSelector.currentIndex === 0

                drawerHeight: 0.5
                drawerParent: root

                descriptionText: qsTr("Protocol")
                headerText: qsTr("Protocol")

                listView: ListViewWithRadioButtonType {
                    id: protocolSelectorListView

                    rootWidth: root.width
                    imageSource: "qrc:/images/controls/check.svg"

                    model: SortFilterProxyModel {
                        id: proxyContainersModel
                        sourceModel: ContainersModel
                        filters: [
                            ValueFilter {
                                roleName: "isInstalled"
                                value: true
                            },
                            ValueFilter {
                                roleName: "isShareable"
                                value: true
                            }
                        ]
                    }

                    clickedFunction: function() {
                        handler()

                        protocolSelector.closeTriggered()
                    }

                    Connections {
                        target: serverSelector

                        function onServerSelectorIndexChanged() {
                            var defaultContainer = proxyContainersModel.mapFromSource(ServersModel.getProcessedServerData("defaultContainer"))
                            protocolSelectorListView.selectedIndex = defaultContainer
                            protocolSelectorListView.positionViewAtIndex(selectedIndex, ListView.Beginning)
                            protocolSelectorListView.triggerCurrentItem()
                        }
                    }

                    function handler() {
                        if (!proxyContainersModel.count) {
                            root.shareButtonEnabled = false
                            return
                        } else {
                            root.shareButtonEnabled = true
                        }

                        protocolSelector.text = selectedText

                        ContainersModel.setProcessedContainerIndex(proxyContainersModel.mapToSource(selectedIndex))

                        fillConnectionTypeModel()

                        if (accessTypeSelector.currentIndex === 1 && root.supportsUserManagement) {
                            ExportController.updateClientManagementModel(ContainersModel.getProcessedContainerIndex(),
                                                                         ServersModel.getProcessedServerCredentials())
                        }

                        protocolSelector.protocolSelectorTextChanged()
                    }

                    function fillConnectionTypeModel() {
                        root.connectionTypesModel = [amneziaConnectionFormat]

                        var index = proxyContainersModel.mapToSource(selectedIndex)
                        root.supportsUserManagement = ContainerProps.supportsUserManagement(index)

                        if (!root.supportsUserManagement && accessTypeSelector.currentIndex === 1) {
                            accessTypeSelector.currentIndex = 0
                        }

                        if (index === ContainerProps.containerFromString("amnezia-openvpn")) {
                            root.connectionTypesModel.push(openVpnConnectionFormat)
                        } else if (index === ContainerProps.containerFromString("amnezia-wireguard")) {
                            root.connectionTypesModel.push(wireGuardConnectionFormat)
                        } else if (index === ContainerProps.containerFromString("amnezia-awg")) {
                            root.connectionTypesModel.push(awgConnectionFormat)
                        } else if (index === ContainerProps.containerFromString("amnezia-awg2")) {
                            root.connectionTypesModel.push(awgConnectionFormat)
                        } else if (index === ContainerProps.containerFromString("amnezia-shadowsocks")) {
                            root.connectionTypesModel.push(openVpnConnectionFormat)
                            root.connectionTypesModel.push(shadowSocksConnectionFormat)
                        } else if (index === ContainerProps.containerFromString("amnezia-openvpn-cloak")) {
                            root.connectionTypesModel.push(openVpnConnectionFormat)
                            root.connectionTypesModel.push(shadowSocksConnectionFormat)
                            root.connectionTypesModel.push(cloakConnectionFormat)
                        } else if (index === ContainerProps.containerFromString("amnezia-ssxray")) {
                            root.connectionTypesModel.push(shadowSocksConnectionFormat)
                        } else if (index === ContainerProps.containerFromString("amnezia-xray")) {
                            root.connectionTypesModel.push(xrayConnectionFormat)
                        }
                    }
                }
            }

            DropDownType {
                id: exportTypeSelector

                property int currentIndex: 0

                Layout.fillWidth: true
                Layout.topMargin: GC.isDesktop() ? 10 : 16

                drawerHeight: 0.4375
                drawerParent: root

                visible: accessTypeSelector.currentIndex === 0
                enabled: root.connectionTypesModel.length > 1

                descriptionText: qsTr("Connection format")
                headerText: qsTr("Connection format")

                listView: ListViewWithRadioButtonType {
                    id: exportTypeSelectorListView

                    onCurrentIndexChanged: {
                        exportTypeSelector.currentIndex = exportTypeSelectorListView.selectedIndex
                        exportTypeSelector.text = exportTypeSelectorListView.selectedText
                    }

                    onModelChanged: {
                        if (exportTypeSelector.currentIndex >= model.length || exportTypeSelector.currentIndex < 0) {
                            exportTypeSelector.currentIndex = 0
                        }
                        selectedIndex = exportTypeSelector.currentIndex
                        if (model.length > 0 && model[selectedIndex] && model[selectedIndex].name !== undefined) {
                            exportTypeSelectorListView.selectedText = model[selectedIndex].name
                            exportTypeSelector.text = model[selectedIndex].name
                        } else {
                            exportTypeSelectorListView.selectedText = ""
                            exportTypeSelector.text = ""
                        }
                    }

                    rootWidth: root.width

                    imageSource: "qrc:/images/controls/check.svg"

                    model: root.connectionTypesModel
                    currentIndex: 0

                    Connections {
                        target: protocolSelector

                        function onProtocolSelectorTextChanged() {
                            if (exportTypeSelector.currentIndex >= root.connectionTypesModel.length) {
                                exportTypeSelectorListView.selectedIndex = 0
                                exportTypeSelector.currentIndex = 0
                                exportTypeSelector.text = root.connectionTypesModel[0].name
                            }
                        }
                    }

                    clickedFunction: function() {
                        exportTypeSelector.text = exportTypeSelectorListView.selectedText
                        exportTypeSelector.currentIndex = exportTypeSelectorListView.selectedIndex
                        exportTypeSelector.closeTriggered()
                    }
                }
            }

            BasicButtonType {
                id: shareButton

                Layout.fillWidth: true
                Layout.topMargin: GC.isDesktop() ? 18 : 40
                Layout.bottomMargin: GC.isDesktop() ? 18 : 32

                enabled: shareButtonEnabled && !ExportController.clientsLoading
                visible: accessTypeSelector.currentIndex === 0

                text: qsTr("Share")
                leftImageSource: "qrc:/images/controls/share-2.svg"

                clickedFunc: function(){
                    if (clientNameTextField.textField.text !== "") {
                        ExportController.generateConfig(root.connectionTypesModel[exportTypeSelector.currentIndex].type)
                    }
                }
            }

            Header2Type {
                id: usersHeader
                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.bottomMargin: 16

                visible: !GC.isDesktop() && accessTypeSelector.currentIndex === 1 && !root.isSearchBarVisible

                headerText: qsTr("Users")
                actionButtonImage: "qrc:/images/controls/search.svg"
                actionButtonFunction: function() {
                    root.isSearchBarVisible = true
                }
            }

            RowLayout {
                Layout.topMargin: 24
                Layout.bottomMargin: 16
                visible: !GC.isDesktop() && accessTypeSelector.currentIndex === 1 && root.isSearchBarVisible

                TextFieldWithHeaderType {
                    id: searchTextField
                    Layout.fillWidth: true

                    textField.placeholderText: qsTr("Search by name, IP, key, site")

                    Keys.onEscapePressed: {
                        searchTextField.textField.text = ""
                        root.isSearchBarVisible = false
                    }

                    function navigateTo() {
                        if (searchTextField.textField.text === "") {
                            root.isSearchBarVisible = false
                        }
                    }

                    Keys.onTabPressed: { navigateTo() }
                    Keys.onEnterPressed: { navigateTo() }
                    Keys.onReturnPressed: { navigateTo() }
                }

                ImageButtonType {
                    id: closeSearchButton
                    image: "qrc:/images/controls/close.svg"
                    imageColor: AmneziaStyle.color.paleGray

                    function clickedFunc() {
                        searchTextField.textField.text = ""
                        root.isSearchBarVisible = false
                    }

                    onClicked: clickedFunc()
                    Keys.onEnterPressed: clickedFunc()
                    Keys.onReturnPressed: clickedFunc()
                }
            }

            ListView {
                id: clientsListView
                objectName: "clientsListView"
                enabled: !ExportController.clientsLoading
                Layout.fillWidth: true
                Layout.preferredHeight: childrenRect.height

                visible: accessTypeSelector.currentIndex === 1

                function escapeRe(s) { return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') }

                property bool isFocusable: true
                property bool freezeFilter: false

                model: SortFilterProxyModel {
                    id: proxyClientManagementModel
                    sourceModel: ClientManagementModel
                    filters: RegExpFilter {
                        roleName: "searchText"
                        enabled: !clientsListView.freezeFilter
                        pattern: ".*" + clientsListView.escapeRe(searchTextField.textField.text) + ".*"
                        caseSensitivity: Qt.CaseInsensitive
                    }
                }

                clip: true
                interactive: false
                reuseItems: true

                delegate: Item {
                    implicitWidth: clientsListView.width
                    implicitHeight: delegateContent.implicitHeight

                    ColumnLayout {
                        id: delegateContent

                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right

                        anchors.rightMargin: GC.isDesktop() ? 0 : -16
                        anchors.leftMargin: GC.isDesktop() ? 0 : -16

                        LabelWithButtonType {
                            id: clientFocusItem
                            Layout.fillWidth: true
                            visible: !GC.isDesktop()

                            text: clientName
                            textMaximumLineCount: 1
                            descriptionText: root.clientListSummary(isOnline, latestClientIp, clientId, latestDestination)
                            hideDescription: false
                            rightImageSource: "qrc:/images/controls/chevron-right.svg"

                            clickedFunction: function() {
                                clientInfoDrawer.openTriggered()
                            }
                        }

                        Rectangle {
                            objectName: "desktopClientRow"
                            Layout.fillWidth: true
                            Layout.preferredHeight: 68
                            visible: GC.isDesktop()
                            color: desktopClientMouse.containsMouse ? AmneziaStyle.color.translucentWhite : "transparent"

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 10
                                spacing: 14

                                TintedIconType {
                                    Layout.preferredWidth: 24
                                    Layout.preferredHeight: 24
                                    source: "qrc:/images/controls/monitor.svg"
                                    tintColor: AmneziaStyle.color.lightGray
                                    iconWidth: 24
                                    iconHeight: 24
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 3
                                    Text {
                                        Layout.fillWidth: true
                                        text: clientName
                                        color: AmneziaStyle.color.paleGray
                                        font.family: "Inter"
                                        font.pixelSize: 14
                                        font.weight: 600
                                        elide: Text.ElideRight
                                    }
                                    RowLayout {
                                        spacing: 7
                                        Rectangle {
                                            Layout.preferredWidth: 8
                                            Layout.preferredHeight: 8
                                            radius: 4
                                            color: isOnline ? AmneziaStyle.color.vibrantGreen : AmneziaStyle.color.charcoalGray
                                        }
                                        Text {
                                            text: isOnline ? qsTr("Online") : qsTr("Offline")
                                            color: isOnline ? AmneziaStyle.color.vibrantGreen : AmneziaStyle.color.mutedGray
                                            font.family: "Inter"
                                            font.pixelSize: 12
                                        }
                                    }
                                }

                                Text {
                                    Layout.preferredWidth: 180
                                    text: root.hasText(latestActivity) ? latestActivity : "—"
                                    color: AmneziaStyle.color.mutedGray
                                    font.family: "Inter"
                                    font.pixelSize: 12
                                    horizontalAlignment: Text.AlignRight
                                    elide: Text.ElideRight
                                }

                                TintedIconType {
                                    Layout.preferredWidth: 18
                                    Layout.preferredHeight: 18
                                    source: "qrc:/images/controls/more-vertical.svg"
                                    tintColor: AmneziaStyle.color.mutedGray
                                    iconWidth: 18
                                    iconHeight: 18
                                }
                            }

                            MouseArea {
                                id: desktopClientMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: clientInfoDrawer.openTriggered()
                            }
                        }

                        DividerType {}

                        DrawerType2 {
                            id: clientInfoDrawer

                            parent: root

                            width: root.width
                            height: root.height

                            expandedStateContent: ColumnLayout {
                                id: expandedStateContent
                                anchors.top: parent.top
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.topMargin: 16
                                anchors.leftMargin: 16
                                anchors.rightMargin: 16

                                onImplicitHeightChanged: {
                                    clientInfoDrawer.expandedHeight = expandedStateContent.implicitHeight + 32
                                }

                                Header2TextType {
                                    Layout.maximumWidth: parent.width
                                    Layout.bottomMargin: 24

                                    text: clientName
                                    maximumLineCount: 2
                                    wrapMode: Text.Wrap
                                    elide: Qt.ElideRight
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.bottomMargin: 8
                                    spacing: 8

                                    Rectangle {
                                        Layout.preferredWidth: 8
                                        Layout.preferredHeight: 8
                                        radius: 4
                                        color: isOnline ? AmneziaStyle.color.vibrantGreen : AmneziaStyle.color.charcoalGray
                                        Layout.alignment: Qt.AlignVCenter
                                    }

                                    ParagraphTextType {
                                        Layout.fillWidth: true
                                        color: isOnline ? AmneziaStyle.color.vibrantGreen : AmneziaStyle.color.mutedGray
                                        maximumLineCount: 1
                                        elide: Qt.ElideRight
                                        text: isOnline ? qsTr("Online") : qsTr("Offline")
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.bottomMargin: 12
                                    visible: root.hasText(clientId)
                                    spacing: 6

                                    SmallTextType {
                                        Layout.fillWidth: true
                                        color: AmneziaStyle.color.mutedGray
                                        text: qsTr("Key")
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 8

                                        CopyableTextType {
                                            Layout.fillWidth: true
                                            text: clientId
                                            color: AmneziaStyle.color.paleGray
                                            font.pixelSize: 14
                                            font.family: "Inter"
                                            wrapMode: Text.WrapAnywhere
                                        }

                                        ImageButtonType {
                                            Layout.preferredWidth: 40
                                            Layout.preferredHeight: 40
                                            image: "qrc:/images/controls/copy.svg"
                                            imageColor: AmneziaStyle.color.paleGray

                                            onClicked: {
                                                GC.copyToClipBoard(clientId)
                                                PageController.showNotificationMessage(qsTr("Copied"))
                                            }
                                        }
                                    }
                                }

                                ParagraphTextType {
                                    color: AmneziaStyle.color.mutedGray
                                    visible: root.hasText(latestClientIp)
                                    Layout.maximumWidth: parent.width

                                    wrapMode: Text.Wrap

                                    text: qsTr("External IP: %1").arg(latestClientIp)
                                }

                                ParagraphTextType {
                                    color: AmneziaStyle.color.mutedGray
                                    visible: root.hasText(latestActivity)
                                    Layout.maximumWidth: parent.width

                                    maximumLineCount: 2
                                    wrapMode: Text.Wrap
                                    elide: Qt.ElideRight

                                    text: qsTr("Latest activity: %1").arg(latestActivity)
                                }

                                ParagraphTextType {
                                    color: AmneziaStyle.color.mutedGray
                                    visible: root.hasText(latestDestination)
                                    Layout.maximumWidth: parent.width

                                    maximumLineCount: 2
                                    wrapMode: Text.Wrap
                                    elide: Qt.ElideRight

                                    text: qsTr("Last site: %1").arg(latestDestination)
                                }

                                ParagraphTextType {
                                    color: AmneziaStyle.color.mutedGray
                                    visible: creationDate
                                    Layout.maximumWidth: parent.width

                                    maximumLineCount: 2
                                    wrapMode: Text.Wrap
                                    elide: Qt.ElideRight

                                    text: qsTr("Creation date: %1").arg(creationDate)
                                }

                                ParagraphTextType {
                                    color: AmneziaStyle.color.mutedGray
                                    visible: latestHandshake
                                    Layout.maximumWidth: parent.width

                                    maximumLineCount: 2
                                    wrapMode: Text.Wrap
                                    elide: Qt.ElideRight

                                    text: qsTr("Latest handshake: %1").arg(latestHandshake)
                                }

                                ParagraphTextType {
                                    color: AmneziaStyle.color.mutedGray
                                    visible: dataReceived
                                    Layout.maximumWidth: parent.width

                                    maximumLineCount: 2
                                    wrapMode: Text.Wrap
                                    elide: Qt.ElideRight

                                    text: qsTr("Data received: %1").arg(dataReceived)
                                }

                                ParagraphTextType {
                                    color: AmneziaStyle.color.mutedGray
                                    visible: dataSent
                                    Layout.maximumWidth: parent.width

                                    maximumLineCount: 2
                                    wrapMode: Text.Wrap
                                    elide: Qt.ElideRight

                                    text: qsTr("Data sent: %1").arg(dataSent)
                                }

                                ParagraphTextType {
                                    color: AmneziaStyle.color.mutedGray
                                    visible: allowedIps
                                    Layout.maximumWidth: parent.width

                                    wrapMode: Text.Wrap

                                    text: qsTr("Allowed IPs: %1").arg(allowedIps)
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.topMargin: 16
                                    visible: root.hasText(visitHistory)
                                    spacing: 8

                                    SmallTextType {
                                        Layout.fillWidth: true
                                        color: AmneziaStyle.color.mutedGray
                                        text: qsTr("Visit history")
                                    }

                                    Repeater {
                                        model: root.historyRows(visitHistory)

                                        delegate: ParagraphTextType {
                                            Layout.fillWidth: true
                                            color: AmneziaStyle.color.paleGray
                                            font.pixelSize: 13
                                            wrapMode: Text.WrapAnywhere
                                            text: modelData
                                        }
                                    }
                                }

                                ParagraphTextType {
                                    color: AmneziaStyle.color.mutedGray
                                    visible: !root.hasText(visitHistory)
                                    Layout.maximumWidth: parent.width
                                    Layout.topMargin: 16

                                    wrapMode: Text.Wrap

                                    text: qsTr("No visit history yet")
                                }

                                BasicButtonType {
                                    id: renameButton
                                    Layout.fillWidth: true
                                    Layout.topMargin: 24

                                    defaultColor: AmneziaStyle.color.transparent
                                    hoveredColor: AmneziaStyle.color.translucentWhite
                                    pressedColor: AmneziaStyle.color.sheerWhite
                                    disabledColor: AmneziaStyle.color.mutedGray
                                    textColor: AmneziaStyle.color.paleGray
                                    borderWidth: 1

                                    text: qsTr("Rename")

                                    clickedFunc: function() {
                                        clientNameEditDrawer.openTriggered()
                                    }

                                    DrawerType2 {
                                        id: clientNameEditDrawer

                                        parent: root

                                        anchors.fill: parent
                                        expandedHeight: root.height * 0.35

                                        expandedStateContent: ColumnLayout {
                                            anchors.top: parent.top
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.topMargin: 32
                                            anchors.leftMargin: 16
                                            anchors.rightMargin: 16

                                            TextFieldWithHeaderType {
                                                id: clientNameEditor
                                                Layout.fillWidth: true
                                                headerText: qsTr("Client name")
                                                textField.text: clientName
                                                textField.maximumLength: 20
                                                checkEmptyText: true
                                            }

                                            BasicButtonType {
                                                id: saveButton

                                                Layout.fillWidth: true

                                                text: qsTr("Save")

                                                clickedFunc: function() {
                                                    if (clientNameEditor.textField.text === "") {
                                                        return
                                                    }

                                                    if (clientNameEditor.textField.text !== clientName) {
                                                        clientsListView.freezeFilter = true
                                                        if (ExportController.clientsLoading) return
                                                        PageController.showBusyIndicator(true)
                                                        ExportController.renameClient(proxyClientManagementModel.mapToSource(index),
                                                                                      clientNameEditor.textField.text,
                                                                                      ContainersModel.getProcessedContainerIndex(),
                                                                                      ServersModel.getProcessedServerCredentials())
                                                        PageController.showBusyIndicator(false)
                                                        Qt.callLater(function(){ clientsListView.freezeFilter = false })
                                                        clientNameEditDrawer.closeTriggered()
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }

                                BasicButtonType {
                                    id: revokeButton
                                    Layout.fillWidth: true
                                    Layout.topMargin: 8

                                    defaultColor: AmneziaStyle.color.transparent
                                    hoveredColor: AmneziaStyle.color.translucentWhite
                                    pressedColor: AmneziaStyle.color.sheerWhite
                                    disabledColor: AmneziaStyle.color.mutedGray
                                    textColor: AmneziaStyle.color.paleGray
                                    borderWidth: 1

                                    text: qsTr("Revoke")

                                    clickedFunc: function() {
                                        var headerText = qsTr("Revoke the config for a user - %1?").arg(clientName)
                                        var descriptionText = qsTr("The user will no longer be able to connect to your server.")
                                        var yesButtonText = qsTr("Continue")
                                        var noButtonText = qsTr("Cancel")

                                        var yesButtonFunction = function() {
                                            clientInfoDrawer.closeTriggered()
                                            if (ExportController.clientsLoading) return
                                            PageController.showBusyIndicator(true)
                                            ExportController.revokeConfig(proxyClientManagementModel.mapToSource(index),
                                                                          ContainersModel.getProcessedContainerIndex(),
                                                                          ServersModel.getProcessedServerCredentials())
                                        }
                                        var noButtonFunction = function() {
                                        }

                                        showQuestionDrawer(headerText, descriptionText, yesButtonText, noButtonText, yesButtonFunction, noButtonFunction)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

}

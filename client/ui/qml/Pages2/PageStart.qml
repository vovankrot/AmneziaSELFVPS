import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes

import PageEnum 1.0
import Style 1.0

import "./"
import "../Controls2"
import "../Controls2/TextTypes"
import "../Config"
import "../Components"

PageType {
    id: root

    property bool isControlsDisabled: false
    property bool isTabBarDisabled: false
    readonly property bool useSideNavigation: GC.isDesktop() && tabBar.visible
    readonly property int sideNavigationWidth: useSideNavigation ? 246 : 0

    function switchRootTab(page, index) {
        tabBarStackView.goToTabBarPage(page)
        tabBar.currentIndex = index
    }

    // Tracks the currently shown page so the side rail can highlight
    // destinations that are pushed (not root tabs), e.g. the Servers list.
    readonly property string currentPagePath: tabBarStackView.currentItem ? tabBarStackView.currentItem.objectName : ""

    // Navigate to the Servers list as a pushed page on top of the Home root
    // tab. This keeps the page's own back button (PageController.closePage)
    // working naturally instead of turning it into a window-hide action.
    function goToServersList() {
        root.switchRootTab(PageEnum.PageHome, 0)
        PageController.goToPage(PageEnum.PageSettingsServersList)
    }

    Connections {
        objectName: "pageControllerConnection"

        target: PageController

        function onGoToPageHome() {
            if (PageController.isStartPageVisible()) {
                tabBar.visible = false
                tabBarStackView.goToTabBarPage(PageEnum.PageSetupWizardStart)
            } else {
                tabBar.visible = true
                tabBar.setCurrentIndex(0)
                tabBarStackView.goToTabBarPage(PageEnum.PageHome)
            }
        }

        function onGoToPageSettings() {
            tabBar.setCurrentIndex(2)
            tabBarStackView.goToTabBarPage(PageEnum.PageSettings)
        }

        function onGoToPageViewConfig() {
            var pagePath = PageController.getPagePath(PageEnum.PageSetupWizardViewConfig)
            tabBarStackView.push(pagePath, { "objectName" : pagePath }, StackView.PushTransition)
        }

        function onGoToShareConnectionPage(headerText, configContentHeaderText, configCaption, configExtension, configFileName) {
            var pagePath = PageController.getPagePath(PageEnum.PageShareConnection)
            tabBarStackView.push(pagePath,
                                 { "objectName" : pagePath,
                                     "headerText" : headerText,
                                     "configContentHeaderText" : configContentHeaderText,
                                     "configCaption" : configCaption,
                                     "configExtension" : configExtension,
                                     "configFileName" : configFileName
                                 },
                                 StackView.PushTransition)
        }

        function onDisableControls(disabled) {
            isControlsDisabled = disabled
        }

        function onDisableTabBar(disabled) {
            isTabBarDisabled = disabled
        }

        function onClosePage() {
            if (tabBarStackView.depth <= 1) {
                PageController.hideWindow()
                return
            }
            tabBarStackView.pop()
        }

        function onGoToPage(page, slide) {
            var pagePath = PageController.getPagePath(page)

            if (slide) {
                tabBarStackView.push(pagePath, { "objectName" : pagePath }, StackView.PushTransition)
            } else {
                tabBarStackView.push(pagePath, { "objectName" : pagePath }, StackView.Immediate)
            }
        }

        function onGoToStartPage() {
            while (tabBarStackView.depth > 1) {
                tabBarStackView.pop()
            }
        }

        function onEscapePressed() {
            if (root.isControlsDisabled || root.isTabBarDisabled) {
                return
            }

            var pageName = tabBarStackView.currentItem.objectName
            if ((pageName === PageController.getPagePath(PageEnum.PageShare)) ||
                    (pageName === PageController.getPagePath(PageEnum.PageSettings)) ||
                    (pageName === PageController.getPagePath(PageEnum.PageSetupWizardConfigSource))) {
                PageController.goToPageHome()
            } else {
                PageController.closePage()
            }
        }
    }

    Connections {
        objectName: "installControllerConnections"

        target: InstallController

        function onInstallationErrorOccurred(error) {
            PageController.showBusyIndicator(false)

            if (error === 204) {
                PageController.showNotificationMessage(qsTr("Installation canceled by user"))
            } else {
                PageController.showErrorMessage(error)
            }

            var needCloseCurrentPage = false
            var currentPageName = tabBarStackView.currentItem.objectName

            if (currentPageName === PageController.getPagePath(PageEnum.PageSetupWizardInstalling)) {
                needCloseCurrentPage = true
            } else if (currentPageName === PageController.getPagePath(PageEnum.PageDeinstalling)) {
                needCloseCurrentPage = true
            }
            if (needCloseCurrentPage) {
                PageController.closePage()
            }
        }

        function onWrongInstallationUser(message) {
            onInstallationErrorOccurred(message)
        }

        function onUpdateContainerFinished(message) {
            PageController.showNotificationMessage(message)
            PageController.closePage()
        }

        function onCachedProfileCleared(message) {
            PageController.showNotificationMessage(message)
        }

        function onApiConfigRemoved(message) {
            PageController.showNotificationMessage(message)
        }

        function onRemoveProcessedServerFinished(finishedMessage) {
            if (!ServersModel.getServersCount()) {
                PageController.goToPageHome()
            } else {
                PageController.goToStartPage()
                PageController.goToPage(PageEnum.PageSettingsServersList)
            }
            PageController.showNotificationMessage(finishedMessage)
        }

        function onNoInstalledContainers() {
            PageController.setTriggeredByConnectButton(true)

            ServersModel.processedIndex = ServersModel.getDefaultServerIndex()
            InstallController.setShouldCreateServer(false)
            PageController.goToPage(PageEnum.PageSetupWizardEasy)
        }
    }

    Connections {
        objectName: "connectionControllerConnections"

        target: ConnectionController

        function onReconnectWithUpdatedContainer(message) {
            PageController.showNotificationMessage(message)
            PageController.closePage()
        }

        function onAutoFailoverTriggered(message) {
            PageController.showNotificationMessage(message)
        }
    }

    Connections {
        objectName: "importControllerConnections"

        target: ImportController

        function onImportErrorOccurred(error, goToPageHome) {
            PageController.showErrorMessage(error)
        }

        function onRestoreAppConfig(data) {
            PageController.showBusyIndicator(true)
            SettingsController.restoreAppConfigFromData(data)
            PageController.showBusyIndicator(false)
        }
    }

    Connections {
        objectName: "settingsControllerConnections"

        target: SettingsController

        function onLoggingDisableByWatcher() {
            PageController.showNotificationMessage(qsTr("Logging was disabled after 14 days, log files were deleted"))
        }

        function onRestoreBackupFinished() {
            PageController.showNotificationMessage(qsTr("Settings restored from backup file"))
            PageController.goToPageHome()
        }

        function onLoggingStateChanged() {
            if (SettingsController.isLoggingEnabled) {
                var message = qsTr("Logging is enabled. Note that logs will be automatically" +
                                   "disabled after 14 days, and all log files will be deleted.")
                PageController.showNotificationMessage(message)
            }
        }
    }

    Connections {
        target: ApiSettingsController

        function onErrorOccurred(error) {
            PageController.showErrorMessage(error)
        }
    }

    Connections {
        target: ApiConfigsController

        function onInstallServerFromApiFinished(message, preferredDefaultIndex) {
            if (!ConnectionController.isConnected) {
                if (preferredDefaultIndex !== undefined && preferredDefaultIndex >= 0) {
                    ServersModel.setDefaultServerIndex(preferredDefaultIndex)
                } else {
                    ServersModel.setDefaultServerIndex(ServersModel.getServersCount() - 1)
                }
                ServersModel.processedIndex = ServersModel.defaultIndex
            }

            PageController.goToPageHome()
            PageController.showNotificationMessage(message)
        }

        function onChangeApiCountryFinished(message) {
            PageController.goToPageHome()
            PageController.showNotificationMessage(message)
        }

        function onReloadServerFromApiFinished(message) {
            PageController.goToPageHome()
            PageController.showNotificationMessage(message)
        }
    }

    component SideNavigationButton: Rectangle {
        id: navigationButton

        property string text
        property string image
        property bool selected: false
        property bool navigationEnabled: true
        property var clickedFunc: function() {}

        Layout.fillWidth: true
        Layout.preferredHeight: 44
        radius: 7
        color: selected ? AmneziaStyle.color.translucentRichBrown : "transparent"
        border.width: selected ? 1 : 0
        border.color: selected ? AmneziaStyle.color.goldenApricot : AmneziaStyle.color.slateGray
        opacity: navigationEnabled ? 1.0 : 0.44

        // Primary-accent indicator bar on the active item.
        Rectangle {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: 3
            height: 22
            radius: 1.5
            color: AmneziaStyle.color.goldenApricot
            visible: navigationButton.selected
        }

        MouseArea {
            anchors.fill: parent
            enabled: navigationButton.navigationEnabled && root.useSideNavigation && !root.isControlsDisabled && !root.isTabBarDisabled
            cursorShape: Qt.PointingHandCursor

            onClicked: navigationButton.clickedFunc()
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            spacing: 12

            Image {
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
                source: navigationButton.image
                opacity: navigationButton.selected ? 1.0 : 0.78
            }

            Text {
                Layout.fillWidth: true
                text: navigationButton.text
                color: navigationButton.selected ? AmneziaStyle.color.goldenApricot : AmneziaStyle.color.mutedGray
                font.pixelSize: 14
                font.weight: navigationButton.selected ? 600 : 500
                elide: Text.ElideRight
            }
        }
    }

    Rectangle {
        id: sideNavigation

        visible: root.useSideNavigation
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        width: root.sideNavigationWidth
        color: AmneziaStyle.color.accentGradientBottom

        gradient: Gradient {
            GradientStop { position: 0.0; color: AmneziaStyle.color.accentGradientTop }
            GradientStop { position: 0.56; color: AmneziaStyle.color.accentGradientMid }
            GradientStop { position: 1.0; color: AmneziaStyle.color.accentGradientBottom }
        }

        Rectangle {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 1
            color: AmneziaStyle.color.slateGray
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.topMargin: 30 + SettingsController.safeAreaTopMargin
            anchors.leftMargin: 24
            anchors.rightMargin: 24
            anchors.bottomMargin: 24
            spacing: 0

            Rectangle {
                Layout.preferredWidth: 150
                Layout.preferredHeight: 104
                Layout.bottomMargin: 18
                radius: 18
                color: AmneziaStyle.color.onyxBlack
                border.width: 1
                border.color: AmneziaStyle.color.slateGray

                Image {
                    anchors.centerIn: parent
                    width: 122
                    height: 82
                    fillMode: Image.PreserveAspectFit
                    source: "qrc:/images/amneziaBigLogo.png"
                }
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("Amnezia")
                color: AmneziaStyle.color.paleGray
                font.pixelSize: 30
                font.weight: 300
            }

            Text {
                Layout.fillWidth: true
                Layout.topMargin: -2
                text: qsTr("VPN")
                color: AmneziaStyle.color.paleGray
                font.pixelSize: 30
                font.weight: 650
            }

            Rectangle {
                Layout.preferredWidth: versionColumn.implicitWidth + 24
                Layout.preferredHeight: versionColumn.implicitHeight + 12
                Layout.topMargin: 16
                Layout.bottomMargin: 28
                radius: 14
                color: AmneziaStyle.color.benefitsPanelBackground

                ColumnLayout {
                    id: versionColumn
                    anchors.centerIn: parent
                    spacing: 1

                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: "v" + SettingsController.getAppVersion()
                        color: AmneziaStyle.color.mutedGray
                        font.family: "Inter"
                        font.pixelSize: 11
                        font.weight: 600
                    }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: "coopilot version"
                        color: AmneziaStyle.color.softViolet
                        font.family: "Inter"
                        font.pixelSize: 9
                        font.weight: 600
                        font.italic: true
                    }
                }
            }

            SideNavigationButton {
                text: qsTr("Home")
                image: "qrc:/images/controls/home.svg"
                // Highlight Home only when its root tab is active and no
                // overlay page (e.g. the Servers list) is pushed on top.
                selected: tabBar.currentIndex === 0
                          && root.currentPagePath === PageController.getPagePath(PageEnum.PageHome)
                clickedFunc: function() {
                    root.switchRootTab(PageEnum.PageHome, 0)
                    ServersModel.processedIndex = ServersModel.defaultIndex
                }
            }

            SideNavigationButton {
                Layout.topMargin: 8
                text: qsTr("Servers")
                image: "qrc:/images/controls/server.svg"
                selected: root.currentPagePath === PageController.getPagePath(PageEnum.PageSettingsServersList)
                clickedFunc: function() {
                    root.goToServersList()
                }
            }

            SideNavigationButton {
                Layout.topMargin: 8
                text: qsTr("Clients")
                image: "qrc:/images/controls/share-2.svg"
                // Clients page can only create configs on a FULL-ACCESS server
                // (added via SSH), not on an imported read-only config — so hide
                // it when no such server exists (otherwise the page is empty).
                visible: !SettingsController.isOnTv() && ServersModel.hasServerWithWriteAccess()
                navigationEnabled: visible
                selected: tabBar.currentIndex === 1
                clickedFunc: function() {
                    root.switchRootTab(PageEnum.PageShare, 1)
                }
            }

            SideNavigationButton {
                Layout.topMargin: 8
                text: qsTr("Settings")
                image: (ServersModel.hasServersFromGatewayApi && NewsModel.hasUnread && SettingsController.isNewsNotificationsEnabled()) ? "qrc:/images/controls/settings-news.svg" : "qrc:/images/controls/settings.svg"
                // Don't light Settings when the Servers list (a settings sub-page)
                // is on top — that's owned by the Servers item. Avoids two items
                // highlighting at once. by vovankrot
                selected: tabBar.currentIndex === 2
                          && root.currentPagePath !== PageController.getPagePath(PageEnum.PageSettingsServersList)
                clickedFunc: function() {
                    root.switchRootTab(PageEnum.PageSettings, 2)
                }
            }

            SideNavigationButton {
                Layout.topMargin: 8
                text: qsTr("Add server")
                image: "qrc:/images/controls/plus.svg"
                selected: tabBar.currentIndex === 3
                clickedFunc: function() {
                    root.switchRootTab(PageEnum.PageSetupWizardConfigSource, 3)
                }
            }

            Item {
                Layout.fillHeight: true
            }

            // Connection status — small shield, shown on every page via the rail.
            // by vovankrot
            RowLayout {
                Layout.leftMargin: 16
                Layout.bottomMargin: 18
                Layout.topMargin: 8
                spacing: 8
                visible: root.useSideNavigation

                TintedIconType {
                    Layout.alignment: Qt.AlignVCenter
                    Layout.preferredWidth: 16
                    Layout.preferredHeight: 16
                    source: "qrc:/images/controls/shield.svg"
                    tintColor: ConnectionController.isConnected
                               ? AmneziaStyle.color.vibrantGreen
                               : (ConnectionController.isConnectionInProgress
                                  ? AmneziaStyle.color.goldenApricot
                                  : AmneziaStyle.color.charcoalGray)
                    iconWidth: 16
                    iconHeight: 16
                }

                Text {
                    text: ConnectionController.connectionStateText
                    color: ConnectionController.isConnected
                           ? AmneziaStyle.color.vibrantGreen
                           : AmneziaStyle.color.mutedGray
                    font.family: "Inter"
                    font.pixelSize: 12
                    font.weight: 600
                }
            }
        }
    }

    StackViewType {
        id: tabBarStackView
        objectName: "tabBarStackView"

        anchors.top: parent.top
        anchors.right: parent.right
        anchors.left: root.useSideNavigation ? sideNavigation.right : parent.left
        anchors.bottom: root.useSideNavigation ? parent.bottom : tabBar.top

        enabled: !root.isControlsDisabled

        function goToTabBarPage(page) {
            var pagePath = PageController.getPagePath(page)
            tabBarStackView.clear(StackView.Immediate)
            tabBarStackView.replace(pagePath, { "objectName" : pagePath }, StackView.Immediate)
        }

        Component.onCompleted: {
            var pagePath
            if (PageController.isStartPageVisible()) {
                tabBar.visible = false
                pagePath = PageController.getPagePath(PageEnum.PageSetupWizardStart)
            } else {
                tabBar.visible = true
                pagePath = PageController.getPagePath(PageEnum.PageHome)
                ServersModel.processedIndex = ServersModel.defaultIndex
            }

            tabBarStackView.push(pagePath, { "objectName" : pagePath })
        }

        Keys.onPressed: function(event) {
            switch (event.key) {
            case Qt.Key_Tab:
            case Qt.Key_Down:
            case Qt.Key_Right:
                FocusController.nextKeyTabItem()
                break
            case Qt.Key_Backtab:
            case Qt.Key_Up:
            case Qt.Key_Left:
                FocusController.previousKeyTabItem()
                break
            default:
                PageController.keyPressEvent(event.key)
                event.accepted = true
            }
        }
    }

    TabBar {
        id: tabBar
        objectName: "tabBar"

        anchors.right: parent.right
        anchors.left: parent.left
        anchors.bottom: parent.bottom

        // Also adjust TabBar position when keyboard appears (Android 14+ workaround)
        anchors.bottomMargin: SettingsController.imeHeight

        topPadding: 8
        bottomPadding: 8 + SettingsController.safeAreaBottomMargin
        leftPadding: 96
        rightPadding: 96

        height: visible && !root.useSideNavigation ? homeTabButton.implicitHeight + tabBar.topPadding + tabBar.bottomPadding : 0
        opacity: root.useSideNavigation ? 0 : 1

        enabled: !root.useSideNavigation && !root.isControlsDisabled && !root.isTabBarDisabled

        background: Shape {
            objectName: "backgroundShape"

            width: parent.width
            height: parent.height

            ShapePath {
                startX: 0
                startY: 0

                PathLine { x: width; y: 0 }
                PathLine { x: width; y: tabBar.height - 1 }
                PathLine { x: 0; y: tabBar.height - 1 }
                PathLine { x: 0; y: 0 }

                strokeWidth: 1
                strokeColor: AmneziaStyle.color.slateGray
                fillColor: AmneziaStyle.color.onyxBlack
            }
        }

        TabImageButtonType {
            id: homeTabButton
            objectName: "homeTabButton"

            isSelected: tabBar.currentIndex === 0
            image: "qrc:/images/controls/home.svg"
            clickedFunc: function () {
                tabBarStackView.goToTabBarPage(PageEnum.PageHome)
                ServersModel.processedIndex = ServersModel.defaultIndex
                tabBar.currentIndex = 0
            }
        }

        TabImageButtonType {
            id: shareTabButton
            objectName: "shareTabButton"

            Connections {
                target: ServersModel

                function onModelReset() {
                    if (!SettingsController.isOnTv()) {
                        var hasServerWithWriteAccess = ServersModel.hasServerWithWriteAccess()
                        shareTabButton.visible = hasServerWithWriteAccess
                        shareTabButton.width = hasServerWithWriteAccess ? undefined : 0
                    }
                }
            }

            visible: !SettingsController.isOnTv() && ServersModel.hasServerWithWriteAccess()
            width: !SettingsController.isOnTv() && ServersModel.hasServerWithWriteAccess() ? undefined : 0

            isSelected: tabBar.currentIndex === 1
            image: "qrc:/images/controls/share-2.svg"
            clickedFunc: function () {
                tabBarStackView.goToTabBarPage(PageEnum.PageShare)
                tabBar.currentIndex = 1
            }
        }

        TabImageButtonType {
            id: settingsTabButton
            objectName: "settingsTabButton"

            isSelected: tabBar.currentIndex === 2
            image: (ServersModel.hasServersFromGatewayApi && NewsModel.hasUnread && SettingsController.isNewsNotificationsEnabled()) ? "qrc:/images/controls/settings-news.svg" : "qrc:/images/controls/settings.svg"
            Binding {
                target: settingsTabButton
                property: "defaultColor"
                value: "transparent"
                when: (ServersModel.hasServersFromGatewayApi && NewsModel.hasUnread)
            }
            clickedFunc: function () {
                tabBarStackView.goToTabBarPage(PageEnum.PageSettings)
                tabBar.currentIndex = 2
            }
        }

        TabImageButtonType {
            id: plusTabButton
            objectName: "plusTabButton"

            isSelected: tabBar.currentIndex === 3
            image: "qrc:/images/controls/plus.svg"
            clickedFunc: function () {
                tabBarStackView.goToTabBarPage(PageEnum.PageSetupWizardConfigSource)
                tabBar.currentIndex = 3
            }
        }
    }
}

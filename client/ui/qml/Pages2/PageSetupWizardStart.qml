import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import PageEnum 1.0
import Style 1.0

import "./"
import "../Controls2"
import "../Config"
import "../Controls2/TextTypes"
import "../Components"

PageType {
    id: root
    enableTimer: (SettingsController.isOnTv()) ? false : true

    readonly property string termsUrl: "https://amnezia.org/"

    // One onboarding feature column (icon + title + caption). by vovankrot
    component FeatureItem: ColumnLayout {
        id: featureRoot

        property url iconSource
        property color iconColor: AmneziaStyle.color.goldenApricot
        property string title
        property string caption

        Layout.fillWidth: true
        Layout.preferredWidth: 1
        Layout.alignment: Qt.AlignTop
        spacing: 8

        TintedIconType {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 30
            Layout.preferredHeight: 30
            source: featureRoot.iconSource
            tintColor: featureRoot.iconColor
            iconWidth: 30
            iconHeight: 30
        }

        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: featureRoot.title
            color: AmneziaStyle.color.paleGray
            font.family: "Inter"
            font.pixelSize: 15
            font.weight: 600
        }

        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: featureRoot.caption
            color: AmneziaStyle.color.mutedGray
            font.family: "Inter"
            font.pixelSize: 12
        }
    }

    ColumnLayout {
        id: content

        anchors.fill: parent
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        anchors.topMargin: SettingsController.safeAreaTopMargin
        anchors.bottomMargin: 24 + SettingsController.safeAreaBottomMargin
        spacing: 0

        Item { Layout.fillHeight: true; Layout.minimumHeight: 8 }

        Image {
            Layout.alignment: Qt.AlignHCenter
            source: "qrc:/images/amneziaBigLogo.png"
            Layout.preferredWidth: 280
            Layout.preferredHeight: Math.min(190, root.height * 0.28)
            fillMode: Image.PreserveAspectFit
        }

        Text {
            Layout.fillWidth: true
            Layout.topMargin: 22
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("Your secure connection")
            color: AmneziaStyle.color.paleGray
            font.family: "Inter"
            font.pixelSize: 24
            font.weight: 600
        }

        Text {
            Layout.alignment: Qt.AlignHCenter
            Layout.fillWidth: true
            Layout.maximumWidth: 460
            Layout.topMargin: 10
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: qsTr("Protect your privacy, bypass restrictions, and stay connected anywhere in the world.")
            color: AmneziaStyle.color.mutedGray
            font.family: "Inter"
            font.pixelSize: 14
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            Layout.fillWidth: true
            Layout.maximumWidth: 520
            Layout.topMargin: 30
            spacing: 12

            FeatureItem {
                iconSource: "qrc:/images/controls/shield.svg"
                iconColor: AmneziaStyle.color.goldenApricot
                title: qsTr("Security")
                caption: qsTr("Reliable encryption of your data")
            }

            FeatureItem {
                iconSource: "qrc:/images/controls/globe-2.svg"
                iconColor: AmneziaStyle.color.goldenApricot
                title: qsTr("Freedom")
                caption: qsTr("Unrestricted content access")
            }

            FeatureItem {
                iconSource: "qrc:/images/controls/gauge.svg"
                iconColor: AmneziaStyle.color.burntOrange
                title: qsTr("Speed")
                caption: qsTr("Fast and stable connection")
            }
        }

        Item { Layout.fillHeight: true; Layout.minimumHeight: 16 }

        BasicButtonType {
            id: startButton

            Layout.alignment: Qt.AlignHCenter
            Layout.fillWidth: true
            Layout.maximumWidth: 560
            implicitHeight: 56

            defaultColor: AmneziaStyle.color.goldenApricot
            hoveredColor: AmneziaStyle.color.softViolet
            pressedColor: AmneziaStyle.color.goldenApricot
            textColor: AmneziaStyle.color.paleGray

            text: qsTr("Let's get started")
            rightImageSource: "qrc:/images/controls/arrow-right.svg"

            clickedFunc: function() {
                PageController.goToPage(PageEnum.PageSetupWizardConfigSource)
            }
        }

        Text {
            Layout.alignment: Qt.AlignHCenter
            Layout.fillWidth: true
            Layout.maximumWidth: 560
            Layout.topMargin: 14
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            textFormat: Text.RichText
            color: AmneziaStyle.color.mutedGray
            linkColor: AmneziaStyle.color.softViolet
            font.family: "Inter"
            font.pixelSize: 12
            text: qsTr("By tapping the button, you accept the %1")
                  .arg("<a href=\"" + root.termsUrl + "\">" + qsTr("Terms of Use") + "</a>")
            onLinkActivated: function(link) { Qt.openUrlExternally(link) }
        }
    }

    Timer {
        interval: 250
        running: SettingsController.isOnTv()
        repeat: true
        onTriggered: {
            startButton.forceActiveFocus()
            if (startButton.activeFocus) {
                running = false
            }
        }
    }

    onVisibleChanged: {
        if (visible && SettingsController.isOnTv()) {
            startButton.forceActiveFocus()
        }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Style 1.0

import "../Config"

Item {
    id: root

    property StackView stackView: StackView.view
    property bool enableTimer: true
    property bool pageBackgroundVisible: false

    Rectangle {
        anchors.fill: parent
        visible: root.pageBackgroundVisible
        color: AmneziaStyle.color.accentGradientBottom

        gradient: Gradient {
            GradientStop { position: 0.0; color: AmneziaStyle.color.accentGradientTop }
            GradientStop { position: 0.48; color: AmneziaStyle.color.accentGradientMid }
            GradientStop { position: 1.0; color: AmneziaStyle.color.accentGradientBottom }
        }
    }

    onVisibleChanged: {
        if (visible && enableTimer) {
            timer.start()
        }
    }

    // Set a timer to set focus after a short delay
    Timer {
        id: timer
        interval: 200 // Milliseconds
        onTriggered: {
            FocusController.resetRootObject()
            FocusController.setFocusOnDefaultItem()
        }
        repeat: false // Stop the timer after one trigger
        running: enableTimer // Start the timer
    }
}

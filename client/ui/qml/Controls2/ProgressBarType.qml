import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Style 1.0

ProgressBar {
    id: root

    implicitHeight: 8

    background: Rectangle {
        radius: height / 2
        color: AmneziaStyle.color.accentTrackLavender
        opacity: 0.32
    }

    contentItem: Item {
        Rectangle {
            width: root.visualPosition * parent.width
            height: parent.height
            radius: height / 2
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: AmneziaStyle.color.softViolet }
                GradientStop { position: 1.0; color: AmneziaStyle.color.accentGlowMagenta }
            }
        }
    }
}

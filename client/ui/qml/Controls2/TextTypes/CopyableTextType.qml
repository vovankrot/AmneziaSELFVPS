import QtQuick

import "../../Config"

Text {
    id: root

    property bool textSelectionEnabled: false
    property bool copyOnSecondaryClick: GC.isDesktop() && text !== ""

    function decodeHtmlEntities(value) {
        return value
                .replace(/&nbsp;/g, " ")
                .replace(/&amp;/g, "&")
                .replace(/&lt;/g, "<")
                .replace(/&gt;/g, ">")
                .replace(/&quot;/g, '"')
                .replace(/&#39;/g, "'")
    }

    function plainTextForCopy(value) {
        return decodeHtmlEntities(value
                .replace(/<br\s*\/?>/gi, "\n")
                .replace(/<\/p>/gi, "\n")
                .replace(/<[^>]+>/g, ""))
    }

    function textToCopy() {
        if (typeof root.selectedText === "string" && root.selectedText.length > 0) {
            return root.selectedText
        }

        var sourceText = typeof root.text === "string" ? root.text : String(root.text || "")
        if (sourceText === "") {
            return ""
        }

        if (root.textFormat === Text.RichText || root.textFormat === Text.StyledText) {
            return plainTextForCopy(sourceText)
        }

        return sourceText
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        enabled: root.copyOnSecondaryClick
        hoverEnabled: false

        onClicked: function(mouse) {
            var value = root.textToCopy()
            if (value !== "") {
                GC.copyToClipBoard(value)
            }
            mouse.accepted = true
        }
    }
}
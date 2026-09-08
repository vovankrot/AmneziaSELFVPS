import QtQuick
import "../../Config"

import Style 1.0

CopyableTextType {
    lineHeight: (GC.isDesktop() ? 28 : 38) + LanguageModel.getLineHeightAppend()
    lineHeightMode: Text.FixedHeight

    color: AmneziaStyle.color.paleGray
    font.pixelSize: GC.isDesktop() ? 22 : 32
    font.weight: GC.isDesktop() ? 600 : 700
    font.family: "Inter"
    font.letterSpacing: GC.isDesktop() ? -0.2 : -1.0

    wrapMode: Text.WordWrap
}

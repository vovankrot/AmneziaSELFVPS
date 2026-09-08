import QtQuick
import "../../Config"

import Style 1.0

CopyableTextType {
    lineHeight: (GC.isDesktop() ? 22 : 30) + LanguageModel.getLineHeightAppend()
    lineHeightMode: Text.FixedHeight

    color: AmneziaStyle.color.paleGray
    font.pixelSize: GC.isDesktop() ? 17 : 25
    font.weight: GC.isDesktop() ? 600 : 700
    font.family: "Inter"

    wrapMode: Text.WordWrap
}

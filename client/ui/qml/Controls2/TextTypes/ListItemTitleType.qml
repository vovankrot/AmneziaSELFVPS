import QtQuick
import "../../Config"

import Style 1.0

CopyableTextType {
    lineHeight: (GC.isDesktop() ? 18 : 21.6) + LanguageModel.getLineHeightAppend()
    lineHeightMode: Text.FixedHeight

    color: AmneziaStyle.color.paleGray
    font.pixelSize: GC.isDesktop() ? 14 : 18
    font.weight: GC.isDesktop() ? 500 : 400
    font.family: "Inter"

    wrapMode: Text.Wrap
}

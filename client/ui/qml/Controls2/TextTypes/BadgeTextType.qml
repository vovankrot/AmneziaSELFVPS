import QtQuick

import Style 1.0

CopyableTextType {
    lineHeight: 10 + LanguageModel.getLineHeightAppend()
    lineHeightMode: Text.FixedHeight

    color: AmneziaStyle.color.midnightBlack
    font.pixelSize: 11
    font.weight: Font.Medium
    font.family: "Inter"

    wrapMode: Text.NoWrap
}

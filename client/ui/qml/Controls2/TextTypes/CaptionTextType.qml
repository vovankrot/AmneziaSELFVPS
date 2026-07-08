import QtQuick

import Style 1.0

CopyableTextType {
    lineHeight: 16 + LanguageModel.getLineHeightAppend()
    lineHeightMode: Text.FixedHeight

    color: AmneziaStyle.color.midnightBlack
    font.pixelSize: 13
    font.weight: 400
    font.family: "Inter"
    font.letterSpacing: 0.02

    wrapMode: Text.Wrap
}

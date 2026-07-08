import QtQuick

import Style 1.0

CopyableTextType {
    lineHeight: 38 + LanguageModel.getLineHeightAppend()
    lineHeightMode: Text.FixedHeight

    color: AmneziaStyle.color.paleGray
    font.pixelSize: 32
    font.weight: 700
    font.family: "Inter"
    font.letterSpacing: -1.0

    wrapMode: Text.WordWrap
}


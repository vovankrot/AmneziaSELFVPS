import QtQuick

import Style 1.0

CopyableTextType {
    lineHeight: 30 + LanguageModel.getLineHeightAppend()
    lineHeightMode: Text.FixedHeight

    color: AmneziaStyle.color.paleGray
    font.pixelSize: 25
    font.weight: 700
    font.family: "Inter"

    wrapMode: Text.WordWrap
}

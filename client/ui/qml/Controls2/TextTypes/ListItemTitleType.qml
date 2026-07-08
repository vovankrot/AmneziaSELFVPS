import QtQuick

import Style 1.0

CopyableTextType {
    lineHeight: 21.6 + LanguageModel.getLineHeightAppend()
    lineHeightMode: Text.FixedHeight

    color: AmneziaStyle.color.paleGray
    font.pixelSize: 18
    font.weight: 400
    font.family: "Inter"

    wrapMode: Text.Wrap
}

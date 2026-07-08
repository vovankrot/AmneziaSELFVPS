import QtQuick
import Style 1.0

CopyableTextType {
    lineHeight: 24 + LanguageModel.getLineHeightAppend()
    lineHeightMode: Text.FixedHeight

    color: AmneziaStyle.color.paleGray
    font.pixelSize: 16
    font.weight: 400
    font.family: "Inter"

    wrapMode: Text.WordWrap
}

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
        id: updateDialog
        objectName: "selfVpsUpdateDialog"
        anchors.centerIn: parent
        width: 520
        modal: true
        title: qsTr("Обновление SELFVPS")
        closePolicy: Popup.CloseOnEscape
        contentItem: ColumnLayout {
            Label {
                Layout.fillWidth: true
                text: typeof AppUpdater !== "undefined" ? AppUpdater.status : ""
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 160
                TextArea {
                    readOnly: true
                    text: typeof AppUpdater !== "undefined" ? AppUpdater.releaseNotes : ""
                    wrapMode: Text.Wrap
                    textFormat: TextEdit.PlainText
                }
            }
            ProgressBar {
                Layout.fillWidth: true
                from: 0; to: 100
                value: typeof AppUpdater !== "undefined" ? AppUpdater.progress : 0
                visible: typeof AppUpdater !== "undefined" && AppUpdater.busy
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Установка отключит VPN и закроет приложение. Настройки сохранятся. Windows запросит права администратора.")
                wrapMode: Text.Wrap
            }
            RowLayout {
                Button {
                    objectName: "selfVpsUpdatePrimary"
                    text: typeof AppUpdater !== "undefined" && AppUpdater.ready ? qsTr("Установить обновление") : qsTr("Скачать")
                    enabled: typeof AppUpdater !== "undefined" && !AppUpdater.busy && AppUpdater.availableVersion !== ""
                    onClicked: {
                        if (AppUpdater.ready) AppUpdater.install()
                        else AppUpdater.download()
                    }
                }
                Button {
                    text: typeof AppUpdater !== "undefined" && AppUpdater.busy ? qsTr("Отменить") : qsTr("Позже")
                    onClicked: {
                        if (AppUpdater.busy) AppUpdater.cancel()
                        updateDialog.close()
                    }
                }
                Button { text: qsTr("Релиз"); onClicked: AppUpdater.openRelease() }
            }
        }
    }

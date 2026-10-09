import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Style 1.0
import "../Config"
import "../Controls2"

Dialog {
    id: root
    objectName: "serverPasswordDialog"
    property int serverIndex: -1
    property string login: ""
    property string errorMessage: ""
    anchors.centerIn: parent
    width: parent ? Math.min(480, parent.width - 24) : 480
    modal: true
    closePolicy: Popup.CloseOnEscape
    title: qsTr("Сохранённый пароль SSH")
    header: Label {
        text: root.title
        color: AmneziaStyle.color.paleGray
        font.pixelSize: 20
        padding: 16
        wrapMode: Text.Wrap
    }
    background: Rectangle {
        color: AmneziaStyle.color.onyxBlack
        radius: 12
        border.color: AmneziaStyle.color.slateGray
    }
    onAboutToShow: {
        password.textField.clear()
        confirmation.textField.clear()
        errorMessage = ""
        password.textField.forceActiveFocus()
    }
    onAboutToHide: {
        password.textField.clear()
        confirmation.textField.clear()
        errorMessage = ""
    }
    contentItem: ColumnLayout {
        spacing: 16
        Label {
            Layout.fillWidth: true
            color: AmneziaStyle.color.paleGray
            text: qsTr("Пользователь: %1").arg(root.login)
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
        }
        Label {
            Layout.fillWidth: true
            color: AmneziaStyle.color.mutedGray
            text: qsTr("Укажите пароль, уже установленный на VPS. Он сохранится только в приложении и будет использоваться при следующих SSH-подключениях.")
            wrapMode: Text.Wrap
        }
        TextFieldWithHeaderType {
            id: password
            Layout.fillWidth: true
            headerText: qsTr("Новый сохранённый пароль")
            textField.objectName: "serverPasswordInput"
            textField.echoMode: TextInput.Password
            textField.inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
        }
        TextFieldWithHeaderType {
            id: confirmation
            Layout.fillWidth: true
            headerText: qsTr("Повторите пароль")
            textField.objectName: "serverPasswordConfirmation"
            textField.echoMode: TextInput.Password
            textField.inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
        }
        Label {
            Layout.fillWidth: true
            visible: root.errorMessage !== ""
            text: root.errorMessage
            color: AmneziaStyle.color.vibrantRed
            wrapMode: Text.Wrap
        }
        RowLayout {
            BasicButtonType {
                Layout.fillWidth: true
                objectName: "serverPasswordSave"
                text: qsTr("Сохранить")
                enabled: password.textField.text.length > 0 && password.textField.text === confirmation.textField.text
                clickedFunc: function() {
                    if (!ServersModel.updateProcessedServerPassword(root.serverIndex, password.textField.text)) {
                        root.errorMessage = qsTr("Не удалось сохранить пароль. Откройте настройки сервера заново.")
                        return
                    }
                    root.close()
                    PageController.showNotificationMessage(qsTr("Пароль SSH сохранён в приложении."))
                }
            }
            BasicButtonType {
                Layout.fillWidth: true
                text: qsTr("Отмена")
                clickedFunc: function() { root.close() }
            }
        }
    }
}

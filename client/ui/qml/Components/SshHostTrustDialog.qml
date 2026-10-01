import QtQuick
import QtQuick.Controls

Dialog {
    id: root
    objectName: "sshHostTrustDialog"
    property string endpoint: ""
    property string fingerprint: ""
    readonly property string yesText: qsTr("Yes")
    readonly property string noText: qsTr("No")

    // BusyIndicatorType is also modal (z=1). The decision must remain clickable
    // while the SSH operation waits for it, including later busy notifications.
    z: 100
    anchors.centerIn: parent
    width: Math.min(610, Math.max(280, parent ? parent.width - 32 : 610))
    modal: true
    closePolicy: Popup.NoAutoClose
    title: qsTr("Первое подключение к VPS")
    standardButtons: Dialog.Yes | Dialog.No
    contentItem: Label {
        text: qsTr("Проверьте отпечаток ключа SSH через панель или консоль провайдера VPS, прежде чем доверять серверу.")
            + "\n\n" + root.endpoint + "\n" + root.fingerprint
        wrapMode: Text.WrapAnywhere
        padding: 16
        textFormat: Text.PlainText
    }
    onOpened: {
        standardButton(Dialog.Yes).objectName = "sshTrustYesButton"
        standardButton(Dialog.No).objectName = "sshTrustNoButton"
        standardButton(Dialog.Yes).text = Qt.binding(function() { return root.yesText })
        standardButton(Dialog.No).text = Qt.binding(function() { return root.noText })
    }
    onAccepted: SshHostTrust.answer(true)
    onRejected: SshHostTrust.answer(false)
}

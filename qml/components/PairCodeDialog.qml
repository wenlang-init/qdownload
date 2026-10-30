import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qdownload

/*
 * 客户端侧：输入对方屏幕上显示的 6 位配对码（设计文档 §7.2）。
 * 由 App.pairCodePrompt 打开；提交走 App.submitPairCode()。
 */
Dialog {
    id: dialog

    modal: true
    closePolicy: Popup.CloseOnEscape
    anchors.centerIn: parent
    width: Math.min(380, (parent ? parent.width : 380) - 2 * Theme.gapL)
    padding: Theme.gapL

    property string errorText: App.connError

    background: Rectangle {
        color: Theme.surface
        radius: Theme.radius
        border.width: 1
        border.color: Theme.border
    }

    function openForPrompt() {
        codeField.text = "";
        errorText = "";
        open();
        codeField.forceActiveFocus();
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.gapM

        Label {
            Layout.fillWidth: true
            text: qsTr("与「%1」配对").arg(App.selectedPeerName)
            color: Theme.textPrimary
            font.pixelSize: Theme.fontTitle
            font.weight: Font.Medium
            elide: Text.ElideRight
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("对方主机界面上会显示一个 6 位配对码，输入即可完成配对。")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }

        TextField {
            id: codeField
            Layout.fillWidth: true
            placeholderText: qsTr("6 位数字")
            font.pixelSize: 22
            font.letterSpacing: 6
            horizontalAlignment: TextInput.AlignHCenter
            inputMethodHints: Qt.ImhDigitsOnly
            maximumLength: 6
            color: Theme.textPrimary
            background: Rectangle {
                color: Theme.surfaceAlt
                radius: Theme.radiusSm
                border.width: 1
                border.color: codeField.activeFocus ? Theme.accent : Theme.border
            }
            onAccepted: if (text.length >= 4) dialog.submit()
        }

        Label {
            Layout.fillWidth: true
            text: dialog.errorText
            visible: text.length > 0
            color: Theme.danger
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gapS

            Item { Layout.fillWidth: true }

            Button {
                text: qsTr("取消")
                onClicked: dialog.close()
            }

            Button {
                text: qsTr("配对")
                enabled: codeField.text.length >= 4
                highlighted: true
                onClicked: dialog.submit()
            }
        }
    }

    function submit() {
        App.submitPairCode(codeField.text);
        errorText = "";
        dialog.close();
    }

    // 失败原因回到这里提示，而不是只在远端页状态条里一闪
    Connections {
        target: App
        function onPairFailed(message) {
            if (dialog.opened || App.connStateCode === "unpaired") {
                dialog.errorText = message;
                dialog.open();
            }
        }
    }
}

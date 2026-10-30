import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qdownload

/*
 * 通用二次确认框（M8）。
 *
 * Qt Quick Controls 没有内置确认框，MessageDialog 又依赖 QtQuick.Dialogs 的原生实现，
 * 在部分平台上样式完全脱离主题。这里用 Dialog 自绘一个，供「关写权限 / 关共享」共用。
 */
Dialog {
    id: dialog

    property string headline: ""
    property string detail: ""
    property string acceptText: qsTr("确定")
    property string cancelText: qsTr("取消")

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    padding: Theme.gapL
    closePolicy: Popup.CloseOnEscape
    width: Math.min(440, (parent ? parent.width : 640) - 2 * Theme.gapL)

    // Basic 样式的默认背景是纯黑，跟主题对不上，自己画一个
    background: Rectangle {
        color: Theme.surface
        radius: 10
        border.width: 1
        border.color: Theme.border
    }

    contentItem: ColumnLayout {
        spacing: Theme.gapM

        Label {
            Layout.fillWidth: true
            text: dialog.headline
            color: Theme.textPrimary
            font.pixelSize: Theme.fontTitle
            wrapMode: Text.WordWrap
        }

        Label {
            Layout.fillWidth: true
            text: dialog.detail
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gapS

            Item { Layout.fillWidth: true }

            Button {
                text: dialog.cancelText
                onClicked: dialog.reject()
            }

            Button {
                text: dialog.acceptText
                highlighted: true
                onClicked: dialog.accept()
            }
        }
    }
}

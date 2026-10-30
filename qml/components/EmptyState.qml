import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qdownload

/* 空态：图标位 + 主文案 + 辅助说明。三个阶段（无主机/未配对/空目录）复用同一个。 */
ColumnLayout {
    id: state

    property string text: ""
    property string hint: ""

    spacing: Theme.gapS
    width: Math.min(360, (parent ? parent.width : 360) - 3 * Theme.gapL)

    Rectangle {
        Layout.alignment: Qt.AlignHCenter
        implicitWidth: 44
        implicitHeight: 44
        radius: 22
        color: Theme.surfaceAlt
        border.width: 1
        border.color: Theme.border
    }

    Label {
        Layout.fillWidth: true
        text: state.text
        horizontalAlignment: Text.AlignHCenter
        color: Theme.textSecondary
        font.pixelSize: Theme.fontBody
        wrapMode: Text.WordWrap
    }

    Label {
        Layout.fillWidth: true
        text: state.hint
        visible: text.length > 0
        horizontalAlignment: Text.AlignHCenter
        color: Theme.textTertiary
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.WordWrap
    }
}

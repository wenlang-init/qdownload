import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qdownload

/*
 * 写权限总开关（设计文档 §7.4 L1）。
 * 常驻顶栏，旁边显示当前生效范围：关 / 白名单 / 全部已配对。
 *
 * 关掉它等于「立刻掐断正在收的上传」，所以有传输时必须先确认一次，
 * 而不是让用户点一下就把别人的传输打断（M8）。
 */
RowLayout {
    id: toggle

    // 窄屏时隐藏策略摘要，只留开关本体，避免顶栏溢出
    property bool compact: false

    /// 截图/演示用：由外部拉起来（--demo-confirm-write-at）
    property bool demoConfirm: false

    onDemoConfirmChanged: if (demoConfirm) confirmCloseWrite.open()

    spacing: Theme.gapS

    ToggleChip {
        id: writeChip
        text: qsTr("写权限")
        checked: App.writeEnabled
        onToggled: function (value) {
            if (!value && App.incomingCount > 0) {
                writeChip.reset();      // 先把开关拨回去，用户点「取消」时状态不能错位
                confirmCloseWrite.open();
                return;
            }
            App.writeEnabled = value;
        }
    }

    Label {
        // 属性值绑定（不是 writeSummary()）：带 NOTIFY，开关一变这行跟着变
        text: App.writeSummary
        visible: !toggle.compact
        color: App.writeEnabled ? Theme.success : Theme.textTertiary
        font.pixelSize: Theme.fontSmall
    }

    ConfirmDialog {
        id: confirmCloseWrite
        headline: qsTr("关闭写权限？")
        detail: qsTr("当前有 %1 个上传正在接收，关闭写权限会立刻中断它们。\n"
                     + "已经收到的部分会保留为断点，重新打开后对方可以接着传。")
                    .arg(App.incomingCount)
        acceptText: qsTr("中断并关闭")
        cancelText: qsTr("继续接收")
        onAccepted: App.writeEnabled = false
    }
}

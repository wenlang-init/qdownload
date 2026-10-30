import QtQuick
import QtQuick.Controls
import Qdownload

/*
 * 轻量提示条：右上角/底部浮一条，3.8 秒自己消失。
 *
 * 为什么不用 Dialog：传输完成的提示不该拦住用户操作；也不该只在传输抽屉里显示，
 * 抽屉收起时用户根本看不到（设计文档 §8.5）。
 */
Rectangle {
    id: toast

    property string message: ""

    /// 外面统一走 show()，避免直接改 message 时忘了重启计时器
    function show(text) {
        if (!text)
            return;
        toast.message = text;
        hideTimer.restart();
    }

    implicitWidth: Math.min(label.implicitWidth + 2 * Theme.gapL, 460)
    implicitHeight: label.implicitHeight + Theme.gapM
    radius: 8
    color: Theme.dark ? "#2E2E2C" : "#2B2B28"
    border.width: 1
    // 8 位色在 Qt 里是 #AARRGGBB（alpha 在前）
    border.color: Theme.dark ? "#4A4A46" : "#22000000"

    opacity: 0
    visible: opacity > 0
    scale: opacity > 0.5 ? 1 : 0.98

    Behavior on opacity { NumberAnimation { duration: 180 } }
    Behavior on scale { NumberAnimation { duration: 180 } }

    Label {
        id: label
        anchors.fill: parent
        anchors.leftMargin: Theme.gapL
        anchors.rightMargin: Theme.gapL
        verticalAlignment: Text.AlignVCenter
        text: toast.message
        color: "#F5F5F0"
        font.pixelSize: Theme.fontBody
        elide: Text.ElideRight
        maximumLineCount: 2
        wrapMode: Text.WordWrap
    }

    Timer {
        id: hideTimer
        interval: 3800
        onTriggered: toast.opacity = 0
    }

    onMessageChanged: opacity = 1

    Component.onCompleted: opacity = 0
}

import QtQuick
import Qt.labs.platform as Platform
import Qdownload

/*
 * 系统托盘（设计文档 §8.5）。
 *
 * 单独一个文件 + Loader 加载：`Qt.labs.platform` 在部分环境（无 StatusNotifier 的
 * Linux、旧 Windows）会加载失败，写在 Main.qml 里会让整个界面起不来。
 * 加载失败时 Loader.status === Error，托盘功能整体降级，主窗口照常可用。
 *
 * Android 没有托盘概念，这里的 available 恒为 false，通知改走 C++ 的 JNI 通道。
 */
Platform.SystemTrayIcon {
    id: tray

    /// 用户在托盘里点了「显示主窗口」——由 Main.qml 接住
    signal windowRequested()

    /// 用户在托盘里点了「退出」。不放 Qt.quit() 在这里：退出前要不要确认是主窗口的职责，
    /// 而且托盘组件自己 quit 会让 Main.qml 的清理流程整个跳过
    signal quitRequested()

    /// 托盘可用性变化。Qt 的 SystemTrayIcon.available 在 QML 侧连不上 availableChanged
    /// （Qt 6 没给它导出变更通知），所以拿它派生出一个属性，改派生属性的通知反过来发信号
    signal availabilityChanged(bool ok)
    readonly property bool availableNow: available
    onAvailableNowChanged: availabilityChanged(availableNow)

    visible: available
    icon.source: Qt.resolvedUrl("../assets/icons/tray.svg")

    // 注意：SystemTrayIcon 不是 Item，没有默认属性，这里放不下任何子元素
    // （menu 是具名属性所以能写）。图标是否真的加载成功由 Main.qml 里的探针负责告警。
    tooltip: App.transferActiveCount > 0
             ? qsTr("qdownload · 正在传输 %1 项").arg(App.transferActiveCount)
             : qsTr("qdownload · %1").arg(App.deviceName)

    onActivated: function (reason) {
        // 左键单击/双击都当"把窗口叫回来"，Windows 用户两种习惯都有
        if (reason === Platform.SystemTrayIcon.Trigger
                || reason === Platform.SystemTrayIcon.DoubleClick) {
            tray.windowRequested();
        }
    }

    /// 气泡通知。返回 false 表示托盘不可用，调用方应退回窗口内提示
    function notify(title, body) {
        if (!available || !visible)
            return false;
        showMessage(title, body, Platform.SystemTrayIcon.Information, 5000);
        return true;
    }

    menu: Platform.Menu {
        Platform.MenuItem {
            text: qsTr("显示主窗口")
            onTriggered: tray.windowRequested()
        }

        Platform.MenuSeparator {}

        // 用 ✓ 前缀而不是 checkable：原生菜单的勾选态在某些后端点击后不会跟随绑定刷新
        Platform.MenuItem {
            text: (App.shareEnabled ? "✓ " : "　") + qsTr("共享我的文件")
            onTriggered: App.shareEnabled = !App.shareEnabled
        }

        Platform.MenuItem {
            text: (App.writeEnabled ? "✓ " : "　") + qsTr("接收别人上传")
            onTriggered: App.writeEnabled = !App.writeEnabled
        }

        Platform.MenuSeparator {}

        Platform.MenuItem {
            text: App.transferActiveCount > 0
                  ? qsTr("传输中 %1 项 · %2").arg(App.transferActiveCount).arg(App.transferSummary)
                  : qsTr("没有进行中的传输")
            enabled: false
        }

        Platform.MenuItem {
            text: App.incomingCount > 0 ? App.incomingSummary : qsTr("收件箱空闲")
            enabled: false
            visible: App.incomingCount > 0
        }

        Platform.MenuSeparator {}

        Platform.MenuItem {
            text: qsTr("退出")
            onTriggered: tray.quitRequested()
        }
    }
}

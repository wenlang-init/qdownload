import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qdownload

/*
 * 共享端侧：有设备请求配对时弹出（设计文档 §7.2）。
 *
 * 这是整套安全模型的关键一环：配对码**由本机生成并显示**，
 * 对方必须能看到这块屏幕才能输对。同时这里是 L3 单设备权限的授予点。
 */
Dialog {
    id: dialog

    readonly property var request: App.pairRequest
    readonly property bool hasRequest: request !== undefined && request.peerId !== undefined
                                       && String(request.peerId).length > 0

    modal: true
    closePolicy: Popup.NoAutoClose
    anchors.centerIn: parent
    width: Math.min(420, (parent ? parent.width : 420) - 2 * Theme.gapL)
    padding: Theme.gapL

    visible: hasRequest

    background: Rectangle {
        color: Theme.surface
        radius: Theme.radius
        border.width: 1
        border.color: Theme.border
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.gapM

        Label {
            Layout.fillWidth: true
            text: qsTr("配对请求")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontTitle
            font.weight: Font.Medium
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("「%1」请求访问本机共享的文件").arg(dialog.hasRequest ? request.name : "")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
            wrapMode: Text.WordWrap
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 64
            radius: Theme.radiusSm
            color: Theme.surfaceAlt
            border.width: 1
            border.color: Theme.border

            RowLayout {
                anchors.centerIn: parent
                spacing: Theme.gapM

                Label {
                    text: dialog.hasRequest ? request.code : "------"
                    color: Theme.textPrimary
                    font.pixelSize: 30
                    font.letterSpacing: 8
                    font.weight: Font.DemiBold
                }

                Label {
                    text: dialog.hasRequest ? qsTr("%1s").arg(request.expiresIn) : ""
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontSmall
                    Layout.alignment: Qt.AlignBottom
                }
            }
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("请让「%1」在它自己的窗口里输入上面这串码。")
                      .arg(dialog.hasRequest ? request.name : "")
            color: Theme.textTertiary
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.border
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gapS

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    text: qsTr("允许该设备上传文件")
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontBody
                }

                Label {
                    text: App.writeEnabled ? qsTr("还需要顶部「写权限」开关处于打开状态")
                                           : qsTr("当前写权限总开关是关的，上传会被拒绝")
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontSmall
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }

            Switch {
                id: writeSwitch
                checked: false
                implicitWidth: 34
                implicitHeight: 20
                padding: 0

                indicator: Rectangle {
                    implicitWidth: 34
                    implicitHeight: 20
                    radius: 10
                    x: 0
                    y: (writeSwitch.height - height) / 2
                    color: writeSwitch.checked ? Theme.accent
                                               : (Theme.dark ? "#40403C" : "#CFCFC8")

                    Rectangle {
                        y: 3
                        width: 14
                        height: 14
                        radius: 7
                        color: writeSwitch.checked ? "#FFFFFF"
                                                   : (Theme.dark ? "#8C8C86" : "#FFFFFF")
                        x: writeSwitch.checked ? parent.width - width - 3 : 3
                        Behavior on x {
                            NumberAnimation { duration: 110; easing.type: Easing.OutCubic }
                        }
                    }
                }

                contentItem: Item {
                    implicitWidth: 0
                    implicitHeight: 0
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gapS

            Item { Layout.fillWidth: true }

            Button {
                text: qsTr("拒绝")
                onClicked: App.rejectPendingPair()
            }

            Button {
                id: allowButton
                text: qsTr("允许配对")
                highlighted: true
                onClicked: App.approvePendingPair(writeSwitch.checked)
            }
        }
    }

    // 仅联调（--approve-after <ms>）：延时"点"一下真正的按钮，
    // 这样走的是 QML 点击 → C++ 同步改状态 → QML 重新求值 的真实栈，
    // 和 --auto-approve 在 begin() 里提前放行完全不是一条路
    Timer {
        interval: Math.max(0, App.demoApproveMs)
        repeat: false
        running: App.demoApproveMs >= 0 && dialog.hasRequest
                 && dialog.request.verified === true
        onTriggered: {
            // 复现真实用户的操作：先勾「允许该设备上传」，再点允许
            writeSwitch.checked = true
            allowButton.clicked()
        }
    }

    // 每次新请求都把「允许上传」重置为关闭：权限默认最小，不能用上一次的选择
    onHasRequestChanged: if (hasRequest) writeSwitch.checked = false
}

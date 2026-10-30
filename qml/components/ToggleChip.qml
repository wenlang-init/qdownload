import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qdownload

/* 顶栏开关胶囊：小圆点状态灯 + 文案 + 开关。共享、写权限共用。 */
RowLayout {
    id: chip

    property alias text: label.text
    property bool checked: false
    signal toggled(bool checked)

    /// 外部拒绝这次切换时把开关拨回去。
    /// 用户点击 Switch 会打断 `checked: chip.checked` 这条绑定，光靠重新赋值
    /// chip.checked 是拨不回来的，必须直接改内部 Switch。
    function reset() {
        control.checked = chip.checked;
    }

    // App 侧状态回写时同步给内部 Switch（主动赋值，避免绑定被打断后不同步）
    onCheckedChanged: control.checked = checked

    spacing: Theme.gapS

    Rectangle {
        implicitWidth: 7
        implicitHeight: 7
        radius: 3.5
        color: chip.checked ? Theme.success : Theme.textTertiary
    }

    Label {
        id: label
        color: chip.checked ? Theme.textPrimary : Theme.textSecondary
        font.pixelSize: Theme.fontBody
    }

    Switch {
        id: control
        checked: chip.checked
        // 自绘指示器后必须显式给尺寸：否则 contentItem 为空时 implicitWidth 算不出，
        // RowLayout 会按 0 宽排列，指示器就压到后面的文字上（M2 踩过）
        implicitWidth: 34
        implicitHeight: 20
        padding: 0
        // checked 由 AppController 回写，值相等时不重复触发，避免环
        onCheckedChanged: if (checked !== chip.checked) chip.toggled(checked)

        // 自绘指示器：Basic 样式的默认配色不跟 Theme 走，深色下开/关几乎看不出区别
        indicator: Rectangle {
            implicitWidth: 34
            implicitHeight: 20
            radius: 10
            x: 0
            y: (control.height - height) / 2
            color: control.checked ? Theme.accent
                                   : (Theme.dark ? "#40403C" : "#CFCFC8")

            Rectangle {
                y: 3
                width: 14
                height: 14
                radius: 7
                color: control.checked ? "#FFFFFF"
                                       : (Theme.dark ? "#8C8C86" : "#FFFFFF")
                x: control.checked ? parent.width - width - 3 : 3

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

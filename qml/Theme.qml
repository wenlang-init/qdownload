pragma Singleton
import QtQuick
import QtQuick.Controls

/*
 * 全局视觉令牌（设计文档 §8）。QML 里直接 `Theme.xxx` 使用。
 * 全部走基本样式自绘，不依赖平台原生控件外观。
 */
QtObject {
    readonly property bool dark: Application.styleHints.colorScheme === Qt.Dark

    // 表面
    readonly property color bg:           dark ? "#141413" : "#F4F4F2"
    readonly property color surface:      dark ? "#1E1E1C" : "#FFFFFF"
    readonly property color surfaceAlt:   dark ? "#262624" : "#EDEDE9"
    readonly property color border:       dark ? "#3A3A37" : "#DCDCD6"

    // 文字
    readonly property color textPrimary:  dark ? "#EDEDE9" : "#1B1B19"
    readonly property color textSecondary: dark ? "#A6A6A0" : "#5A5A55"
    readonly property color textTertiary: dark ? "#6E6E68" : "#8C8C86"

    // 语义色
    readonly property color accent:       dark ? "#5B9CFF" : "#2563EB"
    readonly property color accentSoft:   dark ? "#182A45" : "#E6EFFF"
    readonly property color success:      dark ? "#3FBF92" : "#0F7A5A"
    readonly property color danger:       dark ? "#E5675F" : "#C0392B"
    readonly property color warning:      dark ? "#E0A33F" : "#9A6B10"

    // 尺寸
    readonly property int radius:    10
    readonly property int radiusSm:  6
    readonly property int gapS:      6
    readonly property int gapM:      12
    readonly property int gapL:      18
    readonly property int bpWide:    900

    // 字号
    readonly property int fontSmall: 12
    readonly property int fontBody:  13
    readonly property int fontTitle: 15
}

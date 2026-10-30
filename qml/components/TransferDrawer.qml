import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qdownload

/*
 * 传输抽屉：收起时一行摘要（进行中的合计速度），展开看任务明细。
 *
 * 进度、速度、ETA 全部由 C++ 侧 300ms 节流推送（TransferModel），
 * QML 不做任何计时或推算——否则界面上的速度和实际收字节会各算一套。
 */
Rectangle {
    id: drawer

    property bool expanded: false
    property var tasks: null
    property int taskCount: 0
    property int activeCount: 0
    property int finishedCount: 0
    property string summary: ""
    property string notice: ""

    readonly property int rowHeight: 74

    color: Theme.surface
    implicitHeight: expanded ? Math.min(360, 46 + Math.max(1, taskCount) * rowHeight) : 38

    Behavior on implicitHeight {
        NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
    }

    Rectangle {
        anchors.top: parent.top
        width: parent.width
        height: 1
        color: Theme.border
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---------------- 表头 ----------------
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 38

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.gapM
                anchors.rightMargin: Theme.gapM
                spacing: Theme.gapS

                Label {
                    text: qsTr("传输任务 (%1)").arg(drawer.taskCount)
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }

                Label {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 40
                    text: drawer.notice.length > 0 ? drawer.notice : drawer.summary
                    color: drawer.notice.length > 0 ? Theme.warning : Theme.textTertiary
                    font.pixelSize: Theme.fontSmall
                    elide: Text.ElideRight
                }

                ToolButton {
                    visible: drawer.finishedCount > 0
                    text: qsTr("清除已完成")
                    font.pixelSize: Theme.fontSmall
                    implicitHeight: 24
                    onClicked: App.clearFinishedTransfers()
                }

                Label {
                    text: drawer.expanded ? "▾" : "▴"
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontSmall
                }
            }

            TapHandler {
                onTapped: drawer.expanded = !drawer.expanded
            }

            HoverHandler {
                cursorShape: Qt.PointingHandCursor
            }
        }

        Rectangle {
            Layout.fillWidth: true
            visible: drawer.expanded
            implicitHeight: 1
            color: Theme.border
        }

        // ---------------- 任务列表 ----------------
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: drawer.expanded

            ListView {
                id: list
                anchors.fill: parent
                clip: true
                model: drawer.tasks
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                delegate: Rectangle {
                    id: trow

                    readonly property string stateCode: model.stateCode
                    readonly property color stateColor:
                        stateCode === "failed" ? Theme.danger
                        : stateCode === "completed" ? Theme.success
                        : stateCode === "paused" ? Theme.warning
                        : stateCode === "canceled" ? Theme.textTertiary
                        : Theme.accent

                    width: ListView.view.width
                    height: drawer.rowHeight
                    color: "transparent"

                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: Theme.border
                        opacity: 0.4
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.gapM
                        anchors.rightMargin: Theme.gapM
                        anchors.topMargin: 7
                        anchors.bottomMargin: 5
                        spacing: 3

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gapS

                            Label {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 40
                                text: qsTr("↓ ") + model.name
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontBody
                                elide: Text.ElideMiddle
                            }

                            Label {
                                text: model.percentText
                                color: trow.stateColor
                                font.pixelSize: Theme.fontSmall
                                font.weight: Font.Medium
                            }

                            Label {
                                text: model.stateText
                                color: trow.stateColor
                                font.pixelSize: Theme.fontSmall
                            }
                        }

                        // 进度条：自绘而不是 ProgressBar——Basic 样式的默认配色不跟 Theme 走
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 4
                            radius: 2
                            color: Theme.surfaceAlt

                            Rectangle {
                                width: parent.width * Math.max(0, Math.min(1, model.progress))
                                height: parent.height
                                radius: 2
                                color: trow.stateColor

                                Behavior on width {
                                    NumberAnimation { duration: 180; easing.type: Easing.Linear }
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gapS

                            Label {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 40
                                text: model.errorText.length > 0
                                      ? model.errorText
                                      : model.sizeText + " · " + model.speedText
                                        + (model.etaText.length > 0 ? " · " + model.etaText : "")
                                color: model.errorText.length > 0 ? Theme.danger : Theme.textTertiary
                                font.pixelSize: Theme.fontSmall
                                elide: Text.ElideRight
                            }

                            ToolButton {
                                visible: model.canPause
                                text: qsTr("暂停")
                                font.pixelSize: Theme.fontSmall
                                implicitHeight: 24
                                onClicked: App.pauseTransfer(model.taskId)
                            }

                            ToolButton {
                                visible: model.canResume
                                text: qsTr("继续")
                                font.pixelSize: Theme.fontSmall
                                implicitHeight: 24
                                onClicked: App.resumeTransfer(model.taskId)
                            }

                            ToolButton {
                                visible: model.canCancel
                                text: qsTr("取消")
                                font.pixelSize: Theme.fontSmall
                                implicitHeight: 24
                                onClicked: App.cancelTransfer(model.taskId)
                            }
                        }
                    }
                }
            }

            EmptyState {
                anchors.centerIn: parent
                visible: drawer.taskCount === 0
                text: qsTr("还没有传输任务")
                hint: qsTr("在右侧远端列表里点「下载」，进度、速度、暂停/取消都会显示在这里")
            }
        }
    }
}

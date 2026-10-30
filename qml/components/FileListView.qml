import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qdownload

/*
 * 文件列表容器（本机页与远端页共用）。
 *
 * 平铺 + 面包屑，不用 TreeView：Qt Quick 的 TreeView 在 Android 上 delegate
 * 嵌套深、滚动掉帧，而文件浏览场景里“进目录”的成本很低（设计文档 §8.1）。
 *
 * 模型约定（FileListModel）：name / isDir / sizeText / mtimeText / kind。
 */
Item {
    id: view

    property var files: null
    property string emptyText: qsTr("暂无内容")
    property string emptyHint: ""
    property bool loading: false
    property bool showAction: false
    property bool actionEnabled: true
    property string actionText: qsTr("下载")
    // 删除按钮（M14）：本机页删自己的文件，远端页请主机删它那边的文件。
    // 没权限时保持可见但置灰，比"按钮莫名消失"更容易理解
    property bool showDelete: false
    property bool deleteEnabled: true

    readonly property bool narrow: width < 460
    readonly property bool veryNarrow: width < 360

    signal itemActivated(string name, bool isDir)
    signal itemActionClicked(string name, bool isDir)
    signal itemDeleteClicked(string name, bool isDir)
    signal loadMoreRequested()

    ListView {
        id: list
        anchors.fill: parent
        clip: true
        model: view.files
        visible: count > 0
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        // 滚到底部就向主机要下一页（分页由服务端切片，客户端不做排序）
        onAtYEndChanged: {
            if (atYEnd && view.files && view.files.hasMore)
                view.loadMoreRequested();
        }

        delegate: Rectangle {
            id: row

            readonly property bool isDir: model.isDir

            width: ListView.view.width
            height: 46
            color: mouse.containsMouse ? Theme.surfaceAlt : "transparent"

            Behavior on color {
                ColorAnimation { duration: 90 }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.border
                opacity: 0.45
            }

            // 整行点击区域必须声明在 RowLayout 之前：QML 里后声明的元素在上层，
            // 若放在最后会盖住「下载」按钮，把按钮点击吞掉
            MouseArea {
                id: mouse
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton
                cursorShape: row.isDir ? Qt.PointingHandCursor : Qt.ArrowCursor
                // 目录单击即进入：手机上只有 tap 没有 double-click
                onClicked: view.itemActivated(model.name, row.isDir)
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.gapM
                anchors.rightMargin: Theme.gapS
                spacing: Theme.gapS

                // 图标用矩形拼：emoji 在 Linux 上常缺字体渲成方框，自绘图形三端一致。
                // 文件夹=宽而矮（带标签页），文件=窄而高，形状和颜色双重区分
                Item {
                    implicitWidth: 22
                    implicitHeight: 22
                    Layout.alignment: Qt.AlignVCenter

                    Rectangle {
                        visible: row.isDir
                        x: 1
                        y: 3
                        width: 10
                        height: 4
                        radius: 1.5
                        color: Theme.accent
                        opacity: 0.75
                    }

                    Rectangle {
                        visible: row.isDir
                        x: 1
                        y: 5
                        width: 20
                        height: 15
                        radius: 3
                        color: Theme.accent
                        opacity: 0.9
                    }

                    Rectangle {
                        visible: !row.isDir
                        x: 4
                        y: 2
                        width: 15
                        height: 18
                        radius: 2.5
                        color: Theme.textTertiary
                        opacity: 0.5
                    }
                }

                Label {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 40
                    text: model.name
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontBody
                    font.weight: row.isDir ? Font.Medium : Font.Normal
                    elide: Text.ElideMiddle
                    verticalAlignment: Text.AlignVCenter
                }

                Label {
                    visible: !view.veryNarrow
                    Layout.preferredWidth: 74
                    horizontalAlignment: Text.AlignRight
                    text: model.sizeText
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontSmall
                }

                Label {
                    visible: !view.narrow
                    Layout.preferredWidth: 104
                    horizontalAlignment: Text.AlignRight
                    text: model.mtimeText
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontSmall
                }

                Label {
                    visible: row.isDir
                    Layout.preferredWidth: 14
                    horizontalAlignment: Text.AlignHCenter
                    text: "›"
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontTitle
                }

                // 删除放在主操作左边：破坏性动作别贴着行尾，手滑一下就是不可恢复的
                Button {
                    id: deleteButton

                    visible: view.showDelete && !row.isDir
                    enabled: view.deleteEnabled
                    flat: true
                    implicitWidth: 48
                    implicitHeight: 28
                    text: qsTr("删除")
                    onClicked: view.itemDeleteClicked(model.name, row.isDir)

                    contentItem: Label {
                        text: deleteButton.text
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: deleteButton.enabled ? Theme.danger : Theme.textTertiary
                        font.pixelSize: Theme.fontSmall
                    }
                }

                Button {
                    visible: view.showAction && !row.isDir
                    enabled: view.actionEnabled
                    implicitWidth: 64
                    implicitHeight: 28
                    text: view.actionText
                    font.pixelSize: Theme.fontSmall
                    onClicked: view.itemActionClicked(model.name, row.isDir)
                }
            }
        }
    }

    // 加载下一页时的轻提示
    BusyIndicator {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.gapS
        width: 24
        height: 24
        running: view.loading && list.count > 0
        visible: running
    }

    EmptyState {
        anchors.centerIn: parent
        visible: list.count === 0 && !view.loading
        text: view.emptyText
        hint: view.emptyHint
    }

    Label {
        anchors.centerIn: parent
        visible: list.count === 0 && view.loading
        text: qsTr("加载中…")
        color: Theme.textTertiary
        font.pixelSize: Theme.fontSmall
    }
}

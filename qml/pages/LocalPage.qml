import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qdownload

/*
 * 本机文件页：浏览接收目录（下载落点）。
 * 与"共享给别人的目录"分开——一个是自己看的，一个是别人看的，混在一起容易误删。
 */
Rectangle {
    id: page

    color: Theme.surface

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.gapM
            Layout.rightMargin: page.width < 420 ? Theme.gapS : Theme.gapM
            Layout.topMargin: Theme.gapS
            Layout.bottomMargin: Theme.gapS
            spacing: Theme.gapS

            Label {
                text: qsTr("本机")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontTitle
                font.weight: Font.Medium
            }

            Label {
                Layout.fillWidth: true
                Layout.minimumWidth: 40
                text: App.localRoot
                color: Theme.textTertiary
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideMiddle
            }

            ToolButton {
                text: qsTr("收件箱")
                font.pixelSize: Theme.fontSmall
                implicitHeight: 26
                ToolTip.visible: hovered
                ToolTip.text: App.inboxPath
                onClicked: App.openInbox()
            }

            ToolButton {
                text: qsTr("上级")
                font.pixelSize: Theme.fontSmall
                enabled: App.localPath !== "/"
                implicitHeight: 26
                opacity: enabled ? 1 : 0.4
                onClicked: App.localUp()
            }

            ToolButton {
                text: qsTr("刷新")
                font.pixelSize: Theme.fontSmall
                implicitHeight: 26
                onClicked: App.localRefresh()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.border
        }

        Breadcrumb {
            Layout.fillWidth: true
            segments: App.localBreadcrumb
            onNavigate: function (path) { App.localGo(path) }
        }

        // 目录过大时的提示：不假装只有这些，明确告诉用户被截断了
        Rectangle {
            Layout.fillWidth: true
            visible: App.localNotice.length > 0
            implicitHeight: visible ? 26 : 0
            color: Theme.surfaceAlt

            Label {
                anchors.fill: parent
                anchors.leftMargin: Theme.gapM
                anchors.rightMargin: Theme.gapM
                verticalAlignment: Text.AlignVCenter
                text: App.localNotice
                color: Theme.warning
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
            }
        }

        // 上传不可用时把原因写在脸上：按钮置灰但不解释，用户只会反复点
        Rectangle {
            Layout.fillWidth: true
            visible: App.selectedPeerId.length > 0 && !App.peerCanUpload
            implicitHeight: visible ? 24 : 0
            color: Theme.surfaceAlt

            Label {
                anchors.fill: parent
                anchors.leftMargin: Theme.gapM
                anchors.rightMargin: Theme.gapM
                verticalAlignment: Text.AlignVCenter
                text: App.uploadHint
                color: Theme.textTertiary
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
            }
        }

        FileListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            files: App.localFiles
            emptyText: App.localPath === "/" ? qsTr("接收目录还是空的") : qsTr("这个目录是空的")
            emptyHint: qsTr("从右侧远端列表下载的文件会出现在这里")
            // 上传：落到对方的 /inbox（写权限关闭时按钮置灰）
            showAction: true
            actionText: qsTr("上传")
            actionEnabled: App.peerCanUpload
            // 删除：本机文件删了就是删了，先弹确认框
            showDelete: true
            deleteEnabled: true
            onItemActivated: function (name, isDir) {
                if (isDir)
                    App.localEnter(name);
            }
            onItemActionClicked: function (name, isDir) {
                if (!isDir)
                    App.uploadLocalFile(name);
            }
            onItemDeleteClicked: function (name, isDir) {
                if (!isDir) {
                    deleteDialog.fileName = name;
                    deleteDialog.open();
                }
            }
        }
    }

    ConfirmDialog {
        id: deleteDialog

        property string fileName: ""

        headline: qsTr("删除本机文件？")
        detail: qsTr("「%1」会被永久删除，不进回收站。").arg(fileName)
        acceptText: qsTr("删除")
        onAccepted: App.deleteLocalFile(fileName)
    }
}

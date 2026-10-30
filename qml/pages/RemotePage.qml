import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qdownload

/*
 * 远端文件页。
 * M2 跟着发现结果走，M3 控制通道与配对，M4 真实目录列表（分页 + 面包屑）。
 */
Rectangle {
    id: page

    color: Theme.surface

    readonly property bool hasPeer: App.selectedPeerId.length > 0
    readonly property bool peerOnline: App.selectedPeerOnline
    readonly property string peerBadge: App.selectedPeerBadge
    readonly property string peerKind: App.selectedPeerKind
    readonly property string connState: App.connStateCode
    readonly property bool ready: connState === "ready"

    function statusText() {
        if (!page.hasPeer)
            return qsTr("在上方选择一台共享主机；对方没开共享也会出现在列表里");
        if (!page.peerOnline)
            return qsTr("「%1」已离线，等待它重新广播（10 秒无心跳判离线）").arg(App.selectedPeerName);
        if (page.peerKind === "unshared")
            return qsTr("「%1」在线，但没有开启共享").arg(App.selectedPeerName);

        switch (page.connState) {
        case "connecting":
        case "handshaking":
        case "authenticating":
            return qsTr("正在连接「%1」…").arg(App.selectedPeerName);
        case "unpaired":
            return App.connError.length > 0
                    ? App.connError
                    : qsTr("未配对：点右侧「配对」后，输入对方屏幕上显示的 6 位码");
        case "pairing":
            return qsTr("已发出配对请求，等待对方在主机上点「允许」，并把配对码告诉你");
        case "awaitingCode":
            return qsTr("请输入对方屏幕上显示的 6 位配对码");
        case "ready":
            if (!App.peerShares)
                return qsTr("「%1」已关闭共享，目录暂时不可读").arg(App.selectedPeerName);
            if (App.remoteError.length > 0)
                return App.remoteError;
            return App.peerCanUpload
                    ? qsTr("已连接「%1」· 可读可写 · %2 项").arg(App.selectedPeerName).arg(App.remoteTotal)
                    : qsTr("已连接「%1」· 只读 · %2 项").arg(App.selectedPeerName).arg(App.remoteTotal);
        case "failed":
            return App.connError.length > 0 ? App.connError : qsTr("连接失败");
        default:
            // 手动断开后不会再自动连回来（心跳也不拉），得把出路讲清楚
            return App.peerDisconnectedByUser
                    ? qsTr("已手动断开「%1」，点「重连」恢复").arg(App.selectedPeerName)
                    : qsTr("未连接");
        }
    }

    function statusColor() {
        if (!page.hasPeer || !page.peerOnline)
            return Theme.textTertiary;
        if (page.connState === "ready")
            return App.remoteError.length > 0 ? Theme.warning : Theme.success;
        if (page.connState === "failed" || page.connState === "unpaired")
            return Theme.warning;
        return Theme.accent;
    }

    function emptyText() {
        if (!page.hasPeer)
            return qsTr("尚未选择共享主机");
        if (page.connState === "unpaired")
            return qsTr("尚未配对");
        if (!page.ready)
            return qsTr("尚未连接");
        if (App.remoteError.length > 0)
            return App.remoteError;
        return qsTr("这个目录是空的");
    }

    function emptyHint() {
        if (!page.hasPeer)
            return qsTr("等对方开启共享后会自动出现在上方列表");
        if (page.connState === "unpaired")
            return qsTr("配对后才能浏览对方的文件");
        if (App.peerDisconnectedByUser)
            return qsTr("你手动断开了连接，不会自动重连");
        if (!page.ready)
            return qsTr("连上主机后自动拉取目录");
        if (App.remoteError.length > 0)
            return qsTr("点「刷新」重试，或检查对方是否关闭了共享");
        return qsTr("对方共享目录里还没有内容");
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.gapM
            Layout.rightMargin: Theme.gapM
            Layout.topMargin: Theme.gapS
            Layout.bottomMargin: Theme.gapS
            spacing: Theme.gapS

            Label {
                text: page.hasPeer ? App.selectedPeerName : qsTr("远端")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontTitle
                font.weight: Font.Medium
                elide: Text.ElideRight
                Layout.maximumWidth: page.width * 0.45
            }

            Rectangle {
                visible: page.hasPeer
                implicitWidth: badgeLabel.implicitWidth + 2 * Theme.gapS
                implicitHeight: 18
                radius: 9
                color: badgeLabel.color === Theme.success || badgeLabel.color === Theme.accent
                       ? Theme.accentSoft : Theme.surfaceAlt

                Label {
                    id: badgeLabel
                    anchors.centerIn: parent
                    text: page.peerBadge
                    color: !page.peerOnline ? Theme.textTertiary
                         : page.peerKind === "upload" ? Theme.success
                         : page.peerKind === "readonly" ? Theme.accent
                         : Theme.warning
                    font.pixelSize: Theme.fontSmall - 1
                }
            }

            Item { Layout.fillWidth: true }

            Label {
                text: page.hasPeer ? App.connStateText : qsTr("未选择")
                color: page.statusColor()
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
                Layout.maximumWidth: page.width * 0.35
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.border
        }

        // 连接 / 权限 / 错误状态条
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 38
            color: Theme.accentSoft

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.gapM
                anchors.rightMargin: Theme.gapS
                spacing: Theme.gapS

                Label {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 40
                    text: page.statusText()
                    color: page.connState === "failed" || page.connState === "unpaired"
                           || App.remoteError.length > 0
                           ? Theme.warning : Theme.accent
                    font.pixelSize: Theme.fontSmall
                    elide: Text.ElideRight
                }

                Button {
                    text: qsTr("配对")
                    visible: page.hasPeer && page.peerOnline && page.connState === "unpaired"
                    font.pixelSize: Theme.fontSmall
                    implicitHeight: 26
                    onClicked: App.requestPairing()
                }

                Button {
                    text: qsTr("重连")
                    visible: page.hasPeer && page.peerOnline
                             && (page.connState === "failed" || page.connState === "idle")
                    font.pixelSize: Theme.fontSmall
                    implicitHeight: 26
                    onClicked: App.connectToSelectedPeer()
                }

                Button {
                    text: qsTr("输入配对码")
                    visible: page.connState === "awaitingCode"
                    font.pixelSize: Theme.fontSmall
                    implicitHeight: 26
                    onClicked: codeDialog.openForPrompt()
                }

                ToolButton {
                    text: qsTr("上级")
                    visible: page.ready
                    font.pixelSize: Theme.fontSmall
                    implicitHeight: 26
                    enabled: App.remotePath !== "/"
                    opacity: enabled ? 1 : 0.4
                    onClicked: App.remoteUp()
                }

                ToolButton {
                    text: qsTr("刷新")
                    visible: page.ready || page.connState === "failed"
                    font.pixelSize: Theme.fontSmall
                    implicitHeight: 26
                    onClicked: page.ready ? App.remoteRefresh() : App.connectToSelectedPeer()
                }

                Button {
                    text: qsTr("断开")
                    visible: page.ready || page.connState === "pairing"
                    font.pixelSize: Theme.fontSmall
                    implicitHeight: 26
                    onClicked: App.disconnectPeer()
                }
            }
        }

        Breadcrumb {
            Layout.fillWidth: true
            segments: App.remoteBreadcrumb
            onNavigate: function (path) {
                if (page.ready)
                    App.remoteGo(path);
            }
        }

        FileListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            files: App.remoteFiles
            loading: App.remoteLoading
            emptyText: page.emptyText()
            emptyHint: page.emptyHint()
            showAction: page.ready && App.peerShares
            actionEnabled: page.ready && App.peerShares
            actionText: qsTr("下载")
            // 远端删除：改的是对方共享目录，只有拿到写权限才可点（置灰更说明问题）
            showDelete: page.ready && App.peerShares
            deleteEnabled: App.peerCanUpload
            onItemActivated: function (name, isDir) {
                if (isDir && page.ready)
                    App.remoteEnter(name);
            }
            onItemActionClicked: function (name, isDir) {
                if (!isDir)
                    App.downloadFile(name);
            }
            onItemDeleteClicked: function (name, isDir) {
                if (!isDir && App.peerCanUpload) {
                    deleteDialog.fileName = name;
                    deleteDialog.open();
                }
            }
            onLoadMoreRequested: App.remoteLoadMore()
        }
    }

    ConfirmDialog {
        id: deleteDialog

        property string fileName: ""

        headline: qsTr("删除对方共享目录里的文件？")
        detail: qsTr("「%1」会在「%2」那台机器上被永久删除。").arg(fileName).arg(App.selectedPeerName)
        acceptText: qsTr("删除")
        onAccepted: App.deleteRemoteFile(fileName)
    }

    // 输入配对码的弹窗挂在远端页上：它只服务于“连这台主机”，不属于全局
    PairCodeDialog {
        id: codeDialog
    }

    Connections {
        target: App
        function onPairCodePrompt() {
            codeDialog.openForPrompt();
        }
    }
}

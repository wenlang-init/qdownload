import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Qdownload

/* 设置里的“选目录”一行：标题 + 当前路径 + 更改/恢复默认。
   共享根目录与接收目录共用，role 决定走哪套校验（0 共享根 / 1 接收目录）。

   选目录在 QML 里做：FolderDialog 是平台原生的，C++ 只提供两个工具——
   pathToUrl/urlToPath 做路径往返，directoryIssue 做应用前的体检。 */
ColumnLayout {
    id: picker

    /// 0 = 共享根，1 = 接收目录
    property int role: 0
    property string title: ""
    property string path: ""
    property string fallbackPath: ""
    /// 目录正在对外提供时的附加说明（共享开着时，改了别人立刻会看到）
    property string hint: ""

    /// 结果文案交给外面弹 Toast，组件自己不弹框
    signal outcome(string text, bool isError)

    spacing: 3

    function apply(candidate) {
        // 既吃 FolderDialog 给的 url，也吃外部直接塞进来的路径字符串
        const local = App.urlToPath(candidate);
        const target = local.length > 0 ? local : candidate;

        if (target.length === 0) {
            picker.outcome(qsTr("取不到这个目录的本地路径，换个位置试试"), true);
            return;
        }

        if (target === picker.path) {
            picker.outcome(qsTr("已经是这个目录了"), false);
            return;
        }

        const issue = App.directoryIssue(target, picker.role);
        if (issue.length > 0) {
            picker.outcome(issue, true);
            return;
        }

        if (picker.role === 0)
            App.shareRoot = target;
        else
            App.localRoot = target;

        picker.outcome(qsTr("已切换到 %1").arg(target), false);
    }

    /// 外部（截图 / 排障）直接拉起内置浏览器入口
    function openBrowser() {
        dirBrowser.openAt(picker.path);
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.gapS

        Label {
            text: picker.title
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
        }

        Item {
            Layout.fillWidth: true
        }

        Button {
            text: qsTr("更改")
            Layout.preferredHeight: 28
            // 安卓不能用系统对话框：它返回的是 content:// 授权 URI（QFile 打不开），
            // 也不会过滤掉没权限写的公共目录。那里改走内置浏览器，只在确定能写的范围里选
            onClicked: App.builtInDirPicker ? dirBrowser.openAt(picker.path)
                                            : folderDialog.pickHere()
        }

        Button {
            text: qsTr("恢复默认")
            Layout.preferredHeight: 28
            visible: picker.fallbackPath.length > 0 && picker.path !== picker.fallbackPath
            onClicked: picker.apply(picker.fallbackPath)
        }
    }

    Label {
        Layout.fillWidth: true
        text: picker.path.length > 0 ? picker.path : qsTr("未设置")
        color: Theme.textPrimary
        font.pixelSize: Theme.fontBody
        elide: Text.ElideMiddle

        ToolTip.visible: hoverArea.containsMouse && picker.path.length > 0
        ToolTip.delay: 500
        ToolTip.text: picker.path

        MouseArea {
            id: hoverArea

            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
        }
    }

    Label {
        Layout.fillWidth: true
        visible: text.length > 0
        text: picker.hint
        color: Theme.textTertiary
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.Wrap
    }

    Label {
        Layout.fillWidth: true
        visible: App.builtInDirPicker && App.storageHint.length > 0
        text: App.storageHint
        color: Theme.textTertiary
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.Wrap
    }

    DirBrowser {
        id: dirBrowser

        title: qsTr("选择%1").arg(picker.title)
        startPath: picker.path

        onAccepted: function (path) { picker.apply(path); }
    }

    FolderDialog {
        id: folderDialog

        title: qsTr("选择%1").arg(picker.title)
        acceptLabel: qsTr("选择")

        // currentFolder 只在第一次弹之前有意义，每次重开前重设，
        // 保证起始位置永远对着当前目录（而不是上次打开时记的临时 url）
        function pickHere() {
            const start = picker.path.length > 0 ? picker.path : picker.fallbackPath;
            folderDialog.currentFolder = App.pathToUrl(start);
            folderDialog.open();
        }

        onAccepted: picker.apply(folderDialog.selectedFolder)
    }
}

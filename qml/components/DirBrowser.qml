import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qdownload

/* 安卓用的「内置目录浏览器」。
 *
 * 为什么不用系统的 FolderDialog：android 上它返回的是 content:// 授权 URI
 * （指向 DocumentProvider 的虚拟节点），QFile / QDir 一律打不开；
 * 而且它不会过滤掉本应用没权限写的公共目录——选到 /sdcard/Download 这种地方，
 * 一路点下去最后才弹出「没有写入权限」，用户根本不知道该选哪。
 *
 * 这里的策略反过来：只在「已经试写通过」的根里浏览（由 App.storageRoots 提供），
 * 选到哪一步都能用，不存在点了之后报错的情况。想要更多位置就先去授权
 * （MANAGE_EXTERNAL_STORAGE），授权回来 App.storageRoots 会多出一项。
 *
 * 桌面平台不受影响，仍然用系统对话框。 */
Popup {
    id: root

    property string title: ""
    property string startPath: ""
    /// 结果交给外面处理：DirPicker.apply 还要再做一次互斥 / 写权限体检
    signal accepted(string path)

    /// 打开时刷一次：用户在系统设置页开完权限回来这里才拿得到新状态
    property var locations: []
    property string browsePath: ""
    /// 当前位置的顶端（再往上就出了允许范围）
    property string ceilingPath: ""

    readonly property bool canGoUp: browsePath.length > 0 && browsePath !== ceilingPath
    /// 存储根本身不让选：directoryIssue 同样会拒它（把整块盘共享出去），这里提前挡住更直观
    readonly property bool canPick: browsePath.length > 0 && browsePath !== ceilingPath

    modal: true
    dim: true
    clip: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    // 这个 Popup 声明在设置对话框的 contentItem 里，不显式挂到 Overlay 的话
    // 会跟设置对话框抢 z 序，打开后被压在下面看不见
    parent: Overlay.overlay
    z: 5

    width: Overlay.overlay ? Math.min(Overlay.overlay.width - 24, 460) : 440
    height: Overlay.overlay ? Math.min(Overlay.overlay.height - 40, 640) : 600
    anchors.centerIn: Overlay.overlay

    background: Rectangle {
        radius: Theme.radiusSm
        color: Theme.surface
        border.color: Theme.border
        border.width: 1
    }

    // 某个路径落在哪个可用根下面。顺带决定了「上级」能退到哪为止
    function locationFor(path) {
        if (locations.length === 0)
            return path;

        let best = locations[0].path;
        for (let i = 0; i < locations.length; ++i) {
            const base = locations[i].path;
            if (path === base || path.indexOf(base + "/") === 0) {
                if (base.length > best.length)
                    best = base;
            }
        }
        return best;
    }

    function openAt(path) {
        App.refreshStorageAccess();
        locations = App.storageRoots();

        if (locations.length === 0) {
            browsePath = "";
            ceilingPath = "";
            root.open();
            return;
        }

        const target = path.length > 0 ? path : locations[0].path;
        const base = locationFor(target);
        // 路径落在允许范围之外（比如上次设的内部目录这回被系统收走了）就从头开始
        browsePath = (target === base || target.indexOf(base + "/") === 0) ? target : base;
        ceilingPath = base;
        newFolderName.text = "";
        root.open();
    }

    function enterDir(name) {
        browsePath = App.childPath(browsePath, name);
    }

    function goUp() {
        if (!canGoUp)
            return;
        browsePath = App.parentPath(browsePath);
    }

    function createFolder() {
        const name = newFolderName.text.trim();
        if (name.length === 0)
            return;

        const made = App.createSubDirectory(browsePath, name);
        if (made.length === 0) {
            folderHint.text = qsTr("建不了这个文件夹：换个名字试试");
            folderHint.visible = true;
            return;
        }
        folderHint.visible = false;
        newFolderName.text = "";
        browsePath = made;
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.gapM
        spacing: Theme.gapS

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gapS

            Label {
                Layout.fillWidth: true
                text: root.title
                font.bold: true
                color: Theme.textPrimary
                elide: Text.ElideRight
            }

            Button {
                flat: true
                text: qsTr("取消")
                Layout.preferredHeight: 28
                onClicked: root.close()
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.locations.length === 0
            text: qsTr("找不到可用的存储位置，请在系统设置里允许本应用访问文件")
            color: Theme.textTertiary
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.Wrap
        }

        // 可用位置之间切换：只有一个时没必要占一行
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gapS
            visible: root.locations.length > 1

            Label {
                text: qsTr("位置")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
            }

            ComboBox {
                id: locationBox

                Layout.fillWidth: true
                model: root.locations.map(function (item) { return item.label; })

                onActivated: {
                    const base = root.locations[currentIndex].path;
                    root.browsePath = base;
                    root.ceilingPath = base;
                }
            }
        }

        Label {
            Layout.fillWidth: true
            text: root.browsePath.length > 0 ? root.browsePath : qsTr("未设置")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSmall
            elide: Text.ElideMiddle
            visible: root.browsePath.length > 0
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.border
        }

        ListView {
            id: dirList

            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            currentIndex: -1

            model: root.browsePath.length > 0 ? App.subDirectories(root.browsePath) : []

            delegate: ItemDelegate {
                width: dirList.width
                text: modelData
                font.pixelSize: Theme.fontBody

                contentItem: Label {
                    text: modelData
                    color: Theme.textPrimary
                    elide: Text.ElideMiddle
                    verticalAlignment: Text.AlignVCenter
                }

                onClicked: root.enterDir(modelData)
            }

            header: ItemDelegate {
                width: dirList.width
                visible: root.canGoUp
                height: root.canGoUp ? implicitHeight : 0
                text: ".."

                contentItem: Label {
                    text: qsTr("返回上级")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                    verticalAlignment: Text.AlignVCenter
                }

                onClicked: root.goUp()
            }

            Label {
                anchors.centerIn: parent
                visible: dirList.count === 0
                text: qsTr("这个目录里没有子文件夹")
                color: Theme.textTertiary
                font.pixelSize: Theme.fontSmall
            }
        }

        Label {
            Layout.fillWidth: true
            visible: App.storageHint.length > 0
            text: App.storageHint
            color: Theme.textTertiary
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true
            visible: App.storageHint.length > 0
            spacing: Theme.gapS

            Item { Layout.fillWidth: true }

            Button {
                text: qsTr("去授权")
                Layout.preferredHeight: 28
                onClicked: App.requestStorageAccess()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.border
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gapS

            TextField {
                id: newFolderName

                Layout.fillWidth: true
                placeholderText: qsTr("新建文件夹")
                font.pixelSize: Theme.fontSmall
                Layout.preferredHeight: 30
                enabled: root.browsePath.length > 0

                onAccepted: root.createFolder()
            }

            Button {
                text: qsTr("新建")
                Layout.preferredHeight: 30
                enabled: newFolderName.text.trim().length > 0
                onClicked: root.createFolder()
            }
        }

        Label {
            id: folderHint

            Layout.fillWidth: true
            visible: false
            color: Theme.textTertiary
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gapS

            Item { Layout.fillWidth: true }

            Button {
                text: qsTr("选择此目录")
                Layout.preferredHeight: 30
                highlighted: root.canPick
                enabled: root.canPick

                onClicked: {
                    root.accepted(root.browsePath);
                    root.close();
                }
            }
        }
    }
}

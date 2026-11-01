import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qdownload

ApplicationWindow {
    id: root

    visible: true
    width: 1180
    height: 760
    minimumWidth: 400
    minimumHeight: 520
    title: qsTr("qdownload · %1").arg(App.deviceName)
    color: Theme.bg

    // 设计文档 §8.1：900dp 断点，两种布局共用同一批页面组件
    readonly property bool wide: width >= Theme.bpWide

    // ---- 系统栏避让（Android 高版本打孔屏）----
    // Android 15（targetSdk 35+）起强制 edge-to-edge，窗口铺满整屏，状态栏、挖孔、
    // 手势条直接盖在内容上；低版本系统没有这条强制规则，Qt 会自己把窗口内缩，所以
    // 只有高版本才看得见「界面被任务栏压住」。Qt 6.9+ 把平台 insets 经 SafeArea
    // 送到 QML（QWindow::safeAreaMargins 已换算成逻辑像素，不要再除 dpr）。
    // 平台不提供时全为 0，桌面端布局不受影响。
    readonly property real safeTop: SafeArea.margins.top
    readonly property real safeBottom: SafeArea.margins.bottom
    readonly property real safeLeft: SafeArea.margins.left
    readonly property real safeRight: SafeArea.margins.right

    /// 窗口是否“真的在前台”：最小化时 visible 仍是 true，不能只看它
    readonly property bool windowActive: visible && visibility !== Window.Minimized && active

    function raiseWindow() {
        root.show();
        root.raise();
        root.requestActivate();
    }

    /// 通知分发：前台走窗口内轻提示，后台走托盘气泡，托盘也没有就退回窗口内
    function notifyUser(title, body) {
        if (root.windowActive)
            return;      // 前台已经由 transferNotice 驱动的 Toast 覆盖，不再弹第二条
        if (trayLoader.item && trayLoader.item.notify(title, body))
            return;
        toast.show(title + "：" + body);
    }

    /// 换目录的结果反馈。错误情况除了弹提示也记一行日志——
    /// 用户选了什么被拒，光看界面猜不出来
    function dirMessage(text, isError) {
        if (isError)
            console.warn("切换目录失败：", text);
        toast.show(text);
    }

    /// 设置里改名：空名字直接回退到当前值，不落库
    function applyDeviceName() {
        const value = nameField.text.trim();
        if (value.length === 0) {
            nameField.text = App.deviceName;
            return;
        }
        App.deviceName = value;
        nameField.text = App.deviceName;
        root.dirMessage(qsTr("本端名称已改为「%1」").arg(App.deviceName), false);
    }

    /// 正在跑的任务：自己发起的（下载/上传）+ 别人传进收件箱的
    readonly property int busyTransfers: App.transferActiveCount + App.incomingCount

    /// 本次已经过确认，别再弹第二次
    property bool quitConfirmed: false
    property bool trayHintShown: false

    /// 统一退出入口：窗口 × 与托盘「退出」都走这里
    function requestQuit() {
        if (!root.quitConfirmed && root.busyTransfers > 0) {
            confirmQuit.open();
            return;
        }
        root.quitConfirmed = true;
        root.close();
        // 窗口藏在托盘里时 close() 只是再走一次；Qt.quit() 才是真的收工
        Qt.quit();
    }

    /// 收进托盘。托盘不可用时不能这么做——窗口一藏用户就再也找不回来了
    function hideToTray() {
        root.hide();
        if (root.trayHintShown)
            return;
        root.trayHintShown = true;

        // 窗口已经藏了，窗口内的 Toast 根本看不见 —— 必须走托盘气泡
        const title = qsTr("已收进托盘");
        const body = qsTr("点托盘图标叫回窗口；要真正退出，在托盘菜单里点「退出」");
        if (!trayLoader.item || !trayLoader.item.notify(title, body))
            toast.show(title + "：" + body);
    }

    // 截图/验收钩子：到点自己走一遍 requestQuit()，等于替用户点一次 ×
    Timer {
        running: App.demoQuitMs >= 0
        interval: Math.max(0, App.demoQuitMs)
        repeat: false
        onTriggered: root.requestQuit()
    }

    onClosing: function (close) {
        if (root.quitConfirmed)
            return;                       // 放行：Qt 默认在最后一个窗口关闭时退出

        if (root.busyTransfers > 0) {
            close.accepted = false;       // 别默默把别人的传输掐了，先问
            confirmQuit.open();
            return;
        }

        if (App.trayAvailable && App.closeToTray) {
            close.accepted = false;
            root.hideToTray();
        }
    }

    header: Rectangle {
        // 顶部再垫一条状态栏/挖孔高度，背景色顺势铺到屏幕最上沿
        implicitHeight: 52 + (App.discoveryOk ? 0 : 26) + root.safeTop
        color: Theme.surface

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.border
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.topMargin: root.safeTop
            anchors.leftMargin: root.safeLeft
            anchors.rightMargin: root.safeRight
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 52
                Layout.leftMargin: root.wide ? Theme.gapM : Theme.gapS
                Layout.rightMargin: root.wide ? Theme.gapM : Theme.gapS
                spacing: root.wide ? Theme.gapM : Theme.gapS

                ToggleChip {
                    id: shareChip
                    text: qsTr("共享")
                    checked: App.shareEnabled
                    onToggled: function (value) {
                        // 关共享同样会中断正在收的上传（ControlServer 里把两者一起处理），
                        // 所以和写权限一样要先确认
                        if (!value && App.incomingCount > 0) {
                            shareChip.reset();
                            confirmCloseShare.open();
                            return;
                        }
                        App.shareEnabled = value;
                    }
                }

                WriteToggle {
                    compact: !root.wide
                    demoConfirm: App.demoConfirmWrite
                }

                PeerSelector {
                    Layout.preferredWidth: root.wide ? 220 : 110
                    Layout.minimumWidth: 90
                }

                Item { Layout.fillWidth: true }

                ToolButton {
                    text: qsTr("刷新")
                    font.pixelSize: Theme.fontSmall
                    visible: root.wide
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("立即广播并刷新主机状态")
                    onClicked: App.announceNow()
                }

                ToolButton {
                    text: "⚙"
                    font.pixelSize: 16
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("设置（共享根、上传策略、并发数、限速）")
                    onClicked: settingsDialog.open()
                }
            }

            // 组播被防火墙/交换机拦住时，必须让用户看到原因，否则只会觉得“找不到设备”
            Rectangle {
                visible: !App.discoveryOk
                Layout.fillWidth: true
                Layout.preferredHeight: 26
                color: Theme.dark ? "#3A2A18" : "#FFF4E0"

                Label {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.gapM
                    anchors.rightMargin: Theme.gapM
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("⚠ 组播发现不可用：%1").arg(App.discoveryError)
                    color: Theme.warning
                    font.pixelSize: Theme.fontSmall
                    elide: Text.ElideRight
                }
            }
        }
    }

    Loader {
        anchors.fill: parent
        // 横屏时挖孔大多在侧边，内容区跟着往里让一点
        anchors.leftMargin: root.safeLeft
        anchors.rightMargin: root.safeRight
        sourceComponent: root.wide ? wideLayout : narrowLayout
    }

    footer: ColumnLayout {
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 24
            color: Theme.surfaceAlt

            Label {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: Theme.gapM
                anchors.right: writeState.left
                anchors.rightMargin: Theme.gapM
                text: {
                    var base = App.statusLine + " · " + App.peerSummary;
                    if (App.incomingCount > 0)
                        base += " · " + App.incomingSummary;
                    return base;
                }
                color: Theme.textTertiary
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
            }

            Label {
                id: writeState
                anchors.verticalCenter: parent.verticalCenter
                anchors.right: parent.right
                anchors.rightMargin: Theme.gapM
                // 必须是属性绑定（App.writeSummary），写成 writeSummary() 只求值一次，
                // 写权限开关怎么拨它都不动
                text: qsTr("写权限：%1").arg(App.writeSummary)
                color: App.writeEnabled ? Theme.success : Theme.textTertiary
                font.pixelSize: Theme.fontSmall
            }
        }

        TransferDrawer {
            id: transferDrawer
            Layout.fillWidth: true
            tasks: App.transfers
            taskCount: App.transferCount
            activeCount: App.transferActiveCount
            finishedCount: App.transferFinishedCount
            summary: App.transferSummary
            notice: App.transferNotice

            Component.onCompleted: {
                if (App.demoTransfers)
                    expanded = true;
            }
        }

        // 底部让位：手势条 / 三键导航栏会盖住 footer，垫一条同色空白，
        // 免得状态行和抽屉里的按钮被系统栏压住点不到
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: root.safeBottom
            visible: root.safeBottom > 0
            color: Theme.surfaceAlt
        }
    }

    Component.onCompleted: {
        if (App.demoSettings)
            settingsDialog.open();
        // 截图/排障：桌面也强制走安卓那套内置目录浏览器（没法在真机上一点点试）
        if (App.demoDirBrowser)
            Qt.callLater(sharePicker.openBrowser);
        // 启动自愈：上次存的目录这回写不了，已经临时退回默认值
        if (App.startupNotice.length > 0)
            Qt.callLater(toast.show, App.startupNotice);
    }

    // ---- 桌面：左右双栏 ----
    Component {
        id: wideLayout

        SplitView {
            orientation: Qt.Horizontal

            handle: Rectangle {
                implicitWidth: 1
                implicitHeight: 1
                color: Theme.border
            }

            LocalPage {
                SplitView.fillWidth: true
                SplitView.minimumWidth: 260
                SplitView.preferredWidth: 560
            }

            RemotePage {
                SplitView.fillWidth: true
                SplitView.minimumWidth: 300
                SplitView.preferredWidth: 620
            }
        }
    }

    // ---- 手机 / 窄窗口：两个 Tab ----
    Component {
        id: narrowLayout

        ColumnLayout {
            spacing: 0

            TabBar {
                id: tabs
                Layout.fillWidth: true
                currentIndex: App.demoTabIndex

                background: Rectangle {
                    color: Theme.surface

                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: Theme.border
                    }
                }

                TabButton {
                    id: tabLocal
                    text: qsTr("本机")
                    background: Rectangle {
                        color: tabLocal.checked ? Theme.surfaceAlt : "transparent"

                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 2
                            color: tabLocal.checked ? Theme.accent : "transparent"
                        }
                    }
                    contentItem: Label {
                        text: tabLocal.text
                        color: tabLocal.checked ? Theme.textPrimary : Theme.textSecondary
                        font.pixelSize: Theme.fontBody
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                TabButton {
                    id: tabRemote
                    text: qsTr("远端")
                    background: Rectangle {
                        color: tabRemote.checked ? Theme.surfaceAlt : "transparent"

                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 2
                            color: tabRemote.checked ? Theme.accent : "transparent"
                        }
                    }
                    contentItem: Label {
                        text: tabRemote.text
                        color: tabRemote.checked ? Theme.textPrimary : Theme.textSecondary
                        font.pixelSize: Theme.fontBody
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }

            StackLayout {
                currentIndex: tabs.currentIndex
                Layout.fillWidth: true
                Layout.fillHeight: true

                LocalPage {}
                RemotePage {}
            }
        }
    }

    Dialog {
        id: settingsDialog
        title: qsTr("设置")
        modal: true
        anchors.centerIn: parent
        width: Math.min(420, root.width - 2 * Theme.gapL)
        // 高度 = 理想值（标题栏 + 内容自然高 + 底栏）再跟可用区取小。
        // 不限高的話内容一多就把底部按钮顶到窗口外，「关闭」根本点不到。
        // 可用区还要扣掉状态栏 / 导航栏：高版本 Android 是 edge-to-edge，窗口高度是全屏高度
        height: Math.max(260,
                         Math.min(root.height - 2 * Theme.gapL - root.safeTop - root.safeBottom,
                                  header.implicitHeight + 2 * padding
                                  + settingsBody.implicitHeight
                                  + settingsFooter.implicitHeight
                                  + Theme.gapM))
        padding: Theme.gapL

        background: Rectangle {
            color: Theme.surface
            radius: 10
            border.width: 1
            border.color: Theme.border
        }

        // 默认标题栏是纯黑，和对话框主体有色差，这里统一成主题色
        header: Rectangle {
            color: Theme.surface
            implicitHeight: 42

            Label {
                anchors.fill: parent
                anchors.leftMargin: Theme.gapL
                anchors.rightMargin: Theme.gapL
                text: settingsDialog.title
                color: Theme.textPrimary
                font.pixelSize: Theme.fontTitle
                font.weight: Font.Medium
                verticalAlignment: Text.AlignVCenter
            }

            Rectangle {
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                height: 1
                color: Theme.border
            }
        }

        // 每次打开都把输入框同步回当前名字：TextField 一旦被编辑，
        // 初值那条绑定就断了，不复位的話会看到上一次没保存的草稿
        onOpened: {
            nameField.text = App.deviceName;
            // 上次滚到一半就关掉的话，重开要回到顶部，否则看到的是半截内容
            settingsFlick.contentY = 0;
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: Theme.gapM

            // 加了「本端名称」后内容变长，窗口矮一点就把底部按钮顶出可视区，
            // 「关闭」和「打开收件箱」直接点不到。所以主体做成滚动区，
            // 底栏留在外面永远够得着
            Flickable {
                id: settingsFlick

                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                flickableDirection: Flickable.VerticalFlick
                boundsBehavior: Flickable.StopAtBounds
                contentWidth: width
                contentHeight: settingsBody.implicitHeight

                ScrollBar.vertical: ScrollBar {}

                ColumnLayout {
                    id: settingsBody

                    width: settingsFlick.width
                    spacing: Theme.gapM

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            text: qsTr("本端名称")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSmall
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gapS

                            TextField {
                                id: nameField

                                Layout.fillWidth: true
                                text: App.deviceName
                                placeholderText: App.defaultDeviceName
                                font.pixelSize: Theme.fontBody
                                // 与 C++ 侧 32 字上限保持一致：列表一行放得下，广播包也不会被撑大
                                maximumLength: 32
                                selectByMouse: true
                                onAccepted: root.applyDeviceName()
                            }

                            Button {
                                text: qsTr("保存")
                                font.pixelSize: Theme.fontSmall
                                implicitHeight: 30
                                enabled: nameField.text.trim().length > 0
                                         && nameField.text.trim() !== App.deviceName
                                onClicked: root.applyDeviceName()
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("对方在主机列表里看到的就是这个名字，保存后立刻广播，不用等心跳")
                            color: Theme.textTertiary
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.Wrap
                        }

                        Button {
                            text: qsTr("恢复默认（%1）").arg(App.defaultDeviceName)
                            font.pixelSize: Theme.fontSmall
                            implicitHeight: 28
                            visible: App.deviceName !== App.defaultDeviceName
                            onClicked: {
                                App.deviceName = "";
                                nameField.text = App.deviceName;
                                root.dirMessage(qsTr("本端名称已恢复默认"), false);
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 1
                        color: Theme.border
                    }

                    DirPicker {
                        id: sharePicker

                        Layout.fillWidth: true

                        role: 0
                        title: qsTr("共享根目录")
                        path: App.shareRoot
                        fallbackPath: App.defaultShareRoot
                        hint: App.shareEnabled ? qsTr("正在共享，更换后对方刷新列表就会看到新目录")
                                               : qsTr("别人能看到的目录，建议单独放一个文件夹")

                        onOutcome: function (text, isError) {
                            root.dirMessage(text, isError);
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 1
                        color: Theme.border
                    }

                    DirPicker {
                        Layout.fillWidth: true

                        role: 1
                        title: qsTr("接收目录")
                        path: App.localRoot
                        fallbackPath: App.defaultReceiveRoot
                        hint: qsTr("下载存到这里，本机文件列表显示的就是它")
                                 + (App.transferActiveCount > 0
                                    ? " · " + qsTr("正在传的任务不受影响，断点仍写在旧目录")
                                    : "")

                        onOutcome: function (text, isError) {
                            root.dirMessage(text, isError);
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 1
                        color: Theme.border
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            text: qsTr("已配对设备 %1 台").arg(App.trustedDevices.length)
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontBody
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("控制端口 %1 · 写权限：%2")
                                      .arg(App.ctrlPort).arg(App.writeSummary)
                            color: Theme.textTertiary
                            font.pixelSize: Theme.fontSmall
                        }
                    }

                    Button {
                        text: qsTr("管理")
                        onClicked: {
                            settingsDialog.close();
                            trustedDialog.open();
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 1
                        color: Theme.border
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            text: qsTr("配对记录与断点存在这里")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSmall
                        }

                        Label {
                            Layout.fillWidth: true
                            text: App.storageSummary
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontBody
                            elide: Text.ElideMiddle
                            wrapMode: Text.Wrap

                            ToolTip.visible: dataHover.containsMouse && App.storageRoot.length > 0
                            ToolTip.delay: 500
                            ToolTip.text: App.storageRoot

                            MouseArea {
                                id: dataHover

                                anchors.fill: parent
                                hoverEnabled: true
                                acceptedButtons: Qt.NoButton
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("整个文件夹拷走就能带走全部记录；装在受保护的目录里才会改用系统位置")
                            color: Theme.textTertiary
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.Wrap
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 1
                        color: Theme.border
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("上传策略（仅在写权限打开时生效）")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gapS

                        Repeater {
                            model: [qsTr("仅白名单（单设备 rw）"), qsTr("已配对皆可传")]

                            delegate: ToolButton {
                                required property int index
                                required property string modelData

                                text: modelData
                                font.pixelSize: Theme.fontSmall
                                implicitHeight: 28
                                onClicked: App.uploadPolicy = index

                                background: Rectangle {
                                    radius: 6
                                    color: App.uploadPolicy === index ? Theme.surfaceAlt : "transparent"
                                    border.width: 1
                                    border.color: App.uploadPolicy === index ? Theme.accent : Theme.border
                                }
                            }
                        }

                        Item { Layout.fillWidth: true }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("收件箱：%1").arg(App.inboxPath)
                        color: Theme.textTertiary
                        font.pixelSize: Theme.fontSmall
                        elide: Text.ElideMiddle
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 1
                        color: Theme.border
                    }

                    // 并发数与连接数是同一类设置（都只影响"怎么传"），并排放能省一大块高度，
                    // 否则设置面板在小窗口里要多滚一屏才看得到第二个
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gapL

                        ColumnLayout {
                            spacing: 4

                            Label {
                                text: qsTr("文件传输并发数")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSmall
                            }

                            RowLayout {
                                spacing: Theme.gapS

                                SpinBox {
                                    id: threadBox

                                    from: App.transferThreadsMin
                                    to: App.transferThreadsMax
                                    stepSize: 1
                                    editable: true
                                    implicitHeight: 30
                                    Layout.preferredWidth: 110
                                    font.pixelSize: Theme.fontSmall

                                    // 用命令式赋值 + valueModified（只认用户操作）：
                                    // 挂 valueChanged 会在初始化钳制时把设置写坏
                                    // （SpinBox 创建时 value 先钳到 from，再触发一次 changed）
                                    Component.onCompleted: value = App.transferThreads
                                    onValueModified: App.transferThreads = value

                                    Connections {
                                        target: App

                                        function onTransferThreadsChanged() {
                                            if (!threadBox.activeFocus)
                                                threadBox.value = App.transferThreads;
                                        }
                                    }
                                }

                                Label {
                                    text: qsTr("排队 %1 个").arg(App.queuedTransferCount)
                                    visible: App.queuedTransferCount > 0
                                    color: Theme.accent
                                    font.pixelSize: Theme.fontSmall
                                }
                            }
                        }

                        Item { Layout.fillWidth: true }

                        ColumnLayout {
                            spacing: 4

                            Label {
                                text: qsTr("单文件连接数")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSmall
                            }

                            SpinBox {
                                id: segmentBox

                                from: 1
                                to: App.segmentCountMax
                                stepSize: 1
                                editable: true
                                implicitHeight: 30
                                Layout.preferredWidth: 110
                                font.pixelSize: Theme.fontSmall

                                // 同 threadBox：初始化用命令式赋值，写入只认 valueModified
                                Component.onCompleted: value = App.segmentCount
                                onValueModified: App.segmentCount = value

                                Connections {
                                    target: App

                                    function onSegmentCountChanged() {
                                        if (!segmentBox.activeFocus)
                                            segmentBox.value = App.segmentCount;
                                    }
                                }
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("并发数 = 同时传几个文件，超出的自动排队；连接数 = 每个文件同时开几条连接，谁先传完谁接着领下一段。改动立即生效")
                        color: Theme.textTertiary
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.Wrap
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 1
                        color: Theme.border
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("传输限速")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gapS

                        SpinBox {
                            id: limitBox

                            from: 0
                            to: App.speedLimitMax
                            stepSize: 1
                            editable: true
                            implicitHeight: 30
                            font.pixelSize: Theme.fontSmall

                            // 同并发数那节：valueChanged 会在创建时的钳制中先触发一次，
                            // 用它初值化会把设置写坏，所以只认用户操作（valueModified）
                            Component.onCompleted: value = App.speedLimitMbps
                            onValueModified: App.speedLimitMbps = value

                            Connections {
                                target: App

                                function onSpeedLimitChanged() {
                                    if (!limitBox.activeFocus)
                                        limitBox.value = App.speedLimitMbps;
                                }
                            }
                        }

                        Label {
                            text: qsTr("MB/s")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSmall
                        }

                        Label {
                            text: App.speedLimitMbps === 0 ? qsTr("（不限速）") : qsTr("（当前 %1）").arg(App.speedLimitSummary)
                            color: Theme.textTertiary
                            font.pixelSize: Theme.fontSmall
                        }

                        Item { Layout.fillWidth: true }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("0 表示不限速。限速由所有任务共享：一个文件时它吃满，多个文件时大家分。改完立即生效，正在传的文件不会被打断")
                        color: Theme.textTertiary
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.Wrap
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 1
                        color: Theme.border
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("界面语言")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gapS

                        Repeater {
                            // 显式给 code，默认值是 zh_CN，所以三个都能高亮到位。
                            // 切换后立刻重译，不需要重启
                            model: [
                                { code: "zh_CN", label: "中文" },
                                { code: "en", label: "English" },
                                { code: "system", label: qsTr("跟随系统") }
                            ]

                            delegate: ToolButton {
                                required property var modelData

                                text: modelData.label
                                font.pixelSize: Theme.fontSmall
                                implicitHeight: 28
                                onClicked: App.language = modelData.code

                                background: Rectangle {
                                    radius: 6
                                    color: App.language === modelData.code ? Theme.surfaceAlt : "transparent"
                                    border.width: 1
                                    border.color: App.language === modelData.code ? Theme.accent : Theme.border
                                }
                            }
                        }

                        Item { Layout.fillWidth: true }
                    }

                    CheckBox {
                        Layout.fillWidth: true
                        visible: App.trayAvailable
                        text: qsTr("关闭窗口时收进托盘（后台继续传输）")
                        checked: App.closeToTray
                        font.pixelSize: Theme.fontBody
                        onToggled: App.closeToTray = checked
                    }
                }
            }

            RowLayout {
                id: settingsFooter

                Layout.fillWidth: true
                spacing: Theme.gapS

                Item { Layout.fillWidth: true }

                Button {
                    text: qsTr("打开收件箱")
                    onClicked: App.openInbox()
                }

                Button {
                    text: qsTr("关闭")
                    onClicked: settingsDialog.close()
                }
            }
        }
    }

    // 共享端：有设备请求配对时自动弹出（不受 layout 影响，挂在窗口上）
    PairRequestDialog {
        id: pairRequestDialog
    }

    ConfirmDialog {
        id: confirmCloseShare

        headline: qsTr("关闭共享？")
        detail: qsTr("当前有 %1 个上传正在接收，关闭共享会立刻中断它们。\n"
                     + "断点会保留，重新打开共享后对方可以接着传。")
                    .arg(App.incomingCount)
        acceptText: qsTr("中断并关闭")
        cancelText: qsTr("继续接收")
        onAccepted: App.shareEnabled = false
    }

    // 关窗口时还有传输在跑：退出会把它们掐掉，断点留着但不说一声就是坑
    ConfirmDialog {
        id: confirmQuit

        headline: qsTr("还有 %1 个传输在进行").arg(root.busyTransfers)
        detail: qsTr("现在退出会中断它们。已经传到的部分会保留成断点，"
                     + "下次重新发起会自动从断点接着传，不会丢。")
        acceptText: qsTr("仍要退出")
        cancelText: App.trayAvailable ? qsTr("收进托盘继续") : qsTr("继续传输")

        onAccepted: {
            root.quitConfirmed = true;
            root.close();
            Qt.quit();
        }
        onRejected: {
            if (App.trayAvailable)
                root.hideToTray();
        }
    }

    TrustedDevicesDialog {
        id: trustedDialog
    }

    // ---- 拖拽上传（M8）----
    // 只接文件拖拽（text/uri-list），不干扰窗口内其它交互
    DropArea {
        id: dropArea
        anchors.fill: parent
        z: 2500
        keys: ["text/uri-list"]

        onDropped: function (drop) {
            if (!drop.hasUrls)
                return;
            drop.accepted = true;
            App.uploadUrls(drop.urls);
        }
    }

    Rectangle {
        id: dropOverlay
        anchors.fill: parent
        z: 2600
        visible: dropArea.containsDrag || App.demoDrop
        // 注意 Qt 的 8 位色是 #AARRGGBB（alpha 在前），写成 #RRGGBBAA 会变成全透明
        color: Theme.dark ? "#D0000000" : "#E6FFFFFF"

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(480, root.width - 2 * Theme.gapL)
            implicitHeight: dropHint.implicitHeight + 2 * Theme.gapL
            radius: 12
            color: Theme.surface
            border.width: 2
            border.color: App.peerCanUpload ? Theme.accent : Theme.warning

            ColumnLayout {
                id: dropHint
                anchors.fill: parent
                anchors.margins: Theme.gapL
                spacing: Theme.gapS

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: App.peerCanUpload ? qsTr("松手即可上传") : qsTr("现在不能上传")
                    color: App.peerCanUpload ? Theme.accent : Theme.warning
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.Medium
                }

                Label {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignHCenter
                    text: App.peerCanUpload
                          ? qsTr("文件会存到「%1」的收件箱").arg(App.selectedPeerName)
                          : App.uploadHint
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }
            }
        }
    }

    // ---- 托盘（M8）----
    // Qt.labs.platform 在没有 StatusNotifier 的 Linux / 精简系统上会加载失败，
    // 单独文件 + Loader 隔离：失败只丢托盘，主界面照常可用
    Loader {
        id: trayLoader
        active: true
        source: Qt.resolvedUrl("components/TrayIcon.qml")

        onStatusChanged: {
            if (status === Loader.Error)
                console.warn("系统托盘不可用，关闭窗口将直接退出");
            else if (status === Loader.Ready && item)
                App.setTrayAvailable(item.available);
        }
    }

    // 托盘图标加载失败是完全静默的：托盘照样「可用」，但什么都不显示。
    // 用一个不参与渲染的 Image 探同一份资源，拿不到就告警——不然这事从日志里看不出来
    Image {
        visible: false
        width: 0
        height: 0
        source: trayLoader.item ? trayLoader.item.icon.source : ""

        onStatusChanged: {
            if (status === Image.Error)
                console.warn("托盘图标加载失败（资源不在预期位置，或缺少 SVG 引擎）：",
                             source);
        }
    }

    Connections {
        target: trayLoader.item
        // 不开 ignoreUnknownSignals：信号名拼错的话这里会直接告警，
        // 否则托盘里那条「退出」点了没反应，是查不出来的

        function onAvailabilityChanged(ok) {
            App.setTrayAvailable(ok);
        }

        function onWindowRequested() {
            root.raiseWindow();
        }

        function onQuitRequested() {
            root.requestQuit();
        }
    }

    Connections {
        target: App

        function onNotify(title, body) {
            root.notifyUser(title, body);
        }

        function onTransferNoticeChanged() {
            if (App.transferNotice.length > 0)
                toast.show(App.transferNotice);
        }
    }

    Toast {
        id: toast
        z: 2000
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        // 别落进手势条区域：那块是系统接管的手势热区
        anchors.bottomMargin: Theme.gapL + root.safeBottom
    }
}

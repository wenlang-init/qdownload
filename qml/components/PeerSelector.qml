import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qdownload

/*
 * 主机下拉（设计文档 §8.2）。
 *
 * 数据来自 App.peers（PeerListModel）。手动同步 currentIndex 而不是绑定：
 * 模型在“上线/掉线”时会重排并 reset，绑定会被反复打断，选中项会跳。
 */
ComboBox {
    id: selector

    readonly property var peersModel: App.peers
    readonly property var currentPeerOnline: App.selectedPeerOnline
    readonly property string currentBadge: App.selectedPeerBadge
    readonly property string currentKind: App.selectedPeerKind

    model: peersModel
    textRole: "displayName"
    valueRole: "peerId"
    enabled: count > 0
    font.pixelSize: Theme.fontSmall
    implicitHeight: 32
    currentIndex: -1

    function syncIndex() {
        const id = App.selectedPeerId;
        for (let i = 0; i < count; ++i) {
            if (peersModel.peerIdAt(i) === id) {
                currentIndex = i;
                return;
            }
        }
        currentIndex = -1;
    }

    Component.onCompleted: syncIndex()
    onCountChanged: syncIndex()
    onModelChanged: syncIndex()

    Connections {
        target: App
        function onSelectedPeerChanged() { selector.syncIndex() }
    }

    onActivated: function (index) {
        App.selectedPeerId = peersModel.peerIdAt(index);
    }

    ToolTip.visible: hovered && count === 0
    ToolTip.text: App.discoveryOk
                 ? qsTr("组播发现中，3 秒一次心跳；对端没开共享也会出现在这里")
                 : qsTr("组播不可用：%1").arg(App.discoveryError)

    // Basic 样式默认的上下双三角辨识度差，换成单箭头
    indicator: Label {
        x: selector.width - width - 10
        y: (selector.height - height) / 2
        text: "⌄"
        color: Theme.textSecondary
        font.pixelSize: Theme.fontBody
        visible: selector.count > 0
    }

    contentItem: RowLayout {
        spacing: Theme.gapS

        Rectangle {
            Layout.preferredWidth: 8
            Layout.preferredHeight: 8
            radius: 4
            color: {
                if (selector.currentIndex < 0)
                    return Theme.textTertiary;
                if (!selector.currentPeerOnline)
                    return Theme.textTertiary;
                return selector.currentKind === "upload" ? Theme.success : Theme.accent;
            }
        }

        Label {
            Layout.fillWidth: true
            text: selector.currentIndex >= 0
                  ? (selector.width > 170 ? selector.displayText : App.selectedPeerName)
                  : qsTr("未发现主机")
            color: selector.currentIndex >= 0 ? Theme.textPrimary : Theme.textTertiary
            font.pixelSize: Theme.fontSmall
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
    }

    delegate: ItemDelegate {
        id: delegateItem

        required property string displayName
        required property string statusBadge
        required property string statusKind
        required property string subtitle
        required property bool online

        width: selector.width
        highlighted: selector.highlightedIndex === index

        contentItem: RowLayout {
            spacing: Theme.gapS

            Rectangle {
                Layout.preferredWidth: 8
                Layout.preferredHeight: 8
                radius: 4
                color: !delegateItem.online ? Theme.textTertiary
                     : delegateItem.statusKind === "upload" ? Theme.success
                     : delegateItem.statusKind === "readonly" ? Theme.accent
                     : Theme.warning
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0

                Label {
                    Layout.fillWidth: true
                    text: delegateItem.displayName
                    color: delegateItem.online ? Theme.textPrimary : Theme.textTertiary
                    font.pixelSize: Theme.fontSmall
                    elide: Text.ElideRight
                }

                Label {
                    Layout.fillWidth: true
                    text: delegateItem.subtitle
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontSmall - 1
                    elide: Text.ElideRight
                    visible: text.length > 0
                }
            }
        }
    }
}

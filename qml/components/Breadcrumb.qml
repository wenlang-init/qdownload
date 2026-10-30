import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qdownload

/*
 * 路径条（设计文档 §8.1）。
 *
 * segments 是 [{name, path}] 而非纯字符串：每一段都要能点回去，
 * 否则用户在深目录里只能一层层点"上级"。
 * 桌面显示完整路径；手机端横向滑动，并自动滚到最右端。
 */
Item {
    id: crumb

    property var segments: []
    signal navigate(string path)

    implicitHeight: 34

    Rectangle {
        anchors.fill: parent
        color: Theme.surfaceAlt
    }

    ListView {
        id: list
        anchors.fill: parent
        anchors.leftMargin: Theme.gapM
        anchors.rightMargin: Theme.gapM
        orientation: ListView.Horizontal
        clip: true
        model: crumb.segments
        boundsBehavior: Flickable.StopAtBounds
        spacing: 0

        delegate: Item {
            id: seg

            width: row.implicitWidth
            height: ListView.view.height

            readonly property bool isLast: index === crumb.segments.length - 1

            Row {
                id: row
                anchors.verticalCenter: parent.verticalCenter
                spacing: 0

                Label {
                    id: nameLabel
                    text: modelData.name
                    color: hover.containsMouse && !seg.isLast
                           ? Theme.accent
                           : (seg.isLast ? Theme.textPrimary : Theme.textSecondary)
                    font.pixelSize: Theme.fontSmall
                    font.weight: seg.isLast ? Font.Medium : Font.Normal
                    verticalAlignment: Text.AlignVCenter

                    MouseArea {
                        id: hover
                        anchors.fill: parent
                        anchors.margins: -4
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        enabled: !seg.isLast
                        onClicked: crumb.navigate(modelData.path)
                    }
                }

                Label {
                    text: " / "
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontSmall
                    visible: !seg.isLast
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }

        // 路径变深时自动滚到末尾（手机上当前目录在右侧，不滚过去就看不见）
        onCountChanged: if (count > 0) positionViewAtEnd()
    }
}

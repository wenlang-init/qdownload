import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qdownload

/*
 * 共享端：已配对设备（设计文档 §7.4 L3）。
 * 逐台切 ro/rw，或直接撤销授权。改动立即通过 PERM_UPDATE 推给对方。
 */
Dialog {
    id: dialog

    modal: true
    visible: Qt.application.arguments.indexOf("--demo-trusted") >= 0
    anchors.centerIn: parent
    width: Math.min(460, (parent ? parent.width : 460) - 2 * Theme.gapL)
    height: Math.min(420, (parent ? parent.height : 420) - 2 * Theme.gapL)
    padding: Theme.gapL

    background: Rectangle {
        color: Theme.surface
        radius: Theme.radius
        border.width: 1
        border.color: Theme.border
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.gapM

        Label {
            Layout.fillWidth: true
            text: qsTr("已配对设备")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontTitle
            font.weight: Font.Medium
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("这些设备可以浏览本机共享的文件。勾选「可写」并打开顶部写权限开关后，才能上传。")
            color: Theme.textTertiary
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.border
        }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: App.trustedDevices

            ScrollBar.vertical: ScrollBar {}

            delegate: Rectangle {
                id: row

                required property var modelData

                width: ListView.view.width
                height: 46
                color: "transparent"

                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: Theme.border
                    opacity: 0.6
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.gapS
                    anchors.rightMargin: Theme.gapS
                    spacing: Theme.gapS

                    Rectangle {
                        Layout.preferredWidth: 7
                        Layout.preferredHeight: 7
                        radius: 3.5
                        color: row.modelData.online ? Theme.success : Theme.textTertiary
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0

                        Label {
                            Layout.fillWidth: true
                            text: row.modelData.name
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontBody
                            elide: Text.ElideRight
                        }

                        Label {
                            Layout.fillWidth: true
                            text: row.modelData.online ? qsTr("在线") : qsTr("离线")
                            color: Theme.textTertiary
                            font.pixelSize: Theme.fontSmall - 1
                        }
                    }

                    Label {
                        text: qsTr("可写")
                        color: row.modelData.perm === "rw" ? Theme.textPrimary : Theme.textTertiary
                        font.pixelSize: Theme.fontSmall
                    }

                    Switch {
                        id: permSwitch
                        implicitWidth: 34
                        implicitHeight: 20
                        padding: 0
                        checked: row.modelData.perm === "rw"
                        onToggled: App.setTrustedPerm(row.modelData.peerId, checked ? "rw" : "ro")

                        indicator: Rectangle {
                            id: track
                            implicitWidth: 34
                            implicitHeight: 20
                            radius: 10
                            x: 0
                            y: (parent.height - height) / 2
                            color: permSwitch.checked ? Theme.accent
                                                      : (Theme.dark ? "#40403C" : "#CFCFC8")

                            Rectangle {
                                id: knob
                                y: 3
                                width: 14
                                height: 14
                                radius: 7
                                color: permSwitch.checked ? "#FFFFFF"
                                                          : (Theme.dark ? "#8C8C86" : "#FFFFFF")
                                x: permSwitch.checked ? track.width - knob.width - 3 : 3
                                Behavior on x {
                                    NumberAnimation { duration: 110; easing.type: Easing.OutCubic }
                                }
                            }
                        }

                        contentItem: Item {
                            implicitWidth: 0
                            implicitHeight: 0
                        }
                    }

                    ToolButton {
                        text: qsTr("撤销")
                        font.pixelSize: Theme.fontSmall
                        onClicked: App.revokeTrusted(row.modelData.peerId)
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: App.trustedDevices.length === 0
            text: qsTr("还没有设备配对过。对方发起配对、你在这台机器上点「允许」后，会出现在这里。")
            color: Theme.textTertiary
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true

            Label {
                Layout.fillWidth: true
                text: qsTr("控制端口 %1 · 信任库 %2").arg(App.ctrlPort).arg("trust.db")
                color: Theme.textTertiary
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideMiddle
            }

            Button {
                text: qsTr("关闭")
                onClicked: dialog.close()
            }
        }
    }
}

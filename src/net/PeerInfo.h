#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QMetaType>
#include <QString>

/**
 * 一台可见主机的快照（设计文档 §4）。
 *
 * 只描述“能力状态”，不含任何目录内容或文件名——组播报文是广播给全网的，
 * 泄露文件名等于泄露隐私。
 *
 * `lastSeen` 是本机收到最后一次心跳的本地时间，不采信对方的 `ts`：
 * 跨设备时钟不同步，用对方时间戳会算出负延迟甚至永远在线。
 */
struct PeerInfo
{
    QString peerId;                 // 长期不变，断点归属与信任关系都靠它
    QString name;                   // 主机显示名
    QString platform;               // 系统描述
    QString address;                // 组播报文的源 IP
    quint16 ctrlPort = 0;           // 控制通道端口（被占用时会 +1）
    bool    sharing = false;        // 对方是否开启共享
    bool    write = false;          // 对方是否接收上传（L1 总开关）
    int     uploadPolicy = 0;       // 对方 L2 策略：0 仅白名单 / 1 已配对皆可传
    QString version;                // 对方程序版本，用于兼容性提示
    QDateTime lastSeen;             // UTC
    bool    online = false;
    bool    manual = false;         // 手动添加的主机，不参与组播超时清理

    bool isValid() const { return !peerId.isEmpty(); }

    /// 组装组播心跳。字段与设计文档 §4 一致。
    QJsonObject toAnnounce() const;

    /// 从心跳解析，`fromAddress` 由 socket 提供（不采信报文里的自述 IP）。
    static PeerInfo fromAnnounce(const QJsonObject &obj, const QString &fromAddress);

    /// 下拉框显示用：`客厅PC（只读）` / `客厅PC（离线）`
    QString displayName() const;

    /// 短状态标签：可上传 / 只读 / 未共享 / 离线（跟着界面语言变）
    QString statusBadge() const;

    /// 与语言无关的状态键：upload / readonly / unshared / offline。
    /// 界面按它决定颜色与文案分支——拿译文去比大小写相当于让样式依赖译文，翻错一个字颜色就变了。
    QString statusKind() const;

    /// 副标题：`192.168.1.7:45455 · Windows 11`
    QString subtitle() const;
};

Q_DECLARE_METATYPE(PeerInfo)

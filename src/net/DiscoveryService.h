#pragma once

#include "PeerInfo.h"

#include <QHash>
#include <QNetworkInterface>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVector>

class QUdpSocket;
class QTimer;

/**
 * 组播发现（设计文档 §4）。
 *
 * - 组播组 239.255.42.99:45454，TTL=1（不跨路由，出不了内网）
 * - **按网卡逐条发**：老实现只调一次 writeDatagram 交给系统路由，笔记本上装了
 *   VMware / Hyper-V / VPN 时默认出接口经常不是连内网的那张，心跳就发丢了；
 *   现在对每张可用网卡各发一份（setsockopt(IP_MULTICAST_IF)，Qt 侧即
 *   QNetworkDatagram::setInterfaceIndex），收端靠 peerId 去重，多发不会重复出现
 * - 每 3 秒发一次 ANNOUNCE，10 秒收不到判离线，离线 60 秒后从列表移除
 * - 同一个 socket 既发又收；绑定用 ShareAddress|ReuseAddressHint，
 *   这样同一台机器可以跑多个实例（联调/演示必需），也允许系统里别的进程监听同端口
 * - 关闭共享时仍然广播，只是 `sharing=false`——别人要能看到你、知道你不可访问，
 *   否则下拉框里那台机器会“凭空消失”，用户以为是网络问题
 *
 * 本类不做任何 UI 决策，只维护 peers 表并向外发增量信号，由 PeerListModel 转成 QML 模型。
 */
class DiscoveryService : public QObject
{
    Q_OBJECT

public:
    static constexpr const char *kGroupAddress    = "239.255.42.99";
    static constexpr quint16     kDiscoveryPort   = 45454;
    static constexpr int         kAnnounceMs      = 3000;
    static constexpr int         kOfflineMs       = 10000;
    static constexpr int         kForgetAfterMs   = 60000;
    /// 网卡集合检查周期：换网络后能自动重新加入组播，15 秒足够快也不费电
    static constexpr int         kIfaceCheckMs    = 15000;

    explicit DiscoveryService(QObject *parent = nullptr);
    ~DiscoveryService() override;

    /// 启动监听与心跳。失败时返回 false，reason 带回可读原因（端口占用最常见）
    bool start(QString *reason = nullptr);
    void stop();
    bool isRunning() const;

    /// 设置本机身份（peerId / name / platform / version / ctrlPort）
    void setSelf(const PeerInfo &self);
    PeerInfo self() const { return m_self; }

    /// 共享状态变化时立即广播，不等下一个心跳周期
    void setStatus(bool sharing, bool writeEnabled, int uploadPolicy);

    QVector<PeerInfo> peers() const;
    int onlineCount() const;

    Q_INVOKABLE void announceNow();

signals:
    void peerUpserted(const PeerInfo &peer);
    void peerRemoved(const QString &peerId);
    void discoveryError(const QString &message);

private:
    void onReadyRead();
    void sendAnnounce();
    void sweep();
    /// 重新枚举网卡并补齐组播成员关系。force=true 时无视缓存重来（启动时用）
    void refreshInterfaces(bool force = false);
    /// 取（并缓存）某张网卡的专用发送 socket；没有 IPv4 或绑定失败返回 nullptr
    QUdpSocket *senderFor(const QNetworkInterface &iface);
    void dropStaleSenders();

    QUdpSocket *m_socket = nullptr;
    QTimer *m_announceTimer = nullptr;
    QTimer *m_sweepTimer = nullptr;
    QTimer *m_ifaceTimer = nullptr;

    QVector<QNetworkInterface> m_ifaces;   ///< 当前参与收发组播的网卡
    QSet<int> m_joined;                    ///< 已 join 的网卡 index（含 -1 默认接口）
    QStringList m_ifaceSignature;          ///< 网卡集合指纹，用于判断要不要重来
    QStringList m_lastVia;                 ///< 上一次成功发出的网卡名（变化时才打日志）
    QHash<int, QUdpSocket *> m_senders;    ///< 网卡 index → 专用发送 socket

    QHash<QString, PeerInfo> m_peers;
    PeerInfo m_self;
    bool m_running = false;
    bool m_sendErrorLogged = false;
    qint64 m_lastReplyMs = 0;
};

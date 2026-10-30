#include "DiscoveryService.h"

#include <QJsonDocument>
#include <QNetworkInterface>
#include <QTimer>
#include <QUdpSocket>

#include <algorithm>
#include <utility>

#ifdef Q_OS_ANDROID
#include <QJniObject>
// 见 SystemNotifier.cpp：QNativeInterface 属于 QtCore 的平台头，不是 QtGui。
#include <QtCore/qcoreapplication_platform.h>
#endif

namespace {

#ifdef Q_OS_ANDROID
// Android 会丢组播包，必须持有 WifiManager.MulticastLock（设计文档 §4 的“最常见坑”）。
// 锁必须在整个运行期持有，本地函数返回后会被 GC 回收，所以放在静态里。
QJniObject g_multicastLock;

void acquireMulticastLock()
{
    if (g_multicastLock.isValid())
        return;

    QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid()) {
        qWarning() << "拿不到 Android Context，组播可能收不到";
        return;
    }

    QJniObject wifi = context.callObjectMethod(
        "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;",
        QJniObject::fromString(QStringLiteral("wifi")).object<jstring>());
    if (!wifi.isValid()) {
        qWarning() << "getSystemService(wifi) 失败";
        return;
    }

    QJniObject lock = wifi.callObjectMethod(
        "createMulticastLock",
        "(Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;",
        QJniObject::fromString(QStringLiteral("qdownload")).object<jstring>());
    if (!lock.isValid()) {
        qWarning() << "createMulticastLock 失败";
        return;
    }

    lock.callMethod<void>("setReferenceCounted", "(Z)V", false);
    lock.callMethod<void>("acquire");
    g_multicastLock = lock;
    qInfo() << "已持有 Android MulticastLock";
}
#else
void acquireMulticastLock() {}
#endif

/// 可以收发组播的网卡。排除 IsUp/IsRunning/CanMulticast 不满足的，
/// 但不排除回环——同一台机器跑两个实例联调时，靠回环也能互相看到。
/// 多网卡重复副本的合并窗口：同一轮心跳经不同网卡到达的时间差远小于这个值
constexpr int kDuplicateWindowMs = 800;
QVector<QNetworkInterface> eligibleInterfaces()
{
    QVector<QNetworkInterface> result;
    const auto all = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : all) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp))
            continue;
        if (!(flags & QNetworkInterface::IsRunning))
            continue;
        if (!(flags & QNetworkInterface::CanMulticast))
            continue;
        result.append(iface);
    }
    return result;
}

/// 接口上有没有 IPv4 地址。组播组是 IPv4 的，没有 v4 地址的网卡 join 上去也发不出去
bool hasIpv4(const QNetworkInterface &iface)
{
    const auto entries = iface.addressEntries();
    for (const QNetworkAddressEntry &entry : entries) {
        if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol)
            return true;
    }
    return false;
}

/// 网卡集合指纹：用来判断“网卡变了没有”（插拔网线、连上 WiFi、VPN 起来）
QStringList interfaceSignature(const QVector<QNetworkInterface> &ifaces)
{
    QStringList sig;
    sig.reserve(ifaces.size());
    for (const QNetworkInterface &iface : ifaces)
        sig << QStringLiteral("%1:%2").arg(iface.index()).arg(iface.humanReadableName());
    sig.sort();
    return sig;
}

bool peerLessThan(const PeerInfo &a, const PeerInfo &b)
{
    if (a.online != b.online)
        return a.online;                                 // 在线的排前面
    if (a.sharing != b.sharing)
        return a.sharing;                                // 可访问的排前面
    return QString::localeAwareCompare(a.displayName(), b.displayName()) < 0;
}

} // namespace

DiscoveryService::DiscoveryService(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<PeerInfo>("PeerInfo");
}

DiscoveryService::~DiscoveryService()
{
    stop();
}

bool DiscoveryService::isRunning() const
{
    return m_running;
}

bool DiscoveryService::start(QString *reason)
{
    if (m_running)
        return true;

    if (!m_socket)
        m_socket = new QUdpSocket(this);

    // ShareAddress：允许同机多实例，也允许系统里别的程序占同端口（组播语义本应如此）
    if (!m_socket->bind(QHostAddress::AnyIPv4, kDiscoveryPort,
                        QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        const QString err = tr("组播端口 %1/udp 绑定失败：%2")
                                .arg(kDiscoveryPort)
                                .arg(m_socket->errorString());
        qWarning().noquote() << err;
        if (reason)
            *reason = err;
        emit discoveryError(err);
        return false;
    }

    // TTL=1：出不了内网，这是“组网内可见”的物理边界
    m_socket->setSocketOption(QAbstractSocket::MulticastTtlOption, 1);
    m_socket->setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);

    connect(m_socket, &QUdpSocket::readyRead, this, &DiscoveryService::onReadyRead, Qt::UniqueConnection);

    refreshInterfaces(true);
    if (m_joined.isEmpty()) {
        const QString err = tr("没有可用的组播网卡，只能靠手动添加 IP");
        qWarning().noquote() << err;
        if (reason)
            *reason = err;
        emit discoveryError(err);
        // 不 return false：某些环境（如容器）仍可能通过默认路由收到包
    }

    acquireMulticastLock();

    if (!m_announceTimer) {
        m_announceTimer = new QTimer(this);
        m_announceTimer->setInterval(kAnnounceMs);
        connect(m_announceTimer, &QTimer::timeout, this, &DiscoveryService::sendAnnounce);
    }
    if (!m_sweepTimer) {
        m_sweepTimer = new QTimer(this);
        m_sweepTimer->setInterval(1000);
        connect(m_sweepTimer, &QTimer::timeout, this, &DiscoveryService::sweep);
    }
    if (!m_ifaceTimer) {
        // 笔记本换网络（WiFi 切有线、VPN 连断）时网卡集合会变，不重新 join 就会“突然找不到设备”。
        // 15 秒一次枚举，代价可以忽略，反正只是比一下指纹
        m_ifaceTimer = new QTimer(this);
        m_ifaceTimer->setInterval(kIfaceCheckMs);
        connect(m_ifaceTimer, &QTimer::timeout, this, [this]() { refreshInterfaces(false); });
    }

    m_running = true;
    m_announceTimer->start();
    m_sweepTimer->start();
    m_ifaceTimer->start();
    announceNow();

    // 启动后补发两次：只靠 3 秒周期的话，另一台刚启动的设备最坏要等满一个周期才看到我们。
    // 这两发不改变常驻节奏，只是把“首次见面”从最坏 3 秒压到 1 秒内。
    QTimer::singleShot(300, this, &DiscoveryService::sendAnnounce);
    QTimer::singleShot(900, this, &DiscoveryService::sendAnnounce);

    qInfo().noquote() << QStringLiteral("发现服务已启动：组播 %1:%2，心跳 %3s，超时 %4s")
                             .arg(QString::fromLatin1(kGroupAddress))
                             .arg(kDiscoveryPort)
                             .arg(kAnnounceMs / 1000)
                             .arg(kOfflineMs / 1000);
    return true;
}

void DiscoveryService::stop()
{
    if (!m_running)
        return;
    m_running = false;
    if (m_announceTimer)
        m_announceTimer->stop();
    if (m_sweepTimer)
        m_sweepTimer->stop();
    if (m_ifaceTimer)
        m_ifaceTimer->stop();
    if (m_socket) {
        m_socket->close();
        disconnect(m_socket, nullptr, this, nullptr);
    }
    // 发送 socket 一并收掉：下次 start() 会按当时的网卡重新建
    for (auto it = m_senders.begin(); it != m_senders.end(); ++it) {
        it.value()->close();
        it.value()->deleteLater();
    }
    m_senders.clear();
    qInfo() << "发现服务已停止";
}

void DiscoveryService::refreshInterfaces(bool force)
{
    const QVector<QNetworkInterface> current = eligibleInterfaces();
    const QStringList signature = interfaceSignature(current);

    if (!force && signature == m_ifaceSignature)
        return;

    const QHostAddress group(QString::fromLatin1(kGroupAddress));

    // 组播是 IPv4 的：没有 v4 地址的网卡 join 上去也发不出去，直接跳过
    QVector<QNetworkInterface> usable;
    QStringList names;
    for (const QNetworkInterface &iface : current) {
        if (!hasIpv4(iface)) {
            qInfo().noquote() << QStringLiteral("网卡 %1 没有 IPv4 地址，跳过组播")
                                     .arg(iface.humanReadableName());
            continue;
        }
        usable.append(iface);
        names << iface.humanReadableName();
    }

    if (force)
        m_joined.clear();

    // 已经 join 过的重复 join 会报 Address already in use，所以按 index 记账，只补新出现的
    int joined = 0;
    for (const QNetworkInterface &iface : std::as_const(usable)) {
        if (m_joined.contains(iface.index())) {
            ++joined;
            continue;
        }
        if (m_socket->joinMulticastGroup(group, iface)) {
            m_joined.insert(iface.index());
            ++joined;
            qInfo().noquote() << QStringLiteral("组播已加入 %1 [%2]")
                                     .arg(iface.humanReadableName(), iface.hardwareAddress());
        } else {
            qWarning().noquote() << QStringLiteral("组播加入失败 %1：%2")
                                        .arg(iface.humanReadableName(), m_socket->errorString());
        }
    }

    // 已经消失的网卡要 leave：否则成员关系一直挂着，换了网段还往旧组里发
    QSet<int> alive;
    for (const QNetworkInterface &iface : std::as_const(usable))
        alive.insert(iface.index());

    const QSet<int> stale = m_joined - alive;
    for (int index : stale) {
        const QNetworkInterface iface = QNetworkInterface::interfaceFromIndex(index);
        if (iface.isValid()) {
            m_socket->leaveMulticastGroup(group, iface);
            qInfo().noquote() << QStringLiteral("网卡已离开组播 %1").arg(iface.humanReadableName());
        }
        m_joined.remove(index);
    }

    if (joined == 0 && m_socket->joinMulticastGroup(group)) {
        m_joined.insert(-1);   // -1 = 交给系统默认接口
        qInfo() << "组播已加入默认接口";
    }

    m_ifaces = usable;
    m_ifaceSignature = signature;
    dropStaleSenders();

    if (!names.isEmpty())
        qInfo().noquote() << QStringLiteral("组播网卡：%1").arg(names.join(QStringLiteral("、")));
}

void DiscoveryService::setSelf(const PeerInfo &self)
{
    m_self = self;
    m_self.online = true;
    m_self.lastSeen = QDateTime::currentDateTimeUtc();
}

void DiscoveryService::setStatus(bool sharing, bool writeEnabled, int uploadPolicy)
{
    m_self.sharing = sharing;
    m_self.write = writeEnabled;
    m_self.uploadPolicy = uploadPolicy;
    // 立即广播：否则最坏要等一个心跳周期，用户会以为开关没生效
    announceNow();
}

void DiscoveryService::announceNow()
{
    sendAnnounce();
}

QUdpSocket *DiscoveryService::senderFor(const QNetworkInterface &iface)
{
    const auto it = m_senders.constFind(iface.index());
    if (it != m_senders.cend())
        return it.value();

    QHostAddress local;
    const auto entries = iface.addressEntries();
    for (const QNetworkAddressEntry &entry : entries) {
        if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
            local = entry.ip();
            break;
        }
    }
    if (local.isNull())
        return nullptr;

    auto *socket = new QUdpSocket(this);
    // 绑到这张网卡的地址：绑定哪个地址，出接口就是哪张网卡。
    // 端口给 0（临时端口），它只发不收，不跟主 socket 抢 45454。
    if (!socket->bind(local, 0)) {
        qWarning().noquote() << QStringLiteral("网卡 %1 发送 socket 绑定失败（%2），跳过")
                                    .arg(iface.humanReadableName(), socket->errorString());
        delete socket;
        return nullptr;
    }
    socket->setSocketOption(QAbstractSocket::MulticastTtlOption, 1);
    socket->setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);

    m_senders.insert(iface.index(), socket);
    return socket;
}

void DiscoveryService::dropStaleSenders()
{
    QSet<int> alive;
    for (const QNetworkInterface &iface : std::as_const(m_ifaces))
        alive.insert(iface.index());

    for (auto it = m_senders.begin(); it != m_senders.end();) {
        if (alive.contains(it.key())) {
            ++it;
            continue;
        }
        it.value()->deleteLater();
        it = m_senders.erase(it);
    }
}

void DiscoveryService::sendAnnounce()
{
    if (!m_socket || !m_running || m_self.peerId.isEmpty())
        return;

    const QByteArray payload =
        QJsonDocument(m_self.toAnnounce()).toJson(QJsonDocument::Compact);
    const QHostAddress group(QString::fromLatin1(kGroupAddress));

    // 每张网卡各发一份。老实现只发一次交给系统路由，装了 VMware / Hyper-V / VPN 的机器上
    // 默认出接口经常不是连内网那张，心跳就悄悄发丢了。
    //
    // 这里没走 QNetworkDatagram::setInterfaceIndex：Qt 在 Windows 上调用成功但包实际
    // 发不出去（用独立监听端验证过，一份都收不到）。改成给每张网卡单独开一个 socket
    // 并 bind 到它的 IPv4 地址——绑定哪个地址，内核就用哪张网卡做出接口，可靠得多。
    int sent = 0;
    QStringList via;
    for (const QNetworkInterface &iface : std::as_const(m_ifaces)) {
        QUdpSocket *sender = senderFor(iface);
        if (!sender)
            continue;
        if (sender->writeDatagram(payload, group, kDiscoveryPort) == payload.size()) {
            ++sent;
            via << iface.humanReadableName();
        } else if (!m_sendErrorLogged) {
            m_sendErrorLogged = true;
            qWarning().noquote() << QStringLiteral("组播心跳经 %1 发送失败：%2")
                                        .arg(iface.humanReadableName(), sender->errorString());
        }
    }

    // 一个网卡都没发成功（或枚举为空）：退回“交给系统路由”，至少留一条路
    if (sent == 0 && m_socket->writeDatagram(payload, group, kDiscoveryPort) == payload.size()) {
        ++sent;
        via << QStringLiteral("默认路由");
    }

    if (sent == 0) {
        if (!m_sendErrorLogged) {
            m_sendErrorLogged = true;
            qWarning().noquote() << "组播心跳发送失败：" << m_socket->errorString();
        }
        return;
    }

    m_sendErrorLogged = false;
    // 每 3 秒一条会刷穿日志，只在出接口集合变化时记一行
    if (via != m_lastVia) {
        m_lastVia = via;
        qInfo().noquote() << QStringLiteral("组播心跳出接口：%1").arg(via.join(QStringLiteral("、")));
    }
}

void DiscoveryService::onReadyRead()
{
    if (!m_socket)
        return;

    while (m_socket->hasPendingDatagrams()) {
        const qint64 pending = m_socket->pendingDatagramSize();
        if (pending <= 0) {
            m_socket->readDatagram(nullptr, 0);
            continue;
        }

        QByteArray buffer;
        buffer.resize(int(pending));
        QHostAddress from;
        quint16 fromPort = 0;
        const qint64 read = m_socket->readDatagram(buffer.data(), buffer.size(), &from, &fromPort);
        if (read <= 0)
            continue;
        buffer.resize(int(read));

        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(buffer, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            qWarning().noquote() << "丢弃无法解析的组播包，来自" << from.toString() << parseError.errorString();
            continue;
        }

        const QJsonObject obj = doc.object();
        // 两个硬条件：本程序的包、心跳类型。别把同网段其它组播流量当主机
        if (obj.value(QStringLiteral("app")).toString() != QStringLiteral("qdownload"))
            continue;
        if (obj.value(QStringLiteral("t")).toString() != QStringLiteral("ANNOUNCE"))
            continue;

        PeerInfo peer = PeerInfo::fromAnnounce(obj, from.toString());
        if (!peer.isValid())
            continue;
        if (peer.peerId == m_self.peerId)
            continue;   // 自己的包（多网卡时每条都会回环一份）

        const bool isNew = !m_peers.contains(peer.peerId);
        const bool wasOffline = !isNew && !m_peers.value(peer.peerId).online;

        if (!isNew) {
            const PeerInfo prev = m_peers.value(peer.peerId);

            // 回环地址优先级最低：已经有真实网段地址时，不让经回环送来的副本把它盖回去，
            // 否则同一台机器上的另一个实例会让地址在 127.0.0.1 和 192.168.x.x 之间来回跳
            if (QHostAddress(peer.address).isLoopback()
                && !QHostAddress(prev.address).isLoopback()) {
                peer.address = prev.address;
            }

            // 一张网卡一份心跳，同一轮会在几十毫秒内到好几份。状态没变化就只刷新 lastSeen，
            // 不再往外发信号——否则 5 张网卡的机器每 3 秒能让对端刷 5 次列表
            const bool sameStatus = prev.name == peer.name
                                 && prev.platform == peer.platform
                                 && prev.version == peer.version
                                 && prev.ctrlPort == peer.ctrlPort
                                 && prev.sharing == peer.sharing
                                 && prev.write == peer.write
                                 && prev.uploadPolicy == peer.uploadPolicy
                                 && prev.online == peer.online;
            if (sameStatus && prev.online
                && prev.lastSeen.msecsTo(peer.lastSeen) < kDuplicateWindowMs) {
                m_peers.insert(peer.peerId, peer);
                continue;
            }
        }

        m_peers.insert(peer.peerId, peer);
        emit peerUpserted(peer);

        if (isNew) {
            qInfo().noquote() << QStringLiteral("发现主机 %1 (%2) %3:%4 sharing=%5 write=%6")
                                     .arg(peer.name, peer.peerId)
                                     .arg(peer.address)
                                     .arg(peer.ctrlPort)
                                     .arg(peer.sharing)
                                     .arg(peer.write);

            // 见到陌生主机就立刻回一发，让对方不用等下一个心跳周期才认得我们。
            // 限流 1 秒，避免多台设备同时上线时互相刷包。
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_lastReplyMs >= 1000) {
                m_lastReplyMs = now;
                sendAnnounce();
            }
        } else if (wasOffline) {
            qInfo().noquote() << QStringLiteral("主机恢复在线 %1 (%2)").arg(peer.name, peer.address);
        }
    }
}

void DiscoveryService::sweep()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QVector<PeerInfo> changed;
    QStringList forgotten;

    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) {
        PeerInfo &peer = it.value();
        if (peer.manual)
            continue;

        const qint64 age = peer.lastSeen.msecsTo(now);

        if (peer.online && age > kOfflineMs) {
            peer.online = false;
            changed.append(peer);
        } else if (!peer.online && age > kForgetAfterMs) {
            forgotten.append(it.key());
        }
    }

    // 先收集再发信号：不要在遍历 QHash 的过程中让外部代码有机会改表
    for (const PeerInfo &peer : changed) {
        qInfo().noquote() << QStringLiteral("主机离线 %1 (%2)").arg(peer.name, peer.address);
        emit peerUpserted(peer);
    }
    for (const QString &peerId : forgotten) {
        m_peers.remove(peerId);
        emit peerRemoved(peerId);
    }
}

QVector<PeerInfo> DiscoveryService::peers() const
{
    QVector<PeerInfo> list;
    list.reserve(m_peers.size());
    for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it)
        list.append(it.value());

    std::stable_sort(list.begin(), list.end(), peerLessThan);
    return list;
}

int DiscoveryService::onlineCount() const
{
    int count = 0;
    for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it) {
        if (it.value().online)
            ++count;
    }
    return count;
}

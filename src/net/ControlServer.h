#pragma once

#include "HostContext.h"

#include <QObject>
#include <QStringList>
#include <QVector>

class QTcpServer;
class PeerSession;

namespace Qdl { class ChecksumCache; }
class TrustStore;
class PairingService;
class DataServer;

/**
 * 共享端控制通道服务器（TCP，默认 45455，被占用时依次 +1）。
 *
 * 端口可能与默认值不同，所以实际端口必须回写给组播心跳（`ANNOUNCE.ctrlPort`），
 * 否则客户端会连到一个不存在的端口。
 */
class ControlServer : public QObject
{
    Q_OBJECT

public:
    ControlServer(TrustStore *trust, PairingService *pairing, QObject *parent = nullptr);
    ~ControlServer() override;

    bool start(quint16 preferredPort, QString *error = nullptr);
    void stop();
    bool isListening() const;
    quint16 port() const { return m_host.port; }

    /// 数据通道（下载/上传的裸流），随控制通道一起启停
    DataServer *dataServer() const { return m_data; }
    quint16 dataPort() const;
    /// 全量 SHA-256 的后台计算与缓存（M6 校验用）
    Qdl::ChecksumCache *checksums() const { return m_checksums; }

    void setHostIdentity(const QString &peerId, const QString &name, const QString &platform);
    void setShareRoot(const QString &root);
    void setShareState(bool shareEnabled, bool writeEnabled, int uploadPolicy);

    /// 写权限开关 / 策略 / 单设备权限变化后推给所有已连接设备
    void broadcastPermUpdate();
    void disconnectPeer(const QString &peerId);
    QStringList connectedPeers() const;

signals:
    void clientAuthenticated(const QString &peerId, const QString &name, const QString &perm);
    void clientDisconnected(const QString &peerId);
    void listeningPortChanged(quint16 port);
    void errorOccurred(const QString &message);

private:
    void onNewConnection();
    void onSessionClosed(PeerSession *session, const QString &reason);

    QTcpServer *m_server = nullptr;
    DataServer *m_data = nullptr;
    Qdl::ChecksumCache *m_checksums = nullptr;
    HostContext m_host;
    TrustStore *m_trust = nullptr;
    PairingService *m_pairing = nullptr;
    QVector<PeerSession *> m_sessions;
    bool m_newConnectionWired = false;
};

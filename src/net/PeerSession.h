#pragma once

#include "HostContext.h"
#include "MessageCodec.h"
#include "transfer/TransferTypes.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

class QTcpSocket;
class QTimer;

namespace Qdl { class ChecksumCache; }
class TrustStore;
class PairingService;
class DataServer;

/**
 * 服务端一条控制连接（设计文档 §5.2 / §7.3）。
 *
 * 状态机：WaitHello → WaitAuth → Authenticated
 *   - WaitHello 只接受 HELLO，10 秒内不发就断开（防半开连接堆积）
 *   - WaitAuth 接受 AUTH_REQ（令牌）或 PAIR_REQ（配对码）
 *   - Authenticated 才能进 LIST_REQ 等业务消息
 */
class PeerSession : public QObject
{
    Q_OBJECT

public:
    PeerSession(QTcpSocket *socket, const HostContext *host,
                TrustStore *trust, PairingService *pairing, DataServer *data,
                Qdl::ChecksumCache *checksums, QObject *parent = nullptr);

    QString peerId() const { return m_clientPeerId; }
    QString clientName() const { return m_clientName; }
    QString perm() const { return m_perm; }
    bool isAuthenticated() const { return m_authenticated; }

    void sendPermUpdate();
    void closeSession(const QString &reason);

signals:
    void authenticated(const QString &peerId, const QString &name, const QString &perm);
    void closed(PeerSession *session, const QString &reason);

private:
    enum class Stage { WaitHello, WaitAuth, Authenticated };

    void onReadyRead();
    void onDisconnected();
    void handleFrame(const Qdl::Frame &frame);
    void handleHello(const QJsonObject &payload);
    void handleAuthReq(const QJsonObject &payload);
    void handlePairReq(const QJsonObject &payload);
    void handlePairResp(const QJsonObject &payload);
    void handleListReq(const QJsonObject &payload);
    void handleDownloadReq(const QJsonObject &payload);
    void handleUploadReq(const QJsonObject &payload);
    /**
     * 领某一段的令牌（M12）。一次传输几十段，票若跟着表一次性发出去，
     * 慢链路上后面的票会在轮到它之前就过期；改成现领现用。
     */
    void handleSegReq(const QJsonObject &payload);
    void handleChecksumReq(const QJsonObject &payload);
    /// 对端把本次传输的整文件摘要推过来了（上传方向：发送端算好的源文件摘要）
    void handleChecksumPush(const QJsonObject &payload);
    /// 对端要删共享根里的一个文件（M14）：写权限 + 路径沙箱双重把关
    void handleDeleteReq(const QJsonObject &payload);
    void handleCancel(const QJsonObject &payload);
    void tryCompletePairing();
    void completePairing();

    void send(Qdl::MsgType type, const QJsonObject &payload);
    void sendError(int code, const QString &message, const QString &reqId = QString());
    QString remoteAddress() const;
    void resetIdleTimer();

    QTcpSocket *m_socket = nullptr;
    const HostContext *m_host = nullptr;
    TrustStore *m_trust = nullptr;
    PairingService *m_pairing = nullptr;
    DataServer *m_data = nullptr;
    Qdl::ChecksumCache *m_checksums = nullptr;
    QTimer *m_idleTimer = nullptr;

    QByteArray m_buffer;
    /// 正在等主机算完 SHA-256 的请求：reqId → fileKey
    QHash<QString, QString> m_pendingChecksums;

    /**
     * 一个进行中传输在主机侧的分段表。
     *
     * "记录下分段表"这件事必须留在主机：区间由它算，票由它发，客户端只是照着表
     * 一段段领活干。表在会话内存里，客户端断开就整表作废——数据通道的票也都是
     * 会话无关的短票，不需要跨会话恢复。
     */
    struct TransferPlan
    {
        bool upload = false;
        QString absolutePath;    // 下载：源文件；上传：半成品（.qdpart）路径
        QString finalPath;       // 上传：落地后的路径
        QString displayName;
        qint64 total = 0;
        qint64 chunkSize = 0;
        QVector<Qdl::ChunkPlan> plans;
        QVector<qint64> done;    // 各段已完成字节
        /// 下载：源文件整文件摘要（受理时就开算，算好随表/随推送下发）
        QString sha256;
        /// 上传：对端声明的源文件摘要，合并落地后拿来比对
        QString expectedSha256;
        /// 摘要缓存键：ChecksumCache 算完时用它认领是哪份传输
        QString checksumKey;
        QDateTime updated;
    };
    QHash<QString, TransferPlan> m_plans;

    /// 把 ChecksumCache::ready 接起来：既回老的 CHECKSUM_REQ，也给在跑的下载推摘要
    void wireChecksumReady();
    /// 把 DataServer::uploadFinished 接成 UPLOAD_DONE（数据通道是进程级的，按 transferId 过滤）
    void wireUploadFinished();
    /// 表只增不减会吃内存：太久没动静的传输在下次请求时顺手清掉
    void prunePlans();
    /// 本会话发起、还没回 UPLOAD_DONE 的上传（数据通道是进程级的，必须按会话过滤）
    QSet<QString> m_uploadIds;
    bool m_checksumWired = false;
    bool m_uploadWired = false;
    Stage m_stage = Stage::WaitHello;

    QString m_clientPeerId;
    QString m_clientName;
    QString m_clientPlatform;
    QString m_nonce;
    QString m_perm;
    bool m_authenticated = false;
    bool m_pairingInProgress = false;
    QString m_pairNonce;
};

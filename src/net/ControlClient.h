#pragma once

#include "MessageCodec.h"
#include "PeerInfo.h"
#include "TransferGrant.h"
#include "fs/FileEntry.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

class QTcpSocket;
class TrustStore;

/**
 * 客户端控制通道（设计文档 §7.3）。
 *
 * 状态机：
 *   Idle → Connecting → Handshaking ─┬→ Authenticating → Ready
 *                                    └→ Unpaired ──(用户点配对)──→ Pairing → Ready
 *                                    └→ (主机拒绝/码错/超时) → Unpaired
 *
 * 刻意**不**自动发 PAIR_REQ：一选主机就让对方弹配对窗属于骚扰，
 * 必须由用户明确点「配对」才发起。
 */
class ControlClient : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Idle,             // 未连接
        Connecting,       // TCP 连接中
        Handshaking,      // 已发 HELLO，等 HELLO_ACK
        Authenticating,   // 已发 AUTH_REQ，等结果
        Unpaired,         // 握手完成但没有有效令牌
        Pairing,          // 配对流程进行中（等主机放行）
        AwaitingPairCode, // 等用户输入对方屏幕上显示的 6 位码
        Ready,            // 已鉴权
        Failed            // 出错（网络/协议）
    };
    Q_ENUM(State)

    explicit ControlClient(TrustStore *trust, QObject *parent = nullptr);

    void setSelf(const QString &peerId, const QString &name, const QString &platform);
    void connectToHost(const PeerInfo &peer);
    void disconnectFromHost();

    /// 用户点「配对」：主动发 PAIR_REQ，让主机弹出配对窗
    void requestPairing();
    void submitPairCode(const QString &code);

    /**
     * 发起列目录请求，返回 reqId；返回空表示当前不可发（未鉴权 / 在途请求过多）。
     * 响应通过 listReceived / listFailed 回来，两者都带 path，便于界面区分是不是当前目录。
     */
    QString requestList(const QString &path, int offset, int limit);
    bool hasPendingList() const { return !m_pendingLists.isEmpty(); }

    /**
     * 申请下载。offset 是断点续传的起点（单段），多段时改由 segmentDone 描述每段进度。
     * segments 是希望切几段（1 = 不分段），最终段数由主机按文件大小裁决。
     * 返回 reqId；空串表示当前不可发（未鉴权 / 在途请求过多）。
     */
    QString requestDownload(const QString &path, qint64 offset,
                            const QString &fileKey = QString(), int segments = 1,
                            const QVector<qint64> &segmentDone = QVector<qint64>(),
                            qint64 chunkSize = 0);
    bool hasPendingDownload() const { return !m_pendingDownloads.isEmpty(); }

    /**
     * 领某一段的令牌（M12）。
     *
     * 一次传输可能有几十上百段，票若跟着段表一次性发出来，慢链路上排在后面的票
     * 会在轮到它之前就过期。改成空闲连接现领现用：一段一条连接，连上就发。
     * done 是客户端本地已完成的字节（下载用；上传以主机盘上的分片为准）。
     */
    QString requestSegment(const QString &transferId, int index, qint64 done = 0,
                           const QString &sha256 = QString());

    /**
     * 请求主机给出该文件的全量 SHA-256。
     *
     * M13 起这只是**兜底**：主机在受理下载时就把摘要算上了，随 DOWNLOAD_ACCEPT /
     * SEG_RESP 下发或直接 CHECKSUM_PUSH 推过来。走到这里说明推送没能到达
     * （老主机），所以仍然保留这条同步拉取通道。
     */
    QString requestChecksum(const QString &path);

    /**
     * 把本次传输的整文件摘要推给主机（上传方向：本端是发送端）。
     *
     * 主机落地后拿它比对收到的内容，不一致就直接把副本删掉并回失败。
     * 正常情况下摘要早在传输过程中就算好了，这条消息与"最后一个字节"无关。
     */
    void sendChecksumPush(const QString &transferId, const QString &sha256);

    /**
     * 申请上传（M7）。落点由主机决定（固定 `/inbox`），客户端只报文件名与大小。
     * segments 是希望切几段（1 = 不分段）。
     * 返回 reqId；空串表示当前不可发。
     */
    QString requestUpload(const QString &name, qint64 size, const QString &fileKey = QString(),
                          int segments = 1, qint64 chunkSize = 0);

    /// 取消一次传输。上传会让主机删掉 `.qdpart`；下载只需关数据连接，这里仅在已知 transferId 时发
    void cancelTransfer(const QString &transferId);

    /**
     * 让主机删掉共享根里的一个文件（M14）。
     *
     * 只在 `canUpload()`（主机开了写权限、本端是 rw）时允许；主机侧还会再判一次。
     * 返回 reqId；空串表示当前不可发。结果经 `deleteFinished` 回来。
     */
    QString requestDelete(const QString &path);

    State state() const { return m_state; }
    QString stateText() const;
    QString peerId() const { return m_peerId; }
    QString peerName() const { return m_peerName; }
    QString perm() const { return m_perm; }
    bool canUpload() const { return m_canUpload; }
    /// 主机当前是否开着共享（PERM_UPDATE 会推过来）
    bool peerSharing() const { return m_peerSharing; }
    bool isReady() const { return m_state == State::Ready; }
    bool isConnected() const;
    QString errorText() const { return m_errorText; }
    /// 主机地址（数据通道要连同一个 IP 的另一个端口）
    QString peerAddress() const { return m_peer.address; }

signals:
    void stateChanged();
    void pairCodePrompt();          // 服务端已给出 nonce，请 UI 打开输入框
    void paired();
    void pairFailed(const QString &message);   // 码错 / 过期 / 被拒 / 锁定
    void authRejected(const QString &message);
    void permChanged();

    void listReceived(const QString &path, int offset, int total,
                      const QVector<Qdl::FileEntry> &entries);
    void listFailed(const QString &path, int code, const QString &message);

    void downloadAccepted(const QString &reqId, const QString &path, const Qdl::DownloadGrant &grant);
    void downloadRejected(const QString &reqId, const QString &path, int code, const QString &message);
    void checksumReceived(const QString &reqId, const QString &path, const QString &sha256);
    void checksumFailed(const QString &reqId, const QString &path, const QString &message);
    /// 主机在传输过程中把摘要推过来了（M13）：不必等到收完再问
    void checksumPushed(const QString &transferId, const QString &sha256);

    void uploadAccepted(const QString &reqId, const Qdl::UploadGrant &grant);
    void uploadRejected(const QString &reqId, int code, const QString &message);

    /// 远端删除的结果（M14）：ok=false 时 message 是主机给的原因
    void deleteFinished(const QString &reqId, const QString &path, bool ok, const QString &message);

    /**
     * 领段的结果（M12）。ok=false 时 code 决定下一步：
     * 410 = 主机那边这份传输已经失效，只能重新发起；其余按普通错误处理。
     */
    void segmentGranted(const QString &reqId, bool ok, int index, const QString &token,
                        qint64 offset, qint64 length, qint64 done, bool complete,
                        int code, const QString &message);
    /**
     * 主机收完并算好摘要（M7）。
     *
     * ok=false 有两种性质完全不同的原因：中断（如主机关了写权限，可续传重试）
     * 与校验不一致（checksumMismatch=true，重试同样的内容只会再错一次）。
     */
    void uploadDone(const QString &transferId, bool ok, const QString &sha256,
                    const QString &message, bool checksumMismatch = false);

private:
    void onConnected();
    void onReadyRead();
    void onDisconnected();
    void onSocketError();
    void handleFrame(const Qdl::Frame &frame);
    void handleHelloAck(const QJsonObject &payload);
    void handleAuthResp(const QJsonObject &payload);
    void handlePairChallenge(const QJsonObject &payload);
    void handlePairOk(const QJsonObject &payload);
    void handlePairFail(const QJsonObject &payload);
    void handlePermUpdate(const QJsonObject &payload);
    void handleListResp(const QJsonObject &payload);
    void handleDownloadAccept(const QJsonObject &payload);
    void handleChecksumResp(const QJsonObject &payload);
    void handleChecksumPush(const QJsonObject &payload);
    void handleUploadAccept(const QJsonObject &payload);
    void handleSegResp(const QJsonObject &payload);
    void handleUploadDone(const QJsonObject &payload);
    void handleDeleteResp(const QJsonObject &payload);
    void handleError(const QJsonObject &payload);

    void send(Qdl::MsgType type, const QJsonObject &payload);
    void setState(State state);
    void fail(const QString &message);

    QTcpSocket *m_socket = nullptr;
    TrustStore *m_trust = nullptr;
    QByteArray m_buffer;

    QString m_selfPeerId;
    QString m_selfName;
    QString m_selfPlatform;

    PeerInfo m_peer;
    QString m_peerId;
    QString m_peerName;
    QString m_nonce;
    QString m_perm;
    bool m_canUpload = false;
    bool m_peerSharing = true;
    State m_state = State::Idle;
    QString m_errorText;

    struct PendingList
    {
        QString path;
        int offset = 0;
    };
    QHash<QString, PendingList> m_pendingLists;
    QHash<QString, QString> m_pendingDownloads;   // reqId → 虚拟路径
    QHash<QString, QString> m_pendingChecksums;   // reqId → 虚拟路径
    QHash<QString, QString> m_pendingUploads;     // reqId → 请求时的文件名
    QHash<QString, QString> m_pendingDeletes;     // reqId → 要删的虚拟路径
    int m_reqSeq = 0;
};

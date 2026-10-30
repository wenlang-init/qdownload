#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>

class QTcpServer;

namespace Qdl { class ChecksumCache; }

namespace Qdl {

/// 数据连接的两种用途。令牌里带着它，服务端据此决定是「吐字节」还是「吞字节」
enum class DataKind { Download, Upload };

/// 一次数据连接的授权：谁能读/写、读写哪个文件、从哪开始、什么时候作废
struct DataGrant
{
    DataKind kind = DataKind::Download;
    QString transferId;
    QString absolutePath;    // 下载：源文件；上传：单段半成品（多段时是分片所在的基础路径）
    QString finalPath;       // 上传：收满后改名的目标
    QString displayName;     // 上传：展示用的单段文件名
    qint64 offset = 0;       // 文件内起点（多段上传时是本次要写的位置）
    qint64 length = 0;       // 本段还要传的字节数
    qint64 segBase = 0;      // 本段在整份文件里的起点（分片文件从 0 长起，靠它换算）
    qint64 size = 0;         // 整个文件大小（仅用于日志与进度）
    int segIndex = 0;        // 多段上传：第几段（从 0 起）
    int segCount = 1;        // 多段上传：共几段
    QDateTime expires;
};

/**
 * 多段上传的聚合态：一个 transferId 下若干段写各自的分片文件，
 * 齐了才合并成 `.qdpart` 再改名落地。
 *
 * 为什么要分片而不是直接写同一个文件的对应偏移：分片文件的大小就是该段
 * **已经被文件系统确认过的**进度，断电重启后不需要任何额外的位图/日志就能接着传；
 * 直接写单文件的话，并行段之间会留空洞，"文件多大"就不再等于"收到多少"。
 */
struct UploadGroup
{
    QString transferId;
    QString partPath;
    QString finalPath;
    QString displayName;
    qint64 total = 0;
    int segCount = 0;
    QVector<qint64> lengths;
    QVector<qint64> done;
    QVector<bool> finished;   // 按段号记账：同段被重复上报不会把计数吹到"已到齐"
    int finishedCount = 0;
    bool aborted = false;
    bool reported = false;   // uploadFinished 只能发一次
};

} // namespace Qdl

/**
 * 共享端的数据通道（设计文档 §5.4）。
 *
 * 与文件内容**无关**：下载只做「验票 → 从 offset 起把字节吐出去」，上传只做
 * 「验票 → 从 offset 起把字节收进来」。文件路径、大小、权限判定全部在上游
 * （PeerSession 处理 DOWNLOAD_REQ / UPLOAD_REQ 时）完成，所以数据端口被同网段
 * 的第三方扫到也拿不到任何东西——没有一次性 token 连第一个字节都发不出去。
 *
 * 每条传输独立 TCP，连上先发 `"QDAT" + token(64B hex)`，之后就是裸流：
 * 流里不插任何控制字段，取消就是 close()，收到 EOF 即结束。
 */
class DataServer : public QObject
{
    Q_OBJECT

public:
    /// `"QDAT"` 后的 token 长度：randomToken() = 32 字节 hex = 64 字符
    static constexpr int kTokenLen = 64;
    static constexpr int kHeaderLen = 4 + kTokenLen;

    explicit DataServer(QObject *parent = nullptr);

    bool start(quint16 preferredPort = 45456, QString *error = nullptr);
    void stop();
    bool isListening() const;
    quint16 port() const { return m_port; }

    /// 全量 SHA-256 服务：兜底算摘要（正常情况下收到的字节是边收边算的）
    void setChecksums(Qdl::ChecksumCache *checksums) { m_checksums = checksums; }
    Qdl::ChecksumCache *checksums() const { return m_checksums; }

    /**
     * 发送端声明的源文件摘要（M13）。
     *
     * 上传方向由客户端在开工时后台算好，随段表（SEG_REQ）或 CHECKSUM_PUSH 送来；
     * 主机落地时拿它跟"自己边收边算出来的摘要"比对，不一致就把收到的副本删掉。
     * 摘要可能在收字节的过程中才到，所以这里是"随时可写、落地时读"。
     */
    void setExpectedChecksum(const QString &transferId, const QString &sha256);
    QString expectedChecksum(const QString &transferId) const
    {
        return m_expectedSums.value(transferId);
    }

    /**
     * 签发一次性令牌（下载）。返回空串表示当前不可用（未监听 / 文件已不可读）。
     * length 是本段要发的字节数——分段传输时一条连接只负责文件的一段，
     * 不再是"从 offset 一直发到文件尾"。
     */
    QString grant(const QString &transferId, const QString &absolutePath,
                  qint64 offset, qint64 length);

    /**
     * 签发一次性令牌（上传，单段）。partPath 是半成品落地路径，收满后改名为 finalPath。
     * `totalSize` 是**整个文件**的大小，不是本次要收的字节数。
     */
    QString grantUpload(const QString &transferId, const QString &partPath,
                        const QString &finalPath, const QString &displayName,
                        qint64 offset, qint64 totalSize);

    /**
     * 签发一段的上传令牌。segCount > 1 时每段写自己的分片文件
     * （`<partPath>.seg<i>`），全部到齐后由 DataServer 合并落地。
     */
    QString grantUploadSegment(const QString &transferId, int segIndex, int segCount,
                               const QString &partPath, const QString &finalPath,
                               const QString &displayName, qint64 offset, qint64 length,
                               qint64 totalSize,
                               const QVector<qint64> &segmentLengths = QVector<qint64>(),
                               qint64 segBase = 0);

    /// 分片路径：单段沿用 `<partPath>`，多段用 `<partPath>.seg<i>`
    static QString segmentFilePath(const QString &partPath, int segIndex, int segCount);

    /// 取走令牌（一次性：取到即删除）。数据连接握手成功后调用。
    bool consumeGrant(const QString &token, Qdl::DataGrant *out);

    /**
     * 主机把写权限拨到关：所有进行中的上传立即断开并**保留** .qdpart，
     * 语义等同暂停（设计文档 §7.4）。返回中断的条数。
     */
    int abortUploads(const QString &reason);

    /// 客户端取消上传：断开并删掉 .qdpart（取消不留垃圾）
    bool cancelUpload(const QString &transferId);

    int activeUploadCount() const { return m_uploadAborters.size(); }

    /**
     * 这个落点是否已有上传在写（含"已授权但还没连上来"的窗口期）。
     *
     * 光靠"文件是否已存在"去重是不够的：两个同名文件同时上传时，两边都会看到
     * 目标不存在而选中同一个名字，然后往同一个 `.qdpart` 里交错写——两份内容拼成
     * 一个坏文件。所以名字必须在授权阶段就"占位"。
     */
    bool isUploadPathBusy(const QString &finalPath) const;

    // ---- 以下这些只给 DataStreamer 用（C++ 里没有 private 友元匿名类的写法）----
    /// 注册「怎么被主机掐断」：keepPart=true 保留 .qdpart（等同暂停），false 删除。
    /// 返回 false 表示这次中断被忽略了（例如字节已收满、正在校验）
    void registerUpload(const QString &transferId,
                        std::function<bool(const QString &reason, bool keepPart)> aborter);
    void unregisterUpload(const QString &transferId);

    /// 同上，按段注册：多段上传时一条 transferId 会挂 N 个可中断点
    void registerUploadSegment(const QString &transferId, int segIndex,
                               std::function<bool(const QString &reason, bool keepPart)> aborter);
    void unregisterUploadSegment(const QString &transferId, int segIndex);

    /// 某段又收了 bytes 字节（用于把多段进度汇总成一条进度条）
    void noteUploadProgress(const QString &transferId, int segIndex, qint64 segmentDone);
    /// 某段收满：全部段齐了就合并落地
    void uploadSegmentFinished(const QString &transferId, int segIndex);
    /// 某段断了：整份上传转「中断」，其余段一起停（保留分片等续传）
    void uploadSegmentFailed(const QString &transferId, int segIndex, int segCount,
                             const QString &reason, bool keepPart);

    /**
     * 0 字节上传的落地：没有数据连接，也没有 `.qdpart` 要合并，
     * 落地动作就是"建一个空文件"，然后照常发 `uploadFinished`。
     * 和普通上传共用同一个出口，客户端那边不必为它开特例。
     */
    bool finishEmptyUpload(const QString &transferId, const QString &partPath,
                           const QString &finalPath, const QString &displayName);

signals:
    void errorOccurred(const QString &message);
    /// 一条数据连接结束（成功或失败），用于日志与统计
    void transferFinished(const QString &transferId, qint64 bytes, bool ok);

    // ---- 上传（M7）：给主机界面看的进度，也让 PeerSession 回 UPLOAD_DONE ----
    void uploadStarted(const QString &transferId, const QString &displayName,
                       const QString &finalPath, qint64 total);
    void uploadProgress(const QString &transferId, const QString &displayName,
                        qint64 received, qint64 total);
    /**
     * 一次上传的最终结果。
     *
     * ok=false 且 checksumMismatch=true：字节收齐了，但内容与发送端声明的摘要对不上
     * （副本已删除）。这跟"被中断/可续传"不是一回事——重试同样的内容只会再错一次。
     */
    void uploadFinished(const QString &transferId, const QString &finalPath,
                        const QString &displayName, qint64 received, bool ok,
                        const QString &sha256, const QString &message,
                        bool checksumMismatch = false);

private:
    void onNewConnection();
    void purgeExpired();

    /// 全部段到齐：合并分片（顺手算摘要）→ 校验大小 → 改名 → 比摘要 → 发 uploadFinished
    void finalizeUploadGroup(const QString &transferId);
    void failUploadGroup(const QString &transferId, const QString &reason, bool keepPart);
    void reportUploadGroup(const QString &transferId, bool ok, const QString &sha256,
                           const QString &message, bool checksumMismatch = false);
    /// 删掉某个 transferId 的所有分片（以及可选的 .qdpart）
    void removeGroupParts(const QString &transferId, bool keepPart);

    QTcpServer *m_server = nullptr;
    quint16 m_port = 0;
    QHash<QString, Qdl::DataGrant> m_grants;
    Qdl::ChecksumCache *m_checksums = nullptr;
    QHash<QString, std::function<bool(const QString &reason, bool keepPart)>> m_uploadAborters;
    QHash<QString, QString> m_reservedPaths;   // finalPath → transferId（上传占位）
    QHash<QString, QString> m_expectedSums;    // transferId → 发送端声明的源文件摘要
    QHash<QString, Qdl::UploadGroup> m_uploadGroups;   // 多段上传的聚合态
    QElapsedTimer m_uploadProgressTimer;               // 多段进度汇总的节流
};

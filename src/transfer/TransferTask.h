#pragma once

#include "TransferTypes.h"
#include "net/TransferGrant.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QVector>

class QCryptographicHash;
class QTcpSocket;
class QTimer;

namespace Qdl {

class TransferSegment;

/**
 * 一个下载任务（设计文档 §6.2、§6.7）。
 *
 * **分段表 + 空闲连接领活**（M12）：
 * 主机把文件切成若干段，下发一张表（每段区间 + 已完成字节）。客户端维持
 * `m_workersTarget` 条连接，哪条空下来就按顺序领下一个未完成的段，传完再领——
 * 而不是"几条连接各绑死一段"。这样先干完的连接不会空转，最后一段的拖尾也就
 * 只剩一段的时间；连接数还能在传输过程中随时加减（加立刻领新段，减则等手上
 * 这段干完不再领）。
 *
 * 落盘策略：每段写自己的分片 `<目标>.qdpart.seg<i>`（单段直接写 `.qdpart`），
 * 全部收满后拼回去、核对大小再改名。分片文件的大小天然就是每段的权威进度，
 * 跨进程续传不需要额外的位图。
 */
class TransferTask : public QObject
{
    Q_OBJECT

public:
    TransferTask(const QString &id, TransferDirection direction,
                 const QString &name, const QString &remotePath, const QString &localPath,
                 qint64 total, QObject *parent = nullptr);
    ~TransferTask() override;

    /// 开始 / 继续下载。段表由主机在 DOWNLOAD_ACCEPT 里下发，客户端照表领活
    bool start(const QString &host, quint16 port, const Qdl::DownloadGrant &grant);

    /**
     * 开始上传（M7）。主机在 UPLOAD_ACCEPT 里给出段表与每段已有字节；
     * 客户端从源文件的同一位置读起——以服务端为准，自己算的不作数。
     */
    bool startUpload(const QString &host, quint16 port, const Qdl::UploadGrant &grant);

    /**
     * 主机发来某一段的令牌（SEG_REQ 的应答）。
     *
     * complete=true 表示这一段其实已经传完了（主机盘上已经齐了），直接跳过。
     * 任务已暂停/取消时这里静默丢弃：票有时效，留着也没用。
     */
    void onSegmentLease(int index, const QString &token, qint64 offset, qint64 length,
                        qint64 done, bool complete);
    /// 领段失败（超时/传输失效）。这一段退回待领，换条连接再试
    void onSegmentDenied(int index, int code, const QString &message);

    /**
     * 运行中改连接数：加会立刻补领新段；减不打断在跑的段，等它干完自然不再领。
     * 这是"线程数可动态变化"的入口。
     */
    void setWorkerCount(int count);
    int workerCount() const { return m_workersTarget; }
    /// 当前真正在传（含正在领票）的连接数
    int activeWorkers() const { return m_activeWorkers; }

    /// 续传时上报给主机的每段进度（从本地分片大小读）
    QVector<qint64> segmentProgress() const;
    /// 上次使用的段数（从本地分片元信息读，没传过就是 1）
    int savedSegmentCount() const;
    /// 上次的段长（续传要沿用，否则切法变了盘上的分片就废了）
    qint64 savedChunkSize() const;
    /// 本次由主机下发的段数
    int segmentCount() const { return m_chunks.size(); }

    /// 主机侧的上传 ID：取消时要用它让主机删掉半成品
    void setTransferId(const QString &id) { m_transferId = id; }
    QString transferId() const { return m_transferId; }

    /**
     * 主机明确回了"没传完"（如中途关了写权限）。源文件不受影响，转 Paused，
     * 由用户决定何时重试——自动重试会把"用户不想收"的意图再撞一次。
     */
    void markUploadInterrupted(const QString &message);

    /**
     * 把上次进程留下的半成品接回内存（M6 跨进程续传）。
     * 只改状态、不碰网络：恢复出来的任务一律停在 Paused，等用户点「继续」才走。
     * 这一点是刻意的——应用一启动就自动往外拉数据，用户会以为自己在被偷带宽。
     */
    void restore(const QString &peerId, const QString &fileKey, qint64 done, qint64 total);

    /// 源文件已变化（服务端 409）：半成品作废，回到"可以重新下"的状态
    void resetToZero();

    /// 记住这份断点属于哪台主机（断点库按 peerId 归属，换主机就不能拿它续）
    void setPeer(const QString &peerId, const QString &peerName);
    /// 服务端下发的 fileKey：续传时原样带回去，让主机判断"还是不是同一个文件"
    void setFileKey(const QString &fileKey) { m_fileKey = fileKey; }
    /// 主机重新下发的总大小（源文件被改过后重下时，大小会变）
    void setTotal(qint64 total) { m_total = qMax<qint64>(0, total); }

    /**
     * 接收端拿到"随表传来的对端摘要"。
     *
     * 下载：主机在受理请求时就开始算，随 DOWNLOAD_ACCEPT / SEG_RESP 下发或
     * 单独 CHECKSUM_PUSH 推过来。可能在传输过程中到达，也可能在收尾瞬间才到，
     * 所以这里既存值也顺手推进一步（本地摘要已经好了就当场比对）。
     */
    void setExpectedChecksum(const QString &sha256);
    QString expectedChecksum() const { return m_expectedChecksum; }

    /**
     * 兜底也拿不到对端摘要（老主机 / 连接已断）：按"只校验了大小"收尾，但要留痕，
     * 不能让任务永远挂在"校验中"。
     */
    void checksumUnavailable(const QString &note);

    /**
     * 校验结果回来了。ok=false 会把已经落地的文件删掉：
     * 留一个大小对、内容错的文件，比明确失败更糟——用户会当成好文件用下去。
     */
    void confirmVerified(bool ok, const QString &message = QString());
    void pause();
    void cancel();

    /// 连数据通道之前就失败（申请被拒、磁盘满等）
    void reject(const QString &message);

    QString id() const { return m_id; }
    TransferDirection direction() const { return m_direction; }
    QString directionCode() const;
    QString name() const { return m_name; }
    QString remotePath() const { return m_remotePath; }
    QString peerId() const { return m_peerId; }
    QString peerName() const { return m_peerName; }
    QString fileKey() const { return m_fileKey; }
    /// 本地算出的全量 SHA-256（hex），校验中/完成后才有值
    QString checksum() const { return m_checksum; }
    QString verifyNote() const { return m_verifyNote; }
    QString localPath() const { return m_localPath; }
    QString partPath() const { return m_localPath + QStringLiteral(".qdpart"); }

    TransferState state() const { return m_state; }
    QString stateText() const { return TransferFmt::stateText(m_state); }
    QString stateCode() const { return TransferFmt::stateCode(m_state); }
    QString errorText() const { return m_error; }

    qint64 total() const { return m_total; }
    qint64 done() const { return m_done; }
    double progress() const;
    double speed() const { return m_speed; }

    QString sizeText() const;      // "12.3 MB / 100.0 MB"
    QString speedText() const;
    QString etaText() const;
    QString percentText() const;   // "32%"

    bool isActive() const;
    bool isFinished() const;
    bool canPause() const { return m_state == TransferState::Transferring || m_state == TransferState::Connecting; }
    bool canResume() const { return m_state == TransferState::Paused || m_state == TransferState::Failed; }
    bool canCancel() const { return !isFinished(); }

    /// 暂停/失败后还剩多少字节没拿（续传起点，也用于界面提示）
    qint64 remainBytes() const { return qMax<qint64>(0, m_total - m_done); }

    /**
     * 全局限速值被改了：重算接收窗口，并立刻 pump 一次。
     * 不这么做的话，调低限速后已经在路上的那几十毫秒还是老速率，
     * 调高限速则要等下一个周期才放开，界面上看着像"改了没反应"。
     */
    void refreshRateLimit();

signals:
    void changed();
    void finished(Qdl::TransferTask *task);
    /// 断点位置变了要落库（每 4MB 或进入暂停/中断时触发），供 ResumeStore 持久化
    void checkpoint(qint64 done);
    /**
     * 本端摘要算完了。
     *
     * 上传：源文件摘要是开工时后台算的，算完由外面推给主机（发送端 → 接收端）。
     * 下载：本地摘要算完，只等主机那份来做比对（正常早就在手上了）。
     */
    void checksumReady();
    /// 下载收尾时手上还没有主机摘要：请外面补拉一次（老主机的兼容通道）
    void checksumPullRequested(const QString &remotePath);
    /**
     * 需要某一段的令牌（M12）。AppController 转成 SEG_REQ 发给主机，
     * 拿到结果后回调 onSegmentLease / onSegmentDenied。
     */
    void segmentNeeded(const QString &taskId, int index, qint64 done);

private:
    /// 一段在客户端的状态机：待领 → 领票中 → 传输中 → 完成
    struct Chunk
    {
        enum State { Pending, Claiming, Running, Finished };
        int index = 0;
        qint64 offset = 0;   // 文件内起点
        qint64 length = 0;   // 本段总长
        qint64 done = 0;     // 已完成（多段时等于分片文件大小）
        State state = Pending;
        int retries = 0;
    };

    void buildChunks(const QVector<Qdl::SegmentGrant> &segs, qint64 fallbackOffset);
    /// 空闲连接领活：把待领的段分给空出来的 worker，直到达到目标连接数
    void pumpWorkers();
    /// 下一个还没人干的段；没有就返回 -1
    int nextPendingChunk() const;
    /// 建一条连接去搬这一段
    bool startChunk(int index, const QString &token, qint64 done);
    void finishChunk(int index, TransferSegment *seg);
    void onChunkFailed(int index, TransferSegment *seg, const QString &message);
    void onWorkerConnected();
    void ensureTicker();
    void abortWorkers();
    bool allChunksFinished() const;
    void maybeFinishAll();
    /// 全部收满后的收尾：下载合并分片，上传进入等确认
    void finishAllSegments();
    /// 从盘上刷新某一段的已完成字节（分片文件大小就是权威进度）
    void refreshChunkFromDisk(Chunk &chunk) const;

    void tick();
    /// 所有连接各推一次（限速/背压的兜底节拍）
    void pump();

    /// 多段下载收满：把 `.qdpart.seg<i>` 按顺序拼回 `.qdpart`，再删分片
    bool mergeSegments(QString *error);
    QString segmentFilePath(int index, int count) const;
    QString segMetaPath() const;

    void finishSending();

    void setState(TransferState state);
    void failWith(const QString &message);
    /// 连接中断：有进度就转 Paused（保留分片），一个字节没收到算失败
    void interrupt(const QString &message);
    void completeFile();

    /**
     * 校验流程（M13 重构）。三种摘要来源，优先级从高到低：
     *   1. 收字节/合并分片时**顺手算出来**的本地摘要（不额外读盘）；
     *   2. 单段续传这类没法边收边算的情形：收尾时按 4MB 一片读一遍（fallback）；
     *   3. 上传：开工时后台算源文件（与传输并行，收完时早就算好了）。
     * 摘要齐了以后跟"随表传来的对端摘要"比对，一致才算完成。
     */
    void startVerification();
    void verifyStep();
    void stopVerification();
    /// 本地摘要就绪：能比对就比对，比对不了就补拉 / 降级
    void noteLocalDigest(const QString &hex);
    void finalizeVerification();
    /// 上传：开工即后台算源文件摘要，与传输并行
    void startSourceHash();
    void ensureVerifyWaitTimer();
    void closeSocket();
    void stopTicker();
    void emitCheckpoint();

    QString m_id;
    TransferDirection m_direction = TransferDirection::Download;
    QString m_name;
    QString m_remotePath;
    QString m_localPath;
    QString m_peerId;           // 断点归属：换了主机就不能拿这份半成品去续
    QString m_peerName;
    QString m_fileKey;          // 续传身份指纹，随 DOWNLOAD_ACCEPT 下发/从库里恢复
    QString m_transferId;       // 主机侧传输 ID（上传取消时回传）

    TransferState m_state = TransferState::Queued;
    QString m_error;

    QVector<Chunk> m_chunks;              // 段表 + 每段状态
    QVector<TransferSegment *> m_live;    // 当前在跑的连接（数量 = 活跃 worker）
    int m_workersTarget = 3;              // 目标连接数，可运行中调整
    int m_activeWorkers = 0;              // 领票中 + 传输中
    QString m_host;                       // 数据通道地址（后续领段要重连）
    quint16 m_port = 0;
    /// 主机在 ACCEPT 里预发的第一张票：第一段直接用它，省一次 SEG_REQ 往返
    QString m_firstToken;
    bool m_pumping = false;               // pumpWorkers 重入保护
    bool m_finishHandled = false;         // 段全满后的收尾只做一次
    QFile m_file;                         // 单段下载时复用（多段由各段自己持有）
    QTimer *m_ticker = nullptr;

    qint64 m_total = 0;
    qint64 m_done = 0;
    qint64 m_lastSampleDone = 0;
    double m_speed = 0.0;
    QElapsedTimer m_sampleTimer;

    qint64 m_startOffset = 0;
    qint64 m_lastCheckpoint = 0;
    int m_stallTicks = 0;

    QString m_checksum;         // 本地全量 SHA-256（收字节/合并时算出，或兜底读盘算出）
    QString m_expectedChecksum; // 对端随表传来的整文件摘要
    QString m_verifyNote;       // "主机未提供校验值" 之类的降级说明
    QFile m_verifyFile;
    QCryptographicHash *m_verifyHash = nullptr;
    QTimer *m_verifyTimer = nullptr;
    QTimer *m_verifyWaitTimer = nullptr;   // 校验阶段的上限，避免卡在"校验中"
    QTimer *m_pullDelayTimer = nullptr;    // 收尾时给"摘要推送"留的宽限，过了才补拉
    QFutureWatcher<QString> *m_sourceHash = nullptr;   // 上传：源文件摘要的后台计算
    bool m_pullRequested = false;          // 已经补拉过一次主机摘要（不重复问）
    bool m_uploading = false;   // true = 本任务往主机写字节（方向由 m_direction 决定）
    bool m_finishing = false;   // 防止 disconnected 与错误信号重复收尾
    bool m_intentionalPause = false;
    int m_segTotalCount = 1;    // 本次的段数（多段下载时决定分片文件命名）
};

} // namespace Qdl

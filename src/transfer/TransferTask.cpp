#include "TransferTask.h"

#include "RateLimiter.h"
#include "TransferSegment.h"
#include "fs/FileScanner.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QTcpSocket>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <utility>

namespace {
constexpr int kTickMs = 300;            // 进度/速度刷新频率：再快对眼睛没意义，只会白烧 CPU
constexpr int kStallTicks = 20;         // 连续 6 秒没收到一个字节 → 判定中断
constexpr int kUploadConfirmMs = 60000; // 发完后等主机 UPLOAD_DONE 的上限
constexpr int kChecksumWaitMs = 30000;  // 收完后等主机摘要（补拉）的上限
/// 收尾时先等这么久再补拉：推送通常只比最后一个字节晚几毫秒，抢着发请求等于白跑一次
constexpr int kChecksumGraceMs = 500;
constexpr int kMergeChunk = 1024 * 1024;
/// 同一段连续失败几次就放弃整任务：换连接能解决"这条连接坏了"，
/// 解决不了"这台主机/这块盘坏了"，无限重试只会让任务永远卡在传输中
constexpr int kMaxChunkRetries = 3;

QString segMetaPathOf(const QString &partPath)
{
    return partPath + QStringLiteral(".qseg");
}

bool readSegMeta(const QString &partPath, qint64 *total, int *count, qint64 *chunkSize)
{
    QFile file(segMetaPathOf(partPath));
    if (!file.open(QIODevice::ReadOnly))
        return false;

    const QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
    const int n = obj.value(QStringLiteral("count")).toInt(0);
    if (n <= 0)
        return false;

    if (total)
        *total = qint64(obj.value(QStringLiteral("total")).toDouble(0));
    if (count)
        *count = n;
    if (chunkSize)
        *chunkSize = qint64(obj.value(QStringLiteral("chunkSize")).toDouble(0));
    return true;
}

void writeSegMeta(const QString &partPath, qint64 total, int count, qint64 chunkSize)
{
    QFile file(segMetaPathOf(partPath));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;

    QJsonObject obj;
    obj.insert(QStringLiteral("total"), double(total));
    obj.insert(QStringLiteral("count"), count);
    obj.insert(QStringLiteral("chunkSize"), double(chunkSize));
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}
} // namespace

namespace Qdl {

TransferTask::TransferTask(const QString &id, TransferDirection direction, const QString &name,
                           const QString &remotePath, const QString &localPath, qint64 total,
                           QObject *parent)
    : QObject(parent)
    , m_id(id)
    , m_direction(direction)
    , m_name(name)
    , m_remotePath(remotePath)
    , m_localPath(localPath)
    , m_total(qMax<qint64>(0, total))
{
}

TransferTask::~TransferTask()
{
    // 任务被移除（清空已完成）时不能留着半开连接
    abortWorkers();
}

QString TransferTask::directionCode() const
{
    return m_direction == TransferDirection::Download ? QStringLiteral("download")
                                                      : QStringLiteral("upload");
}

double TransferTask::progress() const
{
    if (m_total <= 0)
        return m_state == TransferState::Completed ? 1.0 : 0.0;
    return qBound(0.0, double(m_done) / double(m_total), 1.0);
}

QString TransferTask::sizeText() const
{
    return QStringLiteral("%1 / %2").arg(TransferFmt::sizeText(m_done),
                                         TransferFmt::sizeText(m_total));
}

QString TransferTask::speedText() const
{
    return TransferFmt::speedText(m_speed);
}

QString TransferTask::etaText() const
{
    return TransferFmt::etaText(remainBytes(), m_speed);
}

QString TransferTask::percentText() const
{
    return QStringLiteral("%1%").arg(int(progress() * 100.0 + 0.5));
}

bool TransferTask::isActive() const
{
    return m_state == TransferState::Queued || m_state == TransferState::Connecting
        || m_state == TransferState::Transferring || m_state == TransferState::Verifying;
}

bool TransferTask::isFinished() const
{
    return m_state == TransferState::Completed || m_state == TransferState::Failed
        || m_state == TransferState::Canceled;
}

// ------------------------------ 分片文件 ------------------------------

QString TransferTask::segMetaPath() const
{
    return partPath() + QStringLiteral(".qseg");
}

QString TransferTask::segmentFilePath(int index, int count) const
{
    if (count <= 1)
        return partPath();
    return partPath() + QStringLiteral(".seg%1").arg(index);
}

int TransferTask::savedSegmentCount() const
{
    qint64 total = 0;
    int count = 0;
    if (!readSegMeta(partPath(), &total, &count, nullptr))
        return 1;
    return qBound(1, count, Qdl::kMaxChunks);
}

qint64 TransferTask::savedChunkSize() const
{
    qint64 chunkSize = 0;
    if (readSegMeta(partPath(), nullptr, nullptr, &chunkSize) && chunkSize > 0)
        return chunkSize;

    // 单段半成品：必须把"一段装下整个文件"这个切法固定下来，否则下次按新的
    // 连接数重切，盘上那半截就对不上任何一段，只能从头再来
    const QFileInfo part(partPath());
    if (part.exists() && part.size() > 0 && m_total > 0)
        return m_total;
    return 0;
}

QVector<qint64> TransferTask::segmentProgress() const
{
    const int count = savedSegmentCount();
    QVector<qint64> out;
    out.fill(0, count);

    if (count <= 1) {
        const QFileInfo info(partPath());
        out[0] = info.exists() ? info.size() : 0;
        return out;
    }

    for (int i = 0; i < count; ++i) {
        const QFileInfo info(segmentFilePath(i, count));
        out[i] = info.exists() ? info.size() : 0;
    }
    return out;
}

void TransferTask::refreshChunkFromDisk(Chunk &chunk) const
{
    const QFileInfo info(segmentFilePath(chunk.index, m_chunks.size()));
    qint64 size = info.exists() ? info.size() : 0;
    if (size > chunk.length) {
        // 分片比这一段本身还大：只可能是上次用了别的切法，删掉重来
        QFile::remove(info.absoluteFilePath());
        size = 0;
    }
    chunk.done = size;
    chunk.state = (chunk.length > 0 && size >= chunk.length) ? Chunk::Finished : Chunk::Pending;
}

void TransferTask::abortWorkers()
{
    const auto live = m_live;
    m_live.clear();

    for (TransferSegment *seg : live) {
        const int idx = seg->index();
        seg->abort();   // 主动停：不再发信号，所以下面从对象上把真实进度抄回来
        if (idx >= 0 && idx < m_chunks.size() && m_chunks[idx].state == Chunk::Running) {
            m_chunks[idx].done = qBound<qint64>(0, seg->done(), m_chunks[idx].length);
            m_chunks[idx].state = Chunk::Pending;
        }
        seg->deleteLater();
    }

    for (Chunk &c : m_chunks) {
        if (c.state == Chunk::Claiming || c.state == Chunk::Running)
            c.state = Chunk::Pending;
    }
    m_activeWorkers = 0;
}

bool TransferTask::allChunksFinished() const
{
    if (m_chunks.isEmpty())
        return true;
    for (const Chunk &c : m_chunks) {
        if (c.state != Chunk::Finished)
            return false;
    }
    return true;
}

int TransferTask::nextPendingChunk() const
{
    // 顺序领：段号天然就是文件里的先后顺序，顺序读对磁盘与预读都友好。
    // 真正的负载均衡靠"谁先空谁先领"，不靠挑肥拣瘦。
    for (int i = 0; i < m_chunks.size(); ++i) {
        if (m_chunks[i].state == Chunk::Pending)
            return i;
    }
    return -1;
}

void TransferTask::setWorkerCount(int count)
{
    const int clamped = qBound(1, count, 8);
    if (clamped == m_workersTarget)
        return;

    m_workersTarget = clamped;
    // 加：立刻补领新段；减：不打断在跑的段，等它干完自然不再领
    if (clamped > m_activeWorkers)
        pumpWorkers();
    emit changed();
}

// ------------------------------ 段表与调度 ------------------------------

void TransferTask::buildChunks(const QVector<Qdl::SegmentGrant> &segs, qint64 fallbackOffset)
{
    m_chunks.clear();

    if (segs.isEmpty()) {
        // 老主机（或 0 字节文件）：退化成一整段，行为与 M11 完全一致
        Chunk c;
        c.index = 0;
        c.offset = fallbackOffset;
        c.length = qMax<qint64>(0, m_total - fallbackOffset);
        m_chunks.append(c);
        return;
    }

    m_chunks.reserve(segs.size());
    for (int i = 0; i < segs.size(); ++i) {
        Chunk c;
        c.index = i;                          // 表里的顺序就是分片文件的编号
        c.offset = segs[i].offset;
        c.length = segs[i].length;
        c.done = qBound<qint64>(0, segs[i].done, segs[i].length);
        c.state = Chunk::Pending;
        m_chunks.append(c);
    }

    // 区间必须首尾相接且不越过文件尾；主机算错时宁可当场失败，也不写出一份坏文件
    qint64 expect = m_chunks.isEmpty() ? 0 : m_chunks.first().offset;
    for (const Chunk &c : std::as_const(m_chunks)) {
        if (c.offset != expect || c.offset + c.length > m_total) {
            qWarning().noquote() << QStringLiteral("段表不连续（第 %1 段：%2 + %3，期望起点 %4）")
                                        .arg(c.index)
                                        .arg(c.offset)
                                        .arg(c.length)
                                        .arg(expect);
            break;
        }
        expect = c.offset + c.length;
    }
}

void TransferTask::pumpWorkers()
{
    if (m_pumping)
        return;
    if (isFinished())
        return;
    if (m_state != TransferState::Connecting && m_state != TransferState::Transferring)
        return;   // 暂停/取消后不再领活
    if (m_finishHandled)
        return;

    m_pumping = true;

    while (m_activeWorkers < m_workersTarget) {
        if (isFinished())
            break;
        const int idx = nextPendingChunk();
        if (idx < 0)
            break;

        m_chunks[idx].state = Chunk::Claiming;
        ++m_activeWorkers;

        if (!m_firstToken.isEmpty() && idx == 0) {
            // 主机在 ACCEPT 里预发的那张票正好是第一段：直接开干，省一次往返
            const QString token = m_firstToken;
            m_firstToken.clear();
            startChunk(idx, token, m_chunks[idx].done);
        } else {
            // 其余段现领现用：票只有 5 分钟寿命，提前发出来慢链路上就作废了
            emit segmentNeeded(m_id, idx, m_chunks[idx].done);
        }
    }

    m_pumping = false;

    maybeFinishAll();
}

void TransferTask::maybeFinishAll()
{
    if (m_finishHandled || m_activeWorkers > 0)
        return;
    if (!allChunksFinished())
        return;

    m_finishHandled = true;
    finishAllSegments();
}

bool TransferTask::startChunk(int index, const QString &token, qint64 done)
{
    if (index < 0 || index >= m_chunks.size() || token.isEmpty())
        return false;

    Chunk &c = m_chunks[index];
    const bool multi = m_chunks.size() > 1;

    auto *seg = new TransferSegment(index, m_direction, c.offset, c.length, done,
                                    multi ? 0 : c.offset, this);

    // 单段且从 0 开始：这条连接收到的字节顺序 = 文件顺序，摘要可以边收边算，
    // 收满即得（M13）。多段是并行乱序、续传的前半段又不在本次连接里，
    // 两种情形分别由"合并时算"和"收尾读一遍"兜住。
    if (!m_uploading && m_chunks.size() == 1 && c.offset == 0 && done == 0) {
        seg->enableInlineDigest();
        qInfo().noquote() << QStringLiteral("%1：边收边算摘要（单段，从 0 开始）").arg(m_name);
    }

    QString openError;
    const QIODevice::OpenMode mode = m_uploading ? QIODevice::ReadOnly : QIODevice::ReadWrite;
    // 下载写的是本段的分片文件；上传读的是**源文件**，跟段号无关
    const QString path = m_uploading ? m_localPath
                                     : segmentFilePath(index, m_chunks.size());
    if (!seg->openFile(path, mode, &openError)) {
        seg->deleteLater();
        if (m_activeWorkers > 0)
            --m_activeWorkers;
        c.state = Chunk::Pending;
        if (++c.retries > kMaxChunkRetries) {
            failWith(m_uploading ? tr("无法读取本地文件：%1").arg(openError)
                                 : tr("无法创建临时文件：%1").arg(openError));
        } else {
            pumpWorkers();
        }
        return false;
    }

    connect(seg, &TransferSegment::progressed, this, [this, index](qint64 delta) {
        if (index >= 0 && index < m_chunks.size())
            m_chunks[index].done += delta;
        m_done += delta;
        m_stallTicks = 0;
    });
    connect(seg, &TransferSegment::connected, this, &TransferTask::onWorkerConnected);
    connect(seg, &TransferSegment::segmentFinished, this, [this, index, seg]() {
        finishChunk(index, seg);
    });
    connect(seg, &TransferSegment::failed, this, [this, index, seg](const QString &message) {
        onChunkFailed(index, seg, message);
    });

    m_live.append(seg);
    c.state = Chunk::Running;
    seg->connectTo(m_host, m_port, token);
    return true;
}

void TransferTask::finishChunk(int index, TransferSegment *seg)
{
    if (index >= 0 && index < m_chunks.size()) {
        Chunk &c = m_chunks[index];
        c.done = c.length;
        c.state = Chunk::Finished;
        c.retries = 0;
    }

    // 边收边算的那份摘要：在这一段收满时就是整份文件的摘要
    if (!m_uploading && seg->inlineDigestEnabled() && !seg->digestHex().isEmpty()) {
        m_checksum = seg->digestHex();
        qInfo().noquote() << QStringLiteral("摘要已随接收算完：%1 %2")
                                 .arg(m_name, m_checksum.left(16));
    }

    if (m_live.removeOne(seg))
        seg->deleteLater();
    if (m_activeWorkers > 0)
        --m_activeWorkers;

    // 这条连接空出来了：立刻去领下一段干（这就是"空闲线程从表里取余下的"）
    pumpWorkers();
}

void TransferTask::onChunkFailed(int index, TransferSegment *seg, const QString &message)
{
    if (m_live.removeOne(seg))
        seg->deleteLater();
    if (m_activeWorkers > 0)
        --m_activeWorkers;

    if (m_finishing || m_intentionalPause)
        return;

    bool giveUp = true;
    if (index >= 0 && index < m_chunks.size()) {
        Chunk &c = m_chunks[index];
        // 断了也要把已经落盘的部分记下来，否则续传会从这段的起点重来
        c.done = qBound<qint64>(0, seg->done(), c.length);
        c.state = Chunk::Pending;
        giveUp = (++c.retries > kMaxChunkRetries);
    }

    if (giveUp) {
        // 换连接能解决"这条链路坏了"，解决不了"主机/磁盘坏了"。
        // 到这里就整体停下来：有进度转暂停（可继续），没进度判失败。
        abortWorkers();
        stopTicker();
        interrupt(message);
        return;
    }

    qWarning().noquote() << QStringLiteral("第 %1 段失败，换条连接重试：%2")
                                .arg(index + 1)
                                .arg(message);
    pumpWorkers();
}

void TransferTask::onSegmentLease(int index, const QString &token, qint64 offset, qint64 length,
                                  qint64 done, bool complete)
{
    Q_UNUSED(offset)
    Q_UNUSED(length)

    if (index < 0 || index >= m_chunks.size())
        return;

    Chunk &c = m_chunks[index];
    if (c.state != Chunk::Claiming)
        return;   // 已经暂停/取消了，或者这是迟到的应答
    if (m_state != TransferState::Connecting && m_state != TransferState::Transferring)
        return;

    if (complete) {
        // 主机那边这一段其实已经齐了，直接跳过
        c.done = c.length;
        c.state = Chunk::Finished;
        if (m_activeWorkers > 0)
            --m_activeWorkers;
        pumpWorkers();
        return;
    }

    // 以主机给的 done 为准（上传时它读的是自己盘上的分片大小）
    c.done = qBound<qint64>(0, done, c.length);
    if (!startChunk(index, token, c.done)) {
        // startChunk 里已经退了 activeWorkers 并重排
        return;
    }
}

void TransferTask::onSegmentDenied(int index, int code, const QString &message)
{
    if (index < 0 || index >= m_chunks.size())
        return;

    Chunk &c = m_chunks[index];
    if (c.state != Chunk::Claiming)
        return;

    c.state = Chunk::Pending;
    if (m_activeWorkers > 0)
        --m_activeWorkers;

    if (m_finishing || m_intentionalPause)
        return;

    if (code == 410) {
        // 主机那边的分段表已经没了（重启/超时清理）：只能重新发起一次
        abortWorkers();
        stopTicker();
        m_intentionalPause = true;
        m_error = message.isEmpty() ? tr("主机已丢弃这次传输，请重新开始") : message;
        emitCheckpoint();
        setState(TransferState::Paused);
        emit finished(this);
        return;
    }

    if (++c.retries > kMaxChunkRetries) {
        abortWorkers();
        stopTicker();
        interrupt(message.isEmpty() ? tr("领取分段失败") : message);
        return;
    }

    pumpWorkers();
}

void TransferTask::onWorkerConnected()
{
    if (m_state == TransferState::Connecting) {
        m_sampleTimer.start();
        m_lastSampleDone = m_done;
        setState(TransferState::Transferring);
    }
    ensureTicker();
}

void TransferTask::ensureTicker()
{
    if (!m_ticker) {
        m_ticker = new QTimer(this);
        m_ticker->setInterval(kTickMs);
        connect(m_ticker, &QTimer::timeout, this, &TransferTask::tick);
    }
    if (!m_ticker->isActive())
        m_ticker->start();
}

// ------------------------------ 下载 ------------------------------

bool TransferTask::start(const QString &host, quint16 port, const Qdl::DownloadGrant &grant)
{
    if (host.isEmpty() || port == 0) {
        failWith(tr("下载参数不完整，无法建立数据连接"));
        return false;
    }

    abortWorkers();
    m_error.clear();
    m_checksum.clear();
    m_expectedChecksum = grant.sha256;   // 主机随表下发的整文件摘要（可能为空）
    m_pullRequested = false;
    m_uploading = false;
    m_finishing = false;
    m_intentionalPause = false;
    m_finishHandled = false;
    m_speed = 0.0;
    m_stallTicks = 0;
    m_done = 0;
    m_host = host;
    m_port = port;
    m_firstToken = grant.token;
    if (grant.size > 0)
        m_total = grant.size;
    if (!grant.fileKey.isEmpty())
        m_fileKey = grant.fileKey;
    if (!grant.transferId.isEmpty())
        m_transferId = grant.transferId;

    buildChunks(grant.segments, grant.offset);

    const int count = m_chunks.size();
    m_segTotalCount = qMax(1, count);

    // 段表与盘上的分片对不上（主机按新切法重新算过）：旧分片作废，不能混着拼。
    // 段长在续传时已经由主机沿用，走到这里说明总大小或段长真的变了。
    {
        qint64 metaTotal = 0;
        int metaCount = 0;
        const bool hasMeta = readSegMeta(partPath(), &metaTotal, &metaCount, nullptr);
        if (hasMeta && (metaCount != count || metaTotal != m_total)) {
            qWarning().noquote() << QStringLiteral("段表变化（%1 段 / %2 字节 → %3 段 / %4 字节），丢弃旧分片：%5")
                                        .arg(metaCount)
                                        .arg(metaTotal)
                                        .arg(count)
                                        .arg(m_total)
                                        .arg(m_name);
            const int stale = qMax(metaCount, count);
            for (int i = 0; i < stale; ++i)
                QFile::remove(segmentFilePath(i, stale));
            QFile::remove(segMetaPath());
        } else if (hasMeta && count <= 1) {
            // 上次多段、这次单段：多出来的分片留着只会被误当成进度
            for (int i = 0; i < metaCount; ++i)
                QFile::remove(segmentFilePath(i, metaCount));
            QFile::remove(segMetaPath());
        }
    }

    // 逐段对齐本地分片：分片文件有多大就是收到多少，客户端是这份文件唯一的作者
    qint64 initialDone = 0;
    for (int i = 0; i < count; ++i) {
        Chunk &c = m_chunks[i];
        c.index = i;

        if (count > 1) {
            refreshChunkFromDisk(c);
        } else {
            const QFileInfo partInfo(partPath());
            qint64 exist = partInfo.exists() ? partInfo.size() : 0;
            if (exist > c.length) {
                QFile::remove(partPath());   // 比整份文件还大：只可能是别的文件残留
                exist = 0;
            }
            c.done = exist;
            c.state = (c.length > 0 && exist >= c.length) ? Chunk::Finished : Chunk::Pending;
        }
        initialDone += c.done;
    }

    m_done = initialDone;
    m_lastSampleDone = m_done;
    m_lastCheckpoint = m_done;

    if (count > 1)
        writeSegMeta(partPath(), m_total, count, grant.chunkSize);

    setState(TransferState::Connecting);
    qInfo().noquote() << QStringLiteral("下载开始：%1 ← %2:%3 %4 段 × %5，已有 %6")
                             .arg(m_name, host)
                             .arg(port)
                             .arg(count)
                             .arg(Qdl::TransferFmt::sizeText(grant.chunkSize))
                             .arg(Qdl::TransferFmt::sizeText(initialDone));

    // 0 字节文件没有段可领、也没有数据连接（主机不发票），直接建空文件进落地流程。
    // 空内容的摘要是个常量，没必要为它再开一遍文件。
    if (count == 0 || m_total <= 0) {
        QFile empty(partPath());
        if (!empty.open(QIODevice::WriteOnly)) {
            failWith(tr("无法创建临时文件：%1").arg(empty.errorString()));
            return false;
        }
        empty.close();
        m_checksum = QString::fromLatin1(
            QCryptographicHash::hash(QByteArray(), QCryptographicHash::Sha256).toHex());
        setState(TransferState::Verifying);
        completeFile();
        return true;
    }

    pumpWorkers();
    return true;
}

// ------------------------------ 上传 ------------------------------

bool TransferTask::startUpload(const QString &host, quint16 port, const Qdl::UploadGrant &grant)
{
    if (host.isEmpty() || port == 0) {
        failWith(tr("上传参数不完整，无法建立数据连接"));
        return false;
    }

    abortWorkers();
    m_error.clear();
    m_verifyNote.clear();
    m_checksum.clear();
    m_expectedChecksum.clear();
    m_pullRequested = false;
    m_uploading = true;
    m_finishing = false;
    m_intentionalPause = false;
    m_finishHandled = false;
    m_speed = 0.0;
    m_stallTicks = 0;
    m_host = host;
    m_port = port;
    m_firstToken = grant.token;
    if (grant.size > 0)
        m_total = grant.size;
    if (!grant.transferId.isEmpty())
        m_transferId = grant.transferId;

    buildChunks(grant.segments, grant.offset);

    const int count = m_chunks.size();
    m_segTotalCount = qMax(1, count);

    // 进度从这里起步：主机已有的字节 = 各段 done 之和，别让用户以为续传一开始是 0%
    qint64 done = 0;
    for (Chunk &c : m_chunks) {
        if (c.length > 0 && c.done >= c.length) {
            c.done = c.length;
            c.state = Chunk::Finished;
        }
        done += c.done;
    }
    m_done = done;
    m_lastSampleDone = m_done;
    m_lastCheckpoint = m_done;

    // 摘要与传输同时开工（M13）：源文件读一遍就够了，不再等发完再读第二遍
    startSourceHash();

    setState(TransferState::Connecting);
    qInfo().noquote() << QStringLiteral("上传开始：%1 → %2:%3 %4 段 × %5，已有 %6")
                             .arg(m_name, host)
                             .arg(port)
                             .arg(count)
                             .arg(Qdl::TransferFmt::sizeText(grant.chunkSize))
                             .arg(Qdl::TransferFmt::sizeText(m_done));

    // 主机已经全收下了：没有内容要发，直接等确认
    if (count == 0 || m_total <= 0 || allChunksFinished()) {
        finishSending();
        return true;
    }

    pumpWorkers();
    return true;
}

void TransferTask::finishAllSegments()
{
    if (m_uploading) {
        finishSending();
        return;
    }

    // 下载：多段时先拼回整份 .qdpart（合并这一遍读顺手把摘要算了），再走"校验大小 → 改名 → 比对"
    if (m_segTotalCount > 1) {
        QString error;
        if (!mergeSegments(&error)) {
            m_finishing = false;
            failWith(error);
            return;
        }
    }
    completeFile();
}

bool TransferTask::mergeSegments(QString *error)
{
    const int count = m_segTotalCount;
    if (count <= 1)
        return true;

    QFile::remove(partPath());
    QFile out(partPath());
    if (!out.open(QIODevice::WriteOnly)) {
        if (error)
            *error = tr("无法创建临时文件：%1").arg(out.errorString());
        return false;
    }

    // 合并必须按段号顺序读一遍，那就顺手把整文件摘要算出来（M13）：
    // 收尾阶段因此不再有"合并完再读一遍算 SHA-256"的第三遍 I/O。
    QCryptographicHash hash(QCryptographicHash::Sha256);

    QByteArray buffer(kMergeChunk, Qt::Uninitialized);
    for (int i = 0; i < count; ++i) {
        const QString path = segmentFilePath(i, count);
        QFile in(path);
        if (!in.open(QIODevice::ReadOnly)) {
            out.close();
            if (error)
                *error = tr("分片文件缺失（第 %1 段）：%2").arg(i + 1).arg(in.errorString());
            return false;
        }
        while (true) {
            const qint64 got = in.read(buffer.data(), buffer.size());
            if (got <= 0)
                break;
            if (out.write(buffer.constData(), got) != got) {
                in.close();
                out.close();
                if (error)
                    *error = tr("合并分片失败（磁盘可能已满）：%1").arg(out.errorString());
                return false;
            }
            hash.addData(QByteArrayView(buffer.constData(), int(got)));
        }
        in.close();
    }
    out.flush();
    out.close();

    m_checksum = QString::fromLatin1(hash.result().toHex());

    // 分片与元信息用完就删，别让用户的文件夹里堆着 N 个隐藏文件
    for (int i = 0; i < count; ++i)
        QFile::remove(segmentFilePath(i, count));
    QFile::remove(segMetaPath());

    qInfo().noquote() << QStringLiteral("分片已合并：%1（%2 段），摘要 %3")
                             .arg(m_name)
                             .arg(count)
                             .arg(m_checksum.left(16));
    return true;
}

// ------------------------------ 推进 ------------------------------

void TransferTask::pump()
{
    for (TransferSegment *seg : std::as_const(m_live))
        seg->pump();
}

void TransferTask::refreshRateLimit()
{
    for (TransferSegment *seg : std::as_const(m_live))
        seg->refreshRateLimit();
}

void TransferTask::finishSending()
{
    if (m_finishing)
        return;
    m_finishing = true;

    stopTicker();
    abortWorkers();

    m_speed = 0.0;
    setState(TransferState::Verifying);

    // 源文件摘要从传输一开始就在后台算，走到这里通常已经算完了（M13）。
    // 真正要等的只有主机的 UPLOAD_DONE——它那边也是边收边算，不用再读一遍。
    ensureVerifyWaitTimer();
    m_verifyWaitTimer->start(kUploadConfirmMs);

    qInfo().noquote() << QStringLiteral("上传已发送完毕：%1（%2 字节），等主机确认%3")
                             .arg(m_name)
                             .arg(m_total)
                             .arg(m_checksum.isEmpty() ? QStringLiteral("（摘要仍在计算）")
                                                       : QStringLiteral("，本端摘要 ")
                                                             + m_checksum.left(16));
}

void TransferTask::markUploadInterrupted(const QString &message)
{
    if (m_direction != TransferDirection::Upload)
        return;
    if (isFinished() && m_state != TransferState::Failed)
        return;   // Completed / Canceled 的结果已经定了，不再改
    if (m_state == TransferState::Completed)
        return;

    m_error = message;
    m_speed = 0.0;

    if (m_state != TransferState::Paused) {
        // 也可能是先被判失败（socket 层报写入错误）才收到主机的说明：
        // 这时把它收敛成"已暂停、可继续"才是用户真正需要看到的状态
        setState(TransferState::Paused);
        emit finished(this);
    }
    emit changed();
}

void TransferTask::interrupt(const QString &message)
{
    // 主动暂停/取消会 abort()，这里不能把它当成“对端断开”，否则状态与错误文案都会被写乱
    if (m_finishing || m_intentionalPause)
        return;

    stopTicker();

    if (m_done > 0) {
        m_error = message;
        emitCheckpoint();
        setState(TransferState::Paused);
        qWarning().noquote() << QStringLiteral("%1中断：%2 %3")
                                    .arg(m_uploading ? tr("上传") : tr("下载"), m_name, message);
    } else {
        failWith(message);
    }
}

void TransferTask::tick()
{
    const qint64 elapsed = m_sampleTimer.restart();
    const qint64 delta = m_done - m_lastSampleDone;
    m_lastSampleDone = m_done;

    if (elapsed > 0) {
        const double instant = double(delta) * 1000.0 / double(elapsed);
        // 指数平滑：瞬时速度抖动太大，界面上的数字跳来跳去反而看不懂
        m_speed = m_speed <= 0.0 ? instant : m_speed * 0.6 + instant * 0.4;
    }

    if (delta == 0 && m_state == TransferState::Transferring) {
        if (++m_stallTicks >= kStallTicks) {
            // 断网不等于失败：留着 qdpart 转"已暂停"，用户点继续就能接上。
            // 判 Failed 也保留 qdpart，但界面上一片红，还会让人以为文件坏了。
            stopTicker();
            abortWorkers();
            m_intentionalPause = true;
            m_speed = 0.0;
            m_error = m_uploading ? tr("连接超时（6 秒无进展），可重试")
                                  : tr("连接超时（6 秒无数据），已保留断点");
            emitCheckpoint();
            setState(TransferState::Paused);
            emit finished(this);
            return;
        }
    }

    // 每个检查点最多 4MB 一次：GB 级文件按 300ms 节奏写库太浪费，太少又白丢进度
    if (m_done - m_lastCheckpoint >= 4 * 1024 * 1024)
        emitCheckpoint();

    // 限速时数据是被我们主动留在内核里的，readyRead 不会再来 —— 靠 tick 定期往下取。
    // 上传同理：令牌用完那一下是定时器叫醒的，但卡在半路时也靠这里兜住
    if (m_state == TransferState::Transferring) {
        pump();
        if (allChunksFinished() && !m_finishHandled) {
            m_finishHandled = true;
            finishAllSegments();
        }
    }

    emit changed();
}

void TransferTask::pause()
{
    if (!canPause())
        return;

    // 先置位再断开：abort() 可能同步触发 failed，晚一步就会把暂停写成"连接中断"
    m_intentionalPause = true;
    stopTicker();
    abortWorkers();

    m_speed = 0.0;
    emitCheckpoint();
    setState(TransferState::Paused);
    qInfo().noquote() << QStringLiteral("%1暂停：%2（%3 %4 字节）")
                             .arg(m_uploading ? tr("上传") : tr("下载"), m_name)
                             .arg(m_uploading ? tr("已发") : tr("已收"))
                             .arg(m_done);
}

void TransferTask::cancel()
{
    if (isFinished())
        return;

    m_finishing = true;   // 同上：先置位，避免 abort() 同步回调把状态改成 Paused
    m_intentionalPause = true;
    stopTicker();
    abortWorkers();

    // 取消要清干净：留着 qdpart / 分片用户既看不到也删不掉，只会占空间。
    // 上传方向的 m_localPath 是**源文件**，绝不能动
    if (m_direction == TransferDirection::Download) {
        if (QFile::exists(partPath()))
            QFile::remove(partPath());
        if (QFile::exists(segMetaPath()))
            QFile::remove(segMetaPath());
        // 分片按段号命名，段数上限是 kMaxChunks；不存在的直接跳过，别在"删到第一个
        // 空位就停"上偷懒，否则中间某段缺失时会留下一堆孤儿文件
        for (int i = 0; i < Qdl::kMaxChunks; ++i)
            QFile::remove(partPath() + QStringLiteral(".seg%1").arg(i));
    }

    m_speed = 0.0;
    setState(TransferState::Canceled);
    emit changed();
    emit finished(this);
    qInfo().noquote() << QStringLiteral("%1已取消：%2")
                             .arg(m_uploading ? tr("上传") : tr("下载"), m_name);
}

void TransferTask::reject(const QString &message)
{
    failWith(message);
}

void TransferTask::completeFile()
{
    if (m_finishing)
        return;
    if (m_uploading) {   // 防御：上传没有"落地改名"这一步
        finishSending();
        return;
    }
    m_finishing = true;

    stopTicker();
    abortWorkers();

    setState(TransferState::Verifying);

    const QFileInfo partInfo(partPath());
    if (!partInfo.exists() || partInfo.size() != m_total) {
        m_finishing = false;   // 放开收尾锁，否则 failWith 会被自己的重入保护吞掉
        failWith(tr("大小校验失败（应为 %1 字节，实际 %2）")
                     .arg(m_total)
                     .arg(partInfo.exists() ? partInfo.size() : 0));
        return;
    }

    QDir().mkpath(QFileInfo(m_localPath).absolutePath());
    if (QFile::exists(m_localPath))
        QFile::remove(m_localPath);   // 目标名在创建任务时已去重，这里的残留只可能是上次失败留下的

    if (!QFile::rename(partPath(), m_localPath)) {
        m_finishing = false;
        failWith(tr("无法重命名为目标文件：%1").arg(m_localPath));
        return;
    }

    // 摘要已经随接收/合并算好了（M13）：落地之后直接比对，收尾阶段不再读盘
    if (!m_checksum.isEmpty()) {
        qInfo().noquote() << QStringLiteral("落地完成，摘要已就绪：%1 %2")
                                 .arg(m_name, m_checksum.left(16));
        finalizeVerification();
        return;
    }

    // 单段续传这类"前半段不在本次连接里"的情形：只能老老实实读一遍（兜底）
    qInfo().noquote() << QStringLiteral("摘要无法随接收算出，收尾读一遍：%1").arg(m_name);
    startVerification();
}

void TransferTask::startVerification()
{
    m_verifyFile.setFileName(m_localPath);
    if (!m_verifyFile.open(QIODevice::ReadOnly)) {
        m_finishing = false;
        failWith(tr("无法读取已下载文件：%1").arg(m_verifyFile.errorString()));
        return;
    }

    delete m_verifyHash;
    m_verifyHash = new QCryptographicHash(QCryptographicHash::Sha256);

    if (!m_verifyTimer) {
        m_verifyTimer = new QTimer(this);
        // 0ms：事件循环一空就跑下一步。分片的目的是让界面能响应，
        // 每片 4MB 大约几毫秒，比开线程简单，也不会让进度条冻住。
        m_verifyTimer->setInterval(0);
        connect(m_verifyTimer, &QTimer::timeout, this, &TransferTask::verifyStep);
    }
    m_verifyTimer->start();

    qInfo().noquote() << QStringLiteral("开始校验：%1（%2 字节）")
                             .arg(m_name)
                             .arg(m_total);
}

void TransferTask::verifyStep()
{
    constexpr qint64 kChunk = 4 * 1024 * 1024;
    const QByteArray chunk = m_verifyFile.read(kChunk);
    if (chunk.isEmpty()) {
        stopVerification();
        noteLocalDigest(QString::fromLatin1(m_verifyHash->result().toHex()));
        return;
    }
    m_verifyHash->addData(chunk);
}

void TransferTask::stopVerification()
{
    if (m_verifyTimer)
        m_verifyTimer->stop();
    if (m_verifyFile.isOpen())
        m_verifyFile.close();
}

void TransferTask::noteLocalDigest(const QString &hex)
{
    if (hex.isEmpty()) {
        failWith(tr("无法计算本地文件校验值"));
        return;
    }

    m_checksum = hex;
    qInfo().noquote() << QStringLiteral("本地摘要已就绪：%1 %2").arg(m_name, hex.left(16));
    emit changed();
    emit checksumReady();   // 上传：外面据此把摘要推给主机

    if (m_state == TransferState::Verifying)
        finalizeVerification();
}

void TransferTask::finalizeVerification()
{
    if (m_state != TransferState::Verifying || m_uploading)
        return;
    if (m_checksum.isEmpty())
        return;   // 本地摘要还没好：等 noteLocalDigest / startVerification 收尾

    // 情形一：对端摘要已经随表到了（M13 的正常路径）——当场比对
    if (!m_expectedChecksum.isEmpty()) {
        if (m_verifyWaitTimer)
            m_verifyWaitTimer->stop();

        if (m_checksum.compare(m_expectedChecksum, Qt::CaseInsensitive) == 0) {
            qInfo().noquote() << QStringLiteral("校验通过：%1 %2")
                                     .arg(m_name, m_checksum.left(16));
            confirmVerified(true);
            return;
        }

        qWarning().noquote() << QStringLiteral("摘要不一致：%1 本地 %2 主机 %3")
                                    .arg(m_name, m_checksum, m_expectedChecksum);
        confirmVerified(false, tr("校验失败：内容与主机不一致，已删除"));
        return;
    }

    // 情形二：还没收到（可能只是推送比最后一个字节晚了几个毫秒）。
    // 先给一小段宽限，等不到再补拉——急着发 CHECKSUM_REQ 等于白白多一次往返。
    if (!m_pullRequested) {
        m_pullRequested = true;
        qInfo().noquote() << QStringLiteral("本地摘要已就绪，主机摘要未到，先等 %1 ms：%2")
                                 .arg(kChecksumGraceMs)
                                 .arg(m_name);
        ensureVerifyWaitTimer();
        m_verifyWaitTimer->start(kChecksumWaitMs);

        if (!m_pullDelayTimer) {
            m_pullDelayTimer = new QTimer(this);
            m_pullDelayTimer->setSingleShot(true);
            connect(m_pullDelayTimer, &QTimer::timeout, this, [this]() {
                if (m_state != TransferState::Verifying || !m_expectedChecksum.isEmpty())
                    return;   // 已经通过推送拿到，或者已经收尾了
                qInfo().noquote() << QStringLiteral("主机摘要仍未到，补拉一次：%1").arg(m_name);
                emit checksumPullRequested(m_remotePath);
            });
        }
        m_pullDelayTimer->start(kChecksumGraceMs);
        return;
    }

    // 情形三：补拉也没结果——降级为"只校验了大小"，但要在界面上留痕
    confirmVerified(true, tr("主机未提供校验值，仅校验了大小"));
}

void TransferTask::ensureVerifyWaitTimer()
{
    if (m_verifyWaitTimer)
        return;

    m_verifyWaitTimer = new QTimer(this);
    m_verifyWaitTimer->setSingleShot(true);
    connect(m_verifyWaitTimer, &QTimer::timeout, this, [this]() {
        // 永远挂在"校验中"是耍赖：按时长收尾，但要写明降级原因
        if (m_uploading)
            confirmVerified(true, tr("主机未确认上传结果，仅确认已发送完毕"));
        else
            confirmVerified(true, tr("主机未提供校验值，仅校验了大小"));
    });
}

void TransferTask::setExpectedChecksum(const QString &sha256)
{
    if (sha256.isEmpty() || m_expectedChecksum == sha256)
        return;

    m_expectedChecksum = sha256;
    if (m_pullDelayTimer)
        m_pullDelayTimer->stop();   // 推送到了，不必再补拉
    qInfo().noquote() << QStringLiteral("收到对端摘要：%1 %2").arg(m_name, sha256.left(16));
    emit changed();

    // 收尾时本地摘要可能已经好了（多半如此），那就当场把校验做完
    if (m_state == TransferState::Verifying)
        finalizeVerification();
}

void TransferTask::checksumUnavailable(const QString &note)
{
    if (m_state != TransferState::Verifying || m_uploading)
        return;
    if (!m_checksum.isEmpty() && !m_expectedChecksum.isEmpty())
        return;   // 其实已经能比对了，别把刚拿到的摘要浪费掉

    confirmVerified(true, note.isEmpty() ? tr("主机未提供校验值，仅校验了大小") : note);
}

void TransferTask::startSourceHash()
{
    if (m_sourceHash) {
        m_sourceHash->disconnect(this);
        m_sourceHash->deleteLater();
        m_sourceHash = nullptr;
    }

    auto *watcher = new QFutureWatcher<QString>(this);
    m_sourceHash = watcher;
    const QString path = m_localPath;

    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher]() {
        const QString hex = watcher->result();
        if (m_sourceHash == watcher)
            m_sourceHash = nullptr;
        watcher->deleteLater();

        // 任务已经被取消/失败：摘要算出来也没人用了
        if (isFinished() && m_state != TransferState::Verifying)
            return;

        if (hex.isEmpty()) {
            qWarning().noquote() << QStringLiteral("源文件摘要计算失败：%1").arg(m_name);
            m_verifyNote = tr("无法计算本机文件校验值");
            emit changed();
            emit checksumReady();   // 外面据此判"本端没有摘要可给"
            return;
        }

        noteLocalDigest(hex);
    });

    // 只读文件、与上传的读并发；GB 级在后台线程里跑，不占 UI 也不拖慢发送
    watcher->setFuture(QtConcurrent::run([path]() {
        QString error;
        const QString hex = Qdl::FileScanner::sha256(path, &error);
        if (hex.isEmpty())
            qWarning().noquote() << QStringLiteral("源文件摘要计算失败：%1").arg(error);
        return hex;
    }));
}

void TransferTask::confirmVerified(bool ok, const QString &message)
{
    if (m_state != TransferState::Verifying)
        return;

    if (m_verifyWaitTimer)
        m_verifyWaitTimer->stop();
    if (m_pullDelayTimer)
        m_pullDelayTimer->stop();

    if (!message.isEmpty())
        m_verifyNote = message;

    if (ok) {
        m_speed = 0.0;
        setState(TransferState::Completed);
        emit changed();
        emit finished(this);
        qInfo().noquote() << QStringLiteral("%1完成：%2 → %3（%4 字节）%5")
                                 .arg(m_uploading ? tr("上传") : tr("下载"), m_name,
                                      m_uploading ? m_remotePath : m_localPath)
                                 .arg(m_total)
                                 .arg(m_verifyNote.isEmpty() ? QString()
                                                             : QStringLiteral("，") + m_verifyNote);
        return;
    }

    if (m_uploading) {
        // 上传失败绝不删本地文件——那是用户的源文件，不是我们的半成品
        m_error = message.isEmpty() ? tr("上传校验失败：主机收到的内容与本地不一致") : message;
        m_checksum.clear();
        m_speed = 0.0;
        setState(TransferState::Failed);
        emit changed();
        emit finished(this);
        qWarning().noquote() << QStringLiteral("上传校验失败：%1 - %2").arg(m_name, m_error);
        return;
    }

    // 内容不对就把文件删掉：大小对、内容错的文件留着只会被当成好文件用下去
    if (QFile::exists(m_localPath))
        QFile::remove(m_localPath);

    m_error = message.isEmpty() ? tr("校验失败：下载内容与主机不一致") : message;
    m_checksum.clear();
    m_speed = 0.0;
    setState(TransferState::Failed);
    emit changed();
    emit finished(this);
    qWarning().noquote() << QStringLiteral("校验失败：%1 - %2").arg(m_name, m_error);
}

void TransferTask::restore(const QString &peerId, const QString &fileKey, qint64 done, qint64 total)
{
    m_peerId = peerId;
    m_fileKey = fileKey;
    if (total > 0)
        m_total = total;
    m_done = qBound<qint64>(0, done, m_total > 0 ? m_total : done);
    m_lastCheckpoint = m_done;
    m_lastSampleDone = m_done;
    m_error = tr("上次未传完，点继续接着下");
    setState(TransferState::Paused);
}

void TransferTask::setPeer(const QString &peerId, const QString &peerName)
{
    m_peerId = peerId;
    m_peerName = peerName;
}

void TransferTask::resetToZero()
{
    // 源文件已经变了：这份半成品拼上去就是坏文件，直接扔
    if (QFile::exists(partPath()))
        QFile::remove(partPath());
    for (int i = 0; i < Qdl::kMaxChunks; ++i)
        QFile::remove(partPath() + QStringLiteral(".seg%1").arg(i));
    if (QFile::exists(segMetaPath()))
        QFile::remove(segMetaPath());

    m_chunks.clear();
    m_done = 0;
    m_lastCheckpoint = 0;
    m_startOffset = 0;
    m_fileKey.clear();
    m_checksum.clear();
    m_error.clear();
    m_speed = 0.0;
    m_finishing = false;
    m_intentionalPause = false;
    m_finishHandled = false;
    setState(TransferState::Paused);
}

void TransferTask::emitCheckpoint()
{
    m_lastCheckpoint = m_done;
    emit checkpoint(m_done);
}

void TransferTask::failWith(const QString &message)
{
    if (m_finishing)
        return;
    m_finishing = true;

    stopTicker();
    if (m_verifyWaitTimer)
        m_verifyWaitTimer->stop();
    if (m_pullDelayTimer)
        m_pullDelayTimer->stop();
    abortWorkers();

    m_error = message;
    m_speed = 0.0;
    setState(TransferState::Failed);
    emit changed();
    emit finished(this);
    qWarning().noquote() << QStringLiteral("%1失败：%2 - %3")
                                .arg(m_uploading ? tr("上传") : tr("下载"), m_name, message);
}

void TransferTask::closeSocket()
{
    abortWorkers();
}

void TransferTask::stopTicker()
{
    if (m_ticker)
        m_ticker->stop();
}

void TransferTask::setState(TransferState state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit changed();
}

} // namespace Qdl

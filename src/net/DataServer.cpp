#include "DataServer.h"

#include "ChecksumCache.h"
#include "CryptoUtil.h"
#include "fs/FileScanner.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QNetworkProxy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <memory>

namespace {

constexpr quint16 kBasePort = 45456;
constexpr int kMaxPortTries = 10;
constexpr int kGrantTtlMs = 5 * 60 * 1000;     // 令牌 5 分钟过期
constexpr int kChunkSize = 256 * 1024;         // 单块 256KB
constexpr qint64 kHighWater = 2 * 1024 * 1024; // 用户态待发数据上限，超过就等 bytesWritten
constexpr int kProgressMs = 500;               // 进度信号节流
constexpr int kUploadIdleMs = 60000;           // 上传连接 60 秒没有字节就判定中断
constexpr int kChecksumTimeoutMs = 30000;      // 收完后等摘要的上限
constexpr char kMagic[4] = { 'Q', 'D', 'A', 'T' };

/**
 * 一条数据连接：验票 → 按类型走下载（吐字节）或上传（吞字节）。
 *
 * 下载侧的关键是背压：不看的 socket->write() 会把几百 MB 全塞进用户态缓冲，
 * 内存瞬间飙高，且取消时要等缓冲排空才真正断开。所以只在待发量低于水位时继续读文件。
 *
 * 上传侧的关键是「落到 .qdpart、收满才改名」：主机随时可能被关掉写权限、
 * 网线随时可能被拔，出现一个大小不对却名叫正式文件的残片是最糟的结果。
 */
class DataStreamer : public QObject
{
public:
    DataStreamer(QTcpSocket *socket, DataServer *server, QObject *parent)
        : QObject(parent)
        , m_socket(socket)
        , m_server(server)
    {
        m_socket->setParent(this);
        m_socket->setProxy(QNetworkProxy::NoProxy);

        connect(m_socket, &QTcpSocket::readyRead, this, &DataStreamer::onReadyRead);
        connect(m_socket, &QTcpSocket::bytesWritten, this, &DataStreamer::pump);
        connect(m_socket, &QTcpSocket::disconnected, this, &DataStreamer::onSocketClosed);

        // 握手不能无限等：连上来不发头就断开，省得堆积半开连接
        QTimer::singleShot(10000, this, [this]() {
            if (!m_headerDone)
                close(QStringLiteral("数据通道握手超时"), true);
        });
    }

    ~DataStreamer() override
    {
        if (m_checksumPending)
            m_server->unregisterUpload(m_transferId);
    }

    /**
     * 主机主动掐断（关写权限 / 关共享 / 对端取消）。keepPart=false 时把半成品删掉。
     *
     * 字节已经收满、正在算摘要的时候要放行过去：此刻没有"后续字节"可停了，
     * 文件也已经改名落地，再按"中断"处理只会让客户端显示成暂停，而收件箱里
     * 躺着一份完整文件——用户点"继续"还会再传一份 `name (1)` 出来。
     */
    bool abortByHost(const QString &reason, bool keepPart)
    {
        if (m_completing && keepPart) {
            qInfo().noquote()
                << QStringLiteral("上传已收满、正在校验，忽略本次中断：%1").arg(reason);
            return false;
        }
        close(reason, keepPart);
        return true;
    }

private:
    void onReadyRead()
    {
        if (!m_headerDone) {
            readHeader();
            return;
        }

        if (m_kind == Qdl::DataKind::Download) {
            // 客户端不该再发东西；多余字节一律忽略（避免把控制流量当文件内容）
            m_socket->readAll();
            return;
        }

        receiveUpload();
    }

    void readHeader()
    {
        m_buffer.append(m_socket->readAll());
        if (m_buffer.size() < DataServer::kHeaderLen)
            return;

        if (m_buffer.left(4) != QByteArray(kMagic, 4)) {
            close(QStringLiteral("数据通道魔数不匹配"), true);
            return;
        }

        const QString token = QString::fromLatin1(m_buffer.mid(4, DataServer::kTokenLen));
        // 只切掉头部，**保留**后面的字节：客户端常常把 QDAT+token 与第一批文件内容
        // 写在同一个 TCP 段里，直接 clear() 会把开头 256KB 静默吞掉
        m_buffer.remove(0, DataServer::kHeaderLen);

        Qdl::DataGrant grant;
        if (!m_server->consumeGrant(token, &grant)) {
            close(QStringLiteral("数据令牌无效或已过期"), true);
            return;
        }

        m_kind = grant.kind;
        m_transferId = grant.transferId;
        m_total = grant.size;

        if (m_kind == Qdl::DataKind::Download) {
            m_buffer.clear();   // 下载方向多出来的字节没有意义
            startDownload(grant);
        } else {
            startUpload(grant);
            // 头部之后同批到达的内容必须立刻处理，否则要等下一次 readyRead（可能永远不来）
            if (!m_buffer.isEmpty())
                receiveUpload();
        }
    }

    // ------------------------------ 下载 ------------------------------

    void startDownload(const Qdl::DataGrant &grant)
    {
        // 这一段在断点里已经凑齐了（客户端报了 done == length）：不用建文件连接，
        // 直接按"完整"收尾，否则空连接会一直挂到 10 秒握手超时
        if (grant.length <= 0) {
            m_total = 0;
            m_headerDone = true;
            finishDownload();
            return;
        }

        m_file.setFileName(grant.absolutePath);
        if (!m_file.open(QIODevice::ReadOnly)) {
            close(QStringLiteral("无法读取源文件：%1").arg(m_file.errorString()), true);
            return;
        }
        if (grant.offset > 0 && !m_file.seek(grant.offset)) {
            close(QStringLiteral("定位到偏移 %1 失败").arg(grant.offset), true);
            return;
        }

        // 分段后 m_total 是**本段**要发的字节数，不再是"到文件尾"
        m_total = grant.length;
        m_headerDone = true;
        qInfo().noquote() << QStringLiteral("数据通道开始下发 %1（从 %2 起，本段 %3 字节，第 %4/%5 段）端口 %6")
                                 .arg(QFileInfo(grant.absolutePath).fileName())
                                 .arg(grant.offset)
                                 .arg(m_total)
                                 .arg(grant.segIndex + 1)
                                 .arg(grant.segCount)
                                 .arg(m_socket->localPort());
        pump();
    }

    void pump()
    {
        if (m_headerDone == false || m_finished || m_kind != Qdl::DataKind::Download)
            return;

        while (m_socket->bytesToWrite() < kHighWater && m_sent < m_total) {
            const qint64 want = qMin<qint64>(kChunkSize, m_total - m_sent);
            const QByteArray chunk = m_file.read(want);
            if (chunk.isEmpty())
                break;
            m_socket->write(chunk);
            m_sent += chunk.size();
        }

        if (m_sent >= m_total && m_socket->bytesToWrite() == 0) {
            // 正常发完：先 flush 再优雅关闭，直接 abort 会丢掉内核里最后几块
            m_socket->disconnectFromHost();
            if (m_socket->state() == QAbstractSocket::UnconnectedState)
                finishDownload();
        }
    }

    void finishDownload()
    {
        if (m_finished)
            return;
        m_finished = true;

        const bool ok = m_total >= 0 && m_sent >= m_total;
        qInfo().noquote() << QStringLiteral("数据通道结束 %1：已发 %2 字节，%3")
                                 .arg(m_transferId)
                                 .arg(m_sent)
                                 .arg(ok ? QStringLiteral("完整") : QStringLiteral("中断"));
        emit m_server->transferFinished(m_transferId, m_sent, ok);
        deleteLater();
    }

    // ------------------------------ 上传 ------------------------------

    void startUpload(const Qdl::DataGrant &grant)
    {
        m_segIndex = grant.segIndex;
        m_segCount = qMax(1, grant.segCount);
        m_finalPath = grant.finalPath;
        m_name = grant.displayName;
        m_totalSize = grant.size;
        m_total = grant.length;        // 本段要收多少
        m_fileOffset = grant.offset;   // 本段在文件里的起点
        m_partPath = DataServer::segmentFilePath(grant.absolutePath, m_segIndex, m_segCount);
        m_startOffset = m_fileOffset;
        // 分片文件是从 0 长起来的：本段已有的字节 = 本次写位置 − 段在文件里的起点
        const qint64 expectedAlready = qMax<qint64>(0, m_fileOffset - grant.segBase);

        m_file.setFileName(m_partPath);
        if (!m_file.open(QIODevice::ReadWrite)) {
            close(QStringLiteral("无法写入收件文件：%1").arg(m_file.errorString()), true);
            return;
        }

        // 分片文件有多大，就是这段已经被文件系统确认收了多少：不需要任何额外的进度记录。
        // 与"应该有多少"不符只可能是上次的段划分变了（改了段数/段长），那份数据没有意义。
        // 注意别拿 m_total（本段**还要收**多少）去比：段过一半时 already 必然大于它。
        qint64 already = m_file.size();
        if (already < 0 || already != expectedAlready) {
            qWarning().noquote() << QStringLiteral("分片大小不符（%1，应为 %2），删除重收：%3")
                                        .arg(already).arg(expectedAlready).arg(m_partPath);
            m_file.close();
            QFile::remove(m_partPath);
            if (!m_file.open(QIODevice::ReadWrite)) {
                close(QStringLiteral("无法重建收件文件：%1").arg(m_file.errorString()), true);
                return;
            }
            already = 0;
        }

        if (!m_file.seek(already)) {
            close(QStringLiteral("定位到续传偏移 %1 失败").arg(already), true);
            return;
        }

        m_already = already;    // 本次连接开写前已有的字节
        m_received = 0;         // 本次连接收到的字节（落盘位置靠 seek(already) 接上）
        m_headerDone = true;

        // 从 0 开始收的这一次，摘要可以边收边算：最后一个字节落盘时摘要也就好了，
        // 收尾只剩一次比对（M13）。续传的前半段不在本进程手里，只能回退成收完再读。
        if (m_fileOffset == 0 && already == 0)
            m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);

        m_server->registerUploadSegment(grant.transferId, m_segIndex,
                                        [this](const QString &reason, bool keepPart) {
                                            return abortByHost(reason, keepPart);
                                        });

        if (m_segCount <= 1) {
            qInfo().noquote() << QStringLiteral("数据通道开始接收 %1（从 %2 起，共 %3 字节）端口 %4")
                                     .arg(m_name)
                                     .arg(m_fileOffset)
                                     .arg(m_totalSize)
                                     .arg(m_socket->localPort());
            emit m_server->uploadStarted(grant.transferId, m_name, m_finalPath, m_totalSize);
            emit m_server->uploadProgress(grant.transferId, m_name, m_already, m_totalSize);
        } else {
            qInfo().noquote() << QStringLiteral("数据通道接收分片 %1 第 %2/%3 段（从 %4 起，本段 %5 字节，已有 %6）端口 %7")
                                     .arg(m_name)
                                     .arg(m_segIndex + 1)
                                     .arg(m_segCount)
                                     .arg(m_fileOffset)
                                     .arg(m_total)
                                     .arg(m_already)
                                     .arg(m_socket->localPort());
        }
        armIdleTimer();

        // 这段已经齐了（上次传到这里断的）：不用再收，直接报告
        if (m_total <= 0 || m_received >= m_total)
            completeSegment();
    }

    void receiveUpload()
    {
        const qint64 remaining = m_total - m_received;
        if (remaining <= 0) {
            m_socket->readAll();   // 多出来的字节一律丢弃，total 才是唯一事实
            m_buffer.clear();
            return;
        }

        QByteArray chunk = m_socket->readAll();
        if (!m_buffer.isEmpty()) {
            chunk = m_buffer + chunk;
            m_buffer.clear();
        }
        if (chunk.size() > remaining)
            chunk.truncate(int(remaining));
        if (chunk.isEmpty())
            return;

        const qint64 written = m_file.write(chunk);
        if (written != chunk.size()) {
            close(QStringLiteral("写入失败（磁盘可能已满）：%1").arg(m_file.errorString()), true);
            return;
        }

        m_received += written;
        if (m_hash)
            m_hash->addData(QByteArrayView(chunk.constData(), chunk.size()));
        armIdleTimer();

        if (m_segCount > 1) {
            m_server->noteUploadProgress(m_transferId, m_segIndex, m_already + m_received);
        } else if (!m_progressTimer.isValid() || m_progressTimer.elapsed() >= kProgressMs) {
            m_progressTimer.restart();
            emit m_server->uploadProgress(m_transferId, m_name, m_received, m_totalSize);
        }

        if (m_received >= m_total) {
            completeSegment();
            return;
        }
    }

    /// 本段收满：单段直接走落地流程，多段只报告，等 DataServer 汇总后统一落地
    void completeSegment()
    {
        if (m_segCount > 1) {
            if (m_finished)
                return;
            m_finished = true;
            if (m_idleTimer)
                m_idleTimer->stop();
            m_server->unregisterUploadSegment(m_transferId, m_segIndex);
            m_file.flush();
            m_file.close();
            m_server->uploadSegmentFinished(m_transferId, m_segIndex);

            // 对端发完这一段就会关连接；先关掉我们这侧，避免半开
            if (m_socket->state() != QAbstractSocket::UnconnectedState)
                m_socket->disconnectFromHost();
            deleteLater();
            return;
        }

        completeUpload();
    }

    /// 收满了：改名 → 算摘要 → 回 UPLOAD_DONE
    void completeUpload()
    {
        if (m_completing)
            return;
        m_completing = true;

        m_file.flush();
        m_file.close();

        const QFileInfo partInfo(m_partPath);
        const qint64 expected = m_fileOffset + m_received;
        if (!partInfo.exists() || partInfo.size() != expected) {
            close(QStringLiteral("大小校验失败（应为 %1 字节，实际 %2）")
                      .arg(expected)
                      .arg(partInfo.exists() ? partInfo.size() : 0),
                  true);
            return;
        }

        // 目标名在 UPLOAD_ACCEPT 阶段已去重，这里的残留只可能是上次同名失败留下的
        if (QFile::exists(m_finalPath) && !QFile::remove(m_finalPath)) {
            close(QStringLiteral("无法覆盖同名文件：%1").arg(m_finalPath), true);
            return;
        }
        if (!QFile::rename(m_partPath, m_finalPath)) {
            close(QStringLiteral("无法重命名为正式文件：%1").arg(m_finalPath), true);
            return;
        }
        // 指纹侧文件只在"有半成品"时有意义，落地后立刻清掉，别在收件箱里留垃圾
        QFile::remove(m_partPath + QStringLiteral(".key"));
        QFile::remove(m_partPath + QStringLiteral(".qseg"));

        qInfo().noquote() << QStringLiteral("上传已落地：%1（%2 字节）").arg(m_finalPath).arg(m_received);

        // 边收边算的那份摘要此刻已经完整：直接进比对，收尾没有再读一遍文件
        if (m_hash) {
            finishUploadChecked(QString::fromLatin1(m_hash->result().toHex()));
            return;
        }
        // 续传：前半段不是本进程收的，只能老老实实读一遍（兜底路径）
        requestChecksum();
    }

    void requestChecksum()
    {
        Qdl::ChecksumCache *cache = m_server->checksums();
        if (!cache) {
            finishUploadChecked(QString());
            return;
        }

        QString keyError;
        const QString key = Qdl::FileScanner::fileKey(m_finalPath, &keyError);
        if (key.isEmpty()) {
            qWarning().noquote() << QStringLiteral("上传落地但无法算 fileKey：%1").arg(keyError);
            finishUpload(true, QString(), QStringLiteral("主机未能计算校验值"));
            return;
        }

        const QString cached = cache->cached(key);
        if (!cached.isEmpty()) {
            finishUploadChecked(cached);
            return;
        }

        m_checksumPending = true;
        m_checksumKey = key;
        connect(cache, &Qdl::ChecksumCache::ready, this,
                [this](const QString &readyKey, const QString &sha256) {
                    if (!m_checksumPending || readyKey != m_checksumKey)
                        return;
                    if (sha256.isEmpty()) {
                        finishUpload(true, QString(), QStringLiteral("主机未能计算校验值"));
                        return;
                    }
                    finishUploadChecked(sha256);
                });

        // 摘要算不出来不能把连接吊死：超时就按"只校验了大小"收尾，但要留痕
        QTimer::singleShot(kChecksumTimeoutMs, this, [this]() {
            if (m_checksumPending)
                finishUpload(true, QString(), QStringLiteral("主机计算校验值超时"));
        });

        cache->request(key, m_finalPath);
    }

    /// 拿到本端摘要之后：跟发送端声明的那份比，一致才算落地成功
    void finishUploadChecked(const QString &own)
    {
        const QString expected = m_server->expectedChecksum(m_transferId);
        if (own.isEmpty()) {
            finishUpload(true, QString(), QStringLiteral("主机未能计算校验值"));
            return;
        }
        if (expected.isEmpty()) {
            // 发送端没声明摘要（老客户端 / 它自己也没算出来）：把本端的值回给它，
            // 由它自己比对——行为与 M7 一致，不至于因为对端老旧就判失败。
            finishUpload(true, own, QString());
            return;
        }
        if (own.compare(expected, Qt::CaseInsensitive) == 0) {
            qInfo().noquote() << QStringLiteral("上传校验通过：%1（%2）")
                                     .arg(m_name, own.left(16));
            finishUpload(true, own, QString());
            return;
        }

        // 内容对不上：这份副本不能留。发送端的源文件不归我们动，只回失败原因
        qWarning().noquote() << QStringLiteral("上传校验失败：%1（本端 %2，源文件 %3）")
                                    .arg(m_name, own.left(16), expected.left(16));
        if (QFile::exists(m_finalPath) && !QFile::remove(m_finalPath))
            qWarning().noquote() << QStringLiteral("校验失败的副本删不掉：%1").arg(m_finalPath);
        finishUpload(false, own, QStringLiteral("校验失败：收到的内容与源文件不一致"),
                     true);
    }

    void armIdleTimer()
    {
        if (!m_idleTimer) {
            m_idleTimer = new QTimer(this);
            m_idleTimer->setSingleShot(true);
            connect(m_idleTimer, &QTimer::timeout, this, [this]() {
                close(QStringLiteral("上传连接 60 秒无数据，已保留断点"), true);
            });
        }
        m_idleTimer->start(kUploadIdleMs);
    }

    void finishUpload(bool ok, const QString &sha256, const QString &message,
                      bool checksumMismatch = false)
    {
        if (m_finished)
            return;
        m_finished = true;
        m_checksumPending = false;
        if (m_idleTimer)
            m_idleTimer->stop();
        m_server->unregisterUpload(m_transferId);
        m_server->setExpectedChecksum(m_transferId, QString());   // 表项用完即销

        if (m_socket->state() != QAbstractSocket::UnconnectedState) {
            m_socket->disconnectFromHost();
            if (m_socket->state() == QAbstractSocket::UnconnectedState)
                m_socket->abort();
        }

        qInfo().noquote() << QStringLiteral("上传结束 %1：已收 %2 / %3 字节，%4%5")
                                 .arg(m_name)
                                 .arg(m_fileOffset + m_received)
                                 .arg(m_totalSize)
                                 .arg(ok ? QStringLiteral("成功") : QStringLiteral("失败"))
                                 .arg(message.isEmpty() ? QString() : QStringLiteral("（%1）").arg(message));
        emit m_server->uploadFinished(m_transferId, m_finalPath, m_name,
                                      m_fileOffset + m_received, ok, sha256, message,
                                      checksumMismatch);
        deleteLater();
    }

    // ------------------------------ 收尾 ------------------------------

    void onSocketClosed()
    {
        if (m_finished || m_completing)
            return;

        if (m_kind == Qdl::DataKind::Download) {
            finishDownload();
            return;
        }

        // 多段上传：这一段收满就算这段结束，落地与否由 DataServer 按整份文件判断
        if (m_segCount > 1) {
            if (m_total <= 0 || m_received >= m_total) {
                completeSegment();
                return;
            }
            close(QStringLiteral("分片中断（已收到 %1 / %2 字节）")
                      .arg(m_already + m_received)
                      .arg(m_already + m_total), true);
            return;
        }

        // 客户端把字节发完就主动关连接也算正常：先看收够没有，再决定成功还是中断
        if (m_received >= m_total) {
            completeUpload();
            return;
        }

        // 中断：保留 .qdpart，等用户（或对端）重新发起续传
        close(QStringLiteral("上传中断（已收到 %1 / %2 字节）")
                  .arg(m_already + m_received)
                  .arg(m_already + m_total), true);
    }

    void close(const QString &reason, bool keepPart)
    {
        if (m_finished)
            return;

        // 头都没发全就断了：不是任何一次传输，直接清掉，别污染统计
        if (!m_headerDone) {
            m_finished = true;
            qWarning().noquote() << QStringLiteral("数据通道断开：%1").arg(reason);
            if (m_socket->state() != QAbstractSocket::UnconnectedState)
                m_socket->abort();
            deleteLater();
            return;
        }

        if (m_kind == Qdl::DataKind::Download) {
            qWarning().noquote() << QStringLiteral("数据通道断开：%1").arg(reason);
            if (m_socket->state() != QAbstractSocket::UnconnectedState)
                m_socket->abort();
            finishDownload();
            return;
        }

        // 多段：交给 DataServer 汇总（它会停掉其余段、决定分片留还是删），
        // 这里不能自己发 uploadFinished —— 那会让一次上传回 N 条结果
        if (m_segCount > 1) {
            m_finished = true;
            m_checksumPending = false;
            if (m_idleTimer)
                m_idleTimer->stop();
            m_server->unregisterUploadSegment(m_transferId, m_segIndex);
            if (m_file.isOpen()) {
                m_file.flush();
                m_file.close();
            }
            if (m_socket->state() != QAbstractSocket::UnconnectedState)
                m_socket->abort();

            qWarning().noquote() << QStringLiteral("上传分片中断 %1 第 %2/%3 段：%4")
                                        .arg(m_name)
                                        .arg(m_segIndex + 1)
                                        .arg(m_segCount)
                                        .arg(reason);
            m_server->uploadSegmentFailed(m_transferId, m_segIndex, m_segCount, reason, keepPart);
            deleteLater();
            return;
        }

        m_finished = true;
        m_checksumPending = false;
        if (m_idleTimer)
            m_idleTimer->stop();
        m_server->unregisterUpload(m_transferId);

        if (m_file.isOpen()) {
            m_file.flush();
            m_file.close();
        }

        if (!keepPart) {
            if (QFile::exists(m_partPath))
                QFile::remove(m_partPath);
            // 指纹侧文件与半成品同生共死，漏了它收件箱里就会留一个藏不掉的 .qdpart.key
            if (QFile::exists(m_partPath + QStringLiteral(".key")))
                QFile::remove(m_partPath + QStringLiteral(".key"));
            QFile::remove(m_partPath + QStringLiteral(".qseg"));
        }

        qWarning().noquote() << QStringLiteral("上传中断 %1：%2%3")
                                    .arg(m_name, reason)
                                    .arg(keepPart ? QStringLiteral("（断点已保留）") : QString());

        if (m_socket->state() != QAbstractSocket::UnconnectedState)
            m_socket->abort();

        m_server->setExpectedChecksum(m_transferId, QString());
        emit m_server->uploadFinished(m_transferId, m_finalPath, m_name, m_received, false,
                                      QString(), reason);
        deleteLater();
    }

    QTcpSocket *m_socket = nullptr;
    DataServer *m_server = nullptr;
    QFile m_file;
    QByteArray m_buffer;
    QTimer *m_idleTimer = nullptr;
    QElapsedTimer m_progressTimer;
    /// 边收边算：只有"从 0 开始"的接收才能这么干（续传的前半段不在本进程手里）
    std::unique_ptr<QCryptographicHash> m_hash;

    Qdl::DataKind m_kind = Qdl::DataKind::Download;
    QString m_transferId;
    QString m_name;          // 上传：去重后的文件名
    QString m_partPath;
    QString m_finalPath;
    QString m_checksumKey;
    qint64 m_total = 0;      // 本段要传/要收的字节数（分段后不再是整个文件）
    qint64 m_totalSize = 0;  // 整个文件大小，日志与进度用
    qint64 m_sent = 0;
    qint64 m_received = 0;   // 本次连接收到的字节
    qint64 m_already = 0;    // 本次连接开写前分片已有的字节
    qint64 m_fileOffset = 0; // 本段在文件里的起点
    qint64 m_startOffset = 0;
    int m_segIndex = 0;
    int m_segCount = 1;
    bool m_headerDone = false;
    bool m_finished = false;
    bool m_completing = false;
    bool m_checksumPending = false;
};

} // namespace

DataServer::DataServer(QObject *parent)
    : QObject(parent)
{
}

bool DataServer::isListening() const
{
    return m_server && m_server->isListening();
}

bool DataServer::start(quint16 preferredPort, QString *error)
{
    if (isListening())
        return true;

    if (!m_server) {
        m_server = new QTcpServer(this);
        connect(m_server, &QTcpServer::newConnection, this, &DataServer::onNewConnection);
    }

    // 与控制通道一样：端口被占用就顺延。端口号随 DOWNLOAD_ACCEPT / UPLOAD_ACCEPT 下发，
    // 客户端不需要预设
    const quint16 base = preferredPort == 0 ? kBasePort : preferredPort;
    for (int i = 0; i < kMaxPortTries; ++i) {
        const quint16 candidate = quint16(base + i);
        if (m_server->listen(QHostAddress::Any, candidate)) {
            m_port = candidate;
            qInfo().noquote() << QStringLiteral("数据通道监听 :%1").arg(candidate);
            return true;
        }
    }

    const QString message = tr("数据通道启动失败（%1..%2 均被占用）：%3")
                                .arg(base)
                                .arg(base + kMaxPortTries - 1)
                                .arg(m_server->errorString());
    qWarning().noquote() << message;
    if (error)
        *error = message;
    emit errorOccurred(message);
    return false;
}

void DataServer::stop()
{
    m_grants.clear();
    m_uploadAborters.clear();
    m_reservedPaths.clear();
    m_uploadGroups.clear();
    m_expectedSums.clear();
    if (m_server && m_server->isListening()) {
        m_server->close();
        qInfo() << "数据通道已停止";
    }
}

QString DataServer::segmentFilePath(const QString &partPath, int segIndex, int segCount)
{
    if (segCount <= 1)
        return partPath;
    return partPath + QStringLiteral(".seg%1").arg(segIndex);
}

QString DataServer::grant(const QString &transferId, const QString &absolutePath,
                          qint64 offset, qint64 length)
{
    if (!isListening())
        return {};

    purgeExpired();

    Qdl::DataGrant g;
    g.kind = Qdl::DataKind::Download;
    g.transferId = transferId;
    g.absolutePath = absolutePath;
    g.offset = qMax<qint64>(0, offset);
    g.length = qMax<qint64>(0, length);
    g.size = g.offset + g.length;
    g.expires = QDateTime::currentDateTimeUtc().addMSecs(kGrantTtlMs);

    const QString token = Qdl::randomToken();
    m_grants.insert(token, g);
    return token;
}

QString DataServer::grantUpload(const QString &transferId, const QString &partPath,
                                const QString &finalPath, const QString &displayName,
                                qint64 offset, qint64 totalSize)
{
    // 单段就是"一段覆盖整个剩余区间"，与分段共用一条落地路径
    return grantUploadSegment(transferId, 0, 1, partPath, finalPath, displayName,
                              offset, qMax<qint64>(0, totalSize - offset), totalSize);
}

QString DataServer::grantUploadSegment(const QString &transferId, int segIndex, int segCount,
                                       const QString &partPath, const QString &finalPath,
                                       const QString &displayName, qint64 offset, qint64 length,
                                       qint64 totalSize,
                                       const QVector<qint64> &segmentLengths, qint64 segBase)
{
    if (!isListening())
        return {};

    purgeExpired();

    Qdl::DataGrant g;
    g.kind = Qdl::DataKind::Upload;
    g.transferId = transferId;
    g.absolutePath = partPath;
    g.finalPath = finalPath;
    g.displayName = displayName;
    g.offset = qMax<qint64>(0, offset);
    g.length = qMax<qint64>(0, length);
    g.segBase = qMax<qint64>(0, segBase);
    g.size = qMax<qint64>(0, totalSize);
    g.segIndex = qMax(0, segIndex);
    g.segCount = qMax(1, segCount);
    g.expires = QDateTime::currentDateTimeUtc().addMSecs(kGrantTtlMs);

    const QString token = Qdl::randomToken();
    m_grants.insert(token, g);
    // 名字占位：并发的同名上传不能再选中同一个落点
    m_reservedPaths.insert(finalPath, transferId);

    if (g.segCount > 1) {
        auto it = m_uploadGroups.find(transferId);
        if (it == m_uploadGroups.end()) {
            // 同名落点的旧表（上次被暂停/断线留下的）先作废：它的段号对应的是
            // 上一轮的区间，留着只会在最后一刻拿旧表去合并、报一句看不懂的错。
            for (auto old = m_uploadGroups.begin(); old != m_uploadGroups.end();) {
                if (old.key() != transferId && old->partPath == partPath) {
                    qInfo().noquote() << QStringLiteral("作废同落点的旧上传表 %1（新的是 %2）")
                                             .arg(old.key(), transferId);
                    old->aborted = true;
                    old = m_uploadGroups.erase(old);
                } else {
                    ++old;
                }
            }

            Qdl::UploadGroup grp;
            grp.transferId = transferId;
            grp.partPath = partPath;
            grp.finalPath = finalPath;
            grp.displayName = displayName;
            grp.total = g.size;
            grp.segCount = g.segCount;
            grp.lengths = segmentLengths;
            while (grp.lengths.size() < g.segCount)
                grp.lengths.append(0);
            grp.done.fill(0, g.segCount);
            grp.finished.fill(false, g.segCount);
            m_uploadGroups.insert(transferId, grp);

            qInfo().noquote() << QStringLiteral("多段上传受理 %1：%2 字节 / %3 段")
                                     .arg(displayName)
                                     .arg(g.size)
                                     .arg(g.segCount);
            emit uploadStarted(transferId, displayName, finalPath, g.size);
        }
    }

    return token;
}

bool DataServer::consumeGrant(const QString &token, Qdl::DataGrant *out)
{
    purgeExpired();

    const auto it = m_grants.find(token);
    if (it == m_grants.end())
        return false;

    if (out)
        *out = it.value();
    m_grants.erase(it);   // 一次性：用掉即删，重放攻击拿不到第二次
    return true;
}

int DataServer::abortUploads(const QString &reason)
{
    if (m_uploadAborters.isEmpty())
        return 0;

    // 先拷一份再动手：每个 aborter 都会注销自己，直接遍历原表会边改边读
    const auto aborters = m_uploadAborters;
    int aborted = 0;
    for (const auto &aborter : aborters) {
        if (aborter(reason, true))
            ++aborted;
    }

    if (aborted > 0)
        qInfo().noquote() << QStringLiteral("已中断 %1 个上传：%2（断点已保留）")
                                 .arg(aborted)
                                 .arg(reason);
    return aborted;
}

bool DataServer::cancelUpload(const QString &transferId)
{
    if (transferId.isEmpty())
        return false;

    // 多段：该 transferId 下所有段一起取消，分片与 qdpart 全部抹掉
    bool any = false;
    const QString prefix = transferId + QLatin1Char('#');
    QVector<std::function<bool(const QString &, bool)>> aborters;
    for (auto it = m_uploadAborters.cbegin(); it != m_uploadAborters.cend(); ++it) {
        if (it.key() == transferId || it.key().startsWith(prefix))
            aborters.append(it.value());
    }
    for (const auto &aborter : aborters) {
        if (aborter(QStringLiteral("对端取消了上传"), false))
            any = true;
    }

    const auto git = m_uploadGroups.find(transferId);
    if (git != m_uploadGroups.end()) {
        git->aborted = true;
        removeGroupParts(transferId, false);
        m_uploadGroups.erase(git);
        m_reservedPaths.remove(transferId);   // 兼容：下面按 finalPath 再清一次
        any = true;
    }

    // 占位一定要还回去，否则同名的下次永远排不上
    for (auto it = m_reservedPaths.begin(); it != m_reservedPaths.end();) {
        if (it.value() == transferId)
            it = m_reservedPaths.erase(it);
        else
            ++it;
    }
    m_expectedSums.remove(transferId);   // 取消的传输留摘要等于给下一次误用
    return any;
}

void DataServer::registerUpload(const QString &transferId,
                                std::function<bool(const QString &, bool)> aborter)
{
    m_uploadAborters.insert(transferId, std::move(aborter));
}

void DataServer::unregisterUpload(const QString &transferId)
{
    m_uploadAborters.remove(transferId);
    // 占位与传输同生共死：传完/被取消/被中断都要释放，否则同名的下次永远排不上
    for (auto it = m_reservedPaths.begin(); it != m_reservedPaths.end();) {
        if (it.value() == transferId)
            it = m_reservedPaths.erase(it);
        else
            ++it;
    }
}

void DataServer::registerUploadSegment(
    const QString &transferId, int segIndex,
    std::function<bool(const QString &, bool)> aborter)
{
    m_uploadAborters.insert(QStringLiteral("%1#%2").arg(transferId).arg(segIndex),
                            std::move(aborter));
}

void DataServer::unregisterUploadSegment(const QString &transferId, int segIndex)
{
    m_uploadAborters.remove(QStringLiteral("%1#%2").arg(transferId).arg(segIndex));

    // 最后一条连接走了 = 这份传输此刻没有人在往盘上写。这时必须把落点占位还回去，
    // 否则客户端「暂停 → 继续」重新申请时，同名候选会因为占位还在被判成忙，
    // 只能退到 up (1).bin 从头再传一遍，断点等于白留。
    const QString prefix = transferId + QLatin1Char('#');
    for (auto it = m_uploadAborters.cbegin(); it != m_uploadAborters.cend(); ++it) {
        if (it.key() == transferId || it.key().startsWith(prefix))
            return;   // 还有别的段在收，占位继续持有
    }
    for (auto it = m_reservedPaths.begin(); it != m_reservedPaths.end();) {
        if (it.value() == transferId)
            it = m_reservedPaths.erase(it);
        else
            ++it;
    }
}

void DataServer::noteUploadProgress(const QString &transferId, int segIndex, qint64 segmentDone)
{
    const auto it = m_uploadGroups.find(transferId);
    if (it == m_uploadGroups.end())
        return;

    Qdl::UploadGroup &grp = *it;
    if (segIndex < 0 || segIndex >= grp.done.size() || grp.aborted)
        return;
    grp.done[segIndex] = qMax<qint64>(0, segmentDone);

    // 多段进度汇总：每段的 readyRead 都会报到这儿，不节流会把界面刷爆
    if (m_uploadProgressTimer.isValid() && m_uploadProgressTimer.elapsed() < kProgressMs)
        return;
    m_uploadProgressTimer.restart();

    qint64 sum = 0;
    for (qint64 d : grp.done)
        sum += d;
    emit uploadProgress(transferId, grp.displayName, qBound<qint64>(0, sum, grp.total), grp.total);
}

void DataServer::uploadSegmentFinished(const QString &transferId, int segIndex)
{
    const auto it = m_uploadGroups.find(transferId);
    if (it == m_uploadGroups.end())
        return;   // 单段由 DataStreamer 自己走落地流程，不会到这里

    Qdl::UploadGroup &grp = *it;
    if (grp.aborted || grp.reported)
        return;

    // 按段号记账：一条连接失败重试、或客户端重连把同一段又报一次，
    // 单纯 ++finishedCount 会让"3/3"提前达成，进而在还有段在写的时候就去合并。
    if (segIndex >= 0 && segIndex < grp.finished.size()) {
        if (grp.finished.at(segIndex))
            return;
        grp.finished[segIndex] = true;
    }
    ++grp.finishedCount;
    qInfo().noquote() << QStringLiteral("上传分片到齐 %1：%2/%3（段 %4）")
                             .arg(grp.displayName)
                             .arg(grp.finishedCount)
                             .arg(grp.segCount)
                             .arg(segIndex + 1);
    if (grp.finishedCount >= grp.segCount)
        finalizeUploadGroup(transferId);
}

void DataServer::uploadSegmentFailed(const QString &transferId, int segIndex, int segCount,
                                     const QString &reason, bool keepPart)
{
    Q_UNUSED(segCount)

    const auto it = m_uploadGroups.find(transferId);
    if (it == m_uploadGroups.end()) {
        // 单段：DataStreamer 自己已经发过 uploadFinished，这里只负责清占位
        return;
    }

    Qdl::UploadGroup &grp = *it;
    if (grp.aborted) {
        if (!keepPart)
            removeGroupParts(transferId, false);
        return;
    }

    // 多段时单条连接断掉只是"这一段没传完"，不是整份传输失败：
    // 客户端会重新领这一段，本端按盘上的分片大小接着收就是了。
    // 一有段失败就整份作废的话，worker 池的重试逻辑永远跑不起来。
    if (keepPart) {
        qWarning().noquote() << QStringLiteral("上传分片未完成 %1 第 %2/%3 段：%4（等客户端重领）")
                                    .arg(grp.displayName)
                                    .arg(segIndex + 1)
                                    .arg(grp.segCount)
                                    .arg(reason);
        return;
    }

    grp.aborted = true;

    // 先把其余段停掉，再报告结果：让每一条连接都收敛，不留半开
    const QString prefix = transferId + QLatin1Char('#');
    const auto aborters = m_uploadAborters;
    for (auto a = aborters.cbegin(); a != aborters.cend(); ++a) {
        if (a.key().startsWith(prefix))
            a.value()(reason, keepPart);
    }

    if (!keepPart)
        removeGroupParts(transferId, false);

    reportUploadGroup(transferId, false, QString(), reason);
    m_uploadGroups.erase(m_uploadGroups.find(transferId));

    for (auto it2 = m_reservedPaths.begin(); it2 != m_reservedPaths.end();) {
        if (it2.value() == transferId)
            it2 = m_reservedPaths.erase(it2);
        else
            ++it2;
    }
}

bool DataServer::finishEmptyUpload(const QString &transferId, const QString &partPath,
                                   const QString &finalPath, const QString &displayName)
{
    // 0 字节没有数据连接，也没有 .qdpart 要合并：落地动作就是"建一个空文件"。
    // 顺带清掉上一轮同名断点（同名的 0 字节文件重传，旧的 .qdpart/.key 没人再用了）。
    QFile::remove(partPath);
    QFile::remove(partPath + QStringLiteral(".key"));
    QFile::remove(partPath + QStringLiteral(".qseg"));

    QFile finalFile(finalPath);
    if (!finalFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString why = QStringLiteral("无法创建文件：%1").arg(finalFile.errorString());
        qWarning().noquote() << QStringLiteral("0 字节上传落地失败 %1：%2").arg(displayName, why);
        // 占位要还，否则同名文件会被占住 5 分钟
        for (auto it = m_reservedPaths.begin(); it != m_reservedPaths.end();) {
            if (it.value() == transferId)
                it = m_reservedPaths.erase(it);
            else
                ++it;
        }
        emit uploadFinished(transferId, finalPath, displayName, 0, false, QString(), why, false);
        return false;
    }
    finalFile.close();

    for (auto it = m_reservedPaths.begin(); it != m_reservedPaths.end();) {
        if (it.value() == transferId)
            it = m_reservedPaths.erase(it);
        else
            ++it;
    }
    // 0 字节没有数据连接，为它发出去的那张票（以及可能的声明摘要）都不再有用
    for (auto it = m_grants.begin(); it != m_grants.end();) {
        if (it.value().transferId == transferId)
            it = m_grants.erase(it);
        else
            ++it;
    }
    m_expectedSums.remove(transferId);

    // 空内容的 SHA-256 是常量值，客户端拿它跟本端源文件的摘要比，
    // 走的还是普通上传那条比对路径（不一致照样判失败）。
    const QString hex = QString::fromLatin1(
        QCryptographicHash::hash(QByteArray(), QCryptographicHash::Sha256).toHex());
    qInfo().noquote() << QStringLiteral("上传已落地：%1（0 字节，无数据连接）").arg(finalPath);
    emit uploadFinished(transferId, finalPath, displayName, 0, true, hex, QString(), false);
    return true;
}

void DataServer::finalizeUploadGroup(const QString &transferId)
{
    const auto it = m_uploadGroups.find(transferId);
    if (it == m_uploadGroups.end())
        return;
    const Qdl::UploadGroup grp = it.value();
    if (grp.aborted || grp.reported)
        return;

    // 合并前先把每段对一遍：分片短了才是常见故障（连接早断、被杀），
    // 直接进合并只会得到一句"大小校验失败"，看不出是哪一段的问题。
    for (int i = 0; i < grp.segCount; ++i) {
        const QString segPath = segmentFilePath(grp.partPath, i, grp.segCount);
        const QFileInfo segInfo(segPath);
        const qint64 want = i < grp.lengths.size() ? grp.lengths.at(i) : -1;
        if (!segInfo.exists()) {
            failUploadGroup(transferId,
                            QStringLiteral("分片 %1 缺失，无法合并（%2）").arg(i + 1).arg(segPath),
                            true);
            return;
        }
        if (want >= 0 && segInfo.size() != want) {
            failUploadGroup(transferId,
                            QStringLiteral("分片 %1 长度不符（应为 %2，实际 %3）")
                                .arg(i + 1)
                                .arg(want)
                                .arg(segInfo.size()),
                            true);
            return;
        }
    }

    // 合并：顺序把分片拷进 qdpart。用固定大小的缓冲块，绝不把 GB 级文件读进内存
    QFile::remove(grp.partPath);
    QFile out(grp.partPath);
    if (!out.open(QIODevice::WriteOnly)) {
        failUploadGroup(transferId, QStringLiteral("无法创建收件文件：%1").arg(out.errorString()),
                        true);
        return;
    }

    // 合并这一遍读是省不掉的，顺手把整文件摘要算出来（M13）：
    // 收尾阶段因此不再需要"读完整个文件算 SHA-256"的第三遍 I/O
    QCryptographicHash hash(QCryptographicHash::Sha256);

    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    for (int i = 0; i < grp.segCount; ++i) {
        QFile in(segmentFilePath(grp.partPath, i, grp.segCount));
        if (!in.open(QIODevice::ReadOnly)) {
            out.close();
            failUploadGroup(transferId,
                            QStringLiteral("分片 %1 无法读取：%2").arg(i).arg(in.errorString()),
                            true);
            return;
        }
        while (true) {
            const qint64 got = in.read(buffer.data(), buffer.size());
            if (got <= 0)
                break;
            if (out.write(buffer.constData(), got) != got) {
                in.close();
                out.close();
                failUploadGroup(transferId,
                                QStringLiteral("合并分片失败（磁盘可能已满）：%1")
                                    .arg(out.errorString()),
                                true);
                return;
            }
            hash.addData(QByteArrayView(buffer.constData(), got));
        }
        in.close();
    }
    out.flush();
    out.close();

    const QFileInfo partInfo(grp.partPath);
    if (!partInfo.exists() || partInfo.size() != grp.total) {
        failUploadGroup(transferId,
                        QStringLiteral("合并后大小校验失败（应为 %1 字节，实际 %2）")
                            .arg(grp.total)
                            .arg(partInfo.exists() ? partInfo.size() : 0),
                        true);
        return;
    }

    // 分片用完就删，别在收件箱里留 N 个看不出用途的文件
    for (int i = 0; i < grp.segCount; ++i)
        QFile::remove(segmentFilePath(grp.partPath, i, grp.segCount));

    if (QFile::exists(grp.finalPath) && !QFile::remove(grp.finalPath)) {
        failUploadGroup(transferId, QStringLiteral("无法覆盖同名文件：%1").arg(grp.finalPath),
                        true);
        return;
    }
    if (!QFile::rename(grp.partPath, grp.finalPath)) {
        failUploadGroup(transferId, QStringLiteral("无法重命名为正式文件：%1").arg(grp.finalPath),
                        true);
        return;
    }
    QFile::remove(grp.partPath + QStringLiteral(".key"));
    QFile::remove(grp.partPath + QStringLiteral(".qseg"));

    qInfo().noquote() << QStringLiteral("多段上传已合并落地：%1（%2 字节）")
                             .arg(grp.finalPath)
                             .arg(grp.total);

    // 摘要已经在合并那一遍里算完了，这里直接比对：内容不对就把副本删掉再报失败
    const QString own = QString::fromLatin1(hash.result().toHex());
    const QString expected = m_expectedSums.value(transferId);

    // 先报告（reportUploadGroup 要从表里取进度），再删表项
    auto reportAndDrop = [this, transferId](bool ok, const QString &sum, const QString &msg,
                                            bool mismatch = false) {
        reportUploadGroup(transferId, ok, sum, msg, mismatch);
        m_uploadGroups.remove(transferId);
    };

    if (expected.isEmpty()) {
        // 发送端没声明摘要（老客户端）：把本端的值回过去让它自己比
        reportAndDrop(true, own, QString());
        return;
    }
    if (own.compare(expected, Qt::CaseInsensitive) == 0) {
        qInfo().noquote() << QStringLiteral("上传校验通过：%1（%2）")
                                 .arg(grp.displayName, own.left(16));
        reportAndDrop(true, own, QString());
        return;
    }

    qWarning().noquote() << QStringLiteral("上传校验失败：%1（本端 %2，源文件 %3），副本已丢弃")
                                .arg(grp.displayName, own.left(16), expected.left(16));
    QFile::remove(grp.finalPath);
    reportAndDrop(false, own, QStringLiteral("校验失败：收到的内容与源文件不一致"), true);
}

void DataServer::failUploadGroup(const QString &transferId, const QString &reason, bool keepPart)
{
    const auto it = m_uploadGroups.find(transferId);
    if (it == m_uploadGroups.end()) {
        // 表已经不在了（多半是同一份传输被重复报告）：至少留一条痕迹，别静默吞掉
        qWarning().noquote() << QStringLiteral("多段上传失败但已无记录 %1：%2")
                                    .arg(transferId, reason);
        return;
    }
    it->aborted = true;

    qWarning().noquote() << QStringLiteral("多段上传失败 %1：%2（%3）")
                                .arg(it->displayName, reason,
                                     keepPart ? QStringLiteral("已保留断点")
                                              : QStringLiteral("已清除断点"));

    if (!keepPart)
        removeGroupParts(transferId, false);

    reportUploadGroup(transferId, false, QString(), reason);
    m_uploadGroups.erase(m_uploadGroups.find(transferId));

    for (auto it2 = m_reservedPaths.begin(); it2 != m_reservedPaths.end();) {
        if (it2.value() == transferId)
            it2 = m_reservedPaths.erase(it2);
        else
            ++it2;
    }
}

void DataServer::reportUploadGroup(const QString &transferId, bool ok, const QString &sha256,
                                  const QString &message, bool checksumMismatch)
{
    const auto it = m_uploadGroups.find(transferId);
    if (it == m_uploadGroups.end() || it->reported)
        return;

    Qdl::UploadGroup &grp = *it;
    grp.reported = true;

    qint64 received = 0;
    for (qint64 d : grp.done)
        received += d;

    m_expectedSums.remove(transferId);
    emit uploadFinished(transferId, grp.finalPath, grp.displayName,
                        ok ? grp.total : received, ok, sha256, message, checksumMismatch);
}

void DataServer::removeGroupParts(const QString &transferId, bool keepPart)
{
    if (keepPart)
        return;

    const auto it = m_uploadGroups.find(transferId);
    if (it == m_uploadGroups.end())
        return;

    const Qdl::UploadGroup grp = it.value();
    for (int i = 0; i < grp.segCount; ++i)
        QFile::remove(segmentFilePath(grp.partPath, i, grp.segCount));
    QFile::remove(grp.partPath);
    QFile::remove(grp.partPath + QStringLiteral(".key"));
    QFile::remove(grp.partPath + QStringLiteral(".qseg"));
}

bool DataServer::isUploadPathBusy(const QString &finalPath) const
{
    return m_reservedPaths.contains(finalPath);
}

void DataServer::setExpectedChecksum(const QString &transferId, const QString &sha256)
{
    if (transferId.isEmpty())
        return;
    if (sha256.isEmpty())
        m_expectedSums.remove(transferId);
    else
        m_expectedSums.insert(transferId, sha256);
}

void DataServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        new DataStreamer(socket, this, this);
    }
}

void DataServer::purgeExpired()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (auto it = m_grants.begin(); it != m_grants.end();) {
        if (it.value().expires < now) {
            // 过期作废的上传令牌要把它占的名字一起还回去，否则同名文件永远排不上号
            if (it.value().kind == Qdl::DataKind::Upload)
                m_reservedPaths.remove(it.value().finalPath);
            it = m_grants.erase(it);
        } else {
            ++it;
        }
    }
}

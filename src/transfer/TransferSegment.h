#pragma once

#include "TransferTypes.h"

#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QString>

#include <memory>

class QCryptographicHash;
class QTcpSocket;

namespace Qdl {

/**
 * 一条数据连接 = 一个文件的一段（设计文档 §6.2 的扩展）。
 *
 * 为什么把连接封成独立对象：一个任务里可能有 1–8 条连接并行跑，各自有自己的
 * socket、自己的重传窗口和自己那份分片文件。放在 TransferTask 里平铺会让
 * "哪条连接停了/哪段落盘了"变成一堆平行数组，拆出来一个任务只需要管聚合。
 *
 * 它只负责一段：连上、发令牌、按限速收发、发满就报告。落地改名、校验、断点
 * 归属全部留在 TransferTask —— 那里才知道整份文件长什么样。
 */
class TransferSegment : public QObject
{
    Q_OBJECT

public:
    TransferSegment(int index, TransferDirection direction, qint64 offset, qint64 length,
                    qint64 alreadyDone, qint64 fileBase, QObject *parent = nullptr);
    ~TransferSegment() override;

    /**
     * 打开本段对应的文件。
     *
     * 下载：filePath 是本段的分片文件，位置 = 段内偏移 alreadyDone。
     * 上传：filePath 是源文件，位置 = offset + alreadyDone。
     */
    bool openFile(const QString &filePath, QIODevice::OpenMode mode, QString *error);

    /**
     * 边收边算整文件摘要（M13）。
     *
     * 只对"单段、从 0 开始"的下载成立：那种情况下本连接收到的字节顺序就是文件顺序，
     * 一个 QCryptographicHash 顺着喂下去即可。多段并行各收各的、续传的前半段又不在
     * 这条连接里，都不满足"顺序且完整"，那两种情形由 TransferTask 在合并/收尾时算。
     */
    void enableInlineDigest();
    bool inlineDigestEnabled() const { return m_hash != nullptr; }
    /// 收满后才有值（hex）；中途异常返回空串
    QString digestHex() const { return m_digest; }

    void connectTo(const QString &host, quint16 port, const QString &token);
    /// 主动停（暂停/取消）：不发信号，当场放弃
    void abort();

    int index() const { return m_index; }
    QIODevice::OpenMode fileMode() const { return m_fileMode; }
    qint64 offset() const { return m_offset; }
    qint64 fileBase() const { return m_fileBase; }
    qint64 length() const { return m_length; }
    qint64 done() const { return m_done; }
    qint64 remain() const { return qMax<qint64>(0, m_length - m_done); }
    bool isFinished() const { return m_finished; }

    /// 兜底节拍：限速时字节被故意留在内核里，readyRead 不会再来了
    void pump();

public slots:
    void refreshRateLimit();

signals:
    /// 本段又前进了 delta 字节（TransferTask 只做累加，不当成绝对进度）
    void progressed(qint64 delta);
    void segmentFinished();
    void failed(const QString &message);
    /// 第一条连接握手成功（状态从「连接中」转「传输中」）
    void connected();

private:
    void onConnected();
    void onReadyRead();
    void onDisconnected();
    void onSocketError();
    void pumpUpload();
    void pumpDownload();
    /// 连接要关了，但缓冲里还压着"故意没读"的字节：先落盘再判成败
    void drainSocket();
    void schedulePump(qint64 want);
    void finish();
    void fail(const QString &message);
    void closeFile();

    QTcpSocket *m_socket = nullptr;
    QFile m_file;

    int m_index = 0;
    TransferDirection m_direction = TransferDirection::Download;
    QIODevice::OpenMode m_fileMode = QIODevice::ReadOnly;
    QString m_token;
    qint64 m_offset = 0;    // 本段在文件里的逻辑起点（上传读源文件、下载上报进度都用它）
    qint64 m_fileBase = 0;  // 本段在**物理文件**里的起点：分片文件恒为 0，单段 .qdpart 是续传起点
    qint64 m_length = 0;    // 本段总长度
    qint64 m_done = 0;      // 段内已完成

    bool m_finished = false;   // 已报告（完成或失败）
    bool m_stopped = false;    // 主动停，不再发任何信号
    bool m_pumpScheduled = false;

    std::unique_ptr<QCryptographicHash> m_hash;   // 边收边算（可选）
    QString m_digest;
};

} // namespace Qdl

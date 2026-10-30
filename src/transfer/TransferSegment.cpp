#include "TransferSegment.h"

#include "RateLimiter.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QNetworkProxy>
#include <QTcpSocket>
#include <QTimer>

namespace {
constexpr int kUploadChunk = 256 * 1024;
constexpr qint64 kHighWater = 2 * 1024 * 1024;   // 用户态待发上限，超过就等 bytesWritten
constexpr char kMagic[4] = { 'Q', 'D', 'A', 'T' };
} // namespace

namespace Qdl {

TransferSegment::TransferSegment(int index, TransferDirection direction, qint64 offset,
                                 qint64 length, qint64 alreadyDone, qint64 fileBase,
                                 QObject *parent)
    : QObject(parent)
    , m_index(index)
    , m_direction(direction)
    , m_offset(qMax<qint64>(0, offset))
    , m_fileBase(qMax<qint64>(0, fileBase))
    , m_length(qMax<qint64>(0, length))
    , m_done(qBound<qint64>(0, alreadyDone, qMax<qint64>(0, length)))
{
}

TransferSegment::~TransferSegment()
{
    if (m_socket)
        m_socket->abort();
    if (m_file.isOpen())
        m_file.close();
}

bool TransferSegment::openFile(const QString &filePath, QIODevice::OpenMode mode, QString *error)
{
    m_fileMode = mode;
    m_file.setFileName(filePath);

    if (!m_file.open(mode)) {
        if (error)
            *error = m_file.errorString();
        return false;
    }

    // 上传从"段起点 + 段内已发"读；下载写到"物理基准 + 段内已收"。
    // 分片文件的物理基准是 0，单段 .qdpart 的物理基准就是续传起点。
    const qint64 pos = (m_direction == TransferDirection::Upload) ? (m_offset + m_done)
                                                                 : (m_fileBase + m_done);
    if (pos > 0 && !m_file.seek(pos)) {
        if (error)
            *error = tr("定位到偏移 %1 失败").arg(pos);
        return false;
    }

    if (m_direction == TransferDirection::Upload) {
        if (m_file.size() < m_offset + m_length) {
            if (error)
                *error = tr("源文件在传输过程中被截断（期望至少 %1 字节，实际 %2）")
                             .arg(m_offset + m_length)
                             .arg(m_file.size());
            return false;
        }
    }
    return true;
}

void TransferSegment::enableInlineDigest()
{
    if (m_hash || m_direction != TransferDirection::Download)
        return;
    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    m_digest.clear();
}

void TransferSegment::connectTo(const QString &host, quint16 port, const QString &token)
{
    m_token = token;

    if (!m_socket) {
        m_socket = new QTcpSocket(this);
        m_socket->setProxy(QNetworkProxy::NoProxy);   // 绝不能被系统代理接管
        connect(m_socket, &QTcpSocket::connected, this, &TransferSegment::onConnected);
        connect(m_socket, &QTcpSocket::readyRead, this, &TransferSegment::onReadyRead);
        connect(m_socket, &QTcpSocket::disconnected, this, &TransferSegment::onDisconnected);
        connect(m_socket, &QTcpSocket::errorOccurred, this, &TransferSegment::onSocketError);
        connect(m_socket, &QTcpSocket::bytesWritten, this, [this]() {
            if (m_direction == TransferDirection::Upload)
                pumpUpload();
        });
    }

    // 下载：限速靠"少读"。窗口压到略大于突发量，多余字节留在内核里，
    // 接收窗口随之收缩，对端自然慢下来（不需要它配合）
    if (m_direction == TransferDirection::Download)
        m_socket->setReadBufferSize(RateLimiter::instance()->recvWindow(
            TransferDirection::Download));

    m_socket->connectToHost(host, port);
}

void TransferSegment::abort()
{
    m_stopped = true;
    if (m_socket && m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();
    closeFile();
}

void TransferSegment::closeFile()
{
    if (m_file.isOpen()) {
        if (m_direction == TransferDirection::Download)
            m_file.flush();
        m_file.close();
    }
}

void TransferSegment::onConnected()
{
    // 数据通道没有请求帧：连上先自报一次性令牌，之后全是裸字节
    QByteArray header(kMagic, 4);
    header.append(m_token.toLatin1());
    m_socket->write(header);

    if (m_direction == TransferDirection::Upload)
        pumpUpload();

    emit connected();
}

void TransferSegment::onReadyRead()
{
    if (m_finished || m_stopped)
        return;

    if (m_direction == TransferDirection::Upload) {
        m_socket->readAll();   // 上传方向是单向裸流，收到的东西一律忽略
        return;
    }

    pumpDownload();
}

void TransferSegment::pump()
{
    if (m_finished || m_stopped)
        return;
    if (m_direction == TransferDirection::Upload)
        pumpUpload();
    else
        pumpDownload();
}

void TransferSegment::pumpUpload()
{
    if (m_direction != TransferDirection::Upload || !m_socket || m_finished || m_stopped)
        return;
    if (m_socket->state() != QAbstractSocket::ConnectedState)
        return;

    // 限速时把水位一起压低：桶里已经灌满 2MB 的话，再怎么限也限不住那一下突发
    const qint64 water = RateLimiter::instance()->sendWaterMark(TransferDirection::Upload,
                                                                 kHighWater);

    while (m_socket->bytesToWrite() < water && m_done < m_length) {
        const qint64 budget = RateLimiter::instance()->acquire(
            TransferDirection::Upload, qMin<qint64>(kUploadChunk, m_length - m_done));
        if (budget <= 0) {
            schedulePump(kUploadChunk);
            return;
        }

        const QByteArray chunk = m_file.read(budget);
        if (chunk.isEmpty()) {
            fail(tr("读取源文件失败（可能已被移动或截断）：%1").arg(m_file.errorString()));
            return;
        }

        m_socket->write(chunk);
        m_done += chunk.size();
        emit progressed(chunk.size());
    }

    if (m_done >= m_length && m_socket->bytesToWrite() == 0)
        finish();
}

void TransferSegment::pumpDownload()
{
    if (m_direction != TransferDirection::Download || !m_socket || m_finished || m_stopped)
        return;

    const qint64 remain = m_length - m_done;
    if (remain <= 0) {
        finish();
        return;
    }

    const qint64 available = qMin(m_socket->bytesAvailable(), remain);
    if (available <= 0)
        return;

    const qint64 budget = RateLimiter::instance()->acquire(TransferDirection::Download, available);
    if (budget <= 0) {
        // 本轮配额用完：数据留在内核缓冲里（不 read 就是在告诉对端"我收不动了"）
        schedulePump(available);
        return;
    }

    const QByteArray chunk = m_socket->read(budget);
    if (chunk.isEmpty())
        return;

    const qint64 written = m_file.write(chunk);
    if (written != chunk.size()) {
        fail(tr("写入失败（磁盘可能已满）：%1").arg(m_file.errorString()));
        return;
    }

    if (m_hash)
        m_hash->addData(QByteArrayView(chunk.constData(), int(written)));

    m_done += written;
    emit progressed(written);

    if (m_done >= m_length)
        finish();
}

void TransferSegment::drainSocket()
{
    if (m_finished || m_stopped || !m_file.isOpen())
        return;
    if (m_direction != TransferDirection::Download || !m_socket)
        return;

    // 收尾阶段不再限速：这些字节已经在本地了，再等令牌纯属自我惩罚
    while (m_socket->bytesAvailable() > 0 && m_done < m_length) {
        const qint64 remain = m_length - m_done;
        const QByteArray chunk = m_socket->read(qMin<qint64>(m_socket->bytesAvailable(), remain));
        if (chunk.isEmpty())
            break;

        const qint64 written = m_file.write(chunk);
        if (written != chunk.size())
            break;

        if (m_hash)
            m_hash->addData(QByteArrayView(chunk.constData(), int(written)));

        m_done += written;
        emit progressed(written);
    }

    if (m_done >= m_length)
        finish();
}

void TransferSegment::schedulePump(qint64 want)
{
    if (m_pumpScheduled || m_finished || m_stopped)
        return;

    const TransferDirection dir = m_direction;
    const int wait = RateLimiter::instance()->waitMs(dir, want);

    m_pumpScheduled = true;
    // 用 this 作上下文：段对象被销毁时这个回调自动作废
    QTimer::singleShot(wait, this, [this]() {
        m_pumpScheduled = false;
        pump();
    });
}

void TransferSegment::refreshRateLimit()
{
    if (m_direction == TransferDirection::Download && m_socket) {
        m_socket->setReadBufferSize(RateLimiter::instance()->recvWindow(
            TransferDirection::Download));
    }
    pump();
}

void TransferSegment::onDisconnected()
{
    if (m_stopped)
        return;

    drainSocket();
    if (m_finished)
        return;

    // 段内字节已经收齐/发齐，只是对端先关了连接：这是正常收尾
    if (m_done >= m_length) {
        finish();
        return;
    }

    fail(m_direction == TransferDirection::Upload
             ? tr("连接中断，本段已发 %1 / %2").arg(m_done).arg(m_length)
             : tr("连接中断，本段已收 %1 / %2").arg(m_done).arg(m_length));
}

void TransferSegment::onSocketError()
{
    if (m_stopped || m_finished)
        return;

    // 对端正常发完就关（含 0 字节段）时，Qt 先报 RemoteHostClosedError 再报 disconnected。
    // 两条路都要走同一套收尾逻辑，否则最后一段会被误判成失败。
    if (m_socket->error() == QAbstractSocket::RemoteHostClosedError) {
        drainSocket();
        if (m_finished)
            return;
        if (m_done >= m_length) {
            finish();
            return;
        }
        fail(tr("数据连接被对端关闭"));
        return;
    }

    // 上传方向主机主动 abort（关写权限 / 取消）在系统层面常报 ConnectionReset，
    // 和"被中断"是同一件事，断点在主机上，不能判死
    if (m_direction == TransferDirection::Upload) {
        fail(m_socket->errorString());
        return;
    }

    fail(tr("数据连接失败：%1").arg(m_socket->errorString()));
}

void TransferSegment::finish()
{
    if (m_finished || m_stopped)
        return;
    m_finished = true;

    // 字节已全部落盘：摘要此刻在内存里就齐了，不需要再读一遍文件
    if (m_hash)
        m_digest = QString::fromLatin1(m_hash->result().toHex());

    closeFile();
    if (m_socket && m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->disconnectFromHost();

    emit segmentFinished();
}

void TransferSegment::fail(const QString &message)
{
    if (m_finished || m_stopped)
        return;
    m_finished = true;

    closeFile();
    if (m_socket && m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();

    emit failed(message);
}

} // namespace Qdl

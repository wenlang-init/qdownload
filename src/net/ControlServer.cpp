#include "ControlServer.h"

#include "ChecksumCache.h"
#include "DataServer.h"
#include "PairingService.h"
#include "PeerSession.h"
#include "TrustStore.h"

#include <QTcpServer>
#include <QTcpSocket>

namespace {
constexpr quint16 kDefaultPort = 45455;
constexpr int kMaxPortTries = 10;
} // namespace

ControlServer::ControlServer(TrustStore *trust, PairingService *pairing, QObject *parent)
    : QObject(parent)
    , m_trust(trust)
    , m_pairing(pairing)
{
}

ControlServer::~ControlServer()
{
    stop();
}

bool ControlServer::isListening() const
{
    return m_server && m_server->isListening();
}

bool ControlServer::start(quint16 preferredPort, QString *error)
{
    if (isListening())
        return true;

    if (!m_server)
        m_server = new QTcpServer(this);

    // 数据通道随控制通道一起起来：控制端口可以顺延，数据端口同样可以在 45456+
    // 找空位，客户端只会从 DOWNLOAD_ACCEPT 里读到实际端口
    if (!m_checksums)
        m_checksums = new Qdl::ChecksumCache(this);
    if (!m_data)
        m_data = new DataServer(this);
    m_data->setChecksums(m_checksums);   // 上传收完后由它算摘要回给客户端
    if (!m_data->isListening()) {
        QString dataError;
        if (!m_data->start(45456, &dataError))
            qWarning().noquote() << dataError;
    }

    if (!m_newConnectionWired) {
        connect(m_server, &QTcpServer::newConnection, this, &ControlServer::onNewConnection);
        m_newConnectionWired = true;
    }

    // 端口被占用就顺延，实际端口会写进心跳广播（设计文档 §4）
    const quint16 base = preferredPort == 0 ? kDefaultPort : preferredPort;
    for (int offset = 0; offset < kMaxPortTries; ++offset) {
        const quint16 candidate = quint16(base + offset);
        if (m_server->listen(QHostAddress::Any, candidate)) {
            m_host.port = candidate;
            if (offset > 0) {
                qWarning().noquote() << QStringLiteral("控制端口 %1 被占用，改用 %2")
                                            .arg(base).arg(candidate);
            }
            qInfo().noquote() << QStringLiteral("控制通道监听 :%1").arg(candidate);
            emit listeningPortChanged(candidate);
            return true;
        }
    }

    const QString message = tr("控制通道启动失败（%1..%2 均被占用）：%3")
                                .arg(base)
                                .arg(base + kMaxPortTries - 1)
                                .arg(m_server->errorString());
    qWarning().noquote() << message;
    if (error)
        *error = message;
    emit errorOccurred(message);
    return false;
}

void ControlServer::stop()
{
    // closeSession 里会同步 emit closed → onSessionClosed 把会话从 m_sessions 摘掉。
    // 直接 range-for m_sessions 就是边遍历边改容器（迭代器失效，MSVC 下表现为
    // ntdll 0xc0000374 堆破坏）。先换出一份快照，再逐个关。
    const QVector<PeerSession *> sessions = m_sessions;
    m_sessions.clear();
    for (PeerSession *session : sessions)
        session->closeSession(QStringLiteral("服务停止"));

    if (m_data)
        m_data->stop();

    if (m_server && m_server->isListening()) {
        m_server->close();
        qInfo() << "控制通道已停止";
    }
}

quint16 ControlServer::dataPort() const
{
    return m_data ? m_data->port() : 0;
}

void ControlServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        auto *session = new PeerSession(socket, &m_host, m_trust, m_pairing, m_data, m_checksums, this);
        m_sessions.append(session);

        connect(session, &PeerSession::authenticated, this,
                [this](const QString &peerId, const QString &name, const QString &perm) {
                    emit clientAuthenticated(peerId, name, perm);
                });
        connect(session, &PeerSession::closed, this, &ControlServer::onSessionClosed);
    }
}

void ControlServer::onSessionClosed(PeerSession *session, const QString &reason)
{
    const QString peerId = session->peerId();
    const bool wasAuthenticated = session->isAuthenticated();
    m_sessions.removeAll(session);

    if (wasAuthenticated && !peerId.isEmpty())
        emit clientDisconnected(peerId);

    Q_UNUSED(reason)
}

void ControlServer::setHostIdentity(const QString &peerId, const QString &name,
                                    const QString &platform)
{
    m_host.peerId = peerId;
    m_host.name = name;
    m_host.platform = platform;
}

void ControlServer::setShareRoot(const QString &root)
{
    m_host.shareRoot = root;
}

void ControlServer::setShareState(bool shareEnabled, bool writeEnabled, int uploadPolicy)
{
    const bool writeWasOn = m_host.writeEnabled;
    const bool shareWasOn = m_host.shareEnabled;

    m_host.shareEnabled = shareEnabled;
    m_host.writeEnabled = writeEnabled;
    m_host.uploadPolicy = uploadPolicy;

    /**
     * 关掉写权限（或直接关共享）时，进行中的上传必须立刻断，不能"传完这一个"：
     * 开关的语义要干净——用户关掉它就是不想再收字节。断点保留，重新打开后由
     * 对端手动继续（不自动续传，避免违背用户意图，设计文档 §7.4）。
     */
    if (m_data && ((writeWasOn && !writeEnabled) || (shareWasOn && !shareEnabled))) {
        const int aborted = m_data->abortUploads(writeEnabled ? QStringLiteral("主机已关闭共享")
                                                             : QStringLiteral("主机已关闭接收文件"));
        if (aborted > 0)
            qInfo().noquote() << QStringLiteral("写权限/共享已关闭，中断 %1 个上传（.qdpart 已保留，可续传）")
                                     .arg(aborted);
    }

    broadcastPermUpdate();
}

void ControlServer::broadcastPermUpdate()
{
    // 同上：send 出问题可能把会话摘掉，遍历前先拿快照
    const QVector<PeerSession *> sessions = m_sessions;
    for (PeerSession *session : sessions) {
        if (m_sessions.contains(session))
            session->sendPermUpdate();
    }
}

void ControlServer::disconnectPeer(const QString &peerId)
{
    QVector<PeerSession *> targets;
    for (PeerSession *session : std::as_const(m_sessions)) {
        if (session->peerId() == peerId)
            targets.append(session);
    }
    for (PeerSession *session : targets)
        session->closeSession(QStringLiteral("主机撤销了授权"));
}

QStringList ControlServer::connectedPeers() const
{
    QStringList list;
    for (PeerSession *session : std::as_const(m_sessions)) {
        if (session->isAuthenticated() && !session->peerId().isEmpty())
            list.append(session->peerId());
    }
    return list;
}

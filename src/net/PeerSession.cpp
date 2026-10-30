#include "PeerSession.h"

#include "CryptoUtil.h"
#include "DataServer.h"
#include "ChecksumCache.h"
#include "PairingService.h"
#include "TrustStore.h"
#include "fs/FileScanner.h"
#include "fs/SafePath.h"
#include "TransferGrant.h"
#include "transfer/TransferTypes.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QTcpSocket>
#include <QTimer>

#include <utility>

namespace {
constexpr int kHelloTimeoutMs = 10000;
constexpr int kIdleTimeoutMs = 90000;
constexpr int kMaxUploadNameLen = 128;
/// 客户端报的"同时开几条连接"只当作切段稠密度的提示，钳到设置允许的上限
constexpr int kMaxWorkersHint = 8;
/// 一张表的有效期：超过这么久没领过票，说明客户端已经不干了，顺手清掉
constexpr int kPlanTtlMs = 30 * 60 * 1000;
/// 一个会话里最多同时挂几份传输表（防止恶意客户端把内存堆爆）
constexpr int kMaxPlans = 16;

/**
 * 上传文件名清洗。名字来自网络，不能直接落盘：
 *   - 只取最后一段（防 `../../evil.sh` 这类路径穿越）
 *   - 去掉控制字符与 Windows 保留字符（否则在某些平台直接创建失败）
 *   - 去掉结尾的点与空格（Windows 会静默改名，客户端对不上）
 *   - 半成品后缀不允许出现在正式名里（列表里就藏不住了）
 */
QString sanitizeUploadName(const QString &raw)
{
    QString name = QFileInfo(raw).fileName();
    static const QRegularExpression badChars(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]"));
    name.replace(badChars, QStringLiteral("_"));
    name = name.trimmed();

    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))
        name.chop(1);

    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String(".."))
        return {};
    if (name.endsWith(Qdl::FileScanner::kPartSuffix, Qt::CaseInsensitive))
        return {};

    if (name.size() > kMaxUploadNameLen) {
        const int dot = name.lastIndexOf(QLatin1Char('.'));
        QString suffix = dot > 0 ? name.mid(dot) : QString();
        if (suffix.size() > 16)
            suffix.clear();
        name = name.left(kMaxUploadNameLen - suffix.size()) + suffix;
    }
    return name;
}

/// 第 n 个候选名：0 = 原名，1 = `name (1).ext`，以此类推
QString uploadCandidatePath(const QString &dirPath, const QString &desired, int n)
{
    if (n <= 0)
        return QDir(dirPath).filePath(desired);

    const int dot = desired.lastIndexOf(QLatin1Char('.'));
    const QString stem = dot > 0 ? desired.left(dot) : desired;
    const QString suffix = dot > 0 ? desired.mid(dot) : QString();
    return QDir(dirPath).filePath(QStringLiteral("%1 (%2)%3").arg(stem).arg(n).arg(suffix));
}

QString keySidecarPath(const QString &partPath)
{
    return partPath + QStringLiteral(".key");
}

QString readKeySidecar(const QString &partPath)
{
    QFile file(keySidecarPath(partPath));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll()).trimmed();
}

void writeKeySidecar(const QString &partPath, const QString &fileKey)
{
    QFile file(keySidecarPath(partPath));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(fileKey.toUtf8());
}

// ---- 多段上传的元信息：记下"这份 .qdpart 上次切成几段、总量多少" ----
//
// 分片文件大小只能说明每段收到多少，说明不了段是怎么切的。改了段数或源文件
// 大小变了，旧分片就毫无意义；没有这个文件就只能靠猜，而猜错等于拼出坏文件。

QString segMetaPath(const QString &partPath)
{
    return partPath + QStringLiteral(".qseg");
}

bool readSegMeta(const QString &partPath, qint64 *total, int *count, qint64 *chunkSize)
{
    QFile file(segMetaPath(partPath));
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
    QFile file(segMetaPath(partPath));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;

    QJsonObject obj;
    obj.insert(QStringLiteral("total"), double(total));
    obj.insert(QStringLiteral("count"), count);
    obj.insert(QStringLiteral("chunkSize"), double(chunkSize));
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void removeSegments(const QString &partPath, int count)
{
    for (int i = 0; i < qMax(1, count); ++i)
        QFile::remove(partPath + QStringLiteral(".seg%1").arg(i));
    QFile::remove(segMetaPath(partPath));
}

} // namespace

PeerSession::PeerSession(QTcpSocket *socket, const HostContext *host,
                         TrustStore *trust, PairingService *pairing, DataServer *data,
                         Qdl::ChecksumCache *checksums, QObject *parent)
    : QObject(parent)
    , m_socket(socket)
    , m_host(host)
    , m_trust(trust)
    , m_pairing(pairing)
    , m_data(data)
    , m_checksums(checksums)
{
    m_socket->setParent(this);
    connect(m_socket, &QTcpSocket::readyRead, this, &PeerSession::onReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &PeerSession::onDisconnected);

    resetIdleTimer();

    // 连上来不说话就断开，避免半开连接占着 fd
    QTimer::singleShot(kHelloTimeoutMs, this, [this]() {
        if (m_stage == Stage::WaitHello)
            closeSession(QStringLiteral("握手超时"));
    });

    // 主机放行/拒绝时，回调到会话里继续收尾
    connect(m_pairing, &PairingService::approved, this, [this](const QString &peerId) {
        if (peerId == m_clientPeerId && m_pairingInProgress)
            tryCompletePairing();
    });
    connect(m_pairing, &PairingService::rejected, this, [this](const QString &peerId, const QString &reason) {
        if (peerId != m_clientPeerId || !m_pairingInProgress)
            return;
        m_pairingInProgress = false;
        QJsonObject payload;
        payload.insert(QStringLiteral("reason"), reason);
        send(Qdl::MsgType::PairFail, payload);
    });
}

void PeerSession::onReadyRead()
{
    m_buffer.append(m_socket->readAll());
    resetIdleTimer();

    QString error;
    const QVector<Qdl::Frame> frames = Qdl::MessageCodec::decode(m_buffer, &error);
    if (!error.isEmpty()) {
        sendError(400, error);
        closeSession(error);
        return;
    }

    for (const Qdl::Frame &frame : frames)
        handleFrame(frame);
}

void PeerSession::onDisconnected()
{
    emit closed(this, QStringLiteral("对端断开"));
    deleteLater();
}

void PeerSession::handleFrame(const Qdl::Frame &frame)
{
    switch (frame.type) {
    case Qdl::MsgType::Hello:       handleHello(frame.payload); break;
    case Qdl::MsgType::AuthReq:     handleAuthReq(frame.payload); break;
    case Qdl::MsgType::PairReq:     handlePairReq(frame.payload); break;
    case Qdl::MsgType::PairResp:    handlePairResp(frame.payload); break;
    case Qdl::MsgType::Ping:
        send(Qdl::MsgType::Pong, {});
        break;
    case Qdl::MsgType::ListReq:
        handleListReq(frame.payload);
        break;
    case Qdl::MsgType::DownloadReq:
        handleDownloadReq(frame.payload);
        break;
    case Qdl::MsgType::ChecksumReq:
        handleChecksumReq(frame.payload);
        break;
    case Qdl::MsgType::UploadReq:
        handleUploadReq(frame.payload);
        break;
    case Qdl::MsgType::SegReq:
        handleSegReq(frame.payload);
        break;
    case Qdl::MsgType::ChecksumPush:
        handleChecksumPush(frame.payload);
        break;
    case Qdl::MsgType::DeleteReq:
        handleDeleteReq(frame.payload);
        break;
    case Qdl::MsgType::Cancel:
        handleCancel(frame.payload);
        break;
    default:
        qWarning().noquote() << "未知控制帧类型：" << Qdl::MessageCodec::typeName(frame.type)
                             << "来自" << remoteAddress();
        break;
    }
}

void PeerSession::handleHello(const QJsonObject &payload)
{
    m_clientPeerId = payload.value(QStringLiteral("peerId")).toString();
    m_clientName = payload.value(QStringLiteral("name")).toString();
    m_clientPlatform = payload.value(QStringLiteral("platform")).toString();

    if (m_clientPeerId.isEmpty()) {
        sendError(400, QStringLiteral("HELLO 缺少 peerId"));
        closeSession(QStringLiteral("非法 HELLO"));
        return;
    }

    const int protoVer = payload.value(QStringLiteral("protoVer")).toInt(1);
    if (protoVer != 1) {
        sendError(426, QStringLiteral("协议版本不兼容"));
        closeSession(QStringLiteral("协议版本不兼容"));
        return;
    }

    m_nonce = Qdl::randomHex(16);
    m_stage = Stage::WaitAuth;

    QJsonObject ack;
    ack.insert(QStringLiteral("peerId"), m_host->peerId);
    ack.insert(QStringLiteral("name"), m_host->name);
    ack.insert(QStringLiteral("platform"), m_host->platform);
    ack.insert(QStringLiteral("nonce"), m_nonce);
    ack.insert(QStringLiteral("protoVer"), 1);
    ack.insert(QStringLiteral("share"), m_host->shareEnabled);
    ack.insert(QStringLiteral("write"), m_host->writeEnabled);
    ack.insert(QStringLiteral("policy"), m_host->uploadPolicy);
    ack.insert(QStringLiteral("needPair"), !m_trust->client(m_clientPeerId).peerId.isEmpty()
                                            ? false : true);
    send(Qdl::MsgType::HelloAck, ack);
}

void PeerSession::handleAuthReq(const QJsonObject &payload)
{
    if (m_stage != Stage::WaitAuth) {
        sendError(400, QStringLiteral("AUTH_REQ 时机不对"));
        return;
    }

    const TrustStore::Record record = m_trust->client(m_clientPeerId);
    if (record.peerId.isEmpty() || record.blocked) {
        QJsonObject resp;
        resp.insert(QStringLiteral("ok"), false);
        resp.insert(QStringLiteral("code"), 401);
        resp.insert(QStringLiteral("message"), record.blocked ? QStringLiteral("已被拉黑")
                                                             : QStringLiteral("未配对"));
        send(Qdl::MsgType::AuthResp, resp);
        return;
    }

    const QString tokenId = payload.value(QStringLiteral("tokenId")).toString();
    const QString mac = payload.value(QStringLiteral("mac")).toString();
    const QString expected = Qdl::hmacHex(record.token.toUtf8(), m_nonce.toUtf8());

    if (tokenId != record.tokenId || !Qdl::constantTimeEquals(expected, mac)) {
        QJsonObject resp;
        resp.insert(QStringLiteral("ok"), false);
        resp.insert(QStringLiteral("code"), 401);
        resp.insert(QStringLiteral("message"), QStringLiteral("令牌校验失败，请重新配对"));
        send(Qdl::MsgType::AuthResp, resp);
        qWarning().noquote() << QStringLiteral("令牌校验失败：%1 (%2)").arg(record.name, m_clientPeerId);
        return;
    }

    m_authenticated = true;
    m_perm = record.perm.isEmpty() ? QStringLiteral("ro") : record.perm;
    m_stage = Stage::Authenticated;
    m_trust->touchClient(m_clientPeerId);

    QJsonObject resp;
    resp.insert(QStringLiteral("ok"), true);
    resp.insert(QStringLiteral("perm"), m_perm);
    resp.insert(QStringLiteral("name"), m_host->name);
    resp.insert(QStringLiteral("writeEnabled"), m_host->writeEnabled);
    resp.insert(QStringLiteral("policy"), m_host->uploadPolicy);
    resp.insert(QStringLiteral("canUpload"), m_host->canUploadFor(m_perm));
    resp.insert(QStringLiteral("sharing"), m_host->shareEnabled);
    send(Qdl::MsgType::AuthResp, resp);

    qInfo().noquote() << QStringLiteral("设备已连接：%1 (%2) perm=%3")
                             .arg(m_clientName, m_clientPeerId, m_perm);

    // 连上就补一条权限状态，客户端的上传按钮不用等下一次变更
    sendPermUpdate();
    emit authenticated(m_clientPeerId, m_clientName, m_perm);
}

void PeerSession::handlePairReq(const QJsonObject &payload)
{
    if (m_stage != Stage::WaitAuth) {
        sendError(400, QStringLiteral("PAIR_REQ 时机不对"));
        return;
    }
    if (!m_host->shareEnabled) {
        sendError(403, QStringLiteral("对方未开启共享"));
        return;
    }

    const QString peerId = payload.value(QStringLiteral("peerId")).toString();
    const QString name = payload.value(QStringLiteral("name")).toString();

    if (!peerId.isEmpty() && peerId != m_clientPeerId) {
        sendError(400, QStringLiteral("PAIR_REQ 的 peerId 与 HELLO 不一致"));
        return;
    }

    if (!m_pairing->begin(m_clientPeerId, name.isEmpty() ? m_clientName : name,
                          m_clientPlatform, remoteAddress())) {
        sendError(429, QStringLiteral("配对尝试过多，请稍后再试"));
        return;
    }

    m_pairingInProgress = true;
    m_pairNonce = m_pairing->pending(m_clientPeerId) ? m_pairing->pending(m_clientPeerId)->nonce
                                                     : QString();

    QJsonObject challenge;
    challenge.insert(QStringLiteral("nonce"), m_pairNonce);
    challenge.insert(QStringLiteral("expiresIn"), PairingService::kCodeTtlMs / 1000);
    send(Qdl::MsgType::PairChallenge, challenge);
}

void PeerSession::handlePairResp(const QJsonObject &payload)
{
    if (!m_pairingInProgress) {
        sendError(400, QStringLiteral("没有进行中的配对请求"));
        return;
    }

    const QString nonce = payload.value(QStringLiteral("nonce")).toString();
    const QString mac = payload.value(QStringLiteral("mac")).toString();

    int remaining = 0;
    const QString reason = m_pairing->verify(m_clientPeerId, nonce, mac, &remaining);

    if (!reason.isEmpty()) {
        QJsonObject fail;
        fail.insert(QStringLiteral("reason"), reason);
        fail.insert(QStringLiteral("remaining"), remaining);
        send(Qdl::MsgType::PairFail, fail);

        if (reason == QLatin1String("locked") || reason == QLatin1String("expired")
            || reason == QLatin1String("no_request")) {
            m_pairingInProgress = false;
        }
        return;
    }

    tryCompletePairing();
}

void PeerSession::tryCompletePairing()
{
    const PairingService::Pending *pending = m_pairing->pending(m_clientPeerId);
    if (!pending || !m_pairingInProgress)
        return;

    // 码对了还要主人放行，两者缺一不可
    if (!pending->verified || !pending->approved)
        return;

    completePairing();
}

void PeerSession::completePairing()
{
    const PairingService::Pending *pending = m_pairing->pending(m_clientPeerId);
    if (!pending)
        return;

    TrustStore::Record record;
    record.peerId = m_clientPeerId;
    record.name = pending->name;
    record.tokenId = QStringLiteral("tk-") + Qdl::randomHex(4);
    record.token = Qdl::randomToken();
    record.perm = pending->useRw ? QStringLiteral("rw") : QStringLiteral("ro");

    if (!m_trust->upsertClient(record)) {
        sendError(500, QStringLiteral("写入信任库失败"));
        return;
    }

    m_pairingInProgress = false;
    m_pairing->clear(m_clientPeerId);

    QJsonObject ok;
    ok.insert(QStringLiteral("tokenId"), record.tokenId);
    ok.insert(QStringLiteral("token"), record.token);
    ok.insert(QStringLiteral("perm"), record.perm);
    ok.insert(QStringLiteral("name"), m_host->name);
    ok.insert(QStringLiteral("writeEnabled"), m_host->writeEnabled);
    ok.insert(QStringLiteral("policy"), m_host->uploadPolicy);
    ok.insert(QStringLiteral("canUpload"), m_host->canUploadFor(record.perm));
    ok.insert(QStringLiteral("sharing"), m_host->shareEnabled);
    send(Qdl::MsgType::PairOk, ok);

    m_authenticated = true;
    m_perm = record.perm;
    m_stage = Stage::Authenticated;

    qInfo().noquote() << QStringLiteral("配对完成：%1 (%2) perm=%3")
                             .arg(record.name, record.peerId, record.perm);

    emit authenticated(m_clientPeerId, m_clientName, m_perm);
}

void PeerSession::handleListReq(const QJsonObject &payload)
{
    const QString reqId = payload.value(QStringLiteral("reqId")).toString();

    if (!m_authenticated) {
        sendError(401, QStringLiteral("未配对，请先配对"), reqId);
        return;
    }
    if (!m_host->shareEnabled) {
        sendError(403, QStringLiteral("对方已关闭共享"), reqId);
        return;
    }

    const QString requestPath = payload.value(QStringLiteral("path")).toString(QStringLiteral("/"));
    const int offset = qMax(0, payload.value(QStringLiteral("offset")).toInt(0));
    const int limit = payload.value(QStringLiteral("limit")).toInt(Qdl::FileScanner::kDefaultLimit);

    QString error;
    const QString absolute = Qdl::SafePath::resolve(m_host->shareRoot, requestPath, &error);
    if (absolute.isEmpty()) {
        qWarning().noquote() << QStringLiteral("拒绝目录请求 %1：%2").arg(requestPath, error);
        // 路径不存在按 404 回，越界/非法按 400 回，客户端可据此区分“没这个目录”和“被拦了”
        sendError(error.contains(QStringLiteral("不存在")) ? 404 : 400, error, reqId);
        return;
    }

    if (!QFileInfo(absolute).isDir()) {
        sendError(400, QStringLiteral("不是目录"), reqId);
        return;
    }

    QVector<Qdl::FileEntry> page;
    int total = 0;
    if (!Qdl::FileScanner::list(absolute, offset, limit, &page, &total, &error)) {
        sendError(500, error, reqId);
        return;
    }

    // 回填规范化后的虚拟路径：客户端面包屑据此重建，不用自己猜
    const QString canonicalPath = Qdl::SafePath::toVirtual(m_host->shareRoot, absolute);

    QJsonArray entries;
    for (const Qdl::FileEntry &entry : std::as_const(page))
        entries.append(entry.toJson());

    QJsonObject resp;
    resp.insert(QStringLiteral("reqId"), reqId);
    resp.insert(QStringLiteral("path"), canonicalPath);
    resp.insert(QStringLiteral("offset"), offset);
    resp.insert(QStringLiteral("total"), total);
    resp.insert(QStringLiteral("entries"), entries);
    send(Qdl::MsgType::ListResp, resp);

    qInfo().noquote() << QStringLiteral("列目录 %1 → %2 项（本次 %3 条，offset=%4）")
                             .arg(canonicalPath)
                             .arg(total)
                             .arg(page.size())
                             .arg(offset);
}

void PeerSession::handleDownloadReq(const QJsonObject &payload)
{
    const QString reqId = payload.value(QStringLiteral("reqId")).toString();

    if (!m_authenticated) {
        sendError(401, QStringLiteral("未配对，请先配对"), reqId);
        return;
    }
    if (!m_host->shareEnabled) {
        sendError(403, QStringLiteral("对方已关闭共享"), reqId);
        return;
    }
    if (!m_data) {
        sendError(500, QStringLiteral("数据通道未启动"), reqId);
        return;
    }

    const QString requestPath = payload.value(QStringLiteral("path")).toString(QStringLiteral("/"));
    const qint64 offset = qMax<qint64>(0, qint64(payload.value(QStringLiteral("offset")).toDouble(0)));

    QString error;
    const QString absolute = Qdl::SafePath::resolve(m_host->shareRoot, requestPath, &error);
    if (absolute.isEmpty()) {
        qWarning().noquote() << QStringLiteral("拒绝下载请求 %1：%2").arg(requestPath, error);
        sendError(error.contains(QStringLiteral("不存在")) ? 404 : 400, error, reqId);
        return;
    }

    const QFileInfo info(absolute);
    if (!info.isFile()) {
        sendError(400, QStringLiteral("不是文件"), reqId);
        return;
    }

    const qint64 size = info.size();
    if (offset > size) {
        sendError(400, QStringLiteral("续传偏移 %1 超出文件大小 %2").arg(offset).arg(size), reqId);
        return;
    }

    // fileKey 在这里算一次：列表里不带它（200 项 × 2MB 会卡死），
    // 但真正开始传输时必须给客户端一个能识别"是不是同一个文件"的指纹（M6 续传要用）
    QString keyError;
    const QString fileKey = Qdl::FileScanner::fileKey(absolute, &keyError);
    if (fileKey.isEmpty())
        qWarning().noquote() << QStringLiteral("计算 fileKey 失败：%1").arg(keyError);

    // 续传的身份校验：客户端带着上次记下的 fileKey 来，说明它手上有一份半成品。
    // 现在按同样的算法重算一遍，对不上就是"源文件已经被换掉了"——继续写只会把
    // 两个不同文件的片段拼在一起，比失败更糟。让客户端从头下（409）。
    //
    // 分段后"有半成品"不再等于 offset > 0（多段的进度分散在分片文件里），
    // 所以只要客户端提到任何进度，就必须过这道身份校验。
    QVector<qint64> segmentDone;
    const QJsonArray doneArr = payload.value(QStringLiteral("segmentDone")).toArray();
    for (const QJsonValue &v : doneArr)
        segmentDone.append(qint64(v.toDouble(0)));

    bool hasProgress = offset > 0;
    for (qint64 d : segmentDone)
        hasProgress = hasProgress || d > 0;

    const QString expectedKey = payload.value(QStringLiteral("fileKey")).toString();
    if (hasProgress && !expectedKey.isEmpty() && !fileKey.isEmpty()
        && expectedKey != fileKey) {
        sendError(409, QStringLiteral("源文件已变化（fileKey 不一致），请重新下载"), reqId);
        qWarning().noquote() << QStringLiteral("续传被拒：%1 的 fileKey 与请求不符").arg(requestPath);
        return;
    }

    const QString transferId = QStringLiteral("t-") + Qdl::randomHex(6);

    // 分段表：客户端报的 segments 只是"同时开几条连接"的提示，用来决定段切多细。
    // 段表本身只有这里算一份，客户端照着表一段段领活干——两端各算一遍迟早会
    // 因为取整差异拼出坏文件，而且只在最后校验时才暴露。
    //
    // 续传时客户端会带上上次的 chunkSize：段划分必须与盘上的分片一致，
    // 否则分片大小对不上号（改了连接数就整份重来，代价太大）。
    const int workers = qBound(1, payload.value(QStringLiteral("segments")).toInt(1),
                               kMaxWorkersHint);
    qint64 chunkSize = qint64(payload.value(QStringLiteral("chunkSize")).toDouble(0));
    if (chunkSize <= 0)
        chunkSize = Qdl::chunkSizeFor(size, workers);
    chunkSize = qBound(Qdl::kMinChunkBytes, chunkSize, qMax(Qdl::kMaxChunkBytes, size));

    QVector<Qdl::ChunkPlan> plans;
    if (size > 0) {
        plans = Qdl::planChunks(size, chunkSize);
        // 续传提示的段长要能被验证：段数对不上就说明客户端记的和这次算的不是一回事
        const int wantCount = payload.value(QStringLiteral("segmentCount")).toInt(0);
        if (wantCount > 0 && wantCount != plans.size() && !segmentDone.isEmpty()) {
            qWarning().noquote() << QStringLiteral("续传段表与本地记录不一致（%1 → %2），从 0 重下：%3")
                                        .arg(wantCount)
                                        .arg(plans.size())
                                        .arg(info.fileName());
            segmentDone.clear();
            // 注意：这里只丢客户端的进度声明，客户端会按返回的段表自行作废旧分片
        }
    }

    // 表里的 done：下载进度只有客户端知道（它写分片文件），主机照抄
    QVector<qint64> doneBytes;
    doneBytes.fill(0, plans.size());
    for (int i = 0; i < plans.size() && i < segmentDone.size(); ++i)
        doneBytes[i] = qBound<qint64>(0, segmentDone[i], plans[i].length);

    PeerSession::TransferPlan table;
    table.upload = false;
    table.absolutePath = absolute;
    table.displayName = info.fileName();
    table.total = size;
    table.chunkSize = chunkSize;
    table.plans = plans;
    table.done = doneBytes;
    table.checksumKey = fileKey;
    table.updated = QDateTime::currentDateTimeUtc();

    // 摘要提前算（M13）：受理请求这一刻就把整文件 SHA-256 排到后台线程池，
    // 而不是等客户端收完再回来问。命中缓存（同一文件传过）当场就能给；
    // 没命中也已经开算，传完时它必然已经躺在缓存里——收尾那一下就只剩比对。
    if (m_checksums) {
        wireChecksumReady();
        table.sha256 = m_checksums->cachedOrRequest(fileKey, absolute);
    }

    QJsonArray segArr;
    for (int i = 0; i < plans.size(); ++i) {
        QJsonObject entry;
        entry.insert(QStringLiteral("index"), plans[i].index);
        entry.insert(QStringLiteral("offset"), double(plans[i].offset));
        entry.insert(QStringLiteral("length"), double(plans[i].length));
        entry.insert(QStringLiteral("done"), double(doneBytes[i]));
        segArr.append(entry);
    }

    // 第一段先把票发下去：单段文件（很常见）靠它省掉一次 SEG_REQ 往返
    QString firstToken;
    qint64 firstOffset = offset;
    qint64 firstLength = qMax<qint64>(0, size - offset);
    if (!plans.isEmpty()) {
        firstOffset = plans[0].offset + doneBytes[0];
        firstLength = plans[0].length - doneBytes[0];
        firstToken = m_data->grant(QStringLiteral("%1-0").arg(transferId), absolute,
                                   firstOffset, firstLength);
        if (firstToken.isEmpty()) {
            sendError(500, QStringLiteral("数据通道不可用"), reqId);
            return;
        }
    }

    prunePlans();
    m_plans.insert(transferId, table);

    QJsonObject resp;
    resp.insert(QStringLiteral("reqId"), reqId);
    resp.insert(QStringLiteral("transferId"), transferId);
    // 顶层那几个字段留着给老客户端（它们只认这套单段字段）
    resp.insert(QStringLiteral("token"), firstToken);
    resp.insert(QStringLiteral("offset"), double(firstOffset));
    resp.insert(QStringLiteral("segments"), segArr);
    resp.insert(QStringLiteral("chunkSize"), double(chunkSize));
    resp.insert(QStringLiteral("segmentCount"), plans.size());
    resp.insert(QStringLiteral("port"), int(m_data->port()));
    resp.insert(QStringLiteral("name"), info.fileName());
    resp.insert(QStringLiteral("size"), double(size));
    resp.insert(QStringLiteral("fileKey"), fileKey);
    // 摘要随表下发：拿到就是"当场可用"，没拿到（还在算）随后由 CHECKSUM_PUSH 补
    if (!table.sha256.isEmpty())
        resp.insert(QStringLiteral("sha256"), table.sha256);
    send(Qdl::MsgType::DownloadAccept, resp);

    qInfo().noquote() << QStringLiteral("下发 %1（%2 字节，从 %3 起，%4 段 × %5%6）→ %7:%8")
                             .arg(info.fileName())
                             .arg(size)
                             .arg(offset)
                             .arg(plans.size())
                             .arg(Qdl::TransferFmt::sizeText(chunkSize))
                             .arg(table.sha256.isEmpty()
                                      ? QStringLiteral("，摘要计算中")
                                      : QStringLiteral("，摘要已随表下发"))
                             .arg(m_socket->peerAddress().toString())
                             .arg(m_data->port());
}

void PeerSession::handleUploadReq(const QJsonObject &payload)
{
    const QString reqId = payload.value(QStringLiteral("reqId")).toString();

    if (!m_authenticated) {
        sendError(401, QStringLiteral("未配对，请先配对"), reqId);
        return;
    }
    if (!m_host->shareEnabled) {
        sendError(403, QStringLiteral("对方已关闭共享"), reqId);
        return;
    }
    // 三级短路判定的最后一跳（设计文档 §7.4）：总开关关 → 一律 403，无视白名单
    if (!m_host->canUploadFor(m_perm)) {
        sendError(403,
                  m_host->writeEnabled
                      ? QStringLiteral("未获写权限（只读设备不能上传）")
                      : QStringLiteral("主机已关闭接收文件"),
                  reqId);
        return;
    }
    if (!m_data) {
        sendError(500, QStringLiteral("数据通道未启动"), reqId);
        return;
    }

    const QString name = sanitizeUploadName(payload.value(QStringLiteral("name")).toString());
    if (name.isEmpty()) {
        sendError(400, QStringLiteral("文件名不合法"), reqId);
        return;
    }

    const qint64 size = qint64(payload.value(QStringLiteral("size")).toDouble(-1));
    if (size < 0) {
        sendError(400, QStringLiteral("UPLOAD_REQ 缺少文件大小"), reqId);
        return;
    }

    // 落点固定 <shareRoot>/inbox：客户端没有任何办法指定别的目录
    if (!QDir().mkpath(QDir(m_host->shareRoot).filePath(QStringLiteral("inbox")))) {
        sendError(500, QStringLiteral("无法创建收件目录"), reqId);
        return;
    }
    QString pathError;
    const QString inboxDir = Qdl::SafePath::resolve(m_host->shareRoot, QStringLiteral("/inbox"),
                                                    &pathError);
    if (inboxDir.isEmpty()) {
        sendError(500, pathError.isEmpty() ? QStringLiteral("收件目录不可用") : pathError, reqId);
        return;
    }

    // 名字分配要同时看两件事：磁盘上有没有同名、有没有别人正在往同名的半成品里写。
    // 只看文件的话，两个同名文件并发上传会选中同一个落点，交错写成一个坏文件。
    QString finalPath;
    for (int n = 0; n < 1000; ++n) {
        const QString candidate = uploadCandidatePath(inboxDir, name, n);
        const bool busy = m_data->isUploadPathBusy(candidate);
        // 多段上传落地前根本没有 `.qdpart` 本体（只有 .seg<i> + .qseg），
        // 只认 `.qdpart` 会让"暂停过的多段上传"永远认不出断点、换个名字重传。
        const bool hasPart = QFile::exists(candidate + Qdl::FileScanner::kPartSuffix)
            || QFile::exists(candidate + Qdl::FileScanner::kPartSuffix
                             + QStringLiteral(".qseg"));

        // 有半成品且没人在写 → 就是同一份文件的上次断点，直接续
        if (hasPart && !busy) {
            finalPath = candidate;
            break;
        }
        if (!hasPart && !QFile::exists(candidate) && !busy) {
            finalPath = candidate;
            break;
        }
    }
    if (finalPath.isEmpty()) {
        sendError(409, QStringLiteral("同名文件过多，请改名后再传"), reqId);
        return;
    }

    const QString finalName = QFileInfo(finalPath).fileName();
    const QString partPath = finalPath + Qdl::FileScanner::kPartSuffix;
    const QString clientKey = payload.value(QStringLiteral("fileKey")).toString();

    // 段表：与下载侧同一套算法，只是 done 的来源不同——上传的进度是**主机**
    // 盘上的分片文件有多大，客户端说了不算。
    const int workers = qBound(1, payload.value(QStringLiteral("segments")).toInt(1),
                               kMaxWorkersHint);
    qint64 chunkSize = qint64(payload.value(QStringLiteral("chunkSize")).toDouble(0));
    if (chunkSize <= 0)
        chunkSize = Qdl::chunkSizeFor(size, workers);
    chunkSize = qBound(Qdl::kMinChunkBytes, chunkSize, qMax(Qdl::kMaxChunkBytes, size));

    // 段划分必须与上次一致，否则盘上的分片大小对不上号（改了连接数就整份重来）
    qint64 metaTotal = 0;
    int metaCount = 0;
    qint64 metaChunk = 0;
    const bool hasMeta = readSegMeta(partPath, &metaTotal, &metaCount, &metaChunk);
    if (hasMeta && metaChunk > 0 && metaTotal == size && metaChunk != chunkSize) {
        // 客户端要求沿用旧段长（续传），那就照它说的切——反正表还是由这里算
        chunkSize = qBound(Qdl::kMinChunkBytes, metaChunk, qMax(Qdl::kMaxChunkBytes, size));
    }

    QVector<Qdl::ChunkPlan> plans;
    if (size > 0)
        plans = Qdl::planChunks(size, chunkSize);

    if (hasMeta && (metaCount != plans.size() || metaTotal != size)) {
        qWarning().noquote() << QStringLiteral("分段表与上次不同（%1 → %2，%3 → %4 字节），清除旧分片重传：%5")
                                    .arg(metaCount)
                                    .arg(plans.size())
                                    .arg(metaTotal)
                                    .arg(size)
                                    .arg(finalName);
        removeSegments(partPath, metaCount);
    }
    if (plans.size() > 1 && !clientKey.isEmpty()) {
        const QString storedKey = readKeySidecar(partPath);
        if (!storedKey.isEmpty() && storedKey != clientKey) {
            qWarning().noquote() << QStringLiteral("同名断点的 fileKey 不一致，作废旧分片重收：%1")
                                        .arg(finalName);
            removeSegments(partPath, qMax(1, metaCount > 0 ? metaCount : plans.size()));
        }
    }
    if (plans.size() > 1)
        writeSegMeta(partPath, size, plans.size(), chunkSize);

    // 续传位点：单段看 .qdpart 的大小，多段看各分片的大小
    QVector<qint64> segDone;
    segDone.fill(0, plans.size());
    qint64 offset = 0;
    if (plans.size() > 1) {
        for (int i = 0; i < plans.size(); ++i) {
            const QFileInfo segInfo(DataServer::segmentFilePath(partPath, i, plans.size()));
            if (segInfo.exists())
                segDone[i] = qBound<qint64>(0, segInfo.size(), plans[i].length);
            offset += segDone[i];
        }
    } else {
        const QFileInfo partInfo(partPath);
        if (partInfo.exists()) {
            const QString storedKey = readKeySidecar(partPath);
            if (!storedKey.isEmpty() && !clientKey.isEmpty() && storedKey != clientKey) {
                qWarning().noquote() << QStringLiteral("同名断点的 fileKey 不一致，作废重收：%1")
                                            .arg(finalName);
                QFile::remove(partPath);
                QFile::remove(keySidecarPath(partPath));
                offset = 0;
            } else {
                offset = qBound<qint64>(0, partInfo.size(), size);
            }
        }
        if (!segDone.isEmpty())
            segDone[0] = offset;
    }
    if (!clientKey.isEmpty() && readKeySidecar(partPath) != clientKey)
        writeKeySidecar(partPath, clientKey);

    const qint64 remain = size - offset;
    if (remain > 0) {
        // 剩余空间要够写两遍（qdpart + 可能的失败重试），否则会让主机磁盘写爆
        const QStorageInfo storage(inboxDir);
        if (storage.isValid() && storage.bytesAvailable() >= 0
            && storage.bytesAvailable() < remain * 2) {
            sendError(507, QStringLiteral("主机磁盘空间不足"), reqId);
            return;
        }
    }

    const QString transferId = QStringLiteral("u-") + Qdl::randomHex(6);

    PeerSession::TransferPlan table;
    table.upload = true;
    table.absolutePath = partPath;
    table.finalPath = finalPath;
    table.displayName = finalName;
    table.total = size;
    table.chunkSize = chunkSize;
    table.plans = plans;
    table.done = segDone;
    table.updated = QDateTime::currentDateTimeUtc();

    // 上传方向反过来：摘要由**发送端**（客户端）在开工时后台算，算好后随
    // SEG_REQ 或 CHECKSUM_PUSH 送过来（见 handleSegReq / handleChecksumPush），
    // 主机落地时拿它比对收到的内容。请求这一刻它多半还没算完，所以这里不读。

    QJsonArray segArr;
    for (int i = 0; i < plans.size(); ++i) {
        QJsonObject entry;
        entry.insert(QStringLiteral("index"), plans[i].index);
        entry.insert(QStringLiteral("offset"), double(plans[i].offset));
        entry.insert(QStringLiteral("length"), double(plans[i].length));
        entry.insert(QStringLiteral("done"), double(segDone[i]));
        segArr.append(entry);
    }

    // 第一段的票先发：单段上传靠它省掉一次 SEG_REQ 往返；多段时也顺手把落点占位
    // （m_reservedPaths 在签发令牌时建立，占位早一步，同名并发上传才不会撞车）
    QString firstToken;
    qint64 firstOffset = offset;
    qint64 firstLength = qMax<qint64>(0, size - offset);
    QVector<qint64> segLengths;
    for (const Qdl::ChunkPlan &plan : plans)
        segLengths.append(plan.length);
    if (!plans.isEmpty()) {
        firstOffset = plans[0].offset + segDone[0];
        firstLength = plans[0].length - segDone[0];
    }
    firstToken = m_data->grantUploadSegment(transferId, 0, qMax(1, plans.size()), partPath,
                                            finalPath, finalName, firstOffset, firstLength,
                                            size, segLengths,
                                            plans.isEmpty() ? 0 : plans[0].offset);
    if (firstToken.isEmpty()) {
        sendError(500, QStringLiteral("数据通道不可用"), reqId);
        return;
    }

    wireUploadFinished();
    m_uploadIds.insert(transferId);

    // 0 字节文件没有段，也就不会有 SEG_REQ 来查表：留着只会等 TTL 回收
    if (size > 0) {
        prunePlans();
        m_plans.insert(transferId, table);
    }

    QJsonObject resp;
    resp.insert(QStringLiteral("reqId"), reqId);
    resp.insert(QStringLiteral("transferId"), transferId);
    resp.insert(QStringLiteral("token"), firstToken);
    resp.insert(QStringLiteral("segments"), segArr);
    resp.insert(QStringLiteral("chunkSize"), double(chunkSize));
    resp.insert(QStringLiteral("segmentCount"), plans.size());
    resp.insert(QStringLiteral("port"), int(m_data->port()));
    resp.insert(QStringLiteral("name"), finalName);
    resp.insert(QStringLiteral("path"),
                Qdl::SafePath::toVirtual(m_host->shareRoot, finalPath));
    resp.insert(QStringLiteral("size"), double(size));
    resp.insert(QStringLiteral("offset"), double(firstOffset));
    send(Qdl::MsgType::UploadAccept, resp);

    qInfo().noquote() << QStringLiteral("接收上传 %1（%2 字节，从 %3 起，%4 段 × %5）← %6")
                             .arg(finalName)
                             .arg(size)
                             .arg(offset)
                             .arg(plans.size())
                             .arg(Qdl::TransferFmt::sizeText(chunkSize))
                             .arg(m_clientName);

    // 0 字节：没有字节要收，票发出去也没人会来连。直接把空文件落地、回 UPLOAD_DONE，
    // 否则就是今天这个死局——客户端"发完"就干等，收件箱里躺一个没人收尾的 .qdpart.key。
    if (size == 0)
        m_data->finishEmptyUpload(transferId, partPath, finalPath, finalName);
}

void PeerSession::handleSegReq(const QJsonObject &payload)
{
    const QString reqId = payload.value(QStringLiteral("reqId")).toString();
    const QString transferId = payload.value(QStringLiteral("transferId")).toString();
    const int index = payload.value(QStringLiteral("index")).toInt(-1);

    auto deny = [this, &reqId](int code, const QString &message) {
        QJsonObject resp;
        resp.insert(QStringLiteral("reqId"), reqId);
        resp.insert(QStringLiteral("ok"), false);
        resp.insert(QStringLiteral("code"), code);
        resp.insert(QStringLiteral("message"), message);
        send(Qdl::MsgType::SegResp, resp);
    };

    if (!m_authenticated) {
        deny(401, QStringLiteral("未配对，请先配对"));
        return;
    }
    if (!m_host->shareEnabled) {
        deny(403, QStringLiteral("对方已关闭共享"));
        return;
    }
    if (!m_data) {
        deny(500, QStringLiteral("数据通道未启动"));
        return;
    }

    const auto it = m_plans.find(transferId);
    if (it == m_plans.end()) {
        // 主机重启过、或这份表已经超时被清掉：让客户端重新走一次 REQ
        deny(410, QStringLiteral("传输已失效，请重新发起"));
        return;
    }

    TransferPlan &plan = it.value();
    if (index < 0 || index >= plan.plans.size()) {
        deny(400, QStringLiteral("段号 %1 超出范围").arg(index));
        return;
    }

    // 摘要搭段表的顺风车（M13）：上传方向客户端算好源文件摘要后，随任意一次
    // SEG_REQ 捎上来即可，不必再单独往返一次控制帧。同一个摘要重复到达无副作用。
    const QString pushedSum = payload.value(QStringLiteral("sha256")).toString();
    if (plan.upload && !pushedSum.isEmpty() && plan.expectedSha256 != pushedSum) {
        plan.expectedSha256 = pushedSum;
        m_data->setExpectedChecksum(transferId, pushedSum);
        qInfo().noquote() << QStringLiteral("收到上传摘要 %1：%2")
                                 .arg(plan.displayName, pushedSum.left(16));
    }

    const Qdl::ChunkPlan &chunk = plan.plans[index];
    qint64 done = plan.done.value(index, 0);

    if (plan.upload) {
        // 上传的权威进度是**主机盘上**那个分片文件有多大，客户端说"我发到哪了"不算数
        const QString segPath = DataServer::segmentFilePath(plan.absolutePath, index,
                                                            plan.plans.size());
        const QFileInfo segInfo(segPath);
        done = segInfo.exists() ? qBound<qint64>(0, segInfo.size(), chunk.length) : 0;
        plan.done[index] = done;
    } else {
        // 下载的进度只有客户端知道（它在写分片文件），取两边更靠前的那个
        const qint64 clientDone = qint64(payload.value(QStringLiteral("done")).toDouble(0));
        done = qMax(done, qBound<qint64>(0, clientDone, chunk.length));
        plan.done[index] = done;
    }

    if (chunk.length > 0 && done >= chunk.length) {
        // 这一段其实已经齐了（上次收满但没来得及记），直接告诉客户端跳过
        QJsonObject resp;
        resp.insert(QStringLiteral("reqId"), reqId);
        resp.insert(QStringLiteral("transferId"), transferId);
        resp.insert(QStringLiteral("ok"), true);
        resp.insert(QStringLiteral("complete"), true);
        resp.insert(QStringLiteral("index"), index);
        resp.insert(QStringLiteral("done"), double(chunk.length));
        resp.insert(QStringLiteral("offset"), double(chunk.offset + chunk.length));
        resp.insert(QStringLiteral("length"), 0.0);
        if (!plan.sha256.isEmpty())
            resp.insert(QStringLiteral("sha256"), plan.sha256);
        send(Qdl::MsgType::SegResp, resp);
        return;
    }

    const qint64 startAt = chunk.offset + done;
    const qint64 remain = chunk.length - done;

    QString token;
    if (plan.upload) {
        QVector<qint64> lengths;
        lengths.reserve(plan.plans.size());
        for (const Qdl::ChunkPlan &p : std::as_const(plan.plans))
            lengths.append(p.length);

        // 段数 / 分段尺寸沿用表里的：DataServer 靠它判断"全部到齐才合并"
        token = m_data->grantUploadSegment(transferId, index, plan.plans.size(),
                                           plan.absolutePath, plan.finalPath, plan.displayName,
                                           startAt, remain, plan.total, lengths, chunk.offset);
    } else {
        token = m_data->grant(QStringLiteral("%1-%2").arg(transferId).arg(index),
                              plan.absolutePath, startAt, remain);
    }

    if (token.isEmpty()) {
        deny(500, QStringLiteral("数据通道不可用"));
        return;
    }

    plan.updated = QDateTime::currentDateTimeUtc();

    QJsonObject resp;
    resp.insert(QStringLiteral("reqId"), reqId);
    resp.insert(QStringLiteral("transferId"), transferId);
    resp.insert(QStringLiteral("ok"), true);
    resp.insert(QStringLiteral("complete"), remain <= 0);
    resp.insert(QStringLiteral("index"), index);
    resp.insert(QStringLiteral("token"), token);
    resp.insert(QStringLiteral("offset"), double(startAt));
    resp.insert(QStringLiteral("length"), double(remain));
    resp.insert(QStringLiteral("done"), double(done));
    // 下载：摘要算好了就搭这次应答一起带过去（客户端可能还没收到推送）
    if (!plan.sha256.isEmpty())
        resp.insert(QStringLiteral("sha256"), plan.sha256);
    send(Qdl::MsgType::SegResp, resp);

    qDebug().noquote() << QStringLiteral("发牌 %1 第 %2 段：从 %3 起 %4 字节")
                              .arg(transferId)
                              .arg(index)
                              .arg(startAt)
                              .arg(remain);
}

void PeerSession::prunePlans()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (auto it = m_plans.begin(); it != m_plans.end();) {
        if (it.value().updated.msecsTo(now) > kPlanTtlMs)
            it = m_plans.erase(it);
        else
            ++it;
    }

    // 兜底：一个会话里挂着的表太多时，先丢最老的
    while (m_plans.size() >= kMaxPlans) {
        auto oldest = m_plans.begin();
        for (auto it = m_plans.begin(); it != m_plans.end(); ++it) {
            if (it.value().updated < oldest.value().updated)
                oldest = it;
        }
        qWarning().noquote() << QStringLiteral("分段表过多，丢弃最老的传输 %1").arg(oldest.key());
        m_plans.erase(oldest);
    }
}

void PeerSession::handleCancel(const QJsonObject &payload)
{
    if (!m_authenticated || !m_data)
        return;

    const QString transferId = payload.value(QStringLiteral("transferId")).toString();
    if (transferId.isEmpty())
        return;

    // 表作废：之后再有 SEG_REQ 会拿到 410，客户端据此重新发起
    m_plans.remove(transferId);

    if (!m_uploadIds.contains(transferId))
        return;   // 下载的取消就是关数据连接，服务端不需要额外动作

    // 先摘掉再动手：cancelUpload 会同步触发 uploadFinished，留着会让已取消的上传回一条 UPLOAD_DONE
    m_uploadIds.remove(transferId);
    m_data->cancelUpload(transferId);
    qInfo().noquote() << QStringLiteral("对端取消上传：%1").arg(transferId);
}

void PeerSession::handleChecksumReq(const QJsonObject &payload)
{
    const QString reqId = payload.value(QStringLiteral("reqId")).toString();

    if (!m_authenticated) {
        sendError(401, QStringLiteral("未配对，请先配对"), reqId);
        return;
    }
    if (!m_host->shareEnabled) {
        sendError(403, QStringLiteral("对方已关闭共享"), reqId);
        return;
    }
    if (!m_checksums) {
        sendError(500, QStringLiteral("校验服务未启动"), reqId);
        return;
    }

    const QString requestPath = payload.value(QStringLiteral("path")).toString();
    QString error;
    const QString absolute = Qdl::SafePath::resolve(m_host->shareRoot, requestPath, &error);
    if (absolute.isEmpty()) {
        sendError(error.contains(QStringLiteral("不存在")) ? 404 : 400, error, reqId);
        return;
    }
    if (!QFileInfo(absolute).isFile()) {
        sendError(400, QStringLiteral("不是文件"), reqId);
        return;
    }

    QString keyError;
    const QString fileKey = Qdl::FileScanner::fileKey(absolute, &keyError);
    if (fileKey.isEmpty()) {
        sendError(500, keyError.isEmpty() ? QStringLiteral("无法读取文件") : keyError, reqId);
        return;
    }

    // 这是一条**兼容老客户端**的兜底通道：新客户端不再主动问，摘要随表下发。
    // 命中缓存当场就回（M13 起 DOWNLOAD_REQ 就提前算过了，收尾时问必然命中）。
    const QString cached = m_checksums->cachedOrRequest(fileKey, absolute);
    if (!cached.isEmpty()) {
        QJsonObject resp;
        resp.insert(QStringLiteral("reqId"), reqId);
        resp.insert(QStringLiteral("sha256"), cached);
        resp.insert(QStringLiteral("size"), double(QFileInfo(absolute).size()));
        send(Qdl::MsgType::ChecksumResp, resp);
        return;
    }

    // 没缓存：挂起这次请求，等后台算完统一回（同文件多设备并发只算一遍）
    wireChecksumReady();
    m_pendingChecksums.insert(reqId, fileKey);
}

void PeerSession::wireChecksumReady()
{
    if (m_checksumWired || !m_checksums)
        return;
    m_checksumWired = true;

    connect(m_checksums, &Qdl::ChecksumCache::ready, this,
            [this](const QString &key, const QString &sha256) {
                // 1) 老客户端的 CHECKSUM_REQ：按 reqId 逐一回话
                const QList<QString> reqIds = m_pendingChecksums.keys(key);
                for (const QString &pendingId : reqIds) {
                    m_pendingChecksums.remove(pendingId);
                    if (sha256.isEmpty()) {
                        sendError(500, QStringLiteral("校验和计算失败"), pendingId);
                        continue;
                    }
                    QJsonObject resp;
                    resp.insert(QStringLiteral("reqId"), pendingId);
                    resp.insert(QStringLiteral("sha256"), sha256);
                    send(Qdl::MsgType::ChecksumResp, resp);
                }

                if (sha256.isEmpty())
                    return;

                // 2) 正在下发的传输：摘要随控制通道直接推给请求端。
                //    传输还在跑，所以这一步不占任何收尾时间；客户端收完就只差比对。
                for (auto it = m_plans.begin(); it != m_plans.end(); ++it) {
                    TransferPlan &plan = it.value();
                    if (plan.upload || plan.checksumKey != key || plan.sha256 == sha256)
                        continue;
                    plan.sha256 = sha256;

                    QJsonObject push;
                    push.insert(QStringLiteral("transferId"), it.key());
                    push.insert(QStringLiteral("sha256"), sha256);
                    push.insert(QStringLiteral("size"), double(plan.total));
                    send(Qdl::MsgType::ChecksumPush, push);

                    qInfo().noquote() << QStringLiteral("摘要已推送 %1：%2")
                                             .arg(plan.displayName, sha256.left(16));
                }
            });
}

void PeerSession::wireUploadFinished()
{
    if (m_uploadWired || !m_data)
        return;
    m_uploadWired = true;

    // 数据通道是进程级的（多台设备共用一个 DataServer），必须按 transferId 过滤，
    // 否则 A 的上传结果会被当成 B 的发给这台设备。
    connect(m_data, &DataServer::uploadFinished, this,
            [this](const QString &finishedId, const QString &finalPathDone,
                   const QString &finishedName, qint64 received, bool ok,
                   const QString &sha256, const QString &message, bool checksumMismatch) {
                if (!m_uploadIds.remove(finishedId))
                    return;

                QJsonObject resp;
                resp.insert(QStringLiteral("transferId"), finishedId);
                resp.insert(QStringLiteral("ok"), ok);
                resp.insert(QStringLiteral("size"), double(received));
                resp.insert(QStringLiteral("name"), finishedName);
                resp.insert(QStringLiteral("sha256"), sha256);
                resp.insert(QStringLiteral("path"),
                            Qdl::SafePath::toVirtual(m_host->shareRoot, finalPathDone));
                if (!message.isEmpty())
                    resp.insert(QStringLiteral("message"), message);
                // 摘要不一致不是"可续传的中断"：客户端据此直接判失败，别让它重试一遍
                if (checksumMismatch)
                    resp.insert(QStringLiteral("checksumMismatch"), true);
                send(Qdl::MsgType::UploadDone, resp);
            });
}

void PeerSession::handleChecksumPush(const QJsonObject &payload)
{
    if (!m_authenticated)
        return;

    const QString transferId = payload.value(QStringLiteral("transferId")).toString();
    const QString sha256 = payload.value(QStringLiteral("sha256")).toString();
    if (transferId.isEmpty() || sha256.isEmpty())
        return;

    const auto it = m_plans.find(transferId);
    if (it == m_plans.end() || !it.value().upload)
        return;   // 只对上传方向有意义：下载的摘要是主机自己算的

    it.value().expectedSha256 = sha256;
    if (m_data) {
        // 落地/合并时 DataServer 直接查这张表做比对，不必再回头问会话
        m_data->setExpectedChecksum(transferId, sha256);
    }
    qInfo().noquote() << QStringLiteral("收到上传摘要 %1：%2")
                             .arg(it.value().displayName, sha256.left(16));
}

void PeerSession::handleDeleteReq(const QJsonObject &payload)
{
    const QString reqId = payload.value(QStringLiteral("reqId")).toString();
    const QString requestPath = payload.value(QStringLiteral("path")).toString();

    if (!m_authenticated) {
        sendError(401, QStringLiteral("未配对，请先配对"), reqId);
        return;
    }
    if (!m_host->shareEnabled) {
        sendError(403, QStringLiteral("对方已关闭共享"), reqId);
        return;
    }
    // 删除是"改共享目录"的写操作，沿用上传那套三级判定：总开关 + 白名单/rw。
    // 只读设备连上传都不许，更不能删别人的文件。
    if (!m_host->canUploadFor(m_perm)) {
        sendError(403,
                  m_host->writeEnabled ? QStringLiteral("未获写权限（只读设备不能删除）")
                                       : QStringLiteral("主机已关闭写权限，无法删除"),
                  reqId);
        return;
    }

    // 路径必须落在共享根内（SafePath::resolve 会挡掉 ../ 与软链接逃逸）
    QString error;
    const QString absolute = Qdl::SafePath::resolve(m_host->shareRoot, requestPath, &error);
    if (absolute.isEmpty()) {
        qWarning().noquote() << QStringLiteral("拒绝删除 %1：%2").arg(requestPath, error);
        sendError(error.contains(QStringLiteral("不存在")) ? 404 : 400, error, reqId);
        return;
    }

    const QFileInfo info(absolute);
    if (!info.exists()) {
        sendError(404, QStringLiteral("文件不存在，可能已经被删掉了"), reqId);
        return;
    }
    // 只删文件：目录要递归删，误操作代价太大，先不开这个口子
    if (info.isDir()) {
        sendError(400, QStringLiteral("暂不支持删除文件夹"), reqId);
        return;
    }
    if (info.canonicalFilePath() == QFileInfo(m_host->shareRoot).canonicalFilePath()) {
        sendError(400, QStringLiteral("不能删除共享根"), reqId);
        return;
    }

    // 正在被别人下载的文件在 Windows 上会因为句柄占用而删不掉，这里照实报回去
    if (!QFile::remove(absolute)) {
        sendError(500, QStringLiteral("删除失败：文件可能正被占用或没有权限"), reqId);
        return;
    }

    qInfo().noquote() << QStringLiteral("已删除 %1（%2 字节）← %3")
                             .arg(requestPath)
                             .arg(info.size())
                             .arg(m_clientName);

    QJsonObject resp;
    resp.insert(QStringLiteral("reqId"), reqId);
    resp.insert(QStringLiteral("ok"), true);
    resp.insert(QStringLiteral("path"), requestPath);
    resp.insert(QStringLiteral("name"), info.fileName());
    send(Qdl::MsgType::DeleteResp, resp);
}

void PeerSession::sendPermUpdate()
{
    if (!m_authenticated)
        return;

    QJsonObject payload;
    payload.insert(QStringLiteral("writeEnabled"), m_host->writeEnabled);
    payload.insert(QStringLiteral("policy"), m_host->uploadPolicy);
    payload.insert(QStringLiteral("perm"), m_perm);
    payload.insert(QStringLiteral("canUpload"), m_host->canUploadFor(m_perm));
    // sharing 一起推：主机拨掉共享时，已连接的客户端要能立刻在界面上看到，
    // 而不是等下一次点目录才发现被 403
    payload.insert(QStringLiteral("sharing"), m_host->shareEnabled);
    send(Qdl::MsgType::PermUpdate, payload);
}

void PeerSession::send(Qdl::MsgType type, const QJsonObject &payload)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        return;
    m_socket->write(Qdl::MessageCodec::encode(type, payload));
}

void PeerSession::sendError(int code, const QString &message, const QString &reqId)
{
    QJsonObject payload;
    payload.insert(QStringLiteral("code"), code);
    payload.insert(QStringLiteral("message"), message);
    // 带上 reqId，客户端才能把错误归到具体那一次请求上（否则只能显示一条飘着的错误）
    if (!reqId.isEmpty())
        payload.insert(QStringLiteral("reqId"), reqId);
    send(Qdl::MsgType::Error, payload);
}

void PeerSession::resetIdleTimer()
{
    if (!m_idleTimer) {
        m_idleTimer = new QTimer(this);
        m_idleTimer->setSingleShot(true);
        m_idleTimer->setInterval(kIdleTimeoutMs);
        connect(m_idleTimer, &QTimer::timeout, this, [this]() {
            closeSession(QStringLiteral("空闲超时"));
        });
    }
    m_idleTimer->start();
}

QString PeerSession::remoteAddress() const
{
    if (!m_socket)
        return {};
    return QStringLiteral("%1:%2").arg(m_socket->peerAddress().toString()).arg(m_socket->peerPort());
}

void PeerSession::closeSession(const QString &reason)
{
    if (!m_socket)
        return;
    qInfo().noquote() << QStringLiteral("断开连接 %1：%2").arg(remoteAddress(), reason);
    m_socket->disconnectFromHost();
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();
    emit closed(this, reason);
    deleteLater();
}

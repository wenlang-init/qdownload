#include "ControlClient.h"

#include "CryptoUtil.h"
#include "TrustStore.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QTcpSocket>
#include <QTimer>

ControlClient::ControlClient(TrustStore *trust, QObject *parent)
    : QObject(parent)
    , m_trust(trust)
{
}

void ControlClient::setSelf(const QString &peerId, const QString &name, const QString &platform)
{
    m_selfPeerId = peerId;
    m_selfName = name;
    m_selfPlatform = platform;
}

bool ControlClient::isConnected() const
{
    return m_socket && m_socket->state() == QAbstractSocket::ConnectedState;
}

void ControlClient::connectToHost(const PeerInfo &peer)
{
    disconnectFromHost();

    m_peer = peer;
    m_peerId = peer.peerId;
    m_peerName = peer.name;
    m_perm.clear();
    m_canUpload = false;
    m_errorText.clear();
    m_buffer.clear();
    m_pendingLists.clear();   // 换主机：上一台的在途请求一律作废
    m_pendingDownloads.clear();
    m_pendingChecksums.clear();
    m_pendingUploads.clear();
    m_pendingDeletes.clear();

    if (peer.address.isEmpty() || peer.ctrlPort == 0) {
        fail(tr("主机地址或控制端口缺失"));
        return;
    }

    if (!m_socket) {
        m_socket = new QTcpSocket(this);
        // 局域网 P2P 绝不能走系统代理：企业代理 / VPN 客户端常会设置 http_proxy，
        // Qt 会把它套到普通 TCP 上，报 “The proxy type is invalid for this operation”
        m_socket->setProxy(QNetworkProxy::NoProxy);
        connect(m_socket, &QTcpSocket::connected, this, &ControlClient::onConnected);
        connect(m_socket, &QTcpSocket::readyRead, this, &ControlClient::onReadyRead);
        connect(m_socket, &QTcpSocket::disconnected, this, &ControlClient::onDisconnected);
        connect(m_socket, &QTcpSocket::errorOccurred, this, &ControlClient::onSocketError);
    }

    setState(State::Connecting);
    qInfo().noquote() << QStringLiteral("连接主机 %1 (%2:%3)")
                             .arg(peer.name, peer.address)
                             .arg(peer.ctrlPort);
    m_socket->connectToHost(peer.address, peer.ctrlPort);

    // 连接本身也要有上限，否则主机掉线时会挂在那里看不出问题
    QTimer::singleShot(8000, this, [this]() {
        if (m_state == State::Connecting)
            fail(tr("连接超时，主机可能已离线或防火墙拦截"));
    });
}

void ControlClient::disconnectFromHost()
{
    if (m_socket && m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();
    if (m_state != State::Idle) {
        m_state = State::Idle;
        emit stateChanged();
    }
}

void ControlClient::onConnected()
{
    m_buffer.clear();

    QJsonObject hello;
    hello.insert(QStringLiteral("peerId"), m_selfPeerId);
    hello.insert(QStringLiteral("name"), m_selfName);
    hello.insert(QStringLiteral("platform"), m_selfPlatform);
    hello.insert(QStringLiteral("protoVer"), 1);

    setState(State::Handshaking);
    send(Qdl::MsgType::Hello, hello);
}

void ControlClient::onReadyRead()
{
    m_buffer.append(m_socket->readAll());

    QString error;
    const QVector<Qdl::Frame> frames = Qdl::MessageCodec::decode(m_buffer, &error);
    if (!error.isEmpty()) {
        fail(error);
        return;
    }
    for (const Qdl::Frame &frame : frames)
        handleFrame(frame);
}

void ControlClient::onDisconnected()
{
    if (m_state == State::Failed)
        return;
    m_perm.clear();
    m_canUpload = false;
    m_pendingLists.clear();
    m_pendingDownloads.clear();
    m_pendingChecksums.clear();
    m_pendingUploads.clear();
    m_pendingDeletes.clear();
    setState(State::Idle);
    emit permChanged();
}

void ControlClient::onSocketError()
{
    if (m_state == State::Idle)
        return;
    fail(m_socket->errorString());
}

void ControlClient::handleFrame(const Qdl::Frame &frame)
{
    switch (frame.type) {
    case Qdl::MsgType::HelloAck:      handleHelloAck(frame.payload); break;
    case Qdl::MsgType::AuthResp:      handleAuthResp(frame.payload); break;
    case Qdl::MsgType::PairChallenge: handlePairChallenge(frame.payload); break;
    case Qdl::MsgType::PairOk:        handlePairOk(frame.payload); break;
    case Qdl::MsgType::PairFail:      handlePairFail(frame.payload); break;
    case Qdl::MsgType::PermUpdate:    handlePermUpdate(frame.payload); break;
    case Qdl::MsgType::ListResp:      handleListResp(frame.payload); break;
    case Qdl::MsgType::DownloadAccept: handleDownloadAccept(frame.payload); break;
    case Qdl::MsgType::ChecksumResp:  handleChecksumResp(frame.payload); break;
    case Qdl::MsgType::ChecksumPush:  handleChecksumPush(frame.payload); break;
    case Qdl::MsgType::UploadAccept:  handleUploadAccept(frame.payload); break;
    case Qdl::MsgType::SegResp:       handleSegResp(frame.payload); break;
    case Qdl::MsgType::UploadDone:    handleUploadDone(frame.payload); break;
    case Qdl::MsgType::DeleteResp:    handleDeleteResp(frame.payload); break;
    case Qdl::MsgType::Error:         handleError(frame.payload); break;
    case Qdl::MsgType::Ping:
        send(Qdl::MsgType::Pong, {});
        break;
    default:
        qWarning().noquote() << "客户端收到未处理的控制帧："
                             << Qdl::MessageCodec::typeName(frame.type);
        break;
    }
}

void ControlClient::handleHelloAck(const QJsonObject &payload)
{
    m_nonce = payload.value(QStringLiteral("nonce")).toString();
    const QString name = payload.value(QStringLiteral("name")).toString();
    if (!name.isEmpty())
        m_peerName = name;

    const TrustStore::Record record = m_trust ? m_trust->host(m_peerId) : TrustStore::Record();
    if (!record.peerId.isEmpty()) {
        // 有令牌就直接挑战应答，用户无感
        QJsonObject req;
        req.insert(QStringLiteral("tokenId"), record.tokenId);
        req.insert(QStringLiteral("mac"), Qdl::hmacHex(record.token.toUtf8(), m_nonce.toUtf8()));
        setState(State::Authenticating);
        send(Qdl::MsgType::AuthReq, req);
        m_perm = record.perm;
        return;
    }

    // 没令牌：只停留在“未配对”，等用户点配对，不主动骚扰对方
    setState(State::Unpaired);
    emit stateChanged();
}

void ControlClient::handleAuthResp(const QJsonObject &payload)
{
    if (!payload.value(QStringLiteral("ok")).toBool()) {
        const QString message = payload.value(QStringLiteral("message")).toString();
        m_perm.clear();
        m_canUpload = false;
        m_errorText = message.isEmpty() ? tr("鉴权失败") : message;
        setState(State::Unpaired);
        emit authRejected(m_errorText);
        emit permChanged();
        return;
    }

    m_perm = payload.value(QStringLiteral("perm")).toString(QStringLiteral("ro"));
    m_canUpload = payload.value(QStringLiteral("canUpload")).toBool();
    m_peerSharing = payload.value(QStringLiteral("sharing")).toBool(true);
    m_errorText.clear();

    if (m_trust && !m_peerId.isEmpty()) {
        TrustStore::Record record = m_trust->host(m_peerId);
        if (!record.peerId.isEmpty()) {
            record.perm = m_perm;
            record.name = m_peerName;
            m_trust->upsertHost(record);
        }
    }

    setState(State::Ready);
    emit permChanged();
    qInfo().noquote() << QStringLiteral("已连接主机 %1，perm=%2 canUpload=%3")
                             .arg(m_peerName, m_perm).arg(m_canUpload);
}

void ControlClient::requestPairing()
{
    if (m_state != State::Unpaired && m_state != State::Failed) {
        // 已在流程里就不重复发
        if (m_state == State::Pairing || m_state == State::AwaitingPairCode)
            return;
    }
    if (!isConnected()) {
        fail(tr("尚未连接到主机"));
        return;
    }

    m_errorText.clear();

    QJsonObject req;
    req.insert(QStringLiteral("peerId"), m_selfPeerId);
    req.insert(QStringLiteral("name"), m_selfName);
    req.insert(QStringLiteral("platform"), m_selfPlatform);
    setState(State::Pairing);
    send(Qdl::MsgType::PairReq, req);
}

void ControlClient::handlePairChallenge(const QJsonObject &payload)
{
    m_nonce = payload.value(QStringLiteral("nonce")).toString();
    setState(State::AwaitingPairCode);
    emit pairCodePrompt();
}

void ControlClient::submitPairCode(const QString &code)
{
    if (m_state != State::AwaitingPairCode)
        return;

    const QString trimmed = code.trimmed();
    if (trimmed.size() < 4) {
        m_errorText = tr("配对码至少 4 位");
        emit stateChanged();
        return;
    }

    // mac = HMAC(code, nonce + peerId)，配对码本身不出网
    const QString mac = Qdl::hmacHex(trimmed.toUtf8(), (m_nonce + m_selfPeerId).toUtf8());

    QJsonObject resp;
    resp.insert(QStringLiteral("nonce"), m_nonce);
    resp.insert(QStringLiteral("mac"), mac);

    setState(State::Pairing);
    send(Qdl::MsgType::PairResp, resp);
}

QString ControlClient::requestList(const QString &path, int offset, int limit)
{
    if (!isReady())
        return {};

    // 在途请求限个上限：主机磁盘慢时界面滚屏会不断触发 loadMore，
    // 无节制地攒请求只会把响应拖得更慢
    if (m_pendingLists.size() >= 4) {
        qWarning() << "列目录请求过多，丢弃本次请求" << path << offset;
        return {};
    }

    const QString reqId = QStringLiteral("r-%1").arg(++m_reqSeq);

    QJsonObject req;
    req.insert(QStringLiteral("reqId"), reqId);
    req.insert(QStringLiteral("path"), path);
    req.insert(QStringLiteral("offset"), offset);
    req.insert(QStringLiteral("limit"), limit);

    m_pendingLists.insert(reqId, PendingList{ path, offset });
    send(Qdl::MsgType::ListReq, req);

    // 10 秒没回就当失败：否则界面的“加载中”会一直转，用户以为死机
    QTimer::singleShot(10000, this, [this, reqId]() {
        const auto it = m_pendingLists.find(reqId);
        if (it == m_pendingLists.end())
            return;
        const QString path = it.value().path;
        m_pendingLists.erase(it);
        emit listFailed(path, 0, tr("列目录超时，主机没有响应"));
    });

    return reqId;
}

void ControlClient::handleListResp(const QJsonObject &payload)
{
    const QString reqId = payload.value(QStringLiteral("reqId")).toString();
    const auto it = m_pendingLists.find(reqId);
    if (it == m_pendingLists.end()) {
        // 请求已被超时清理，或来自上一次连接：直接丢弃，别把旧数据混进当前目录
        qWarning().noquote() << "收到未知 reqId 的 LIST_RESP：" << reqId;
        return;
    }

    const int requestedOffset = it.value().offset;
    const QString requestedPath = it.value().path;
    m_pendingLists.erase(it);

    const QString path = payload.value(QStringLiteral("path")).toString(requestedPath);
    const int offset = payload.value(QStringLiteral("offset")).toInt(requestedOffset);
    const int total = payload.value(QStringLiteral("total")).toInt();

    QVector<Qdl::FileEntry> entries;
    const QJsonArray array = payload.value(QStringLiteral("entries")).toArray();
    entries.reserve(array.size());
    for (const QJsonValue &value : array) {
        const Qdl::FileEntry entry = Qdl::FileEntry::fromJson(value.toObject());
        if (!entry.name.isEmpty())
            entries.append(entry);
    }

    emit listReceived(path, offset, total, entries);
}

QString ControlClient::requestDownload(const QString &path, qint64 offset, const QString &fileKey,
                                       int segments, const QVector<qint64> &segmentDone,
                                       qint64 chunkSize)
{
    if (!isReady())
        return {};

    // 下载申请不像列表那么频繁，但仍限个上限：用户连点同一个文件不该攒出一堆数据连接
    if (m_pendingDownloads.size() >= 3) {
        qWarning() << "下载申请过多，丢弃本次请求" << path;
        return {};
    }

    const QString reqId = QStringLiteral("d-%1").arg(++m_reqSeq);

    QJsonObject req;
    req.insert(QStringLiteral("reqId"), reqId);
    req.insert(QStringLiteral("path"), path);
    req.insert(QStringLiteral("offset"), double(qMax<qint64>(0, offset)));
    // segments 现在只表示"我可以同时开几条连接"，用来决定段切多细；段数由主机定
    req.insert(QStringLiteral("segments"), qBound(1, segments, 8));

    if (!segmentDone.isEmpty()) {
        QJsonArray arr;
        for (qint64 done : segmentDone)
            arr.append(double(qMax<qint64>(0, done)));
        req.insert(QStringLiteral("segmentDone"), arr);
        req.insert(QStringLiteral("segmentCount"), segmentDone.size());
    }
    // 续传要沿用上次的段长，否则盘上的分片与新的切法对不上，只能整份重来
    if (chunkSize > 0)
        req.insert(QStringLiteral("chunkSize"), double(chunkSize));

    // 只有续传才带 fileKey：首次下载没有"还是不是同一个文件"这个问题。
    // 分段续传时 offset 可能还是 0（进度分散在各分片里），所以看 segmentDone。
    const bool resuming = offset > 0 || !segmentDone.isEmpty();
    if (resuming && !fileKey.isEmpty())
        req.insert(QStringLiteral("fileKey"), fileKey);

    m_pendingDownloads.insert(reqId, path);
    send(Qdl::MsgType::DownloadReq, req);

    // 15 秒没回就当失败：主机可能在算 fileKey（大文件要读首尾各 1MB）
    QTimer::singleShot(15000, this, [this, reqId]() {
        const auto it = m_pendingDownloads.find(reqId);
        if (it == m_pendingDownloads.end())
            return;
        const QString path = it.value();
        m_pendingDownloads.erase(it);
        emit downloadRejected(reqId, path, 408, tr("申请下载超时，主机没有响应"));
    });

    return reqId;
}

void ControlClient::handleDownloadAccept(const QJsonObject &payload)
{
    const QString reqId = payload.value(QStringLiteral("reqId")).toString();
    const auto it = m_pendingDownloads.find(reqId);
    if (it == m_pendingDownloads.end()) {
        qWarning().noquote() << "收到未知 reqId 的 DOWNLOAD_ACCEPT：" << reqId;
        return;
    }

    const QString path = it.value();
    m_pendingDownloads.erase(it);

    Qdl::DownloadGrant grant;
    grant.transferId = payload.value(QStringLiteral("transferId")).toString();
    grant.token = payload.value(QStringLiteral("token")).toString();
    grant.name = payload.value(QStringLiteral("name")).toString();
    grant.fileKey = payload.value(QStringLiteral("fileKey")).toString();
    // 摘要随表下发（M13）：命中缓存时主机受理请求的同一刻就能给出来
    grant.sha256 = payload.value(QStringLiteral("sha256")).toString();
    grant.port = quint16(payload.value(QStringLiteral("port")).toInt());
    grant.size = qint64(payload.value(QStringLiteral("size")).toDouble());
    grant.offset = qint64(payload.value(QStringLiteral("offset")).toDouble());
    grant.chunkSize = qint64(payload.value(QStringLiteral("chunkSize")).toDouble());

    const QJsonArray segArr = payload.value(QStringLiteral("segments")).toArray();
    for (const QJsonValue &value : segArr) {
        const QJsonObject obj = value.toObject();
        Qdl::SegmentGrant seg;
        seg.token = obj.value(QStringLiteral("token")).toString();
        seg.index = obj.value(QStringLiteral("index")).toInt();
        seg.offset = qint64(obj.value(QStringLiteral("offset")).toDouble());
        seg.length = qint64(obj.value(QStringLiteral("length")).toDouble());
        seg.done = qint64(obj.value(QStringLiteral("done")).toDouble());
        grant.segments.append(seg);
    }

    // 0 字节文件没有数据连接可走，主机不会发票（段表也是空的）：
    // 落地由任务自己"建空文件 → 比摘要"完成，不能因为缺票就当成坏应答拒掉。
    const bool needsDataChannel = grant.size > 0;
    if (grant.port == 0 || (needsDataChannel && grant.token.isEmpty())) {
        emit downloadRejected(reqId, path, 500, tr("主机返回的下载信息不完整"));
        return;
    }

    emit downloadAccepted(reqId, path, grant);
}

QString ControlClient::requestChecksum(const QString &path)
{
    if (!isReady())
        return {};

    const QString reqId = QStringLiteral("c-%1").arg(++m_reqSeq);

    QJsonObject req;
    req.insert(QStringLiteral("reqId"), reqId);
    req.insert(QStringLiteral("path"), path);
    m_pendingChecksums.insert(reqId, path);
    send(Qdl::MsgType::ChecksumReq, req);

    // 主机要读完整文件才算得出来：1GB 大概 1–2 秒，给 90 秒足够，
    // 但也不能不给上限——主机卡住时任务会一直停在"校验中"。
    QTimer::singleShot(90000, this, [this, reqId]() {
        const auto it = m_pendingChecksums.find(reqId);
        if (it == m_pendingChecksums.end())
            return;
        const QString path = it.value();
        m_pendingChecksums.erase(it);
        emit checksumFailed(reqId, path, tr("主机未在 90 秒内返回校验值"));
    });

    return reqId;
}

void ControlClient::handleChecksumResp(const QJsonObject &payload)
{
    const QString reqId = payload.value(QStringLiteral("reqId")).toString();
    const auto it = m_pendingChecksums.find(reqId);
    if (it == m_pendingChecksums.end()) {
        qWarning().noquote() << "收到未知 reqId 的 CHECKSUM_RESP：" << reqId;
        return;
    }
    const QString path = it.value();
    m_pendingChecksums.erase(it);

    const QString sha256 = payload.value(QStringLiteral("sha256")).toString();
    if (sha256.isEmpty()) {
        emit checksumFailed(reqId, path, tr("主机未能计算校验值"));
        return;
    }
    emit checksumReceived(reqId, path, sha256);
}

QString ControlClient::requestUpload(const QString &name, qint64 size, const QString &fileKey,
                                     int segments, qint64 chunkSize)
{
    if (!isReady())
        return {};

    if (!m_canUpload) {
        // 界面理论上已经置灰，这里是最后一道保险：可写能力以主机推送为准
        return {};
    }

    if (m_pendingUploads.size() >= 3) {
        qWarning() << "上传申请过多，丢弃本次请求" << name;
        return {};
    }

    const QString reqId = QStringLiteral("u-%1").arg(++m_reqSeq);

    QJsonObject req;
    req.insert(QStringLiteral("reqId"), reqId);
    req.insert(QStringLiteral("name"), name);
    req.insert(QStringLiteral("size"), double(qMax<qint64>(0, size)));
    req.insert(QStringLiteral("segments"), qBound(1, segments, 8));
    if (chunkSize > 0)
        req.insert(QStringLiteral("chunkSize"), double(chunkSize));
    // fileKey 让主机识别"同名半成品是不是同一个文件"，对不上就作废重收
    if (!fileKey.isEmpty())
        req.insert(QStringLiteral("fileKey"), fileKey);

    m_pendingUploads.insert(reqId, name);
    send(Qdl::MsgType::UploadReq, req);

    QTimer::singleShot(15000, this, [this, reqId]() {
        const auto it = m_pendingUploads.find(reqId);
        if (it == m_pendingUploads.end())
            return;
        m_pendingUploads.erase(it);
        emit uploadRejected(reqId, 408, tr("申请上传超时，主机没有响应"));
    });

    return reqId;
}

QString ControlClient::requestSegment(const QString &transferId, int index, qint64 done,
                                      const QString &sha256)
{
    if (!isReady() || transferId.isEmpty())
        return {};

    const QString reqId = QStringLiteral("s-%1").arg(++m_reqSeq);

    QJsonObject req;
    req.insert(QStringLiteral("reqId"), reqId);
    req.insert(QStringLiteral("transferId"), transferId);
    req.insert(QStringLiteral("index"), index);
    req.insert(QStringLiteral("done"), double(qMax<qint64>(0, done)));
    // 上传方向：摘要算好了就搭这张"领段票"的顺风车一起过去（M13）。
    // 领段本来就是传输过程中反复发生的事，摘要因此不占任何额外往返。
    if (!sha256.isEmpty())
        req.insert(QStringLiteral("sha256"), sha256);
    send(Qdl::MsgType::SegReq, req);

    // 10 秒没回就当这一段领不到：任务会把它重新排队，换下一个连接再试
    QTimer::singleShot(10000, this, [this, reqId, index]() {
        emit segmentGranted(reqId, false, index, QString(), 0, 0, 0, false, 408,
                            tr("领取分段超时"));
    });

    return reqId;
}

void ControlClient::handleSegResp(const QJsonObject &payload)
{
    const QString reqId = payload.value(QStringLiteral("reqId")).toString();
    const bool ok = payload.value(QStringLiteral("ok")).toBool(false);

    // 下载方向的摘要常搭在段应答里（M13）：先把它交给任务，再看这次领段的结果
    const QString pushed = payload.value(QStringLiteral("sha256")).toString();
    if (!pushed.isEmpty()) {
        emit checksumPushed(payload.value(QStringLiteral("transferId")).toString(), pushed);
    }

    emit segmentGranted(reqId, ok,
                        payload.value(QStringLiteral("index")).toInt(-1),
                        payload.value(QStringLiteral("token")).toString(),
                        qint64(payload.value(QStringLiteral("offset")).toDouble()),
                        qint64(payload.value(QStringLiteral("length")).toDouble()),
                        qint64(payload.value(QStringLiteral("done")).toDouble()),
                        payload.value(QStringLiteral("complete")).toBool(false),
                        payload.value(QStringLiteral("code")).toInt(0),
                        payload.value(QStringLiteral("message")).toString());
}

void ControlClient::handleUploadAccept(const QJsonObject &payload)
{
    const QString reqId = payload.value(QStringLiteral("reqId")).toString();
    const auto it = m_pendingUploads.find(reqId);
    if (it == m_pendingUploads.end()) {
        qWarning().noquote() << "收到未知 reqId 的 UPLOAD_ACCEPT：" << reqId;
        return;
    }
    m_pendingUploads.erase(it);

    Qdl::UploadGrant grant;
    grant.transferId = payload.value(QStringLiteral("transferId")).toString();
    grant.token = payload.value(QStringLiteral("token")).toString();
    grant.name = payload.value(QStringLiteral("name")).toString();
    grant.path = payload.value(QStringLiteral("path")).toString();
    grant.port = quint16(payload.value(QStringLiteral("port")).toInt());
    grant.size = qint64(payload.value(QStringLiteral("size")).toDouble());
    grant.offset = qint64(payload.value(QStringLiteral("offset")).toDouble());
    grant.chunkSize = qint64(payload.value(QStringLiteral("chunkSize")).toDouble());

    const QJsonArray segArr = payload.value(QStringLiteral("segments")).toArray();
    for (const QJsonValue &value : segArr) {
        const QJsonObject obj = value.toObject();
        Qdl::SegmentGrant seg;
        seg.token = obj.value(QStringLiteral("token")).toString();
        seg.index = obj.value(QStringLiteral("index")).toInt();
        seg.offset = qint64(obj.value(QStringLiteral("offset")).toDouble());
        seg.length = qint64(obj.value(QStringLiteral("length")).toDouble());
        seg.done = qint64(obj.value(QStringLiteral("done")).toDouble());
        grant.segments.append(seg);
    }

    if (grant.token.isEmpty() || grant.port == 0 || grant.name.isEmpty()) {
        emit uploadRejected(reqId, 500, tr("主机返回的上传信息不完整"));
        return;
    }

    emit uploadAccepted(reqId, grant);
}

void ControlClient::handleUploadDone(const QJsonObject &payload)
{
    const QString transferId = payload.value(QStringLiteral("transferId")).toString();
    const bool ok = payload.value(QStringLiteral("ok")).toBool();
    const QString sha256 = payload.value(QStringLiteral("sha256")).toString();
    const QString message = payload.value(QStringLiteral("message")).toString();
    const bool mismatch = payload.value(QStringLiteral("checksumMismatch")).toBool(false);
    emit uploadDone(transferId, ok, sha256, message, mismatch);
}

void ControlClient::handleDeleteResp(const QJsonObject &payload)
{
    const QString reqId = payload.value(QStringLiteral("reqId")).toString();
    const QString path = payload.value(QStringLiteral("path")).toString();
    if (reqId.isEmpty())
        return;

    m_pendingDeletes.remove(reqId);
    const bool ok = payload.value(QStringLiteral("ok")).toBool(false);
    const QString message = payload.value(QStringLiteral("message")).toString();
    emit deleteFinished(reqId, path, ok, message);
}

QString ControlClient::requestDelete(const QString &path)
{
    if (!isReady() || path.isEmpty())
        return {};

    // 主机侧还会复核一遍，这里先挡一次：没写权限时连请求都别发，
    // 用户看到的应该是"对方没开写权限"，而不是一句冷冷的 403
    if (!m_canUpload) {
        qWarning().noquote() << QStringLiteral("没有写权限，不能删除 %1").arg(path);
        return {};
    }

    const QString reqId = QStringLiteral("d-%1").arg(++m_reqSeq);

    QJsonObject req;
    req.insert(QStringLiteral("reqId"), reqId);
    req.insert(QStringLiteral("path"), path);
    m_pendingDeletes.insert(reqId, path);
    send(Qdl::MsgType::DeleteReq, req);

    // 15 秒没回就当失败：删除是用户明确点的操作，不能一直没声音
    QTimer::singleShot(15000, this, [this, reqId]() {
        const auto it = m_pendingDeletes.find(reqId);
        if (it == m_pendingDeletes.end())
            return;
        const QString path = it.value();
        m_pendingDeletes.erase(it);
        emit deleteFinished(reqId, path, false, tr("主机没有响应"));
    });

    return reqId;
}

void ControlClient::handleChecksumPush(const QJsonObject &payload)
{
    const QString transferId = payload.value(QStringLiteral("transferId")).toString();
    const QString sha256 = payload.value(QStringLiteral("sha256")).toString();
    if (transferId.isEmpty() || sha256.isEmpty())
        return;

    qInfo().noquote() << QStringLiteral("收到主机摘要推送 %1：%2")
                             .arg(transferId, sha256.left(16));
    emit checksumPushed(transferId, sha256);
}

void ControlClient::sendChecksumPush(const QString &transferId, const QString &sha256)
{
    if (!isConnected() || transferId.isEmpty() || sha256.isEmpty())
        return;

    QJsonObject req;
    req.insert(QStringLiteral("transferId"), transferId);
    req.insert(QStringLiteral("sha256"), sha256);
    send(Qdl::MsgType::ChecksumPush, req);
}

void ControlClient::cancelTransfer(const QString &transferId)
{
    if (!isConnected() || transferId.isEmpty())
        return;
    QJsonObject req;
    req.insert(QStringLiteral("transferId"), transferId);
    send(Qdl::MsgType::Cancel, req);
}

void ControlClient::handlePairOk(const QJsonObject &payload){
    const QString token = payload.value(QStringLiteral("token")).toString();
    const QString tokenId = payload.value(QStringLiteral("tokenId")).toString();
    m_perm = payload.value(QStringLiteral("perm")).toString(QStringLiteral("ro"));
    m_canUpload = payload.value(QStringLiteral("canUpload")).toBool();

    const QString name = payload.value(QStringLiteral("name")).toString();
    if (!name.isEmpty())
        m_peerName = name;

    if (m_trust && !m_peerId.isEmpty() && !token.isEmpty()) {
        TrustStore::Record record;
        record.peerId = m_peerId;
        record.name = m_peerName;
        record.tokenId = tokenId;
        record.token = token;
        record.perm = m_perm;
        m_trust->upsertHost(record);
    }

    m_errorText.clear();
    setState(State::Ready);
    emit permChanged();
    emit paired();

    qInfo().noquote() << QStringLiteral("配对成功：%1 perm=%2").arg(m_peerName, m_perm);
}

void ControlClient::handlePairFail(const QJsonObject &payload)
{
    const QString reason = payload.value(QStringLiteral("reason")).toString();
    const int remaining = payload.value(QStringLiteral("remaining")).toInt(-1);

    QString message;
    if (reason == QLatin1String("bad_code")) {
        message = remaining >= 0
                      ? tr("配对码不正确，还可尝试 %1 次").arg(remaining)
                      : tr("配对码不正确");
    } else if (reason == QLatin1String("expired")) {
        message = tr("配对码已过期，请让对方重新发起");
    } else if (reason == QLatin1String("rejected")) {
        message = tr("对方拒绝了配对请求");
    } else if (reason == QLatin1String("locked")) {
        message = tr("尝试次数过多，请 60 秒后重试");
    } else if (reason == QLatin1String("bad_nonce")) {
        message = tr("配对会话已失效，请重新发起");
    } else {
        message = tr("配对失败（%1）").arg(reason);
    }

    m_errorText = message;
    m_perm.clear();
    m_canUpload = false;
    setState(State::Unpaired);
    emit permChanged();
    emit pairFailed(message);
}

void ControlClient::handlePermUpdate(const QJsonObject &payload)
{
    const QString perm = payload.value(QStringLiteral("perm")).toString(m_perm);
    const bool canUpload = payload.value(QStringLiteral("canUpload")).toBool();
    const bool sharing = payload.value(QStringLiteral("sharing")).toBool(m_peerSharing);

    const bool permChangedValue = perm != m_perm;
    const bool uploadChangedValue = canUpload != m_canUpload;
    const bool sharingChangedValue = sharing != m_peerSharing;

    m_perm = perm;
    m_canUpload = canUpload;
    m_peerSharing = sharing;

    if (m_trust && !m_peerId.isEmpty() && permChangedValue) {
        TrustStore::Record record = m_trust->host(m_peerId);
        if (!record.peerId.isEmpty()) {
            record.perm = perm;
            m_trust->upsertHost(record);
        }
    }

    if (permChangedValue || uploadChangedValue || sharingChangedValue)
        emit permChanged();
}

void ControlClient::handleError(const QJsonObject &payload)
{
    const int code = payload.value(QStringLiteral("code")).toInt();
    const QString message = payload.value(QStringLiteral("message")).toString();
    const QString reqId = payload.value(QStringLiteral("reqId")).toString();

    // 带 reqId 的错误属于某一次具体请求，交给界面按请求处理，不改连接状态
    if (!reqId.isEmpty()) {
        const auto download = m_pendingDownloads.find(reqId);
        if (download != m_pendingDownloads.end()) {
            const QString path = download.value();
            m_pendingDownloads.erase(download);
            emit downloadRejected(reqId, path, code,
                                  message.isEmpty() ? tr("主机返回错误 %1").arg(code) : message);
            return;
        }

        const auto checksum = m_pendingChecksums.find(reqId);
        if (checksum != m_pendingChecksums.end()) {
            const QString path = checksum.value();
            m_pendingChecksums.erase(checksum);
            emit checksumFailed(reqId, path,
                                message.isEmpty() ? tr("主机返回错误 %1").arg(code) : message);
            return;
        }

        const auto upload = m_pendingUploads.find(reqId);
        if (upload != m_pendingUploads.end()) {
            m_pendingUploads.erase(upload);
            emit uploadRejected(reqId, code,
                                message.isEmpty() ? tr("主机返回错误 %1").arg(code) : message);
            return;
        }

        const auto del = m_pendingDeletes.find(reqId);
        if (del != m_pendingDeletes.end()) {
            const QString path = del.value();
            m_pendingDeletes.erase(del);
            emit deleteFinished(reqId, path, false,
                                message.isEmpty() ? tr("主机返回错误 %1").arg(code) : message);
            return;
        }

        const auto it = m_pendingLists.find(reqId);
        if (it != m_pendingLists.end()) {
            const QString path = it.value().path;
            m_pendingLists.erase(it);
            emit listFailed(path, code, message.isEmpty() ? tr("主机返回错误 %1").arg(code)
                                                          : message);
            return;
        }
    }

    if (code == 401) {
        m_perm.clear();
        m_canUpload = false;
        setState(State::Unpaired);
        emit permChanged();
        return;
    }

    m_errorText = message.isEmpty() ? tr("主机返回错误 %1").arg(code) : message;
    emit stateChanged();
}

void ControlClient::send(Qdl::MsgType type, const QJsonObject &payload)
{
    if (!isConnected())
        return;
    m_socket->write(Qdl::MessageCodec::encode(type, payload));
}

void ControlClient::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged();
}

void ControlClient::fail(const QString &message)
{
    m_errorText = message;
    m_perm.clear();
    m_canUpload = false;
    m_pendingLists.clear();
    m_pendingDownloads.clear();
    m_pendingChecksums.clear();
    m_pendingUploads.clear();
    m_pendingDeletes.clear();
    setState(State::Failed);
    emit stateChanged();
    qWarning().noquote() << QStringLiteral("控制通道失败：%1 - %2").arg(m_peerName, message);

    if (m_socket && m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();
}

QString ControlClient::stateText() const
{
    switch (m_state) {
    case State::Idle:             return tr("未连接");
    case State::Connecting:       return tr("连接中…");
    case State::Handshaking:      return tr("握手中…");
    case State::Authenticating:   return tr("鉴权中…");
    case State::Unpaired:         return tr("未配对");
    case State::Pairing:          return tr("等待对方确认…");
    case State::AwaitingPairCode: return tr("等待输入配对码");
    case State::Ready:
        // 以「现在到底能不能写」为准：perm=rw 但主机把写权限总开关拨掉了，界面上不该说可写
        return m_canUpload ? tr("已连接 · 可写") : tr("已连接 · 只读");
    case State::Failed:           return tr("连接失败");
    }
    return {};
}

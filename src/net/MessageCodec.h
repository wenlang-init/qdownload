#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace Qdl {

/// 控制通道消息类型（设计文档 §5.2）。数值一旦发布不可改，只可追加。
enum class MsgType : quint8 {
    Hello       = 1,   // C→S 握手
    HelloAck    = 2,   // S→C 携带 nonce
    AuthReq     = 3,   // C→S 令牌挑战应答
    AuthResp    = 4,   // S→C ok / perm
    PairReq     = 5,   // C→S 发起配对
    PairChallenge = 6, // S→C nonce
    PairResp    = 7,   // C→S 提交配对码摘要
    PairOk      = 8,   // S→C 下发令牌
    PairFail    = 9,   // S→C 失败原因
    PermUpdate  = 10,  // S→C 权限变更推送
    ListReq     = 11,  // M4
    ListResp    = 12,  // M4
    Ping        = 13,
    Pong        = 14,
    Error       = 15,
    DownloadReq    = 16,  // M5 C→S 申请下载，带起始偏移
    DownloadAccept = 17,  // M5 S→C 数据端口 + 一次性令牌
    Cancel         = 18,  // M5 双向：取消传输（尽力而为，关连接即生效）
    UploadReq      = 19,  // M7 C→S 申请上传（落点固定 /inbox，服务端权威 offset）
    UploadAccept   = 20,  // M7 S→C 数据端口 + 一次性令牌 + 去重后的文件名
    ChecksumReq    = 21,  // M6 C→S 请求全量 SHA-256（传完自校验用）
    ChecksumResp   = 22,  // M6 S→C 返回 hex 摘要
    UploadDone     = 23,  // M7 S→C 收完并校验后的结果（含主机算出的 SHA-256）
    SegReq         = 24,  // M12 C→S 领取某一段的令牌（按需发牌，不预发）
    SegResp        = 25,  // M12 S→C 该段的令牌与本次起发位置
    ChecksumPush   = 26,  // M13 双向：本次传输的整文件摘要算好了，随控制通道推给对端
    DeleteReq      = 27,  // M14 C→S 删除共享根里的一个文件（需写权限）
    DeleteResp     = 28   // M14 S→C 删除结果
};

struct Frame
{
    MsgType type = MsgType::Error;
    QJsonObject payload;
};

/**
 * 控制帧编解码：`"QDL1" + type(1B) + len(u32be) + JSON`。
 *
 * 解码是累积式的：TCP 一次 read 可能只到半个头、或一次到多帧，
 * 所以必须喂 buffer 让它反复切，不能假设“一条 readyRead 就是一帧”。
 */
class MessageCodec
{
public:
    static constexpr int kHeaderSize = 9;
    static constexpr int kMaxPayload = 8 * 1024 * 1024;   // 8MB，超了直接断开

    static QByteArray encode(MsgType type, const QJsonObject &payload);

    /// 从 buffer 头部切出所有完整帧，剩余的留在 buffer 里。error 非空表示协议错误。
    static QVector<Frame> decode(QByteArray &buffer, QString *error = nullptr);

    static QString typeName(MsgType type);
    static MsgType typeFromName(const QString &name, bool *ok = nullptr);
};

} // namespace Qdl

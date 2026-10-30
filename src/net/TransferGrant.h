#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>
#include <QtGlobal>

namespace Qdl {

/**
 * 分段表里的一段。
 *
 * M12 起 `token` 不再随表下发：一次传输几十上百段，票提前发出去会在慢链路上
 * 直接过期（票只有 5 分钟）。现在表里只有区间与进度，票在真正要传这一段时
 * 用 SEG_REQ 现领现用——见 PeerSession::handleSegReq。
 */
struct SegmentGrant
{
    QString token;       // 只有按需领取时才非空（老版主机一次性下发时也会有）
    int index = 0;
    qint64 offset = 0;   // 本段在**文件里**的起点（不含 done）
    qint64 length = 0;   // 本段总字节数
    qint64 done = 0;     // 这一段已经完成多少（跨进程续传用）
};

/**
 * 一次下载的入场券（DOWNLOAD_ACCEPT 的内容）。
 *
 * 数据通道与文件内容无关，只认 token：token 一次性、5 分钟过期、绑 transferId 与
 * 相对偏移。这样即使数据端口被同网段的第三方扫到，没有 token 也拿不到一个字节。
 *
 * `segments` 为空表示对端不认识分段（老版本主机），此时退化成单段，用顶层的
 * token/offset/size 那套老字段走。
 *
 * `sha256` 是**整份文件**的摘要：主机在受理请求时就已经在后台开始算，算好随表
 * 下发（或随后用 CHECKSUM_PUSH 推过来）。客户端收完只算自己的本地摘要即可比对，
 * 不用再等主机读完一遍文件——这正是"最终校验阶段"从数秒降到近乎为零的关键。
 */
struct DownloadGrant
{
    QString transferId;
    QString token;
    QString name;        // 单段文件名（展示用）
    QString fileKey;     // 断点续传的身份标识（M6 用，M5 只透传）
    QString sha256;      // 源文件整文件摘要（可能为空：老主机 / 还没算完）
    quint16 port = 0;
    qint64 size = 0;     // 文件总大小
    qint64 offset = 0;   // 服务端将从这里开始发（单段 / 兼容字段）
    qint64 chunkSize = 0;   // 段长（服务端裁决，续传时沿用旧值）
    QVector<SegmentGrant> segments;
};

/**
 * 一次上传的入场券（UPLOAD_ACCEPT 的内容，M7）。
 *
 * 与 DownloadGrant 刻意分开：上传多了 path（服务端去重后的落点）与权威 offset。
 * `offset` 以**服务端**为准——客户端以为自己传到哪不算数，落盘文件有多大才是事实。
 */
struct UploadGrant
{
    QString transferId;
    QString token;
    QString name;        // 服务端去重后的文件名，可能与客户端请求的不同
    QString path;        // 虚拟路径，如 /inbox/a.zip
    quint16 port = 0;
    qint64 size = 0;     // 文件总大小
    qint64 offset = 0;   // 服务端已有的字节数（单段 / 兼容字段）
    qint64 chunkSize = 0;
    QVector<SegmentGrant> segments;
};

} // namespace Qdl

Q_DECLARE_METATYPE(Qdl::DownloadGrant)
Q_DECLARE_METATYPE(Qdl::UploadGrant)

#pragma once

#include <QString>
#include <QVector>
#include <QtGlobal>

namespace Qdl {

/// 传输方向。上传在 M7 接入，枚举先占位，模型与界面按同一套渲染。
enum class TransferDirection { Download, Upload };

/**
 * 分段表里的一段（不含令牌）。令牌在真正要传这一段时才向主机领。
 *
 * 表由**服务端**生成并下发，客户端只维护每一段的状态（待领 / 已领 / 已完成）。
 * 客户端绝不自己算区间：两端各算一遍，取整差一个字节就是一份坏文件，
 * 而且这种坏只会在最后校验时暴露。
 */
struct ChunkPlan
{
    int index = 0;
    qint64 offset = 0;   // 文件内起点
    qint64 length = 0;   // 本段字节数
};

/// 一个连接至少搬这么多字节才值得为它握一次手
constexpr qint64 kMinChunkBytes = 4LL * 1024 * 1024;
/// 段太大则最后一段收尾时的"拖尾"变长，负载均衡失效
constexpr qint64 kMaxChunkBytes = 32LL * 1024 * 1024;
/// 段数上限：每个段在下载侧对应一个分片文件，太多会把目录塞满
constexpr int kMaxChunks = 128;
/// 每个连接期望分到几段：>1 才能让先干完的连接继续领活（负载均衡）
constexpr int kChunksPerWorker = 4;

/**
 * 按连接数推算段大小：目标让每个连接分到 kChunksPerWorker 段，
 * 这样先干完的连接还有活可领，不会出现"三条连接闲着等最后一段"。
 */
inline qint64 chunkSizeFor(qint64 total, int workers)
{
    if (total <= 0)
        return 0;

    const int w = qBound(1, workers, 8);
    qint64 size = total / (qint64(w) * kChunksPerWorker);
    size = qBound(kMinChunkBytes, size, kMaxChunkBytes);

    // 段数上限：文件再大也不能切出几百个分片文件
    const qint64 minForCap = (total + kMaxChunks - 1) / kMaxChunks;
    if (minForCap > size)
        size = minForCap;

    return size;
}

/// 按固定段长切表；最后一段吃掉余数，保证覆盖到文件尾
inline QVector<ChunkPlan> planChunks(qint64 total, qint64 chunkSize)
{
    QVector<ChunkPlan> out;
    if (total <= 0 || chunkSize <= 0)
        return out;

    qint64 offset = 0;
    while (offset < total && out.size() < kMaxChunks) {
        const qint64 length = qMin(chunkSize, total - offset);
        out.append({ int(out.size()), offset, length });
        offset += length;
    }

    // 极端情况下（段数触顶）把剩下的一股脑塞给最后一段，宁可不均衡也不能丢字节
    if (offset < total && !out.isEmpty())
        out.last().length += total - offset;

    return out;
}

/**
 * 任务状态机（设计文档 §6.4）。
 *
 * Queued → Connecting → Transferring → Verifying → Completed
 *   任何阶段都可能失败；Transferring 可暂停（保留 .qdpart）或取消（删 .qdpart）
 */
enum class TransferState {
    Queued,
    Connecting,
    Transferring,
    Verifying,
    Completed,
    Paused,
    Failed,
    Canceled
};

/// 展示用的格式化。放在这里而不是各界面里，保证抽屉、日志、CLI 输出三处口径一致。
namespace TransferFmt {

QString sizeText(qint64 bytes);
QString speedText(double bytesPerSecond);
/// 剩余时间；速度过低（<1KB/s）时给 "—" 而不是一个荒谬的巨大数字
QString etaText(qint64 remainBytes, double bytesPerSecond);

QString stateCode(TransferState state);   // 给小写字符串给 QML 做样式分支
QString stateText(TransferState state);   // 给用户看的中文

} // namespace TransferFmt

} // namespace Qdl

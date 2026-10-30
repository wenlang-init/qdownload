#pragma once

#include "TransferTypes.h"

#include <QElapsedTimer>
#include <QMutex>

namespace Qdl {

/**
 * 进程级令牌桶限速器（M10）。
 *
 * 为什么是「全局一张桶」而不是每个任务一张：用户设的是"这台机器最多占用多少带宽"，
 * 不是"每个文件多少"。一个文件在传就它吃满，三个文件同时传就三家分——
 * 否则并发数调大反而把限速穿透了。
 *
 * 上传（发送侧）能精确限：字节在我们手上，没令牌就不 write。
 * 下载（接收侧）只能靠 TCP 背压：我们停止 read，数据就堆在内核接收缓冲里，
 * 内核窗口缩到 0，对端自然写不动。这条路有半个窗口（约 2MB）的启动突发，
 * 换来的是不用改协议——对端根本不需要知道限速这回事。
 *
 * 线程：目前所有任务都跑在 GUI 线程，锁是为了以后挪到工作线程时不至于重写出血。
 */
class RateLimiter
{
public:
    static RateLimiter *instance();

    /// 设置某个方向的速率上限（字节/秒）。0 = 不限速
    void setLimit(TransferDirection dir, qint64 bytesPerSec);
    qint64 limit(TransferDirection dir) const;
    /// 上下行一起设（界面上就一个开关）
    void setLimitForAll(qint64 bytesPerSec);

    /// 取走至多 want 字节的配额，返回实际允许量（可能小于 want，也可能为 0）
    qint64 acquire(TransferDirection dir, qint64 want);

    /// 想再拿 want 字节还得等多少毫秒；配额够就返回 0。用于安排下一次 pump
    int waitMs(TransferDirection dir, qint64 want) const;

    /**
     *  socket 读缓冲上限。不限速时返回 0（Qt 的"无限"）；限速时给一个略大于
     *  突发容量的窗口——数据留在内核里才能触发背压，全读进用户态就限不住了。
     */
    qint64 recvWindow(TransferDirection dir) const;

    /// 限速时把发送水位压低，避免开局就把 2MB 一次性推出去（那一下就是突发）
    qint64 sendWaterMark(TransferDirection dir, qint64 unlimitedWater) const;

    /// 仅供自检：跳过真实等待，直接把桶抽空/补满
    void resetForTest();

private:
    RateLimiter();

    struct Bucket
    {
        qint64 rate = 0;        // 字节/秒，0 = 不限
        double tokens = 0.0;    // 当前可用令牌
        qint64 lastMs = 0;      // 上次补充的时刻
    };

    Bucket &bucketFor(TransferDirection dir);
    const Bucket &bucketFor(TransferDirection dir) const;
    /// 调用者必须持锁
    void refillLocked(Bucket &bucket) const;
    /// 突发容量：半个窗口的量，但要保证一次 chunk（256KB）拿得出手
    qint64 burstFor(const Bucket &bucket) const;

    mutable QMutex m_mutex;
    QElapsedTimer m_clock;
    Bucket m_upload;
    Bucket m_download;
};

} // namespace Qdl

#include "RateLimiter.h"

#include <algorithm>

namespace Qdl {
namespace {
constexpr qint64 kMinBurst = 256 * 1024;    // 至少够一次 chunk，否则等一次发一点、碎得太厉害
constexpr qint64 kMaxBurst = 4 * 1024 * 1024;
constexpr int kMaxWaitMs = 120;             // 单次等待上限：再长界面看着就像卡住了
} // namespace

RateLimiter::RateLimiter()
{
    m_clock.start();
    m_upload.lastMs = 0;
    m_download.lastMs = 0;
}

RateLimiter *RateLimiter::instance()
{
    static RateLimiter limiter;
    return &limiter;
}

RateLimiter::Bucket &RateLimiter::bucketFor(TransferDirection dir)
{
    return dir == TransferDirection::Upload ? m_upload : m_download;
}

const RateLimiter::Bucket &RateLimiter::bucketFor(TransferDirection dir) const
{
    return dir == TransferDirection::Upload ? m_upload : m_download;
}

qint64 RateLimiter::burstFor(const Bucket &bucket) const
{
    if (bucket.rate <= 0)
        return 0;
    return qBound(kMinBurst, bucket.rate / 2, kMaxBurst);
}

void RateLimiter::refillLocked(Bucket &bucket) const
{
    const qint64 now = m_clock.elapsed();
    if (bucket.lastMs == 0)
        bucket.lastMs = now;

    const qint64 elapsed = now - bucket.lastMs;
    bucket.lastMs = now;

    if (bucket.rate <= 0 || elapsed <= 0)
        return;

    const double addition = double(bucket.rate) * double(elapsed) / 1000.0;
    bucket.tokens = std::min(double(burstFor(bucket)), bucket.tokens + addition);
}

void RateLimiter::setLimit(TransferDirection dir, qint64 bytesPerSec)
{
    QMutexLocker locker(&m_mutex);
    Bucket &bucket = bucketFor(dir);
    if (bucket.rate == qMax<qint64>(0, bytesPerSec))
        return;

    // 先按旧速率把时间结清，再换速率——否则改限速的那一刻会白送一大把令牌
    refillLocked(bucket);
    bucket.rate = qMax<qint64>(0, bytesPerSec);
    bucket.tokens = std::min(bucket.tokens, double(burstFor(bucket)));
}

qint64 RateLimiter::limit(TransferDirection dir) const
{
    QMutexLocker locker(&m_mutex);
    return bucketFor(dir).rate;
}

void RateLimiter::setLimitForAll(qint64 bytesPerSec)
{
    setLimit(TransferDirection::Upload, bytesPerSec);
    setLimit(TransferDirection::Download, bytesPerSec);
}

qint64 RateLimiter::acquire(TransferDirection dir, qint64 want)
{
    if (want <= 0)
        return 0;

    QMutexLocker locker(&m_mutex);
    Bucket &bucket = bucketFor(dir);

    if (bucket.rate <= 0)
        return want;   // 不限速：要多少给多少，一次系统调用都不少

    refillLocked(bucket);
    if (bucket.tokens < 1.0)
        return 0;

    const double granted = std::min(double(want), bucket.tokens);
    bucket.tokens -= granted;
    return qint64(granted);
}

int RateLimiter::waitMs(TransferDirection dir, qint64 want) const
{
    QMutexLocker locker(&m_mutex);
    const Bucket &bucket = bucketFor(dir);   // 只读，不补充令牌：问"还要等多久"不该消耗配额

    if (bucket.rate <= 0 || want <= 0)
        return 0;
    if (bucket.tokens >= double(want))
        return 0;

    const double missing = double(want) - bucket.tokens;
    const double ms = missing * 1000.0 / double(bucket.rate);
    return qBound(1, int(ms + 0.5), kMaxWaitMs);
}

qint64 RateLimiter::recvWindow(TransferDirection dir) const
{
    QMutexLocker locker(&m_mutex);
    const Bucket &bucket = bucketFor(dir);
    if (bucket.rate <= 0)
        return 0;   // Qt：0 = 不限，保持原来的全速行为
    return burstFor(bucket) + kMinBurst;
}

qint64 RateLimiter::sendWaterMark(TransferDirection dir, qint64 unlimitedWater) const
{
    QMutexLocker locker(&m_mutex);
    const Bucket &bucket = bucketFor(dir);
    if (bucket.rate <= 0)
        return unlimitedWater;
    // 待发量小一点，限速才跟得上手；否则水桶里已经灌满 2MB，再限也限不住那一下
    return qMin(unlimitedWater, burstFor(bucket));
}

void RateLimiter::resetForTest()
{
    QMutexLocker locker(&m_mutex);
    m_upload.tokens = double(burstFor(m_upload));
    m_download.tokens = double(burstFor(m_download));
}

} // namespace Qdl

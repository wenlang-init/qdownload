#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

namespace Qdl {

/**
 * 服务端全量 SHA-256 计算与缓存（M6 校验的"主机那一侧"）。
 *
 * 为什么不在 PeerSession 里直接算：1GB 文件的 SHA-256 在开发机上要 1–2 秒，
 * 这段时间里控制通道、组播心跳、其他设备的所有请求全被卡在同一根线程上。
 * 所以丢给 QtConcurrent 线程池，算完再回主线程发消息。
 *
 * 缓存键用 `fileKey`（已含 size + mtime + 首尾 1MB），同一个文件被反复校验时
 * 不必重复读盘；文件一变 fileKey 就变，缓存自然失效，不需要手动清理逻辑。
 */
class ChecksumCache : public QObject
{
    Q_OBJECT

public:
    explicit ChecksumCache(QObject *parent = nullptr);

    QString cached(const QString &key) const { return m_cache.value(key); }

    /// 未缓存则后台开算；算完发 ready(key, sha256)。key 为空表示不缓存（仅计算）。
    void request(const QString &key, const QString &absolutePath);

    /**
     * 缓存命中就**同步**返回摘要，否则排到后台算并返回空串。
     *
     * 这是"摘要随表下发"的入口：命中（同一个文件传第二遍）时直接把值写进
     * ACCEPT/段表，客户端连一次往返都省了；没命中也已经开算，等 ready 信号
     * 再推给对端，传输结束时它早就好了。
     */
    QString cachedOrRequest(const QString &key, const QString &absolutePath);

signals:
    void ready(const QString &key, const QString &sha256);

private:
    void store(const QString &key, const QString &sha256);

    QHash<QString, QString> m_cache;
    QSet<QString> m_running;
    static constexpr int kMaxEntries = 64;   // 长期运行时防止缓存无限涨
};

} // namespace Qdl

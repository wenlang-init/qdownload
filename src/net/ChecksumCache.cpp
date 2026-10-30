#include "ChecksumCache.h"

#include "fs/FileScanner.h"

#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

namespace Qdl {

ChecksumCache::ChecksumCache(QObject *parent)
    : QObject(parent)
{
}

void ChecksumCache::request(const QString &key, const QString &absolutePath)
{
    if (!key.isEmpty()) {
        const auto it = m_cache.constFind(key);
        if (it != m_cache.constEnd()) {
            // 已经在缓存里：排队回一次，保持"request → ready"的调用契约一致
            const QString hash = it.value();
            QMetaObject::invokeMethod(this, [this, key, hash]() { emit ready(key, hash); },
                                      Qt::QueuedConnection);
            return;
        }
        if (m_running.contains(key))
            return;   // 同一个文件多台设备同时要，算一遍就够
        m_running.insert(key);
    }

    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, key]() {
        const QString hash = watcher->result();
        watcher->deleteLater();
        if (key.isEmpty()) {
            emit ready(key, hash);
            return;
        }
        m_running.remove(key);
        if (hash.isEmpty()) {
            qWarning().noquote() << QStringLiteral("校验和计算失败：%1").arg(key);
            emit ready(key, QString());
            return;
        }
        store(key, hash);
        emit ready(key, hash);
    });

    watcher->setFuture(QtConcurrent::run([absolutePath]() {
        QString error;
        const QString hash = Qdl::FileScanner::sha256(absolutePath, &error);
        if (hash.isEmpty())
            qWarning().noquote() << QStringLiteral("SHA-256 失败：%1").arg(error);
        return hash;
    }));
}

QString ChecksumCache::cachedOrRequest(const QString &key, const QString &absolutePath)
{
    if (key.isEmpty())
        return {};

    const auto it = m_cache.constFind(key);
    if (it != m_cache.constEnd())
        return it.value();

    request(key, absolutePath);
    return {};
}

void ChecksumCache::store(const QString &key, const QString &sha256)
{
    if (m_cache.size() >= kMaxEntries)
        m_cache.clear();   // 简单粗暴：条目不多，清空比算 LRU 更省事
    m_cache.insert(key, sha256);
}

} // namespace Qdl

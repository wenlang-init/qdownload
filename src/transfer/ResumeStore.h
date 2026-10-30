#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QVector>

namespace Qdl {

/**
 * 断点持久化（设计文档 §6.2 / §6.5）。
 *
 * 记录的是"这个文件的这一份半成品下到哪了"，而不是某个内存任务的 id：
 * 进程重启后任务对象全没了，`t-3` 这种自增 id 也不再有意义，唯一还站得住的
 * 身份是 **(主机 peerId, 远端路径, 本地落点)**。
 *
 * `fileKey` 也一起存：它是"还是不是同一个文件"的判据。续传时把它发给主机，
 * 主机重算一遍，不一致就返回 409 —— 源文件已经被换掉了，接着写只会拼出一个
 * 前后两截来自不同文件的东西。
 *
 * 只存元数据、不存字节：字节在 `<目标>.qdpart` 里，由文件系统保证。
 */
class ResumeStore
{
public:
    struct Record
    {
        QString peerId;
        QString peerName;
        QString remotePath;
        QString localPath;
        QString name;
        QString fileKey;
        qint64  total = 0;
        qint64  done = 0;
        qint64  updatedAt = 0;
    };

    ResumeStore();
    ~ResumeStore();

    bool open(const QString &path, QString *error = nullptr);
    bool isOpen() const;
    QString path() const { return m_path; }

    bool upsert(const Record &record);
    bool remove(const QString &peerId, const QString &remotePath);

    /// 全部记录，按最近更新倒序（重启后最想接着下的排在最上面）
    QVector<Record> list() const;

    /**
     * 清掉 `.qdpart` 已经不存在的记录（用户手动删了、或被清理工具扫走了）。
     * 返回删除条数。启动时调用一次，避免抽屉里冒出一堆点了没反应的僵尸任务。
     */
    int prune();

private:
    bool migrate(QString *error);

    QSqlDatabase m_db;
    QString m_connectionName;
    QString m_path;
};

} // namespace Qdl

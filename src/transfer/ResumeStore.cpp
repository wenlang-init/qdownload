#include "ResumeStore.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace {
qint64 nowSecs()
{
    return QDateTime::currentSecsSinceEpoch();
}

Qdl::ResumeStore::Record recordFromQuery(const QSqlQuery &query)
{
    Qdl::ResumeStore::Record record;
    record.peerId     = query.value(QStringLiteral("peer_id")).toString();
    record.peerName   = query.value(QStringLiteral("peer_name")).toString();
    record.remotePath = query.value(QStringLiteral("remote_path")).toString();
    record.localPath  = query.value(QStringLiteral("local_path")).toString();
    record.name       = query.value(QStringLiteral("name")).toString();
    record.fileKey    = query.value(QStringLiteral("file_key")).toString();
    record.total      = query.value(QStringLiteral("total")).toLongLong();
    record.done       = query.value(QStringLiteral("done")).toLongLong();
    record.updatedAt  = query.value(QStringLiteral("updated_at")).toLongLong();
    return record;
}
} // namespace

namespace Qdl {

ResumeStore::ResumeStore()
    : m_connectionName(QStringLiteral("qdownload-resume-%1").arg(quintptr(this), 0, 16))
{
}

ResumeStore::~ResumeStore()
{
    if (m_db.isOpen())
        m_db.close();
    m_db = QSqlDatabase();
    if (!m_connectionName.isEmpty())
        QSqlDatabase::removeDatabase(m_connectionName);
}

bool ResumeStore::open(const QString &path, QString *error)
{
    m_path = path;
    QDir().mkpath(QFileInfo(path).absolutePath());

    if (QSqlDatabase::contains(m_connectionName))
        m_db = QSqlDatabase::database(m_connectionName, false);
    else
        m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);

    m_db.setDatabaseName(path);
    if (!m_db.open()) {
        if (error)
            *error = m_db.lastError().text();
        return false;
    }

    // 传输中每 2 秒左右写一次检查点，WAL 让写库不阻塞读、也不会把 UI 卡住
    QSqlQuery pragma(m_db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));

    return migrate(error);
}

bool ResumeStore::migrate(QString *error)
{
    QSqlQuery query(m_db);
    const QString sql = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS resume ("
        "  peer_id     TEXT NOT NULL,"
        "  remote_path TEXT NOT NULL,"
        "  peer_name   TEXT,"
        "  local_path  TEXT NOT NULL,"
        "  name        TEXT NOT NULL,"
        "  file_key    TEXT,"
        "  total       INTEGER NOT NULL DEFAULT 0,"
        "  done        INTEGER NOT NULL DEFAULT 0,"
        "  updated_at  INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (peer_id, remote_path))");

    if (!query.exec(sql)) {
        if (error)
            *error = query.lastError().text();
        return false;
    }
    return true;
}

bool ResumeStore::isOpen() const
{
    return m_db.isOpen();
}

bool ResumeStore::upsert(const Record &record)
{
    if (!isOpen() || record.peerId.isEmpty() || record.remotePath.isEmpty())
        return false;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO resume (peer_id, remote_path, peer_name, local_path, name, file_key,"
        "                    total, done, updated_at)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(peer_id, remote_path) DO UPDATE SET"
        "   peer_name=excluded.peer_name, local_path=excluded.local_path,"
        "   name=excluded.name, file_key=excluded.file_key,"
        "   total=excluded.total, done=excluded.done, updated_at=excluded.updated_at"));
    query.addBindValue(record.peerId);
    query.addBindValue(record.remotePath);
    query.addBindValue(record.peerName);
    query.addBindValue(record.localPath);
    query.addBindValue(record.name);
    query.addBindValue(record.fileKey);
    query.addBindValue(record.total);
    query.addBindValue(record.done);
    query.addBindValue(record.updatedAt > 0 ? record.updatedAt : nowSecs());
    return query.exec();
}

bool ResumeStore::remove(const QString &peerId, const QString &remotePath)
{
    if (!isOpen())
        return false;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM resume WHERE peer_id = ? AND remote_path = ?"));
    query.addBindValue(peerId);
    query.addBindValue(remotePath);
    return query.exec();
}

QVector<ResumeStore::Record> ResumeStore::list() const
{
    QVector<Record> records;
    if (!isOpen())
        return records;

    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT * FROM resume ORDER BY updated_at DESC")))
        return records;
    while (query.next())
        records.append(recordFromQuery(query));
    return records;
}

int ResumeStore::prune()
{
    if (!isOpen())
        return 0;

    int removed = 0;
    const QVector<Record> records = list();
    for (const Record &record : records) {
        // 半成品没了，这条记录就是个死引用：留着只会在抽屉里放一个点了没反应的「继续」。
        // 多段下载没有单一 .qdpart，进度散在 .seg<i> 里，所以三种形态都要认。
        const QString part = record.localPath + QStringLiteral(".qdpart");
        const bool hasPart = QFileInfo::exists(part)
                          || QFileInfo::exists(part + QStringLiteral(".qseg"))
                          || QFileInfo::exists(part + QStringLiteral(".seg0"));
        if (!hasPart) {
            if (remove(record.peerId, record.remotePath))
                ++removed;
        }
    }
    return removed;
}

} // namespace Qdl

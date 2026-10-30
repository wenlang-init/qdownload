#include "TrustStore.h"

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

TrustStore::Record recordFromQuery(const QSqlQuery &query)
{
    TrustStore::Record record;
    record.peerId  = query.value(QStringLiteral("peer_id")).toString();
    record.name    = query.value(QStringLiteral("name")).toString();
    record.tokenId = query.value(QStringLiteral("token_id")).toString();
    record.token   = query.value(QStringLiteral("token")).toString();
    record.perm    = query.value(QStringLiteral("perm")).toString();
    record.lastSeen = query.value(QStringLiteral("last_seen")).toLongLong();
    return record;
}
} // namespace

TrustStore::TrustStore()
    : m_connectionName(QStringLiteral("qdownload-trust-%1")
                           .arg(quintptr(this), 0, 16))
{
}

TrustStore::~TrustStore()
{
    if (m_db.isOpen())
        m_db.close();
    m_db = QSqlDatabase();
    if (!m_connectionName.isEmpty())
        QSqlDatabase::removeDatabase(m_connectionName);
}

bool TrustStore::open(const QString &path, QString *error)
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

    // WAL：读写不互斥，后续传输进度写库时不会卡住 UI 线程
    QSqlQuery pragma(m_db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));

    return migrate(error);
}

bool TrustStore::migrate(QString *error)
{
    QSqlQuery query(m_db);

    const QString clients = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS trusted_clients ("
        "  peer_id    TEXT PRIMARY KEY,"
        "  name       TEXT,"
        "  token_id   TEXT NOT NULL,"
        "  token      TEXT NOT NULL,"
        "  perm       TEXT NOT NULL DEFAULT 'ro',"
        "  created_at INTEGER NOT NULL DEFAULT 0,"
        "  last_seen  INTEGER NOT NULL DEFAULT 0,"
        "  blocked    INTEGER NOT NULL DEFAULT 0)");

    const QString hosts = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS trusted_hosts ("
        "  peer_id   TEXT PRIMARY KEY,"
        "  name      TEXT,"
        "  token_id  TEXT NOT NULL,"
        "  token     TEXT NOT NULL,"
        "  perm      TEXT NOT NULL DEFAULT 'ro',"
        "  last_seen INTEGER NOT NULL DEFAULT 0)");

    for (const QString &sql : { clients, hosts }) {
        if (!query.exec(sql)) {
            if (error)
                *error = query.lastError().text();
            return false;
        }
    }
    return true;
}

bool TrustStore::isOpen() const
{
    return m_db.isOpen();
}

// ---------------- 共享端：被信任的客户端 ----------------

bool TrustStore::upsertClient(const Record &record)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO trusted_clients (peer_id, name, token_id, token, perm, created_at, last_seen, blocked)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, 0)"
        " ON CONFLICT(peer_id) DO UPDATE SET"
        "   name=excluded.name, token_id=excluded.token_id, token=excluded.token,"
        "   perm=excluded.perm, last_seen=excluded.last_seen"));
    query.addBindValue(record.peerId);
    query.addBindValue(record.name);
    query.addBindValue(record.tokenId);
    query.addBindValue(record.token);
    query.addBindValue(record.perm.isEmpty() ? QStringLiteral("ro") : record.perm);
    query.addBindValue(nowSecs());
    query.addBindValue(nowSecs());
    return query.exec();
}

TrustStore::Record TrustStore::client(const QString &peerId) const
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT * FROM trusted_clients WHERE peer_id = ?"));
    query.addBindValue(peerId);
    if (query.exec() && query.next()) {
        Record record = recordFromQuery(query);
        record.blocked = query.value(QStringLiteral("blocked")).toInt() != 0;
        return record;
    }
    return {};
}

QVector<TrustStore::Record> TrustStore::clients() const
{
    QVector<Record> list;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT * FROM trusted_clients ORDER BY last_seen DESC"));
    if (query.exec()) {
        while (query.next()) {
            Record record = recordFromQuery(query);
            record.blocked = query.value(QStringLiteral("blocked")).toInt() != 0;
            list.append(record);
        }
    }
    return list;
}

bool TrustStore::setClientPerm(const QString &peerId, const QString &perm)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("UPDATE trusted_clients SET perm = ? WHERE peer_id = ?"));
    query.addBindValue(perm);
    query.addBindValue(peerId);
    return query.exec() && query.numRowsAffected() > 0;
}

bool TrustStore::removeClient(const QString &peerId)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM trusted_clients WHERE peer_id = ?"));
    query.addBindValue(peerId);
    return query.exec();
}

void TrustStore::touchClient(const QString &peerId)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("UPDATE trusted_clients SET last_seen = ? WHERE peer_id = ?"));
    query.addBindValue(nowSecs());
    query.addBindValue(peerId);
    query.exec();
}

// ---------------- 客户端：我配对过的主机 ----------------

bool TrustStore::upsertHost(const Record &record)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO trusted_hosts (peer_id, name, token_id, token, perm, last_seen)"
        " VALUES (?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(peer_id) DO UPDATE SET"
        "   name=excluded.name, token_id=excluded.token_id, token=excluded.token,"
        "   perm=excluded.perm, last_seen=excluded.last_seen"));
    query.addBindValue(record.peerId);
    query.addBindValue(record.name);
    query.addBindValue(record.tokenId);
    query.addBindValue(record.token);
    query.addBindValue(record.perm.isEmpty() ? QStringLiteral("ro") : record.perm);
    query.addBindValue(nowSecs());
    return query.exec();
}

TrustStore::Record TrustStore::host(const QString &peerId) const
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT * FROM trusted_hosts WHERE peer_id = ?"));
    query.addBindValue(peerId);
    if (query.exec() && query.next())
        return recordFromQuery(query);
    return {};
}

QVector<TrustStore::Record> TrustStore::hosts() const
{
    QVector<Record> list;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT * FROM trusted_hosts ORDER BY last_seen DESC"));
    if (query.exec()) {
        while (query.next())
            list.append(recordFromQuery(query));
    }
    return list;
}

bool TrustStore::removeHost(const QString &peerId)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM trusted_hosts WHERE peer_id = ?"));
    query.addBindValue(peerId);
    return query.exec();
}

void TrustStore::touchHost(const QString &peerId)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("UPDATE trusted_hosts SET last_seen = ? WHERE peer_id = ?"));
    query.addBindValue(nowSecs());
    query.addBindValue(peerId);
    query.exec();
}

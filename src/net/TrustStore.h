#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QVector>

/**
 * 配对信任库（SQLite，设计文档 §7.3）。
 *
 * 两张表，因为“我信任谁”和“谁信任我”是两件独立的事：
 *   - `trusted_clients`：我是共享端，记录被允许访问我的设备及其 perm（ro/rw）
 *   - `trusted_hosts`：我是客户端，记录我配对过的主机及其令牌
 *
 * token 明文存库是刻意的：本机文件系统已经是信任边界，再套一层加密只是把密钥换个地方放。
 * 但 token **绝不上日志、不上网络**——网络上传的是 HMAC(token, nonce)。
 */
class TrustStore
{
public:
    struct Record
    {
        QString peerId;
        QString name;
        QString tokenId;
        QString token;      // hex
        QString perm;       // "ro" | "rw"
        qint64  lastSeen = 0;
        bool    blocked = false;
    };

    TrustStore();
    ~TrustStore();

    bool open(const QString &path, QString *error = nullptr);
    bool isOpen() const;
    QString path() const { return m_path; }

    // ---- 共享端视角：被信任的客户端 ----
    bool upsertClient(const Record &record);
    Record client(const QString &peerId) const;
    QVector<Record> clients() const;
    bool setClientPerm(const QString &peerId, const QString &perm);
    bool removeClient(const QString &peerId);
    void touchClient(const QString &peerId);

    // ---- 客户端视角：我配对过的主机 ----
    bool upsertHost(const Record &record);
    Record host(const QString &peerId) const;
    QVector<Record> hosts() const;
    bool removeHost(const QString &peerId);
    void touchHost(const QString &peerId);

private:
    bool migrate(QString *error);

    QSqlDatabase m_db;
    QString m_connectionName;
    QString m_path;
};

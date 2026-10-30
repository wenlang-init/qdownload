#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QHash>
#include <QVector>

class QTimer;

/**
 * 共享端的配对会话管理（设计文档 §7.2）。
 *
 * 关键设计：
 *   - 配对码**由共享端生成并显示**，不是客户端自己填——凭“能看到对方屏幕”证明物理邻近
 *   - 网上传的只有 `HMAC(code, nonce + peerId)`，码本身不出网
 *   - “验证通过”和“主机同意”是两件事：码对了还要主人点「允许」，两者都满足才发令牌
 *     这样即使码被旁观者看到，也得主人放行
 *   - 连错 5 次锁 60 秒，避免 6 位码被暴力枚举
 */
class PairingService : public QObject
{
    Q_OBJECT

public:
    static constexpr int kCodeTtlMs = 120 * 1000;
    static constexpr int kMaxAttempts = 5;
    static constexpr int kLockoutMs = 60 * 1000;

    struct Pending
    {
        QString peerId;
        QString name;
        QString platform;
        QString address;
        QString code;
        QString nonce;
        QDateTime createdAt;
        bool approved = false;
        bool useRw = false;
        bool verified = false;
        int attempts = 0;
    };

    explicit PairingService(QObject *parent = nullptr);

    /// 客户端发起配对：生成码与 nonce。返回 false 表示该 peer 已有进行中的请求或处于锁定期
    bool begin(const QString &peerId, const QString &name,
               const QString &platform, const QString &address);

    bool hasPending() const;
    QString firstPendingPeerId() const;
    const Pending *pending(const QString &peerId) const;
    QString codeFor(const QString &peerId) const;
    int expiresInSeconds(const QString &peerId) const;

    /// 校验客户端提交的摘要。返回空字符串表示通过，否则是失败原因
    QString verify(const QString &peerId, const QString &nonce, const QString &mac, int *remaining = nullptr);

    /// 主机点了「允许」；allowWrite 决定下发 rw 还是 ro
    void approve(const QString &peerId, bool allowWrite);
    void reject(const QString &peerId);

    void clear(const QString &peerId);

    /// QML 弹窗用：最早一条待确认请求
    QVariantMap pendingInfo() const;

    // ---- 联调/自动化：固定码 + 自动放行（不写日志以外的副作用）----
    void setFixedCode(const QString &code);
    void setAutoApprove(bool enabled, bool allowWrite);

signals:
    /// 请求产生 / 被清除 / 倒计时变化 / 状态推进，统一走这一个信号，UI 重读 pendingInfo
    void pendingChanged();
    void approved(const QString &peerId);
    void rejected(const QString &peerId, const QString &reason);
    void blocked(const QString &peerId, int retryAfterSeconds);

private:
    void expireIfNeeded();
    void startTicker();
    void stopTickerIfIdle();

    QHash<QString, Pending> m_pending;
    QHash<QString, QDateTime> m_lockedUntil;
    QTimer *m_ticker = nullptr;
    QString m_fixedCode;
    bool m_autoApprove = false;
    bool m_autoApproveWrite = false;
};

#include "PairingService.h"

#include "CryptoUtil.h"

#include <QDateTime>
#include <QRandomGenerator>
#include <QTimer>

namespace {

QString generateCode()
{
    // 6 位数字，首位非 0，避免显示成 "012345" 被误当成 5 位
    const quint32 value = QRandomGenerator::system()->bounded(100000, 1000000);
    return QString::number(value);
}

} // namespace

PairingService::PairingService(QObject *parent)
    : QObject(parent)
{
}

void PairingService::setFixedCode(const QString &code)
{
    m_fixedCode = code;
}

void PairingService::setAutoApprove(bool enabled, bool allowWrite)
{
    m_autoApprove = enabled;
    m_autoApproveWrite = allowWrite;
}

bool PairingService::hasPending() const
{
    return !m_pending.isEmpty();
}

QString PairingService::firstPendingPeerId() const
{
    QString bestId;
    QDateTime bestTime;
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it) {
        if (bestId.isEmpty() || it.value().createdAt < bestTime) {
            bestId = it.key();
            bestTime = it.value().createdAt;
        }
    }
    return bestId;
}

const PairingService::Pending *PairingService::pending(const QString &peerId) const
{
    auto it = m_pending.constFind(peerId);
    if (it == m_pending.constEnd())
        return nullptr;
    return &it.value();
}

QString PairingService::codeFor(const QString &peerId) const
{
    auto it = m_pending.constFind(peerId);
    return it == m_pending.constEnd() ? QString() : it.value().code;
}

int PairingService::expiresInSeconds(const QString &peerId) const
{
    auto it = m_pending.constFind(peerId);
    if (it == m_pending.constEnd())
        return 0;
    const qint64 remain = it.value().createdAt.msecsTo(QDateTime::currentDateTime()) ;
    return qMax(0, int((kCodeTtlMs - remain) / 1000));
}

bool PairingService::begin(const QString &peerId, const QString &name,
                           const QString &platform, const QString &address)
{
    if (peerId.isEmpty())
        return false;

    const QDateTime now = QDateTime::currentDateTime();

    auto locked = m_lockedUntil.constFind(peerId);
    if (locked != m_lockedUntil.constEnd()) {
        if (locked.value() > now) {
            const int retry = int(now.secsTo(locked.value()));
            emit blocked(peerId, retry);
            return false;
        }
        m_lockedUntil.erase(locked);
    }

    Pending pending;
    pending.peerId = peerId;
    pending.name = name;
    pending.platform = platform;
    pending.address = address;
    pending.code = m_fixedCode.isEmpty() ? generateCode() : m_fixedCode;
    pending.nonce = Qdl::randomHex(16);
    pending.createdAt = now;
    pending.approved = m_autoApprove;
    pending.useRw = m_autoApproveWrite;

    m_pending.insert(peerId, pending);

    qInfo().noquote() << QStringLiteral("配对请求：%1 (%2) 来自 %3，配对码 %4")
                             .arg(name, peerId, address, pending.code);

    startTicker();
    emit pendingChanged();

    // 自动放行仅用于联调；正常路径等主人点「允许」
    if (pending.approved)
        emit approved(peerId);

    return true;
}

QString PairingService::verify(const QString &peerId, const QString &nonce,
                               const QString &mac, int *remaining)
{
    auto it = m_pending.find(peerId);
    if (it == m_pending.end())
        return QStringLiteral("no_request");

    Pending &pending = it.value();

    if (pending.createdAt.msecsTo(QDateTime::currentDateTime()) > kCodeTtlMs) {
        m_pending.erase(it);
        emit pendingChanged();
        return QStringLiteral("expired");
    }

    if (nonce != pending.nonce)
        return QStringLiteral("bad_nonce");

    // mac = HMAC(code, nonce + peerId)
    const QString expected = Qdl::hmacHex(pending.code.toUtf8(),
                                          (nonce + peerId).toUtf8());

    if (!Qdl::constantTimeEquals(expected, mac)) {
        ++pending.attempts;
        const int left = qMax(0, kMaxAttempts - pending.attempts);
        if (remaining)
            *remaining = left;

        if (pending.attempts >= kMaxAttempts) {
            m_lockedUntil.insert(peerId, QDateTime::currentDateTime().addMSecs(kLockoutMs));
            const QString name = pending.name;
            const QString peer = pending.peerId;
            m_pending.erase(it);
            emit pendingChanged();
            qWarning().noquote() << QStringLiteral("配对码连续错误 %1 次，锁定 60 秒：%2")
                                        .arg(kMaxAttempts).arg(name);
            emit blocked(peer, kLockoutMs / 1000);
            return QStringLiteral("locked");
        }

        emit pendingChanged();
        return QStringLiteral("bad_code");
    }

    pending.verified = true;
    pending.attempts = 0;
    emit pendingChanged();
    return QString();
}

void PairingService::approve(const QString &peerId, bool allowWrite)
{
    auto it = m_pending.find(peerId);
    if (it == m_pending.end())
        return;

    it.value().approved = true;
    it.value().useRw = allowWrite;

    qInfo().noquote() << QStringLiteral("主机已允许配对：%1（%2）")
                             .arg(it.value().name, allowWrite ? QStringLiteral("rw") : QStringLiteral("ro"));

    emit pendingChanged();
    emit approved(peerId);
}

void PairingService::reject(const QString &peerId)
{
    auto it = m_pending.find(peerId);
    if (it == m_pending.end())
        return;

    const QString name = it.value().name;
    m_pending.erase(it);

    qInfo().noquote() << QStringLiteral("主机拒绝了配对请求：%1").arg(name);

    emit pendingChanged();
    emit rejected(peerId, QStringLiteral("rejected"));
}

void PairingService::clear(const QString &peerId)
{
    if (m_pending.remove(peerId) > 0) {
        stopTickerIfIdle();
        emit pendingChanged();
    }
}

QVariantMap PairingService::pendingInfo() const
{
    QVariantMap info;
    const QString peerId = firstPendingPeerId();
    if (peerId.isEmpty())
        return info;

    const Pending *pending = this->pending(peerId);
    if (!pending)
        return info;

    info.insert(QStringLiteral("peerId"), pending->peerId);
    info.insert(QStringLiteral("name"), pending->name);
    info.insert(QStringLiteral("platform"), pending->platform);
    info.insert(QStringLiteral("address"), pending->address);
    info.insert(QStringLiteral("code"), pending->code);
    info.insert(QStringLiteral("verified"), pending->verified);
    info.insert(QStringLiteral("approved"), pending->approved);
    info.insert(QStringLiteral("useRw"), pending->useRw);
    info.insert(QStringLiteral("expiresIn"), expiresInSeconds(peerId));
    info.insert(QStringLiteral("attempts"), pending->attempts);
    return info;
}

void PairingService::expireIfNeeded()
{
    const QDateTime now = QDateTime::currentDateTime();
    QVector<QString> expired;

    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it) {
        if (it.value().createdAt.msecsTo(now) > kCodeTtlMs)
            expired.append(it.key());
    }

    for (const QString &peerId : expired) {
        m_pending.remove(peerId);
        qInfo().noquote() << QStringLiteral("配对码已过期：%1").arg(peerId);
        emit rejected(peerId, QStringLiteral("expired"));
    }

    if (!expired.isEmpty())
        emit pendingChanged();
}

void PairingService::startTicker()
{
    if (!m_ticker) {
        m_ticker = new QTimer(this);
        m_ticker->setInterval(1000);
        connect(m_ticker, &QTimer::timeout, this, [this]() {
            expireIfNeeded();
            // 倒计时要在界面上走字，直接复用 pendingChanged
            if (hasPending())
                emit pendingChanged();
            else
                stopTickerIfIdle();
        });
    }
    if (!m_ticker->isActive())
        m_ticker->start();
}

void PairingService::stopTickerIfIdle()
{
    if (m_ticker && m_ticker->isActive() && !hasPending())
        m_ticker->stop();
}

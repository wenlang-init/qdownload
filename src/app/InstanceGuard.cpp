#include "InstanceGuard.h"

#include "DataPaths.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QLockFile>
#include <QStandardPaths>
#include <QUuid>

namespace {

/// 同机最多认这么多个实例；再多就退化成"无锁的临时实例"（照常能跑，只是编号不保证唯一）
constexpr int kMaxInstances = 8;
/// 进程被硬杀时锁文件会留下：5 秒后允许按 PID 判死并回收
constexpr int kStaleLockMs = 5000;

QLockFile *g_lock = nullptr;
int g_index = 0;
QList<QLockFile *> g_claims;   // 本进程认领的 peerId，活到进程结束

QString lockPathFor(int index)
{
    return QDir(Qdl::DataPaths::recordsRoot())
        .filePath(QStringLiteral("instance-%1.lock").arg(index));
}

/// peerId 的认领目录必须**跳出程序目录**：整份拷一份程序（或搬一个 data/ 过来）
/// 后两个副本的 recordsRoot 不同，只有放到机器级目录里才拦得住重名。
QString claimDirectory()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (base.isEmpty())
        base = QDir::tempPath();
    const QString dir = QDir(base).filePath(QStringLiteral("qdownload/instances"));
    QDir().mkpath(dir);
    return dir;
}

QString claimPathFor(const QString &peerId)
{
    return QDir(claimDirectory()).filePath(QStringLiteral("peer-%1.lock").arg(peerId));
}

/// 拿锁（顺便回收硬杀留下的死锁）
bool tryLockWithStaleCheck(QLockFile *lock)
{
    lock->setStaleLockTime(kStaleLockMs);
    return lock->tryLock(0) || (lock->removeStaleLockFile() && lock->tryLock(0));
}

/// 清掉硬杀留下的死认领文件，免得目录越攒越多（数量只影响默认名的编号）
void sweepStaleClaims()
{
    const QDir dir(claimDirectory());
    const QStringList files = dir.entryList({QStringLiteral("peer-*.lock")}, QDir::Files);
    for (const QString &name : files) {
        QLockFile lock(dir.filePath(name));
        lock.setStaleLockTime(kStaleLockMs);
        lock.removeStaleLockFile();
    }
}

void ensureCleanupRegistered()
{
    static bool added = false;
    if (added)
        return;
    added = true;

    // 正常退出时把锁文件删掉：不然下次启动会先把它当成"被占用"，
    // 白白退到第 2 个实例（身份与设置都跟着换一套，看起来像换了台设备）
    qAddPostRoutine([]() {
        for (QLockFile *lock : g_claims) {
            lock->unlock();
            delete lock;
        }
        g_claims.clear();

        if (g_lock) {
            g_lock->unlock();
            delete g_lock;
            g_lock = nullptr;
        }
    });
}

} // namespace

int Qdl::InstanceGuard::acquire()
{
    if (g_index > 0)
        return g_index;

    const QString root = Qdl::DataPaths::recordsRoot();
    if (root.isEmpty()) {
        // DataPaths 还没 init（多半是调用顺序错了）：退化成主实例，至少不会崩
        g_index = 1;
        return g_index;
    }

    ensureCleanupRegistered();
    sweepStaleClaims();

    for (int n = 1; n <= kMaxInstances; ++n) {
        auto *lock = new QLockFile(lockPathFor(n));
        if (tryLockWithStaleCheck(lock)) {
            g_lock = lock;
            g_index = n;
            break;
        }
        delete lock;
    }

    if (g_index == 0) {
        // 8 个都被占了：仍然放行，编号取 9（配置目录不会和任何一个已占用的撞上）
        g_index = kMaxInstances + 1;
        qWarning().noquote() << QStringLiteral("同机实例已达上限（%1），本次按临时实例启动")
                                    .arg(kMaxInstances);
    }

    if (g_index > 1) {
        qInfo().noquote() << QStringLiteral("同机多开：本次是本机第 %1 个实例，身份与设置独立存放")
                                 .arg(g_index);
    }

    return g_index;
}

int Qdl::InstanceGuard::index()
{
    return g_index > 0 ? g_index : 1;
}

QString Qdl::InstanceGuard::nameSuffix()
{
    const int n = index();
    return n > 1 ? QStringLiteral(" (%1)").arg(n) : QString();
}

bool Qdl::InstanceGuard::claimPeerId(const QString &peerId)
{
    if (peerId.isEmpty())
        return false;

    ensureCleanupRegistered();

    auto *lock = new QLockFile(claimPathFor(peerId));
    if (tryLockWithStaleCheck(lock)) {
        g_claims.append(lock);
        return true;
    }
    delete lock;
    return false;
}

QString Qdl::InstanceGuard::newPeerId()
{
    // 撞了极少数情况（UUID 重复或认领失败）就再摇一个
    for (int i = 0; i < 8; ++i) {
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (claimPeerId(id))
            return id;
    }
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

int Qdl::InstanceGuard::livePeerClaimCount()
{
    // 只用于给默认设备名编号，数不准也无所谓（多算一个比撞名好）
    const QStringList files = QDir(claimDirectory()).entryList({QStringLiteral("peer-*.lock")},
                                                               QDir::Files);
    return qMax(1, int(files.size()));
}

QString Qdl::InstanceGuard::scopedSettingsPath(const QString &basePath)
{
    const int n = index();
    if (n <= 1)
        return basePath;

    // 每个副实例一份自己的 ini：共用一份的话，两个进程各自 sync() 会互相覆盖，
    // 首启那次最容易把对方的 peerId 写丢（然后下次启动身份就变了）
    const QString dir = QDir(Qdl::DataPaths::recordsRoot())
                            .filePath(QStringLiteral("instance-%1").arg(n));
    QDir().mkpath(dir);
    return QDir(dir).filePath(QStringLiteral("settings.ini"));
}

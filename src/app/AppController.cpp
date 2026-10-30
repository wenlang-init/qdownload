#include "AppController.h"

#include "AndroidService.h"
#include "AndroidStorage.h"
#include "DataPaths.h"
#include "InstanceGuard.h"
#include "LaunchOptions.h"
#include "Logger.h"
#include "SystemNotifier.h"
#include "Translation.h"
#include "net/ControlClient.h"
#include "net/ControlServer.h"
#include "net/DataServer.h"
#include "net/DiscoveryService.h"
#include "net/PairingService.h"
#include "net/PeerListModel.h"
#include "fs/FileScanner.h"
#include "fs/SafePath.h"
#include "transfer/RateLimiter.h"
#include "transfer/ResumeStore.h"
#include "transfer/TransferManager.h"
#include "transfer/TransferModel.h"
#include "transfer/TransferTask.h"
#include "transfer/TransferTypes.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QQmlEngine>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#include <QUuid>

namespace {
constexpr int kRemotePageSize = 200;    // 远端每页条数：滚到底再要下一页
constexpr int kLocalListLimit = 2000;   // 本机一次扫完（不做分页，超出只提示）

// 出厂默认：共享根放在“文档”下（用户找得到、也愿意放文件的地方），
// 接收目录放在应用数据下（下载的杂物，不该占着用户视线）。
//
// Android 例外（分区存储）：公共 Documents 从 Android 10 起用 File API 直接写会 EACCES，
// 出厂默认必须落在应用专属目录里；想指到别处得先拿到“所有文件访问权限”，
// 拿到后 built-in 目录浏览器会把整个外部存储开放出来。
QString defaultShareRootPath()
{
#ifdef Q_OS_ANDROID
    const QString appDir = Qdl::AndroidStorage::preferredDir();
    if (!appDir.isEmpty())
        return QDir(appDir).filePath(QStringLiteral("share"));
#endif
    QString base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (base.isEmpty())
        base = QDir::homePath();
    return QDir(base).filePath(QStringLiteral("qdownload"));
}

QString defaultReceiveRootPath()
{
#ifdef Q_OS_ANDROID
    const QString appDir = Qdl::AndroidStorage::preferredDir();
    if (!appDir.isEmpty())
        return QDir(appDir).filePath(QStringLiteral("downloads"));
#endif
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.qdownload");
    return QDir(base).filePath(QStringLiteral("downloads"));
}

// 目录互斥判定用。大小写不敏感（Windows 上 D:\Foo 和 d:\foo 是同一个目录），
// 统一补一个 '/ 再比前缀，否则 .../share 会被误判成 .../share2 的父目录
QString comparableDir(const QString &path)
{
    return QDir::cleanPath(path).toCaseFolded() + QLatin1Char('/');
}

// 真正试一次能不能写。Windows 上 QFileInfo::isWritable() 只看「只读」属性、不看 ACL，
// 拿它判断系统目录会得到“可写”（实测 C:\Windows\System32\config 也返回 true），
// 于是这里直接落一个临时文件：能建出来才算数。
// callerCreated：调用方应据此在函数返回 false 时把刚 mkpath 出来的目录清理掉
bool probeWritable(const QString &path, bool *callerCreated, QString *reason)
{
    const QDir target(path);
    const bool existed = target.exists();
    *callerCreated = false;

    if (!existed && !QDir().mkpath(path)) {
        *reason = QCoreApplication::translate("AppController", "建不出这个目录：%1").arg(path);
        return false;
    }
    if (!existed)
        *callerCreated = true;

    QTemporaryFile probe(QDir(path).filePath(
        QStringLiteral(".qdownload-write-probe-XXXXXX")));
    if (!probe.open()) {
        *reason = QCoreApplication::translate("AppController", "没有写入权限：%1").arg(path);
        return false;
    }
    probe.close();
    return true;
}
} // namespace

AppController::AppController(QObject *parent)
    : QObject(parent)
{
    // ---------------- 设置存放位置 ----------------
    // 便携模式用 exe 旁边的 settings.ini；装在系统目录时用原生位置（Windows 注册表）。
    // 第一次切到 ini 时把老配置搬过来，不然用户已配对的设备全丢。
    //
    // 同机多开时（本机第 2 个及以后的进程）走各自独立的 instance-<n>/settings.ini：
    // 共用一份配置会让两个进程拿到同一个 peerId，组播里互相把对方当"自己"丢掉。
    m_settingsFile = Qdl::InstanceGuard::scopedSettingsPath(Qdl::DataPaths::settingsFilePath());
    m_settings = openSettings();
    qInfo().noquote() << "数据存储：" << (Qdl::DataPaths::portable() ? QStringLiteral("便携模式")
                                                                    : QStringLiteral("安装模式"))
                      << Qdl::DataPaths::recordsRoot();
    if (Qdl::InstanceGuard::index() > 1) {
        qInfo().noquote() << QStringLiteral("同机实例 #%1 的配置文件：%2")
                                 .arg(Qdl::InstanceGuard::index())
                                 .arg(m_settingsFile);
    }

    // ---------------- 配置 ----------------
    m_shareEnabled = m_settings->value(QStringLiteral("share/enabled"), false).toBool();
    m_writeEnabled = m_settings->value(QStringLiteral("share/writeEnabled"), false).toBool();
    m_uploadPolicy = m_settings->value(QStringLiteral("share/uploadPolicy"), 0).toInt();
    // 并发上限：任务是一条数据连接，一侧多个文件同时开跑会互相抢带宽，默认 3
    m_transferThreads = qBound(kTransferThreadsMin,
                               m_settings->value(QStringLiteral("transfer/threads"),
                                                 kTransferThreadsDefault).toInt(),
                               kTransferThreadsMax);
    // 全局限速（MB/s，0 = 不限）。命令行 --limit 只作用于本次运行，不写回设置
    m_speedLimitMbps = qBound(0,
                              m_settings->value(QStringLiteral("transfer/speedLimitMbps"),
                                                kSpeedLimitDefaultMbps).toInt(),
                              kSpeedLimitMaxMbps);
    if (launchOptions().speedLimitOverrideMbps >= 0)
        m_speedLimitMbps = qBound(0, launchOptions().speedLimitOverrideMbps, kSpeedLimitMaxMbps);
    Qdl::RateLimiter::instance()->setLimitForAll(qint64(m_speedLimitMbps) * 1024 * 1024);

    // 单文件连接数：一份文件同时开几条连接并行拉，高延迟链路上单流跑不满的问题
    // 主要靠这个解决。默认 3（对局域网几乎无感，Wi-Fi 上提升明显）
    m_segmentCount = qBound(kSegmentMin,
                            m_settings->value(QStringLiteral("transfer/segments"),
                                              kSegmentDefault).toInt(),
                            kSegmentMax);
    if (launchOptions().segmentOverride > 0)
        m_segmentCount = qBound(kSegmentMin, launchOptions().segmentOverride, kSegmentMax);
    // 默认「关窗口就退出」：多数人点 × 是想要真的关掉。想让它在后台接着收就去设置里打开
    m_closeToTray = m_settings->value(QStringLiteral("ui/closeToTray"), false).toBool();
    if (launchOptions().closeToTrayOverride >= 0)
        m_closeToTray = launchOptions().closeToTrayOverride == 1;

    // 语言由 main 合并好 --lang 与设置再塞进来；这里兜底读一次（--pair-test 等路径）
    m_language = launchOptions().language;
    if (m_language.isEmpty())
        m_language = m_settings->value(QStringLiteral("ui/language"),
                                      QStringLiteral("zh_CN")).toString();

    // 命令行强制值（联调/演示用），不写回设置
    if (launchOptions().shareOverride >= 0)
        m_shareEnabled = launchOptions().shareOverride != 0;
    if (launchOptions().writeOverride >= 0)
        m_writeEnabled = launchOptions().writeOverride != 0;

    // 安卓能不能读写公共目录，取回的是系统当前状态（用户在设置页随时能改）
    m_allFilesAccess = Qdl::AndroidStorage::hasFullAccess();

    m_shareRoot = m_settings->value(QStringLiteral("share/root"), defaultShareRootPath()).toString();
    if (!launchOptions().shareRootOverride.isEmpty())
        m_shareRoot = launchOptions().shareRootOverride;
    QDir().mkpath(m_shareRoot);

    // ---------------- 目录列表（M4）----------------
    // 本机页 = 接收目录（下载落点），与“共享给别人的目录”刻意分开：
    // 一个是“我要看的”，一个是“我给别人看的”，混在一起容易误删
    m_localRoot = m_settings->value(QStringLiteral("receive/root"), defaultReceiveRootPath()).toString();
    if (!launchOptions().receiveRootOverride.isEmpty())
        m_localRoot = launchOptions().receiveRootOverride;
    QDir().mkpath(m_localRoot);

    // 上次能写不代表这次还能写（Android 重装后应用专属目录会换、桌面可能被策略收走权限）。
    // 写不进去就退回出厂默认，总比让用户面对一个“开关拨了没反应”的界面强
    repairUnusableRoots();

    m_localFiles = new Qdl::FileListModel(this);
    m_remoteFiles = new Qdl::FileListModel(this);
    m_localBreadcrumb = makeBreadcrumb(QStringLiteral("/"), tr("接收目录"));
    m_remoteBreadcrumb = makeBreadcrumb(QStringLiteral("/"), tr("共享根"));

    // peerId 长期不变：断点归属、信任关系、白名单都挂在它上面。
    // 但"不变"不等于"本机唯一"——把程序目录整份拷一份（还带着 data/）、或者从别处搬一个
    // data/ 过来，两个进程就带着同一个 peerId 跑：组播里双方都会把对方的心跳当成自己的
    // 回环包丢掉，表现就是"两份程序在不同的目录里互相发现不了"。
    // 所以拿到 id 之后还要在**本机范围**内认领一次，被占就必须换一个。
    m_peerId = launchOptions().peerIdOverride;
    const bool forcedPeerId = !m_peerId.isEmpty();
    if (!forcedPeerId)
        m_peerId = m_settings->value(QStringLiteral("net/peerId")).toString();

    if (m_peerId.isEmpty()) {
        m_peerId = Qdl::InstanceGuard::newPeerId();
        persist(QStringLiteral("net/peerId"), m_peerId);
        qInfo().noquote() << "首次启动，生成 peerId =" << m_peerId;
    } else if (!Qdl::InstanceGuard::claimPeerId(m_peerId)) {
        if (forcedPeerId) {
            // 命令行强制的身份照用（联调脚本要靠它对准 peerId），但得留一句提醒
            qWarning().noquote() << QStringLiteral("peerId %1 已被本机另一个进程占用，"
                                                   "两个进程会互相看不见（--instance 请换个 tag）")
                                        .arg(m_peerId);
        } else {
            const QString taken = m_peerId;
            m_peerId = Qdl::InstanceGuard::newPeerId();
            persist(QStringLiteral("net/peerId"), m_peerId);
            qWarning().noquote() << QStringLiteral("peerId %1 已被本机另一个进程占用"
                                                   "（多半是复制过程序目录或 data/），改用 %2")
                                        .arg(taken, m_peerId);

            // 顺手把默认名也区分开：两份程序默认同名，主机列表里两条一模一样的名字没法选
            if (launchOptions().nameOverride.isEmpty()
                && m_settings->value(QStringLiteral("net/deviceName")).toString().isEmpty()) {
                const QString base = QSysInfo::machineHostName();
                if (!base.isEmpty()) {
                    persist(QStringLiteral("net/deviceName"),
                            QStringLiteral("%1 (%2)")
                                .arg(base)
                                .arg(Qdl::InstanceGuard::livePeerClaimCount()));
                }
            }
        }
    }

    // ---------------- 信任库 / 断点库 ----------------
    // 与设置的落脚点一致（都在程序目录 data/ 下），整个程序拷走就能带走全部状态
    Qdl::DataPaths::migrateLegacy({QStringLiteral("trust.db"), QStringLiteral("resume.db")});
    const QString dataDir = Qdl::DataPaths::recordsRoot();

    const QString dbPath = QDir(dataDir).filePath(QStringLiteral("trust.db"));

    QString trustError;
    if (!m_trust.open(dbPath, &trustError))
        qWarning().noquote() << "信任库打开失败：" << trustError;
    else
        qInfo().noquote() << "信任库：" << dbPath
                          << QStringLiteral("已配对客户端 %1 台").arg(m_trust.clients().size());

    // ---------------- 配对与控制通道 ----------------
    m_pairing = new PairingService(this);
    if (!launchOptions().pairCode.isEmpty())
        m_pairing->setFixedCode(launchOptions().pairCode);
    if (launchOptions().autoApprove)
        m_pairing->setAutoApprove(true, launchOptions().autoApproveWrite);

    m_server = new ControlServer(&m_trust, m_pairing, this);
    m_server->setHostIdentity(m_peerId, deviceName(), platformName());
    m_server->setShareRoot(m_shareRoot);
    m_server->setShareState(m_shareEnabled, m_writeEnabled, m_uploadPolicy);

    QString serverError;
    if (!m_server->start(launchOptions().ctrlPort, &serverError)) {
        m_discoveryOk = false;
        m_discoveryError = serverError;
    }

    m_client = new ControlClient(&m_trust, this);
    m_client->setSelf(m_peerId, deviceName(), platformName());

    // ---------------- 传输（M5）----------------
    m_transfers = new Qdl::TransferManager(this);
    connect(m_transfers->model(), &Qdl::TransferModel::changed, this, [this]() {
        emit transfersChanged();
        updateForegroundService();
    });
    connect(m_transfers, &Qdl::TransferManager::taskFinished, this,
            [this](Qdl::TransferTask *task) {
                if (task->state() != Qdl::TransferState::Completed)
                    return;

                if (task->direction() == Qdl::TransferDirection::Upload) {
                    setTransferNotice(tr("已上传 %1").arg(task->name()));
                    emit notify(tr("上传完成"),
                                tr("%1 已存到对方的收件箱").arg(task->name()));
                    return;
                }

                setTransferNotice(tr("已下载 %1").arg(task->name()));
                emit notify(tr("下载完成"),
                            tr("%1 已存到本机接收目录").arg(task->name()));
                // 落点就在本机页当前目录里，立刻刷新——否则用户会以为没下下来
                localRefresh();
            });

    // ---------------- 断点库（M6）----------------
    m_resumeStore = new Qdl::ResumeStore;
    QString resumeError;
    if (!m_resumeStore->open(QDir(dataDir).filePath(QStringLiteral("resume.db")), &resumeError))
        qWarning().noquote() << "断点库打开失败：" << resumeError;
    else
        qInfo().noquote() << "断点库：" << m_resumeStore->path();
    m_transfers->setResumeStore(m_resumeStore);

    // 每个任务（含恢复出来的）落地后都要接校验流程；必须在 restoreResumable 之前接
    connect(m_transfers, &Qdl::TransferManager::taskCreated, this,
            [this](Qdl::TransferTask *task) {
                startVerifyFlow(task);
                // 每个任务开始时先记下当前连接数；运行中改设置会再调一次 setWorkerCount
                task->setWorkerCount(m_segmentCount);
                // 任务让出槽位（完成/失败/取消/暂停）就把排队的补上来。
                // 两个信号都要接：finished 覆盖正常结束，changed 覆盖暂停/取消
                connect(task, &Qdl::TransferTask::finished, this, [this]() {
                    drainTransferQueues();
                });
                connect(task, &Qdl::TransferTask::changed, this, [this]() {
                    drainTransferQueues();
                });

                // 空闲连接要领下一段干活了：替它去主机那儿领票（M12）。
                // 用 reqId 回连，任务在中间被删掉也不会串号。
                connect(task, &Qdl::TransferTask::segmentNeeded, this,
                        [this](const QString &taskId, int index, qint64 done) {
                            Qdl::TransferTask *t = m_transfers->find(taskId);
                            if (!t)
                                return;
                            if (t->transferId().isEmpty())
                                return;
                            // 上传方向：本端摘要算好后搭"领段"的顺风车一起送过去；
                            // 下载时 checksum() 是本端自己算的，不往主机发（主机不需要）。
                            const QString piggyback =
                                t->direction() == Qdl::TransferDirection::Upload
                                    ? t->checksum()
                                    : QString();
                            const QString reqId = m_client->requestSegment(t->transferId(), index,
                                                                           done, piggyback);
                            if (reqId.isEmpty()) {
                                t->onSegmentDenied(index, 503, tr("控制通道未就绪"));
                                return;
                            }
                            PendingSegment pending;
                            pending.taskId = taskId;
                            pending.index = index;
                            m_pendingSegments.insert(reqId, pending);
                        });
            });

    // 上次没下完的任务恢复成「已暂停」放进抽屉，等用户点继续：
    // 启动就自动往外拉数据，用户会以为自己在被偷带宽
    const int restoredCount = m_transfers->restoreResumable();
    if (restoredCount > 0) {
        setTransferNotice(tr("有 %1 个未完成的下载，点继续可接着下").arg(restoredCount));
        emit transfersChanged();
    }

    // ---------------- 发现 ----------------
    m_peers = new PeerListModel(this);
    m_discovery = new DiscoveryService(this);
    m_discovery->setSelf(buildSelf());

    connect(m_discovery, &DiscoveryService::peerUpserted, this, [this](const PeerInfo &peer) {
        m_peers->upsert(peer);
        refreshCounts();

        if (m_selectedPeerId.isEmpty() && peer.online && peer.sharing) {
            setSelectedPeerId(peer.peerId);
        } else if (peer.peerId == m_selectedPeerId) {
            emit selectedPeerChanged();
            // 主机重新上线后自动重连，用户不用手动点
            if (peer.online && peer.sharing)
                maybeAutoConnect();
        }
    });

    connect(m_discovery, &DiscoveryService::peerRemoved, this, [this](const QString &peerId) {
        m_peers->remove(peerId);
        refreshCounts();
        if (peerId == m_selectedPeerId) {
            m_selectedPeerId.clear();
            m_remoteFiles->clear();
            m_remotePath.clear();
            updateRemoteBreadcrumb();
            setRemoteError(QString());
            setRemoteLoading(false);
            emit selectedPeerChanged();
            emit remotePathChanged();
            emit remoteListChanged();
        }
    });

    connect(m_discovery, &DiscoveryService::discoveryError, this, [this](const QString &message) {
        m_discoveryOk = false;
        m_discoveryError = message;
        emit discoveryErrorChanged();
    });

    // ---------------- 客户端侧状态 ----------------
    connect(m_client, &ControlClient::stateChanged, this, [this]() {
        emit connChanged();
        emit uploadHintChanged();

        if (m_client->isReady()) {
            // 连上（含断线重连）就重拉当前目录：主机那边可能已经变了
            QString target = m_remotePath;
            if (target.isEmpty())
                target = launchOptions().demoPath.isEmpty() ? QStringLiteral("/")
                                                            : launchOptions().demoPath;
            remoteGo(target);

            // 断线期间排的队现在可以放出来了
            drainTransferQueues();

            // 截图/演示：连上后自动下一份，让抽屉里有真实进度可看
            if (!m_demoGetStarted && !launchOptions().demoGetPath.isEmpty()) {
                m_demoGetStarted = true;
                const QString demoFile = launchOptions().demoGetPath;
                QTimer::singleShot(700, this, [this, demoFile]() { downloadPath(demoFile, 0); });
            }

            // 截图/演示：连上后自动传一份上去，让抽屉里有真实的上传进度
            if (!m_demoPutStarted && !launchOptions().demoPutPath.isEmpty()) {
                m_demoPutStarted = true;
                const QString localFile = launchOptions().demoPutPath;
                QTimer::singleShot(700, this, [this, localFile]() { uploadPaths({ localFile }); });
            }
        } else if (m_remoteLoading) {
            setRemoteLoading(false);
        }
    });
    connect(m_client, &ControlClient::permChanged, this, [this]() { emit connChanged();
        emit uploadHintChanged(); });

    connect(m_client, &ControlClient::listReceived, this,
            [this](const QString &path, int offset, int total,
                   const QVector<Qdl::FileEntry> &entries) {
                setRemoteLoading(false);
                setRemoteError(QString());

                if (offset == 0) {
                    m_remoteFiles->reset(path, entries, total);
                    if (m_remotePath != path) {
                        m_remotePath = path;
                        updateRemoteBreadcrumb();
                        emit remotePathChanged();
                    }
                } else {
                    m_remoteFiles->appendPage(path, offset, entries, total);
                }
                emit remoteListChanged();
            });

    connect(m_client, &ControlClient::listFailed, this,
            [this](const QString &path, int code, const QString &message) {
                Q_UNUSED(path)
                Q_UNUSED(code)
                setRemoteLoading(false);
                setRemoteError(message);
            });

    // ---------------- 下载（M5）----------------
    connect(m_client, &ControlClient::downloadAccepted, this,
            [this](const QString &reqId, const QString &path, const Qdl::DownloadGrant &grant) {
                const auto it = m_pendingDownloads.find(reqId);
                if (it == m_pendingDownloads.end()) {
                    qWarning().noquote() << "收到未知 reqId 的下载授权：" << reqId;
                    return;
                }
                const PendingDownload pending = it.value();
                m_pendingDownloads.erase(it);

                // 续传：复用原任务对象，进度条与历史状态连续
                if (!pending.taskId.isEmpty()) {
                    if (Qdl::TransferTask *task = m_transfers->find(pending.taskId)) {
                        task->setFileKey(grant.fileKey);   // 主机那边可能刚换了 key，以最新为准
                        task->setTotal(grant.size);
                        task->setTransferId(grant.transferId);
                        task->setWorkerCount(m_segmentCount);
                        task->start(m_client->peerAddress(), grant.port, grant);
                    }
                    return;
                }

                const QString dest = downloadDestination(path, grant.name);
                Qdl::TransferTask *task = m_transfers->createDownload(grant.name, path, dest,
                                                                     grant.size);
                task->setPeer(m_client->peerId(), m_client->peerName());
                task->setFileKey(grant.fileKey);
                task->setTransferId(grant.transferId);
                task->setWorkerCount(m_segmentCount);
                if (!task->start(m_client->peerAddress(), grant.port, grant))
                    setTransferNotice(task->errorText());
            });

    connect(m_client, &ControlClient::downloadRejected, this,
            [this](const QString &reqId, const QString &path, int code, const QString &message) {
                const auto it = m_pendingDownloads.find(reqId);
                QString taskId;
                if (it != m_pendingDownloads.end()) {
                    taskId = it.value().taskId;
                    m_pendingDownloads.erase(it);
                }
                qWarning().noquote() << QStringLiteral("下载被拒：%1（%2）- %3")
                                            .arg(path).arg(code).arg(message);

                // 409 = 源文件已经变了：断点作废，从头再来一次（只重来一次，避免死循环）
                if (code == 409 && !taskId.isEmpty()) {
                    if (Qdl::TransferTask *task = m_transfers->find(taskId)) {
                        setTransferNotice(tr("%1 在主机上已变化，重新从头下载").arg(task->name()));
                        task->resetToZero();
                        requestDownloadFor(path, 0, taskId);
                    }
                    return;
                }

                if (!taskId.isEmpty()) {
                    if (Qdl::TransferTask *task = m_transfers->find(taskId))
                        task->reject(message);
                }
                setTransferNotice(message);
            });

    // 主机把摘要推过来了（M13 正常路径）：交给任务存着，收尾当场比对
    connect(m_client, &ControlClient::checksumPushed, this,
            [this](const QString &transferId, const QString &sha256) {
                if (Qdl::TransferTask *task = m_transfers->findByTransferId(transferId))
                    task->setExpectedChecksum(sha256);
            });

    // 补拉（兜底通道）拿到的摘要：同样交给任务，比对逻辑只有一份
    connect(m_client, &ControlClient::checksumReceived, this,
            [this](const QString &reqId, const QString &path, const QString &sha256) {
                Q_UNUSED(path)
                const QString taskId = m_pendingChecksums.take(reqId);
                Qdl::TransferTask *task = taskId.isEmpty() ? nullptr : m_transfers->find(taskId);
                if (!task)
                    return;
                task->setExpectedChecksum(sha256);
            });

    // 兜底也拿不到（老版本主机 / 超时）：降级为"只校验了大小"，但要留痕
    connect(m_client, &ControlClient::checksumFailed, this,
            [this](const QString &reqId, const QString &path, const QString &message) {
                Q_UNUSED(path)
                const QString taskId = m_pendingChecksums.take(reqId);
                Qdl::TransferTask *task = taskId.isEmpty() ? nullptr : m_transfers->find(taskId);
                if (!task)
                    return;
                qWarning().noquote() << QStringLiteral("拿不到主机校验值：%1").arg(message);
                task->checksumUnavailable(tr("主机未提供校验值，仅校验了大小"));
            });

    // ---------------- 远端删除（M14）----------------
    connect(m_client, &ControlClient::deleteFinished, this,
            [this](const QString &reqId, const QString &path, bool ok, const QString &message) {
                Q_UNUSED(reqId)
                const QString name = QFileInfo(path).fileName();
                if (!ok) {
                    qWarning().noquote() << QStringLiteral("删除远端文件失败 %1：%2")
                                                .arg(path, message);
                    setTransferNotice(tr("删除「%1」失败：%2")
                                          .arg(name, message.isEmpty() ? tr("主机没有说明原因")
                                                                      : message));
                } else {
                    qInfo().noquote() << QStringLiteral("已删除远端文件 %1").arg(path);
                    setTransferNotice(tr("已删除「%1」").arg(name));
                }
                // 删除成功要刷新列表，失败也刷一次：可能文件本来就不在了，让界面说实话
                if (m_client && m_client->isReady())
                    remoteRefresh();
            });

    // ---------------- 分段领取（M12）----------------
    connect(m_client, &ControlClient::segmentGranted, this,
            [this](const QString &reqId, bool ok, int index, const QString &token,
                   qint64 offset, qint64 length, qint64 done, bool complete,
                   int code, const QString &message) {
                const auto it = m_pendingSegments.find(reqId);
                if (it == m_pendingSegments.end())
                    return;   // 迟到的应答（任务已被清掉）

                const PendingSegment pending = it.value();
                m_pendingSegments.erase(it);

                Qdl::TransferTask *task = m_transfers->find(pending.taskId);
                if (!task)
                    return;

                if (!ok) {
                    task->onSegmentDenied(index, code, message);
                    return;
                }
                task->onSegmentLease(index, token, offset, length, done, complete);
            });

    // ---------------- 上传（M7）----------------
    connect(m_client, &ControlClient::uploadAccepted, this,
            [this](const QString &reqId, const Qdl::UploadGrant &grant) {
                const auto it = m_pendingUploads.find(reqId);
                if (it == m_pendingUploads.end()) {
                    qWarning().noquote() << "收到未知 reqId 的上传授权：" << reqId;
                    return;
                }
                const PendingUpload pending = it.value();
                m_pendingUploads.erase(it);

                // 文件名与落点都以主机为准：重名时主机可能已经改成 name(1).ext
                Qdl::TransferTask *task = m_transfers->createUpload(grant.name, pending.localPath,
                                                                    grant.path, grant.size);
                task->setPeer(m_client->peerId(), m_client->peerName());
                task->setFileKey(pending.fileKey);
                task->setTransferId(grant.transferId);
                task->setWorkerCount(m_segmentCount);
                m_uploadTransferIds.insert(grant.transferId, task->id());

                if (!task->startUpload(m_client->peerAddress(), grant.port, grant)) {
                    setTransferNotice(task->errorText());
                    return;
                }
                if (grant.offset > 0) {
                    setTransferNotice(tr("主机已有 %1，从断点续传 %2")
                                          .arg(Qdl::TransferFmt::sizeText(grant.offset), grant.name));
                }
            });

    connect(m_client, &ControlClient::uploadRejected, this,            [this](const QString &reqId, int code, const QString &message) {
                const auto it = m_pendingUploads.find(reqId);
                const QString name = it != m_pendingUploads.end() ? it.value().name : QString();
                if (it != m_pendingUploads.end())
                    m_pendingUploads.erase(it);

                qWarning().noquote() << QStringLiteral("上传被拒：%1（%2）- %3")
                                            .arg(name).arg(code).arg(message);
                // 403 是"对方不收了"：直接把原因摆到用户面前，而不是让他反复点
                setTransferNotice(name.isEmpty() ? message
                                                 : tr("%1 未能上传：%2").arg(name, message));
            });

    connect(m_client, &ControlClient::uploadDone, this,
            [this](const QString &transferId, bool ok, const QString &sha256,
                   const QString &message, bool checksumMismatch) {
                const QString taskId = m_uploadTransferIds.take(transferId);
                Qdl::TransferTask *task = taskId.isEmpty() ? nullptr : m_transfers->find(taskId);
                if (!task)
                    return;

                if (!ok) {
                    if (checksumMismatch) {
                        // 主机边收边算的摘要跟本端对不上：它已经把副本删了。
                        // 这跟"被中断"性质不同——重试同样的内容只会再错一次，直接判失败。
                        const QString why = message.isEmpty()
                                                ? tr("校验失败：主机收到的内容与本地不一致")
                                                : message;
                        m_uploadLocalHash.remove(taskId);
                        m_uploadHashDone.remove(taskId);
                        task->confirmVerified(false, why);
                        setTransferNotice(tr("%1 上传校验失败：%2").arg(task->name(), why));
                        return;
                    }
                    // 主机中途断了（典型场景：它把写权限拨到关）：转「已暂停」，可重试
                    task->markUploadInterrupted(message.isEmpty() ? tr("主机中断了上传") : message);
                    setTransferNotice(tr("%1 未传完：%2")
                                          .arg(task->name(),
                                               message.isEmpty() ? tr("主机中断了上传") : message));
                    return;
                }

                m_uploadServerHash.insert(taskId, sha256);
                if (!message.isEmpty())
                    m_uploadServerNote.insert(taskId, message);
                tryFinishUpload(taskId);
            });

    connect(m_client, &ControlClient::pairCodePrompt, this, [this]() { emit pairCodePrompt(); });
    connect(m_client, &ControlClient::pairFailed, this, [this](const QString &message) {
        emit connChanged();
        emit uploadHintChanged();
        emit pairFailed(message);
    });
    connect(m_client, &ControlClient::paired, this, [this]() {
        emit connChanged();
        emit uploadHintChanged();
        emit pairSucceeded();
        refreshTrusted();
    });
    connect(m_client, &ControlClient::authRejected, this, [this](const QString &message) {
        emit connChanged();
        emit uploadHintChanged();
        // 401 是“令牌失效/未配对”，不是配对失败：界面只更新状态条，
        // 由用户决定是否重新配对。这里只把本地失效令牌清掉。
        qInfo().noquote() << QStringLiteral("鉴权被拒：%1").arg(message);
        if (!m_selectedPeerId.isEmpty()) {
            m_trust.removeHost(m_selectedPeerId);
            refreshTrusted();
        }
    });

    // ---------------- 服务端侧状态 ----------------
    connect(m_server, &ControlServer::listeningPortChanged, this, [this](quint16 port) {
        m_discovery->setSelf(buildSelf());
        m_discovery->announceNow();
        emit ctrlPortChanged();
        qInfo() << "控制端口变更，已更新组播心跳：" << port;
    });
    connect(m_server, &ControlServer::clientAuthenticated, this,
            [this](const QString &peerId, const QString &name, const QString &perm) {
                qInfo().noquote() << QStringLiteral("已授权设备接入：%1 (%2) %3").arg(name, peerId, perm);
                // 这个回调在 PeerSession 发 PairOk 的信号栈里，别就地查 SQLite：
                // 推迟一轮，等网络/信号栈退干净再读信任库
                QMetaObject::invokeMethod(this, [this]() { refreshTrusted(); }, Qt::QueuedConnection);
            });
    connect(m_server, &ControlServer::clientDisconnected, this, [this](const QString &) {
        QMetaObject::invokeMethod(this, [this]() { refreshTrusted(); }, Qt::QueuedConnection);
    });

    // ---------------- 共享端：收件箱（M7）----------------
    if (DataServer *data = m_server->dataServer()) {
        connect(data, &DataServer::uploadStarted, this,
                [this](const QString &transferId, const QString &name, const QString &finalPath,
                       qint64 total) {
                    Q_UNUSED(finalPath)
                    Q_UNUSED(total)
                    m_incomingNames.insert(transferId, name);
                    m_incomingPercents.insert(transferId, 0);
                    refreshIncoming();
                    qInfo().noquote() << QStringLiteral("开始接收上传：%1").arg(name);
                });

        connect(data, &DataServer::uploadProgress, this,
                [this](const QString &transferId, const QString &name, qint64 received,
                       qint64 total) {
                    if (!m_incomingNames.contains(transferId))
                        return;
                    m_incomingPercents.insert(transferId,
                                              total > 0 ? int(received * 100 / total) : 100);
                    Q_UNUSED(name)
                    refreshIncoming();
                });

        connect(data, &DataServer::uploadFinished, this,
                [this](const QString &transferId, const QString &finalPath,
                       const QString &name, qint64 received, bool ok, const QString &sha256,
                       const QString &message) {
                    m_incomingNames.remove(transferId);
                    m_incomingPercents.remove(transferId);
                    refreshIncoming();

                    if (ok) {
                        setTransferNotice(tr("已收到 %1（%2）").arg(name,
                                                                   Qdl::TransferFmt::sizeText(received)));
                        emit notify(tr("收到文件"),
                                    tr("%1（%2）已存进收件箱").arg(
                                        name, Qdl::TransferFmt::sizeText(received)));
                        qInfo().noquote() << QStringLiteral("已接收 %1 → %2（sha256=%3）")
                                                 .arg(name, finalPath,
                                                      sha256.isEmpty() ? QStringLiteral("-")
                                                                       : sha256.left(16));
                    } else {
                        setTransferNotice(tr("接收 %1 中断：%2").arg(name, message));
                    }
                });
    }

    connect(m_pairing, &PairingService::pendingChanged, this, [this]() {
        emit pairRequestChanged();
    });
    connect(m_pairing, &PairingService::blocked, this, [this](const QString &peerId, int seconds) {
        Q_UNUSED(peerId)
        qWarning() << "配对尝试过多，已锁定" << seconds << "秒";
    });

    QString reason;
    if (!m_discovery->start(&reason)) {
        m_discoveryOk = false;
        m_discoveryError = reason;
        emit discoveryErrorChanged();
    }

    refreshTrusted();
    localGo(QStringLiteral("/"));

    // 联调用：到点翻转写权限开关，验证已连接设备能否即时收到 PERM_UPDATE
    if (launchOptions().toggleWriteAfterMs >= 0) {
        const int delay = launchOptions().toggleWriteAfterMs;
        QTimer::singleShot(delay, this, [this]() {
            setWriteEnabled(!m_writeEnabled);
            qInfo().noquote() << QStringLiteral("[联调] 写权限已翻转为 %1").arg(m_writeEnabled);
        });
    }

    // 联调用：到点关闭共享，验证已连接设备的列目录请求立即被 403 拦下
    if (launchOptions().toggleShareAfterMs >= 0) {
        const int delay = launchOptions().toggleShareAfterMs;
        QTimer::singleShot(delay, this, [this]() {
            setShareEnabled(!m_shareEnabled);
            qInfo().noquote() << QStringLiteral("[联调] 共享开关已翻转为 %1").arg(m_shareEnabled);
        });
    }

    // 联调用：到点换语言，验证 QQmlEngine::retranslate() 能热切换而不是要求重启
    if (launchOptions().demoLangSwitchMs >= 0) {
        const int delay = launchOptions().demoLangSwitchMs;
        QTimer::singleShot(delay, this, [this]() {
            // 当前不是英文就切英文；是英文就切回中文
            setLanguage(m_language == QLatin1String("en") ? QStringLiteral("zh_CN")
                                                          : QStringLiteral("en"));
        });
    }

    // 联调用：到点换一次目录，并把“选择是否被接受、被拒绝的原因”打进日志
    // （改目录是看不到窗口以外反馈的操作，自动化只能靠日志断言）
    if (!launchOptions().demoDirPath.isEmpty()) {
        QTimer::singleShot(1500, this, [this]() {
            const int role = launchOptions().demoDirRole;
            const QString path = launchOptions().demoDirPath;
            const QString before = role == 0 ? m_shareRoot : m_localRoot;
            const QString issue = directoryIssue(path, role);
            if (issue.isEmpty()) {
                if (role == 0)
                    setShareRoot(path);
                else
                    setLocalRoot(path);
            }
            const QString after = role == 0 ? m_shareRoot : m_localRoot;
            qInfo().noquote() << QStringLiteral("DEMO_DIR role=%1 path=\"%2\" before=\"%3\" after=\"%4\" issue=\"%5\"")
                                     .arg(role == 0 ? QStringLiteral("shareRoot")
                                                    : QStringLiteral("receiveRoot"),
                                          path, before, after, issue);
        });
    }

    // 截图用：到点把「关闭写权限」确认框拉起来（对话框本体在 WriteToggle.qml）
    if (launchOptions().demoConfirmWriteMs >= 0) {
        const int delay = launchOptions().demoConfirmWriteMs;
        QTimer::singleShot(delay, this, [this]() {
            m_demoConfirmWrite = true;
            emit demoConfirmWriteChanged();
        });
    }

    qInfo().noquote() << QStringLiteral("AppController 初始化完成 platform=%1 device=%2 peerId=%3 share=%4 write=%5 ctrlPort=%6")
                             .arg(platformName(), deviceName(), m_peerId)
                             .arg(m_shareEnabled)
                             .arg(m_writeEnabled)
                             .arg(ctrlPort());

    // 目录是被持久化了的状态，日志里没有的话，“怎么重启后又变回去了”根本查不动
    qInfo().noquote() << QStringLiteral("目录：共享=%1 接收=%2 传输并发=%3 每文件连接=%4 限速=%5")
                             .arg(m_shareRoot, m_localRoot)
                             .arg(m_transferThreads)
                             .arg(m_segmentCount)
                             .arg(m_speedLimitMbps > 0
                                      ? QStringLiteral("%1 MB/s").arg(m_speedLimitMbps)
                                      : QStringLiteral("不限"));
}

QSettings *AppController::openSettings()
{
    if (m_settingsFile.isEmpty())
        return new QSettings(this);   // 安装模式：原生位置（Windows 注册表 / macOS plist）

    auto *settings = new QSettings(m_settingsFile, QSettings::IniFormat, this);

    // ini 第一次出现时，把老版本写在注册表里的配置搬过来，
    // 否则用户会以为“配对过的设备全没了”。
    // 副实例（同机第 2 个及以后）跳过：那份老配置里带着主实例的 peerId，
    // 照搬过来等于又把两个进程的身份搅在一起了。
    const bool firstTime = !QFileInfo::exists(m_settingsFile);
    if (firstTime && Qdl::InstanceGuard::index() <= 1) {
        QSettings legacy;
        const QStringList keys = legacy.allKeys();
        for (const QString &key : keys)
            settings->setValue(key, legacy.value(key));
        settings->sync();
        if (!keys.isEmpty())
            qInfo().noquote() << QStringLiteral("已从系统位置迁移 %1 项设置到 %2")
                                     .arg(keys.size())
                                     .arg(m_settingsFile);
    }
    return settings;
}

AppController::~AppController()
{
    // 显式按依赖顺序销毁：m_trust 是成员对象，会比 QObject 子对象先析构，
    // 若此时 m_server/m_client 还活着并回调到已销毁的信任库就会崩。
    if (m_server) {
        m_server->stop();
        delete m_server;
        m_server = nullptr;
    }
    delete m_client;
    m_client = nullptr;
    delete m_pairing;
    m_pairing = nullptr;
    if (m_discovery) {
        m_discovery->stop();
        delete m_discovery;
        m_discovery = nullptr;
    }
    delete m_peers;
    m_peers = nullptr;

    // 断点库没有 QObject 父对象（它是纯数据层），必须手动收
    delete m_resumeStore;
    m_resumeStore = nullptr;
}

// ---------------------------- 基础信息 ----------------------------

QString AppController::appVersion() const
{
    return QStringLiteral(QDOWNLOAD_VERSION);
}

QString AppController::platformName() const
{
    return QSysInfo::prettyProductName();
}

QString AppController::defaultDeviceName() const
{
    QString host = QSysInfo::machineHostName();
    if (host.isEmpty())
        host = tr("未命名设备");

    // 同机多开：默认名后面挂上编号，否则主机列表里两条一模一样的名字，
    // 用户根本分不清哪条是哪个窗口。名称上限 32 字符（要塞进一个 UDP 心跳包），先截再拼。
    const QString suffix = Qdl::InstanceGuard::nameSuffix();
    if (!suffix.isEmpty())
        host = host.left(qMax(1, 32 - suffix.size())) + suffix;
    return host;
}

QString AppController::deviceName() const
{
    // 命令行 --name 是演示/排障用的硬覆盖，界面上改不动它
    if (!launchOptions().nameOverride.isEmpty())
        return launchOptions().nameOverride;

    const QString configured = m_settings->value(QStringLiteral("net/deviceName")).toString();
    return configured.isEmpty() ? defaultDeviceName() : configured;
}

void AppController::setDeviceName(const QString &name)
{
    if (!launchOptions().nameOverride.isEmpty())
        return;

    // 上限 32：主机列表里一行放得下，也不会把广播包撑大（组播包要塞进一个 UDP 报文）
    const QString cleaned = name.trimmed().left(32);
    const QString current = m_settings->value(QStringLiteral("net/deviceName")).toString();

    if (cleaned == current)
        return;

    if (cleaned.isEmpty())
        m_settings->remove(QStringLiteral("net/deviceName"));
    else
        m_settings->setValue(QStringLiteral("net/deviceName"), cleaned);
    m_settings->sync();

    // 三处身份都要跟着换，否则会出现“列表里是新名字、握手时还是旧名字”
    if (m_server)
        m_server->setHostIdentity(m_peerId, deviceName(), platformName());
    if (m_client)
        m_client->setSelf(m_peerId, deviceName(), platformName());
    if (m_discovery) {
        m_discovery->setSelf(buildSelf());
        m_discovery->announceNow();   // 立刻广播，对方不用等下一个心跳
    }

    emit deviceNameChanged();
    emit statusLineChanged();
    qInfo().noquote() << QStringLiteral("本端名称改为：%1").arg(deviceName());
}

QString AppController::logFilePath() const
{
    return Logger::logFilePath();
}

QString AppController::storageRoot() const
{
    return Qdl::DataPaths::recordsRoot();
}

QString AppController::storageSummary() const
{
    return Qdl::DataPaths::summary();
}

QString AppController::statusLine() const
{
    return QStringLiteral("v%1 · %2 · %3").arg(appVersion(), platformName(), deviceName());
}

int AppController::demoTabIndex() const
{
    return launchOptions().demoTabRemote ? 1 : 0;
}

// ---------------------------- 共享开关 ----------------------------

void AppController::setShareEnabled(bool enabled)
{
    if (m_shareEnabled == enabled)
        return;
    m_shareEnabled = enabled;
    persist(QStringLiteral("share/enabled"), enabled);
    applyShareStateToServer();
    emit shareEnabledChanged();
    emit statusLineChanged();
    qInfo() << "共享开关 ->" << enabled;
}

void AppController::setWriteEnabled(bool enabled)
{
    if (m_writeEnabled == enabled)
        return;

    // 拨到关会同步中断进行中的上传（ControlServer 里做），先记下条数用于提示
    const int activeUploads = m_incomingCount;
    m_writeEnabled = enabled;
    persist(QStringLiteral("share/writeEnabled"), enabled);
    applyShareStateToServer();
    emit writeEnabledChanged();
    // 状态栏那句摘要挂在 writeSummary 上，得单独通知，否则只有颜色跟着变
    emit writeSummaryChanged();
    if (!enabled && activeUploads > 0) {
        setTransferNotice(tr("写权限已关闭，%1 个上传已暂停（断点已保留）").arg(activeUploads));
        emit notify(tr("上传已暂停"),
                    tr("写权限已关闭，%1 个上传中断（断点已保留）").arg(activeUploads));
    }
    qInfo() << "写权限开关 ->" << enabled;
}

void AppController::setShareRoot(const QString &path)
{
    if (m_shareRoot == path || path.isEmpty())
        return;

    const QString issue = directoryIssue(path, 0);
    if (!issue.isEmpty()) {
        qWarning().noquote() << "拒绝切换共享根：" << issue;
        return;
    }

    m_shareRoot = path;
    QDir().mkpath(m_shareRoot);
    if (m_server)
        m_server->setShareRoot(m_shareRoot);
    persist(QStringLiteral("share/root"), path);
    emit shareRootChanged();
    qInfo().noquote() << "共享根 ->" << path;

    // 共享中的会话共用同一份 HostContext，后面再有人列目录就是新内容；
    // 已经开着的传输落点早已定好绝对路径，不受影响
    if (m_shareEnabled)
        emit notify(tr("共享目录已切换"), tr("对方下次刷新会看到新的内容"));
}

void AppController::setLocalRoot(const QString &path)
{
    if (m_localRoot == path || path.isEmpty())
        return;

    const QString issue = directoryIssue(path, 1);
    if (!issue.isEmpty()) {
        qWarning().noquote() << "拒绝切换接收目录：" << issue;
        return;
    }

    m_localRoot = path;
    QDir().mkpath(m_localRoot);
    persist(QStringLiteral("receive/root"), path);

    // 目录换了，当前停在的子路径多半不存在了，回根重新列一次
    m_localPath = QStringLiteral("/");
    m_localFiles->reset(QString(), {}, 0);
    m_localBreadcrumb = makeBreadcrumb(m_localPath, tr("接收目录"));
    localGo(QStringLiteral("/"));

    emit localRootChanged();
    qInfo().noquote() << "接收目录 ->" << path;
}

QString AppController::defaultShareRoot() const
{
    return defaultShareRootPath();
}

QString AppController::defaultReceiveRoot() const
{
    return defaultReceiveRootPath();
}

// ---------------------------- 目录选择（含 Android 分区存储适配）----------------------------

void AppController::repairUnusableRoots()
{
    bool created = false;
    QString reason;

    if (!probeWritable(m_shareRoot, &created, &reason)) {
        if (created)
            QDir(m_shareRoot).rmdir(m_shareRoot);
        const QString fallback = defaultShareRootPath();
        qWarning().noquote() << "共享根目录不可用：" << reason
                             << QStringLiteral("→ 改用") << fallback;
        m_shareRoot = fallback;
        QDir().mkpath(m_shareRoot);
        m_startupNotice = tr("原来的共享目录不可写，已改用 %1").arg(m_shareRoot);
    }

    reason.clear();
    if (!probeWritable(m_localRoot, &created, &reason)) {
        if (created)
            QDir(m_localRoot).rmdir(m_localRoot);
        const QString fallback = defaultReceiveRootPath();
        qWarning().noquote() << "接收目录不可用：" << reason
                             << QStringLiteral("→ 改用") << fallback;
        m_localRoot = fallback;
        QDir().mkpath(m_localRoot);
        if (m_startupNotice.isEmpty())
            m_startupNotice = tr("原来的接收目录不可写，已改用 %1").arg(m_localRoot);
    }
}

bool AppController::builtInDirPicker() const
{
#ifdef Q_OS_ANDROID
    // 系统目录选择器在安卓上返回的是 content:// 授权 URI，QFile 一律打不开，
    // 而且它不会过滤掉本应用没权限写的公共目录——选完才发现写不了。
    // 所以安卓改用自带浏览器，只在“确定能写”的范围里让选
    return true;
#else
    return launchOptions().demoDirBrowser;
#endif
}

bool AppController::allFilesAccess() const
{
    return m_allFilesAccess;
}

/// 回到前台时重测一次：用户在系统设置页开关权限，我们是收不到回调的
void AppController::refreshStorageAccess()
{
    const bool has = Qdl::AndroidStorage::hasFullAccess();
    if (has == m_allFilesAccess)
        return;
    m_allFilesAccess = has;
    emit allFilesAccessChanged();
    qInfo() << "外部存储全盘访问 ->" << has;
}

QVariantList AppController::storageRoots() const
{
    QVariantList list;
    QStringList already;

    struct Candidate { QString path; QString label; };
    QList<Candidate> candidates;

#ifdef Q_OS_ANDROID
    for (const Qdl::AndroidStorage::Root &root : Qdl::AndroidStorage::roots())
        candidates.append({root.path, root.label});
#else
    // 桌面默认不走这条路（有系统对话框），这里给的是 --demo-dir-browser 排障/截图用，
    // 以及以后想在桌面上也统一体验时的备选：球权仍由 builtInDirPicker 决定
    candidates.append({QDir::rootPath(), tr("此电脑")});
    candidates.append({QStandardPaths::writableLocation(QStandardPaths::HomeLocation),
                       tr("主目录")});
    candidates.append({QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
                       tr("文档")});
    candidates.append({QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
                       tr("下载")});
#endif

    for (const Candidate &candidate : candidates) {
        if (candidate.path.isEmpty() || already.contains(candidate.path))
            continue;

        // 探测不过的位置不列出来。把一条注定被拒的路径摆给用户选，
        // 只会让人以为程序坏了——这也是之前“改了就报没有权限”的来源
        bool created = false;
        QString reason;
        const bool writable = probeWritable(candidate.path, &created, &reason);
        if (created)
            QDir(candidate.path).rmdir(candidate.path);

        const bool readable = QDir(candidate.path).exists()
                              && QDir(candidate.path).isReadable();

#ifdef Q_OS_ANDROID
        // 安卓上列不出的目录大多也读不了，没通过试写就别摆出来
        const bool usable = writable;
#else
        // 桌面反过来：盘符根、系统目录常常写不了但能读，
        // 作为“只能往下走、不能选”的浏览起点仍然有用（顶层本身就不让选）
        const bool usable = writable || readable;
#endif
        if (!usable) {
            qInfo().noquote() << "候选根目录不可用，不列出：" << candidate.path << reason;
            continue;
        }

        already << candidate.path;
        list << QVariantMap{{QStringLiteral("path"), candidate.path},
                            {QStringLiteral("label"), candidate.label},
                            {QStringLiteral("writable"), writable}};
    }
    return list;
}

QStringList AppController::subDirectories(const QString &path) const
{
    QStringList names;
    const QDir dir(path);
    if (!dir.exists())
        return names;

    const auto entries = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable,
                                           QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &info : entries) {
        if (info.fileName().startsWith(QLatin1Char('.')))
            continue;           // sdcard 上全是各 app 的隐藏目录，列出来是噪音
        if (info.isSymLink())
            continue;           // 软链能把浏览路线绕出去，只认真实子目录
        names << info.fileName();
    }
    return names;
}

QString AppController::createSubDirectory(const QString &parent, const QString &name)
{
    const QString clean = name.trimmed();
    const QDir base(parent);
    if (clean.isEmpty() || !base.exists())
        return {};

    const QString target = QDir::cleanPath(base.filePath(clean));
    if (QFileInfo::exists(target))
        return {};

    if (!base.mkdir(clean)) {
        qWarning().noquote() << "建子目录失败：" << target;
        return {};
    }
    return target;
}

void AppController::requestStorageAccess()
{
    Qdl::AndroidStorage::requestFullAccess();
    // 用户从系统设置页回来后才真的可用，稍后补一次探测把成功的情况接住
    QTimer::singleShot(800, this, [this]() { refreshStorageAccess(); });
}

QString AppController::storageHint() const
{
#ifdef Q_OS_ANDROID
    if (!Qdl::AndroidStorage::hasFullAccess()) {
        return tr("安卓禁止直接读写公共目录，现在只能选本应用专属目录；"
                  "想指定任意位置请先授予「所有文件访问权限」");
    }
#endif
    return {};
}

QString AppController::childPath(const QString &parent, const QString &name) const
{
    if (parent.isEmpty() || name.isEmpty())
        return QString();
    return QDir::cleanPath(QDir(parent).filePath(name));
}

QString AppController::parentPath(const QString &path) const
{
    if (path.isEmpty())
        return QString();
    return QDir::cleanPath(QDir(path).filePath(QStringLiteral("..")));
}

QString AppController::startupNotice() const
{
    return m_startupNotice;
}

QString AppController::urlToPath(const QVariant &url) const
{
    QUrl u = url.canConvert<QUrl>() ? url.toUrl() : QUrl(url.toString());
    return u.isLocalFile() ? u.toLocalFile() : QString();
}

QUrl AppController::pathToUrl(const QString &path) const
{
    return path.isEmpty() ? QUrl() : QUrl::fromLocalFile(path);
}

QString AppController::directoryIssue(const QString &path, int role) const
{
    if (path.trimmed().isEmpty())
        return tr("路径为空");

    const QFileInfo info(path);
    if (info.exists() && !info.isDir())
        return tr("这不是一个目录");

    // 盘符根 / 文件系统根：共享它等于把整块盘交出去，当接收目录则会把整个盘的活文件都算进来
    if (info.exists() && QDir(path).isRoot())
        return tr("不能用磁盘根目录，请选一个子目录");

    if (info.exists()) {
        if (!info.isReadable())
            return tr("这个目录读不了");
    }

    bool justCreated = false;
    QString writeError;
    if (!probeWritable(path, &justCreated, &writeError)) {
        // 试探失败别留下半成品目录
        if (justCreated)
            QDir(path).rmdir(path);
        return writeError;
    }

    // 两个目录必须互不嵌套。混在一起会造成“下载完立刻被共享出去”，
    // 而且列举目录时两边互相看见对方正在写的半成品
    const QString other = role == 0 ? m_localRoot : m_shareRoot;
    const QString otherLabel = role == 0 ? tr("接收目录") : tr("共享目录");
    if (!other.isEmpty()) {
        const QString self = comparableDir(path);
        const QString peer = comparableDir(other);

        // 当前这一对已经互相嵌套时（比如老版本的设置），不许改动会让用户彻底卡死，
        // 于是只在“原本没冲突、正要制造冲突”时才拦
        const QString current = comparableDir(role == 0 ? m_shareRoot : m_localRoot);
        const bool alreadyNested = !current.isEmpty()
                                   && (current == peer || peer.startsWith(current)
                                       || current.startsWith(peer));
        if (!alreadyNested) {
            if (self == peer)
                return tr("不能和%1用同一个目录").arg(otherLabel);
            if (peer.startsWith(self))
                return tr("不能是%1的上级目录").arg(otherLabel);
            if (self.startsWith(peer))
                return tr("不能是%1的子目录").arg(otherLabel);
        }
    }

    return QString();
}

void AppController::setUploadPolicy(int policy)
{
    if (policy < 0 || policy > 1 || m_uploadPolicy == policy)
        return;
    m_uploadPolicy = policy;
    persist(QStringLiteral("share/uploadPolicy"), policy);
    applyShareStateToServer();
    emit uploadPolicyChanged();
    emit writeSummaryChanged();   // 摘要里包含策略名（白名单 / 全部已配对）
    qInfo() << "上传策略 ->" << uploadPolicyText();
}

QString AppController::uploadPolicyText() const
{
    return m_uploadPolicy == 0 ? tr("仅白名单") : tr("已配对皆可传");
}

// ---------------------------- 托盘与通知（M8）----------------------------

void AppController::setCloseToTray(bool on)
{
    if (m_closeToTray == on)
        return;
    m_closeToTray = on;
    persist(QStringLiteral("ui/closeToTray"), on);
    emit closeToTrayChanged();
    qInfo().noquote() << "关闭窗口行为 ->" << (on ? "收进托盘" : "退出程序");
}

void AppController::setTrayAvailable(bool available)
{
    if (m_trayAvailable == available)
        return;
    m_trayAvailable = available;
    emit trayAvailableChanged();
    qInfo().noquote() << (available ? QStringLiteral("系统托盘可用")
                                    : QStringLiteral("系统托盘不可用：关闭窗口将直接退出"));
}

void AppController::notifySystem(const QString &title, const QString &body)
{
    // 桌面端 QML 会先试托盘气泡，走不到这里。真正需要系统通道的是 Android
    // （没有托盘概念，只能挂通知栏），实现见 src/app/SystemNotifier.cpp。
    if (Qdl::SystemNotifier::notify(title, body))
        return;
    qInfo().noquote() << QStringLiteral("通知：%1 · %2").arg(title, body);
}

void AppController::setLanguage(const QString &code)
{
    if (m_language == code)
        return;

    m_language = code;
    persist(QStringLiteral("ui/language"), code);

    if (!Qdl::Translation::apply(code))
        setTransferNotice(tr("翻译包加载失败，界面保持中文"));

    // QML 里所有 qsTr 绑定重算；挂在绑定上的 C++ getter 也会被顺带重新调用
    if (QQmlEngine *engine = qmlEngine(this))
        engine->retranslate();

    // 面包屑的根节点是构造时算好的 tr() 字符串，不在绑定里，retranslate 管不到
    m_localBreadcrumb = makeBreadcrumb(m_localPath, tr("接收目录"));
    m_remoteBreadcrumb = makeBreadcrumb(m_remotePath, tr("共享根"));

    // 少数文本不在绑定里（由信号驱动刷新），补一轮信号
    emit languageChanged();
    emit statusLineChanged();
    emit uploadHintChanged();
    emit peerCountChanged();
    emit connChanged();
    emit transfersChanged();
    emit incomingChanged();
    emit uploadPolicyChanged();
    emit writeSummaryChanged();
    emit localPathChanged();
    emit remotePathChanged();

    qInfo().noquote() << "界面语言 ->" << (code.isEmpty() ? QStringLiteral("system") : code);
}

QString AppController::writeSummary() const
{
    if (!m_writeEnabled)
        return tr("关");
    return m_uploadPolicy == 0 ? tr("白名单") : tr("全部已配对");
}

void AppController::applyShareStateToServer()
{
    // 组播给未连接的设备看，PERM_UPDATE 给已连接的设备看，两条通道都要走
    m_discovery->setStatus(m_shareEnabled, m_writeEnabled, m_uploadPolicy);
    m_server->setShareState(m_shareEnabled, m_writeEnabled, m_uploadPolicy);
    m_discovery->setSelf(buildSelf());
}

// ---------------------------- 发现 ----------------------------

QAbstractItemModel *AppController::peers() const
{
    return m_peers;
}

int AppController::peerCount() const
{
    return m_peerCount;
}

int AppController::onlinePeerCount() const
{
    return m_onlinePeerCount;
}

QString AppController::peerSummary() const
{
    if (m_peerCount == 0)
        return tr("未发现主机");
    if (m_onlinePeerCount == 0)
        return tr("%1 台主机全部离线").arg(m_peerCount);
    return tr("发现 %1 台（%2 在线）").arg(m_peerCount).arg(m_onlinePeerCount);
}

QString AppController::selfPeerId() const
{
    return m_peerId;
}

void AppController::setSelectedPeerId(const QString &peerId)
{
    if (m_selectedPeerId == peerId)
        return;
    m_selectedPeerId = peerId;

    // 换主机 = 新的明确意图，解除上一次「手动断开」的挂起
    m_manualDisconnect = false;

    // 换主机：远端列表必须清空。不同主机的 "/" 是不同目录，
    // 沿用上一个主机的路径既可能不存在，也会把别人的目录结构漏给用户看
    m_remoteFiles->clear();
    m_remotePath.clear();
    updateRemoteBreadcrumb();
    setRemoteError(QString());
    setRemoteLoading(false);
    emit remotePathChanged();
    emit remoteListChanged();

    if (!peerId.isEmpty()) {
        const int row = m_peers->indexOf(peerId);
        if (row >= 0) {
            const PeerInfo peer = m_peers->peerAt(row);
            if (peer.online && peer.sharing)
                m_client->connectToHost(peer);
            else
                m_client->disconnectFromHost();
        }
    } else {
        m_client->disconnectFromHost();
    }

    emit selectedPeerChanged();
}

QString AppController::selectedPeerName() const
{
    const int row = m_peers->indexOf(m_selectedPeerId);
    if (row < 0)
        return m_selectedPeerId.isEmpty() ? tr("未选择主机") : tr("主机已离线");
    return m_peers->peerAt(row).name;
}

QString AppController::selectedPeerBadge() const
{
    const int row = m_peers->indexOf(m_selectedPeerId);
    if (row < 0)
        return QString();
    return m_peers->peerAt(row).statusBadge();
}

QString AppController::selectedPeerKind() const
{
    const int row = m_peers->indexOf(m_selectedPeerId);
    if (row < 0)
        return QString();
    return m_peers->peerAt(row).statusKind();
}

bool AppController::selectedPeerOnline() const
{
    const int row = m_peers->indexOf(m_selectedPeerId);
    return row >= 0 && m_peers->peerAt(row).online;
}

void AppController::announceNow()
{
    if (m_discovery)
        m_discovery->announceNow();
    maybeAutoConnect();
}

void AppController::maybeAutoConnect()
{
    // 用户自己点的断开必须尊重：心跳每 3 秒来一次，不挂起的话点完断开 3 秒就又连上了
    if (m_manualDisconnect)
        return;

    if (m_selectedPeerId.isEmpty())
        return;

    const int row = m_peers->indexOf(m_selectedPeerId);
    if (row < 0)
        return;

    const PeerInfo peer = m_peers->peerAt(row);
    if (!peer.online || !peer.sharing)
        return;

    if (m_client->isConnected())
        return;

    // 失败后 8 秒内不重试：心跳每 3 秒来一次，否则会疯狂重连刷日志
    if (m_client->state() == ControlClient::State::Failed) {
        if (m_lastConnectAttempt.isValid() && m_lastConnectAttempt.elapsed() < 8000)
            return;
    }

    m_lastConnectAttempt.start();
    m_client->connectToHost(peer);
}

void AppController::refreshCounts()
{
    const int total = m_peers->count();
    int online = 0;
    for (int row = 0; row < total; ++row) {
        if (m_peers->peerAt(row).online)
            ++online;
    }

    if (total == m_peerCount && online == m_onlinePeerCount)
        return;

    m_peerCount = total;
    m_onlinePeerCount = online;
    emit peerCountChanged();
}

// ---------------------------- 控制通道 / 配对 ----------------------------

int AppController::ctrlPort() const
{
    return m_server ? int(m_server->port()) : 0;
}

QString AppController::trustStorePath() const
{
    return m_trust.path();
}

QString AppController::connStateCode() const
{
    if (!m_client)
        return QStringLiteral("idle");

    switch (m_client->state()) {
    case ControlClient::State::Idle:             return QStringLiteral("idle");
    case ControlClient::State::Connecting:       return QStringLiteral("connecting");
    case ControlClient::State::Handshaking:      return QStringLiteral("handshaking");
    case ControlClient::State::Authenticating:   return QStringLiteral("authenticating");
    case ControlClient::State::Unpaired:         return QStringLiteral("unpaired");
    case ControlClient::State::Pairing:          return QStringLiteral("pairing");
    case ControlClient::State::AwaitingPairCode: return QStringLiteral("awaitingCode");
    case ControlClient::State::Ready:            return QStringLiteral("ready");
    case ControlClient::State::Failed:           return QStringLiteral("failed");
    }
    return QStringLiteral("idle");
}

QString AppController::connStateText() const
{
    return m_client ? m_client->stateText() : tr("未连接");
}

QString AppController::connError() const
{
    return m_client ? m_client->errorText() : QString();
}

QString AppController::peerPerm() const
{
    return m_client ? m_client->perm() : QString();
}

bool AppController::peerCanUpload() const
{
    return m_client && m_client->canUpload();
}

bool AppController::peerShares() const
{
    return m_client ? m_client->peerSharing() : true;
}

bool AppController::peerConnected() const
{
    return m_client && m_client->isReady();
}

QVariantMap AppController::pairRequest() const
{
    return m_pairing ? m_pairing->pendingInfo() : QVariantMap();
}

QVariantList AppController::trustedDevices() const
{
    return m_trustedDevices;
}

void AppController::connectToSelectedPeer()
{
    if (m_selectedPeerId.isEmpty())
        return;
    const int row = m_peers->indexOf(m_selectedPeerId);
    if (row < 0)
        return;

    // 明确要连，解除“手动断开”的挂起
    m_manualDisconnect = false;

    const PeerInfo peer = m_peers->peerAt(row);
    m_lastConnectAttempt.start();
    m_client->connectToHost(peer);
    emit connChanged();
}

void AppController::disconnectPeer()
{
    // 先挂起自动重连再断：否则 disconnected 会把状态推回 Idle，
    // 下一次心跳（3 秒内）就把连接又拉起来了
    m_manualDisconnect = true;
    m_client->disconnectFromHost();
    emit connChanged();
    emit uploadHintChanged();
    qInfo() << "已手动断开，自动重连暂停（点「重连」或换主机后恢复）";
}

void AppController::requestPairing()
{
    m_client->requestPairing();
}

void AppController::submitPairCode(const QString &code)
{
    m_client->submitPairCode(code);
}

void AppController::approvePendingPair(bool allowWrite)
{
    const QString peerId = m_pairing->firstPendingPeerId();
    if (peerId.isEmpty() || m_pairReplyPending)
        return;

    /**
     * **不能在这个栈里直接跑完配对**。
     *
     * 这个函数是从 QML 按钮的 clicked 里调进来的，此时鼠标的 press/release
     * 还没结束。把配对跑完 = 写信任库 + 发 PairOk + `pairRequestChanged`
     * 让弹窗隐藏，于是 Qt 回过头继续处理那次点击时，按钮和弹窗的状态已经
     * 不存在了。MinGW 构建侥幸能活，MSVC Release 下点「允许配对」**必崩**
     * （Windows 事件日志：Qt6Core.dll + 0xc0000005、ntdll 0xc0000374 堆破坏）。
     *
     * 推到事件循环下一轮，让这次点击先干干净净处理完。
     */
    m_pairReplyPending = true;
    QMetaObject::invokeMethod(
        this,
        [this, peerId, allowWrite]() {
            m_pairReplyPending = false;
            m_pairing->approve(peerId, allowWrite);
            emit pairRequestChanged();
            refreshTrusted();
        },
        Qt::QueuedConnection);
}

void AppController::rejectPendingPair()
{
    const QString peerId = m_pairing->firstPendingPeerId();
    if (peerId.isEmpty() || m_pairReplyPending)
        return;

    // 同上：拒绝同样会让弹窗消失，必须离开按钮的事件处理栈
    m_pairReplyPending = true;
    QMetaObject::invokeMethod(
        this,
        [this, peerId]() {
            m_pairReplyPending = false;
            m_pairing->reject(peerId);
            emit pairRequestChanged();
        },
        Qt::QueuedConnection);
}

void AppController::setTrustedPerm(const QString &peerId, const QString &perm)
{
    if (peerId.isEmpty() || (perm != QLatin1String("ro") && perm != QLatin1String("rw")))
        return;

    if (!m_trust.setClientPerm(peerId, perm)) {
        qWarning() << "修改设备权限失败" << peerId;
        return;
    }

    qInfo().noquote() << QStringLiteral("设备权限改为 %1：%2").arg(perm, peerId);
    m_server->broadcastPermUpdate();   // 该设备界面立即联动，无需重连
    refreshTrusted();
}

void AppController::revokeTrusted(const QString &peerId)
{
    if (peerId.isEmpty())
        return;

    m_trust.removeClient(peerId);
    m_server->disconnectPeer(peerId);
    qInfo().noquote() << QStringLiteral("已撤销授权：%1").arg(peerId);
    refreshTrusted();
}

void AppController::refreshTrusted()
{
    /**
     * 这个函数会被好几条路径调到，其中配对完成那条是在**四层深的信号栈里**
     * （pendingChanged → approved → authenticated → clientAuthenticated）。
     * 在那种栈里碰 SQLite，MSVC Release 下点「允许配对」会崩（Qt6Core
     * QByteArray 构造 + 堆破坏 0xc0000374）。
     *
     * 统一推到事件循环顶层执行，顺带把连续多次调用合并成一次查询。
     */
    if (m_trustedRefreshQueued)
        return;
    m_trustedRefreshQueued = true;
    QMetaObject::invokeMethod(this, [this]() {
        m_trustedRefreshQueued = false;
        rebuildTrustedDeviceList();
    }, Qt::QueuedConnection);
}

void AppController::rebuildTrustedDeviceList()
{
    QVariantList list;
    const QStringList online = m_server ? m_server->connectedPeers() : QStringList();

    const QVector<TrustStore::Record> records = m_trust.clients();
    for (const TrustStore::Record &record : records) {
        QVariantMap item;
        item.insert(QStringLiteral("peerId"), record.peerId);
        item.insert(QStringLiteral("name"), record.name.isEmpty() ? record.peerId : record.name);
        item.insert(QStringLiteral("perm"), record.perm);
        item.insert(QStringLiteral("online"), online.contains(record.peerId));
        item.insert(QStringLiteral("lastSeen"), record.lastSeen);
        list.append(item);
    }

    m_trustedDevices = list;
    emit trustedDevicesChanged();
}

PeerInfo AppController::buildSelf() const
{
    PeerInfo self;
    self.peerId = m_peerId;
    self.name = deviceName();
    self.platform = platformName();
    self.version = appVersion();
    self.ctrlPort = m_server && m_server->port() > 0 ? m_server->port() : launchOptions().ctrlPort;
    self.sharing = m_shareEnabled;
    self.write = m_writeEnabled;
    self.uploadPolicy = m_uploadPolicy;
    self.online = true;
    self.lastSeen = QDateTime::currentDateTimeUtc();
    return self;
}

// ---------------------------- 目录列表（M4）----------------------------

QAbstractItemModel *AppController::localFiles() const
{
    return m_localFiles;
}

QAbstractItemModel *AppController::remoteFiles() const
{
    return m_remoteFiles;
}

bool AppController::remoteCanMore() const
{
    return m_remoteFiles && m_remoteFiles->hasMore();
}

int AppController::remoteTotal() const
{
    return m_remoteFiles ? m_remoteFiles->total() : 0;
}

QVariantList AppController::makeBreadcrumb(const QString &path, const QString &rootLabel) const
{
    QVariantList list;

    QVariantMap root;
    root.insert(QStringLiteral("name"), rootLabel);
    root.insert(QStringLiteral("path"), QStringLiteral("/"));
    list.append(root);

    QString accumulated;
    const QStringList parts = Qdl::SafePath::segments(path);
    for (const QString &part : parts) {
        accumulated += QLatin1Char('/') + part;
        QVariantMap item;
        item.insert(QStringLiteral("name"), part);
        item.insert(QStringLiteral("path"), accumulated);
        list.append(item);
    }
    return list;
}

void AppController::updateRemoteBreadcrumb()
{
    m_remoteBreadcrumb = makeBreadcrumb(m_remotePath, tr("共享根"));
}

void AppController::setRemoteLoading(bool loading)
{
    if (m_remoteLoading == loading)
        return;
    m_remoteLoading = loading;
    emit remoteLoadingChanged();
}

void AppController::setRemoteError(const QString &message)
{
    if (m_remoteError == message)
        return;
    m_remoteError = message;
    emit remoteErrorChanged();
}

void AppController::localGo(const QString &path)
{
    QString error;
    const QString absolute = Qdl::SafePath::resolve(m_localRoot, path, &error);
    if (absolute.isEmpty()) {
        m_localNotice = error;
        emit localPathChanged();
        return;
    }

    QVector<Qdl::FileEntry> entries;
    int total = 0;
    if (!Qdl::FileScanner::list(absolute, 0, kLocalListLimit, &entries, &total, &error)) {
        m_localNotice = error;
        emit localPathChanged();
        return;
    }

    const QString canonical = Qdl::SafePath::toVirtual(m_localRoot, absolute);
    m_localPath = canonical.isEmpty() ? QStringLiteral("/") : canonical;
    m_localFiles->reset(m_localPath, entries, total);
    m_localBreadcrumb = makeBreadcrumb(m_localPath, tr("接收目录"));

    // 本地不做分页，超出上限只提示，不假装只有这些
    m_localNotice = total > entries.size()
                        ? tr("目录共 %1 项，仅显示前 %2 项").arg(total).arg(entries.size())
                        : QString();
    emit localPathChanged();
}

void AppController::localEnter(const QString &name)
{
    const QString child = Qdl::SafePath::child(m_localPath.isEmpty() ? QStringLiteral("/") : m_localPath,
                                               name);
    if (child.isEmpty())
        return;
    localGo(child);
}

void AppController::localUp()
{
    localGo(Qdl::SafePath::parent(m_localPath.isEmpty() ? QStringLiteral("/") : m_localPath));
}

void AppController::localRefresh()
{
    localGo(m_localPath.isEmpty() ? QStringLiteral("/") : m_localPath);
}

void AppController::remoteGo(const QString &path)
{
    if (!m_client) {
        setRemoteError(tr("尚未连接到主机"));
        return;
    }
    if (!m_client->isReady()) {
        setRemoteError(tr("尚未连接到主机"));
        return;
    }

    // 先清空再请求：否则用户会对着上一个目录的内容等待新目录，误以为已经切过去了
    m_remoteFiles->clear();
    setRemoteError(QString());
    setRemoteLoading(true);
    emit remoteListChanged();

    if (m_client->requestList(path, 0, kRemotePageSize).isEmpty())
        setRemoteLoading(false);
}

void AppController::remoteEnter(const QString &name)
{
    const QString child = Qdl::SafePath::child(
        m_remotePath.isEmpty() ? QStringLiteral("/") : m_remotePath, name);
    if (child.isEmpty())
        return;
    remoteGo(child);
}

void AppController::remoteUp()
{
    remoteGo(Qdl::SafePath::parent(m_remotePath.isEmpty() ? QStringLiteral("/") : m_remotePath));
}

void AppController::remoteRefresh()
{
    remoteGo(m_remotePath.isEmpty() ? QStringLiteral("/") : m_remotePath);
}

void AppController::remoteLoadMore()
{
    if (!m_client || !m_client->isReady() || !m_remoteFiles->hasMore() || m_remoteLoading)
        return;

    setRemoteLoading(true);
    if (m_client->requestList(m_remotePath, m_remoteFiles->rowCount(), kRemotePageSize).isEmpty())
        setRemoteLoading(false);
}

// ---------------------------- 删除（M14）----------------------------

void AppController::deleteLocalFile(const QString &name)
{
    const QString virtualPath = Qdl::SafePath::child(
        m_localPath.isEmpty() ? QStringLiteral("/") : m_localPath, name);
    if (virtualPath.isEmpty()) {
        setTransferNotice(tr("文件名不合法"));
        return;
    }
    deleteLocalPath(virtualPath);
}

bool AppController::deleteLocalPath(const QString &virtualPath)
{
    const QString name = QFileInfo(virtualPath).fileName();

    QString error;
    const QString absolute = Qdl::SafePath::resolve(m_localRoot, virtualPath, &error);
    if (absolute.isEmpty()) {
        m_localNotice = error;
        emit localPathChanged();
        return false;
    }

    if (QFileInfo(absolute).isDir()) {
        setTransferNotice(tr("文件夹请进入后逐个删除"));
        return false;
    }

    // 正在收的文件不能删：删掉之后任务收尾会对着一个不存在（或被顶替）的文件改名校验
    for (QObject *child : m_transfers->children()) {
        auto *task = qobject_cast<Qdl::TransferTask *>(child);
        if (!task || !task->isActive() || task->localPath() != absolute)
            continue;
        setTransferNotice(tr("「%1」正在传输，先取消再删除").arg(task->name()));
        return false;
    }

    if (!QFile::remove(absolute)) {
        setTransferNotice(tr("删除失败：%1（可能正被其他程序占用）").arg(name));
        return false;
    }

    qInfo().noquote() << QStringLiteral("已删除本机文件 %1").arg(absolute);
    setTransferNotice(tr("已删除「%1」").arg(name));
    localRefresh();
    return true;
}

void AppController::deleteRemoteFile(const QString &name)
{
    if (!m_client || !m_client->isReady()) {
        setTransferNotice(tr("尚未连接到主机，无法删除"));
        return;
    }
    if (!m_client->peerSharing()) {
        setTransferNotice(tr("对方已关闭共享"));
        return;
    }
    // 删除会改对方的共享目录，按"写操作"处理：没写权限连请求都不发
    if (!m_client->canUpload()) {
        setTransferNotice(m_client->perm() == QLatin1String("rw")
                              ? tr("对方已关闭写权限，无法删除")
                              : tr("未获写权限（只读设备不能删除）"));
        return;
    }

    const QString path = Qdl::SafePath::child(
        m_remotePath.isEmpty() ? QStringLiteral("/") : m_remotePath, name);
    if (path.isEmpty()) {
        setTransferNotice(tr("文件名不合法"));
        return;
    }

    if (m_client->requestDelete(path).isEmpty()) {
        setTransferNotice(tr("无法发起删除：连接状态已变化"));
        return;
    }
}

// ---------------------------- 下载（M5）----------------------------

QAbstractItemModel *AppController::transfers() const
{
    return m_transfers ? m_transfers->model() : nullptr;
}

int AppController::transferActiveCount() const
{
    return m_transfers ? m_transfers->model()->activeCount() : 0;
}

int AppController::transferCount() const
{
    return m_transfers ? m_transfers->model()->rowCount() : 0;
}

int AppController::transferFinishedCount() const
{
    return m_transfers ? m_transfers->model()->finishedCount() : 0;
}

QString AppController::transferSummary() const
{
    return m_transfers ? m_transfers->model()->summary() : QString();
}

bool AppController::demoTransfers() const
{
    return launchOptions().demoTransfers;
}

bool AppController::demoDrop() const
{
    return launchOptions().demoDrop;
}

bool AppController::demoSettings() const
{
    return launchOptions().demoSettings;
}

bool AppController::demoDirBrowser() const
{
    return launchOptions().demoDirBrowser;
}

void AppController::downloadFile(const QString &name)
{
    if (!m_client || !m_client->isReady()) {
        setTransferNotice(tr("尚未连接到主机，无法下载"));
        return;
    }
    if (!m_client->peerSharing()) {
        setTransferNotice(tr("对方已关闭共享"));
        return;
    }

    const QString path = Qdl::SafePath::child(
        m_remotePath.isEmpty() ? QStringLiteral("/") : m_remotePath, name);
    if (path.isEmpty()) {
        setTransferNotice(tr("文件名不合法"));
        return;
    }
    downloadPath(path, 0);
}

void AppController::downloadPath(const QString &virtualPath, qint64 offset)
{
    requestDownloadFor(virtualPath, offset, QString());
}

int AppController::activeTransferCount() const
{
    if (!m_transfers)
        return 0;

    int count = 0;
    for (QObject *child : m_transfers->children()) {
        auto *task = qobject_cast<Qdl::TransferTask *>(child);
        if (task && task->isActive())
            ++count;
    }
    // 请求已发出、还没收到 ACCEPT 的那段也算占着位置：
    // 只数任务的话，连点几下就能把上限穿透出去
    count += m_pendingDownloads.size() + m_pendingUploads.size();
    return count;
}

bool AppController::enqueueDownload(const QString &virtualPath, qint64 offset,
                                    const QString &taskId)
{
    for (const QueuedDownload &item : m_queuedDownloads) {
        if (item.path == virtualPath)
            return false;   // 同一个文件已经在排队：再点一次不该堆出第二条
    }

    m_queuedDownloads.append({virtualPath, offset, taskId});
    setTransferNotice(tr("%1 已排队：同时进行最多 %2 个，等前面的传完自动接上")
                          .arg(virtualPath, QString::number(m_transferThreads)));
    return true;
}

void AppController::drainTransferQueues()
{
    if (!m_client || !m_client->isReady())
        return;   // 没连上主机的排队留着，连上那一瞬间会再来一次
    if (m_queuedDownloads.isEmpty() && m_queuedUploads.isEmpty())
        return;

    const int before = queuedTransferCount();

    while (!m_queuedDownloads.isEmpty() && activeTransferCount() < m_transferThreads) {
        const QueuedDownload item = m_queuedDownloads.takeFirst();
        issueDownloadRequest(item.path, item.offset, item.taskId);
    }

    while (!m_queuedUploads.isEmpty() && activeTransferCount() < m_transferThreads) {
        const QString path = m_queuedUploads.takeFirst();
        uploadPaths(QStringList{ path });   // 里面还会再确认一次可上传性与空位
    }

    if (queuedTransferCount() != before)
        emit queuedTransfersChanged();
}

void AppController::requestDownloadFor(const QString &virtualPath, qint64 offset,
                                       const QString &taskId)
{
    if (!m_client || !m_client->isReady()) {
        setTransferNotice(tr("尚未连接到主机，无法下载"));
        return;
    }

    // 并发已满就先排队。授权包里带一次性 token 和数据端口，
    // 拿在手上迟迟不连容易撞上主机那边的清理，不如等有位置时再申请
    if (activeTransferCount() >= m_transferThreads) {
        if (enqueueDownload(virtualPath, offset, taskId))
            emit queuedTransfersChanged();
        return;
    }

    issueDownloadRequest(virtualPath, offset, taskId);
}

void AppController::issueDownloadRequest(const QString &virtualPath, qint64 offset,
                                         const QString &taskId)
{
    // 续传必须带上原来的 fileKey：主机据此判断"还是不是同一个文件"，
    // 对不上就回 409，比闷头接上去拼出一个坏文件强
    QString fileKey;
    QVector<qint64> segmentDone;
    int segments = m_segmentCount;
    qint64 chunkSize = 0;

    if (!taskId.isEmpty()) {
        if (Qdl::TransferTask *task = m_transfers->find(taskId)) {
            fileKey = task->fileKey();
            // 续传要沿用上次的切法：盘上的分片是按那个段长写的，换了切法
            // 分片就对不上任何一段，只能整份重来（代价太大）
            segmentDone = task->segmentProgress();
            chunkSize = task->savedChunkSize();
        }
    }

    const QString reqId = m_client->requestDownload(virtualPath, offset, fileKey, segments,
                                                    segmentDone, chunkSize);
    if (reqId.isEmpty()) {
        setTransferNotice(tr("主机忙或未连接，稍后再试"));
        return;
    }

    qInfo().noquote() << QStringLiteral("申请下载 %1（offset=%2，连接数=%3%4）")
                             .arg(virtualPath)
                             .arg(offset)
                             .arg(segments)
                             .arg(taskId.isEmpty() ? QString()
                                                   : QStringLiteral("，续传任务 ") + taskId);

    PendingDownload pending;
    pending.path = virtualPath;
    pending.offset = offset;
    pending.taskId = taskId;
    m_pendingDownloads.insert(reqId, pending);
}

void AppController::setTransferThreads(int count)
{
    const int clamped = qBound(kTransferThreadsMin, count, kTransferThreadsMax);
    if (clamped == m_transferThreads)
        return;

    m_transferThreads = clamped;
    persist(QStringLiteral("transfer/threads"), clamped);
    emit transferThreadsChanged();
    // 调大了立刻把排队的放出来；调小了只约束新任务，已经在跑的不打断
    drainTransferQueues();
    qInfo().noquote() << QStringLiteral("传输并发数 ->") << clamped;
}

void AppController::setSegmentCount(int count)
{
    const int clamped = qBound(kSegmentMin, count, kSegmentMax);
    if (clamped == m_segmentCount)
        return;

    m_segmentCount = clamped;
    persist(QStringLiteral("transfer/segments"), clamped);
    emit segmentCountChanged();

    // 立刻作用于正在跑的任务（这正是"线程数可动态变化"的意义）：
    // 调大 → 空闲槽位马上补领新段；调小 → 不打断手上的段，等它干完自然不再领。
    // 段表本身不用动：段是主机早就算好的，增减的只是"同时有几条连接在搬"。
    int affected = 0;
    m_transfers->forEachTask([clamped, &affected](Qdl::TransferTask *task) {
        if (!task->isActive())
            return;
        task->setWorkerCount(clamped);
        ++affected;
    });

    qInfo().noquote() << QStringLiteral("单文件连接数 -> %1（影响 %2 个进行中的任务）")
                             .arg(clamped)
                             .arg(affected);
}

void AppController::setSpeedLimitMbps(int mbps)
{
    const int clamped = qBound(0, mbps, kSpeedLimitMaxMbps);
    if (clamped == m_speedLimitMbps)
        return;

    m_speedLimitMbps = clamped;
    persist(QStringLiteral("transfer/speedLimitMbps"), clamped);

    // 立刻作用到正在跑的任务：不刷新的话，调低限速后还会按老速率再冲一小段
    Qdl::RateLimiter::instance()->setLimitForAll(qint64(clamped) * 1024 * 1024);
    if (m_transfers) {
        m_transfers->forEachTask([](Qdl::TransferTask *task) {
            if (task->isActive())
                task->refreshRateLimit();
        });
    }

    emit speedLimitChanged();
    qInfo().noquote() << QStringLiteral("传输限速 ->")
                      << (clamped > 0 ? QStringLiteral("%1 MB/s").arg(clamped)
                                      : QStringLiteral("不限速"));
}

QString AppController::speedLimitSummary() const
{
    return m_speedLimitMbps > 0 ? tr("%1 MB/s").arg(m_speedLimitMbps) : tr("不限速");
}

void AppController::pauseTransfer(const QString &taskId)
{
    if (auto *task = m_transfers->find(taskId))
        task->pause();
}

void AppController::resumeTransfer(const QString &taskId)
{
    auto *task = m_transfers->find(taskId);
    if (!task || !task->canResume())
        return;

    // 上传的断点在主机侧：重新申请一次，主机会回它已经收到的字节数，客户端据此跳读
    if (task->direction() == Qdl::TransferDirection::Upload) {
        const QString localPath = task->localPath();
        task->cancel();   // 旧任务留在抽屉里只会让人分不清哪条才是"正在传"
        uploadPaths(QStringList{ localPath });
        return;
    }

    // 续传起点以本地 .qdpart 的实际已收字节为准：M6 还会带上 fileKey 校验是否同一个文件
    requestDownloadFor(task->remotePath(), task->done(), task->id());
}

int AppController::resumeAllTransfers()
{
    if (!m_transfers)
        return 0;
    if (!m_client || !m_client->isReady()) {
        setTransferNotice(tr("尚未连接到主机，无法继续"));
        return 0;
    }

    // 先收集再动手：resumeTransfer 会改任务状态，边遍历边改容易漏
    QStringList ids;
    for (QObject *child : m_transfers->children()) {
        auto *task = qobject_cast<Qdl::TransferTask *>(child);
        // 只续下载：上传的可续性取决于主机那块半成品，一键续传会变成"重传一遍"
        if (task && task->canResume() && task->peerId() == m_client->peerId()
            && task->direction() == Qdl::TransferDirection::Download) {
            ids.append(task->id());
        }
    }
    for (const QString &id : std::as_const(ids))
        resumeTransfer(id);
    return ids.size();
}

QString AppController::transferChecksum(const QString &taskId)
{
    if (Qdl::TransferTask *task = m_transfers->find(taskId))
        return task->checksum();
    return {};
}

void AppController::startVerifyFlow(Qdl::TransferTask *task)
{
    if (!task)
        return;

    /**
     * M13 的校验分工：**摘要的计算与比对全部落在 TransferTask 里**，
     * AppController 只做两件它才做得了的事——
     *   1. 上传方向把本端算好的摘要推给主机（发送端 → 接收端）；
     *   2. 下载方向万一没收到推送，替任务补拉一次（老主机的兼容通道）。
     * 这样"什么时候算、算谁的、对不上怎么办"只有一份实现，不会两边各说一套。
     */
    connect(task, &Qdl::TransferTask::checksumReady, this, [this, task]() {
        // 下载方向不从这里发起补拉：任务自己判断"该不该补拉、还能不能降级"
        if (task->direction() != Qdl::TransferDirection::Upload)
            return;

        const QString hex = task->checksum();
        m_uploadLocalHash.insert(task->id(), hex);
        m_uploadHashDone.insert(task->id());

        // 推给接收端：它落地时拿这份摘要比对，不一致就把副本删掉
        if (!hex.isEmpty() && !task->transferId().isEmpty()
            && m_client && m_client->isReady()) {
            m_client->sendChecksumPush(task->transferId(), hex);
        }
        tryFinishUpload(task->id());
    });

    // 兜底通道：本地摘要已就绪但迟迟没有对端摘要，主动补拉（只拉一次）
    connect(task, &Qdl::TransferTask::checksumPullRequested, this,
            [this, task](const QString &path) {
                if (!m_client || !m_client->isReady()) {
                    task->checksumUnavailable(tr("主机已断开，只校验了大小"));
                    return;
                }
                const QString reqId = m_client->requestChecksum(path);
                if (reqId.isEmpty()) {
                    task->checksumUnavailable(tr("无法请求校验值，只校验了大小"));
                    return;
                }
                m_pendingChecksums.insert(reqId, task->id());
            });

    if (task->direction() == Qdl::TransferDirection::Upload) {
        // 主机的 UPLOAD_DONE 可能比本端"发送完毕"更早到（它边收边算，最后一个字节落地就收尾），
        // 那一刻本端状态还不是 Verifying、tryFinishUpload 会跳过。状态一变就再对一次账。
        connect(task, &Qdl::TransferTask::changed, this, [this, task]() {
            if (task->state() == Qdl::TransferState::Verifying)
                tryFinishUpload(task->id());
        });
    }
}

void AppController::tryFinishUpload(const QString &taskId)
{
    Qdl::TransferTask *task = m_transfers ? m_transfers->find(taskId) : nullptr;
    if (!task || task->state() != Qdl::TransferState::Verifying)
        return;

    // 两件事都到齐才下结论：本端摘要（与传输并行的后台算）＋ 主机的 UPLOAD_DONE
    if (!m_uploadHashDone.contains(taskId) || !m_uploadServerHash.contains(taskId))
        return;

    const QString localHash = m_uploadLocalHash.take(taskId);
    const QString serverHash = m_uploadServerHash.take(taskId);
    const QString note = m_uploadServerNote.take(taskId);
    m_uploadHashDone.remove(taskId);

    if (localHash.isEmpty()) {
        // 本端没能算出摘要：主机已经把它的值给回来了，人也留了痕，按"只校验了大小"收尾
        task->confirmVerified(true,
                              note.isEmpty() ? tr("无法计算本机文件校验值，只校验了大小") : note);
        setTransferNotice(tr("已上传 %1").arg(task->name()));
        return;
    }

    if (serverHash.isEmpty()) {
        task->confirmVerified(true,
                              note.isEmpty() ? tr("主机未提供校验值，只校验了大小") : note);
        setTransferNotice(tr("已上传 %1").arg(task->name()));
        return;
    }

    if (localHash.compare(serverHash, Qt::CaseInsensitive) == 0) {
        task->confirmVerified(true);
        setTransferNotice(tr("已上传 %1（校验一致）").arg(task->name()));
        return;
    }

    qWarning().noquote() << QStringLiteral("上传摘要不一致：本地 %1 主机 %2")
                                .arg(localHash, serverHash);
    task->confirmVerified(false, tr("校验失败：主机收到的内容与本地不一致"));
    setTransferNotice(tr("%1 上传校验失败").arg(task->name()));
}

void AppController::cancelTransfer(const QString &taskId)
{
    auto *task = m_transfers->find(taskId);
    if (!task)
        return;

    // 上传：必须让主机把半成品删掉，否则收件箱里会留一堆列表里看不见的 .qdpart
    if (task->direction() == Qdl::TransferDirection::Upload && m_client && m_client->isReady()
        && !task->transferId().isEmpty()) {
        m_client->cancelTransfer(task->transferId());
    }
    task->cancel();
}

void AppController::clearFinishedTransfers()
{
    if (m_transfers)
        m_transfers->clearFinished();
}

// ---------------------------- 上传（M7）----------------------------

QString AppController::inboxPath() const
{
    return QDir(m_shareRoot).filePath(QStringLiteral("inbox"));
}

QString AppController::uploadHint() const
{
    if (!m_client || !m_client->isReady())
        return tr("连接主机后可上传");
    if (!m_client->peerSharing())
        return tr("对方已关闭共享");
    if (!m_client->canUpload()) {
        // 分清两种"不能传"：只读设备本身没有写权限；有 rw 但主机把总开关拨掉了
        return m_client->perm() == QLatin1String("rw") ? tr("对方已关闭接收")
                                                       : tr("未获写权限（只读设备）");
    }
    return tr("接收方：%1 · 收件箱").arg(m_client->peerName());
}

void AppController::uploadLocalFile(const QString &name)
{
    if (!m_client || !m_client->isReady()) {
        setTransferNotice(tr("尚未连接到主机，无法上传"));
        return;
    }

    const QString virtualPath = Qdl::SafePath::child(
        m_localPath.isEmpty() ? QStringLiteral("/") : m_localPath, name);
    QString error;
    const QString absolute = virtualPath.isEmpty()
                                 ? QString()
                                 : Qdl::SafePath::resolve(m_localRoot, virtualPath, &error);
    if (absolute.isEmpty()) {
        setTransferNotice(error.isEmpty() ? tr("文件名不合法") : error);
        return;
    }
    uploadPaths(QStringList{ absolute });
}

int AppController::uploadPaths(const QStringList &absolutePaths)
{
    if (!m_client || !m_client->isReady()) {
        setTransferNotice(tr("尚未连接到主机，无法上传"));
        return 0;
    }
    if (!m_client->peerSharing()) {
        setTransferNotice(tr("对方已关闭共享"));
        return 0;
    }
    // 界面上按钮已置灰，这里是最后一道闸：可写能力以主机推送的 PERM_UPDATE 为准
    if (!m_client->canUpload()) {
        setTransferNotice(tr("对方已关闭接收文件"));
        return 0;
    }

    int queued = 0;
    int waiting = 0;
    for (const QString &path : absolutePaths) {
        // 每发一个都要重算并发：刚发走的那个马上就占了一个位置
        if (activeTransferCount() >= m_transferThreads) {
            if (!m_queuedUploads.contains(path)) {
                m_queuedUploads.append(path);
                ++waiting;
            }
            continue;
        }

        const QFileInfo info(path);
        if (!info.isFile()) {
            setTransferNotice(tr("只能上传文件：%1").arg(path));
            continue;
        }

        QString keyError;
        const QString fileKey = Qdl::FileScanner::fileKey(path, &keyError);
        if (fileKey.isEmpty())
            qWarning().noquote() << QStringLiteral("无法计算上传文件指纹：%1").arg(keyError);

        const QString reqId = m_client->requestUpload(info.fileName(), info.size(), fileKey,
                                                      m_segmentCount);
        if (reqId.isEmpty()) {
            setTransferNotice(tr("上传申请未发出（主机忙或未授权）"));
            break;
        }

        PendingUpload pending;
        pending.localPath = info.absoluteFilePath();
        pending.name = info.fileName();
        pending.size = info.size();
        pending.fileKey = fileKey;
        m_pendingUploads.insert(reqId, pending);
        ++queued;
    }

    if (waiting > 0) {
        setTransferNotice(tr("同时进行的文件已达 %1 个，%2 个上传在排队，前面的传完自动接上")
                              .arg(m_transferThreads).arg(waiting));
        emit queuedTransfersChanged();
    }

    if (queued > 0) {
        setTransferNotice(tr("正在上传 %1 个文件到 %2 的收件箱")
                              .arg(queued)
                              .arg(m_client->peerName()));
    }
    return queued;
}

int AppController::uploadUrls(const QVariantList &urls)
{
    QStringList paths;
    int ignored = 0;

    for (const QVariant &value : urls) {
        QUrl url = value.toUrl();
        if (!url.isValid())
            url = QUrl(value.toString());

        if (!url.isLocalFile()) {
            ++ignored;
            continue;
        }

        const QFileInfo info(url.toLocalFile());
        if (!info.isFile()) {   // 目录暂不支持：上传目录要先递归展开，语义也不清楚
            ++ignored;
            continue;
        }
        paths << info.absoluteFilePath();
    }

    if (paths.isEmpty()) {
        setTransferNotice(ignored > 0 ? tr("只能拖入本机文件（不支持目录）")
                                      : tr("没有可上传的文件"));
        return 0;
    }

    if (ignored > 0)
        qInfo().noquote() << QStringLiteral("拖拽上传：忽略 %1 个非文件项").arg(ignored);

    return uploadPaths(paths);
}

void AppController::openInbox()
{
    const QString path = inboxPath();
    QDir().mkpath(path);
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        setTransferNotice(tr("无法打开收件目录：%1").arg(path));
}

void AppController::refreshIncoming()
{
    const int count = m_incomingNames.size();
    QString summary;
    if (count == 1) {
        const QString id = m_incomingNames.constBegin().key();
        summary = tr("正在接收 %1（%2%）")
                      .arg(m_incomingNames.value(id))
                      .arg(m_incomingPercents.value(id, 0));
    } else if (count > 1) {
        summary = tr("正在接收 %1 个文件").arg(count);
    }

    const bool countChanged = (count != m_incomingCount);
    const bool textChanged = (summary != m_incomingSummary);
    m_incomingCount = count;
    m_incomingSummary = summary;

    if (textChanged || countChanged)
        emit incomingChanged();

    // m_incomingCount 更新之后再判断，否则拿到的是上一轮的值
    updateForegroundService();
}

void AppController::updateForegroundService()
{
#ifdef Q_OS_ANDROID
    const bool busy = transferActiveCount() > 0 || m_incomingCount > 0;
    if (busy == m_foregroundActive)
        return;
    m_foregroundActive = busy;
    if (busy)
        Qdl::AndroidService::start(tr("正在传输文件"));
    else
        Qdl::AndroidService::stop();
#endif
}

QString AppController::downloadDestination(const QString &virtualPath, const QString &name) const
{
    // 保留远端目录结构（/docs/a.pdf → 接收目录/docs/a.pdf）：
    // 同名文件散在一个平面目录里，过两天就分不清哪个是哪个了
    const QString remoteDir = Qdl::SafePath::parent(virtualPath);
    QString dir = m_localRoot;
    if (remoteDir != QLatin1String("/") && !remoteDir.isEmpty())
        dir = QDir(m_localRoot).filePath(remoteDir.mid(1));

    QDir().mkpath(dir);

    const QString baseName = Qdl::SafePath::isValidName(name) ? name : QStringLiteral("download.bin");
    const int dot = baseName.lastIndexOf(QLatin1Char('.'));
    const QString stem = dot > 0 ? baseName.left(dot) : baseName;
    const QString suffix = dot > 0 ? baseName.mid(dot) : QString();

    QString candidate = QDir(dir).filePath(baseName);
    int n = 1;
    auto taken = [](const QString &path) {
        return QFile::exists(path) || QFile::exists(path + QStringLiteral(".qdpart"));
    };
    while (taken(candidate)) {
        candidate = QDir(dir).filePath(QStringLiteral("%1 (%2)%3").arg(stem).arg(n).arg(suffix));
        ++n;
        if (n > 999)
            break;
    }
    return candidate;
}

void AppController::setTransferNotice(const QString &message)
{
    if (m_transferNotice == message && !message.isEmpty())
        return;

    m_transferNotice = message;
    emit transferNoticeChanged();

    if (message.isEmpty())
        return;

    // 自动消失：这类提示不该长期占着抽屉表头
    QTimer::singleShot(6000, this, [this, message]() {
        if (m_transferNotice == message) {
            m_transferNotice.clear();
            emit transferNoticeChanged();
        }
    });
}

// ---------------------------- 持久化 ----------------------------

void AppController::persist(const QString &key, const QVariant &value)
{
    m_settings->setValue(key, value);
    m_settings->sync();
}

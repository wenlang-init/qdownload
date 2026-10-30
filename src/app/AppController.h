#pragma once

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include "net/PeerInfo.h"
#include "net/TrustStore.h"
#include "fs/FileListModel.h"
#include "app/LaunchOptions.h"

class DiscoveryService;
class PeerListModel;
class PairingService;
class ControlServer;
class ControlClient;

namespace Qdl {
class TransferManager;
class TransferTask;
class ResumeStore;
}

/**
 * 应用级单例，QML 里以 `App` 访问。
 *
 * 职责：
 *   1. 只读环境信息（版本、平台、设备名、日志路径）
 *   2. 顶栏两个总开关（共享 / 写权限）并持久化，变化时立刻广播
 *   3. 组播发现与主机列表（M2）
 *   4. 控制通道、配对与信任库（M3）
 *
 * 约束（设计文档 §3）：QML 只读属性、只调槽，业务判定一律在 C++ 侧完成。
 */
class AppController : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(App)
    QML_SINGLETON

    Q_PROPERTY(QString appVersion   READ appVersion   CONSTANT)
    Q_PROPERTY(QString platformName READ platformName CONSTANT)
    /// 本端名称：对方在主机列表里看到的名字。可在设置里改，改完立刻重新广播
    Q_PROPERTY(QString deviceName       READ deviceName       WRITE setDeviceName NOTIFY deviceNameChanged)
    /// 「恢复默认」用的出厂值（主机名），不随当前设置变化
    Q_PROPERTY(QString defaultDeviceName READ defaultDeviceName CONSTANT)
    Q_PROPERTY(QString logFilePath  READ logFilePath  CONSTANT)
    /// 信任库/断点库/设置的存放位置，以及当前是便携模式还是安装模式
    Q_PROPERTY(QString storageRoot READ storageRoot CONSTANT)
    Q_PROPERTY(QString storageSummary READ storageSummary CONSTANT)
    Q_PROPERTY(QString statusLine   READ statusLine   NOTIFY statusLineChanged)
    // 截图用：窄屏布局启动时停在哪一页（0 本机 / 1 远端）
    Q_PROPERTY(int     demoTabIndex READ demoTabIndex CONSTANT)

    Q_PROPERTY(bool    shareEnabled READ shareEnabled WRITE setShareEnabled NOTIFY shareEnabledChanged)
    Q_PROPERTY(bool    writeEnabled READ writeEnabled WRITE setWriteEnabled NOTIFY writeEnabledChanged)
    /// 状态栏那句「写权限：xxx」的值。
    /// 必须是带 NOTIFY 的**属性**而不是 Q_INVOKABLE：QML 对 Q_INVOKABLE 只求值一次，
    /// 也不会建立依赖，写权限开关拨来拨去它就永远停在旧值（症状：只有颜色在变）
    Q_PROPERTY(QString writeSummary READ writeSummary NOTIFY writeSummaryChanged)
    Q_PROPERTY(QString shareRoot    READ shareRoot    WRITE setShareRoot    NOTIFY shareRootChanged)
    /// 「恢复默认」按钮用的出厂值，只读，不随当前选择变化
    Q_PROPERTY(QString defaultShareRoot   READ defaultShareRoot   CONSTANT)
    Q_PROPERTY(QString defaultReceiveRoot READ defaultReceiveRoot CONSTANT)

    // 0 = 仅白名单，1 = 已配对皆可传（设计文档 §7.4 L2）
    Q_PROPERTY(int     uploadPolicy READ uploadPolicy WRITE setUploadPolicy NOTIFY uploadPolicyChanged)
    Q_PROPERTY(QString uploadPolicyText READ uploadPolicyText NOTIFY uploadPolicyChanged)

    // ---- 发现（M2）----
    Q_PROPERTY(QAbstractItemModel *peers READ peers CONSTANT)
    Q_PROPERTY(int     peerCount       READ peerCount       NOTIFY peerCountChanged)
    Q_PROPERTY(int     onlinePeerCount READ onlinePeerCount NOTIFY peerCountChanged)
    Q_PROPERTY(QString peerSummary     READ peerSummary     NOTIFY peerCountChanged)
    Q_PROPERTY(QString selfPeerId      READ selfPeerId      CONSTANT)
    Q_PROPERTY(bool    discoveryOk     READ discoveryOk     NOTIFY discoveryErrorChanged)
    Q_PROPERTY(QString discoveryError  READ discoveryError  NOTIFY discoveryErrorChanged)

    Q_PROPERTY(QString selectedPeerId    READ selectedPeerId    WRITE setSelectedPeerId NOTIFY selectedPeerChanged)
    Q_PROPERTY(QString selectedPeerName  READ selectedPeerName  NOTIFY selectedPeerChanged)
    Q_PROPERTY(QString selectedPeerBadge READ selectedPeerBadge NOTIFY selectedPeerChanged)
    Q_PROPERTY(QString selectedPeerKind  READ selectedPeerKind  NOTIFY selectedPeerChanged)
    Q_PROPERTY(bool    selectedPeerOnline READ selectedPeerOnline NOTIFY selectedPeerChanged)

    // ---- 控制通道与配对（M3）----
    Q_PROPERTY(int     ctrlPort         READ ctrlPort         NOTIFY ctrlPortChanged)
    Q_PROPERTY(QString trustStorePath   READ trustStorePath   CONSTANT)
    Q_PROPERTY(QString connStateCode    READ connStateCode    NOTIFY connChanged)
    Q_PROPERTY(QString connStateText    READ connStateText    NOTIFY connChanged)
    Q_PROPERTY(QString connError        READ connError        NOTIFY connChanged)
    /// 用户自己点了「断开」：为 true 时不再自动重连，直到他手动点「重连」或换一台主机
    Q_PROPERTY(bool peerDisconnectedByUser READ peerDisconnectedByUser NOTIFY connChanged)
    Q_PROPERTY(QString peerPerm         READ peerPerm         NOTIFY connChanged)
    Q_PROPERTY(bool    peerCanUpload    READ peerCanUpload    NOTIFY connChanged)
    Q_PROPERTY(bool    peerShares       READ peerShares       NOTIFY connChanged)
    Q_PROPERTY(bool    peerConnected    READ peerConnected    NOTIFY connChanged)
    Q_PROPERTY(QVariantMap pairRequest  READ pairRequest      NOTIFY pairRequestChanged)
    Q_PROPERTY(QVariantList trustedDevices READ trustedDevices NOTIFY trustedDevicesChanged)

    // ---- 目录列表（M4）----
    // 本机页浏览的是接收目录；远端页浏览的是对方共享根
    Q_PROPERTY(QAbstractItemModel *localFiles READ localFiles CONSTANT)
    Q_PROPERTY(QString localRoot     READ localRoot     WRITE setLocalRoot     NOTIFY localRootChanged)
    Q_PROPERTY(QString localPath     READ localPath     NOTIFY localPathChanged)
    Q_PROPERTY(QVariantList localBreadcrumb READ localBreadcrumb NOTIFY localPathChanged)
    Q_PROPERTY(QString localNotice   READ localNotice   NOTIFY localPathChanged)

    Q_PROPERTY(QAbstractItemModel *remoteFiles READ remoteFiles CONSTANT)
    Q_PROPERTY(QString remotePath    READ remotePath    NOTIFY remotePathChanged)
    Q_PROPERTY(QVariantList remoteBreadcrumb READ remoteBreadcrumb NOTIFY remotePathChanged)
    Q_PROPERTY(bool   remoteLoading  READ remoteLoading NOTIFY remoteLoadingChanged)
    Q_PROPERTY(QString remoteError   READ remoteError   NOTIFY remoteErrorChanged)
    Q_PROPERTY(bool   remoteCanMore  READ remoteCanMore NOTIFY remoteListChanged)
    Q_PROPERTY(int    remoteTotal    READ remoteTotal   NOTIFY remoteListChanged)

    // ---- 传输（M5）----
    Q_PROPERTY(QAbstractItemModel *transfers READ transfers CONSTANT)
    Q_PROPERTY(int     transferActiveCount READ transferActiveCount NOTIFY transfersChanged)
    Q_PROPERTY(int     transferCount       READ transferCount       NOTIFY transfersChanged)
    Q_PROPERTY(int     transferFinishedCount READ transferFinishedCount NOTIFY transfersChanged)
    Q_PROPERTY(QString transferSummary     READ transferSummary     NOTIFY transfersChanged)
    Q_PROPERTY(QString transferNotice      READ transferNotice      NOTIFY transferNoticeChanged)
    /// 同时进行的文件传输条数（一个任务一条数据连接）。超出的自动排队，等空位接上
    Q_PROPERTY(int transferThreads     READ transferThreads     WRITE setTransferThreads NOTIFY transferThreadsChanged)
    Q_PROPERTY(int transferThreadsMin  READ transferThreadsMin  CONSTANT)
    Q_PROPERTY(int transferThreadsMax  READ transferThreadsMax  CONSTANT)
    /// 因为并发上限在排队等待的下载+上传条数
    Q_PROPERTY(int queuedTransferCount READ queuedTransferCount NOTIFY queuedTransfersChanged)
    /// 单个文件同时开几条连接并行搬（1 = 单连接）。段表由主机下发，这里只控制并发连接数
    Q_PROPERTY(int segmentCount    READ segmentCount    WRITE setSegmentCount NOTIFY segmentCountChanged)
    Q_PROPERTY(int segmentCountMax READ segmentCountMax CONSTANT)
    /// 全局传输限速，单位 MB/s；0 = 不限速。上传下载共用一个上限
    Q_PROPERTY(int speedLimitMbps READ speedLimitMbps WRITE setSpeedLimitMbps NOTIFY speedLimitChanged)
    Q_PROPERTY(int speedLimitMax READ speedLimitMax CONSTANT)
    Q_PROPERTY(QString speedLimitSummary READ speedLimitSummary NOTIFY speedLimitChanged)
    // 截图/演示：启动即展开传输抽屉
    Q_PROPERTY(bool    demoTransfers       READ demoTransfers       CONSTANT)
    /// 截图用：强制显示拖拽上传提示层
    Q_PROPERTY(bool    demoDrop            READ demoDrop            CONSTANT)
    /// 截图用：到点后把「关闭写权限」确认框拉起来
    Q_PROPERTY(bool    demoConfirmWrite    READ demoConfirmWrite    NOTIFY demoConfirmWriteChanged)
    /// 截图用：启动即打开设置面板
    Q_PROPERTY(bool    demoSettings        READ demoSettings        CONSTANT)
    /// 排障/截图：桌面也强制走安卓那套内置目录浏览器（真机没法一点点试）
    Q_PROPERTY(bool    demoDirBrowser      READ demoDirBrowser      CONSTANT)
    Q_PROPERTY(int     demoQuitMs          READ demoQuitMs          CONSTANT)
    // 仅联调：让 QML 里的配对弹窗延时"点"一下「允许配对」，
    // 走真实的按钮路径（--auto-approve 是 begin() 里提前放行，盖不到这条）
    Q_PROPERTY(int     demoApproveMs       READ demoApproveMs       CONSTANT)

    // ---- 上传（M7）----
    /// 上传按钮的状态说明（未连接 / 对方关闭接收 / 可上传）
    Q_PROPERTY(QString uploadHint      READ uploadHint      NOTIFY uploadHintChanged)
    /// 共享端：正在接收的上传（收件箱）摘要
    Q_PROPERTY(QString incomingSummary READ incomingSummary NOTIFY incomingChanged)
    Q_PROPERTY(int     incomingCount   READ incomingCount   NOTIFY incomingChanged)
    /// 收件箱真实路径（展示与「打开」按钮用）
    Q_PROPERTY(QString inboxPath       READ inboxPath       CONSTANT)

    // ---- 托盘与通知（M8）----
    /// 点窗口关闭按钮时收进托盘而不是退出（设置里可关）
    Q_PROPERTY(bool    closeToTray     READ closeToTray     WRITE setCloseToTray NOTIFY closeToTrayChanged)
    /// 托盘是否真的可用（由 QML 侧探测后回填，C++ 不猜平台能力）
    Q_PROPERTY(bool    trayAvailable   READ trayAvailable   NOTIFY trayAvailableChanged)

    // ---- 界面语言（M8）----
    /// 空 / "system" = 跟随系统；"zh_CN" / "en" = 指定语言
    Q_PROPERTY(QString language        READ language        WRITE setLanguage NOTIFY languageChanged)

public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    QString appVersion() const;
    QString platformName() const;
    QString deviceName() const;
    /// 出厂值：主机名；主机名为空时给个占位，保证界面上永远有名字
    QString defaultDeviceName() const;
    /// 空串 = 恢复默认（主机名）。会去掉首尾空白并截断到 32 字
    void setDeviceName(const QString &name);
    QString logFilePath() const;
    QString storageRoot() const;
    QString storageSummary() const;
    QString statusLine() const;
    int demoTabIndex() const;

    bool shareEnabled() const { return m_shareEnabled; }
    void setShareEnabled(bool enabled);

    bool writeEnabled() const { return m_writeEnabled; }
    void setWriteEnabled(bool enabled);

    QString shareRoot() const { return m_shareRoot; }
    void setShareRoot(const QString &path);

    /// 出厂默认目录：本机文档下的 qdownload / 应用数据下的 downloads
    QString defaultShareRoot() const;
    QString defaultReceiveRoot() const;

    /// 换目录前的体检。返回空串表示可以换，否则是给用户看的中文原因。
    /// role：0 = 共享根，1 = 接收目录。UI 在应用之前调用，C++ 的 setter 也会自己再兜一遍
    Q_INVOKABLE QString directoryIssue(const QString &path, int role) const;

    /// QML 选目录拿到的是 url，转成本地路径；不是 file:// 就返回空（Android 的 content:// 走不了真实路径）
    Q_INVOKABLE QString urlToPath(const QVariant &url) const;
    Q_INVOKABLE QUrl pathToUrl(const QString &path) const;

    // ---- 目录选择（Android 分区存储适配）----
    /// Android true：系统目录选择器在这里返回 content:// 授权 URI（QFile 打不开），
    /// 也不会过滤掉没权限写的公共目录，所以改用应用自带的内置浏览器
    Q_PROPERTY(bool    builtInDirPicker READ builtInDirPicker CONSTANT)
    /// 是否拿到外部存储的「所有文件访问权限」（桌面恒为 true）
    Q_PROPERTY(bool    allFilesAccess   READ allFilesAccess   NOTIFY allFilesAccessChanged)
    /// 没授权时的说明文案（桌面为空）
    Q_PROPERTY(QString storageHint      READ storageHint      NOTIFY allFilesAccessChanged)
    /// 启动自愈说明：原来存的目录这次不可写、已临时退回默认值。没有则为空
    Q_PROPERTY(QString startupNotice    READ startupNotice    CONSTANT)

    /// 当前能用的根目录：[{path, label}]，只有实际试写通过的才会出现
    Q_INVOKABLE QVariantList storageRoots() const;
    /// path 下可直接进入的子目录名（隐藏目录与软链已剔除）
    Q_INVOKABLE QStringList subDirectories(const QString &path) const;
    /// 在 parent 下建一个新目录；成功返回绝对路径，失败返回空
    Q_INVOKABLE QString createSubDirectory(const QString &parent, const QString &name);
    /// 跳到系统设置页让用户开「所有文件访问权限」
    Q_INVOKABLE void requestStorageAccess();
    /// 重测一次权限：用户在系统设置页改完回来才拿得到，我们收不到回调，只能轮询
    Q_INVOKABLE void refreshStorageAccess();

    /// 拼子路径／取上级：交给 C++ 统一处理，免得 QML 手写斜杠被根目录的尾斜杠坑到
    Q_INVOKABLE QString childPath(const QString &parent, const QString &name) const;
    Q_INVOKABLE QString parentPath(const QString &path) const;

    bool builtInDirPicker() const;
    bool allFilesAccess() const;
    QString storageHint() const;
    QString startupNotice() const;

    int uploadPolicy() const { return m_uploadPolicy; }
    void setUploadPolicy(int policy);
    QString uploadPolicyText() const;

    QAbstractItemModel *peers() const;
    int peerCount() const;
    int onlinePeerCount() const;
    QString peerSummary() const;
    QString selfPeerId() const;
    bool discoveryOk() const { return m_discoveryOk; }
    QString discoveryError() const { return m_discoveryError; }

    QString selectedPeerId() const { return m_selectedPeerId; }
    void setSelectedPeerId(const QString &peerId);
    QString selectedPeerName() const;
    QString selectedPeerBadge() const;
    /// upload / readonly / unshared / offline，空表示没选中
    QString selectedPeerKind() const;
    bool selectedPeerOnline() const;

    int ctrlPort() const;
    QString trustStorePath() const;
    QString connStateCode() const;
    QString connStateText() const;
    QString connError() const;
    QString peerPerm() const;
    bool peerCanUpload() const;
    bool peerShares() const;
    bool peerConnected() const;
    /// 当前这个「未连接」是用户自己点的，而不是掉线/失败
    bool peerDisconnectedByUser() const { return m_manualDisconnect; }
    QVariantMap pairRequest() const;
    QVariantList trustedDevices() const;

    QAbstractItemModel *localFiles() const;
    QString localRoot() const { return m_localRoot; }
    void setLocalRoot(const QString &path);

    QString localPath() const { return m_localPath; }
    QVariantList localBreadcrumb() const { return m_localBreadcrumb; }
    QString localNotice() const { return m_localNotice; }

    QAbstractItemModel *remoteFiles() const;
    QString remotePath() const { return m_remotePath; }
    QVariantList remoteBreadcrumb() const { return m_remoteBreadcrumb; }
    bool remoteLoading() const { return m_remoteLoading; }
    QString remoteError() const { return m_remoteError; }
    bool remoteCanMore() const;
    int remoteTotal() const;

    QAbstractItemModel *transfers() const;
    int transferActiveCount() const;
    int transferCount() const;
    int transferFinishedCount() const;
    QString transferSummary() const;
    QString transferNotice() const { return m_transferNotice; }
    bool demoTransfers() const;
    bool demoDrop() const;
    bool demoConfirmWrite() const { return m_demoConfirmWrite; }
    bool demoSettings() const;
    bool demoDirBrowser() const;
    int demoQuitMs() const { return launchOptions().demoQuitMs; }
    int demoApproveMs() const { return launchOptions().approvePendingAfterMs; }

    QString uploadHint() const;
    QString incomingSummary() const { return m_incomingSummary; }
    int incomingCount() const { return m_incomingCount; }
    QString inboxPath() const;

    bool closeToTray() const { return m_closeToTray; }
    void setCloseToTray(bool on);
    bool trayAvailable() const { return m_trayAvailable; }
    /// QML 探测到托盘后回填；关掉它就不再往托盘发通知
    Q_INVOKABLE void setTrayAvailable(bool available);
    /// 桌面平台由托盘气泡承担通知；Android 无托盘，走系统通知通道
    Q_INVOKABLE void notifySystem(const QString &title, const QString &body);

    QString language() const { return m_language; }
    void setLanguage(const QString &code);

    /// 状态栏那句「写权限：xxx」：「关」/「白名单」/「全部已配对」
    QString writeSummary() const;

    int transferThreads() const { return m_transferThreads; }
    void setTransferThreads(int count);
    int transferThreadsMin() const { return kTransferThreadsMin; }
    int transferThreadsMax() const { return kTransferThreadsMax; }
    int queuedTransferCount() const { return m_queuedDownloads.size() + m_queuedUploads.size(); }

    int segmentCount() const { return m_segmentCount; }
    void setSegmentCount(int count);
    int segmentCountMax() const { return kSegmentMax; }

    int speedLimitMbps() const { return m_speedLimitMbps; }
    void setSpeedLimitMbps(int mbps);
    int speedLimitMax() const { return kSpeedLimitMaxMbps; }
    QString speedLimitSummary() const;

    /// 正在传（含连接中、校验中）的任务数，下载上传都算
    int activeTransferCount() const;
    /// 空位放出来时把排队的补上去，以及并发上限变化后立即结算一次
    void drainTransferQueues();
    /// 塞进下载队列；重复路径会被忽略，返回值表示是否新排了一条
    bool enqueueDownload(const QString &virtualPath, qint64 offset, const QString &taskId);
    /// 真正向主机发 DOWNLOAD_REQ（并发检查在外面）
    void issueDownloadRequest(const QString &virtualPath, qint64 offset, const QString &taskId);

    /// “刷新”按钮：立即发一次心跳，并重连当前主机（若断开）
    Q_INVOKABLE void announceNow();

    Q_INVOKABLE void connectToSelectedPeer();
    /// 手动断开：除了断链，还会把“自动重连”挂起（心跳/刷新都不再把它拉回来）
    Q_INVOKABLE void disconnectPeer();
    Q_INVOKABLE void requestPairing();
    Q_INVOKABLE void submitPairCode(const QString &code);

    /// 共享端：放行 / 拒绝当前配对请求；allowWrite 决定 ro 还是 rw
    Q_INVOKABLE void approvePendingPair(bool allowWrite);
    Q_INVOKABLE void rejectPendingPair();

    /// 共享端：改某台已配对设备的权限（ro/rw）或撤销
    Q_INVOKABLE void setTrustedPerm(const QString &peerId, const QString &perm);
    Q_INVOKABLE void revokeTrusted(const QString &peerId);

    // ---- 目录导航（M4）----
    Q_INVOKABLE void localGo(const QString &path);
    Q_INVOKABLE void localEnter(const QString &name);
    Q_INVOKABLE void localUp();
    Q_INVOKABLE void localRefresh();

    Q_INVOKABLE void remoteGo(const QString &path);
    Q_INVOKABLE void remoteEnter(const QString &name);
    Q_INVOKABLE void remoteUp();
    Q_INVOKABLE void remoteRefresh();
    Q_INVOKABLE void remoteLoadMore();

    // ---- 删除（M14）----
    /// 本机当前目录里的某个文件：删掉（只删文件，目录要进去逐个删）
    Q_INVOKABLE void deleteLocalFile(const QString &name);
    /// 按虚拟路径删本机文件（CLI 与 deleteLocalFile 共用），返回是否真的删掉了
    Q_INVOKABLE bool deleteLocalPath(const QString &virtualPath);
    /// 远端当前目录里的某个文件：让主机删掉，需要对方开着写权限
    Q_INVOKABLE void deleteRemoteFile(const QString &name);

    // ---- 下载（M5）----
    /// 远端当前目录里的某个文件：点「下载」
    Q_INVOKABLE void downloadFile(const QString &name);
    /// 直接按虚拟路径下载（offset > 0 即续传）
    Q_INVOKABLE void downloadPath(const QString &virtualPath, qint64 offset = 0);
    Q_INVOKABLE void pauseTransfer(const QString &taskId);
    Q_INVOKABLE void resumeTransfer(const QString &taskId);
    Q_INVOKABLE void cancelTransfer(const QString &taskId);
    Q_INVOKABLE void clearFinishedTransfers();
    /// 把所有「已暂停 / 有断点」的任务挨个接着下（重启后一键续传）
    Q_INVOKABLE int resumeAllTransfers();
    /// 已完成的校验值（CLI / 排障用；未算出返回空）
    Q_INVOKABLE QString transferChecksum(const QString &taskId);

    // ---- 上传（M7）----
    /// 本机当前目录里的某个文件：点「上传」（落到主机的 /inbox）
    Q_INVOKABLE void uploadLocalFile(const QString &name);
    /// 直接按本机绝对路径上传（CLI / 拖拽）
    Q_INVOKABLE int uploadPaths(const QStringList &absolutePaths);
    /// 桌面拖拽：把 DropArea 收到的 URL 列表转成本机路径再上传（非文件项会被忽略）
    Q_INVOKABLE int uploadUrls(const QVariantList &urls);
    /// 在系统文件管理器里打开收件箱
    Q_INVOKABLE void openInbox();

signals:
    void shareEnabledChanged();
    void writeEnabledChanged();
    void writeSummaryChanged();
    void shareRootChanged();
    void localRootChanged();
    void allFilesAccessChanged();
    void uploadPolicyChanged();
    void statusLineChanged();
    void deviceNameChanged();
    void peerCountChanged();
    void discoveryErrorChanged();
    void selectedPeerChanged();
    void ctrlPortChanged();
    void connChanged();
    void pairRequestChanged();
    void trustedDevicesChanged();
    void localPathChanged();
    void remotePathChanged();
    void remoteLoadingChanged();
    void remoteErrorChanged();
    void remoteListChanged();
    void transfersChanged();
    void transferNoticeChanged();
    void transferThreadsChanged();
    void segmentCountChanged();
    void queuedTransfersChanged();
    void speedLimitChanged();
    void uploadHintChanged();
    void incomingChanged();
    void closeToTrayChanged();
    void trayAvailableChanged();
    void languageChanged();
    void demoConfirmWriteChanged();

    /// 需要弹一条系统通知（桌面走托盘气泡，Android 走通知栏）。
    /// QML 决定最终形态：窗口在前台时它会换成窗口内的轻提示
    void notify(const QString &title, const QString &body);

    /// 客户端侧：主机已给出 nonce，请打开“输入配对码”弹窗
    void pairCodePrompt();
    void pairSucceeded();
    void pairFailed(const QString &message);
    void devicePaired(const QString &name, const QString &perm);

private:
    void persist(const QString &key, const QVariant &value);

    /// 按当前模式创建设置对象：便携模式用 ini 文件，且首次会从系统位置迁移老配置
    QSettings *openSettings();
    PeerInfo buildSelf() const;
    void refreshCounts();
    void refreshTrusted();
    void rebuildTrustedDeviceList();
    void maybeAutoConnect();
    void applyShareStateToServer();

    /// 下载落点：接收目录 + 远端目录结构，重名自动加序号
    QString downloadDestination(const QString &virtualPath, const QString &name) const;
    void setTransferNotice(const QString &message);
    /// taskId 非空 = 续传，复用已有任务对象
    void requestDownloadFor(const QString &virtualPath, qint64 offset, const QString &taskId);
    /// 下载落地后跟主机对 SHA-256：先本机算，再问主机，一致才算完成
    void startVerifyFlow(Qdl::TransferTask *task);
    /// 上传：本地摘要与主机回传的摘要都到齐后给出结论
    void tryFinishUpload(const QString &taskId);
    void refreshIncoming();
    /// Android：有活跃传输时挂前台服务，全部结束后撤掉（桌面平台空实现）
    void updateForegroundService();

    /// 面包屑：根 + 每一级目录，QML 点击任一段直接跳过去
    QVariantList makeBreadcrumb(const QString &path, const QString &rootLabel) const;
    void setRemoteLoading(bool loading);
    void setRemoteError(const QString &message);
    void updateRemoteBreadcrumb();

    /// 上一次存下来的目录这回可能已经写不了（Android 重装、桌面策略收紧），
    /// 遇到就退回出厂默认，避免界面上“拨了开关没反应”却查不出原因
    void repairUnusableRoots();

    // 便携模式指向程序目录下的 settings.ini，安装模式指向空串（用 QSettings 原生位置）
    QString m_settingsFile;
    QSettings *m_settings = nullptr;
    bool m_shareEnabled = false;
    bool m_writeEnabled = false;
    QString m_shareRoot;
    int m_uploadPolicy = 0;
    /// 外部存储全盘访问权（Android 的 MANAGE_EXTERNAL_STORAGE）；桌面不受限，恒为 true
    bool m_allFilesAccess = true;
    /// 启动自愈留给用户看的一句话（目录被退回默认值时才非空）
    QString m_startupNotice;

    QString m_peerId;
    DiscoveryService *m_discovery = nullptr;
    PeerListModel *m_peers = nullptr;
    bool m_discoveryOk = true;
    QString m_discoveryError;
    int m_peerCount = 0;
    int m_onlinePeerCount = 0;
    QString m_selectedPeerId;

    TrustStore m_trust;
    PairingService *m_pairing = nullptr;
    ControlServer *m_server = nullptr;
    ControlClient *m_client = nullptr;
    QVariantList m_trustedDevices;
    QElapsedTimer m_lastConnectAttempt;
    /// 用户在界面上点了「断开」：从此不再自动重连（心跳、刷新、主机重新上线一律不拉），
    /// 直到他点「重连」或换选一台主机。只影响当前这台主机，随进程退出一起消失
    bool m_manualDisconnect = false;

    Qdl::FileListModel *m_localFiles = nullptr;
    Qdl::FileListModel *m_remoteFiles = nullptr;
    QString m_localRoot;
    QString m_localPath;
    QString m_localNotice;
    QVariantList m_localBreadcrumb;

    QString m_remotePath;
    QVariantList m_remoteBreadcrumb;
    bool m_remoteLoading = false;
    QString m_remoteError;

    /// 已发出 DOWNLOAD_REQ、还没收到 DOWNLOAD_ACCEPT 的请求
    struct PendingDownload
    {
        QString path;
        qint64 offset = 0;
        QString taskId;   // 续传时复用已有任务；空表示新建
    };
    Qdl::TransferManager *m_transfers = nullptr;
    Qdl::ResumeStore *m_resumeStore = nullptr;
    QHash<QString, PendingDownload> m_pendingDownloads;
    QHash<QString, QString> m_pendingChecksums;   // reqId → taskId

    /// 已发出 SEG_REQ、还没拿到令牌的领取请求（M12）
    struct PendingSegment
    {
        QString taskId;
        int index = -1;
    };
    QHash<QString, PendingSegment> m_pendingSegments;   // reqId → (任务, 段号)
    QString m_transferNotice;

    /// 并发达到上限时排队的下载：还没向主机发 DOWNLOAD_REQ，等空位再发
    struct QueuedDownload
    {
        QString path;
        qint64 offset = 0;
        QString taskId;
    };
    QList<QueuedDownload> m_queuedDownloads;
    /// 同理排队的上传（只记住本地路径，文件可能随后被改动，所以发请求前再校验）
    QStringList m_queuedUploads;

    // 同时进行的文件传输条数。一个任务就是一条数据连接，太多只会互相抢带宽
    static constexpr int kTransferThreadsMin = 1;
    static constexpr int kTransferThreadsMax = 16;
    static constexpr int kTransferThreadsDefault = 3;
    int m_transferThreads = kTransferThreadsDefault;
    /// 单文件连接数：1 = 老的单连接行为
    static constexpr int kSegmentMin = 1;
    static constexpr int kSegmentMax = 8;
    static constexpr int kSegmentDefault = 3;
    int m_segmentCount = kSegmentDefault;

    /// 限速上限 1000 MB/s：千兆网的理论上限是 125 MB/s，封高点不至于挡住 2.5G/万兆网
    static constexpr int kSpeedLimitMaxMbps = 1000;
    static constexpr int kSpeedLimitDefaultMbps = 0;   // 默认不限速：局域网传文件本来就是为了快
    int m_speedLimitMbps = kSpeedLimitDefaultMbps;
    bool m_demoGetStarted = false;
    bool m_demoPutStarted = false;

    /// 已发 UPLOAD_REQ、还没收到 UPLOAD_ACCEPT 的请求
    struct PendingUpload
    {
        QString localPath;
        QString name;
        qint64 size = 0;
        QString fileKey;
    };
    QHash<QString, PendingUpload> m_pendingUploads;   // reqId → 待上传
    QHash<QString, QString> m_uploadTransferIds;      // 主机 transferId → taskId
    QHash<QString, QString> m_uploadLocalHash;        // taskId → 本地 SHA-256
    QSet<QString> m_uploadHashDone;                   // 已算完本地摘要的上传任务（可能为空值）
    QHash<QString, QString> m_uploadServerHash;       // taskId → 主机 SHA-256
    QHash<QString, QString> m_uploadServerNote;       // taskId → 主机说明

    // 共享端：正在接收的上传（收件箱）
    QHash<QString, QString> m_incomingNames;    // transferId → 文件名
    QHash<QString, int> m_incomingPercents;     // transferId → 百分比
    QString m_incomingSummary;
    int m_incomingCount = 0;

    bool m_closeToTray = false;
    bool m_trayAvailable = false;
    QString m_language;
    bool m_foregroundActive = false;
    bool m_demoConfirmWrite = false;
    bool m_pairReplyPending = false;       // 「允许/拒绝」已排队，等下一轮事件循环执行
    bool m_trustedRefreshQueued = false;   // refreshTrusted 已排队（合并连续调用）
};

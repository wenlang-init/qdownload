#pragma once

#include "TransferTypes.h"

#include <functional>

#include <QObject>
#include <QString>

namespace Qdl {

class TransferTask;
class TransferModel;
class ResumeStore;

/**
 * 传输任务的宿主：建任务、回收任务、把任务变化转成模型通知。
 *
 * AppController 只跟这里打交道，不需要知道 QTcpSocket / QFile 的存在。
 */
class TransferManager : public QObject
{
    Q_OBJECT

public:
    explicit TransferManager(QObject *parent = nullptr);

    TransferModel *model() const { return m_model; }

    TransferTask *createDownload(const QString &name, const QString &remotePath,
                                 const QString &localPath, qint64 total);

    /**
     * 建一个上传任务（M7）。localPath 是**源文件**，remotePath 是主机去重后的落点。
     * 上传的断点在主机侧（`.qdpart`），所以不写进本机的断点库。
     */
    TransferTask *createUpload(const QString &name, const QString &localPath,
                               const QString &remotePath, qint64 total);

    /**
     * 接上断点库。接上之后：任务每 4MB 落一次库，结束时按结果留档/销账。
     */
    void setResumeStore(ResumeStore *store) { m_store = store; }

    /**
     * 启动时恢复上次没下完的任务（M6 跨进程续传）。
     * 恢复出来的任务停在 Paused，不自动开跑；返回恢复的条数。
     */
    int restoreResumable();

    /// 把任务的当前断点写进库（完成/取消的走 forget）
    void recordCheckpoint(TransferTask *task);
    void forget(TransferTask *task);

    TransferTask *find(const QString &id) const;
    /// 按主机给的 transferId 找任务（摘要推送只带这个号，不带本端的任务 id）
    TransferTask *findByTransferId(const QString &transferId) const;
    /// 最后一个处于暂停/失败的下载任务（续传按钮用）
    TransferTask *lastResumable() const;

    /// 对所有现存任务执行 fn（全局设置变化后逐个刷新用）
    void forEachTask(const std::function<void(TransferTask *)> &fn) const;

    void clearFinished();

signals:
    /// 新任务已建立（含启动时从断点库恢复的），用于挂校验流程
    void taskCreated(Qdl::TransferTask *task);
    /// 任务结束（完成 / 失败 / 取消），用于刷新本机列表、弹提示
    void taskFinished(Qdl::TransferTask *task);
    /// 有任务结束但还留了 .qdpart，可以续传
    void taskResumable(Qdl::TransferTask *task);

private:
    TransferModel *m_model = nullptr;
    ResumeStore *m_store = nullptr;
    int m_seq = 0;
};

} // namespace Qdl

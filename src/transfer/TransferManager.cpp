#include "TransferManager.h"

#include "ResumeStore.h"
#include "TransferModel.h"
#include "TransferTask.h"

#include <QFileInfo>

namespace Qdl {

TransferManager::TransferManager(QObject *parent)
    : QObject(parent)
    , m_model(new TransferModel(this))
{
}

TransferTask *TransferManager::createDownload(const QString &name, const QString &remotePath,
                                              const QString &localPath, qint64 total)
{
    auto *task = new TransferTask(QStringLiteral("t-%1").arg(++m_seq),
                                  TransferDirection::Download, name, remotePath, localPath,
                                  total, this);

    connect(task, &TransferTask::changed, this, [this, task]() {
        m_model->notifyTaskChanged(task);
    });

    connect(task, &TransferTask::checkpoint, this, [this, task](qint64 done) {
        Q_UNUSED(done)
        recordCheckpoint(task);
    });

    connect(task, &TransferTask::finished, this, [this, task]() {
        emit taskFinished(task);
        // 留有 qdpart 且没传输完 → 断点要留档（跨进程也能接着下）。
        // 校验失败会把落地文件删掉，这时半成品已经不在，记录必须销掉，
        // 否则重启后会冒出一个永远续不上的任务。
        const bool resumable = (task->state() == TransferState::Paused
                                || task->state() == TransferState::Failed)
                            && (QFileInfo::exists(task->localPath() + QStringLiteral(".qdpart"))
                                || QFileInfo::exists(task->localPath()
                                                     + QStringLiteral(".qdpart.seg0")));
        if (resumable) {
            recordCheckpoint(task);
            emit taskResumable(task);
        } else {
            forget(task);
        }
    });

    m_model->insertTask(task);
    emit taskCreated(task);
    return task;
}

TransferTask *TransferManager::createUpload(const QString &name, const QString &localPath,
                                            const QString &remotePath, qint64 total)
{
    auto *task = new TransferTask(QStringLiteral("t-%1").arg(++m_seq),
                                  TransferDirection::Upload, name, remotePath, localPath,
                                  total, this);

    connect(task, &TransferTask::changed, this, [this, task]() {
        m_model->notifyTaskChanged(task);
    });

    // 上传的断点在主机侧，本机没有 .qdpart 可续：不落库，只刷新界面
    connect(task, &TransferTask::finished, this, [this, task]() {
        emit taskFinished(task);
    });

    m_model->insertTask(task);
    emit taskCreated(task);
    return task;
}

TransferTask *TransferManager::find(const QString &id) const
{
    for (QObject *child : children()) {
        auto *task = qobject_cast<TransferTask *>(child);
        if (task && task->id() == id)
            return task;
    }
    return nullptr;
}

TransferTask *TransferManager::findByTransferId(const QString &transferId) const
{
    if (transferId.isEmpty())
        return nullptr;
    for (QObject *child : children()) {
        auto *task = qobject_cast<TransferTask *>(child);
        if (task && task->transferId() == transferId)
            return task;
    }
    return nullptr;
}

TransferTask *TransferManager::lastResumable() const
{
    for (QObject *child : children()) {
        auto *task = qobject_cast<TransferTask *>(child);
        if (task && task->canResume())
            return task;
    }
    return nullptr;
}

void TransferManager::forEachTask(const std::function<void(TransferTask *)> &fn) const
{
    if (!fn)
        return;
    for (QObject *child : children()) {
        auto *task = qobject_cast<TransferTask *>(child);
        if (task)
            fn(task);
    }
}

void TransferManager::recordCheckpoint(TransferTask *task)
{
    if (!m_store || !m_store->isOpen() || !task)
        return;
    // 断点库只服务下载：上传的半成品在主机上，写进来会变成永远续不上的僵尸记录
    if (task->direction() != TransferDirection::Download)
        return;
    // 没有 peerId / fileKey 的断点没有续传价值：连"这是哪台主机的哪个文件"都说不清
    if (task->peerId().isEmpty() || task->fileKey().isEmpty())
        return;
    if (task->done() <= 0 || task->isFinished() && task->state() != TransferState::Paused)
        return;

    ResumeStore::Record record;
    record.peerId = task->peerId();
    record.remotePath = task->remotePath();
    record.peerName = task->peerName();
    record.localPath = task->localPath();
    record.name = task->name();
    record.fileKey = task->fileKey();
    record.total = task->total();
    record.done = task->done();
    m_store->upsert(record);
}

void TransferManager::forget(TransferTask *task)
{
    if (!m_store || !m_store->isOpen() || !task)
        return;
    if (task->direction() != TransferDirection::Download)
        return;   // 上传没有本机断点，别顺手删了同路径的下载记录
    if (task->peerId().isEmpty() || task->remotePath().isEmpty())
        return;
    m_store->remove(task->peerId(), task->remotePath());
}

int TransferManager::restoreResumable()
{
    if (!m_store || !m_store->isOpen())
        return 0;

    m_store->prune();   // 半成品都被删了的老记录先清掉，免得抽屉里冒僵尸

    int restored = 0;
    const QVector<ResumeStore::Record> records = m_store->list();
    for (const ResumeStore::Record &record : records) {
        const QFileInfo part(record.localPath + QStringLiteral(".qdpart"));
        qint64 done = part.exists() ? part.size() : 0;

        // 多段下载没有单一 .qdpart：进度是各分片大小之和。少认这一种形态，
        // 上次多段下的任务重启后就变成"点了继续没反应"。
        if (done <= 0) {
            for (int i = 0; i < 64; ++i) {
                const QFileInfo seg(record.localPath + QStringLiteral(".qdpart.seg%1").arg(i));
                if (!seg.exists())
                    break;
                done += seg.size();
            }
        }

        if (done <= 0) {
            m_store->remove(record.peerId, record.remotePath);
            continue;
        }

        // 已经有同路径的任务在跑（同一进程里重复恢复）就跳过
        bool duplicate = false;
        for (QObject *child : children()) {
            auto *task = qobject_cast<TransferTask *>(child);
            if (task && task->peerId() == record.peerId
                && task->remotePath() == record.remotePath) {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
            continue;

        auto *task = createDownload(record.name, record.remotePath, record.localPath,
                                    record.total > 0 ? record.total : done);
        task->restore(record.peerId, record.fileKey, done, record.total);
        ++restored;
        qInfo().noquote() << QStringLiteral("恢复断点：%1（已收 %2 字节）")
                                 .arg(record.name)
                                 .arg(done);
    }
    return restored;
}

void TransferManager::clearFinished()
{
    QVector<TransferTask *> doomed;
    for (QObject *child : children()) {
        auto *task = qobject_cast<TransferTask *>(child);
        if (task && task->isFinished())
            doomed.append(task);
    }

    for (TransferTask *task : std::as_const(doomed)) {
        m_model->removeTask(task);
        task->deleteLater();
    }
}

} // namespace Qdl

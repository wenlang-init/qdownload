#include "TransferModel.h"

#include <QCoreApplication>

namespace Qdl {

TransferModel::TransferModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int TransferModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_tasks.size();
}

int TransferModel::indexOf(TransferTask *task) const
{
    return int(m_tasks.indexOf(task));
}

QVariant TransferModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_tasks.size())
        return {};

    const TransferTask *task = m_tasks.at(index.row());
    switch (role) {
    case IdRole:         return task->id();
    case NameRole:       return task->name();
    case DirectionRole:  return task->directionCode();
    case StateRole:      return task->stateCode();
    case StateTextRole:  return task->stateText();
    case ProgressRole:   return task->progress();
    case PercentRole:    return task->percentText();
    case SizeTextRole:   return task->sizeText();
    case SpeedTextRole:  return task->speedText();
    case EtaTextRole:    return task->etaText();
    case ErrorRole:      return task->errorText();
    case ActiveRole:     return task->isActive();
    case FinishedRole:   return task->isFinished();
    case CanPauseRole:   return task->canPause();
    case CanResumeRole:  return task->canResume();
    case CanCancelRole:  return task->canCancel();
    case LocalPathRole:  return task->localPath();
    default:             return {};
    }
}

QHash<int, QByteArray> TransferModel::roleNames() const
{
    return {
        { IdRole,         "taskId" },
        { NameRole,       "name" },
        { DirectionRole,  "direction" },
        { StateRole,      "stateCode" },
        { StateTextRole,  "stateText" },
        { ProgressRole,   "progress" },
        { PercentRole,    "percentText" },
        { SizeTextRole,   "sizeText" },
        { SpeedTextRole,  "speedText" },
        { EtaTextRole,    "etaText" },
        { ErrorRole,      "errorText" },
        { ActiveRole,     "active" },
        { FinishedRole,   "finished" },
        { CanPauseRole,   "canPause" },
        { CanResumeRole,  "canResume" },
        { CanCancelRole,  "canCancel" },
        { LocalPathRole,  "localPath" }
    };
}

int TransferModel::activeCount() const
{
    int count = 0;
    for (const TransferTask *task : m_tasks) {
        if (task->isActive())
            ++count;
    }
    return count;
}

int TransferModel::finishedCount() const
{
    int count = 0;
    for (const TransferTask *task : m_tasks) {
        if (task->isFinished())
            ++count;
    }
    return count;
}

QString TransferModel::summary() const
{
    if (m_tasks.isEmpty())
        return QCoreApplication::translate("Transfer", "暂无任务");

    const int active = activeCount();
    const int finished = finishedCount();
    if (active > 0) {
        double speed = 0.0;
        for (const TransferTask *task : m_tasks) {
            if (task->isActive())
                speed += task->speed();
        }
        return QCoreApplication::translate("Transfer", "%1 个传输中 · %2")
            .arg(active)
            .arg(TransferFmt::speedText(speed));
    }
    return QCoreApplication::translate("Transfer", "%1 个任务 · %2 已完成").arg(m_tasks.size()).arg(finished);
}

void TransferModel::insertTask(TransferTask *task)
{
    if (!task)
        return;

    beginInsertRows(QModelIndex(), 0, 0);
    m_tasks.prepend(task);
    endInsertRows();
    emit changed();
}

void TransferModel::removeTask(TransferTask *task)
{
    const int row = indexOf(task);
    if (row < 0)
        return;

    beginRemoveRows(QModelIndex(), row, row);
    m_tasks.remove(row);
    endRemoveRows();
    emit changed();
}

void TransferModel::clearFinished()
{
    for (int row = m_tasks.size() - 1; row >= 0; --row) {
        if (!m_tasks.at(row)->isFinished())
            continue;
        beginRemoveRows(QModelIndex(), row, row);
        m_tasks.remove(row);
        endRemoveRows();
    }
    emit changed();
}

void TransferModel::notifyTaskChanged(TransferTask *task)
{
    const int row = indexOf(task);
    if (row < 0)
        return;

    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx);
    emit changed();   // 表头的合计速度也跟着变
}

} // namespace Qdl

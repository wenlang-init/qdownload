#pragma once

#include "TransferTask.h"

#include <QAbstractListModel>
#include <QVector>

namespace Qdl {

/**
 * 传输任务列表模型（抽屉里的那份数据）。
 *
 * 新任务插在第 0 行：用户刚点的那个文件必须出现在最上面，而不是滚到列表底部
 * 去找。任务对象归 TransferManager 所有，模型只持有指针。
 */
class TransferModel : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(int  count       READ rowCount NOTIFY changed)
    Q_PROPERTY(int  activeCount READ activeCount NOTIFY changed)
    Q_PROPERTY(int  finishedCount READ finishedCount NOTIFY changed)
    Q_PROPERTY(bool hasActive   READ hasActive NOTIFY changed)
    Q_PROPERTY(QString summary  READ summary NOTIFY changed)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        DirectionRole,
        StateRole,        // "transferring" / "paused" / ...
        StateTextRole,
        ProgressRole,
        PercentRole,
        SizeTextRole,
        SpeedTextRole,
        EtaTextRole,
        ErrorRole,
        ActiveRole,
        FinishedRole,
        CanPauseRole,
        CanResumeRole,
        CanCancelRole,
        LocalPathRole
    };
    Q_ENUM(Role)

    explicit TransferModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int activeCount() const;
    int finishedCount() const;
    bool hasActive() const { return activeCount() > 0; }
    QString summary() const;

    int indexOf(TransferTask *task) const;
    void insertTask(TransferTask *task);
    void removeTask(TransferTask *task);
    void clearFinished();
    void notifyTaskChanged(TransferTask *task);

signals:
    void changed();

private:
    QVector<TransferTask *> m_tasks;
};

} // namespace Qdl

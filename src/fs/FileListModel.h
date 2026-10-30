#pragma once

#include "FileEntry.h"

#include <QAbstractListModel>
#include <QVector>

namespace Qdl {

/**
 * 文件列表模型，本机页与远端页共用同一份实现与同一套 role。
 *
 * 两端的差别只在"怎么填"：
 *   - 本机：一次性扫描完，`reset()` 全量塞入
 *   - 远端：按页到达，`reset()` 第一页 + `appendPage()` 后续页（滚到底触发）
 *
 * 排序由服务端 / FileScanner 统一完成，模型**不再排序**——分页场景下
 * 客户端二次排序会让第二页的数据插到第一页前面。
 */
class FileListModel : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(int     count   READ rowCount  NOTIFY changed)
    Q_PROPERTY(QString path    READ path      NOTIFY pathChanged)
    Q_PROPERTY(int     total   READ total     NOTIFY changed)
    Q_PROPERTY(bool    hasMore READ hasMore   NOTIFY changed)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        IsDirRole,
        SizeRole,
        SizeTextRole,
        MtimeRole,
        MtimeTextRole,
        ExtensionRole,
        KindRole          // "dir" / 扩展名 / "file"，供 delegate 选图标
    };
    Q_ENUM(Role)

    explicit FileListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString path() const { return m_path; }
    int total() const { return m_total; }
    bool hasMore() const { return m_entries.size() < m_total; }
    const QVector<FileEntry> &entries() const { return m_entries; }
    FileEntry entryAt(int row) const;

    /// 换目录：清空并写入第一页
    void reset(const QString &path, const QVector<FileEntry> &entries, int total);
    /// 追加下一页；路径不一致时自动忽略（防止上一目录的迟到响应串进来）
    void appendPage(const QString &path, int offset, const QVector<FileEntry> &entries, int total);
    void clear();

    /// QML 侧取一行完整信息（点击进入目录 / 未来发起下载都要用）
    Q_INVOKABLE QVariantMap get(int row) const;

signals:
    void changed();
    void pathChanged();

private:
    QVector<FileEntry> m_entries;
    QString m_path;
    int m_total = 0;
};

} // namespace Qdl

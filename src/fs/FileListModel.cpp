#include "FileListModel.h"

namespace Qdl {

FileListModel::FileListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int FileListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_entries.size();
}

QVariant FileListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size())
        return {};

    const FileEntry &entry = m_entries.at(index.row());
    switch (role) {
    case NameRole:      return entry.name;
    case IsDirRole:     return entry.isDir;
    case SizeRole:      return entry.size;
    case SizeTextRole:  return entry.sizeText();
    case MtimeRole:     return entry.mtime;
    case MtimeTextRole: return entry.mtimeText();
    case ExtensionRole: return entry.extension();
    case KindRole:
        if (entry.isDir)
            return QStringLiteral("dir");
        return entry.extension().isEmpty() ? QStringLiteral("file") : entry.extension();
    default:
        return {};
    }
}

QHash<int, QByteArray> FileListModel::roleNames() const
{
    return {
        { NameRole,      "name" },
        { IsDirRole,     "isDir" },
        { SizeRole,      "size" },
        { SizeTextRole,  "sizeText" },
        { MtimeRole,     "mtime" },
        { MtimeTextRole, "mtimeText" },
        { ExtensionRole, "extension" },
        { KindRole,      "kind" },
    };
}

FileEntry FileListModel::entryAt(int row) const
{
    if (row < 0 || row >= m_entries.size())
        return {};
    return m_entries.at(row);
}

void FileListModel::reset(const QString &path, const QVector<FileEntry> &entries, int total)
{
    beginResetModel();
    m_entries = entries;
    m_total = qMax(total, entries.size());
    const bool pathChangedValue = m_path != path;
    m_path = path;
    endResetModel();

    if (pathChangedValue)
        emit pathChanged();
    emit changed();
}

void FileListModel::appendPage(const QString &path, int offset, const QVector<FileEntry> &entries,
                               int total)
{
    // 迟到的上一目录响应直接丢弃：否则会把另一个目录的文件混进当前列表
    if (path != m_path) {
        return;
    }
    if (offset != m_entries.size()) {
        // 不是紧接着当前末尾的页，说明中间丢了一页；丢掉比留下空洞安全
        return;
    }
    if (entries.isEmpty()) {
        m_total = qMax(total, m_entries.size());
        emit changed();
        return;
    }

    beginInsertRows(QModelIndex(), m_entries.size(), m_entries.size() + entries.size() - 1);
    m_entries += entries;
    m_total = qMax(total, m_entries.size());
    endInsertRows();

    emit changed();
}

void FileListModel::clear()
{
    if (m_entries.isEmpty() && m_path.isEmpty() && m_total == 0)
        return;

    beginResetModel();
    m_entries.clear();
    m_total = 0;
    const bool pathChangedValue = !m_path.isEmpty();
    m_path.clear();
    endResetModel();

    if (pathChangedValue)
        emit pathChanged();
    emit changed();
}

QVariantMap FileListModel::get(int row) const
{
    QVariantMap map;
    if (row < 0 || row >= m_entries.size())
        return map;

    const FileEntry &entry = m_entries.at(row);
    map.insert(QStringLiteral("name"), entry.name);
    map.insert(QStringLiteral("isDir"), entry.isDir);
    map.insert(QStringLiteral("size"), entry.size);
    map.insert(QStringLiteral("sizeText"), entry.sizeText());
    map.insert(QStringLiteral("mtime"), entry.mtime);
    map.insert(QStringLiteral("mtimeText"), entry.mtimeText());
    map.insert(QStringLiteral("extension"), entry.extension());
    return map;
}

} // namespace Qdl

#include "FileEntry.h"

#include <QDateTime>

namespace Qdl {

namespace {
constexpr qint64 kKb = 1024;
constexpr qint64 kMb = 1024 * 1024;
constexpr qint64 kGb = 1024LL * 1024 * 1024;
} // namespace

QJsonObject FileEntry::toJson() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("name"), name);
    obj.insert(QStringLiteral("isDir"), isDir);
    obj.insert(QStringLiteral("size"), double(size));   // JSON 数字是 double，qint64 直转会编译失败
    obj.insert(QStringLiteral("mtime"), double(mtime));
    return obj;
}

FileEntry FileEntry::fromJson(const QJsonObject &obj)
{
    FileEntry entry;
    entry.name = obj.value(QStringLiteral("name")).toString();
    entry.isDir = obj.value(QStringLiteral("isDir")).toBool();
    entry.size = qint64(obj.value(QStringLiteral("size")).toDouble());
    entry.mtime = qint64(obj.value(QStringLiteral("mtime")).toDouble());
    return entry;
}

QString FileEntry::sizeText() const
{
    if (isDir)
        return QStringLiteral("—");
    if (size < kKb)
        return QStringLiteral("%1 B").arg(size);
    if (size < kMb)
        return QStringLiteral("%1 KB").arg(double(size) / kKb, 0, 'f', 1);
    if (size < kGb)
        return QStringLiteral("%1 MB").arg(double(size) / kMb, 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(double(size) / kGb, 0, 'f', 2);
}

QString FileEntry::mtimeText() const
{
    if (mtime <= 0)
        return {};
    return QDateTime::fromSecsSinceEpoch(mtime).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString FileEntry::extension() const
{
    if (isDir)
        return {};
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot == name.size() - 1)
        return {};
    return name.mid(dot + 1).toLower();
}

} // namespace Qdl

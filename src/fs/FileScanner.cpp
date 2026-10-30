#include "FileScanner.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QVector>
#include <algorithm>

namespace Qdl {

namespace {

constexpr qint64 kFingerprintChunk = 1024 * 1024;   // 首尾各读 1MB

struct EntryLess
{
    bool operator()(const FileEntry &a, const FileEntry &b) const
    {
        if (a.isDir != b.isDir)
            return a.isDir;   // 目录恒在前
        const int cmp = QString::localeAwareCompare(a.name, b.name);
        if (cmp != 0)
            return cmp < 0;
        return a.name < b.name;   // 本地化比较相等时兜底，保证排序稳定
    }
};

} // namespace

bool FileScanner::list(const QString &absoluteDir, int offset, int limit,
                       QVector<FileEntry> *page, int *total, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    if (!page || !total)
        return fail(QStringLiteral("内部错误：输出参数为空"));

    page->clear();
    *total = 0;

    QDir dir(absoluteDir);
    if (!dir.exists())
        return fail(QStringLiteral("目录不存在"));

    const QFileInfoList infos =
        dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);

    QVector<FileEntry> all;
    all.reserve(infos.size());

    for (const QFileInfo &info : infos) {
        const QString name = info.fileName();

        // 半成品与隐藏的系统占位符不上列表。
        // 用 contains 而不是 endsWith：.qdpart.key（上传指纹侧文件）也要一起藏掉
        if (name.contains(kPartSuffix, Qt::CaseInsensitive))
            continue;
        if (name == QLatin1String("desktop.ini") || name == QLatin1String(".DS_Store"))
            continue;

        FileEntry entry;
        entry.name = name;
        entry.isDir = info.isDir();
        if (!entry.isDir) {
            entry.size = info.size();
        }
        entry.mtime = info.lastModified().toSecsSinceEpoch();
        all.append(entry);
    }

    std::sort(all.begin(), all.end(), EntryLess());
    *total = all.size();

    const int from = qBound(0, offset, all.size());
    const int count = qBound(0, limit, kMaxLimit);
    const int to = qMin(all.size(), from + count);
    for (int i = from; i < to; ++i)
        page->append(all.at(i));

    return true;
}

QString FileScanner::fileKey(const QString &absolutePath, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };

    QFileInfo info(absolutePath);
    if (!info.exists() || !info.isFile())
        return fail(QStringLiteral("文件不存在：%1").arg(absolutePath));

    QFile file(absolutePath);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("无法读取：%1").arg(file.errorString()));

    QCryptographicHash hash(QCryptographicHash::Sha256);

    const qint64 size = info.size();
    const qint64 mtime = info.lastModified().toSecsSinceEpoch();
    // Qt 6.4 起 addData(const char*, len) 已弃用，统一走 QByteArrayView 重载，
    // 否则 Android（clang）编译时刷两条 deprecation 警告。
    hash.addData(QByteArrayView(reinterpret_cast<const char *>(&size), sizeof(size)));
    hash.addData(QByteArrayView(reinterpret_cast<const char *>(&mtime), sizeof(mtime)));

    const QByteArray head = file.read(kFingerprintChunk);
    hash.addData(head);

    if (size > kFingerprintChunk * 2) {
        if (file.seek(size - kFingerprintChunk)) {
            const QByteArray tail = file.read(kFingerprintChunk);
            hash.addData(tail);
        }
    }

    return QStringLiteral("k-") + QString::fromLatin1(hash.result().toHex().left(20));
}

QString FileScanner::sha256(const QString &absolutePath, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };

    QFileInfo info(absolutePath);
    if (!info.exists() || !info.isFile())
        return fail(QStringLiteral("文件不存在：%1").arg(absolutePath));

    QFile file(absolutePath);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("无法读取：%1").arg(file.errorString()));

    QCryptographicHash hash(QCryptographicHash::Sha256);
    // 一次 1MB：再大只是让内存峰值变难看，再小则 syscall 次数翻倍
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(kFingerprintChunk);
        if (chunk.isEmpty())
            break;
        hash.addData(chunk);
    }
    return QString::fromLatin1(hash.result().toHex());
}

} // namespace Qdl

#include "SafePath.h"

#include <QDir>
#include <QFileInfo>

namespace Qdl {

namespace {

#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

/// 统一用 '/'，去掉尾部斜杠（根除外）
QString tidy(const QString &path)
{
    QString p = path;
    while (p.size() > 1 && p.endsWith(QLatin1Char('/')))
        p.chop(1);
    return p.isEmpty() ? QStringLiteral("/") : p;
}

bool isInsideRoot(const QString &rootCanonical, const QString &target)
{
    if (target.compare(rootCanonical, kPathCase) == 0)
        return true;

    QString prefix = rootCanonical;
    if (!prefix.endsWith(QLatin1Char('/')))
        prefix.append(QLatin1Char('/'));
    return target.startsWith(prefix, kPathCase);
}

} // namespace

QString SafePath::normalize(const QString &path, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };

    if (path.isEmpty())
        return QStringLiteral("/");

    // 反斜杠一律拒绝而不是当成 '/'：Windows 客户端若把本地路径发上来，
    // 静默转换会让 "C:\Users" 变成 "/C:/Users" 这种四不像，不如直接报错。
    if (path.contains(QLatin1Char('\\')))
        return fail(QStringLiteral("路径不能包含反斜杠"));

    for (const QChar ch : path) {
        if (ch.unicode() < 0x20)
            return fail(QStringLiteral("路径包含控制字符"));
    }

    QStringList parts;
    QString current;
    for (int i = 0; i <= path.size(); ++i) {
        const bool atEnd = (i == path.size());
        const QChar ch = atEnd ? QLatin1Char('/') : path.at(i);
        if (ch == QLatin1Char('/')) {
            if (current.isEmpty() || current == QLatin1String(".")) {
                // 空段与 "." 直接忽略（"//a" 与 "/./a" 都等价于 "/a"）
            } else if (current == QLatin1String("..")) {
                return fail(QStringLiteral("路径不能包含 .."));
            } else if (current.contains(QLatin1Char(':'))) {
                return fail(QStringLiteral("路径不能包含盘符"));
            } else if (current.size() > 200) {
                return fail(QStringLiteral("路径段过长"));
            } else {
                parts.append(current);
            }
            current.clear();
        } else {
            current.append(ch);
        }
    }

    if (parts.isEmpty())
        return QStringLiteral("/");
    return QLatin1Char('/') + parts.join(QLatin1Char('/'));
}

QString SafePath::resolve(const QString &root, const QString &virtualPath, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };

    if (root.isEmpty())
        return fail(QStringLiteral("共享根未设置"));

    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    if (canonicalRoot.isEmpty())
        return fail(QStringLiteral("共享根不存在或不可访问"));

    const QString normalized = normalize(virtualPath, error);
    if (normalized.isEmpty())
        return QString();

    QString absolute = canonicalRoot;
    if (normalized != QLatin1String("/"))
        absolute += normalized;   // normalized 自带前导 '/'

    const QFileInfo info(absolute);
    if (!info.exists())
        return fail(QStringLiteral("路径不存在：%1").arg(normalized));

    // 关键一步：canonicalFilePath 会把符号链接解到最终目标，
    // 指向根外的软链因此在这里被拦下来。
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty())
        return fail(QStringLiteral("无法解析路径：%1").arg(normalized));

    if (!isInsideRoot(canonicalRoot, canonical))
        return fail(QStringLiteral("路径越界，已拒绝"));

    return canonical;
}

QString SafePath::toVirtual(const QString &root, const QString &absolutePath)
{
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    if (canonicalRoot.isEmpty() || absolutePath.isEmpty())
        return QString();

    const QString target = QFileInfo(absolutePath).canonicalFilePath();
    if (target.isEmpty() || !isInsideRoot(canonicalRoot, target))
        return QString();

    if (target.compare(canonicalRoot, kPathCase) == 0)
        return QStringLiteral("/");

    QString relative = target.mid(canonicalRoot.size());
    if (!relative.startsWith(QLatin1Char('/')))
        relative.prepend(QLatin1Char('/'));
    return tidy(relative);
}

QString SafePath::parent(const QString &virtualPath)
{
    QString path = normalize(virtualPath);
    if (path.isEmpty() || path == QLatin1String("/"))
        return QStringLiteral("/");

    const int slash = path.lastIndexOf(QLatin1Char('/'));
    if (slash <= 0)
        return QStringLiteral("/");
    return path.left(slash);
}

QString SafePath::child(const QString &virtualPath, const QString &name)
{
    if (!isValidName(name))
        return QString();

    QString base = normalize(virtualPath);
    if (base.isEmpty())
        return QString();
    if (base == QLatin1String("/"))
        return QLatin1Char('/') + name;
    return base + QLatin1Char('/') + name;
}

bool SafePath::isValidName(const QString &name)
{
    if (name.isEmpty() || name.size() > 200)
        return false;
    if (name == QLatin1String(".") || name == QLatin1String(".."))
        return false;
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
        return false;
    if (name.contains(QLatin1Char(':')))
        return false;
    for (const QChar ch : name) {
        if (ch.unicode() < 0x20)
            return false;
    }
    return true;
}

QStringList SafePath::segments(const QString &virtualPath)
{
    const QString path = normalize(virtualPath);
    if (path.isEmpty() || path == QLatin1String("/"))
        return {};
    return path.mid(1).split(QLatin1Char('/'), Qt::SkipEmptyParts);
}

} // namespace Qdl

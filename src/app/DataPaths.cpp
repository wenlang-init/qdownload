#include "DataPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryFile>

namespace {

QString g_recordsRoot;
QString g_settingsFile;
Qdl::DataPaths::Mode g_mode = Qdl::DataPaths::Mode::Installed;

// 系统给的应用数据目录。Android 上是沙箱内的 files 目录，桌面上是 %LOCALAPPDATA%/组织/应用
QString systemRoot()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.qdownload");
    return base;
}

// 真的能写才算。Windows 的 QFileInfo::isWritable() 只看「只读」属性、不看 ACL，
// 放在这里判 would 会把 Program Files 判成可写（Program Files 恰好没有只读属性，
// 但普通进程确实建不了目录），所以直接 mkpath + 落一个临时文件试
bool ensureWritable(const QString &dir)
{
    if (!QDir().mkpath(dir))
        return false;
    QTemporaryFile probe(QDir(dir).filePath(QStringLiteral(".write-probe-XXXXXX")));
    if (!probe.open())
        return false;
    probe.close();
    return true;
}

} // namespace

void Qdl::DataPaths::init()
{
    const QString portableRoot = QDir(QCoreApplication::applicationDirPath())
                                     .filePath(QStringLiteral("data"));

    if (ensureWritable(portableRoot)) {
        g_recordsRoot = portableRoot;
        g_settingsFile = QDir(portableRoot).filePath(QStringLiteral("settings.ini"));
        g_mode = Qdl::DataPaths::Mode::Portable;
        return;
    }

    // 程序目录不可写：装在 Program Files 之类受保护的位置，或 Android（apk 目录只读）。
    // 这时候仍然要能跑，退回系统数据目录
    g_recordsRoot = systemRoot();
    g_settingsFile.clear();
    g_mode = Qdl::DataPaths::Mode::Installed;
    qWarning().noquote() << QCoreApplication::translate(
                                "DataPaths",
                                "程序目录 %1 不可写，数据改用系统位置 %2")
                                .arg(QCoreApplication::applicationDirPath(), g_recordsRoot);
}

QString Qdl::DataPaths::recordsRoot()
{
    return g_recordsRoot;
}

QString Qdl::DataPaths::logDirectory()
{
    return QDir(g_recordsRoot).filePath(QStringLiteral("logs"));
}

QString Qdl::DataPaths::settingsFilePath()
{
    return g_settingsFile;
}

Qdl::DataPaths::Mode Qdl::DataPaths::mode()
{
    return g_mode;
}

bool Qdl::DataPaths::portable()
{
    return g_mode == Qdl::DataPaths::Mode::Portable;
}

QString Qdl::DataPaths::summary()
{
    return QCoreApplication::translate(
        "DataPaths", "%1: %2")
        .arg(portable() ? QCoreApplication::translate("DataPaths", "便携模式（数据跟着程序走）")
                        : QCoreApplication::translate("DataPaths", "安装模式（程序目录不可写）"),
             g_recordsRoot);
}

void Qdl::DataPaths::migrateLegacy(const QStringList &fileNames)
{
    if (!portable())
        return;

    const QString legacy = systemRoot();
    if (legacy == g_recordsRoot)
        return;

    int moved = 0;
    for (const QString &name : fileNames) {
        const QString from = QDir(legacy).filePath(name);
        const QString to = QDir(g_recordsRoot).filePath(name);
        if (!QFileInfo::exists(from) || QFileInfo::exists(to))
            continue;
        if (QFile::copy(from, to))
            ++moved;
    }

    if (moved > 0)
        qInfo().noquote()
            << QCoreApplication::translate("DataPaths", "已从旧位置复制 %1 个记录文件到 %2")
                   .arg(moved)
                   .arg(g_recordsRoot);
}

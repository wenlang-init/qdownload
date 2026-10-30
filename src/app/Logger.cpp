#include "Logger.h"

#include "DataPaths.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QTextStream>

#include <cstdio>

namespace {

constexpr qint64 kMaxLogBytes = 5 * 1024 * 1024;

QMutex g_mutex;
QString g_logPath;
QString g_fileTag;

const char *levelTag(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return "DEBUG";
    case QtInfoMsg:     return "INFO ";
    case QtWarningMsg:  return "WARN ";
    case QtCriticalMsg: return "ERROR";
    case QtFatalMsg:    return "FATAL";
    }
    return "?????";
}

void rotateIfNeeded(const QString &path)
{
    QFileInfo info(path);
    if (!info.exists() || info.size() < kMaxLogBytes)
        return;

    const QString backup = path + QStringLiteral(".1");
    QFile::remove(backup);
    QFile::rename(path, backup);
}

QString todayPath()
{
    const QString dir = Logger::logDirectory();
    QDir().mkpath(dir);
    const QString suffix = g_fileTag.isEmpty() ? QString()
                                               : QStringLiteral("-") + g_fileTag;
    const QString name = QStringLiteral("qdownload-%1%2.log")
                             .arg(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")), suffix);
    return QDir(dir).filePath(name);
}

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    const QString category = QString::fromLatin1(context.category ? context.category : "default");
    const QString line = QStringLiteral("%1 [%2] %3: %4")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
                                  QString::fromLatin1(levelTag(type)),
                                  category,
                                  msg);

    // 控制台：直接写 stderr。不能依赖“上一个 handler”——首次安装时它是空的，
    // 那样控制台就什么都不打印了（M1 调试吃过的亏）。
    fprintf(stderr, "%s\n", line.toUtf8().constData());
    fflush(stderr);

    QMutexLocker locker(&g_mutex);
    if (g_logPath.isEmpty())
        return;

    QFile file(g_logPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;

    QTextStream stream(&file);
    stream << line << '\n';
}

} // namespace

QString Logger::logDirectory()
{
    // 与其它记录文件同在一个根底下：拷走 exe 所在的整个目录，日志也跟着走
    return Qdl::DataPaths::logDirectory();
}

QString Logger::logFilePath()
{
    QMutexLocker locker(&g_mutex);
    return g_logPath;
}

void Logger::install(const QString &fileTag)
{
    {
        QMutexLocker locker(&g_mutex);
        if (!g_logPath.isEmpty())
            return;
        g_fileTag = fileTag;
        g_logPath = todayPath();
    }

    rotateIfNeeded(logFilePath());
    qInstallMessageHandler(messageHandler);
}

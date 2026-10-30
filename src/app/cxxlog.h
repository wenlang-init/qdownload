
#pragma once

// #ifndef _CXXLOG_H
// # define _CXXLOG_H

#include <list>
#include <thread>
#include <fstream>
#include <mutex>

class CxxLog {
public:

    enum LOG_TYPE {
        LOG_TYPE_DEBUG = 0,
        LOG_TYPE_INFO,
        LOG_TYPE_WARRING,
        LOG_TYPE_CRITICAL,
        LOG_TYPE_FATAL,
    };

    static CxxLog& getInstance() {
        static CxxLog instance;

        return instance;
    }

    virtual ~CxxLog();

    // 设置输出编码为UTF-8或主机默认编码，默认UTF-8
    void setOutputEncodeIsLocal(bool isLocal = true) {
        isLocalOutput = isLocal;
    }

    bool isOutputEncodeIsLocal() {
        return isLocalOutput;
    }

#ifdef _WIN32
    static void setConsoleEncode2Gbk() {
        system("chcp.com 936");
    }

    static void setConsoleEncode2Utf8() {
        system("chcp.com 65001");
    }

#endif // ifdef _WIN32
    void stop();

    bool initLog(const std::string & logDir,
                 const unsigned int& flushMs,
                 const unsigned int& fileLogMaxSize);

    void setLogLevel(const LOG_TYPE& logLevel) {
        m_logLevel = logLevel;
    }

    void setFileLogLevel(const LOG_TYPE& fileLogLevel) {
        m_fileLogLevel = fileLogLevel;
    }

    void setPrint(bool isPrinting) {
        m_isPrinting = isPrinting;
    }

    void setColorLog(bool isColorLog) {
        m_isColorLog = isColorLog;
    }

    void addLog(const LOG_TYPE   & logType,
                const char        *function,
                const char        *file,
                const int          line,
                const std::string& logMessage);
    void addLogFormat(const LOG_TYPE& logType,
                      const char     *function,
                      const char     *file,
                      const int       line,
                      const char     *format,
                      ...);

private:

    CxxLog() = default;
    bool initLog();
    void logThreadFunc();
    bool creatLogFile();
    bool isInit = false;

    bool isLocalOutput = true;

    // bool isFirst = true;

    std::thread m_thread;
    std::mutex m_mutex;                               // 互斥锁保护日志队列
    std::list<std::string>m_logQueue;                 // 日志队列
    bool m_stopThread = false;

    bool m_isPrinting = true;                         // 是否打印日志
    LOG_TYPE m_logLevel = LOG_TYPE_DEBUG;             // 日志输出级别
    LOG_TYPE m_fileLogLevel = LOG_TYPE_DEBUG;         // 日志文件保存级别
    bool m_isColorLog = false;                        // 是否使用彩色日志

    unsigned int m_flushMs = 1000;                    // 日志保存刷新周期
    unsigned int m_fileLogMaxSize = 10 * 1024 * 1024; // 日志文件大小
    std::string m_logDir = "./log";                   // 日志路径

    std::string m_currentLogFile;                     // 当前日志文件
    int m_currentLogFileSize = 0;                     // 当前日志文件大小
    int m_logFileIndex = 0;                           // 日志文件索引
    int m_currentDay = 0;                             // 当前日志文件的日期日
    unsigned long long m_currentTime;                 // 当前时间

    std::ofstream m_logFileStream;                    // 日志文件流
};

#define WRITE_LOG_CXX_STRING(level, logMessage)    \
    do {                                           \
        CxxLog::getInstance().addLog(level,        \
                                     __FUNCTION__, \
                                     __FILE__,     \
                                     __LINE__,     \
                                     logMessage);  \
    } while (0)
#define DEBUG_LOG_CXX_STRING(logMessage) WRITE_LOG_CXX_STRING( \
        CxxLog::LOG_TYPE_DEBUG, logMessage)
#define WARRING_LOG_CXX_STRING(logMessage) WRITE_LOG_CXX_STRING( \
        CxxLog::LOG_TYPE_WARRING, logMessage)
#define CRITICAL_LOG_CXX_STRING(logMessage) WRITE_LOG_CXX_STRING( \
        CxxLog::LOG_TYPE_CRITICAL, logMessage)
#define FATAL_LOG_CXX_STRING(logMessage) WRITE_LOG_CXX_STRING( \
        CxxLog::LOG_TYPE_FATAL, logMessage)
#define INFO_LOG_CXX_STRING(logMessage) WRITE_LOG_CXX_STRING( \
        CxxLog::LOG_TYPE_INFO, logMessage)

#define WRITE_LOG_CXX(level, __format, ...)                 \
    do {                                                    \
        CxxLog::getInstance().addLogFormat(level,           \
                                           __FUNCTION__,    \
                                           __FILE__,        \
                                           __LINE__,        \
                                           __format,        \
                                           ## __VA_ARGS__); \
    } while (0)
#define DEBUG_LOG_CXX(__format, ...) WRITE_LOG_CXX( \
        CxxLog::LOG_TYPE_DEBUG, __format, ## __VA_ARGS__)
#define WARRING_LOG_CXX(__format, ...) WRITE_LOG_CXX( \
        CxxLog::LOG_TYPE_WARRING, __format, ## __VA_ARGS__)
#define CRITICAL_LOG_CXX(__format, ...) WRITE_LOG_CXX( \
        CxxLog::LOG_TYPE_CRITICAL, __format, ## __VA_ARGS__)
#define FATAL_LOG_CXX(__format, ...) WRITE_LOG_CXX( \
        CxxLog::LOG_TYPE_FATAL, __format, ## __VA_ARGS__)
#define INFO_LOG_CXX(__format, ...) WRITE_LOG_CXX( \
        CxxLog::LOG_TYPE_INFO, __format, ## __VA_ARGS__)

#define CUSTOMMSGHANDLER_LOG_CXX                                               \
    [] (QtMsgType type, const QMessageLogContext& context, const QString& msg) \
    {                                                                          \
        std::string logMessage;                                                \
        if (!CxxLog::getInstance().isOutputEncodeIsLocal()) {                  \
            logMessage = msg.toUtf8().constData();                             \
        } else {                                                               \
            logMessage = msg.toLocal8Bit().constData();                        \
        }                                                                      \
        logMessage += "\n";                                                    \
        switch (type)                                                          \
        {                                                                      \
        case QtDebugMsg:                                                       \
        {                                                                      \
            CxxLog::getInstance().addLog(CxxLog::LOG_TYPE_DEBUG,               \
                                         context.function,                     \
                                         context.file,                         \
                                         context.line,                         \
                                         logMessage);                          \
            break;                                                             \
        }                                                                      \
        case QtInfoMsg:                                                        \
        {                                                                      \
            CxxLog::getInstance().addLog(CxxLog::LOG_TYPE_INFO,                \
                                         context.function,                     \
                                         context.file,                         \
                                         context.line,                         \
                                         logMessage);                          \
            break;                                                             \
        }                                                                      \
        case QtWarningMsg:                                                     \
        {                                                                      \
            CxxLog::getInstance().addLog(CxxLog::LOG_TYPE_WARRING,             \
                                         context.function,                     \
                                         context.file,                         \
                                         context.line,                         \
                                         logMessage);                          \
            break;                                                             \
        }                                                                      \
        case QtCriticalMsg:                                                    \
        {                                                                      \
            CxxLog::getInstance().addLog(CxxLog::LOG_TYPE_CRITICAL,            \
                                         context.function,                     \
                                         context.file,                         \
                                         context.line,                         \
                                         logMessage);                          \
            break;                                                             \
        }                                                                      \
        case QtFatalMsg:                                                       \
        {                                                                      \
            CxxLog::getInstance().addLog(CxxLog::LOG_TYPE_FATAL,               \
                                         context.function,                     \
                                         context.file,                         \
                                         context.line,                         \
                                         logMessage);                          \
            break;                                                             \
        }                                                                      \
        default:                                                               \
            break;                                                             \
        }                                                                      \
    }

#define REDIRECT_QTMESSAGE_LOG_CXX(handler)                   \
    do {                                                      \
        if (handler) {                                        \
            qInstallMessageHandler(handler);                  \
        } else {                                              \
            qInstallMessageHandler(CUSTOMMSGHANDLER_LOG_CXX); \
        }                                                     \
    } while (0)

// #endif

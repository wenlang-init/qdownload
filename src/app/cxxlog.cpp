#include "cxxlog.h"
#include <iostream>
#include <filesystem>
#include <chrono>
#include <stdarg.h>
#include <string>
#include <cstring>
#include <cerrno>
#include <vector>
#include <algorithm>

#if defined(WIN32) || defined(WIN64)
# include <windows.h>
#endif // if defined(WIN32) || defined(WIN64)
#if defined(__GNUC__)
# include <unistd.h> // getpid();
#endif // if defined(__GNUC__)
#if defined(__unix__)
# include <errno.h>
# include <sys/syscall.h>
#endif // if defined(__unix__)

inline std::tm localtime_safe(std::time_t timer) {
    std::tm bt{};

#if defined(_MSC_VER)
    localtime_s(&bt, &timer); // MSVC
#elif defined(__unix__) || defined(__APPLE__)
    localtime_r(&timer, &bt); // POSIX
#else // if defined(_MSC_VER)

    // 回退方案（警告：非线程安全！）
    bt = *std::localtime(&timer);
#endif // if defined(_MSC_VER)
    return bt;
}

std::string strerror_safe(int errnum) {
    char buffer[256];

#if defined(_WIN32)

    // Windows 使用 strerror_s
    if (strerror_s(buffer, sizeof(buffer), errnum) == 0) {
        return std::string(buffer);
    }
#elif defined(__unix__) || defined(__APPLE__)

    // POSIX 平台使用 strerror_r
    # if defined(__GLIBC__) && defined(_GNU_SOURCE)

    // GNU 版本
    char *msg = strerror_r(errnum, buffer, sizeof(buffer));
    return std::string(msg ? msg : "Unknown error");

    # else // if defined(__GLIBC__) && defined(_GNU_SOURCE)

    // XSI 版本
    if (strerror_r(errnum, buffer, sizeof(buffer)) == 0) {
        return std::string(buffer);
    }
    # endif // if defined(__GLIBC__) && defined(_GNU_SOURCE)
#else // if defined(_WIN32)

    // 回退方案（非线程安全）
    return std::string(std::strerror(errnum));

#endif // if defined(_WIN32)
    return "Unknown error";
}

// #define USE_COLOR_LOG

#define CLEAR "\e[2j"            // 清屏
#define CLRLINE "\r\e[K"         // or "\e[K\r" 清除从光标到行尾的内容

#define RESET       "\033[0m"    // 恢复终端默认属性
#define BLACK       "\033[30m"   /* Black 黑色 */
#define RED         "\033[31m"   /* Red 红色 */
#define GREEN       "\033[32m"   /* Green 绿色 */
#define YELLOW      "\033[33m"   /* Yellow 黄的 */
#define BLUE        "\033[34m"   /* Blue 蓝色 */
#define MAGENTA     "\033[35m"   /* Magenta 品红 */
#define CYAN        "\033[36m"   /* Cyan 青色 */
#define WHITE       "\033[37m"   /* White 白色 */
#define BOLDBLACK   "\033[1;30m" /* Bold Black 粗黑 */
#define BOLDRED     "\033[1;31m" /* Bold Red 粗红 */
#define BOLDGREEN   "\033[1;32m" /* Bold Green */
#define BOLDYELLOW  "\033[1;33m" /* Bold Yellow */
#define BOLDBLUE    "\033[1;34m" /* Bold Blue */
#define BOLDMAGENTA "\033[1;35m" /* Bold Magenta */
#define BOLDCYAN    "\033[1;36m" /* Bold Cyan */
#define BOLDWHITE   "\033[1;37m" /* Bold White */

// echo -e RED"这是红色文本"RESET

static inline void printMessage(const CxxLog::LOG_TYPE& logType,
                                const char             *function,
                                const char             *file,
                                const int               line,
                                const char             *format,
                                ...)
{
    va_list arg;

    va_start(arg, format);
    int size = vsnprintf(NULL, 0, format, arg);
    va_end(arg);

    if (size < 0) {
        printf("vsnprintf failed:%s\n", strerror_safe(errno).c_str());
        fflush(stdout);
        return;
    }

    if (size == 0) {
        return;
    }

    // auto ThreadId = std::this_thread::get_id();
    unsigned long ProcessId = 0;
    unsigned long ThreadId = 0;

#if defined(WIN32) || defined(WIN64)
    ProcessId = GetCurrentProcessId();
    ThreadId = GetCurrentThreadId();
#endif // if defined(WIN32) || defined(WIN64)
#if defined(__unix__)
    ProcessId = getpid();
    ThreadId = syscall(SYS_gettid);
#endif // if defined(__unix__)

    std::string logString;

    const char *typemsg;
    std::string str;

    switch (logType) {
    case CxxLog::LOG_TYPE_DEBUG:
        typemsg = "Debug   ";
        str = str + YELLOW;
        break;

    case CxxLog::LOG_TYPE_WARRING:
        typemsg = "Warring ";
        str = str + CYAN;
        break;

    case CxxLog::LOG_TYPE_CRITICAL:
        typemsg = "Critical";
        str = str + RED;
        break;

    case CxxLog::LOG_TYPE_FATAL:
        typemsg = "Fatal   ";
        str = str + MAGENTA;
        break;

    case CxxLog::LOG_TYPE_INFO:
        typemsg = "Info    ";
        str = str + GREEN;
        break;


    default:
        typemsg = "        ";
        break;
    }
    str = str + typemsg;

    std::vector<char> vec(size + 1);
    va_start(arg, format);
    vsnprintf(vec.data(), size + 1, format, arg);
    va_end(arg);

    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm     ntm = localtime_safe(now_time_t);
    std::tm    *now_tm = &ntm;

    // std::tm    *now_tm = std::localtime(&now_time_t);

    // std::chrono::milliseconds ms =
    //     std::chrono::duration_cast<std::chrono::milliseconds>(
    //         now.time_since_epoch()) % 1000;
    // std::chrono::microseconds cs =
    //     std::chrono::duration_cast<std::chrono::microseconds>(
    //         now.time_since_epoch()) % 1000000;
    std::chrono::nanoseconds ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()) % 1000000000;
    std::string strms = std::to_string(ns.count() / 1000000);

    if (strms.size() < 3) {
        strms = std::string(3 - strms.size(), '0') + strms;
    }
    std::string strcs = std::to_string(ns.count() / 1000 % 1000);

    if (strcs.size() < 3) {
        strcs = std::string(3 - strcs.size(), '0') + strcs;
    }
    std::string strns = std::to_string(ns.count() % 1000);

    if (strns.size() < 3) {
        strns = std::string(3 - strns.size(), '0') + strns;
    }
    char dateStr[128];
    std::strftime(dateStr, sizeof(dateStr), "%Y%m%d_%H:%M:%S", now_tm);
    str = str +  GREEN +
          dateStr + "." + strms +
          "." + strcs +
          "." + strns + RESET;
    str = str + "(" + BOLDYELLOW + std::to_string(ProcessId) + ":" + BOLDBLUE +
          std::to_string(ThreadId) + ")" + RESET;
    str = str + CYAN + file + ":" BOLDYELLOW + std::to_string(line) + BLUE +
          "(" + function + ") " + RESET;
    std::cout << str << vec.data(); // << std::endl;
    std::cout.flush();
}

// 输出msg信息，并带上时间戳、文件名、行号、函数名等信息
#define DEBUG_PRINT(__format, ...)                        \
    printMessage(CxxLog::LOG_TYPE_DEBUG,    __FUNCTION__, \
                 __FILE__, __LINE__, __format, ## __VA_ARGS__)
#define WARRING_PRINT(__format, ...)                      \
    printMessage(CxxLog::LOG_TYPE_WARRING,  __FUNCTION__, \
                 __FILE__, __LINE__, __format, ## __VA_ARGS__)
#define CRITICAL_PRINT(__format, ...)                     \
    printMessage(CxxLog::LOG_TYPE_CRITICAL, __FUNCTION__, \
                 __FILE__, __LINE__, __format, ## __VA_ARGS__)
#define FATAL_PRINT(__format, ...)                        \
    printMessage(CxxLog::LOG_TYPE_FATAL,    __FUNCTION__, \
                 __FILE__, __LINE__, __format, ## __VA_ARGS__)
#define INFO_PRINT(__format, ...)                         \
    printMessage(CxxLog::LOG_TYPE_INFO,     __FUNCTION__, \
                 __FILE__, __LINE__, __format, ## __VA_ARGS__)

CxxLog::~CxxLog()
{
    stop();
    INFO_PRINT("\n");
}

void CxxLog::stop()
{
    m_stopThread = true;

    if (m_thread.joinable()) m_thread.join();
    isInit = false;
}

bool CxxLog::initLog(const std::string & logDir,
                     const unsigned int& flushMs,
                     const unsigned int& fileLogMaxSize)
{
    if (isInit) return true;

    m_logDir = logDir;
    m_flushMs = flushMs;
    m_fileLogMaxSize = fileLogMaxSize;

    std::replace(m_logDir.begin(), m_logDir.end(), '\\', '/');

    if (m_logDir.size() == 0) {
        m_logDir = m_logDir + "./log/";
    }

    if (m_logDir[m_logDir.size() - 1] != '/') {
        m_logDir = m_logDir + "/";
    }

    if (!std::filesystem::exists(m_logDir)) {
        if (!std::filesystem::create_directory(m_logDir)) {
            // std::cout << "create dir failed:" << m_logDir << std::endl;
            FATAL_PRINT("create dir failed:%s\n", m_logDir.c_str());
            return false;
        }
    }

    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm     ntm = localtime_safe(now_time_t);
    std::tm    *now_tm = &ntm;

    // std::tm    *now_tm = std::localtime(&now_time_t); // 线程不安全

    // std::chrono::seconds s =
    //     std::chrono::duration_cast<std::chrono::seconds>(
    //         now.time_since_epoch());
    // std::chrono::milliseconds ms =
    //     std::chrono::duration_cast<std::chrono::milliseconds>(
    //         now.time_since_epoch()) % 1000; // ms.count()
    // std::chrono::microseconds cs =
    //     std::chrono::duration_cast<std::chrono::microseconds>(
    //         now.time_since_epoch()) % 1000000;
    // std::chrono::nanoseconds ns =
    //     std::chrono::duration_cast<std::chrono::nanoseconds>(
    //         now.time_since_epoch()) % 1000000000;

    // 获取当前日期并转为年月日格式
    // std::time_t now = std::time(nullptr);
    // std::tm    *now_tm = std::localtime(&now);

    char dateStr[20];
    std::strftime(dateStr, sizeof(dateStr), "%Y%m%d_%H%M%S", now_tm);
    m_currentLogFile = m_logDir + dateStr + "_" +
                       std::to_string(m_logFileIndex) + ".log";

    // 自动创建，默认 ios::out | ios::trunc
    m_logFileStream = std::ofstream(std::filesystem::path(m_currentLogFile));

    if (!m_logFileStream) {
        // m_logFileStream.is_open()
        // std::cout << "create file failed:" << m_currentLogFile << std::endl;
        FATAL_PRINT("create file failed:%s\n", m_currentLogFile.c_str());
        return false;
    }

    // m_logFileStream<<"1";
    // m_logFileStream.write("1",2);

    m_currentDay = now_tm->tm_mday;
    m_logFileIndex++;
    m_currentLogFileSize = 0;

    m_thread = std::thread(&CxxLog::logThreadFunc, this);
    isInit = true;

    // std::cout << "init Log success:" << m_currentLogFile << std::endl;
    INFO_PRINT("init Log success:%s\n", m_currentLogFile.c_str());
    return true;
}

inline bool CxxLog::creatLogFile()
{
    // 创建新文件
    if (m_currentLogFileSize >= m_fileLogMaxSize) {
        m_logFileStream.close();

        std::time_t now = std::time(nullptr);
        std::tm     ntm = localtime_safe(now);
        std::tm    *localTime = &ntm;

        // std::tm    *localTime = std::localtime(&now);

        if (m_currentDay != localTime->tm_mday) {
            m_currentDay = localTime->tm_mday;
            m_logFileIndex = 0;
        }

        char dateStr[20];
        std::strftime(dateStr, sizeof(dateStr), "%Y%m%d_%H%M%S",
                      localTime);
        m_currentLogFile = m_logDir + dateStr + "_" +
                           std::to_string(m_logFileIndex) + ".log";
        m_logFileStream.open(m_currentLogFile);

        if (!m_logFileStream) {
            FATAL_PRINT("create file failed:%s\n", m_currentLogFile.c_str());
            return false;
        }
        m_currentLogFileSize = 0;
        m_logFileIndex++;
    }
    return true;
}

void CxxLog::logThreadFunc()
{
    const static std::chrono::nanoseconds nanoseconds =
        std::chrono::milliseconds(1);

    const int maxsize = 1000;

    while (!m_stopThread) {
        std::this_thread::sleep_for(nanoseconds);
        unsigned long long nowTimeMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
                ).count();

        if (nowTimeMs - m_currentTime >= (unsigned long long)m_flushMs) {
            m_currentTime = nowTimeMs;

            if (m_logFileStream.is_open()) m_logFileStream.flush();
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_logQueue.size() < 1) continue;

        // 当缓存区数据条数过大时，全部取出
        if (m_logQueue.size() > maxsize) {
            while (m_logQueue.size() > 0) {
                if (creatLogFile()) {
                    // 写日志
                    const std::string& logMessage = m_logQueue.back();
                    m_logFileStream << logMessage;
                    m_currentLogFileSize += logMessage.size();
                }
                m_logQueue.pop_back();
            }
        } else {
            // 取1次数据
            if (creatLogFile()) {
                // 写日志
                const std::string& logMessage = m_logQueue.back();
                m_logFileStream << logMessage;
                m_currentLogFileSize += logMessage.size();
                m_logQueue.pop_back();
            }
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    // 写剩下的日志
    while (!m_logQueue.empty()) {
        if (!creatLogFile()) {
            break;
        }
        const std::string& logMessage = m_logQueue.back();
        m_logFileStream << logMessage;
        m_currentLogFileSize += logMessage.size();
        m_logQueue.pop_back();
    }
}

void CxxLog::addLog(const LOG_TYPE   & logType,
                    const char        *_function,
                    const char        *_file,
                    const int          line,
                    const std::string& logMessage) {
    // #if defined(WIN32) || defined(WIN64)
    //     if (isFirst) {
    //         isFirst = false;
    //         system("color 0");
    //     }
    // #endif // if defined(WIN32) || defined(WIN64)
    const char *typemsg;
    const char *function = const_cast<const char *>(_function);
    const char *file = _file;

    if (function == nullptr) function = (char *)"null";

    if (file == nullptr) file = (char *)"null";
    std::string str;
    std::string strcolor;

    switch (logType) {
    case LOG_TYPE_DEBUG:
        typemsg = "Debug    ";
        strcolor = strcolor + YELLOW;
        break;

    case LOG_TYPE_WARRING:
        typemsg = "Warring  ";
        strcolor = strcolor + CYAN;
        break;

    case LOG_TYPE_CRITICAL:
        typemsg = "Critical ";
        strcolor = strcolor + RED;
        break;

    case LOG_TYPE_FATAL:
        typemsg = "Fatal    ";
        strcolor = strcolor + MAGENTA;
        break;

    case LOG_TYPE_INFO:
        typemsg = "Info     ";
        strcolor = strcolor + GREEN;
        break;

    default:
        typemsg = "         ";
        break;
    }
    str = strcolor + typemsg;

    // auto ThreadId = std::this_thread::get_id();
    unsigned long ProcessId = 0;
    unsigned long ThreadId = 0;
#if defined(WIN32) || defined(WIN64)
    ProcessId = GetCurrentProcessId();
    ThreadId = GetCurrentThreadId();
#endif // if defined(WIN32) || defined(WIN64)
#if defined(__unix__)
    ProcessId = getpid();
    ThreadId = syscall(SYS_gettid);
#endif // if defined(__unix__)

    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm     ntm = localtime_safe(now_time_t);
    std::tm    *now_tm = &ntm;

    // std::tm    *now_tm = std::localtime(&now_time_t);

    // std::chrono::milliseconds ms =
    //     std::chrono::duration_cast<std::chrono::milliseconds>(
    //         now.time_since_epoch()) % 1000;
    // std::chrono::microseconds cs =
    //     std::chrono::duration_cast<std::chrono::microseconds>(
    //         now.time_since_epoch()) % 1000000;
    std::chrono::nanoseconds ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()) % 1000000000;
    char dateStr[128];
    std::strftime(dateStr, sizeof(dateStr), "%Y%m%d_%H:%M:%S", now_tm);

    if (m_isPrinting && (logType >= m_logLevel)) {
        std::string strms = std::to_string(ns.count() / 1000000);

        if (strms.size() < 3) {
            strms = std::string(3 - strms.size(), '0') + strms;
        }
        std::string strcs = std::to_string(ns.count() / 1000 % 1000);

        if (strcs.size() < 3) {
            strcs = std::string(3 - strcs.size(), '0') + strcs;
        }
        std::string strns = std::to_string(ns.count() % 1000);

        if (strns.size() < 3) {
            strns = std::string(3 - strns.size(), '0') + strns;
        }
        str = str +  GREEN +
              dateStr +
              "." + strms +
              "." + strcs +
              "." + strns +
              BOLDBLACK "|" RESET;
        str = str + BOLDYELLOW + std::to_string(ProcessId) +
              ":" BOLDBLUE + std::to_string(ThreadId) +
              BOLDBLACK "|" RESET;

        str = str + CYAN + file + ":" BOLDYELLOW + std::to_string(line) +
              BLUE "(" + function + ")"  MAGENTA  "---"  RESET;
        str = str + logMessage;

        std::cout << str; // << std::endl;
        std::cout.flush();
    }

    if (isInit && (logType >= m_fileLogLevel)) {
        std::string strms = std::to_string(ns.count() / 1000000);

        if (strms.size() < 3) {
            strms = std::string(3 - strms.size(), '0') + strms;
        }
        std::string strcs = std::to_string(ns.count() / 1000 % 1000);

        if (strcs.size() < 3) {
            strcs = std::string(3 - strcs.size(), '0') + strcs;
        }
        std::string strns = std::to_string(ns.count() % 1000);

        if (strns.size() < 3) {
            strns = std::string(3 - strns.size(), '0') + strns;
        }

        str.clear();
        std::string filename(file);
        std::size_t found = filename.find_last_of("/\\");

        if (m_isColorLog) {
            str = strcolor + typemsg +  GREEN +
                  dateStr +
                  "." + strms +
                  "." + strcs +
                  "." + strns +
                  BOLDBLACK "|" RESET;
            str = str + BOLDYELLOW + std::to_string(ProcessId) +
                  ":" BOLDBLUE + std::to_string(ThreadId) +
                  BOLDBLACK "|" RESET;
            str = str + CYAN +
                  filename.substr(found + 1) +
                  ":" BOLDYELLOW + std::to_string(line) +
                  BLUE "(" + function + ")" MAGENTA "---" RESET;
        } else {
            str = str + typemsg +
                  dateStr +
                  "." + strms +
                  "." + strcs +
                  "." + strns +
                  "|";
            str = str + std::to_string(ProcessId) + ":" +
                  std::to_string(ThreadId) + "|";
            str = str + filename.substr(found + 1) + ":" +
                  std::to_string(line) +
                  "(" + function + ")" +
                  "---";
        }
        str = str + logMessage;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_logQueue.push_front(str);
    }
}

void CxxLog::addLogFormat(const LOG_TYPE& logType,
                          const char     *function,
                          const char     *file,
                          const int       line,
                          const char     *format,
                          ...)
{
    va_list arg;

    va_start(arg, format);
    int size = vsnprintf(NULL, 0, format, arg);
    va_end(arg);

    if (size < 0) {
        FATAL_PRINT("vsnprintf failed:%s\n", strerror_safe(errno).c_str());
        return;
    }

    if (size > 0) {
        std::vector<char> vec(size + 1);
        va_start(arg, format);
        vsnprintf(vec.data(), size + 1, format, arg);
        va_end(arg);

        addLog(logType, function, file, line, vec.data());
    }
}

#pragma once

#include <QString>

/**
 * 全局日志。
 *
 * 设计文档 §1/S3：三平台统一的落盘日志，联调排查靠它。
 * - 输出到 stderr（开发期可见）与 <AppData>/logs/qdownload-YYYY-MM-DD.log
 * - 单文件超过 5 MB 自动切到 .1（只保留一份历史，避免无限增长）
 * - 线程安全（QThreadPool 里的写盘线程也会打日志）
 */
class Logger
{
public:
    /// 安装消息处理器，幂等。必须在 QCoreApplication 初始化之后、创建 QML 引擎之前调用。
    /// fileTag 非空时写进文件名（同机双实例联调时区分日志）。
    static void install(const QString &fileTag = QString());

    /// 当前日志文件绝对路径（未安装时为空）
    static QString logFilePath();

    /// 日志目录
    static QString logDirectory();
};

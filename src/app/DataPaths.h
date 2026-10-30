#pragma once

#include <QString>
#include <QStringList>

namespace Qdl::DataPaths {

/// 数据落在哪儿：Portable = 跟着程序走（data/ 与 exe 同级），Installed = 退回系统应用数据目录
enum class Mode {
    Portable,
    Installed
};

/// 必须在 QGuiApplication / QCoreApplication 构造之后、任何数据文件打开之前调用。
/// 判定的依据是程序目录能不能真的写进去，猜同名 exe 的目录会算错。
void init();

QString recordsRoot();
QString logDirectory();

/// 便携模式返回 settings.ini 的绝对路径；安装模式返回空串——
/// 空串表示“用 QSettings 原生位置”（Windows 注册表 / macOS plist），不强行指定文件
QString settingsFilePath();

Mode mode();
bool portable();

/// 给用户或日志看的一句话说明，写明当前模式与实际位置
QString summary();

/// 把旧版本散落在系统目录里的记录文件复制过来（只在便携模式、且新位置还没有时做）。
/// **只复制不删除**：万一用户在同一个系统上同时跑旧版 exe，删了那边的数据就找不回来了
void migrateLegacy(const QStringList &fileNames);

} // namespace Qdl::DataPaths

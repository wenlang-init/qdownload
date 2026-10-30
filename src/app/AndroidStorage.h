#pragma once

#include <QString>
#include <QList>

/**
 * Android 存储能力探测（分区存储适配）。
 *
 * Android 10（API 29）起分区存储收紧：能用普通 File API 直接读写的只剩两块地方——
 *   1) 内部 files 目录          /data/user/0/<pkg>/files
 *   2) 应用专属外部目录         /storage/emulated/0/Android/data/<pkg>/files
 * 别的公共位置（Download / Documents / sdcard 根） fopen 直接 EACCES——
 * 这就是「系统目录选择器返回一个 content:// 地址 / 选完提示没有写入权限」的来源。
 *
 * 想把目录指到任意位置只有一条路：让用户授予「所有文件访问权限」
 * （MANAGE_EXTERNAL_STORAGE，跳系统设置页由用户开关，代码请求不了）。
 *
 * 这个模块把「能不能全盘访问 / 去哪授权 / 现在能用的根有哪些」收敛到一处，
 * 桌面平台没有这些限制——全部返回空 / false，那边继续用系统目录对话框。
 */
namespace Qdl {
namespace AndroidStorage {

struct Root {
    QString path;   /// 真实文件系统路径（QFile / QDir 能用）
    QString label;  /// 界面显示的名字，已过翻译
};

/// 能否用 File API 访问外部存储的任意位置
/// Android 11+ 看 MANAGE_EXTERNAL_STORAGE；Android 9/10 看 WRITE_EXTERNAL_STORAGE 运行时授权
bool hasFullAccess();

/// 跳到系统设置页让用户授权（已有权限时不做事）
void requestFullAccess();

/// 当前可候选的根目录，第一项是推荐默认值。不写其它位置不代表没权限——
/// 公共位置要等 hasFullAccess() 为真才会出现在列表里
QList<Root> roots();

/// 出厂默认放哪：应用专属外部目录优先（空间大、卸载时自动清理），没有则内部 files
QString preferredDir();

} // namespace AndroidStorage
} // namespace Qdl

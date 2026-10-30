#pragma once

#include <QString>

/**
 * 界面语言（M8）。
 *
 * 源码里的文案本身就是中文，所以「中文」不需要翻译文件 —— 中文环境直接不装
 * translator，省一次查表也更不容易出错。非中文环境统一装英文包
 * （`:/i18n/qdownload_en.qm`，由 CMake 的 qt_add_translations 生成并嵌入）。
 *
 * 语言代码约定：空表示「没设置过」→ 中文；"system" 表示跟随系统 UI 语言；
 * 其余是具体语言（"zh_CN" / "en"）。
 *
 * 为什么不把「没设置」当跟随系统：很多中文用户的 Windows 显示语言是英文而区域
 * 是中文，跟着系统走会打开就是英文界面，第一印象很糟。宁可默认中文，让想用英文的人
 * 在设置里主动切。
 */
namespace Qdl {
namespace Translation {

/// 系统语言，例：zh_CN / en_GB
QString systemLanguage();

/// 把 "system"/空 解析成实际语言代码（空 → zh_CN）
QString effective(const QString &code);

/// 装/卸 translator。返回 false 表示语言包装载失败（界面会退回中文）
bool apply(const QString &code);

/// 当前生效的语言代码
QString current();

} // namespace Translation
} // namespace Qdl

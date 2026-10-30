#pragma once

#include <QString>

/**
 * Android 前台服务开关（M8）。
 *
 * 安卓在切后台/锁屏后会冻结普通进程，正在跑的 TCP 传输会被直接掐断。
 * 传输期间挂一个前台服务（带常驻通知）才能继续跑。
 *
 * 其它平台是空实现——桌面程序本来就一直在跑，没有这个概念。
 */
namespace Qdl {
namespace AndroidService {

/// 启动前台服务并更新常驻通知文案
void start(const QString &text);
void stop();

} // namespace AndroidService
} // namespace Qdl

#pragma once

#include <QString>

namespace Qdl {

/**
 * 系统级通知通道（M8）。
 *
 * 桌面平台不需要它：QML 里的托盘气泡 `SystemTrayIcon.showMessage()` 已经能把
 * 提示弹到系统右下角。真正缺这块的是 Android——没有托盘概念，只能挂通知栏。
 *
 * 所以本文件在非 Android 平台是空实现，`notify()` 直接返回 false，
 * 调用方（AppController::notifySystem）退化成打一行日志。
 */
namespace SystemNotifier {

/// 返回 false 表示本平台没实现、缺权限或调用失败
bool notify(const QString &title, const QString &body);

} // namespace SystemNotifier
} // namespace Qdl

#pragma once

#include <QString>

/**
 * 同机多开（多个进程共用一个程序目录）时的实例编号。
 *
 * 以前两个进程会共用一份 `settings.ini`，于是拿到**同一个 peerId**：组播里收到对方的心跳，
 * 一看 peerId 跟自己一样，当成自己的回环包丢掉——表现就是"同机开两个程序互相看不见"。
 * 这里用一把锁文件把本机实例编号区分开：第 1 个进程是主实例（沿用原有设置与身份），
 * 第 2 个及以后各自有自己的 `settings.ini`（peerId、显示名、端口互不干扰）。
 *
 * 必须 **DataPaths::init() 之后、任何 AppController 之前**调用 acquire()。
 */
namespace Qdl::InstanceGuard {

/// 抢占本机实例编号（1 = 主实例）。重复调用返回同一个值，进程退出时自动释放
int acquire();

/// 当前实例编号（1 起）。未调用 acquire() 时返回 1
int index();

/// 主实例返回空串；其余返回 " (2)" 这种可直接拼到设备名后面的后缀
QString nameSuffix();

/**
 * 在**本机范围内**认领一个 peerId：同一台机器上不允许两个活进程用同一个 id。
 * 被别的活进程占着返回 false（调用方必须换一个，否则组播里双方会把对方当自己丢掉）。
 *
 * 为什么不能只靠 `settings.ini`：把程序目录整份拷一份、或者从别的机器搬过来一个
 * `data/` 目录，两个副本会带着同一个 peerId 跑，配置再独立也救不回来。
 */
bool claimPeerId(const QString &peerId);

/// 造一个本机没人在用的 peerId（UUID v4）。造不出来时也返回一个（极端兜底）
QString newPeerId();

/// 本机当前有几个活进程认领着 peerId（含自己）。用于给默认设备名编号
int livePeerClaimCount();

/**
 * 该实例的配置文件路径。
 * 主实例原样返回 basePath（安装模式下 basePath 为空串，表示走 QSettings 原生位置）；
 * 其余实例返回 `<数据目录>/instance-<n>/settings.ini`，与主实例彻底分开。
 */
QString scopedSettingsPath(const QString &basePath);

} // namespace Qdl::InstanceGuard

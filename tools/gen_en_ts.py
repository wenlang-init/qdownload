#!/usr/bin/env python3
"""把英文对照表刷进 i18n/qdownload_en.ts。

为什么用脚本而不是直接在 Qt Linguist 里翻译：
  1. 源码文案是中文，ts 里的 source 就是中文原文，逐条手点 260 多条太慢；
  2. 文案会随功能变化，脚本可以重跑，lupdate 会保留已有 translation 节点，
     所以重跑只补新增条目，不会覆盖人工改过的译文。

用法：
  lupdate src qml -ts i18n/qdownload_en.ts -locations none -no-obsolete
  python tools/gen_en_ts.py
  lrelease i18n/qdownload_en.ts -qm build/qdownload_en.qm
"""

from __future__ import annotations

import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# source（中文原文） → 英文译文。占位符 %1 %2 必须原样保留。
EN: dict[str, str] = {
    # ---------------- AppController ----------------
    "接收目录": "Received files",
    "共享根": "Shared folder",
    "已上传 %1": "Uploaded %1",
    "上传完成": "Upload complete",
    "%1 已存到对方的收件箱": "%1 was saved to the other device's inbox",
    "已下载 %1": "Downloaded %1",
    "下载完成": "Download complete",
    "%1 已存到本机接收目录": "%1 was saved to your received folder",
    "有 %1 个未完成的下载，点继续可接着下":
        "%1 unfinished download(s). Press Resume to continue.",
    "%1 在主机上已变化，重新从头下载": "%1 changed on the host; restarting from the beginning",
    "校验失败：内容与主机不一致，已删除":
        "Checksum mismatch with the host; the file was deleted",
    "%1 校验失败，已删除": "%1 failed verification and was deleted",
    "主机未提供校验值，仅校验了大小": "The host sent no checksum; only the size was verified",
    "主机已有 %1，从断点续传 %2": "The host already has %1; resuming at %2",
    "%1 未能上传：%2": "%1 could not be uploaded: %2",
    "主机中断了上传": "The host interrupted the upload",
    "%1 未传完：%2": "%1 did not finish: %2",
    "已收到 %1（%2）": "Received %1 (%2)",
    "收到文件": "File received",
    "%1（%2）已存进收件箱": "%1 (%2) was saved to the inbox",
    "接收 %1 中断：%2": "Receiving %1 was interrupted: %2",
    "未命名设备": "Unnamed device",
    "写权限已关闭，%1 个上传已暂停（断点已保留）":
        "Write access is off. %1 upload(s) paused; partial data kept.",
    "上传已暂停": "Upload paused",
    "写权限已关闭，%1 个上传中断（断点已保留）":
        "Write access is off. %1 upload(s) interrupted; partial data kept.",
    "仅白名单": "Allowlist only",
    "已配对皆可传": "Any paired device",
    "关": "Off",
    "白名单": "Allowlist",
    "全部已配对": "All paired devices",
    "未发现主机": "No hosts found",
    "%1 台主机全部离线": "All %1 host(s) are offline",
    "发现 %1 台（%2 在线）": "%1 host(s) found (%2 online)",
    "未选择主机": "No host selected",
    "主机已离线": "Host is offline",
    "未连接": "Not connected",
    "目录共 %1 项，仅显示前 %2 项": "%1 items in total, showing the first %2",
    "尚未连接到主机": "Not connected to a host",
    "尚未连接到主机，无法下载": "Not connected to a host; cannot download",
    "对方已关闭共享": "The other device has sharing turned off",
    "文件名不合法": "Invalid file name",
    "主机忙或未连接，稍后再试": "The host is busy or unreachable; try again later",
    "尚未连接到主机，无法继续": "Not connected to a host; cannot continue",
    "主机已断开，只校验了大小": "The host disconnected; only the size was verified",
    "无法请求校验值，只校验了大小": "Could not request a checksum; only the size was verified",
    "主机未提供校验值，只校验了大小": "The host sent no checksum; only the size was verified",
    "已上传 %1（校验一致）": "Uploaded %1 (checksums match)",
    "校验失败：主机收到的内容与本地不一致":
        "Verification failed: the host received different content",
    "%1 上传校验失败": "%1 failed upload verification",
    # ---- Android 存储适配（AndroidStorage / DirBrowser）----
    "应用专属目录": "App-specific storage",
    "内部存储": "Internal storage",
    "共享存储空间": "Shared storage",
    "原来的共享目录不可写，已改用 %1":
        "The previous shared folder is no longer writable; switched to %1",
    "原来的接收目录不可写，已改用 %1":
        "The previous received-files folder is no longer writable; switched to %1",
    "此电脑": "This PC",
    "主目录": "Home",
    "文档": "Documents",
    "位置": "Location",
    "安卓禁止直接读写公共目录，现在只能选本应用专属目录；想指定任意位置请先授予「所有文件访问权限」":
        "Android blocks direct access to public folders, so only the app's own storage is selectable. Grant \"All files access\" to pick any location.",
    "建不了这个文件夹：换个名字试试": "Couldn't create that folder; try a different name",
    "找不到可用的存储位置，请在系统设置里允许本应用访问文件":
        "No usable storage location. Allow file access for this app in system settings.",
    "返回上级": "Go up one level",
    "这个目录里没有子文件夹": "No subfolders here",
    "去授权": "Grant access",
    "新建文件夹": "New folder",
    "新建": "Create",
    "选择此目录": "Use this folder",
    "%1 已排队：同时进行最多 %2 个，等前面的传完自动接上":
        "%1 queued: at most %2 transfer at a time; it starts when a slot frees up",
    "同时进行的文件已达 %1 个，%2 个上传在排队，前面的传完自动接上":
        "Already running %1 transfers; %2 upload(s) queued and will start as slots free up",
    "连接主机后可上传": "Connect to a host to upload",
    "对方已关闭接收": "The other device is not accepting files",
    "未获写权限（只读设备）": "No write access (read-only device)",
    "接收方：%1 · 收件箱": "Receiving device: %1 · inbox",
    "尚未连接到主机，无法上传": "Not connected to a host; cannot upload",
    "对方已关闭接收文件": "The other device has stopped accepting files",
    "同时最多上传 3 个文件，请等当前任务完成":
        "Up to 3 uploads at a time; wait for one to finish",
    "只能上传文件：%1": "Only files can be uploaded: %1",
    "上传申请未发出（主机忙或未授权）":
        "Upload request was not sent (host busy or not authorized)",
    "正在上传 %1 个文件到 %2 的收件箱": "Uploading %1 file(s) to %2's inbox",
    "只能拖入本机文件（不支持目录）": "Only local files can be dropped (folders are not supported)",
    "没有可上传的文件": "No files to upload",
    "无法打开收件目录：%1": "Could not open the inbox: %1",
    "正在接收 %1（%2%）": "Receiving %1 (%2%)",
    "正在接收 %1 个文件": "Receiving %1 file(s)",

    # ---------------- ConfirmDialog ----------------
    "确定": "OK",
    "取消": "Cancel",

    # ---------------- 状态栏 / 语言 / 前台服务 ----------------
    "翻译包加载失败，界面保持中文": "Could not load the translation pack; the UI stays in Chinese",
    "正在传输文件": "Transferring files",
    "界面语言": "Language",
    "跟随系统": "Follow system",
    "↓ ": "↓ ",

    # ---------------- ControlClient ----------------
    "主机地址或控制端口缺失": "Host address or control port is missing",
    "连接超时，主机可能已离线或防火墙拦截":
        "Connection timed out; the host may be offline or blocked by a firewall",
    "鉴权失败": "Authentication failed",
    "配对码至少 4 位": "The pairing code must be at least 4 digits",
    "列目录超时，主机没有响应": "Listing the folder timed out; the host did not respond",
    "申请下载超时，主机没有响应": "Download request timed out; the host did not respond",
    "主机返回的下载信息不完整": "The host returned incomplete download details",
    "主机未在 90 秒内返回校验值": "The host did not return a checksum within 90 seconds",
    "主机未能计算校验值": "The host could not compute a checksum",
    "申请上传超时，主机没有响应": "Upload request timed out; the host did not respond",
    "主机返回的上传信息不完整": "The host returned incomplete upload details",
    "配对码不正确，还可尝试 %1 次": "Wrong pairing code; %1 attempt(s) left",
    "配对码不正确": "Wrong pairing code",
    "配对码已过期，请让对方重新发起": "The code expired; ask the other device to start over",
    "对方拒绝了配对请求": "The other device declined the pairing request",
    "尝试次数过多，请 60 秒后重试": "Too many attempts; try again in 60 seconds",
    "配对会话已失效，请重新发起": "The pairing session expired; start over",
    "配对失败（%1）": "Pairing failed (%1)",
    "主机返回错误 %1": "The host returned error %1",
    "连接中…": "Connecting…",
    "握手中…": "Handshaking…",
    "鉴权中…": "Authenticating…",
    "未配对": "Not paired",
    "等待对方确认…": "Waiting for confirmation…",
    "等待输入配对码": "Waiting for the pairing code",
    "已连接 · 可写": "Connected · read-write",
    "已连接 · 只读": "Connected · read-only",
    "连接失败": "Connection failed",

    # ---------------- ControlServer / DataServer ----------------
    "控制通道启动失败（%1..%2 均被占用）：%3":
        "Could not start the control channel (ports %1..%2 are all in use): %3",
    "数据通道启动失败（%1..%2 均被占用）：%3":
        "Could not start the data channel (ports %1..%2 are all in use): %3",

    # ---------------- DiscoveryService ----------------
    "组播端口 %1/udp 绑定失败：%2": "Could not bind the multicast port %1/udp: %2",
    "没有可用的组播网卡，只能靠手动添加 IP":
        "No multicast-capable network interface; add the host IP manually",

    # ---------------- FileListView ----------------
    "暂无内容": "Nothing here",
    "下载": "Download",
    "加载中…": "Loading…",

    # ---------------- LocalPage ----------------
    "本机": "This device",
    "收件箱": "Inbox",
    "上级": "Up",
    "刷新": "Refresh",
    "接收目录还是空的": "Your received folder is empty",
    "这个目录是空的": "This folder is empty",
    "从右侧远端列表下载的文件会出现在这里":
        "Files you download from the remote list appear here",
    "上传": "Upload",

    # ---------------- Main ----------------
    "qdownload · %1": "qdownload · %1",
    "已收进托盘，点托盘图标可以叫回来":
        "Still running in the tray; click the tray icon to reopen",
    "共享": "Share",
    "立即广播并刷新主机状态": "Announce now and refresh host status",
    "设置（共享根、上传策略、并发数）":
        "Settings (shared folder, upload policy, concurrency)",
    "⚠ 组播发现不可用：%1": "⚠ Multicast discovery unavailable: %1",
    "写权限：%1": "Write access: %1",
    "远端": "Remote",
    "设置": "Settings",
    "共享根目录": "Shared folder",
    "已配对设备 %1 台": "%1 paired device(s)",
    "控制端口 %1 · 写权限：%2": "Control port %1 · write access: %2",
    "管理": "Manage",
    "上传策略（仅在写权限打开时生效）":
        "Upload policy (only applies while write access is on)",
    "仅白名单（单设备 rw）": "Allowlist only (per-device rw)",
    "收件箱：%1": "Inbox: %1",
    "关闭窗口时收进托盘": "Hide to tray when closing the window",
    "打开收件箱": "Open inbox",
    "关闭": "Close",
    "关闭共享？": "Turn off sharing?",
    "当前有 %1 个上传正在接收，关闭共享会立刻中断它们。\n断点会保留，重新打开共享后对方可以接着传。":
        "%1 upload(s) are being received right now. Turning off sharing interrupts them "
        "immediately.\nPartial data is kept, so they can resume once sharing is back on.",
    "中断并关闭": "Interrupt and turn off",
    "继续接收": "Keep receiving",
    "松手即可上传": "Drop to upload",
    "现在不能上传": "Uploading is not available",
    "文件会存到「%1」的收件箱": "Files go to the inbox on \"%1\"",

    # ---------------- PairCodeDialog ----------------
    "与「%1」配对": "Pair with \"%1\"",
    "对方主机界面上会显示一个 6 位配对码，输入即可完成配对。":
        "The other device shows a 6-digit pairing code on its screen. Enter it here.",
    "6 位数字": "6 digits",
    "配对": "Pair",

    # ---------------- PairRequestDialog ----------------
    "配对请求": "Pairing request",
    "「%1」请求访问本机共享的文件": "\"%1\" wants to browse the files you share",
    "%1s": "%1s",
    "请让「%1」在它自己的窗口里输入上面这串码。":
        "Ask \"%1\" to enter this code in its own window.",
    "允许该设备上传文件": "Allow this device to upload files",
    "还需要顶部「写权限」开关处于打开状态":
        "The \"Write access\" switch at the top must also be on",
    "当前写权限总开关是关的，上传会被拒绝":
        "Write access is currently off; uploads will be rejected",
    "拒绝": "Decline",
    "允许配对": "Allow",

    # ---------------- PeerSelector ----------------
    "组播发现中，3 秒一次心跳；对端没开共享也会出现在这里":
        "Discovering over multicast (heartbeat every 3s). Hosts with sharing off are listed too.",
    "组播不可用：%1": "Multicast unavailable: %1",
    "可上传": "Writable",
    "只读": "Read-only",

    # ---------------- TransferFmt ----------------
    "剩余 %1 秒": "%1s left",
    "剩余 %1 分 %2 秒": "%1m %2s left",
    "剩余 %1 小时 %2 分": "%1h %2m left",
    "排队中": "Queued",
    "传输中": "Transferring",
    "校验中…": "Verifying…",
    "已完成": "Completed",
    "已暂停": "Paused",
    "失败": "Failed",
    "已取消": "Canceled",

    # ---------------- TransferTask ----------------
    "下载参数不完整，无法建立数据连接":
        "Incomplete download parameters; cannot open the data connection",
    "无法创建临时文件：%1": "Could not create the temporary file: %1",
    "续传文件不完整（应有 %1 字节，实际 %2），请重新下载":
        "Incomplete partial file (%1 bytes expected, %2 found); please download again",
    "无法续写临时文件：%1": "Could not append to the temporary file: %1",
    "续传文件在打开时被截断（应为 %1 字节，实际 %2）":
        "The partial file was truncated when opened (%1 bytes expected, %2 found)",
    "上传参数不完整，无法建立数据连接":
        "Incomplete upload parameters; cannot open the data connection",
    "无法读取本地文件：%1": "Could not read the local file: %1",
    "无法定位到上传起点 %1": "Could not seek to the upload start offset %1",
    "主机未确认上传结果，仅确认已发送完毕":
        "The host did not confirm the result; all bytes were sent",
    "写入失败（磁盘可能已满）：%1": "Write failed (the disk may be full): %1",
    "数据连接失败：%1": "Data connection failed: %1",
    "连接中断，已发送 %1 / %2": "Connection lost after sending %1 / %2",
    "数据连接被对端关闭": "The other side closed the data connection",
    "连接中断，已收到 %1 / %2": "Connection lost after receiving %1 / %2",
    "连接超时（6 秒无进展），可重试": "Connection timed out (no progress for 6s); you can retry",
    "连接超时（6 秒无数据），已保留断点":
        "Connection timed out (no data for 6s); partial data kept",
    "已发": "Sent",
    "已收": "Received",
    "大小校验失败（应为 %1 字节，实际 %2）": "Size check failed (%1 bytes expected, %2 found)",
    "无法重命名为目标文件：%1": "Could not rename to the target file: %1",
    "无法读取已下载文件：%1": "Could not read the downloaded file: %1",
    "上传校验失败：主机收到的内容与本地不一致":
        "Upload verification failed: the host received different content",
    "校验失败：下载内容与主机不一致":
        "Verification failed: the downloaded content differs from the host's",
    "上次未传完，点继续接着下": "Unfinished from last time; press Resume to continue",

    # ---------------- RemotePage ----------------
    "在上方选择一台共享主机；对方没开共享也会出现在列表里":
        "Pick a host above. Hosts with sharing off are listed too.",
    "「%1」已离线，等待它重新广播（10 秒无心跳判离线）":
        "\"%1\" is offline; waiting for it to announce again (10s without a heartbeat means offline)",
    "未共享": "Not sharing",
    "「%1」在线，但没有开启共享": "\"%1\" is online but is not sharing",
    "正在连接「%1」…": "Connecting to \"%1\"…",
    "未配对：点右侧「配对」后，输入对方屏幕上显示的 6 位码":
        "Not paired: click \"Pair\" and enter the 6-digit code shown on the other device",
    "已发出配对请求，等待对方在主机上点「允许」，并把配对码告诉你":
        "Pairing request sent. The other device must tap \"Allow\" and tell you the code.",
    "请输入对方屏幕上显示的 6 位配对码":
        "Enter the 6-digit pairing code shown on the other device",
    "「%1」已关闭共享，目录暂时不可读":
        "\"%1\" turned sharing off; the folder is not readable right now",
    "已连接「%1」· 可读可写 · %2 项": "Connected to \"%1\" · read-write · %2 items",
    "已连接「%1」· 只读 · %2 项": "Connected to \"%1\" · read-only · %2 items",
    "尚未选择共享主机": "No host selected yet",
    "尚未配对": "Not paired yet",
    "尚未连接": "Not connected yet",
    "等对方开启共享后会自动出现在上方列表":
        "It appears above automatically once the other device turns sharing on",
    "配对后才能浏览对方的文件": "Pair first to browse the other device's files",
    "连上主机后自动拉取目录": "The folder loads automatically once connected",
    "点「刷新」重试，或检查对方是否关闭了共享":
        "Press \"Refresh\" to retry, or check whether sharing was turned off",
    "对方共享目录里还没有内容": "The shared folder is empty",
    "未选择": "Not selected",
    "重连": "Reconnect",
    "文件传输并发数": "Concurrent transfers",
    "排队 %1 个": "%1 queued",
    "单文件连接数": "Connections per file",
    "并发数 = 同时传几个文件，超出的自动排队；连接数 = 每个文件同时开几条连接，谁先传完谁接着领下一段。改动立即生效":
        "Concurrency = how many files at once (the rest queue). Connections = how many "
        "connections one file opens; the first to finish takes the next chunk. "
        "Changes apply immediately.",

    # ---------------- 单文件多线程（M12，TransferSegment / TransferTask） ----------------
    "定位到偏移 %1 失败": "Failed to seek to offset %1",
    "源文件在传输过程中被截断（期望至少 %1 字节，实际 %2）":
        "The source file was truncated during transfer (expected at least %1 bytes, found %2)",
    "读取源文件失败（可能已被移动或截断）：%1":
        "Failed to read the source file (it may have been moved or truncated): %1",
    "连接中断，本段已发 %1 / %2": "Connection lost; this segment sent %1 / %2",
    "连接中断，本段已收 %1 / %2": "Connection lost; this segment received %1 / %2",
    "本地分片与主机下发的区间不一致（第 %1 段），请重新下载":
        "A local part file does not match the range from the host (segment %1). Please download again.",
    "分片文件缺失（第 %1 段）：%2": "Part file missing (segment %1): %2",
    "合并分片失败（磁盘可能已满）：%1": "Failed to merge parts (the disk may be full): %1",
    "控制通道未就绪": "The control channel is not ready",
    "领取分段失败": "Could not claim a chunk",
    "领取分段超时": "Timed out claiming a chunk",
    "主机已丢弃这次传输，请重新开始":
        "The host dropped this transfer. Please start it again.",

    # ---------------- 传输限速（M10，Main.qml 设置面板） ----------------
    "传输限速": "Speed limit",
    "%1 MB/s": "%1 MB/s",
    "MB/s": "MB/s",
    "（不限速）": "(unlimited)",
    "（当前 %1）": "(now %1)",
    "不限速": "Unlimited",
    "0 表示不限速。限速由所有任务共享：一个文件时它吃满，多个文件时大家分。改完立即生效，正在传的文件不会被打断":
        "0 means no limit. The cap is shared by all transfers: a single file gets all of it, several files split it. Changes apply immediately without interrupting running transfers.",
    "设置（共享根、上传策略、并发数、限速）":
        "Settings (shared folder, upload policy, concurrency, speed limit)",
    "输入配对码": "Enter pairing code",
    "断开": "Disconnect",
    "已手动断开「%1」，点「重连」恢复":
        "Manually disconnected from \"%1\". Press \"Reconnect\" to connect again.",
    "你手动断开了连接，不会自动重连":
        "You disconnected manually; it will not reconnect automatically.",

    # ---------------- 本端名称（Main.qml 设置面板） ----------------
    "本端名称": "Device name",
    "保存": "Save",
    "对方在主机列表里看到的就是这个名字，保存后立刻广播，不用等心跳":
        "This is the name others see in their host list. It is announced "
        "immediately on save, no need to wait for the next heartbeat.",
    "恢复默认（%1）": "Reset to default (%1)",
    "本端名称已改为「%1」": "Device name changed to \"%1\"",
    "本端名称已恢复默认": "Device name reset to default",

    # ---------------- Transfer / TransferDrawer ----------------
    "暂无任务": "No transfers",
    "%1 个传输中 · %2": "%1 transferring · %2",
    "%1 个任务 · %2 已完成": "%1 task(s) · %2 done",
    "传输任务 (%1)": "Transfers (%1)",
    "清除已完成": "Clear finished",
    "暂停": "Pause",
    "继续": "Resume",
    "还没有传输任务": "No transfers yet",
    "在右侧远端列表里点「下载」，进度、速度、暂停/取消都会显示在这里":
        "Click \"Download\" in the remote list; progress, speed and pause/cancel show up here",

    # ---------------- TrayIcon ----------------
    "qdownload · 正在传输 %1 项": "qdownload · %1 transfer(s) in progress",
    "显示主窗口": "Show main window",
    "共享我的文件": "Share my files",
    "接收别人上传": "Accept uploads",
    "传输中 %1 项 · %2": "%1 transferring · %2",
    "没有进行中的传输": "No transfers in progress",
    "收件箱空闲": "Inbox idle",
    "退出": "Quit",

    # ---------------- 关窗与退出（Main.qml / AppController） ----------------
    "关闭窗口时收进托盘（后台继续传输）": "Keep running in the tray when the window closes",
    "还有 %1 个传输在进行": "%1 transfer(s) still in progress",
    "现在退出会中断它们。已经传到的部分会保留成断点，下次重新发起会自动从断点接着传，不会丢。":
        "Quitting now interrupts them. What has been received is kept as a resume "
        "point; starting again will pick up from there. Nothing is lost.",
    "仍要退出": "Quit anyway",
    "收进托盘继续": "Minimize to tray",
    "继续传输": "Keep transferring",
    "已收进托盘": "Still running in the tray",
    "点托盘图标叫回窗口；要真正退出，在托盘菜单里点「退出」":
        "Click the tray icon to reopen the window. To quit for real, choose \"Quit\" "
        "in the tray menu.",

    # ---------------- PeerInfo（tr 走 QCoreApplication::translate("PeerInfo", …)） ----------------
    "未知主机": "Unknown host",
    "%1（%2）": "%1 (%2)",

    # ---------------- TrustedDevicesDialog ----------------
    "已配对设备": "Paired devices",
    "这些设备可以浏览本机共享的文件。勾选「可写」并打开顶部写权限开关后，才能上传。":
        "These devices can browse the files you share. They can upload only when "
        "\"Writable\" is checked and the write-access switch at the top is on.",
    "在线": "Online",
    "离线": "Offline",
    "可写": "Writable",
    "撤销": "Revoke",
    "还没有设备配对过。对方发起配对、你在这台机器上点「允许」后，会出现在这里。":
        "No devices paired yet. They appear here after they request pairing and you "
        "tap \"Allow\" on this machine.",
    "控制端口 %1 · 信任库 %2": "Control port %1 · trust store %2",

    # ---------------- WriteToggle ----------------
    "写权限": "Write access",
    "关闭写权限？": "Turn off write access?",
    "当前有 %1 个上传正在接收，关闭写权限会立刻中断它们。\n已经收到的部分会保留为断点，重新打开后对方可以接着传。":
        "%1 upload(s) are being received right now. Turning off write access interrupts "
        "them immediately.\nPartial data is kept, so they can resume once it is back on.",

    # ---------------- AppController 目录相关（directoryIssue 等） ----------------
    "建不出这个目录：%1": "Cannot create this directory: %1",
    "没有写入权限：%1": "No write permission: %1",
    "路径为空": "The path is empty",
    "这不是一个目录": "This is not a directory",
    "不能用磁盘根目录，请选一个子目录": "A drive root cannot be used. Pick a subfolder",
    "这个目录读不了": "This directory cannot be read",
    "共享目录": "Shared folder",
    "接收目录": "Received files",
    "不能和%1用同一个目录": "Cannot be the same folder as the %1",
    "不能是%1的上级目录": "Cannot be a parent of the %1",
    "不能是%1的子目录": "Cannot be inside the %1",
    "共享目录已切换": "Shared folder changed",
    "对方下次刷新会看到新的内容": "Peers will see the new content after they refresh",

    # ---------------- DirPicker ----------------
    "取不到这个目录的本地路径，换个位置试试":
        "Cannot get a local path for this folder. Try another location",
    "已经是这个目录了": "Already using this folder",
    "已切换到 %1": "Switched to %1",
    "更改": "Change",
    "恢复默认": "Reset to default",
    "未设置": "Not set",
    "选择%1": "Choose %1",
    "选择": "Select",
    "正在共享，更换后对方刷新列表就会看到新目录":
        "Sharing is on. Peers will see the new folder after they refresh",
    "别人能看到的目录，建议单独放一个文件夹":
        "What others can browse. Use a dedicated folder",
    "下载存到这里，本机文件列表显示的就是它":
        "Downloads land here. The \"This device\" list shows this folder",
    "正在传的任务不受影响，断点仍写在旧目录":
        "Running transfers are unaffected; partial files stay in the old folder",
    "有 %1 个未完成的下载，点继续可接着下":
        "%1 unfinished download(s). Tap resume to continue",

    # ---------------- DataPaths（存储位置） ----------------
    "程序目录 %1 不可写，数据改用系统位置 %2":
        "Program folder %1 is not writable; using system location %2 instead",
    "%1: %2": "%1: %2",
    "便携模式（数据跟着程序走）":
        "Portable (data lives next to the program)",
    "安装模式（程序目录不可写）":
        "Installed (program folder is not writable)",
    "已从旧位置复制 %1 个记录文件到 %2":
        "Copied %1 record file(s) from the old location to %2",

    # ---------------- 设置面板 · 存储位置 ----------------
    "配对记录与断点存在这里": "Where pairing records and resume points live",
    "整个文件夹拷走就能带走全部记录；装在受保护的目录里才会改用系统位置":
        "Copy the whole folder to take all records with you. System location is "
        "used only when the program folder is protected",

    # ---------------- 删除文件（M14，本机 / 远端） ----------------
    "删除": "Delete",
    "删除本机文件？": "Delete this file?",
    "「%1」会被永久删除，不进回收站。":
        "\"%1\" will be deleted permanently (it does not go to the trash).",
    "删除对方共享目录里的文件？": "Delete a file in the other device's shared folder?",
    "「%1」会在「%2」那台机器上被永久删除。":
        "\"%1\" will be deleted permanently on \"%2\".",
    "已删除「%1」": "Deleted \"%1\"",
    "删除「%1」失败：%2": "Could not delete \"%1\": %2",
    "删除失败：%1（可能正被其他程序占用）":
        "Could not delete %1 (it may be in use by another program)",
    "主机没有说明原因": "the host gave no reason",
    "文件夹请进入后逐个删除": "Open the folder and delete the files inside one by one",
    "「%1」正在传输，先取消再删除": "\"%1\" is still transferring; cancel it first",
    "尚未连接到主机，无法删除": "Not connected to a host; nothing to delete",
    "对方已关闭写权限，无法删除": "The host turned write access off; cannot delete",
    "未获写权限（只读设备不能删除）": "No write access (read-only devices cannot delete)",
    "无法发起删除：连接状态已变化":
        "Could not start the delete: the connection state changed",
    "主机没有响应": "The host did not respond",

    # ---------------- 校验降级（M13） ----------------
    "无法计算本机文件校验值": "Could not compute the local checksum",
    "无法计算本机文件校验值，只校验了大小":
        "Could not compute the local checksum; only the size was verified",
    "无法计算本地文件校验值": "Could not compute the local checksum",
    "%1 上传校验失败：%2": "%1 failed verification on upload: %2",
}


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    ts_path = root / "i18n" / "qdownload_en.ts"
    if not ts_path.exists():
        print(f"找不到 {ts_path}，先跑 lupdate 生成", file=sys.stderr)
        return 1

    tree = ET.parse(ts_path)
    applied = 0
    missing: list[str] = []

    for message in tree.getroot().iter("message"):
        source_node = message.find("source")
        if source_node is None or source_node.text is None:
            continue

        translation = message.find("translation")
        if translation is None:
            translation = ET.SubElement(message, "translation")

        # 已经人工改过的译文不覆盖（除非还是 unfinished）
        if translation.text and translation.get("type") != "unfinished":
            continue

        english = EN.get(source_node.text)
        if english is None:
            missing.append(source_node.text)
            continue

        translation.text = english
        translation.attrib.pop("type", None)
        applied += 1

    tree.write(ts_path, encoding="utf-8", xml_declaration=True)

    print(f"已写入 {applied} 条译文")
    if missing:
        print(f"还有 {len(missing)} 条没有对照（将回退显示中文原文）：")
        for text in missing:
            print("  -", text.replace("\n", "\\n"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

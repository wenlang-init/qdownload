#pragma once

#include <QString>
#include <QtGlobal>

/**
 * 命令行覆盖项。
 *
 * 存在的唯一理由是**同一台机器跑两个实例**做联调：否则两个进程共用一份 QSettings，
 * 拿到同一个 peerId，彼此都把自己的心跳当“自己”丢弃，就永远发现不了对方。
 *
 * 用法：`qdownload.exe --instance a --name 甲机`
 */
struct LaunchOptions
{
    QString instanceTag;      // 写进日志文件名，避免两实例争抢同一个日志
    QString peerIdOverride;   // 覆盖 peerId（不落盘）
    QString nameOverride;     // 覆盖显示名（不落盘）
    quint16 ctrlPort = 45455; // M3 的控制通道起始端口

    // -1 = 用设置里的值；0/1 = 本次运行强制。联调时验证 sharing/write 传播要用
    int shareOverride = -1;
    int writeOverride = -1;
    // -1 = 用设置里的值；0/1 = 本次运行强制收进托盘（验证托盘/退出两条分支）
    int closeToTrayOverride = -1;

    // 传输限速（MB/s）：-1 = 用设置里的值，>=0 = 本次运行强制上限（0 = 不限速）。
    // 联调时用来验证限速在上传/下载两侧是否都生效，不写入设置
    int speedLimitOverrideMbps = -1;

    // 单文件连接数：0 = 用设置里的值，1..8 = 本次运行强制（联调时对比单连接/多连接速度）
    int segmentOverride = 0;

    // 覆盖共享根（不落盘）：联调与生成文档截图时指向一份准备好的目录树
    QString shareRootOverride;
    // 覆盖接收目录（不落盘）：下载落点，与共享根同理
    QString receiveRootOverride;

    // 界面语言：空 / "system" = 跟随系统，其余是具体语言（zh_CN / en）。
    // 由 main 合并 --lang 与设置里的值，AppController 只负责读
    QString language;

    // 仅用于生成文档截图：启动即切到远端 Tab（窄屏布局默认停在本机 Tab）
    bool demoTabRemote = false;
    // 仅用于生成文档截图：连上后直接进到这个虚拟路径（展示面包屑层级）
    QString demoPath;
    // 仅用于生成文档截图：启动即展开传输抽屉
    bool demoTransfers = false;
    // 仅用于生成文档截图：连上后自动下载这个虚拟路径（让抽屉里有真实进度）
    QString demoGetPath;
    // 仅用于生成文档截图：连上后自动上传这个本机文件（让抽屉里有真实的上传进度）
    QString demoPutPath;
    // 仅用于验证 i18n 热切换：启动 N 毫秒后切到另一种语言（截图对比用）
    int demoLangSwitchMs = -1;
    // 仅用于生成文档截图：强制显示拖拽提示层
    bool demoDrop = false;
    // 仅用于生成文档截图：N 毫秒后打开「关闭写权限」确认框
    int demoConfirmWriteMs = -1;
    // 仅用于验证退出流程：N 毫秒后自己点一次「关闭窗口」（有传输时会先弹确认框）
    int demoQuitMs = -1;
    // 仅用于验证切换目录：role 0=共享根 1=接收目录，到点自动切并把体检结果打进日志
    int demoDirRole = 0;
    QString demoDirPath;
    // 仅用于生成文档截图：启动即打开设置面板
    bool demoSettings = false;
    // 仅用于排障/截图：桌面也强制走安卓那套内置目录浏览器（真机上没法一点点试）
    bool demoDirBrowser = false;

    // ---- 配对相关（仅联调/自动化）----
    QString pairCode;              // 固定配对码，省去读屏
    bool autoApprove = false;      // 收到配对请求直接放行
    bool autoApproveWrite = false; // 自动放行时是否给 rw
    // 仅在「码已校验通过、等主人点同意」时延时模拟点击「允许配对」。
    // auto-approve 走的是 begin() 里的提前放行，覆盖不到这条真实路径
    int approvePendingAfterMs = -1;
    QString pairTestPeerId;        // --pair-test 的目标主机
    QString pairTestCode;          // --pair-test 使用的配对码
    int toggleWriteAfterMs = -1;   // 到点后翻转写权限开关（验证 PERM_UPDATE 即时生效）
    int toggleShareAfterMs = -1;   // 到点后翻转共享开关（验证列目录立即 403 / 连接不断）
    int watchAfterReadyMs = -1;    // Ready 后继续观察多久，打印 canUpload 变化

    static LaunchOptions &instance()
    {
        static LaunchOptions options;
        return options;
    }
};

inline LaunchOptions &launchOptions()
{
    return LaunchOptions::instance();
}

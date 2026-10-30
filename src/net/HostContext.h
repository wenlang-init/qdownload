#pragma once

#include <QString>

/// 共享端对外呈现的身份与共享状态（由 AppController 持续同步）
struct HostContext
{
    QString peerId;
    QString name;
    QString platform;
    QString shareRoot;         // 共享根的真实绝对路径；LIST_REQ 的虚拟路径都相对它解析
    bool shareEnabled = false;
    bool writeEnabled = false;
    int uploadPolicy = 0;      // 0 = 仅白名单，1 = 已配对皆可传
    quint16 port = 0;          // 实际监听端口（被占用时会 +1）

    /// 有效写权限 = L1 总开关 && (L2 全部已配对 || L3 该设备 rw)，见设计文档 §7.4
    bool canUploadFor(const QString &perm) const
    {
        if (!shareEnabled || !writeEnabled)
            return false;
        return uploadPolicy == 1 || perm == QLatin1String("rw");
    }
};

#include "TransferTypes.h"

#include <QCoreApplication>

namespace Qdl {
namespace TransferFmt {

namespace {
QString tr(const char *text)
{
    return QCoreApplication::translate("Transfer", text);
}
} // namespace

QString sizeText(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("—");

    constexpr double kb = 1024.0;
    constexpr double mb = kb * 1024.0;
    constexpr double gb = mb * 1024.0;

    const double value = double(bytes);
    if (value >= gb)
        return QStringLiteral("%1 GB").arg(value / gb, 0, 'f', 2);
    if (value >= mb)
        return QStringLiteral("%1 MB").arg(value / mb, 0, 'f', 1);
    if (value >= kb)
        return QStringLiteral("%1 KB").arg(value / kb, 0, 'f', 0);
    return QStringLiteral("%1 B").arg(bytes);
}

QString speedText(double bytesPerSecond)
{
    if (bytesPerSecond < 1.0)
        return QStringLiteral("—");
    return sizeText(qint64(bytesPerSecond)) + QStringLiteral("/s");
}

QString etaText(qint64 remainBytes, double bytesPerSecond)
{
    if (remainBytes <= 0)
        return QString();
    if (bytesPerSecond < 1024.0)
        return QStringLiteral("—");

    const int seconds = int(remainBytes / bytesPerSecond);
    if (seconds < 60)
        return tr("剩余 %1 秒").arg(seconds);
    if (seconds < 3600)
        return tr("剩余 %1 分 %2 秒").arg(seconds / 60).arg(seconds % 60);
    return tr("剩余 %1 小时 %2 分").arg(seconds / 3600).arg((seconds % 3600) / 60);
}

QString stateCode(TransferState state)
{
    switch (state) {
    case TransferState::Queued:       return QStringLiteral("queued");
    case TransferState::Connecting:   return QStringLiteral("connecting");
    case TransferState::Transferring: return QStringLiteral("transferring");
    case TransferState::Verifying:    return QStringLiteral("verifying");
    case TransferState::Completed:    return QStringLiteral("completed");
    case TransferState::Paused:       return QStringLiteral("paused");
    case TransferState::Failed:       return QStringLiteral("failed");
    case TransferState::Canceled:     return QStringLiteral("canceled");
    }
    return QStringLiteral("queued");
}

QString stateText(TransferState state)
{
    switch (state) {
    case TransferState::Queued:       return tr("排队中");
    case TransferState::Connecting:   return tr("连接中…");
    case TransferState::Transferring: return tr("传输中");
    case TransferState::Verifying:    return tr("校验中…");
    case TransferState::Completed:    return tr("已完成");
    case TransferState::Paused:       return tr("已暂停");
    case TransferState::Failed:       return tr("失败");
    case TransferState::Canceled:     return tr("已取消");
    }
    return {};
}

} // namespace TransferFmt
} // namespace Qdl

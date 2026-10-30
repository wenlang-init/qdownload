#include "PeerInfo.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonValue>

namespace {
constexpr int kProtocolVersion = 1;

// PeerInfo 是纯数据结构（值类型，会进 QVariant 与容器），不便继承 QObject，
// 所以翻译走静态 translate；context 统一用 "PeerInfo"，方便在 .ts 里定位。
QString tr(const char *text)
{
    return QCoreApplication::translate("PeerInfo", text);
}
}

QJsonObject PeerInfo::toAnnounce() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("app"), QStringLiteral("qdownload"));
    obj.insert(QStringLiteral("proto"), kProtocolVersion);
    obj.insert(QStringLiteral("t"), QStringLiteral("ANNOUNCE"));
    obj.insert(QStringLiteral("peerId"), peerId);
    obj.insert(QStringLiteral("name"), name);
    obj.insert(QStringLiteral("platform"), platform);
    obj.insert(QStringLiteral("ctrlPort"), int(ctrlPort));
    obj.insert(QStringLiteral("sharing"), sharing);
    obj.insert(QStringLiteral("write"), write);
    obj.insert(QStringLiteral("policy"), uploadPolicy);
    obj.insert(QStringLiteral("version"), version);
    obj.insert(QStringLiteral("ts"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    return obj;
}

PeerInfo PeerInfo::fromAnnounce(const QJsonObject &obj, const QString &fromAddress)
{
    PeerInfo peer;
    peer.peerId     = obj.value(QStringLiteral("peerId")).toString();
    peer.name       = obj.value(QStringLiteral("name")).toString();
    peer.platform   = obj.value(QStringLiteral("platform")).toString();
    peer.address    = fromAddress;
    peer.ctrlPort   = quint16(obj.value(QStringLiteral("ctrlPort")).toInt());
    peer.sharing    = obj.value(QStringLiteral("sharing")).toBool();
    peer.write      = obj.value(QStringLiteral("write")).toBool();
    peer.uploadPolicy = obj.value(QStringLiteral("policy")).toInt();
    peer.version    = obj.value(QStringLiteral("version")).toString();
    peer.lastSeen   = QDateTime::currentDateTimeUtc();
    peer.online     = true;
    return peer;
}

QString PeerInfo::statusKind() const
{
    if (!online)
        return QStringLiteral("offline");
    if (!sharing)
        return QStringLiteral("unshared");
    return write ? QStringLiteral("upload") : QStringLiteral("readonly");
}

QString PeerInfo::statusBadge() const
{
    const QString kind = statusKind();
    if (kind == QLatin1String("offline"))
        return tr("离线");
    if (kind == QLatin1String("unshared"))
        return tr("未共享");
    if (kind == QLatin1String("upload"))
        return tr("可上传");
    return tr("只读");
}

QString PeerInfo::displayName() const
{
    QString base = name;
    if (base.isEmpty())
        base = address.isEmpty() ? tr("未知主机") : address;
    return tr("%1（%2）").arg(base, statusBadge());
}

QString PeerInfo::subtitle() const
{
    QStringList parts;
    if (!address.isEmpty()) {
        parts << (ctrlPort > 0 ? QStringLiteral("%1:%2").arg(address).arg(ctrlPort)
                               : address);
    }
    if (!platform.isEmpty())
        parts << platform;
    if (!version.isEmpty())
        parts << QStringLiteral("v%1").arg(version);
    return parts.join(QStringLiteral(" · "));
}

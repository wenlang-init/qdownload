#pragma once

#include "PeerInfo.h"

#include <QAbstractListModel>
#include <QVector>

/**
 * 主机列表模型，直接给 QML 的 ComboBox / ListView 用。
 *
 * 排序规则与 DiscoveryService::peers() 一致：在线 → 可访问 → 名称。
 * 只有排序键（在线/共享/名称）变化时才 reset，普通心跳更新（lastSeen、write 等）
 * 只发 dataChanged，避免 ComboBox 反复重置选中项。
 */
class PeerListModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        PeerIdRole = Qt::UserRole + 1,
        NameRole,
        DisplayNameRole,
        PlatformRole,
        AddressRole,
        CtrlPortRole,
        SharingRole,
        WriteRole,
        UploadPolicyRole,
        OnlineRole,
        VersionRole,
        StatusBadgeRole,
        StatusKindRole,
        SubtitleRole,
        LastSeenRole
    };
    Q_ENUM(Roles)

    explicit PeerListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void upsert(const PeerInfo &peer);
    void remove(const QString &peerId);
    void clear();

    int indexOf(const QString &peerId) const;
    PeerInfo peerAt(int row) const;
    Q_INVOKABLE QString peerIdAt(int row) const;
    Q_INVOKABLE int count() const { return m_peers.size(); }

private:
    QVector<PeerInfo> m_peers;
};

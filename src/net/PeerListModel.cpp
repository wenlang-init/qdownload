#include "PeerListModel.h"

#include <algorithm>

namespace {

bool sortKeyChanged(const PeerInfo &a, const PeerInfo &b)
{
    return a.online != b.online || a.sharing != b.sharing || a.displayName() != b.displayName();
}

bool lessThan(const PeerInfo &a, const PeerInfo &b)
{
    if (a.online != b.online)
        return a.online;
    if (a.sharing != b.sharing)
        return a.sharing;
    return QString::localeAwareCompare(a.displayName(), b.displayName()) < 0;
}

} // namespace

PeerListModel::PeerListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int PeerListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_peers.size());
}

QVariant PeerListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_peers.size())
        return {};

    const PeerInfo &peer = m_peers.at(index.row());
    switch (role) {
    case PeerIdRole:       return peer.peerId;
    case NameRole:         return peer.name;
    case DisplayNameRole:  return peer.displayName();
    case PlatformRole:     return peer.platform;
    case AddressRole:      return peer.address;
    case CtrlPortRole:     return peer.ctrlPort;
    case SharingRole:      return peer.sharing;
    case WriteRole:        return peer.write;
    case UploadPolicyRole: return peer.uploadPolicy;
    case OnlineRole:       return peer.online;
    case VersionRole:      return peer.version;
    case StatusBadgeRole:  return peer.statusBadge();
    case StatusKindRole:   return peer.statusKind();
    case SubtitleRole:     return peer.subtitle();
    case LastSeenRole:     return peer.lastSeen.toLocalTime().toString(QStringLiteral("HH:mm:ss"));
    case Qt::DisplayRole:  return peer.displayName();
    default:               return {};
    }
}

QHash<int, QByteArray> PeerListModel::roleNames() const
{
    return {
        { PeerIdRole,       "peerId" },
        { NameRole,         "name" },
        { DisplayNameRole,  "displayName" },
        { PlatformRole,     "platform" },
        { AddressRole,      "address" },
        { CtrlPortRole,     "ctrlPort" },
        { SharingRole,      "sharing" },
        { WriteRole,        "write" },
        { UploadPolicyRole, "uploadPolicy" },
        { OnlineRole,       "online" },
        { VersionRole,      "version" },
        { StatusBadgeRole,  "statusBadge" },
        { StatusKindRole,   "statusKind" },
        { SubtitleRole,     "subtitle" },
        { LastSeenRole,     "lastSeen" },
    };
}

int PeerListModel::indexOf(const QString &peerId) const
{
    for (int i = 0; i < m_peers.size(); ++i) {
        if (m_peers.at(i).peerId == peerId)
            return i;
    }
    return -1;
}

PeerInfo PeerListModel::peerAt(int row) const
{
    if (row < 0 || row >= m_peers.size())
        return {};
    return m_peers.at(row);
}

QString PeerListModel::peerIdAt(int row) const
{
    return peerAt(row).peerId;
}

void PeerListModel::upsert(const PeerInfo &peer)
{
    if (!peer.isValid())
        return;

    const int row = indexOf(peer.peerId);
    if (row < 0) {
        int pos = 0;
        while (pos < m_peers.size() && lessThan(m_peers.at(pos), peer))
            ++pos;
        beginInsertRows(QModelIndex(), pos, pos);
        m_peers.insert(pos, peer);
        endInsertRows();
        return;
    }

    const PeerInfo previous = m_peers.at(row);
    m_peers[row] = peer;

    if (!sortKeyChanged(previous, peer)) {
        emit dataChanged(index(row), index(row));
        return;
    }

    // 排序键变了（上线/掉线、共享开关、改名）→ 重排。主机数量是个位数，直接 reset 最稳。
    beginResetModel();
    std::stable_sort(m_peers.begin(), m_peers.end(), lessThan);
    endResetModel();
}

void PeerListModel::remove(const QString &peerId)
{
    const int row = indexOf(peerId);
    if (row < 0)
        return;
    beginRemoveRows(QModelIndex(), row, row);
    m_peers.removeAt(row);
    endRemoveRows();
}

void PeerListModel::clear()
{
    if (m_peers.isEmpty())
        return;
    beginResetModel();
    m_peers.clear();
    endResetModel();
}

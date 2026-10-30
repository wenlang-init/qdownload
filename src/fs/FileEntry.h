#pragma once

#include <QJsonObject>
#include <QMetaType>
#include <QString>

namespace Qdl {

/// 目录里的一项。协议层直接序列化这个结构（LIST_RESP.entries）。
struct FileEntry
{
    QString name;
    bool isDir = false;
    qint64 size = 0;      // 字节；目录固定 0
    qint64 mtime = 0;     // epoch 秒，跨平台统一用秒避免毫秒精度差异导致续传判定抖动

    QJsonObject toJson() const;
    static FileEntry fromJson(const QJsonObject &obj);

    /// "1.2 MB" / "—"（目录）
    QString sizeText() const;
    /// "2026-10-23 19:20"
    QString mtimeText() const;
    /// 文件扩展名（小写，无点）；目录返回空
    QString extension() const;
};

} // namespace Qdl

Q_DECLARE_METATYPE(Qdl::FileEntry)

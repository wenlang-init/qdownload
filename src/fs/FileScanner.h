#pragma once

#include "FileEntry.h"

#include <QString>
#include <QVector>

namespace Qdl {

/**
 * 目录扫描与文件指纹（设计文档 §6.1）。
 *
 * 排序在服务端完成（目录优先 + 本地化名称比较），客户端不再二次排序——
 * 否则分页会错位：第 2 页可能插到第 1 页前面。
 */
class FileScanner
{
public:
    static constexpr int kMaxLimit = 1000;
    static constexpr int kDefaultLimit = 200;

    /// 列目录一页。entries 已排序；total 是过滤后的总条数（用于客户端判断还有没有下一页）。
    static bool list(const QString &absoluteDir, int offset, int limit,
                     QVector<FileEntry> *page, int *total, QString *error = nullptr);

    /// 上传中的临时文件后缀：对列表隐藏，避免半成品被当成正式文件下载
    inline static const QString kPartSuffix = QStringLiteral(".qdpart");

    /**
     * 文件指纹 `k-xxxxxxxxxxxxxxxx`，用于断点续传判定"是不是同一个文件"。
     *
     * 内容取 size + mtime + 首 1MB + 末 1MB 的 SHA-256：
     * 大文件不必全量读，但改名、改内容、被其他文件替换都能识别出来。
     * 代价是**列目录时不计算**（200 个文件各读 2MB 会让列表卡死），
     * 只在真正发起传输时算一次并缓存。
     */
    static QString fileKey(const QString &absolutePath, QString *error = nullptr);

    /**
     * 全量 SHA-256（hex）。断点续传"三个一致"里的最后一道：大小对上不代表内容对。
     *
     * 这个调用会读完整个文件，不要在 UI 线程里对 GB 级文件直接调——
     * 客户端是自己分片算的（TransferTask::verifyStep），服务端走后台线程（ChecksumCache）。
     */
    static QString sha256(const QString &absolutePath, QString *error = nullptr);
};

} // namespace Qdl

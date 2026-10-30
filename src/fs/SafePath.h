#pragma once

#include <QString>
#include <QStringList>

namespace Qdl {

/**
 * 虚拟路径 ↔ 真实路径的转换与校验（设计文档 §9 安全）。
 *
 * 协议里的路径永远是**虚拟路径**：以 `/` 开头、`/` 分隔、语义是"相对共享根"。
 * 客户端无法表达任何绝对路径，这是防目录穿越的第一道（也是最重要的一道）闸门。
 *
 * 校验分三层，任何一层不过都拒绝：
 *   1. 语法层 —— 拒绝 `..`、反斜杠、盘符、控制字符、超长段
 *   2. 规范化层 —— `canonicalFilePath()` 解析符号链接后再比对前缀
 *   3. 归属层 —— 结果必须落在共享根内（含根自身）
 */
class SafePath
{
public:
    /// 把任意输入规范成合法虚拟路径。失败返回空串并写入 error。
    static QString normalize(const QString &path, QString *error = nullptr);

    /// 虚拟路径 → 共享根下的真实绝对路径。目录必须存在（符号链接会在这一步被解引用后校验）。
    static QString resolve(const QString &root, const QString &virtualPath,
                           QString *error = nullptr);

    /// 真实绝对路径 → 虚拟路径。不在根内返回空串。
    static QString toVirtual(const QString &root, const QString &absolutePath);

    /// 父目录的虚拟路径（根的父亲是根自己，避免界面出现无效的“上一级”）
    static QString parent(const QString &virtualPath);

    /// 拼接子项。name 必须是单段合法名（不含分隔符）。
    static QString child(const QString &virtualPath, const QString &name);

    /// 单段名称是否可直接用于落盘 / 展示
    static bool isValidName(const QString &name);

    /// 路径各段，用于面包屑
    static QStringList segments(const QString &virtualPath);
};

} // namespace Qdl

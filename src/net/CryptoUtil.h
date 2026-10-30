#pragma once

#include <QByteArray>
#include <QString>

namespace Qdl {

/// 随机十六进制串（默认 16 字节 = 32 个字符），用于 nonce、tokenId
QString randomHex(int byteCount);

/// 32 字节长期令牌，hex 编码后存库与传输
QString randomToken();

/// HMAC-SHA256，返回小写 hex
QString hmacHex(const QByteArray &key, const QByteArray &data);

/// 定长比较，避免按字符短路带来的时序侧信道
bool constantTimeEquals(const QString &a, const QString &b);

} // namespace Qdl

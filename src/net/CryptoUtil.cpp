#include "CryptoUtil.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>

namespace Qdl {

QString randomHex(int byteCount)
{
    if (byteCount <= 0)
        return {};

    QByteArray bytes;
    bytes.resize(byteCount);
    for (int i = 0; i < byteCount; ++i)
        bytes[i] = char(QRandomGenerator::system()->generate() & 0xFF);
    return QString::fromLatin1(bytes.toHex());
}

QString randomToken()
{
    return randomHex(32);   // 256 bit
}

QString hmacHex(const QByteArray &key, const QByteArray &data)
{
    QMessageAuthenticationCode mac(QCryptographicHash::Sha256);
    mac.setKey(key);
    mac.addData(data);
    return QString::fromLatin1(mac.result().toHex());
}

bool constantTimeEquals(const QString &a, const QString &b)
{
    const QByteArray left = a.toUtf8();
    const QByteArray right = b.toUtf8();
    if (left.size() != right.size())
        return false;

    unsigned char diff = 0;
    for (int i = 0; i < left.size(); ++i)
        diff |= static_cast<unsigned char>(left.at(i) ^ right.at(i));
    return diff == 0;
}

} // namespace Qdl

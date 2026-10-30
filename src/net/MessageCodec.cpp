#include "MessageCodec.h"

#include <QJsonDocument>
#include <QJsonParseError>

namespace Qdl {

namespace {
constexpr char kMagic[4] = { 'Q', 'D', 'L', '1' };

struct TypeName
{
    MsgType type;
    const char *name;
};

const TypeName kTypeNames[] = {
    { MsgType::Hello,         "HELLO" },
    { MsgType::HelloAck,      "HELLO_ACK" },
    { MsgType::AuthReq,       "AUTH_REQ" },
    { MsgType::AuthResp,      "AUTH_RESP" },
    { MsgType::PairReq,       "PAIR_REQ" },
    { MsgType::PairChallenge, "PAIR_CHALLENGE" },
    { MsgType::PairResp,      "PAIR_RESP" },
    { MsgType::PairOk,        "PAIR_OK" },
    { MsgType::PairFail,      "PAIR_FAIL" },
    { MsgType::PermUpdate,    "PERM_UPDATE" },
    { MsgType::ListReq,       "LIST_REQ" },
    { MsgType::ListResp,      "LIST_RESP" },
    { MsgType::Ping,          "PING" },
    { MsgType::Pong,          "PONG" },
    { MsgType::Error,         "ERROR" },
    { MsgType::DownloadReq,    "DOWNLOAD_REQ" },
    { MsgType::DownloadAccept, "DOWNLOAD_ACCEPT" },
    { MsgType::Cancel,         "CANCEL" },
    { MsgType::UploadReq,      "UPLOAD_REQ" },
    { MsgType::UploadAccept,   "UPLOAD_ACCEPT" },
    { MsgType::ChecksumReq,    "CHECKSUM_REQ" },
    { MsgType::ChecksumResp,   "CHECKSUM_RESP" },
    { MsgType::UploadDone,     "UPLOAD_DONE" },
    { MsgType::SegReq,         "SEG_REQ" },
    { MsgType::SegResp,        "SEG_RESP" },
    { MsgType::ChecksumPush,   "CHECKSUM_PUSH" },
    { MsgType::DeleteReq,      "DELETE_REQ" },
    { MsgType::DeleteResp,     "DELETE_RESP" },
};
} // namespace

QByteArray MessageCodec::encode(MsgType type, const QJsonObject &payload)
{
    QJsonObject body = payload;
    body.insert(QStringLiteral("type"), typeName(type));

    const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QByteArray frame;
    frame.reserve(kHeaderSize + json.size());
    frame.append(kMagic, 4);
    frame.append(char(type));
    frame.append(char((json.size() >> 24) & 0xFF));
    frame.append(char((json.size() >> 16) & 0xFF));
    frame.append(char((json.size() >> 8) & 0xFF));
    frame.append(char(json.size() & 0xFF));
    frame.append(json);
    return frame;
}

QVector<Frame> MessageCodec::decode(QByteArray &buffer, QString *error)
{
    QVector<Frame> frames;

    while (buffer.size() >= kHeaderSize) {
        if (buffer.left(4) != QByteArray(kMagic, 4)) {
            if (error)
                *error = QStringLiteral("魔数不匹配，可能不是 qdownload 连接");
            buffer.clear();
            return frames;
        }

        const quint8 typeValue = quint8(buffer.at(4));
        const quint32 length = (quint32(quint8(buffer.at(5))) << 24)
                             | (quint32(quint8(buffer.at(6))) << 16)
                             | (quint32(quint8(buffer.at(7))) << 8)
                             |  quint32(quint8(buffer.at(8)));

        if (length > quint32(kMaxPayload)) {
            if (error)
                *error = QStringLiteral("帧长 %1 超过上限").arg(length);
            buffer.clear();
            return frames;
        }

        if (quint32(buffer.size() - kHeaderSize) < length)
            break;   // 还没收全，等下一批

        const QByteArray json = buffer.mid(kHeaderSize, int(length));
        buffer.remove(0, kHeaderSize + int(length));

        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            if (error)
                *error = QStringLiteral("JSON 解析失败：%1").arg(parseError.errorString());
            continue;
        }

        Frame frame;
        frame.type = MsgType(typeValue);
        frame.payload = doc.object();
        frames.append(frame);
    }

    return frames;
}

QString MessageCodec::typeName(MsgType type)
{
    for (const auto &entry : kTypeNames) {
        if (entry.type == type)
            return QString::fromLatin1(entry.name);
    }
    return QStringLiteral("UNKNOWN(%1)").arg(int(type));
}

MsgType MessageCodec::typeFromName(const QString &name, bool *ok)
{
    for (const auto &entry : kTypeNames) {
        if (name == QLatin1String(entry.name)) {
            if (ok)
                *ok = true;
            return entry.type;
        }
    }
    if (ok)
        *ok = false;
    return MsgType::Error;
}

} // namespace Qdl

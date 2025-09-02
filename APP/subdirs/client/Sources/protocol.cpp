#include "protocol.h"

static const int kLenFieldSize = 4; // uint32 length（大端）
static const int kTypeSize     = 2; // uint16
static const int kJsonSizeSize = 4; // uint32

QByteArray buildPacket(quint16 type,
                       const QJsonObject& json,
                       const QByteArray& bin)
{
    QByteArray jsonBytes = toJsonBytes(json);
    quint32 jsonSize = static_cast<quint32>(jsonBytes.size());
    quint32 length = static_cast<quint32>(kTypeSize + kJsonSizeSize + jsonSize + bin.size());

    QByteArray out;
    out.reserve(kLenFieldSize + length);

    QDataStream ds(&out, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::BigEndian);

    ds << length;   // 4B: 后续总长度（从type开始）
    ds << type;     // 2B: 消息类型
    ds << jsonSize; // 4B: JSON长度
    if (!jsonBytes.isEmpty())
        ds.writeRawData(jsonBytes.constData(), jsonBytes.size());
    if (!bin.isEmpty())
        ds.writeRawData(bin.constData(), bin.size());

    return out;
}

bool drainPackets(QByteArray& buffer, QVector<Packet>& out)
{
    bool produced = false;

    for (;;) {
        if (buffer.size() < kLenFieldSize) break;

        quint32 length = 0;
        {
            QDataStream peek(buffer.left(kLenFieldSize));
            peek.setByteOrder(QDataStream::BigEndian);
            peek >> length;
        }

        if (length < static_cast<quint32>(kTypeSize + kJsonSizeSize) ||
            length > kMaxPacketLen) {
            buffer.clear();
            break;
        }

        const int totalNeed = kLenFieldSize + static_cast<int>(length);
        if (buffer.size() < totalNeed) break;

        QByteArray block = buffer.left(totalNeed);
        buffer.remove(0, totalNeed);

        QDataStream ds(block);
        ds.setByteOrder(QDataStream::BigEndian);

        quint32 lenField = 0; ds >> lenField; Q_UNUSED(lenField);
        quint16 type = 0;     ds >> type;
        quint32 jsonSize = 0; ds >> jsonSize;

        const int payloadBytes = totalNeed - kLenFieldSize - kTypeSize - kJsonSizeSize;
        if (jsonSize > static_cast<quint32>(payloadBytes) || jsonSize > kMaxJsonLen) {
            continue;
        }

        QByteArray jsonBytes(jsonSize, Qt::Uninitialized);
        if (jsonSize > 0) {
            ds.readRawData(jsonBytes.data(), jsonBytes.size());
        }
        QByteArray bin;
        const int binSize = payloadBytes - static_cast<int>(jsonSize);
        if (binSize > 0) {
            bin = block.right(binSize);
        }

        Packet pkt;
        pkt.type = type;
        pkt.json = fromJsonBytes(jsonBytes);
        pkt.bin  = bin;
        out.push_back(std::move(pkt));
        produced = true;
    }

    return produced;
}
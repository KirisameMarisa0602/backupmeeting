#pragma once
// ===============================================
// common/protocol.h
// 统一协议（打包/拆包）最小实现
// 结构: [uint32 length][uint16 type][uint32 jsonSize][jsonBytes][bin...]
// - length: 从 type 开始的总字节数（大端序）
// - type  : 消息类型（见枚举 MsgType）
// - jsonSize: JSON字节长度（UTF-8, Compact）
// - jsonBytes: 固定存在的JSON（至少包含roomId/ts等字段）
// - bin   : 可选二进制负载（如JPEG/PCM）
// ===============================================

#include <QtCore>
#include <QtNetwork>

enum MsgType : quint16 {
    MSG_REGISTER         = 1,
    MSG_LOGIN            = 2,
    MSG_CREATE_WORKORDER = 3,
    MSG_JOIN_WORKORDER   = 4,

    MSG_TEXT             = 10,
    MSG_DEVICE_DATA      = 20,
    MSG_VIDEO_FRAME      = 30,  // bin: JPEG
    MSG_AUDIO_FRAME      = 40,  // 预留
    MSG_CONTROL          = 50,  // 控制/状态，如 {kind:"video", state:"on/off"}

    MSG_SERVER_EVENT     = 90,   // 服务器事件，如房间成员列表

    MSG_FILE            = 60,  // ：文件/图片传输（bin 载荷）

    MSG_DEVICE_CONTROL   = 100, // 设备控制广播
};

// 安全上限（防御异常/恶意输入）
constexpr quint32 kMaxPacketLen = 8u * 1024u * 1024u; // 8MB
constexpr quint32 kMaxJsonLen   = 1u * 1024u * 1024u; // 1MB

// 一条完整消息
struct Packet {
    quint16 type = 0;
    QJsonObject json;
    QByteArray bin; // 可为空
};

inline QByteArray toJsonBytes(const QJsonObject& j) {
    return QJsonDocument(j).toJson(QJsonDocument::Compact);
}
inline QJsonObject fromJsonBytes(const QByteArray& b) {
    auto doc = QJsonDocument::fromJson(b);
    return doc.isObject() ? doc.object() : QJsonObject{};
}

QByteArray buildPacket(quint16 type,
                       const QJsonObject& json,
                       const QByteArray& bin = QByteArray());

bool drainPackets(QByteArray& buffer, QVector<Packet>& out);

// 标注消息类型
static const quint16 MSG_ANNOT = 1206;



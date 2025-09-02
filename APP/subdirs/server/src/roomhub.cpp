#include "roomhub.h"
#include "recorder.h"

RoomHub::RoomHub(QObject* parent) : QObject(parent) {}

bool RoomHub::start(quint16 port) {
    connect(&server_, &QTcpServer::newConnection, this, &RoomHub::onNewConnection);
    if (!server_.listen(QHostAddress::Any, port)) {
        qWarning() << "Listen failed on port" << port << ":" << server_.errorString();
        return false;
    }
    qInfo() << "Server listening on" << server_.serverAddress().toString() << ":" << port;
    return true;
}

void RoomHub::onNewConnection() {
    while (server_.hasPendingConnections()) {
        QTcpSocket* sock = server_.nextPendingConnection();
        auto* ctx = new ClientCtx;
        ctx->sock = sock;
        clients_.insert(sock, ctx);
        connect(sock, &QTcpSocket::readyRead, this, &RoomHub::onReadyRead);
        connect(sock, &QTcpSocket::disconnected, this, &RoomHub::onDisconnected);
    }
}

void RoomHub::onDisconnected() {
    auto* sock = qobject_cast<QTcpSocket*>(sender());
    if (!sock) return;
    auto it = clients_.find(sock);
    if (it == clients_.end()) return;
    ClientCtx* c = it.value();

    const QString oldRoom = c->roomId;
    if (!oldRoom.isEmpty()) {
        auto range = rooms_.equal_range(oldRoom);
        for (auto i = range.first; i != range.second; ) {
            if (i.value() == sock) i = rooms_.erase(i);
            else ++i;
        }
        broadcastRoomMembers(oldRoom, "leave", c->user);
    }

    clients_.erase(it);
    sock->deleteLater();
    delete c;
}

void RoomHub::onReadyRead() {
    auto* sock = qobject_cast<QTcpSocket*>(sender());
    if (!sock) return;
    auto it = clients_.find(sock);
    if (it == clients_.end()) return;
    ClientCtx* c = it.value();

    c->buffer.append(sock->readAll());
    QVector<Packet> pkts;
    if (drainPackets(c->buffer, pkts)) {
        for (const Packet& p : pkts) handlePacket(c, p);
    }
}

void RoomHub::handlePacket(ClientCtx* c, const Packet& p) {
    if (p.type == MSG_JOIN_WORKORDER) {
        const QString roomId = p.json.value("roomId").toString();
        const QString user   = p.json.value("user").toString();
        if (roomId.isEmpty()) {
            QJsonObject j{{"code",400},{"message","roomId required"}};
            c->sock->write(buildPacket(MSG_SERVER_EVENT, j));
            return;
        }
        c->user = user;
        joinRoom(c, roomId);

        QJsonObject ack{{"code",0},{"message","joined"},{"roomId",roomId}};
        c->sock->write(buildPacket(MSG_SERVER_EVENT, ack));

        sendRoomMembersTo(c->sock, roomId, "snapshot", c->user);
        broadcastRoomMembers(roomId, "join", c->user);
        return;
    }

    if (c->roomId.isEmpty()) {
        QJsonObject j{{"code",403},{"message","join a room first"}};
        c->sock->write(buildPacket(MSG_SERVER_EVENT, j));
        return;
    }

    // 录制服务同步 TCP 包（视频帧、标注等）
    if (recorder_) recorder_->onPacketTCP(c->roomId, p);

    if (p.type == MSG_TEXT ||
        p.type == MSG_DEVICE_DATA ||
        p.type == MSG_VIDEO_FRAME ||
        p.type == MSG_AUDIO_FRAME ||
        p.type == MSG_CONTROL ||
        p.type == MSG_ANNOT ||
        p.type == MSG_FILE ||
        p.type == MSG_DEVICE_CONTROL)  // 新增：设备控制广播
    {
        if (p.type == MSG_VIDEO_FRAME) {
            const QString sender = p.json.value("sender").toString();
            const QString media  = p.json.value("media").toString("camera");
            qInfo() << "[hub]" << "video pkt"
                    << "room=" << c->roomId
                    << "sender=" << sender
                    << "media=" << media
                    << "bytes=" << p.bin.size();
        } else if (p.type == MSG_DEVICE_CONTROL) {
            qInfo() << "[hub][device_control]"
                    << "room="   << c->roomId
                    << "sender=" << p.json.value("sender").toString()
                    << "device=" << p.json.value("device").toString()
                    << "cmd="    << p.json.value("command").toString();
        }

        QByteArray raw = buildPacket(p.type, p.json, p.bin);
        const bool isVideo = (p.type == MSG_VIDEO_FRAME);
        broadcastToRoom(c->roomId, raw, c->sock, isVideo);
        return;
    }

    QJsonObject j{{"code",404},{"message",QString("unknown type %1").arg(p.type)}};
    c->sock->write(buildPacket(MSG_SERVER_EVENT, j));
}

void RoomHub::joinRoom(ClientCtx* c, const QString& roomId) {
    if (!c->roomId.isEmpty()) {
        auto range = rooms_.equal_range(c->roomId);
        for (auto i = range.first; i != range.second; ) {
            if (i.value() == c->sock) i = rooms_.erase(i);
            else ++i;
        }
    }
    c->roomId = roomId;
    rooms_.insert(roomId, c->sock);
}

void RoomHub::broadcastToRoom(const QString& roomId,
                              const QByteArray& packet,
                              QTcpSocket* except,
                              bool dropVideoIfBacklog) {
    auto range = rooms_.equal_range(roomId);
    for (auto i = range.first; i != range.second; ++i) {
        QTcpSocket* s = i.value();
        if (s == except) continue;
        if (dropVideoIfBacklog && s->bytesToWrite() > kBacklogDropThreshold) {
            continue;
        }
        s->write(packet);
    }
}

QStringList RoomHub::listMembers(const QString& roomId) const {
    QStringList members;
    auto range = rooms_.equal_range(roomId);
    for (auto i = range.first; i != range.second; ++i) {
        QTcpSocket* s = i.value();
        if (!clients_.contains(s)) continue;
        auto* c = clients_.value(s);
        if (!c->user.isEmpty()) members << c->user;
        else members << QString("peer-%1").arg(reinterpret_cast<quintptr>(s));
    }
    members.removeDuplicates();
    members.sort();
    return members;
}

void RoomHub::broadcastRoomMembers(const QString& roomId, const QString& event, const QString& whoChanged) {
    QJsonObject j{
        {"code", 0},
        {"kind", "room"},
        {"event", event},
        {"roomId", roomId},
        {"who", whoChanged},
        {"members", QJsonArray::fromStringList(listMembers(roomId))},
        {"ts", QDateTime::currentMSecsSinceEpoch()}
    };
    QByteArray pkt = buildPacket(MSG_SERVER_EVENT, j);

    // 通知录制服务最新成员
    if (recorder_) recorder_->onServerEventMembers(roomId, listMembers(roomId));

    broadcastToRoom(roomId, pkt, nullptr, false);
}

void RoomHub::sendRoomMembersTo(QTcpSocket* target, const QString& roomId, const QString& event, const QString& whoChanged) {
    if (!target) return;
    QJsonObject j{
        {"code", 0},
        {"kind", "room"},
        {"event", event},
        {"roomId", roomId},
        {"who", whoChanged},
        {"members", QJsonArray::fromStringList(listMembers(roomId))},
        {"ts", QDateTime::currentMSecsSinceEpoch()}
    };
    target->write(buildPacket(MSG_SERVER_EVENT, j));
}

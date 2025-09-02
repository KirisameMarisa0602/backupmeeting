#pragma once
#include <QtCore>
#include <QtNetwork>
#include "protocol.h"

class RecorderService; // 前向声明

struct ClientCtx {
    QTcpSocket* sock = nullptr;
    QString user;
    QString roomId;
    QByteArray buffer;
};

class RoomHub : public QObject {
    Q_OBJECT
public:
    explicit RoomHub(QObject* parent=nullptr);
    bool start(quint16 port);

    // 注入录制服务
    void setRecorder(RecorderService* r) { recorder_ = r; }

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();

private:
    QTcpServer server_;
    QHash<QTcpSocket*, ClientCtx*> clients_;
    QMultiHash<QString, QTcpSocket*> rooms_; // roomId -> sockets

    static constexpr qint64 kBacklogDropThreshold = 3 * 1024 * 1024; // 3MB

    void handlePacket(ClientCtx* c, const Packet& p);
    void joinRoom(ClientCtx* c, const QString& roomId);
    void broadcastToRoom(const QString& roomId,
                         const QByteArray& packet,
                         QTcpSocket* except = nullptr,
                         bool dropVideoIfBacklog = false);

    QStringList listMembers(const QString& roomId) const;
    void broadcastRoomMembers(const QString& roomId, const QString& event, const QString& whoChanged);
    void sendRoomMembersTo(QTcpSocket* target, const QString& roomId, const QString& event, const QString& whoChanged);

    RecorderService* recorder_{nullptr};
};

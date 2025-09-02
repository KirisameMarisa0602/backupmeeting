#include "clientconn.h"

ClientConn::ClientConn(QObject* parent) : QObject(parent) {
    connect(&sock_, &QTcpSocket::readyRead,   this, &ClientConn::onReadyRead);
    connect(&sock_, &QTcpSocket::connected,   this, &ClientConn::onConnected);
    connect(&sock_, &QTcpSocket::disconnected,this, &ClientConn::onDisconnected);
    connect(&sock_, SIGNAL(error(QAbstractSocket::SocketError)),
            this,   SLOT(onError(QAbstractSocket::SocketError)));
}

void ClientConn::connectTo(const QString& host, quint16 port) {
    sock_.connectToHost(host, port);
}

void ClientConn::send(quint16 type, const QJsonObject& json, const QByteArray& bin) {
    if (sock_.state() == QAbstractSocket::ConnectedState) {
        sock_.write(buildPacket(type, json, bin));
    }
}

// 新增：主动断开
void ClientConn::disconnectFromServer() {
    if (sock_.state() == QAbstractSocket::ConnectedState ||
        sock_.state() == QAbstractSocket::ConnectingState) {
        sock_.disconnectFromHost();
        // 不阻塞等待，交由 onDisconnected 信号通知 UI
    }
}

void ClientConn::onConnected()    { emit connected(); }
void ClientConn::onDisconnected() { emit disconnected(); }

void ClientConn::onReadyRead() {
    buf_.append(sock_.readAll());
    QVector<Packet> pkts;
    if (drainPackets(buf_, pkts)) {
        for (auto& p : pkts) emit packetArrived(p);
    }
}

void ClientConn::onError(QAbstractSocket::SocketError) {
    // 可在 UI 层读取 sock_.errorString() 打印
}

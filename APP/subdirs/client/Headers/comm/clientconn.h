#pragma once
#include <QtCore>
#include <QtNetwork>
#include "protocol.h"

class ClientConn : public QObject {
    Q_OBJECT
public:
    explicit ClientConn(QObject* parent=nullptr);
    void connectTo(const QString& host, quint16 port);
    void send(quint16 type, const QJsonObject& json, const QByteArray& bin = QByteArray());

    // 新增：主动断开与服务器的连接
    void disconnectFromServer();

    bool isConnected() const { return sock_.state() == QAbstractSocket::ConnectedState; }
    qint64 bytesToWrite() const { return sock_.bytesToWrite(); }

signals:
    void connected();
    void disconnected();
    void packetArrived(Packet pkt);

private slots:
    void onReadyRead();
    void onConnected();
    void onDisconnected();
    void onError(QAbstractSocket::SocketError);

private:
    QTcpSocket sock_;
    QByteArray buf_;
};

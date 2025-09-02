#include "udprelay.h"

UdpRelay::UdpRelay(QObject* parent) : QObject(parent)
{
    cleanup_.setInterval(5000);
    connect(&cleanup_, &QTimer::timeout, this, &UdpRelay::onCleanup);
}

bool UdpRelay::start(quint16 port)
{
    if (!sock_.bind(QHostAddress::AnyIPv4, port, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        qWarning() << "[UDP] bind failed on" << port << sock_.errorString();
        return false;
    }
    port_ = port;
    connect(&sock_, &QUdpSocket::readyRead, this, &UdpRelay::onReadyRead);
    cleanup_.start();
    qInfo() << "[UDP] relay listening on" << port_;
    return true;
}

bool UdpRelay::parseHeader(QDataStream& ds, quint8& ver, quint8& type)
{
    ds.setByteOrder(QDataStream::BigEndian);
    quint32 magic=0; quint16 reserved=0;
    ds >> magic >> ver >> type >> reserved;
    if (ds.status()!=QDataStream::Ok) return false;
    if (magic != kMagic) return false;
    if (ver != 1 && ver != 2) return false; // 兼容 v1/v2
    return true;
}

void UdpRelay::onReadyRead()
{
    while (sock_.hasPendingDatagrams()) {
        QByteArray d;
        d.resize(int(sock_.pendingDatagramSize()));
        QHostAddress from; quint16 port=0;
        sock_.readDatagram(d.data(), d.size(), &from, &port);

        QDataStream ds(d);
        quint8 ver=0, type=0;
        if (!parseHeader(ds, ver, type)) continue;

        if (type == 1) {
            // register
            QString room, user;
            ds >> room >> user;
            if (ds.status()!=QDataStream::Ok) continue;
            auto& m = rooms_[room];
            Peer p; p.addr = from; p.port = port; p.lastSeen = QDateTime::currentMSecsSinceEpoch();
            m.insert(user, p);
        } else if (type == 2) {
            // video chunk - 转发给房间内其他用户
            QString room, sender;
            ds >> room >> sender;
            if (ds.status()!=QDataStream::Ok) continue;

            const auto now = QDateTime::currentMSecsSinceEpoch();
            auto it = rooms_.find(room);
            if (it != rooms_.end()) {
                for (auto pit = it->begin(); pit != it->end(); ++pit) {
                    const Peer& peer = pit.value();
                    if (now - peer.lastSeen > 10000) continue;
                    if (peer.addr == from && peer.port == port) continue;
                    sock_.writeDatagram(d, peer.addr, peer.port);
                }
            }
        }
    }
}

void UdpRelay::onCleanup()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList emptyRooms;
    for (auto it = rooms_.begin(); it != rooms_.end(); ++it) {
        QStringList rmUsers;
        for (auto pit = it->begin(); pit != it->end(); ++pit) {
            if (now - pit->lastSeen > 15000) rmUsers << pit.key();
        }
        for (const auto& u : rmUsers) it->remove(u);
        if (it->isEmpty()) emptyRooms << it.key();
    }
    for (const auto& k : emptyRooms) rooms_.remove(k);
}

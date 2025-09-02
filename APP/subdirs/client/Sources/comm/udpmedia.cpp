#include "udpmedia.h"
#include <QtGlobal>   // 为 qMin 提供声明

UdpMediaClient::UdpMediaClient(QObject* parent) : QObject(parent)
{
    connect(&sock_, &QUdpSocket::readyRead, this, &UdpMediaClient::onReadyRead);
    heartbeat_.setInterval(3000);
    connect(&heartbeat_, &QTimer::timeout, this, &UdpMediaClient::onHeartbeat);
    cleanup_.setInterval(1000);
    connect(&cleanup_, &QTimer::timeout, this, &UdpMediaClient::onCleanup);
}

void UdpMediaClient::configureServer(const QString& host, quint16 port) {
    serverAddr_ = QHostAddress(host);
    serverPort_ = port;
    if (sock_.state() != QAbstractSocket::BoundState) {
        sock_.bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    }
    if (!roomId_.isEmpty() && !user_.isEmpty()) sendRegister();
}

void UdpMediaClient::setIdentity(const QString& roomId, const QString& user) {
    roomId_ = roomId;
    user_ = user;
    if (serverPort_ != 0) sendRegister();
    heartbeat_.start();
    cleanup_.start();
}

void UdpMediaClient::stop() {
    heartbeat_.stop();
    cleanup_.stop();
    reassem_.clear();
}

void UdpMediaClient::sendRegister() {
    QByteArray d = buildRegister(roomId_, user_);
    sock_.writeDatagram(d, serverAddr_, serverPort_);
}

QByteArray UdpMediaClient::buildRegister(const QString& roomId, const QString& user) {
    QByteArray d;
    QDataStream ds(&d, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::BigEndian);
    ds << (quint32)kMagic << (quint8)2 /*ver*/ << (quint8)1 /*type*/ << (quint16)0;
    ds << roomId << user;
    return d;
}

QByteArray UdpMediaClient::buildVideoChunk(const QString& roomId, const QString& sender,
                                           quint32 frameId, quint16 idx, quint16 cnt,
                                           quint8 codec, int w, int h, qint64 ts,
                                           const char* payload, int len) {
    QByteArray d;
    d.reserve(64 + len);
    QDataStream ds(&d, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::BigEndian);
    ds << (quint32)kMagic << (quint8)2 /*ver*/ << (quint8)2 /*type*/ << (quint16)0;
    ds << roomId << sender;
    ds << (quint32)frameId << (quint16)idx << (quint16)cnt;
    ds << (quint8)codec;
    ds << (quint16)w << (quint16)h;
    ds << (quint64)ts;
    ds << (quint32)len;
    ds.writeRawData(payload, len);
    return d;
}

void UdpMediaClient::sendScreenJpeg(const QByteArray& jpeg, int w, int h, qint64 tsMs) {
    if (serverPort_ == 0 || roomId_.isEmpty() || user_.isEmpty() || jpeg.isEmpty()) return;
    const quint32 fid = ++frameSeq_;
    const int total = int((jpeg.size() + kChunkPayload - 1) / kChunkPayload); // 都转成 int
    const char* base = jpeg.constData();
    for (int i = 0; i < total; ++i) {
        const int off = i * kChunkPayload;
        const int remaining = int(jpeg.size()) - off;
        const int len = qMin<int>(kChunkPayload, remaining);      // 显式模板参数，避免类型不一致
        QByteArray d = buildVideoChunk(roomId_, user_, fid, (quint16)i, (quint16)total,
                                       (quint8)JPEG, w, h, tsMs, base + off, len);
        sock_.writeDatagram(d, serverAddr_, serverPort_);
    }
}

void UdpMediaClient::sendScreenDelta(const QByteArray& blob, int w, int h, qint64 tsMs) {
    if (serverPort_ == 0 || roomId_.isEmpty() || user_.isEmpty() || blob.isEmpty()) return;
    const quint32 fid = ++frameSeq_;
    const int total = int((blob.size() + kChunkPayload - 1) / kChunkPayload);
    const char* base = blob.constData();
    for (int i = 0; i < total; ++i) {
        const int off = i * kChunkPayload;
        const int remaining = int(blob.size()) - off;
        const int len = qMin<int>(kChunkPayload, remaining);
        QByteArray d = buildVideoChunk(roomId_, user_, fid, (quint16)i, (quint16)total,
                                       (quint8)DELTA, w, h, tsMs, base + off, len);
        sock_.writeDatagram(d, serverAddr_, serverPort_);
    }
}

void UdpMediaClient::onHeartbeat() {
    if (serverPort_ == 0 || roomId_.isEmpty() || user_.isEmpty()) return;
    sendRegister();
}

void UdpMediaClient::onCleanup() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList rm;
    for (auto it = reassem_.begin(); it != reassem_.end(); ++it) {
        if (now - it->startMs > 2000) rm << it.key();
    }
    for (const auto& k : rm) reassem_.remove(k);
}

void UdpMediaClient::onReadyRead() {
    while (sock_.hasPendingDatagrams()) {
        QByteArray d;
        d.resize(int(sock_.pendingDatagramSize()));
        QHostAddress from; quint16 port=0;
        sock_.readDatagram(d.data(), d.size(), &from, &port);
        parseDatagram(d, from, port);
    }
}

void UdpMediaClient::parseDatagram(const QByteArray& dgram, const QHostAddress&, quint16) {
    QDataStream ds(dgram);
    ds.setByteOrder(QDataStream::BigEndian);
    quint32 magic=0; quint8 ver=0; quint8 type=0; quint16 reserved=0;
    ds >> magic >> ver >> type >> reserved;
    if (magic != kMagic || (ver != 1 && ver != 2)) return;

    if (type == 2) {
        QString room, sender;
        quint32 fid=0; quint16 idx=0, cnt=0; quint16 w=0, h=0; quint64 ts=0; quint32 len=0;
        quint8 codec = 0; // 默认 JPEG
        ds >> room >> sender >> fid >> idx >> cnt;
        if (ver >= 2) {
            ds >> codec;
        }
        ds >> w >> h >> ts >> len;
        if (roomId_.isEmpty() || room != roomId_) return;
        if (ds.status() != QDataStream::Ok || int(dgram.size()) < ds.device()->pos() + (qint64)len) return;

        QByteArray payload;
        payload.resize(int(len));
        ds.readRawData(payload.data(), len);

        const QString key = sender + '|' + QString::number(fid);
        auto& as = reassem_[key];
        if (as.startMs == 0) {
            as.startMs = QDateTime::currentMSecsSinceEpoch();
            as.codec = codec;
            as.chunkCnt = cnt;
            as.w = w; as.h = h; as.ts = (qint64)ts;
            as.parts.resize(cnt);
            as.received = 0;
        }
        if (idx < as.parts.size() && as.parts[int(idx)].isEmpty()) {
            as.parts[int(idx)] = std::move(payload);
            as.received++;
        }
        if (as.received == as.chunkCnt) {
            QByteArray blob;
            blob.reserve(int(as.chunkCnt) * 1000);
            for (int i = 0; i < as.chunkCnt; ++i) blob.append(as.parts[i]);
            if (as.codec == DELTA) {
                emit udpScreenDeltaFrame(sender, blob, as.w, as.h, as.ts);
            } else {
                emit udpScreenFrame(sender, blob, as.w, as.h, as.ts);
            }
            reassem_.remove(key);
        }
    }
}

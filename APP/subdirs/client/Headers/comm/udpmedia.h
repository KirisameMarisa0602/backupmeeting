#pragma once
#include <QtCore>
#include <QtNetwork>
#include <algorithm>

class UdpMediaClient : public QObject {
    Q_OBJECT
public:
    enum Codec : quint8 { JPEG = 0, DELTA = 1 };

    explicit UdpMediaClient(QObject* parent=nullptr);

    void configureServer(const QString& host, quint16 port);
    void setIdentity(const QString& roomId, const QString& user);
    void stop();

    void sendScreenJpeg(const QByteArray& jpeg, int w, int h, qint64 tsMs = 0);
    void sendScreenDelta(const QByteArray& blob, int w, int h, qint64 tsMs = 0);

signals:
    void udpScreenFrame(const QString& sender, QByteArray jpeg, int w, int h, qint64 ts);
    void udpScreenDeltaFrame(const QString& sender, QByteArray blob, int w, int h, qint64 ts);

private slots:
    void onReadyRead();
    void onHeartbeat();
    void onCleanup();

private:
    struct Assembly {
        quint8  codec = 0;
        int     w=0, h=0;
        int     chunkCnt=0;
        qint64  ts=0;
        qint64  startMs=0;
        QVector<QByteArray> parts;
        int     received=0;
    };

    void sendRegister();
    void parseDatagram(const QByteArray& dgram, const QHostAddress& from, quint16 port);

    static QByteArray buildRegister(const QString& roomId, const QString& user);
    static QByteArray buildVideoChunk(const QString& roomId, const QString& sender,
                                      quint32 frameId, quint16 idx, quint16 cnt,
                                      quint8 codec, int w, int h, qint64 ts,
                                      const char* payload, int len);

    QUdpSocket sock_;
    QHostAddress serverAddr_{QHostAddress::LocalHost};
    quint16 serverPort_{0};
    QString roomId_;
    QString user_;
    QTimer heartbeat_;
    QTimer cleanup_;
    quint32 frameSeq_{0};
    QHash<QString, Assembly> reassem_;
    enum { kChunkPayload = 1200 };
    static constexpr quint32 kMagic = 0x55444D31;
};
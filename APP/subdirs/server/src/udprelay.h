#pragma once
#include <QtCore>
#include <QtNetwork>

class UdpRelay : public QObject {
    Q_OBJECT
public:
    explicit UdpRelay(QObject* parent=nullptr);

    bool start(quint16 port);
    quint16 port() const { return port_; }

private slots:
    void onReadyRead();
    void onCleanup();

private:
    struct Peer {
        QHostAddress addr;
        quint16 port=0;
        qint64 lastSeen=0;
    };
    // roomId -> user -> Peer
    QHash<QString, QHash<QString, Peer>> rooms_;
    QUdpSocket sock_;
    quint16 port_{0};
    QTimer cleanup_;

    // 统一的头部解析：三个参数（引用）
    static bool parseHeader(QDataStream& ds, quint8& ver, quint8& type);

    static constexpr quint32 kMagic = 0x55444D31; // 'UDM1'
};

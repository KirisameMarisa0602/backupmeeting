#pragma once
#include <QtCore>
#include <QtGui>
#include <QtMultimedia>
#include "clientconn.h"
#include "protocol.h"

class UdpMediaClient;

class ScreenShare : public QObject {
    Q_OBJECT
public:
    explicit ScreenShare(ClientConn* conn, QObject* parent=nullptr);

    void setIdentity(const QString& roomId, const QString& sender);
    void setUdpClient(UdpMediaClient* udp) { udp_ = udp; }

    void setEnabled(bool on);
    bool isEnabled() const { return enabled_; }

    void setParams(const QSize& sendBaseSize, int baseFps, int jpegQuality);

signals:
    void localFrameReady(QImage img);

private slots:
    void onTick();
    void onEncodedKeyframe(QByteArray jpeg, QSize wh, qint64 encodeMs);

private:
    void sendControl(const char* state);
    void scheduleNext();
    QSize clampMin720p(const QSize& in) const;
    QByteArray buildDeltaBlob(const QImage& prev, const QImage& curr, int block) const;

    ClientConn*     conn_{};
    UdpMediaClient* udp_{nullptr};
    QString roomId_;
    QString sender_;
    QTimer  timer_;
    int     intervalMs_{33};
    QSize   baseSendSize_{1280, 720};
    int     baseQuality_{50};
    QThread worker_;
    class KeyEncoder* encoder_{nullptr};
    QAtomicInt keyBusy_{0};
    bool    enabled_{false};
    qint64  lastKeyMs_{0};
    int     keyIntervalMs_{1000};
    QImage  prevFrame_;
};

class KeyEncoder : public QObject {
    Q_OBJECT
public:
    explicit KeyEncoder(int quality=50) : quality_(quality) {}
    void setQuality(int q){ quality_ = q; }
public slots:
    void encode(QImage img) {
        if (img.isNull()) { emit encoded(QByteArray(), QSize(), 0); return; }
        const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
        QByteArray jpeg;
        jpeg.reserve(img.width()*img.height()/6);
        QBuffer buf(&jpeg);
        buf.open(QIODevice::WriteOnly);
        QImageWriter w(&buf, "jpeg");
        w.setQuality(quality_);
        w.setOptimizedWrite(true);
        w.write(img);
        buf.close();
        const qint64 cost = QDateTime::currentMSecsSinceEpoch() - t0;
        emit encoded(jpeg, img.size(), cost);
    }
signals:
    void encoded(QByteArray jpeg, QSize wh, qint64 encodeMs);
private:
    int quality_{50};
};

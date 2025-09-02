#pragma once
#include <QtCore>
#include <QtNetwork>
#include <QtGui>
#include <QProcess>
#include "protocol.h"
#include "annot.h"
#include "udpmedia_client.h"

class RecorderStream : public QObject {
    Q_OBJECT
public:
    RecorderStream(const QString& roomId, const QString& user, const QString& outDir, int fps=12, QObject* parent=nullptr);
    ~RecorderStream();

    void setAnnotModel(AnnotModel* m) { annot_ = m; }

    void onCameraFrame(const QImage& img);
    void onScreenFrame(const QImage& img);

    // 现在：收到第一帧时再启动 ffmpeg
    void start();
    void stop();

    bool isActive() const { return active_; }
    QString outputPath() const { return outPath_; }

private slots:
    void onTick();

private:
    static QImage compose(const QImage& cam, const QImage& scr, const QSize& target);
    void writeFrame(const QImage& frame);
    void ensureFfmpegStarted();
    static QString findFfmpegExecutable();

    QString roomId_;
    QString user_;
    QString outDir_;
    QString outPath_;
    int fps_{12};
    QTimer timer_;
    QImage lastCam_;
    QImage lastScreen_;
    QProcess ff_;
    bool active_{false};
    bool ffmpegStarted_{false};
    int  writtenFrames_{0};
    AnnotModel* annot_{nullptr};
    QSize baseSize_{1280,720};
};

class RecorderRoom : public QObject {
    Q_OBJECT
public:
    RecorderRoom(const QString& roomId, quint16 udpPort, QObject* parent=nullptr);
    ~RecorderRoom();

    void membersUpdated(const QStringList& members);
    void onTcpPacket(const Packet& p);

    bool isEmpty() const { return currentMembers_.isEmpty(); }
    void finalizeAndClose();

private:
    void ensureStream(const QString& user);
    void handleAnnot(const QJsonObject& j);
    QImage parseDeltaIntoBack(const QString& sender, const QByteArray& blob, int w, int h);

    QString roomId_;
    QString outDir_;
    QSet<QString> currentMembers_;
    QHash<QString, RecorderStream*> streams_;
    QHash<QString, AnnotModel*> annotByUser_;
    QHash<QString, QImage> screenBack_;

    UdpMediaClient udp_;
    quint16 udpPort_{0};
};

class RecorderService : public QObject {
    Q_OBJECT
public:
    explicit RecorderService(QObject* parent=nullptr);

    void init(quint16 udpPort, const QString& kbRoot = QStringLiteral("knowledge"));

    // RoomHub hooks
    void onServerEventMembers(const QString& roomId, const QStringList& members);
    void onPacketTCP(const QString& roomId, const Packet& p);

private:
    QString kbRoot_;
    quint16 udpPort_{0};
    QHash<QString, RecorderRoom*> rooms_;
    void ensureTables();
};

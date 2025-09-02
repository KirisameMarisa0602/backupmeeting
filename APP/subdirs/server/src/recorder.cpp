#include "recorder.h"
#include <QImageReader>
#include <QImageWriter>
#include <QBuffer>
#include <QDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QStandardPaths>
#include <QFileInfo>
#include <QDebug>

// 可调整参数
static const int kOutFps = 12;
static const int kJpegQ  = 80;

QString RecorderStream::findFfmpegExecutable()
{
    const QByteArray envPath = qgetenv("FFMPEG_PATH");
    if (!envPath.isEmpty()) {
        QString p = QString::fromLocal8Bit(envPath);
        if (QFileInfo(p).isExecutable()) return p;
        QString p1 = QDir(p).filePath(
#ifdef Q_OS_WIN
            "ffmpeg.exe"
#else
            "ffmpeg"
#endif
        );
        if (QFileInfo(p1).isExecutable()) return p1;
    }

#ifdef Q_OS_WIN
    QString sys = QStandardPaths::findExecutable("ffmpeg.exe");
#else
    QString sys = QStandardPaths::findExecutable("ffmpeg");
#endif
    if (!sys.isEmpty() && QFileInfo(sys).isExecutable()) return sys;

    QStringList candidates;
#ifdef Q_OS_WIN
    candidates << "C:/ffmpeg/bin/ffmpeg.exe"
               << "C:/Program Files/ffmpeg/bin/ffmpeg.exe"
               << "C:/Program Files (x86)/ffmpeg/bin/ffmpeg.exe";
#else
    candidates << "/usr/bin/ffmpeg"
               << "/usr/local/bin/ffmpeg"
               << "/opt/homebrew/bin/ffmpeg"
               << "/opt/local/bin/ffmpeg";
#endif
    for (const QString& c : candidates) {
        if (QFileInfo(c).isExecutable()) return c;
    }
    return QString();
}

// ========== RecorderStream ==========
RecorderStream::RecorderStream(const QString& roomId, const QString& user, const QString& outDir, int fps, QObject* parent)
    : QObject(parent), roomId_(roomId), user_(user), outDir_(outDir), fps_(fps)
{
    timer_.setInterval(qMax(5, 1000 / qMax(1, fps_)));
    connect(&timer_, &QTimer::timeout, this, &RecorderStream::onTick);
}

RecorderStream::~RecorderStream() { stop(); }

void RecorderStream::start()
{
    if (active_) return;
    QDir().mkpath(outDir_);
    outPath_ = QDir(outDir_).filePath(QString("%1_%2.mp4").arg(roomId_, user_));
    active_ = true;
    ffmpegStarted_ = false;
    writtenFrames_ = 0;
    timer_.start();
    qInfo() << "[rec]" << roomId_ << user_ << "armed recorder; waiting first frame to start ffmpeg...";
}

void RecorderStream::stop()
{
    if (!active_) return;
    timer_.stop();
    if (ffmpegStarted_) {
        ff_.closeWriteChannel();
        ff_.waitForFinished(5000);
        qInfo() << "[rec]" << roomId_ << user_ << "stopped; frames=" << writtenFrames_ << "out=" << outPath_;
    } else {
        qInfo() << "[rec]" << roomId_ << user_ << "stopped before any frame arrived; no file written.";
        QFile f(outPath_);
        if (f.exists() && f.size()==0) f.remove();
    }
    active_ = false;
}

void RecorderStream::ensureFfmpegStarted()
{
    if (ffmpegStarted_) return;

    const QString ffmpegPath = findFfmpegExecutable();
    if (ffmpegPath.isEmpty()) {
        qWarning().noquote() << "[rec] ffmpeg not found. Please install ffmpeg or set FFMPEG_PATH.";
        active_ = false;
        return;
    }

    QStringList args;
    args << "-loglevel" << "error"
         << "-y"
         << "-f" << "image2pipe"
         << "-vcodec" << "mjpeg"
         << "-r" << QString::number(fps_)
         << "-i" << "pipe:0"
         << "-c:v" << "libx264"
         << "-pix_fmt" << "yuv420p"
         << "-movflags" << "+faststart"
         << outPath_;

    ff_.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&ff_, &QProcess::readyReadStandardError, this, [this](){
        QByteArray err = ff_.readAllStandardError();
        if (!err.isEmpty()) qWarning().noquote() << "[rec] ffmpeg stderr:" << QString::fromLocal8Bit(err).trimmed();
    });
    connect(&ff_, QOverload<QProcess::ProcessError>::of(&QProcess::errorOccurred), this, [this](QProcess::ProcessError e){
        qWarning() << "[rec] ffmpeg process error:" << e << ff_.errorString();
    });

    qInfo().noquote() << "[rec] starting ffmpeg:" << ffmpegPath << args.join(' ')
                      << "cwd=" << QDir::currentPath();

    ff_.start(ffmpegPath, args);
    if (!ff_.waitForStarted(5000)) {
        qWarning().noquote() << "[rec] ffmpeg start failed:" << ff_.errorString();
        active_ = false;
        return;
    }
    ffmpegStarted_ = true;
}

void RecorderStream::onCameraFrame(const QImage& img)
{
    if (!img.isNull()) {
        lastCam_ = img.convertToFormat(QImage::Format_RGB32);
        if (active_ && !ffmpegStarted_) ensureFfmpegStarted();
    }
}

void RecorderStream::onScreenFrame(const QImage& img)
{
    if (!img.isNull()) {
        lastScreen_ = img.convertToFormat(QImage::Format_RGB32);
        if (active_ && !ffmpegStarted_) ensureFfmpegStarted();
    }
}

void RecorderStream::onTick()
{
    if (!active_) return;

    if (lastCam_.isNull() && lastScreen_.isNull()) return;

    if (!ffmpegStarted_) {
        ensureFfmpegStarted();
        if (!ffmpegStarted_) return;
    }

    QImage frame = compose(lastCam_, lastScreen_, baseSize_);

    if (annot_) {
        QPainter p(&frame);
        annot_->paint(p, frame.size());
        p.end();
    }

    writeFrame(frame);
}

QImage RecorderStream::compose(const QImage& cam, const QImage& scr, const QSize& target)
{
    QImage out(target, QImage::Format_RGB32);
    out.fill(Qt::black);
    QPainter p(&out);
    p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform, true);

    auto drawFit = [&](const QImage& img, const QRect& rect){
        if (img.isNull() || rect.isEmpty()) return;
        QSize fitted = img.size(); fitted.scale(rect.size(), Qt::KeepAspectRatio);
        QPoint tl(rect.x() + (rect.width()-fitted.width())/2,
                  rect.y() + (rect.height()-fitted.height())/2);
        p.drawImage(QRect(tl, fitted), img);
    };

    if (!scr.isNull()) {
        drawFit(scr, QRect(QPoint(0,0), target));
        if (!cam.isNull()) {
            int margin=10;
            int sw = qMax(120, target.width()*22/100);
            int sh = sw * cam.height() / qMax(1, cam.width());
            if (sh > target.height()*30/100) {
                sh = target.height()*30/100;
                sw = sh * cam.width() / qMax(1, cam.height());
            }
            QRect rc(target.width()-margin-sw, target.height()-margin-sh, sw, sh);
            p.fillRect(rc.adjusted(-2,-2,2,2), QColor(0,0,0,160));
            p.setPen(QPen(Qt::white,2));
            p.drawRect(rc);
            drawFit(cam, rc);
        }
    } else if (!cam.isNull()) {
        drawFit(cam, QRect(QPoint(0,0), target));
    }
    p.end();
    return out;
}

void RecorderStream::writeFrame(const QImage& frame)
{
    if (!ff_.isWritable()) return;
    QByteArray jpg;
    QBuffer buf(&jpg); buf.open(QIODevice::WriteOnly);
    QImageWriter w(&buf, "jpeg");
    w.setQuality(kJpegQ);
    w.setOptimizedWrite(true);
    if (!w.write(frame)) {
        qWarning() << "[rec] jpeg encode failed:" << w.errorString();
        return;
    }
    buf.close();
    if (!jpg.isEmpty()) {
        ff_.write(jpg);
        ff_.waitForBytesWritten(10);
        ++writtenFrames_;
        if ((writtenFrames_ % 60) == 0) {
            qInfo() << "[rec]" << roomId_ << user_ << "written frames=" << writtenFrames_;
        }
    }
}

// ========== RecorderRoom ==========
RecorderRoom::RecorderRoom(const QString& roomId, quint16 udpPort, QObject* parent)
    : QObject(parent), roomId_(roomId), udpPort_(udpPort)
{
    outDir_ = QDir("knowledge").filePath(roomId_);
    QDir().mkpath(outDir_);

    udp_.configureServer("127.0.0.1", udpPort_);
    udp_.setIdentity(roomId_, QStringLiteral("__recorder__"));

    connect(&udp_, &UdpMediaClient::udpScreenFrame, this,
            [this](const QString& sender, const QByteArray& jpeg, int, int, qint64){
        QBuffer buf(const_cast<QByteArray*>(&jpeg));
        buf.open(QIODevice::ReadOnly);
        QImageReader r(&buf, "jpeg");
        r.setAutoTransform(true);
        QImage img = r.read().convertToFormat(QImage::Format_RGB32);
        if (img.isNull()) return;
        ensureStream(sender);
        streams_[sender]->onScreenFrame(img);
        screenBack_[sender] = img;
    });

    connect(&udp_, &UdpMediaClient::udpScreenDeltaFrame, this,
            [this](const QString& sender, const QByteArray& blob, int w, int h, qint64){
        QImage composed = parseDeltaIntoBack(sender, blob, w, h);
        if (composed.isNull()) return;
        ensureStream(sender);
        streams_[sender]->onScreenFrame(composed);
    });
}

RecorderRoom::~RecorderRoom()
{
    finalizeAndClose();
}

void RecorderRoom::membersUpdated(const QStringList& members)
{
    QSet<QString> next = QSet<QString>::fromList(members);
    currentMembers_ = next;

    for (const QString& u : currentMembers_) {
        ensureStream(u);
        if (!streams_[u]->isActive()) streams_[u]->start();
    }

    if (currentMembers_.isEmpty()) {
        finalizeAndClose();
    }
}

void RecorderRoom::onTcpPacket(const Packet& p)
{
    if (p.type == MSG_VIDEO_FRAME) {
        const QString sender = p.json.value("sender").toString();
        const QString media  = p.json.value("media").toString("camera");
        if (sender.isEmpty()) return;

        qInfo() << "[rec][tcp]" << roomId_ << "recv" << media << "frame from" << sender << "bytes=" << p.bin.size();

        QBuffer buf(const_cast<QByteArray*>(&p.bin));
        buf.open(QIODevice::ReadOnly);
        QImageReader r(&buf);
        r.setAutoTransform(true);
        QImage img = r.read().convertToFormat(QImage::Format_RGB32);

        if (img.isNull()) {
            qWarning().noquote() << "[rec][tcp]" << roomId_
                                 << "decode failed for" << media
                                 << "sender=" << sender
                                 << "bytes=" << p.bin.size()
                                 << "fmt=" << QString::fromLatin1(r.format())
                                 << "err=" << r.errorString();
            return;
        }

        ensureStream(sender);
        if (media == "screen") {
            streams_[sender]->onScreenFrame(img);
            screenBack_[sender] = img;
        } else {
            streams_[sender]->onCameraFrame(img);
        }
    } else if (p.type == MSG_ANNOT) {
        handleAnnot(p.json);
    }
}

void RecorderRoom::finalizeAndClose()
{
    for (auto it = streams_.begin(); it != streams_.end(); ++it) {
        it.value()->stop();
        it.value()->deleteLater();
    }
    streams_.clear();

    QSqlQuery q;
    q.exec("CREATE TABLE IF NOT EXISTS recordings (id INTEGER PRIMARY KEY AUTOINCREMENT, order_id TEXT, room_id TEXT, started_at INTEGER, ended_at INTEGER, title TEXT)");
    q.exec("CREATE TABLE IF NOT EXISTS recording_files (id INTEGER PRIMARY KEY AUTOINCREMENT, recording_id INTEGER, user TEXT, file_path TEXT, kind TEXT)");

    QSqlQuery qi;
    qi.prepare("INSERT INTO recordings(order_id, room_id, started_at, ended_at, title) VALUES(?, ?, ?, ?, ?)");
    qi.addBindValue(roomId_);
    qi.addBindValue(roomId_);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    qi.addBindValue(now);
    qi.addBindValue(now);
    qi.addBindValue(QString("会议录制 %1").arg(roomId_));
    if (!qi.exec()) {
        qWarning() << "[rec] insert recordings failed:" << qi.lastError();
        return;
    }
    int recId = qi.lastInsertId().toInt();

    QDir dir(outDir_);
    const QStringList files = dir.entryList(QStringList() << "*.mp4", QDir::Files);
    for (const QString& f : files) {
        QSqlQuery qf;
        qf.prepare("INSERT INTO recording_files(recording_id, user, file_path, kind) VALUES(?, ?, ?, ?)");
        qf.addBindValue(recId);
        QString u = f; u.chop(4); u = u.mid(u.indexOf('_')+1);
        qf.addBindValue(u);
        qf.addBindValue(dir.filePath(f));
        qf.addBindValue("video");
        if (!qf.exec()) qWarning() << "[rec] insert file failed:" << qf.lastError();
    }
}

void RecorderRoom::ensureStream(const QString& user)
{
    if (streams_.contains(user)) return;
    auto* st = new RecorderStream(roomId_, user, outDir_, kOutFps, this);
    auto* m = annotByUser_.value(user, nullptr);
    if (!m) { m = new AnnotModel(); annotByUser_.insert(user, m); }
    st->setAnnotModel(m);
    st->start();
    streams_.insert(user, st);
}

void RecorderRoom::handleAnnot(const QJsonObject& j)
{
    if (j.value("roomId").toString() != roomId_) return;
    QString target = j.value("target").toString();
    if (target.isEmpty()) return;
    if (target == QStringLiteral("__local__")) target = j.value("sender").toString();
    if (target.isEmpty()) return;
    auto* m = annotByUser_.value(target, nullptr);
    if (!m) { m = new AnnotModel(); annotByUser_.insert(target, m); }
    m->applyEvent(j);
}

QImage RecorderRoom::parseDeltaIntoBack(const QString& sender, const QByteArray& blob, int w, int h)
{
    QImage& back = screenBack_[sender];
    if (back.isNull() || back.size() != QSize(w, h)) {
        back = QImage(w, h, QImage::Format_RGB32);
        back.fill(Qt::black);
    }
    QDataStream ds(blob); ds.setByteOrder(QDataStream::BigEndian);
    quint32 magic=0; quint16 rectCount=0; ds >> magic >> rectCount;
    if (magic != 0x44533031) return QImage();
    for (int i = 0; i < rectCount; ++i) {
        quint16 x=0,y=0,rw=0,rh=0; quint32 clen=0;
        ds >> x >> y >> rw >> rh >> clen;
        if (ds.status()!=QDataStream::Ok) return QImage();
        if (int(ds.device()->bytesAvailable()) < int(clen)) return QImage();
        QByteArray comp; comp.resize(int(clen));
        ds.readRawData(comp.data(), clen);
        QByteArray raw = qUncompress(comp);
        if (raw.size() != int(rw) * int(rh) * 4) continue;
        const char* src = raw.constData();
        for (int row = 0; row < rh; ++row) {
            uchar* dst = back.scanLine(y + row) + x * 4;
            memcpy(dst, src + row * rw * 4, rw * 4);
        }
    }
    return back;
}

// ========== RecorderService ==========
RecorderService::RecorderService(QObject* parent) : QObject(parent) {}

void RecorderService::init(quint16 udpPort, const QString& kbRoot)
{
    udpPort_ = udpPort;
    kbRoot_ = kbRoot;
    QDir().mkpath(kbRoot_);
    ensureTables();
}

void RecorderService::ensureTables()
{
    QSqlQuery q;
    q.exec("CREATE TABLE IF NOT EXISTS recordings (id INTEGER PRIMARY KEY AUTOINCREMENT, order_id TEXT, room_id TEXT, started_at INTEGER, ended_at INTEGER, title TEXT)");
    q.exec("CREATE TABLE IF NOT EXISTS recording_files (id INTEGER PRIMARY KEY AUTOINCREMENT, recording_id INTEGER, user TEXT, file_path TEXT, kind TEXT)");
}

void RecorderService::onServerEventMembers(const QString& roomId, const QStringList& members)
{
    RecorderRoom* room = rooms_.value(roomId, nullptr);
    if (!room) {
        room = new RecorderRoom(roomId, udpPort_, this);
        rooms_.insert(roomId, room);
    }
    room->membersUpdated(members);

    if (room->isEmpty()) {
        room->finalizeAndClose();
        room->deleteLater();
        rooms_.remove(roomId);
    }
}

void RecorderService::onPacketTCP(const QString& roomId, const Packet& p)
{
    RecorderRoom* room = rooms_.value(roomId, nullptr);
    if (!room) {
        room = new RecorderRoom(roomId, udpPort_, this);
        rooms_.insert(roomId, room);
    }
    room->onTcpPacket(p);
}

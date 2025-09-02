#include "screenshare.h"
#include "udpmedia.h"

ScreenShare::ScreenShare(ClientConn* conn, QObject* parent)
    : QObject(parent), conn_(conn)
{
    encoder_ = new KeyEncoder(baseQuality_);
    encoder_->moveToThread(&worker_);
    connect(&worker_, &QThread::finished, encoder_, &QObject::deleteLater);
    connect(this, &ScreenShare::destroyed, &worker_, &QThread::quit);
    connect(encoder_, &KeyEncoder::encoded, this, &ScreenShare::onEncodedKeyframe, Qt::QueuedConnection);
    worker_.start(QThread::HighPriority);

    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &ScreenShare::onTick);
}

void ScreenShare::setIdentity(const QString& roomId, const QString& sender) {
    roomId_ = roomId; sender_ = sender;
}

void ScreenShare::setParams(const QSize& sendBaseSize, int baseFps, int jpegQuality) {
    QSize s = sendBaseSize.isValid() ? sendBaseSize : baseSendSize_;
    baseSendSize_ = clampMin720p(s);              // 强制不低于 1280x720
    intervalMs_   = qMax(5, 1000 / qMax(30, baseFps)); // 强制不低于 30fps
    baseQuality_  = qBound(35, jpegQuality, 75);  // 关键帧质量下限 35，避免糊成一片
    QMetaObject::invokeMethod(encoder_, [this]{ static_cast<KeyEncoder*>(encoder_)->setQuality(baseQuality_); }, Qt::QueuedConnection);
}

void ScreenShare::setEnabled(bool on) {
    if (enabled_ == on) return;
    enabled_ = on;
    if (enabled_) {
        sendControl("on");
        lastKeyMs_ = 0;
        prevFrame_ = QImage();
        scheduleNext();
    } else {
        timer_.stop();
        prevFrame_ = QImage();
        sendControl("off");
    }
}

void ScreenShare::sendControl(const char* state) {
    if (!conn_) return;
    QJsonObject j{
        {"roomId", roomId_},
        {"sender", sender_},
        {"kind",   "screen"},
        {"state",  QString::fromUtf8(state)},
        {"ts",     QDateTime::currentMSecsSinceEpoch()}
    };
    conn_->send(MSG_CONTROL, j);
}

QSize ScreenShare::clampMin720p(const QSize& in) const {
    QSize s = in.isValid() ? in : QSize(1280, 720);
    int w = s.width(), h = s.height();
    if (w < 1280 || h < 720) {
        // 维持原纵横比，放大到最小 1280x720
        double ar = w > 0 && h > 0 ? (double)w / h : (16.0/9.0);
        if (ar >= 16.0/9.0) { w = 1280; h = int(w / ar); if (h < 720) { h = 720; w = int(h * ar); } }
        else                { h = 720;  w = int(h * ar); if (w < 1280){ w = 1280; h = int(w / ar); } }
    }
    return QSize(w, h);
}

void ScreenShare::scheduleNext() {
    timer_.start(intervalMs_);
}

void ScreenShare::onTick() {
    if (!enabled_) return;

    // 抓取主屏并缩放到不低于 720p 的目标
    QScreen* scr = QGuiApplication::primaryScreen();
    if (!scr) { scheduleNext(); return; }

    QPixmap pix = scr->grabWindow(0);
    if (pix.isNull()) { scheduleNext(); return; }

    QSize target = clampMin720p(baseSendSize_);
    QPixmap scaledPix = pix.scaled(target, Qt::KeepAspectRatio, Qt::FastTransformation);
    QImage img = scaledPix.toImage().convertToFormat(QImage::Format_RGB32);
    if (img.isNull()) { scheduleNext(); return; }

    // 本地预览（720p 或更高）
    emit localFrameReady(img);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool needKey = (now - lastKeyMs_ >= keyIntervalMs_) || prevFrame_.isNull();

    if (!needKey && !prevFrame_.isNull()) {
        // 尝试增量帧：按块比较，生成 DS01 blob
        QByteArray blob = buildDeltaBlob(prevFrame_, img, /*block*/32);
        if (!blob.isEmpty() && udp_) {
            udp_->sendScreenDelta(blob, img.width(), img.height(), now);
            prevFrame_ = img;
            scheduleNext();
            return;
        }
        // 变化过大或生成失败 -> 回退关键帧
        needKey = true;
    }

    // 发关键帧（JPEG），编码异步
    if (udp_ && keyBusy_.loadAcquire() == 0) {
        keyBusy_.storeRelease(1);
        QMetaObject::invokeMethod(encoder_, "encode", Qt::QueuedConnection, Q_ARG(QImage, img));
        prevFrame_ = img; // 同步更新参考帧
    }
    scheduleNext();
}

void ScreenShare::onEncodedKeyframe(QByteArray jpeg, QSize wh, qint64 /*encodeMs*/) {
    keyBusy_.storeRelease(0);
    if (!enabled_) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (udp_ && !jpeg.isEmpty()) {
        udp_->sendScreenJpeg(jpeg, wh.width(), wh.height(), now);
        lastKeyMs_ = now;
    }
}

// DS01 blob：BigEndian
// u32 magic='DS01', u16 rectCount,
// [rectLoop] u16 x, u16 y, u16 w, u16 h, u32 compLen, [compData...]
// compData 是 QImage::Format_RGB32 的原始像素区域逐行拼接后 qCompress 得到
QByteArray ScreenShare::buildDeltaBlob(const QImage& prev, const QImage& curr, int block) const
{
    if (prev.size() != curr.size()) return QByteArray();

    const int W = curr.width(), H = curr.height();
    const int bw = qMax(8, block), bh = qMax(8, block);
    const int bx = (W + bw - 1) / bw;
    const int by = (H + bh - 1) / bh;

    QVector<QRect> rects;
    rects.reserve(bx * by / 4);

    // 粗粒度：逐块比较是否有变化（memcmp 快速判定）
    for (int gy = 0; gy < by; ++gy) {
        for (int gx = 0; gx < bx; ++gx) {
            int x = gx * bw;
            int y = gy * bh;
            int w = qMin(bw, W - x);
            int h = qMin(bh, H - y);
            bool diff = false;
            for (int row = 0; row < h; ++row) {
                const uchar* p0 = prev.constScanLine(y + row) + x * 4;
                const uchar* p1 = curr.constScanLine(y + row) + x * 4;
                if (memcmp(p0, p1, w * 4) != 0) { diff = true; break; }
            }
            if (diff) rects.push_back(QRect(x, y, w, h));
        }
    }

    if (rects.isEmpty()) {
        // 无变化：发一个极小的“空增量”，由接收端略过
        QByteArray blob;
        QDataStream ds(&blob, QIODevice::WriteOnly);
        ds.setByteOrder(QDataStream::BigEndian);
        ds << (quint32)0x44533031 /*'DS01'*/ << (quint16)0;
        return blob;
    }

    // 简单合并：把同一行相邻块合并成长条（降低 rect 数）
    std::sort(rects.begin(), rects.end(), [](const QRect& a, const QRect& b){
        if (a.y() == b.y()) return a.x() < b.x();
        return a.y() < b.y();
    });
    QVector<QRect> merged;
    for (const QRect& r : rects) {
        if (!merged.isEmpty()) {
            QRect& last = merged.last();
            if (last.y() == r.y() && last.height() == r.height() && last.right()+1 >= r.x()-1) {
                last.setRight(qMax(last.right(), r.right()));
                continue;
            }
        }
        merged.push_back(r);
    }

    // 限制最大 rect 数量，超出则返回空（触发关键帧）
    const int maxRects = 120;
    if (merged.size() > maxRects) return QByteArray();

    // 打包 DS01
    QByteArray blob;
    blob.reserve(merged.size() * 128);
    QDataStream ds(&blob, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::BigEndian);
    ds << (quint32)0x44533031 /*'DS01'*/ << (quint16)merged.size();

    for (const QRect& r : merged) {
        // 提取原始像素（逐行拼接）
        QByteArray raw;
        raw.reserve(r.width() * r.height() * 4);
        for (int row = 0; row < r.height(); ++row) {
            const uchar* src = curr.constScanLine(r.y() + row) + r.x() * 4;
            raw.append(reinterpret_cast<const char*>(src), r.width() * 4);
        }
        QByteArray comp = qCompress(raw, 6); // 压缩等级 1..9，6 性能/比率折中
        ds << (quint16)r.x() << (quint16)r.y() << (quint16)r.width() << (quint16)r.height();
        ds << (quint32)comp.size();
        ds.writeRawData(comp.constData(), comp.size());
    }
    return blob;
}

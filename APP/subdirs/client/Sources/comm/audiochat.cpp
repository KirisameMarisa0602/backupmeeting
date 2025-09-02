#include "audiochat.h"

static inline qint16 clamp16(int v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return static_cast<qint16>(v);
}

// µ-law 实现（G.711）
quint8 AudioChat::linearToUlaw(qint16 pcm) {
    const int BIAS = 0x84;
    const int CLIP = 32635;
    int sign = (pcm >> 8) & 0x80;
    if (sign) pcm = -pcm;
    if (pcm > CLIP) pcm = CLIP;
    pcm += BIAS;
    int exponent = 7;
    for (int expMask = 0x4000; (pcm & expMask) == 0 && exponent > 0; expMask >>= 1) {
        --exponent;
    }
    int mantissa = (pcm >> (exponent + 3)) & 0x0F;
    return static_cast<quint8>(~(sign | (exponent << 4) | mantissa));
}

qint16 AudioChat::ulawToLinear(quint8 u) {
    u = ~u;
    int t = ((u & 0x0F) << 3) + 0x84;
    t <<= ((u & 0x70) >> 4);
    return (u & 0x80) ? (0x84 - t) : (t - 0x84);
}

AudioChat::AudioChat(ClientConn* conn, QObject* parent)
    : QObject(parent), conn_(conn)
{
    // 混音定时器：按帧长输出
    mixTimer_.setInterval(kFrameMs);
    connect(&mixTimer_, &QTimer::timeout, this, &AudioChat::mixTick);
    mixTimer_.start();

    // 确保输出设备可用
    ensureOutput();
}

void AudioChat::setIdentity(const QString& roomId, const QString& sender) {
    roomId_ = roomId;
    sender_ = sender;
}

void AudioChat::setEnabled(bool on) {
    if (enabled_ == on) return;
    enabled_ = on;
    if (enabled_) startInput();
    else          stopInput();
    emit micStateChanged(enabled_);
}

void AudioChat::startInput() {
    if (audioIn_) return;

    QAudioFormat fmt;
    fmt.setSampleRate(kSampleRate);
    fmt.setChannelCount(kChannels);
    fmt.setSampleSize(16);
    fmt.setSampleType(QAudioFormat::SignedInt);
    fmt.setByteOrder(QAudioFormat::LittleEndian);
    fmt.setCodec("audio/pcm");
    inFmt_ = fmt;

    QAudioDeviceInfo devInfo = QAudioDeviceInfo::defaultInputDevice();
    if (!devInfo.isFormatSupported(fmt)) {
        inFmt_ = devInfo.nearestFormat(fmt);
        inFmt_.setCodec("audio/pcm");
    }
    audioIn_ = new QAudioInput(devInfo, inFmt_, this);
    audioIn_->setBufferSize(kPcmBytesPerFrm * 4);

    inDev_ = audioIn_->start();
    if (!inDev_) {
        qWarning() << "AudioInput start failed";
        delete audioIn_; audioIn_ = nullptr;
        return;
    }
    connect(inDev_, &QIODevice::readyRead, this, &AudioChat::onMicReadyRead);
}

void AudioChat::stopInput() {
    if (!audioIn_) return;
    disconnect(inDev_, &QIODevice::readyRead, this, &AudioChat::onMicReadyRead);
    audioIn_->stop();
    audioIn_->deleteLater();
    audioIn_ = nullptr;
    inDev_ = nullptr;
    inBuf_.clear();
}

void AudioChat::ensureOutput() {
    if (audioOut_) return;

    QAudioFormat fmt;
    fmt.setSampleRate(kSampleRate);
    fmt.setChannelCount(kChannels);
    fmt.setSampleSize(16);
    fmt.setSampleType(QAudioFormat::SignedInt);
    fmt.setByteOrder(QAudioFormat::LittleEndian);
    fmt.setCodec("audio/pcm");
    outFmt_ = fmt;

    QAudioDeviceInfo devInfo = QAudioDeviceInfo::defaultOutputDevice();
    if (!devInfo.isFormatSupported(fmt)) {
        outFmt_ = devInfo.nearestFormat(fmt);
        outFmt_.setCodec("audio/pcm");
    }
    audioOut_ = new QAudioOutput(devInfo, outFmt_, this);
    audioOut_->setBufferSize(kPcmBytesPerFrm * 20);
    outDev_ = audioOut_->start();
    if (!outDev_) {
        qWarning() << "AudioOutput start failed";
        delete audioOut_; audioOut_ = nullptr;
        return;
    }
}

void AudioChat::onMicReadyRead() {
    if (!inDev_ || roomId_.isEmpty() || sender_.isEmpty()) { if (inDev_) inDev_->readAll(); return; }

    inBuf_.append(inDev_->readAll());

    // 每帧按 20ms 发送
    while (inBuf_.size() >= kPcmBytesPerFrm) {
        QByteArray pcm = inBuf_.left(kPcmBytesPerFrm);
        inBuf_.remove(0, kPcmBytesPerFrm);

        // 应用本地麦克风增益并限幅（在编码前）
        qint16* s = reinterpret_cast<qint16*>(pcm.data());
        if (micGain_ != 1.0f) {
            for (int i = 0; i < kFrameSamples; ++i) {
                int v = static_cast<int>(s[i] * micGain_);
                s[i] = clamp16(v);
            }
        }

        // PCM16 -> µ-law
        QByteArray ulaw; ulaw.resize(kUlawBytesPerFrm);
        for (int i = 0; i < kFrameSamples; ++i) {
            ulaw[i] = static_cast<char>(linearToUlaw(s[i]));
        }

        // 组包并发送
        QJsonObject j{
            {"roomId", roomId_},
            {"sender", sender_},
            {"codec",  "mulaw"},
            {"sr",     kSampleRate},
            {"ch",     kChannels},
            {"seq",    static_cast<int>(seq_++)},
            {"ts",     QDateTime::currentMSecsSinceEpoch()}
        };
        if (conn_) conn_->send(MSG_AUDIO_FRAME, j, ulaw);
    }
}

void AudioChat::shrinkQueueIfNeeded(QByteArray& q) {
    const int maxBytes = kMaxQueueFrames * kPcmBytesPerFrm;
    if (q.size() > maxBytes) {
        q.remove(0, q.size() - maxBytes);
    }
}

void AudioChat::onPacket(Packet p) {
    if (p.type != MSG_AUDIO_FRAME) return;

    const QString roomId = p.json.value("roomId").toString();
    const QString sender = p.json.value("sender").toString();
    if (roomId.isEmpty() || sender.isEmpty()) return;
    if (!roomId_.isEmpty() && roomId != roomId_) return;
    if (!sender_.isEmpty() && sender == sender_) return;

    const QString codec = p.json.value("codec").toString("mulaw").toLower();
    const int sr = p.json.value("sr").toInt(kSampleRate);
    const int ch = p.json.value("ch").toInt(kChannels);
    if (sr != kSampleRate || ch != kChannels) {
        return;
    }

    QByteArray& q = rxQueues_[sender];
    if (codec == "mulaw") {
        const int n = p.bin.size();
        if (n <= 0) return;
        const uchar* u = reinterpret_cast<const uchar*>(p.bin.constData());
        QByteArray pcm; pcm.resize(n * 2);
        qint16* d = reinterpret_cast<qint16*>(pcm.data());
        for (int i = 0; i < n; ++i) d[i] = ulawToLinear(u[i]);
        q.append(pcm);
    } else if (codec == "pcm16") {
        q.append(p.bin);
    } else {
        return;
    }
    shrinkQueueIfNeeded(q);
}

void AudioChat::mixTick() {
    if (!audioOut_ || !outDev_) return;

    int bytesFree = audioOut_->bytesFree();
    while (bytesFree >= kPcmBytesPerFrm) {
        QByteArray out; out.resize(kPcmBytesPerFrm);
        qint16* outS = reinterpret_cast<qint16*>(out.data());
        for (int i = 0; i < kFrameSamples; ++i) outS[i] = 0;

        // 逐路读取并按各自增益混合
        for (auto it = rxQueues_.begin(); it != rxQueues_.end(); ++it) {
            const QString sender = it.key();
            const float   gain   = peerGain_.value(sender, 1.0f);
            QByteArray&   q      = it.value();

            if (q.size() >= kPcmBytesPerFrm) {
                const qint16* inS = reinterpret_cast<const qint16*>(q.constData());
                if (gain == 1.0f) {
                    for (int i = 0; i < kFrameSamples; ++i) {
                        int acc = static_cast<int>(outS[i]) + static_cast<int>(inS[i]);
                        outS[i] = clamp16(acc);
                    }
                } else if (gain > 0.0f) {
                    for (int i = 0; i < kFrameSamples; ++i) {
                        int acc = static_cast<int>(outS[i]) + static_cast<int>(inS[i] * gain);
                        outS[i] = clamp16(acc);
                    }
                } // gain==0 静音：跳过
                q.remove(0, kPcmBytesPerFrm);
            }
        }

        // 应用整体播放增益
        if (playbackGain_ != 1.0f) {
            for (int i = 0; i < kFrameSamples; ++i) {
                int v = static_cast<int>(outS[i] * playbackGain_);
                outS[i] = clamp16(v);
            }
        }

        qint64 w = outDev_->write(out);
        if (w <= 0) break;
        bytesFree -= static_cast<int>(w);
    }
}

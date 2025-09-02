#pragma once
#include <QtCore>
#include <QtNetwork>

class KbClient {
public:
    // 同步调用，返回 {"ok":bool, ...}
    static QJsonObject call(const QHostAddress& host, quint16 port, const QJsonObject& req, int msTimeout = 3000);

    static QJsonObject getRecordings(const QHostAddress& host, quint16 port,
                                     const QString& roomId = QString()) {
        QJsonObject req{{"action","get_recordings"}};
        if (!roomId.isEmpty()) req["room_id"] = roomId;
        return call(host, port, req);
    }

    static QJsonObject getRecordingFiles(const QHostAddress& host, quint16 port,
                                         int recordingId = 0, const QString& roomId = QString()) {
        QJsonObject req{{"action","get_recording_files"}};
        if (recordingId > 0) req["recording_id"] = recordingId;
        if (!roomId.isEmpty()) req["room_id"] = roomId;
        return call(host, port, req);
    }
};

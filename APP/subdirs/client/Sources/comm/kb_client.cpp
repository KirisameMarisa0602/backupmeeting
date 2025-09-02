#include "kb_client.h"

QJsonObject KbClient::call(const QHostAddress& host, quint16 port, const QJsonObject& req, int msTimeout)
{
    QTcpSocket s;
    s.connectToHost(host, port);
    if (!s.waitForConnected(msTimeout)) return QJsonObject{{"ok", false}, {"msg", "connect timeout"}};

    QByteArray line = QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n';
    s.write(line);
    s.flush();

    if (!s.waitForReadyRead(msTimeout)) return QJsonObject{{"ok", false}, {"msg", "no reply"}};
    QByteArray reply = s.readLine().trimmed();
    QJsonParseError pe{};
    auto doc = QJsonDocument::fromJson(reply, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
        return QJsonObject{{"ok", false}, {"msg", "bad json"}};
    return doc.object();
}

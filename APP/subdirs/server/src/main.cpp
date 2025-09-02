#include <QCoreApplication>
#include "roomhub.h"
#include "udprelay.h"
#include "recorder.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QDebug>
#include <QTime>
#include <QDateTime>

static const quint16 Port = 5555;
static const char* DB_FILE = "users.db";

static QString hashPass(const QString &pass) {
    return QString::fromLatin1(QCryptographicHash::hash(pass.toUtf8(), QCryptographicHash::Sha256).toHex());
}

static bool tableExists(const QString &name)
{
    QSqlQuery q;
    q.prepare("SELECT name FROM sqlite_master WHERE type='table' AND name=?");
    q.addBindValue(name);
    return q.exec() && q.next();
}

static bool migrateOldUsersToNewTables()
{
    if (!tableExists("users")) return true;
    QSqlQuery q;
    if (!q.exec("CREATE TABLE IF NOT EXISTS expert_users ( username TEXT PRIMARY KEY, password TEXT NOT NULL );")) {
        qCritical() << "Create expert_users failed:" << q.lastError().text();
        return false;
    }
    if (!q.exec("CREATE TABLE IF NOT EXISTS factory_users ( username TEXT PRIMARY KEY, password TEXT NOT NULL );")) {
        qCritical() << "Create factory_users failed:" << q.lastError().text();
        return false;
    }
    if (!q.exec("SELECT username, password, role FROM users")) {
        qCritical() << "Select legacy users failed:" << q.lastError().text();
        return false;
    }
    QSqlQuery qi;
    while (q.next()) {
        const QString u = q.value(0).toString();
        const QString p = q.value(1).toString();
        const QString r = q.value(2).toString();
        if (r == "expert") {
            qi.prepare("INSERT OR IGNORE INTO expert_users(username, password) VALUES(?, ?)");
            qi.addBindValue(u);
            qi.addBindValue(p);
            if (!qi.exec()) {
                qWarning() << "Migrate expert failed:" << qi.lastError().text();
            }
        } else if (r == "factory") {
            qi.prepare("INSERT OR IGNORE INTO factory_users(username, password) VALUES(?, ?)");
            qi.addBindValue(u);
            qi.addBindValue(p);
            if (!qi.exec()) {
                qWarning() << "Migrate factory failed:" << qi.lastError().text();
            }
        }
    }
    return true;
}

static void ensureOrdersTable()
{
    QSqlQuery q;
    q.exec("CREATE TABLE IF NOT EXISTS orders ("
           " id INTEGER PRIMARY KEY,"
           " title TEXT,"
           " desc TEXT,"
           " status TEXT,"
           " factory_user TEXT"
           ")");
}

static int generateRandomOrderId() {
    QSqlQuery q;
    for (int i = 0; i < 100; ++i) {
        int id = 1000 + qrand() % 9000;
        q.prepare("SELECT 1 FROM orders WHERE id=?");
        q.addBindValue(id);
        q.exec();
        if (!q.next()) return id;
    }
    return -1;
}

class AuthServer : public QObject {
    Q_OBJECT
public:
    explicit AuthServer(QObject *parent=nullptr) : QObject(parent) {}

    bool start() {
        if (!initDb()) return false;
        ensureOrdersTable();

        m_server = new QTcpServer(this);
        connect(m_server, &QTcpServer::newConnection, this, &AuthServer::onNewConnection);
        if (!m_server->listen(QHostAddress::Any, Port)) {
            qCritical() << "Listen failed:" << m_server->errorString();
            return false;
        }
        qInfo() << "Auth/Order server listening on port" << Port;
        return true;
    }

private slots:
    void onNewConnection() {
        while (m_server->hasPendingConnections()) {
            QTcpSocket *sock = m_server->nextPendingConnection();
            connect(sock, &QTcpSocket::readyRead, this, [this, sock](){
                while (sock->canReadLine()) {
                    QByteArray line = sock->readLine().trimmed();
                    if (line.isEmpty()) continue;
                    QJsonParseError pe{};
                    QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
                    QJsonObject reply;
                    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
                        reply = makeReply(false, "bad json");
                    } else {
                        reply = handle(doc.object());
                    }
                    QByteArray out = QJsonDocument(reply).toJson(QJsonDocument::Compact) + "\n";
                    sock->write(out);
                    sock->flush();
                }
            });
            connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
        }
    }

private:
    bool initDb() {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE");
        db.setDatabaseName(DB_FILE);
        if (!db.open()) {
            qCritical() << "Open DB failed:" << db.lastError().text();
            return false;
        }
        QSqlQuery q;
        if (!q.exec("CREATE TABLE IF NOT EXISTS expert_users ( username TEXT PRIMARY KEY, password TEXT NOT NULL );")) {
            qCritical() << "Create expert_users failed:" << q.lastError().text();
            return false;
        }
        if (!q.exec("CREATE TABLE IF NOT EXISTS factory_users ( username TEXT PRIMARY KEY, password TEXT NOT NULL );")) {
            qCritical() << "Create factory_users failed:" << q.lastError().text();
            return false;
        }
        if (!migrateOldUsersToNewTables()) {
            qWarning() << "Legacy users migration failed.";
        }
        return true;
    }

    bool existsInAny(const QString &username) {
        QSqlQuery q;
        q.prepare("SELECT 1 FROM expert_users WHERE username=? "
                  "UNION ALL SELECT 1 FROM factory_users WHERE username=? LIMIT 1");
        q.addBindValue(username);
        q.addBindValue(username);
        if (!q.exec()) return false;
        return q.next();
    }

    QJsonObject handle(const QJsonObject &req) {
        const QString action = req.value("action").toString();
        if (action == "register" || action == "login") {
            const QString role = req.value("role").toString();
            const QString username = req.value("username").toString().trimmed();
            const QString password = req.value("password").toString();

            if (role != "expert" && role != "factory") {
                return makeReply(false, "invalid role");
            }
            if (username.isEmpty() || password.isEmpty()) {
                return makeReply(false, "账号或密码为空");
            }

            if (action == "register") {
                return doRegister(username, role, password);
            } else if (action == "login") {
                return doLogin(username, role, password);
            } else {
                return makeReply(false, "unknown action");
            }
        }
        if (action == "new_order") {
            QString title = req.value("title").toString();
            QString desc = req.value("desc").toString();
            QString factory_user = req.value("factory_user").toString();
            int id = generateRandomOrderId();
            if (id < 0) return makeReply(false, "无法分配工单号");
            QSqlQuery q;
            q.prepare("INSERT INTO orders (id, title, desc, status, factory_user) VALUES (?, ?, ?, ?, ?)");
            q.addBindValue(id);
            q.addBindValue(title);
            q.addBindValue(desc);
            q.addBindValue("待处理");
            q.addBindValue(factory_user);
            if (q.exec()) return makeReply(true, "ok");
            else return makeReply(false, q.lastError().text());
        } else if (action == "get_orders") {
            QString role = req.value("role").toString();
            QString username = req.value("username").toString();
            QString keyword = req.value("keyword").toString();
            QString status = req.value("status").toString();
            QString sql = "SELECT id, title, desc, status, factory_user FROM orders WHERE 1=1";
            if (role == "factory" && !username.isEmpty()) {
                sql += " AND factory_user=?";
            }
            if (!keyword.isEmpty()) {
                sql += " AND (title LIKE ? OR desc LIKE ?)";
            }
            if (!status.isEmpty() && status != "全部") {
                sql += " AND status=?";
            }
            QSqlQuery q;
            q.prepare(sql);
            if (role == "factory" && !username.isEmpty()) q.addBindValue(username);
            if (!keyword.isEmpty()) {
                QString like = "%" + keyword + "%";
                q.addBindValue(like);
                q.addBindValue(like);
            }
            if (!status.isEmpty() && status != "全部") {
                q.addBindValue(status);
            }
            q.exec();
            QJsonArray arr;
            while (q.next()) {
                QJsonObject o;
                o["id"] = q.value(0).toInt();
                o["title"] = q.value(1).toString();
                o["desc"] = q.value(2).toString();
                o["status"] = q.value(3).toString();
                o["factory_user"] = q.value(4).toString();
                arr.append(o);
            }
            QJsonObject rep;
            rep["ok"] = true;
            rep["orders"] = arr;
            return rep;
        } else if (action == "update_order") {
            int id = req.value("id").toInt();
            QString status = req.value("status").toString();
            QSqlQuery q;
            q.prepare("UPDATE orders SET status=? WHERE id=?");
            q.addBindValue(status);
            q.addBindValue(id);
            if (q.exec()) return makeReply(true, "ok");
            else return makeReply(false, q.lastError().text());
        } else if (action == "delete_order") {
            int id = req.value("id").toInt();
            QString username = req.value("username").toString();
            QSqlQuery q;
            q.prepare("SELECT 1 FROM orders WHERE id=? AND factory_user=?");
            q.addBindValue(id);
            q.addBindValue(username);
            if (!q.exec() || !q.next()) {
                return makeReply(false, "只能销毁自己创建的工单");
            }
            q.prepare("DELETE FROM orders WHERE id=?");
            q.addBindValue(id);
            if (q.exec()) return makeReply(true, "ok");
            else return makeReply(false, q.lastError().text());
        } else if (action == "get_recordings") {
            QString roomId = req.value("room_id").toString();
            QString sql = "SELECT id, order_id, room_id, started_at, ended_at, title FROM recordings";
            if (!roomId.isEmpty()) sql += " WHERE room_id=?";
            QSqlQuery q;
            if (!roomId.isEmpty()) { q.prepare(sql); q.addBindValue(roomId); }
            else { q.prepare(sql); }
            if (!q.exec()) return makeReply(false, q.lastError().text());
            QJsonArray items;
            while (q.next()) {
                QJsonObject o;
                o["id"] = q.value(0).toInt();
                o["order_id"] = q.value(1).toString();
                o["room_id"] = q.value(2).toString();
                o["started_at"] = QJsonValue::fromVariant(q.value(3));
                o["ended_at"] = QJsonValue::fromVariant(q.value(4));
                o["title"] = q.value(5).toString();
                items.append(o);
            }
            QJsonObject rep; rep["ok"] = true; rep["items"] = items; return rep;
        } else if (action == "get_recording_files") {
            int recordingId = req.value("recording_id").toInt();
            QString roomId = req.value("room_id").toString();
            QString sql = "SELECT f.id, f.recording_id, f.user, f.file_path, f.kind "
                          "FROM recording_files f JOIN recordings r ON f.recording_id=r.id";
            QString where;
            if (recordingId > 0) {
                where = " WHERE f.recording_id=?";
            } else if (!roomId.isEmpty()) {
                where = " WHERE r.room_id=?";
            }
            QSqlQuery q;
            q.prepare(sql + where);
            if (recordingId > 0) q.addBindValue(recordingId);
            else if (!roomId.isEmpty()) q.addBindValue(roomId);

            if (!q.exec()) return makeReply(false, q.lastError().text());

            QJsonArray files;
            while (q.next()) {
                QJsonObject o;
                o["id"] = q.value(0).toInt();
                o["recording_id"] = q.value(1).toInt();
                o["user"] = q.value(2).toString();
                o["file_path"] = q.value(3).toString();
                o["kind"] = q.value(4).toString();
                files.append(o);
            }
            QJsonObject rep; rep["ok"] = true; rep["files"] = files; return rep;
        }
        return makeReply(false, "unknown action");
    }

    QJsonObject doRegister(const QString &user, const QString &role, const QString &pass) {
        if (existsInAny(user)) {
            return makeReply(false, "用户已存在");
        }
        QSqlQuery q;
        if (role == "expert") {
            q.prepare("INSERT INTO expert_users(username, password) VALUES(?, ?)");
        } else {
            q.prepare("INSERT INTO factory_users(username, password) VALUES(?, ?)");
        }
        q.addBindValue(user);
        q.addBindValue(hashPass(pass));
        if (!q.exec()) {
            qWarning() << "Insert failed:" << q.lastError().text();
            return makeReply(false, "数据库错误");
        }
        return makeReply(true, "ok");
    }

    QJsonObject doLogin(const QString &user, const QString &role, const QString &pass) {
        QSqlQuery q;
        if (role == "expert") {
            q.prepare("SELECT 1 FROM expert_users WHERE username=? AND password=? LIMIT 1");
        } else {
            q.prepare("SELECT 1 FROM factory_users WHERE username=? AND password=? LIMIT 1");
        }
        q.addBindValue(user);
        q.addBindValue(hashPass(pass));
        if (!q.exec()) {
            return makeReply(false, "数据库错误");
        }
        if (q.next()) return makeReply(true, "ok");
        return makeReply(false, "账号或密码不正确");
    }

    static QJsonObject makeReply(bool ok, const QString &msg) {
        return QJsonObject{{"ok", ok}, {"msg", msg}};
    }

private:
    QTcpServer *m_server = nullptr;
};

#include "main.moc"

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("rt-meeting-server");
    QCoreApplication::setApplicationVersion("1.0");

    // 随机数种子
    qsrand(QTime::currentTime().msec() ^ QDateTime::currentMSecsSinceEpoch());

    // 端口：TCP 用于信令/媒体，UDP 用于屏幕共享中继
    const quint16 tcpPort = 9000;
    const quint16 udpPort = tcpPort + 1;

    // 启动鉴权/工单/知识库查询服务
    AuthServer auth;
    if (!auth.start()) {
        qCritical() << "Auth/Order server start failed";
        return 1;
    }

    // 信令/转发
    RoomHub hub;
    if (!hub.start(tcpPort)) {
        return 1;
    }

    // 屏幕共享 UDP 中继
    UdpRelay udp;
    if (!udp.start(udpPort)) {
        return 1;
    }

    // 录制服务
    RecorderService recorder;
    recorder.init(/*udpPort*/ udpPort, /*kbRoot*/ QStringLiteral("knowledge"));
    hub.setRecorder(&recorder);

    return app.exec();
}

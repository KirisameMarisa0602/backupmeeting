#include "regist.h"
#include "ui_regist.h"
#include "login.h"
#include <QTcpSocket>
#include <QJsonObject>
#include <QJsonDocument>
#include <QMessageBox>
#include <QHostAddress>
#include <QComboBox>
#include <QLineEdit>

static const char* SERVER_HOST = "127.0.0.1";
static const quint16 SERVER_PORT = 5555;

Regist::Regist(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Regist)
{
    ui->setupUi(this);

    setWindowFlag(Qt::Window, true);
    setAttribute(Qt::WA_DeleteOnClose);

    ui->btnRegister->setProperty("primary", true);
    ui->cbRole->clear();
        ui->cbRole->addItem("请选择身份"); // 0
        ui->cbRole->addItem("专家");        // 1
        ui->cbRole->addItem("工厂");        // 2
        ui->cbRole->setCurrentIndex(0);

}

Regist::~Regist()
{
    delete ui;
}

void Regist::preset(const QString &role, const QString &user, const QString &pass)
{
    if (role == "expert") ui->cbRole->setCurrentIndex(1);
    else if (role == "factory") ui->cbRole->setCurrentIndex(2);
    else ui->cbRole->setCurrentIndex(0);

    ui->leUsername->setText(user);
    ui->lePassword->setText(pass);
    ui->leConfirm->clear();
}

QString Regist::selectedRole() const
{
    switch (ui->cbRole->currentIndex()) {
    case 1: return "expert";
    case 2: return "factory";
    default: return "";
    }
}

bool Regist::sendRequest(const QJsonObject &obj, QJsonObject &reply, QString *errMsg)
{
    QTcpSocket sock;
    sock.connectToHost(QHostAddress(QString::fromLatin1(SERVER_HOST)), SERVER_PORT);
    if (!sock.waitForConnected(3000)) {
        if (errMsg) *errMsg = "服务器连接失败";
        return false;
    }
    const QByteArray line = QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n";
    if (sock.write(line) == -1 || !sock.waitForBytesWritten(2000)) {
        if (errMsg) *errMsg = "请求发送失败";
        return false;
    }
    if (!sock.waitForReadyRead(5000)) {
        if (errMsg) *errMsg = "服务器无响应";
        return false;
    }
    QByteArray resp = sock.readAll();
    if (int nl = resp.indexOf('\n'); nl >= 0) resp = resp.left(nl);

    QJsonParseError pe{};
    QJsonDocument rdoc = QJsonDocument::fromJson(resp, &pe);
    if (pe.error != QJsonParseError::NoError || !rdoc.isObject()) {
        if (errMsg) *errMsg = "响应解析失败";
        return false;
    }
    reply = rdoc.object();
    return true;
}

void Regist::on_btnRegister_clicked()
{
    const QString username = ui->leUsername->text().trimmed();
    const QString password = ui->lePassword->text();
    const QString confirm  = ui->leConfirm->text();

    if (username.isEmpty() || password.isEmpty() || confirm.isEmpty()) {
        QMessageBox::warning(this, "提示", "请输入账号、密码与确认密码");
        return;
    }
    if (password != confirm) {
        QMessageBox::warning(this, "提示", "两次输入的密码不一致");
        return;
    }
    const QString role = selectedRole();
    if (role.isEmpty()) {
        QMessageBox::warning(this, "提示", "请选择身份");
        return;
    }

    QJsonObject req{
        {"action",  "register"},
        {"role",    role},
        {"username",username},
        {"password",password}
    };
    QJsonObject rep;
    QString err;
    if (!sendRequest(req, rep, &err)) {
        QMessageBox::warning(this, "注册失败", err);
        return;
    }
    if (!rep.value("ok").toBool(false)) {
        QMessageBox::warning(this, "注册失败", rep.value("msg").toString("未知错误"));
        return;
    }

    QMessageBox::information(this, "注册成功", "账号初始化完成");
    emit registered(username, role);
    close(); // 关闭注册窗口
}

void Regist::on_btnBack_clicked()
{
    close(); // 关闭注册窗口，登录窗口将由外部连接恢复显示
}

// 顶层打开注册窗口：隐藏登录窗口，注册窗口关闭时恢复
void openRegistDialog(QWidget *login, const QString &prefRole,
                      const QString &prefUser, const QString &prefPass)
{
    // 1) 创建为顶层窗口（无 parent）
    Regist *r = new Regist(nullptr);
    r->setAttribute(Qt::WA_DeleteOnClose);
    r->preset(prefRole, prefUser, prefPass);

    // 2) 隐藏登录窗口，避免残留
    if (login) login->hide();

    // 3) 注册成功时：回填登录界面并恢复显示
    QObject::connect(r, &Regist::registered, r, [login](const QString &u, const QString &role){
        if (!login) return;
        if (auto cb = login->findChild<QComboBox*>("cbRole")) {
            if (role == "expert") cb->setCurrentIndex(1);
            else if (role == "factory") cb->setCurrentIndex(2);
            else cb->setCurrentIndex(0);
        }
        if (auto leUser = login->findChild<QLineEdit*>("leUsername")) leUser->setText(u);
        if (auto lePass = login->findChild<QLineEdit*>("lePassword")) { lePass->clear(); lePass->setFocus(); }
        login->show(); login->raise(); login->activateWindow();
    });

    // 4) 无论何种方式关闭注册窗口，都恢复显示登录窗口
    QObject::connect(r, &QObject::destroyed, login, [login](){
        if (!login) return;
        login->show(); login->raise(); login->activateWindow();
    });

    // 5) 将注册窗口居中到登录窗口位置
    if (login) {
        const QRect lg = login->geometry();
        r->resize(r->sizeHint().expandedTo(QSize(680, 620))); // 与 UI 最小尺寸一致
        const QPoint p = lg.center() - QPoint(r->width()/2, r->height()/2);
        r->move(p);
    }

    r->show();
}

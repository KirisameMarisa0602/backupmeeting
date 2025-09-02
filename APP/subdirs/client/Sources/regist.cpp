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
#include <QRegularExpression>
#include <QStyle>
#include <QPushButton>
#include <QToolButton>
#include <QWidgetAction>

static const char* SERVER_HOST = "127.0.0.1";
static const quint16 SERVER_PORT = 5555;

static inline void repolish(QWidget* w) {
    if (!w) return;
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

// 同步把 roleTheme 设置到窗口本身以及“primary”按钮上，确保即时生效
static void applyRoleThemeTo(QWidget* root, const QString& key) {
    if (!root) return;
    root->setProperty("roleTheme", key);
    const auto buttons = root->findChildren<QPushButton*>();
    for (QPushButton* b : buttons) {
        if (b && b->property("primary").toBool()) {
            b->setProperty("roleTheme", key);
            repolish(b);
        }
    }
    repolish(root);
}

// 为密码输入框添加“眼睛”按钮，点击切换显示/隐藏
static void addPasswordToggle(QLineEdit* le) {
    if (!le) return;
    auto wa = new QWidgetAction(le);
    auto btn = new QToolButton(le);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setAutoRaise(true);
    btn->setText(QString::fromUtf8("👁"));
    btn->setToolTip(QString::fromUtf8("显示/隐藏密码"));
    wa->setDefaultWidget(btn);
    le->addAction(wa, QLineEdit::TrailingPosition);
    QObject::connect(btn, &QToolButton::clicked, le, [le](){
        le->setEchoMode(le->echoMode() == QLineEdit::Password ? QLineEdit::Normal
                                                              : QLineEdit::Password);
    });
}

Regist::Regist(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Regist)
{
    ui->setupUi(this);

    setWindowFlag(Qt::Window, true);
    setAttribute(Qt::WA_DeleteOnClose);

    // 注册页：主按钮声明为 primary（样式用）
    ui->btnRegister->setProperty("primary", true);

    // 角色下拉
    ui->cbRole->clear();
    ui->cbRole->addItem("请选择身份"); // 0
    ui->cbRole->addItem("专家");        // 1
    ui->cbRole->addItem("工厂");        // 2
    ui->cbRole->setCurrentIndex(0);

    // 初始主题：未选择 -> 灰色（同步到按钮）
    applyRoleThemeTo(this, QStringLiteral("none"));

    // 身份变化 -> 更新UI主题属性（不触碰任何功能逻辑）
    connect(ui->cbRole, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx){
                QString key = QStringLiteral("none");
                if (idx == 1) key = QStringLiteral("expert");
                else if (idx == 2) key = QStringLiteral("factory");
                applyRoleThemeTo(this, key);
            });

    // 为密码与确认密码添加“眼睛”按钮
    addPasswordToggle(ui->lePassword);
    addPasswordToggle(ui->leConfirm);
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

    // 预填后立即应用主题（同步到按钮）
    int idx = ui->cbRole->currentIndex();
    QString key = (idx==1) ? "expert" : (idx==2) ? "factory" : "none";
    applyRoleThemeTo(this, key);
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
    int nl = resp.indexOf('\n');
    if (nl >= 0) resp = resp.left(nl);

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

    // 密码格式校验：≥8 位，包含字母和数字，且仅字母数字（不含特殊字符）
    QRegularExpression re(QStringLiteral("^(?=.*[A-Za-z])(?=.*\\d)[A-Za-z\\d]{8,}$"));
    if (!re.match(password).hasMatch()) {
        QMessageBox::warning(this, "提示", "密码需至少8位，包含字母和数字，且不可含特殊字符");
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
    close();
}

void Regist::on_btnBack_clicked()
{
    close();
}

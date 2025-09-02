#include "login.h"
#include "ui_login.h"
#include "regist.h"
#include "client_factory.h"
#include "client_expert.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QHostAddress>
#include <QTcpSocket>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QApplication>
#include <QComboBox>
#include <QStyle>

// 全局样式（仅UI）。说明：
// - roleTheme 属性用于登录/注册页动态切换主题：none(灰)/expert(蓝)/factory(绿)。
// - 对主界面不改代码，通过根对象名 #ClientExpert / #ClientFactory 自动着色。
// - 不使用任何图形特效，跨平台稳定。
// - 提升 QComboBox 下拉箭头和下拉项对比度；突显表头与 Tab 选中态。
static const char kGlobalQss[] = R"QSS(
* {
    font-family: "Microsoft YaHei","PingFang SC","Noto Sans CJK SC","Segoe UI",sans-serif;
    font-size: 16px;
    color: #1f2937;
}

/* 登录/注册：根据 roleTheme 切换背景 */
QWidget[roleTheme="none"] {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #f3f4f6, stop:1 #e5e7eb);
}
QWidget[roleTheme="expert"] {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #e8f3fe, stop:1 #a8d0fb);
}
QWidget[roleTheme="factory"] {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #e9f8ef, stop:1 #b8e3c2);
}

/* 主界面：通过根对象名自动着色（无需改代码） */
QWidget#ClientExpert {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #e8f3fe, stop:1 #a8d0fb);
}
QWidget#ClientFactory {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #e9f8ef, stop:1 #b8e3c2);
}

/* 容器透明以露出背景 */
QGroupBox, QFrame, QWidget#centralWidget { background: transparent; border: none; }

/* 输入与文本框 */
QLineEdit, QPlainTextEdit, QTextEdit {
    background: #ffffff;
    border: 1px solid #d1d5db;
    border-radius: 12px;
    padding: 8px 12px;
    selection-background-color: #cbd5e1;
    selection-color: #111827;
    min-height: 38px;
}

/* 登录/注册页的聚焦边框颜色随主题变化 */
QWidget[roleTheme="expert"] QLineEdit:focus,
QWidget[roleTheme="expert"] QPlainTextEdit:focus,
QWidget[roleTheme="expert"] QTextEdit:focus { border: 1px solid #1976d2; }
QWidget[roleTheme="factory"] QLineEdit:focus,
QWidget[roleTheme="factory"] QPlainTextEdit:focus,
QWidget[roleTheme="factory"] QTextEdit:focus { border: 1px solid #2e7d32; }
QWidget[roleTheme="none"] QLineEdit:focus,
QWidget[roleTheme="none"] QPlainTextEdit:focus,
QWidget[roleTheme="none"] QTextEdit:focus { border: 1px solid #6b7280; }

/* 身份下拉（增强箭头与对比度） */
QComboBox {
    background: #ffffff;
    border: 1px solid #cfd8dc;
    border-radius: 12px;
    padding: 6px 10px;
    min-height: 38px;
}
QComboBox:focus { border: 1px solid #94a3b8; }
QComboBox::drop-down {
    width: 36px;
    border-left: 1px solid #e5e7eb;
    border-top-right-radius: 12px;
    border-bottom-right-radius: 12px;
    background: #f3f4f6;
}
QWidget[roleTheme="expert"] QComboBox::drop-down { background: #e3f2fd; }
QWidget[roleTheme="factory"] QComboBox::drop-down { background: #e8f5e9; }
QComboBox::down-arrow { width: 14px; height: 14px; margin-right: 10px; }
QComboBox QAbstractItemView { background: #ffffff; border: 1px solid #d1d5db; outline: 0; }
QWidget[roleTheme="expert"] QComboBox QAbstractItemView::item:selected { background: #1976d2; color: #ffffff; }
QWidget[roleTheme="factory"] QComboBox QAbstractItemView::item:selected { background: #2e7d32; color: #ffffff; }
QWidget[roleTheme="none"]   QComboBox QAbstractItemView::item:selected { background: #6b7280; color: #ffffff; }

/* 按钮（非 primary） */
QPushButton {
    background: rgba(255,255,255,0.9);
    border: 1px solid #d1d5db;
    border-radius: 12px;
    padding: 8px 16px;
    color: #111827;
    min-height: 40px;
}
QPushButton:hover { background: rgba(255,255,255,1.0); }
QPushButton:pressed { background: #eef2f7; }
QPushButton:disabled { color: #9ca3af; background: #f3f4f6; border-color: #e5e7eb; }

/* 主要按钮（primary=true）主题色 */
QWidget[roleTheme="expert"] QPushButton[primary="true"] { background: #1976d2; color: #ffffff; border: 1px solid #115293; }
QWidget[roleTheme="expert"] QPushButton[primary="true"]:hover   { background: #1e88e5; }
QWidget[roleTheme="expert"] QPushButton[primary="true"]:pressed { background: #1565c0; }

QWidget[roleTheme="factory"] QPushButton[primary="true"] { background: #2e7d32; color: #ffffff; border: 1px solid #1b5e20; }
QWidget[roleTheme="factory"] QPushButton[primary="true"]:hover   { background: #388e3c; }
QWidget[roleTheme="factory"] QPushButton[primary="true"]:pressed { background: #1b5e20; }

QWidget[roleTheme="none"] QPushButton[primary="true"] { background: #6b7280; color: #ffffff; border: 1px solid #4b5563; }
QWidget[roleTheme="none"] QPushButton[primary="true"]:hover   { background: #7b8391; }
QWidget[roleTheme="none"] QPushButton[primary="true"]:pressed { background: #4b5563; }

/* 主界面 Tab 着色（对象名匹配），和登录/注册页（roleTheme）两种方式都支持 */
QTabBar::tab {
    min-width: 100px; min-height: 28px;
    background: #eaeaea; color: #111827;
    border-radius: 8px; padding: 6px 18px; margin: 2px;
}
QWidget#ClientExpert  QTabBar::tab:selected { background: #1976d2; color: #ffffff; }
QWidget#ClientFactory QTabBar::tab:selected { background: #2e7d32; color: #ffffff; }
QWidget[roleTheme="expert"] QTabBar::tab:selected { background: #1976d2; color: #ffffff; }
QWidget[roleTheme="factory"] QTabBar::tab:selected { background: #2e7d32; color: #ffffff; }
QWidget[roleTheme="none"]    QTabBar::tab:selected { background: #6b7280; color: #ffffff; }

/* 表格：表头高亮、选中高亮、斑马纹 */
QTableView, QTableWidget {
    background: #ffffff;
    border: 1px solid #e5e7eb;
    border-radius: 10px;
    gridline-color: #e5e7eb;
    alternate-background-color: #fafafa;
}
QHeaderView::section {
    padding: 6px 8px;
    border: none;
    background: #9aa0a6;
    color: #ffffff;
}
QWidget#ClientExpert  QHeaderView::section { background: #1976d2; color: #ffffff; }
QWidget#ClientFactory QHeaderView::section { background: #2e7d32; color: #ffffff; }
QTableView::item:selected, QTableWidget::item:selected { background: #90caf9; color: #0b1020; }
QWidget#ClientFactory QTableView::item:selected,
QWidget#ClientFactory QTableWidget::item:selected { background: #a5d6a7; color: #0b1020; }

/* 分隔线（如有） */
QFrame[frameShape="4"], QFrame[frameShape="5"] { color: #e5e7eb; background: #e5e7eb; }
)QSS";

static const char* SERVER_HOST = "127.0.0.1";
static const quint16 SERVER_PORT = 5555;

Login::Login(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Login)
{
    ui->setupUi(this);

    // 应用全局样式（仅UI，不影响功能）
    qApp->setStyleSheet(QString::fromUtf8(kGlobalQss));

    // 登录页：主按钮声明为 primary（样式用）
    ui->btnLogin->setProperty("primary", true);

    // 初始化角色下拉
    ui->cbRole->clear();
    ui->cbRole->addItem("请选择身份"); // 0
    ui->cbRole->addItem("专家");        // 1
    ui->cbRole->addItem("工厂");        // 2
    ui->cbRole->setCurrentIndex(0);

    // 初始主题：未选择 -> 灰色
    this->setProperty("roleTheme", "none");
    this->style()->unpolish(this);
    this->style()->polish(this);

    // 身份变化 -> 仅更新UI属性，不触碰业务逻辑
    connect(ui->cbRole, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx){
                QString key = "none";
                if (idx == 1) key = "expert";
                else if (idx == 2) key = "factory";
                this->setProperty("roleTheme", key);
                this->style()->unpolish(this);
                this->style()->polish(this);
                this->update();
            });
}

Login::~Login()
{
    delete ui;
}

void Login::closeEvent(QCloseEvent *event)
{
    QCoreApplication::quit();
    QWidget::closeEvent(event);
}

QString Login::selectedRole() const
{
    switch (ui->cbRole->currentIndex()) {
    case 1: return "expert";
    case 2: return "factory";
    default: return "";
    }
}

bool Login::sendRequest(const QJsonObject &obj, QJsonObject &reply, QString *errMsg)
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

void Login::on_btnLogin_clicked()
{
    const QString username = ui->leUsername->text().trimmed();
    const QString password = ui->lePassword->text();
    if (username.isEmpty() || password.isEmpty()) {
        QMessageBox::warning(this, "提示", "请输入账号和密码");
        return;
    }
    const QString role = selectedRole();
    if (role.isEmpty()) {
        QMessageBox::warning(this, "提示", "请选择身份");
        return;
    }

    QJsonObject req{
        {"action",  "login"},
        {"role",    role},
        {"username",username},
        {"password",password}
    };
    QJsonObject rep;
    QString err;
    if (!sendRequest(req, rep, &err)) {
        QMessageBox::warning(this, "登录失败", err);
        return;
    }
    if (!rep.value("ok").toBool(false)) {
        QMessageBox::warning(this, "登录失败", rep.value("msg").toString("未知错误"));
        return;
    }

    // 仅UI窗口切换，功能不变
    if (role == "expert") {
        if (!expertWin) expertWin = new ClientExpert;
        expertWin->show();
    } else {
        if (!factoryWin) factoryWin = new ClientFactory;
        factoryWin->show();
    }
    this->hide();
}

void Login::on_btnToReg_clicked()
{
    // 打开注册窗口（不改注册逻辑）
    Regist *r = new Regist(nullptr);
    r->setAttribute(Qt::WA_DeleteOnClose);
    // 若 Regist::preset 存在则预填（存在即用）
    if (r->metaObject()->indexOfMethod("preset(QString,QString,QString)") >= 0) {
        QMetaObject::invokeMethod(r, "preset",
                                  Q_ARG(QString, selectedRole()),
                                  Q_ARG(QString, ui->leUsername->text()),
                                  Q_ARG(QString, ui->lePassword->text()));
    }
    // 关闭/销毁注册窗口后恢复显示登录
    connect(r, &QObject::destroyed, this, [this](){
        this->show(); this->raise(); this->activateWindow();
    });
    this->hide();
    r->show();
}

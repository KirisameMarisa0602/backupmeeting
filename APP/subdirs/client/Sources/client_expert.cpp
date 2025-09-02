#include "client_expert.h"
#include "ui_client_expert.h"

#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMessageBox>
#include <QTimer>
#include <QString>

#include "comm/commwidget.h"
#include "comm/devicepanel.h"
#include "comm/knowledge_panel.h"   // 恢复：嵌入式企业知识库

// 与工程既有约定保持一致
QString g_factoryUsername;
QString g_expertUsername;

static const char*  SERVER_HOST = "127.0.0.1";
static const quint16 SERVER_PORT = 5555;

ClientExpert::ClientExpert(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::ClientExpert)
{
    ui->setupUi(this);

    // 实时通讯模块
    commWidget_ = new CommWidget(this);
    ui->verticalLayoutTabRealtime->addWidget(commWidget_);

    // 切到“实时通讯”页时把焦点交给会议主窗
    connect(ui->tabWidget, &QTabWidget::currentChanged, this, [this](int idx){
        if (ui->tabWidget->widget(idx) == ui->tabRealtime) {
            commWidget_->mainWindow()->setFocus();
        }
    });

    // 设备管理面板（嵌入）
    devicePanel_ = new DevicePanel(this);
    ui->verticalLayoutTabDevice->addWidget(devicePanel_);

    // 设备控制：面板 -> 会议广播
    connect(devicePanel_, SIGNAL(deviceControlSent(QString,QString)),
            this, SLOT(onDeviceControl(QString,QString)));

    // 会议广播（含本端回显） -> 面板日志
    connect(commWidget_->mainWindow(), SIGNAL(deviceControlMessage(QString,QString,QString,qint64)),
            devicePanel_, SLOT(applyControlCommand(QString,QString,QString,qint64)));

    // 企业知识库面板（嵌入“企业知识库”页）
    kbPanel_ = new KnowledgePanel(ui->tabOther);
    kbPanel_->setServer(QString::fromLatin1(SERVER_HOST), SERVER_PORT);
    ui->verticalLayoutTabOther->addWidget(kbPanel_);

    // 进入知识库页自动刷新
    connect(ui->tabWidget, &QTabWidget::currentChanged, this, [this](int idx){
        if (ui->tabWidget->widget(idx) == ui->tabOther && kbPanel_) {
            kbPanel_->refresh();
        }
    });

    // 其它已有连接
    connect(ui->tabWidget, SIGNAL(currentChanged(int)), this, SLOT(on_tabChanged(int)));
    connect(ui->btnAccept, SIGNAL(clicked()), this, SLOT(on_btnAccept_clicked()));
    connect(ui->btnReject, SIGNAL(clicked()), this, SLOT(on_btnReject_clicked()));
    connect(ui->btnRefreshOrderStatus, SIGNAL(clicked()), this, SLOT(refreshOrders()));
    connect(ui->btnSearchOrder, SIGNAL(clicked()), this, SLOT(onSearchOrder()));

    // 筛选项
    ui->comboBoxStatus->clear();
    ui->comboBoxStatus->addItem("全部");
    ui->comboBoxStatus->addItem("待处理");
    ui->comboBoxStatus->addItem("已接受");
    ui->comboBoxStatus->addItem("已拒绝");

    refreshOrders();
    updateTabEnabled();
}

ClientExpert::~ClientExpert()
{
    delete ui;
}

void ClientExpert::setJoinedOrder(bool joined)
{
    joinedOrder = joined;
    updateTabEnabled();
}

void ClientExpert::updateTabEnabled()
{
    // 索引提示：0=工单设置, 1=设备管理, 2=企业知识库, 3=实时通讯
    ui->tabWidget->setTabEnabled(1, joinedOrder);
    ui->tabWidget->setTabEnabled(3, joinedOrder);
}

void ClientExpert::refreshOrders()
{
    QTcpSocket sock;
    sock.connectToHost(SERVER_HOST, SERVER_PORT);
    if (!sock.waitForConnected(2000)) {
        QMessageBox::warning(this, "提示", "无法连接服务器");
        return;
    }
    QJsonObject req;
    req.insert("action", "get_orders");
    req.insert("role", "expert");
    req.insert("username", g_expertUsername);
    QString keyword = ui->lineEditKeyword->text().trimmed();
    if (!keyword.isEmpty()) req.insert("keyword", keyword);
    QString status = ui->comboBoxStatus->currentText();
    if (status != "全部") req.insert("status", status);

    sock.write(QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n");
    sock.waitForBytesWritten(1000);
    sock.waitForReadyRead(2000);
    QByteArray resp = sock.readAll();
    int nl = resp.indexOf('\n');
    if (nl >= 0) resp = resp.left(nl);
    QJsonDocument doc = QJsonDocument::fromJson(resp);
    if (!doc.isObject() || !doc.object().value("ok").toBool()) {
        QMessageBox::warning(this, "提示", "服务器响应异常");
        return;
    }
    orders.clear();
    QJsonArray arr = doc.object().value("orders").toArray();
    for (int i = 0; i < arr.size(); ++i) {
        QJsonObject o = arr.at(i).toObject();
        OrderInfo od;
        od.id = o.value("id").toInt();
        od.title = o.value("title").toString();
        od.desc = o.value("desc").toString();
        od.status = o.value("status").toString();
        orders.append(od);
    }
    QTableWidget* tbl = ui->tableOrders;
    tbl->clear();
    tbl->setColumnCount(4);
    tbl->setRowCount(orders.size());
    QStringList headers;
    headers << "工单号" << "标题" << "描述" << "状态";
    tbl->setHorizontalHeaderLabels(headers);
    for (int i = 0; i < orders.size(); ++i) {
        const OrderInfo& od = orders[i];
        tbl->setItem(i, 0, new QTableWidgetItem(QString::number(od.id)));
        tbl->setItem(i, 1, new QTableWidgetItem(od.title));
        tbl->setItem(i, 2, new QTableWidgetItem(od.desc));
        tbl->setItem(i, 3, new QTableWidgetItem(od.status));
    }
    tbl->resizeColumnsToContents();
}

void ClientExpert::on_btnAccept_clicked()
{
    int row = ui->tableOrders->currentRow();
    if (row < 0 || row >= orders.size()) {
        QMessageBox::warning(this, "提示", "请选择一个工单");
        return;
    }
    int id = orders[row].id;

    // 更新状态
    sendUpdateOrder(id, "已接受");

    // 进入工单上下文
    setJoinedOrder(true);

    // 设备面板上下文（决定曲线基线与日志归属）
    if (devicePanel_) devicePanel_->setOrderContext(QString::number(id));

    // 知识库页按工单过滤
    if (kbPanel_) kbPanel_->setRoomFilter(QString::number(id));

    // 自动加入会议
    if (!g_expertUsername.isEmpty()) {
        commWidget_->mainWindow()->setJoinedContext(g_expertUsername, QString::number(id));
    } else {
        commWidget_->mainWindow()->setJoinedContext(QStringLiteral("expert"), QString::number(id));
    }
    QMetaObject::invokeMethod(commWidget_->mainWindow(), "onJoin");

    QTimer::singleShot(150, this, SLOT(refreshOrders()));
}

void ClientExpert::on_btnReject_clicked()
{
    int row = ui->tableOrders->currentRow();
    if (row < 0 || row >= orders.size()) {
        QMessageBox::warning(this, "提示", "请选择一个工单");
        return;
    }
    int id = orders[row].id;

    sendUpdateOrder(id, "已拒绝");
    setJoinedOrder(false);

    QTimer::singleShot(150, this, SLOT(refreshOrders()));
}

void ClientExpert::sendUpdateOrder(int orderId, const QString& status)
{
    QTcpSocket sock;
    sock.connectToHost(SERVER_HOST, SERVER_PORT);
    if (!sock.waitForConnected(2000)) {
        QMessageBox::warning(this, "提示", "无法连接服务器");
        return;
    }
    QJsonObject req;
    req.insert("action", "update_order");
    req.insert("id", orderId);
    req.insert("status", status);

    sock.write(QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n");
    sock.waitForBytesWritten(1000);
    sock.waitForReadyRead(2000);
    QByteArray resp = sock.readAll();
    int nl = resp.indexOf('\n');
    if (nl >= 0) resp = resp.left(nl);
    QJsonDocument doc = QJsonDocument::fromJson(resp);
    if (!doc.isObject() || !doc.object().value("ok").toBool()) {
        QMessageBox::warning(this, "提示", "服务器响应异常");
    }
}

void ClientExpert::on_tabChanged(int idx)
{
    QWidget* page = ui->tabWidget->widget(idx);
    if (idx == 0) {
        refreshOrders();
    } else if (page == ui->tabDevice) {
        // 进入设备页时，若有选中工单，确保上下文设置
        int row = ui->tableOrders->currentRow();
        if (row >= 0 && row < orders.size() && devicePanel_) {
            devicePanel_->setOrderContext(QString::number(orders[row].id));
        }
    } else if (page == ui->tabOther) {
        // 进入知识库页时刷新；若有选中工单，按工单过滤
        int row = ui->tableOrders->currentRow();
        if (row >= 0 && row < orders.size() && kbPanel_) {
            kbPanel_->setRoomFilter(QString::number(orders[row].id));
        }
        if (kbPanel_) kbPanel_->refresh();
    }
}

void ClientExpert::onSearchOrder()
{
    refreshOrders();
}

void ClientExpert::onDeviceControl(const QString& device, const QString& command)
{
    if (!joinedOrder) {
        QMessageBox::information(this, "提示", "请先加入工单（接受工单后会自动加入）");
        return;
    }
    commWidget_->mainWindow()->sendDeviceControlBroadcast(device, command);
}

#include "client_factory.h"
#include "ui_client_factory.h"

#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QInputDialog>
#include <QMessageBox>
#include <QTimer>
#include <QDialog>
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QTextEdit>
#include <QDialogButtonBox>

#include "comm/commwidget.h"
#include "comm/devicepanel.h"
#include "comm/knowledge_panel.h"   // 恢复：嵌入式企业知识库

static const char*  SERVER_HOST = "127.0.0.1";
static const quint16 SERVER_PORT = 5555;

extern QString g_factoryUsername;

class NewOrderDialog : public QDialog {
public:
    QLineEdit*  editTitle;
    QTextEdit*  editDesc;
    NewOrderDialog(QWidget* parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle("新建工单");
        setMinimumSize(400, 260);

        QVBoxLayout* layout = new QVBoxLayout(this);
        QLabel* labelTitle = new QLabel("工单标题：", this);
        editTitle = new QLineEdit(this);
        QLabel* labelDesc = new QLabel("工单描述：", this);
        editDesc = new QTextEdit(this);
        editDesc->setMinimumHeight(100);

        QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

        layout->addWidget(labelTitle);
        layout->addWidget(editTitle);
        layout->addWidget(labelDesc);
        layout->addWidget(editDesc);
        layout->addWidget(buttons);

        connect(buttons, SIGNAL(accepted()), this, SLOT(accept()));
        connect(buttons, SIGNAL(rejected()), this, SLOT(reject()));
    }
};

ClientFactory::ClientFactory(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::ClientFactory)
{
    ui->setupUi(this);

    // 实时通讯模块
    commWidget_ = new CommWidget(this);
    ui->verticalLayoutTabRealtime->addWidget(commWidget_);

    // 设备管理面板
    devicePanel_ = new DevicePanel(this);
    ui->verticalLayoutTabDevice->addWidget(devicePanel_);

    // 设备控制：面板 -> 广播
    connect(devicePanel_, SIGNAL(deviceControlSent(QString,QString)),
            this, SLOT(onSendDeviceControl(QString,QString)));

    // 广播（含本端回显） -> 面板日志
    connect(commWidget_->mainWindow(), SIGNAL(deviceControlMessage(QString,QString,QString,qint64)),
            devicePanel_, SLOT(applyControlCommand(QString,QString,QString,qint64)));

    // 企业知识库面板（嵌入“企业知识库”页）
    kbPanel_ = new KnowledgePanel(ui->tabOther);
    kbPanel_->setServer(QString::fromLatin1(SERVER_HOST), SERVER_PORT);
    ui->verticalLayoutTabOther->addWidget(kbPanel_);

    // 选择页时：实时通讯聚焦；设备页设上下文；知识库页刷新
    connect(ui->tabWidget, &QTabWidget::currentChanged, this, [this](int idx){
        QWidget* page = ui->tabWidget->widget(idx);
        if (page == ui->tabRealtime) {
            commWidget_->mainWindow()->setFocus();
        } else if (page == ui->tabDevice) {
            int row = ui->tableOrders->currentRow();
            if (row >= 0 && row < orders.size() && devicePanel_) {
                devicePanel_->setOrderContext(QString::number(orders[row].id));
            }
        } else if (page == ui->tabOther) {
            int row = ui->tableOrders->currentRow();
            if (row >= 0 && row < orders.size() && kbPanel_) {
                kbPanel_->setRoomFilter(QString::number(orders[row].id));
            }
            if (kbPanel_) kbPanel_->refresh();
        }
    });

    // 原有连接
    connect(ui->tabWidget, SIGNAL(currentChanged(int)), this, SLOT(on_tabChanged(int)));
    connect(ui->btnSearchOrder, SIGNAL(clicked()), this, SLOT(onSearchOrder()));
    connect(ui->btnRefreshOrderStatus, SIGNAL(clicked()), this, SLOT(refreshOrders()));
    connect(ui->btnDeleteOrder, SIGNAL(clicked()), this, SLOT(on_btnDeleteOrder_clicked()));

    refreshOrders();
    updateTabEnabled();
}

ClientFactory::~ClientFactory()
{
    delete ui;
}

void ClientFactory::refreshOrders()
{
    QTcpSocket sock;
    sock.connectToHost(SERVER_HOST, SERVER_PORT);
    if (!sock.waitForConnected(2000)) {
        QMessageBox::warning(this, "提示", "无法连接服务器");
        return;
    }
    QJsonObject req;
    req.insert("action", "get_orders");
    req.insert("role", "factory");
    req.insert("username", g_factoryUsername);
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
    tbl->clearSelection();
}

void ClientFactory::on_btnNewOrder_clicked()
{
    NewOrderDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted) {
        QString title = dlg.editTitle->text().trimmed();
        QString desc  = dlg.editDesc->toPlainText().trimmed();
        if (title.isEmpty()) {
            QMessageBox::warning(this, "提示", "工单标题不能为空");
            return;
        }
        sendCreateOrder(title, desc);
        QTimer::singleShot(150, this, SLOT(refreshOrders()));
    }
}

void ClientFactory::sendCreateOrder(const QString& title, const QString& desc)
{
    QTcpSocket sock;
    sock.connectToHost(SERVER_HOST, SERVER_PORT);
    if (!sock.waitForConnected(2000)) {
        QMessageBox::warning(this, "提示", "无法连接服务器");
        return;
    }
    QJsonObject req;
    req.insert("action", "new_order");
    req.insert("title", title);
    req.insert("desc",  desc);
    req.insert("factory_user", g_factoryUsername);

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

void ClientFactory::on_btnDeleteOrder_clicked()
{
    if (deletingOrder) return;
    deletingOrder = true;

    int row = ui->tableOrders->currentRow();
    if (row < 0 || row >= orders.size()) {
        QMessageBox::warning(this, "提示", "请选择要销毁的工单");
        deletingOrder = false;
        return;
    }
    int id = orders[row].id;
    if (QMessageBox::question(this, "确认", "确定要销毁该工单？") != QMessageBox::Yes) {
        deletingOrder = false;
        return;
    }
    QTcpSocket sock;
    sock.connectToHost(SERVER_HOST, SERVER_PORT);
    if (!sock.waitForConnected(2000)) {
        QMessageBox::warning(this, "提示", "无法连接服务器");
        deletingOrder = false;
        return;
    }
    QJsonObject req;
    req.insert("action", "delete_order");
    req.insert("id", id);
    req.insert("username", g_factoryUsername);

    sock.write(QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n");
    sock.waitForBytesWritten(1000);
    sock.waitForReadyRead(2000);
    QByteArray resp = sock.readAll();
    int nl = resp.indexOf('\n');
    if (nl >= 0) resp = resp.left(nl);
    QJsonDocument doc = QJsonDocument::fromJson(resp);
    if (!doc.isObject() || !doc.object().value("ok").toBool()) {
        QMessageBox::warning(this, "提示", "服务器响应异常");
        deletingOrder = false;
        return;
    }
    QTimer::singleShot(150, this, SLOT(refreshOrders()));
    deletingOrder = false;
}

void ClientFactory::updateTabEnabled()
{
    // 工厂端默认不限制 Tab
    ui->tabWidget->setTabEnabled(1, true);
    ui->tabWidget->setTabEnabled(3, true);
}

void ClientFactory::on_tabChanged(int idx)
{
    QWidget* page = ui->tabWidget->widget(idx);
    if (idx == 0) {
        refreshOrders();
    } else if (page == ui->tabDevice) {
        int row = ui->tableOrders->currentRow();
        if (row >= 0 && row < orders.size() && devicePanel_) {
            devicePanel_->setOrderContext(QString::number(orders[row].id));
        }
    } else if (page == ui->tabOther) {
        int row = ui->tableOrders->currentRow();
        if (row >= 0 && row < orders.size() && kbPanel_) {
            kbPanel_->setRoomFilter(QString::number(orders[row].id));
        }
        if (kbPanel_) kbPanel_->refresh();
    }
}

void ClientFactory::onSearchOrder()
{
    refreshOrders();
}

void ClientFactory::onSendDeviceControl(const QString& device, const QString& command)
{
    commWidget_->mainWindow()->sendDeviceControlBroadcast(device, command);
}

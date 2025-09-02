#include "client_expert.h"
#include "ui_client_expert.h"

#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMessageBox>
#include <QTimer>
#include <QString>
#include <QElapsedTimer>

#include "comm/commwidget.h"
#include "comm/devicepanel.h"
#include "comm/knowledge_panel.h"
#include <QTabWidget>
#include <QTabBar>

namespace {
static void applyExpertTabStyle(QTabWidget* tabs) {
    if (!tabs) return;
    QTabBar* bar = tabs->tabBar();
    if (!bar) return;

    // 专家端：选中蓝底
    bar->setStyleSheet(
        "QTabWidget::pane{ border:0; }"
        "QTabBar::tab{"
        "  padding:8px 16px; margin:2px; border-radius:8px;"
        "  background:#e5e7eb; color:#111827;"
        "}"
        "QTabBar::tab:selected{ background:#2563eb; color:#ffffff; }"
        "QTabBar::tab:hover{ background:#d1d5db; }"
    );
}
} // namespace

// 与工程既有约定保持一致：在本文件定义全局用户名变量
QString g_factoryUsername;
QString g_expertUsername;

static const char*  SERVER_HOST = "127.0.0.1";
static const quint16 SERVER_PORT = 5555;

// 可靠读取“以换行分隔”的一条 JSON（带超时聚合，避免半包）
static QJsonDocument readJsonLine(QTcpSocket& sock, int timeoutMs = 5000) {
    QByteArray buf;
    QElapsedTimer et; et.start();
    while (et.elapsed() < timeoutMs) {
        if (!sock.bytesAvailable())
            sock.waitForReadyRead(qMax(1, timeoutMs - int(et.elapsed())));
        buf += sock.readAll();
        int nl = buf.indexOf('\n');
        if (nl >= 0) {
            QByteArray line = buf.left(nl);
            if (!line.isEmpty() && line.endsWith('\r')) line.chop(1);
            return QJsonDocument::fromJson(line);
        }
    }
    // 超时也尝试解析（容错）
    return QJsonDocument::fromJson(buf);
}

ClientExpert::ClientExpert(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::ClientExpert)
{
    ui->setupUi(this);
    applyExpertTabStyle(ui->tabWidget);
    // 标注顶层对象名，用于主题检测（聊天侧栏蓝色渐变等）
    setObjectName("ClientExpert");

    // 实时通讯模块
    commWidget_ = new CommWidget(this);
    ui->verticalLayoutTabRealtime->addWidget(commWidget_);

    // 切到“实时通讯”页时自动入会并把焦点交给会议主窗
    connect(ui->tabWidget, &QTabWidget::currentChanged, this, [this](int idx){
        QWidget* page = ui->tabWidget->widget(idx);
        if (page == ui->tabRealtime) {
            // 确保有选中工单
            if (ui->tableOrders->currentRow() < 0 && ui->tableOrders->rowCount() > 0) {
                ui->tableOrders->setCurrentCell(0, 0);
            }
            int row = ui->tableOrders->currentRow();
            if (row >= 0 && row < orders.size()) {
                const QString room = QString::number(orders[row].id);
                // User = g_expertUsername，room = 工单号
                commWidget_->mainWindow()->setJoinedContext(g_expertUsername, room);
                QMetaObject::invokeMethod(commWidget_->mainWindow(), "onJoin");
            }
            commWidget_->mainWindow()->setFocus();
        } else if (page == ui->tabOther && kbPanel_) {
            // 进入知识库页根据当前工单过滤并刷新
            int row = ui->tableOrders->currentRow();
            if (row >= 0 && row < orders.size()) {
                kbPanel_->setRoomFilter(QString::number(orders[row].id));
            }
            kbPanel_->refresh();
        }
    });

    // 设备管理面板（嵌入）
    devicePanel_ = new DevicePanel(this);
    ui->verticalLayoutTabDevice->addWidget(devicePanel_);

    connect(devicePanel_, &DevicePanel::deviceControlSent,
            this, &ClientExpert::onDeviceControl);

    connect(commWidget_->mainWindow(), &MainWindow::deviceControlMessage,
            devicePanel_, [this](const QString& device,const QString& cmd,const QString& sender,qint64 ts){
                devicePanel_->applyControlCommand(device, cmd, sender, ts);
            });

    // 企业知识库面板（嵌入“企业知识库”页）
    kbPanel_ = new KnowledgePanel(ui->tabOther);
    kbPanel_->setServer(QString::fromLatin1(SERVER_HOST), SERVER_PORT);
    ui->verticalLayoutTabOther->addWidget(kbPanel_);

    // 进入知识库页自动刷新（其余逻辑在上面的 currentChanged 合并处理）
    connect(ui->tabWidget, &QTabWidget::currentChanged, this, &ClientExpert::on_tabChanged);

    connect(ui->btnAccept, &QPushButton::clicked, this, &ClientExpert::on_btnAccept_clicked);
    connect(ui->btnReject, &QPushButton::clicked, this, &ClientExpert::on_btnReject_clicked);
    connect(ui->btnRefreshOrderStatus, &QPushButton::clicked, this, &ClientExpert::refreshOrders);
    connect(ui->btnSearchOrder, &QPushButton::clicked, this, &ClientExpert::onSearchOrder);

    // 切换选中工单时：若当前在“实时通讯”页，自动切换入会房间
    connect(ui->tableOrders, &QTableWidget::itemSelectionChanged, this, [this](){
        if (ui->tabWidget->currentWidget() == ui->tabRealtime) {
            int row = ui->tableOrders->currentRow();
            if (row >= 0 && row < orders.size()) {
                const QString room = QString::number(orders[row].id);
                commWidget_->mainWindow()->setJoinedContext(g_expertUsername, room);
                QMetaObject::invokeMethod(commWidget_->mainWindow(), "onJoin");
            }
        }
        // 知识库页保持原行为：切换时不强制刷新，进入该页时刷新
    });

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
    // 之前是根据 joinedOrder 禁用/启用页签：
    // ui->tabWidget->setTabEnabled(1, joinedOrder);
    // ui->tabWidget->setTabEnabled(3, joinedOrder);
    //
    // 现在改为：不再禁用页签，统一在切换时做提示与拦截。
    // 因此这里保持空实现，保留函数以兼容其他调用点。
}

void ClientExpert::refreshOrders()
{
    QTcpSocket sock;
    sock.connectToHost(SERVER_HOST, SERVER_PORT);
    if (!sock.waitForConnected(2000)) {
        QMessageBox::warning(this, "提示", "无法连接服务器");
        return;
    }
    QJsonObject req{
        {"action", "get_orders"},
        {"role", "expert"},
        {"username", g_expertUsername}
    };
    QString keyword = ui->lineEditKeyword->text().trimmed();
    if (!keyword.isEmpty()) req["keyword"] = keyword;
    QString status = ui->comboBoxStatus->currentText();
    if (status != "全部") req["status"] = status;

    sock.write(QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n");
    sock.flush();

    QJsonDocument doc = readJsonLine(sock);
    if (!doc.isObject() || !doc.object().value("ok").toBool()) {
        QMessageBox::warning(this, "提示", "服务器响应异常");
        return;
    }

    orders.clear();
    QJsonArray arr = doc.object().value("orders").toArray();
    orders.reserve(arr.size());
    for (const QJsonValue& v : arr) {
        QJsonObject o = v.toObject();
        orders.append(OrderInfo{
            o.value("id").toInt(), o.value("title").toString(),
            o.value("desc").toString(), o.value("status").toString()
        });
    }

    auto* tbl = ui->tableOrders;
    bool wasSorting = tbl->isSortingEnabled();
    tbl->setSortingEnabled(false);
    tbl->clearContents();
    tbl->setColumnCount(4);
    tbl->setRowCount(0);

    QStringList headers{"工单号", "标题", "描述", "状态"};
    tbl->setHorizontalHeaderLabels(headers);

    tbl->setRowCount(orders.size());
    for (int i = 0; i < orders.size(); ++i) {
        const auto& od = orders[i];
        tbl->setItem(i, 0, new QTableWidgetItem(od.id > 0 ? QString::number(od.id) : QString()));
        tbl->setItem(i, 1, new QTableWidgetItem(od.title));
        tbl->setItem(i, 2, new QTableWidgetItem(od.desc));
        tbl->setItem(i, 3, new QTableWidgetItem(od.status));
    }
    tbl->resizeColumnsToContents();
    tbl->clearSelection();
    tbl->setSortingEnabled(wasSorting);

    // 如果当前在“实时通讯”页，刷新后确保房间上下文
    if (ui->tabWidget->currentWidget() == ui->tabRealtime) {
        if (ui->tableOrders->currentRow() < 0 && ui->tableOrders->rowCount() > 0) {
            ui->tableOrders->setCurrentCell(0, 0);
        }
        int row = ui->tableOrders->currentRow();
        if (row >= 0 && row < orders.size()) {
            const QString room = QString::number(orders[row].id);
            commWidget_->mainWindow()->setJoinedContext(g_expertUsername, room);
            QMetaObject::invokeMethod(commWidget_->mainWindow(), "onJoin");
        }
    }
}

void ClientExpert::on_btnAccept_clicked()
{
    int row = ui->tableOrders->currentRow();
    if (row < 0 || row >= orders.size()) {
        return;
    }
    const int id = orders[row].id;

    // 更新状态为“已接受”
    sendUpdateOrder(id, "已接受");

    // 标记已加入工单上下文
    setJoinedOrder(true);

    // 若有设备/知识库面板，需要设定上下文
    if (devicePanel_) devicePanel_->setOrderContext(QString::number(id));
    if (kbPanel_)     kbPanel_->setRoomFilter(QString::number(id));

    // 自动加入会议上下文（User=用户名；room=工单号）
    if (!g_expertUsername.isEmpty()) {
        commWidget_->mainWindow()->setJoinedContext(g_expertUsername, QString::number(id));
    } else {
        commWidget_->mainWindow()->setJoinedContext(QStringLiteral("expert"), QString::number(id));
    }
    QMetaObject::invokeMethod(commWidget_->mainWindow(), "onJoin");

    // 引导提示
    QMessageBox::information(this, tr("提示"),
        tr("已接受工单，可前往设备详情页面/实时通讯页面处理工单"));

    // 刷新工单列表
    refreshOrders();
}

void ClientExpert::on_btnReject_clicked()
{
    int row = ui->tableOrders->currentRow();
    if (row < 0 || row >= orders.size()) {
        // 静默返回：不再弹出“请选择一个工单”
        return;
    }
    int id = orders[row].id;

    sendUpdateOrder(id, "已拒绝");
    setJoinedOrder(false);

    refreshOrders();
}

void ClientExpert::sendUpdateOrder(int orderId, const QString& status)
{
    QTcpSocket sock;
    sock.connectToHost(SERVER_HOST, SERVER_PORT);
    if (!sock.waitForConnected(2000)) {
        QMessageBox::warning(this, "提示", "无法连接服务器");
        return;
    }
    QJsonObject req{
        {"action", "update_order"},
        {"id", orderId},
        {"status", status}
    };
    sock.write(QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n");
    sock.flush();

    QJsonDocument doc = readJsonLine(sock);
    if (!doc.isObject() || !doc.object().value("ok").toBool()) {
        QMessageBox::warning(this, "提示", "服务器响应异常");
    }
}

void ClientExpert::on_tabChanged(int idx)
{
    QWidget* page = ui->tabWidget->widget(idx);

    // 当尚未接受工单时，拦截进入“设备管理”与“实时通讯”，并弹出提示信息
    if (!joinedOrder && (page == ui->tabDevice || page == ui->tabRealtime)) {
        QMessageBox::information(this, tr("提示"),
                                 tr("没有待处理工单，请先在“工单设置”页接受工单。"));
        // 回到“工单设置”（索引 0）
        ui->tabWidget->setCurrentIndex(0);
        return;
    }
    // 其他页签切换逻辑由已有的 currentChanged 连接处理
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

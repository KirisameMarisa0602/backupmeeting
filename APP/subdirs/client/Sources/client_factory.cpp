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
#include <QElapsedTimer>

#include "comm/commwidget.h"
#include "comm/devicepanel.h"
#include "comm/knowledge_panel.h"
#include <QTabWidget>
#include <QTabBar>

namespace {
static void applyFactoryTabStyle(QTabWidget* tabs) {
    if (!tabs) return;
    QTabBar* bar = tabs->tabBar();
    if (!bar) return;

    // 工厂端：选中绿底
    bar->setStyleSheet(
        "QTabWidget::pane{ border:0; }"
        "QTabBar::tab{"
        "  padding:8px 16px; margin:2px; border-radius:8px;"
        "  background:#e5e7eb; color:#111827;"
        "}"
        "QTabBar::tab:selected{ background:#10b981; color:#ffffff; }"
        "QTabBar::tab:hover{ background:#d1d5db; }"
    );
}
} // namespace

static const char*  SERVER_HOST = "127.0.0.1";
static const quint16 SERVER_PORT = 5555;

extern QString g_factoryUsername;

// 可靠读取“以换行分隔”的一条 JSON
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
    return QJsonDocument::fromJson(buf);
}

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

        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    }
};

ClientFactory::ClientFactory(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::ClientFactory)
{
    ui->setupUi(this);
    applyFactoryTabStyle(ui->tabWidget);
    // 标注顶层对象名，用于主题检测（聊天侧栏绿色渐变等）
    setObjectName("ClientFactory");

    // 实时通讯模块
    commWidget_ = new CommWidget(this);
    ui->verticalLayoutTabRealtime->addWidget(commWidget_);

    // 设备管理面板（纯本地模拟曲线，不依赖入会）
    devicePanel_ = new DevicePanel(this);
    ui->verticalLayoutTabDevice->addWidget(devicePanel_);

    // 面板发起控制 -> 会议主窗广播
    connect(devicePanel_, &DevicePanel::deviceControlSent,
            this, &ClientFactory::onSendDeviceControl);

    // 会议主窗接收到（或本端回显） -> 面板写日志
    connect(commWidget_->mainWindow(), &MainWindow::deviceControlMessage,
            devicePanel_, [this](const QString& device,const QString& cmd,const QString& sender,qint64 ts){
                devicePanel_->applyControlCommand(device, cmd, sender, ts);
            });

    // 企业知识库面板（嵌入“企业知识库”页）
    kbPanel_ = new KnowledgePanel(ui->tabOther);
    kbPanel_->setServer(QString::fromLatin1(SERVER_HOST), SERVER_PORT);
    ui->verticalLayoutTabOther->addWidget(kbPanel_);

    // 选择页时：实时通讯聚焦；设备页设置上下文；知识库页刷新
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
                // 自动设置：User = g_factoryUsername，room = 工单号
                commWidget_->mainWindow()->setJoinedContext(g_factoryUsername, room);
                QMetaObject::invokeMethod(commWidget_->mainWindow(), "onJoin");
            } else {
                QMessageBox::information(this, QString::fromUtf8("提示"), QString::fromUtf8("请先选择一个工单"));
            }
            commWidget_->mainWindow()->setFocus();
        } else if (page == ui->tabDevice) {
            ensureDeviceContextFromSelection();   // 进入设备页时确保上下文
        } else if (page == ui->tabOther) {
            int row = ui->tableOrders->currentRow();
            if (row >= 0 && row < orders.size() && kbPanel_) {
                kbPanel_->setRoomFilter(QString::number(orders[row].id));
            }
            if (kbPanel_) kbPanel_->refresh();
        }
    });

    // 切换选中工单时：
    // - 若当前在“设备管理”页，同步上下文到 DevicePanel
    // - 若当前在“实时通讯”页，切换入会房间（room = 工单号）
    connect(ui->tableOrders, &QTableWidget::itemSelectionChanged, this, [this](){
        if (ui->tabWidget->currentWidget() == ui->tabDevice) {
            ensureDeviceContextFromSelection();
        } else if (ui->tabWidget->currentWidget() == ui->tabRealtime) {
            int row = ui->tableOrders->currentRow();
            if (row >= 0 && row < orders.size()) {
                const QString room = QString::number(orders[row].id);
                commWidget_->mainWindow()->setJoinedContext(g_factoryUsername, room);
                QMetaObject::invokeMethod(commWidget_->mainWindow(), "onJoin");
            }
        }
    });

    // 原有连接（注意：槽指针不加括号）
    connect(ui->tabWidget, &QTabWidget::currentChanged, this, &ClientFactory::on_tabChanged);
    connect(ui->btnSearchOrder, &QPushButton::clicked, this, &ClientFactory::onSearchOrder);
    connect(ui->btnRefreshOrderStatus, &QPushButton::clicked, this, &ClientFactory::refreshOrders);
    connect(ui->btnDeleteOrder, &QPushButton::clicked, this, &ClientFactory::on_btnDeleteOrder_clicked);

    refreshOrders();
    updateTabEnabled();
}

ClientFactory::~ClientFactory()
{
    delete ui;
}

void ClientFactory::ensureDeviceContextFromSelection()
{
    if (!devicePanel_) return;

    // 若无选中行且有数据，默认选中第一行
    if (ui->tableOrders->currentRow() < 0 && ui->tableOrders->rowCount() > 0) {
        ui->tableOrders->setCurrentCell(0, 0);
    }

    int row = ui->tableOrders->currentRow();
    if (row >= 0 && row < orders.size()) {
        const QString orderId = QString::number(orders[row].id);
        devicePanel_->setOrderContext(orderId);
    }
}

void ClientFactory::refreshOrders()
{
    QTcpSocket sock;
    sock.connectToHost(SERVER_HOST, SERVER_PORT);
    if (!sock.waitForConnected(2000)) {
        QMessageBox::warning(this, "提示", "无法连接服务器");
        return;
    }
    QJsonObject req{
        {"action", "get_orders"},
        {"role", "factory"},
        {"username", g_factoryUsername}
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
        tbl->setItem(i, 0, new QTableWidgetItem(QString::number(od.id)));
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
            commWidget_->mainWindow()->setJoinedContext(g_factoryUsername, room);
            QMetaObject::invokeMethod(commWidget_->mainWindow(), "onJoin");
        }
    }

    // 如果当前就在“设备管理”页，刷新完工单后立即确保上下文（让曲线立刻出现）
    if (ui->tabWidget->currentWidget() == ui->tabDevice) {
        ensureDeviceContextFromSelection();
    }
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
        // 服务端确认后立即刷新
        refreshOrders();
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
    QJsonObject req{
        {"action", "new_order"},
        {"title", title},
        {"desc",  desc},
        {"factory_user", g_factoryUsername}
    };
    sock.write(QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n");
    sock.flush();

    QJsonDocument doc = readJsonLine(sock);
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
    QJsonObject req{
        {"action", "delete_order"},
        {"id", id},
        {"username", g_factoryUsername}
    };
    sock.write(QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n");
    sock.flush();

    QJsonDocument doc = readJsonLine(sock);
    if (!doc.isObject() || !doc.object().value("ok").toBool()) {
        QMessageBox::warning(this, "提示", "服务器响应异常");
        deletingOrder = false;
        return;
    }

    deletingOrder = false;
    refreshOrders();
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
        ensureDeviceContextFromSelection();  // 兜底：进入设备页确保上下文
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
    // 广播给会议主窗（专家/工厂都会收到并在各自 DevicePanel 里写日志）
    commWidget_->mainWindow()->sendDeviceControlBroadcast(device, command);
}

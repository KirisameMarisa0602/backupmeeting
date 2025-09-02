#ifndef CLIENT_FACTORY_H
#define CLIENT_FACTORY_H

#include <QWidget>
#include <QVector>

#include <client_expert.h>     // 复用 OrderInfo
#include "comm/commwidget.h"

QT_BEGIN_NAMESPACE
namespace Ui { class ClientFactory; }
QT_END_NAMESPACE

class DevicePanel;     // 设备管理
class KnowledgePanel;  // 企业知识库

class ClientFactory : public QWidget
{
    Q_OBJECT

public:
    explicit ClientFactory(QWidget *parent = nullptr);
    ~ClientFactory();

private slots:
    void on_btnNewOrder_clicked();
    void on_btnDeleteOrder_clicked();
    void on_tabChanged(int idx);
    void onSearchOrder();

    // 设备面板发起控制命令
    void onSendDeviceControl(const QString& device, const QString& command);

private:
    Ui::ClientFactory *ui;
    QVector<OrderInfo> orders;
    bool deletingOrder = false;

    void refreshOrders();
    void updateTabEnabled();
    void sendCreateOrder(const QString& title, const QString& desc);

    // 进入设备页或数据刷新后，确保 DevicePanel 有上下文
    void ensureDeviceContextFromSelection();

    CommWidget*    commWidget_   = nullptr;
    DevicePanel*   devicePanel_  = nullptr;
    KnowledgePanel* kbPanel_     = nullptr;
};

#endif // CLIENT_FACTORY_H

#ifndef DEVICEPANEL_H
#define DEVICEPANEL_H

#include <QtWidgets>

// Qt Charts 头
#include <QtCharts/QChartView>
#include <QtCharts/QChart>
#include <QtCharts/QLineSeries>

QT_CHARTS_USE_NAMESPACE

struct DeviceInfo {
    QString name;
    double basePressure = 0;
    double baseTemp     = 0;
    QString status;
    QStringList logs;
    QStringList faults;
};

class DevicePanel : public QWidget {
    Q_OBJECT
public:
    explicit DevicePanel(QWidget* parent = nullptr);
    void setOrderContext(const QString& orderId);
    void applyControlCommand(const QString& device,
                             const QString& command,
                             const QString& sender,
                             qint64 tsMs);
signals:
    void deviceControlSent(const QString& device, const QString& command);

private slots:
    void onSendControl();
    void onTickUpdate();
    // 新增：当设备下拉框变更时，先广播，再刷新
    void onDeviceSelected(int index);

private:
    QChartView* pressureChart_;
    QChartView* tempChart_;
    QTableWidget* tableLogs_;
    QTableWidget* tableFaults_;
    QComboBox* cbDevice_;
    QLabel* lblPressure_;
    QLabel* lblTemperature_;
    QLineEdit* editControl_;
    QPushButton* btnSendControl_;

    QTimer timer_;

    QString currentOrderId_;
    QVector<DeviceInfo> devices_;
    QVector<QVector<QPointF> > pressureHist_;
    QVector<QVector<QPointF> > tempHist_;
    const int kMaxPoints = 120;

    // 防止对端切换设备时本端再次广播形成回环
    bool suppressSelectionBroadcast_ = false;

    void setupUi();
    void buildDeterministicDevices(const QString& orderId);
    void refreshUi();
    void appendLog(int idx, const QString& log);
    void pushPoint(int idx, double pressure, double temp);
};

#endif // DEVICEPANEL_H

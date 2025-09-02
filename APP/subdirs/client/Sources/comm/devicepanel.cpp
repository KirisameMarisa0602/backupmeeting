#include "devicepanel.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QtCharts/QLineSeries>
#include <QtMath>
#include <QSignalBlocker>

QT_CHARTS_USE_NAMESPACE

static quint32 stableHash(const QString& s) {
    const QByteArray bytes = s.toUtf8();
    quint32 h = 2166136261u;
    for (int i=0;i<bytes.size();++i) { h ^= (uchar)bytes[i]; h *= 16777619u; }
    h ^= (h >> 13); h *= 0x9e3779b1u; h ^= (h >> 11);
    return h;
}

DevicePanel::DevicePanel(QWidget* parent)
    : QWidget(parent),
      pressureChart_(0),
      tempChart_(0),
      tableLogs_(0),
      tableFaults_(0),
      cbDevice_(0),
      lblPressure_(0),
      lblTemperature_(0),
      editControl_(0),
      btnSendControl_(0)
{
    setupUi();
    connect(&timer_, SIGNAL(timeout()), this, SLOT(onTickUpdate()));
    timer_.start(1000);
}

void DevicePanel::setupUi() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    QHBoxLayout* top = new QHBoxLayout();
    cbDevice_ = new QComboBox(this);
    top->addWidget(new QLabel(QString::fromUtf8("选择设备:")));
    top->addWidget(cbDevice_,1);

    lblPressure_    = new QLabel(QString::fromUtf8("压力: --"), this);
    lblTemperature_ = new QLabel(QString::fromUtf8("温度: --"), this);
    QFont f; f.setPointSize(16); f.setBold(true);
    lblPressure_->setFont(f);
    lblTemperature_->setFont(f);
    top->addWidget(lblPressure_);
    top->addWidget(lblTemperature_);
    mainLayout->addLayout(top);

    QHBoxLayout* ctl = new QHBoxLayout();
    editControl_ = new QLineEdit(this);
    editControl_->setPlaceholderText(QString::fromUtf8("请输入控制指令 (START/STOP/RESET...)"));
    btnSendControl_ = new QPushButton(QString::fromUtf8("发送指令"), this);
    ctl->addWidget(new QLabel(QString::fromUtf8("远程控制指令:")));
    ctl->addWidget(editControl_,2);
    ctl->addWidget(btnSendControl_);
    mainLayout->addLayout(ctl);
    connect(btnSendControl_, SIGNAL(clicked()), this, SLOT(onSendControl()));

    QHBoxLayout* charts = new QHBoxLayout();
    pressureChart_ = new QChartView(new QChart(), this);
    tempChart_     = new QChartView(new QChart(), this);
    pressureChart_->setMinimumSize(360,240);
    tempChart_->setMinimumSize(360,240);
    charts->addWidget(pressureChart_,1);
    charts->addWidget(tempChart_,1);
    mainLayout->addLayout(charts);

    QHBoxLayout* info = new QHBoxLayout();
    tableLogs_ = new QTableWidget(this);
    tableLogs_->setColumnCount(1);
    QStringList hdr1; hdr1 << QString::fromUtf8("运行日志");
    tableLogs_->setHorizontalHeaderLabels(hdr1);
    tableLogs_->setMinimumWidth(280);
    tableLogs_->horizontalHeader()->setStretchLastSection(true);

    tableFaults_ = new QTableWidget(this);
    tableFaults_->setColumnCount(1);
    QStringList hdr2; hdr2 << QString::fromUtf8("故障信息");
    tableFaults_->setHorizontalHeaderLabels(hdr2);
    tableFaults_->setMinimumWidth(200);
    tableFaults_->horizontalHeader()->setStretchLastSection(true);

    info->addWidget(tableLogs_,1);
    info->addWidget(tableFaults_,1);
    mainLayout->addLayout(info);

    // 原来这里是连接到 refreshUi：
    // connect(cbDevice_, SIGNAL(currentIndexChanged(int)), this, SLOT(refreshUi()));
    // 改为连接到 onDeviceSelected（内部会广播并调用 refreshUi）
    connect(cbDevice_, SIGNAL(currentIndexChanged(int)), this, SLOT(onDeviceSelected(int)));
}

void DevicePanel::setOrderContext(const QString& orderId)
{
    currentOrderId_ = orderId;
    buildDeterministicDevices(orderId);
    // 在图表标题中展示工单号
    if (pressureChart_ && pressureChart_->chart()) {
        pressureChart_->chart()->setTitle(QString::fromUtf8("压力曲线（工单 %1）").arg(orderId));
    }
    if (tempChart_ && tempChart_->chart()) {
        tempChart_->chart()->setTitle(QString::fromUtf8("温度曲线（工单 %1）").arg(orderId));
    }
    refreshUi();
}

void DevicePanel::buildDeterministicDevices(const QString& orderId)
{
    devices_.clear();
    pressureHist_.clear();
    tempHist_.clear();
    cbDevice_->clear();

    QByteArray seed = QCryptographicHash::hash(orderId.toUtf8(), QCryptographicHash::Sha256);
    if (seed.isEmpty()) seed = QByteArray(32,'\0');

    for (int i=0;i<4;++i){
        DeviceInfo d;
        d.name = QString::fromUtf8("设备%1").arg(i+1);
        int b1 = (uchar)seed[(i*4)%seed.size()];
        int b2 = (uchar)seed[(i*4+1)%seed.size()];
        d.basePressure = 10.0 + (b1 % 70)/10.0;
        d.baseTemp     = 40.0 + (b2 %100)/10.0;
        d.status = (b1 & 1) ? QString::fromUtf8("运行") : QString::fromUtf8("待机");
        if (b2 & 0x20) d.faults << QString::fromUtf8("温度传感器提示");
        d.logs << QString::fromUtf8("初始化完成");
        devices_.push_back(d);
        pressureHist_.push_back(QVector<QPointF>());
        tempHist_.push_back(QVector<QPointF>());
        pushPoint(i, d.basePressure, d.baseTemp);
        pushPoint(i, d.basePressure, d.baseTemp);
        cbDevice_->addItem(d.name);
    }
}

void DevicePanel::pushPoint(int idx, double pressure, double temp)
{
    if (idx < 0 || idx >= devices_.size()) return;
    double now = QDateTime::currentMSecsSinceEpoch()/1000.0;
    pressureHist_[idx].push_back(QPointF(now, pressure));
    tempHist_[idx].push_back(QPointF(now, temp));
    if (pressureHist_[idx].size() > kMaxPoints)
        pressureHist_[idx].erase(pressureHist_[idx].begin(),
                                 pressureHist_[idx].begin() + (pressureHist_[idx].size()-kMaxPoints));
    if (tempHist_[idx].size() > kMaxPoints)
        tempHist_[idx].erase(tempHist_[idx].begin(),
                             tempHist_[idx].begin() + (tempHist_[idx].size()-kMaxPoints));
}

void DevicePanel::onTickUpdate()
{
    if (devices_.isEmpty()) return;
    qint64 t = QDateTime::currentMSecsSinceEpoch();
    for (int i=0;i<devices_.size();++i){
        DeviceInfo &d = devices_[i];
        quint32 phaseSeed = stableHash(currentOrderId_ + d.name);
        double phase = ( (t/1000.0) + (phaseSeed % 97) ) / 6.0;
        double wave = qSin(phase) * 0.5;
        double pressure = d.basePressure + wave;
        double temp     = d.baseTemp     + wave * 1.2;
        pushPoint(i, pressure, temp);
    }
    refreshUi();
}

void DevicePanel::refreshUi()
{
    int idx = cbDevice_->currentIndex();
    if (idx<0 || idx>=devices_.size()) return;
    const DeviceInfo& d = devices_[idx];

    double pressure = pressureHist_[idx].isEmpty() ? d.basePressure : pressureHist_[idx].last().y();
    double temp     = tempHist_[idx].isEmpty()     ? d.baseTemp     : tempHist_[idx].last().y();

    lblPressure_->setText(QString::fromUtf8("压力: %1 MPa").arg(pressure,0,'f',2));
    lblTemperature_->setText(QString::fromUtf8("温度: %1 ℃").arg(temp,0,'f',2));

    // 压力曲线
    QLineSeries* ps = new QLineSeries;
    for (int i=0;i<pressureHist_[idx].size();++i) ps->append(pressureHist_[idx][i]);
    QChart* pc = pressureChart_->chart();
    pc->removeAllSeries();
    pc->addSeries(ps);
    pc->createDefaultAxes();
    if (!pressureHist_[idx].isEmpty()){
        double minX = pressureHist_[idx].first().x();
        double maxX = pressureHist_[idx].last().x();
        pc->axes(Qt::Horizontal).first()->setRange(minX, maxX);
        pc->axes(Qt::Vertical).first()->setRange(d.basePressure-2, d.basePressure+2);
    }
    pc->setTitle(QString::fromUtf8("压力曲线（工单 %1）").arg(currentOrderId_));

    // 温度曲线
    QLineSeries* ts = new QLineSeries;
    for (int i=0;i<tempHist_[idx].size();++i) ts->append(tempHist_[idx][i]);
    QChart* tc = tempChart_->chart();
    tc->removeAllSeries();
    tc->addSeries(ts);
    tc->createDefaultAxes();
    if (!tempHist_[idx].isEmpty()){
        double minX = tempHist_[idx].first().x();
        double maxX = tempHist_[idx].last().x();
        tc->axes(Qt::Horizontal).first()->setRange(minX, maxX);
        tc->axes(Qt::Vertical).first()->setRange(d.baseTemp-3, d.baseTemp+3);
    }
    tc->setTitle(QString::fromUtf8("温度曲线（工单 %1）").arg(currentOrderId_));

    // 日志/故障
    tableLogs_->setRowCount(devices_[idx].logs.size());
    for (int i=0;i<devices_[idx].logs.size();++i)
        tableLogs_->setItem(i,0,new QTableWidgetItem(devices_[idx].logs[i]));
    tableFaults_->setRowCount(devices_[idx].faults.size());
    for (int i=0;i<devices_[idx].faults.size();++i)
        tableFaults_->setItem(i,0,new QTableWidgetItem(devices_[idx].faults[i]));
}

void DevicePanel::appendLog(int idx, const QString& log)
{
    if (idx<0 || idx>=devices_.size()) return;
    devices_[idx].logs << log;
    while (devices_[idx].logs.size() > 100)
        devices_[idx].logs.removeFirst();
    if (cbDevice_->currentIndex()==idx)
        refreshUi();
}

void DevicePanel::applyControlCommand(const QString& device,
                                      const QString& command,
                                      const QString& sender,
                                      qint64 tsMs)
{
    // 优先处理“SELECT”命令（对端切换设备）
    if (command.compare(QString::fromUtf8("SELECT"), Qt::CaseInsensitive) == 0 && cbDevice_) {
        for (int i = 0; i < cbDevice_->count(); ++i) {
            if (cbDevice_->itemText(i) == device) {
                const QSignalBlocker blocker(cbDevice_);
                suppressSelectionBroadcast_ = true;
                cbDevice_->setCurrentIndex(i);
                suppressSelectionBroadcast_ = false;
                break;
            }
        }
        int idx = cbDevice_->currentIndex();
        if (idx >= 0 && idx < devices_.size()) {
            appendLog(idx, QString::fromUtf8("[%1] 对端选择了设备：%2")
                              .arg(QDateTime::fromMSecsSinceEpoch(tsMs).toString("HH:mm:ss"))
                              .arg(device));
        }
        refreshUi();
        return;
    }

    // 其它命令：记录日志
    int idx=-1;
    for (int i=0;i<devices_.size();++i)
        if (devices_[i].name==device){ idx=i; break; }
    if (idx<0) return;
    QString timeStr = QDateTime::fromMSecsSinceEpoch(tsMs).toString("HH:mm:ss");
    appendLog(idx, QString::fromUtf8("设备收到指令: %1 (来自:%2) [%3]")
                     .arg(command, sender, timeStr));
}

void DevicePanel::onDeviceSelected(int)
{
    if (suppressSelectionBroadcast_) {
        refreshUi();
        return;
    }
    int idx = cbDevice_ ? cbDevice_->currentIndex() : -1;
    if (idx >= 0 && idx < devices_.size()) {
        // 本端选择设备 -> 广播给对端保持同步
        emit deviceControlSent(devices_[idx].name, QString::fromUtf8("SELECT"));
    }
    refreshUi();
}

void DevicePanel::onSendControl()
{
    int idx = cbDevice_->currentIndex();
    if (idx<0 || idx>=devices_.size()) return;
    QString dev = devices_[idx].name;
    QString cmd = editControl_->text().trimmed();
    if (cmd.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("提示"), QString::fromUtf8("请输入控制指令"));
        return;
    }
    emit deviceControlSent(dev, cmd);
    editControl_->clear();
}

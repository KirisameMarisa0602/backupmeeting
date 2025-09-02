#include "knowledge_panel.h"
#include "kb_client.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QTableWidget>
#include <QHeaderView>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDesktopServices>
#include <QUrl>
#include <QFileInfo>
#include <QDir>
#include <QProcess>
#include <QApplication>
#include <QSettings>
#include <QFileDialog>
#include <QStandardPaths>
#include <QDebug>

KnowledgePanel::KnowledgePanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("KnowledgePanelRoot");
    setStyleSheet(
        "#KnowledgePanelRoot { background:#121212; color:#e6e6e6; }"
        "#KnowledgePanelRoot QLineEdit, "
        "#KnowledgePanelRoot QSpinBox { background:#1e1e1e; color:#eeeeee; border:1px solid #3a3a3a; padding:4px; }"
        "#KnowledgePanelRoot QPushButton { background:#2b2b2b; color:#eaeaea; border:1px solid #3a3a3a; padding:5px 10px; }"
        "#KnowledgePanelRoot QTableWidget { background:#151515; color:#dddddd; gridline-color:#333333; }"
        "#KnowledgePanelRoot QHeaderView::section { background:#1b1b1b; color:#d0d0d0; border:1px solid #333; padding:4px; }"
        "#KnowledgePanelRoot QTableWidget::item:selected { background:#2d6cdf; color:#ffffff; }"
    );

    // 顶部操作条
    hostEdit_    = new QLineEdit(this);
    hostEdit_->setPlaceholderText(QStringLiteral("Host，例如 127.0.0.1"));
    hostEdit_->setText(QStringLiteral("127.0.0.1"));

    portSpin_    = new QSpinBox(this);
    portSpin_->setRange(1, 65535);
    portSpin_->setValue(5555);

    roomEdit_    = new QLineEdit(this);
    roomEdit_->setPlaceholderText(QStringLiteral("房间（可留空查看全部）"));

    refreshBtn_  = new QPushButton(QStringLiteral("刷新"), this);

    auto* topBar = new QHBoxLayout();
    topBar->setContentsMargins(8, 8, 8, 8);
    topBar->setSpacing(8);
    topBar->addWidget(hostEdit_, 0);
    topBar->addWidget(portSpin_, 0);
    topBar->addWidget(roomEdit_, 1);
    topBar->addWidget(refreshBtn_, 0);

    // 表格
    table_ = new QTableWidget(this);
    table_->setColumnCount(8);
    QStringList headers;
    headers << QStringLiteral("ID")
            << QStringLiteral("房间")
            << QStringLiteral("用户")
            << QStringLiteral("路径")
            << QStringLiteral("类型")
            << QStringLiteral("标题")
            << QStringLiteral("开始")
            << QStringLiteral("结束");
    table_->setHorizontalHeaderLabels(headers);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(7, QHeaderView::ResizeToContents);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addLayout(topBar);
    lay->addWidget(table_);

    connect(refreshBtn_, &QPushButton::clicked, this, &KnowledgePanel::refresh);
    connect(table_, &QTableWidget::cellDoubleClicked, this, &KnowledgePanel::onTableDoubleClicked);

    qInfo() << "[KB] KnowledgePanel ctor";
}

// 兼容旧接口
void KnowledgePanel::setServer(const QHostAddress& host, quint16 port) {
    hostEdit_->setText(host.toString());
    portSpin_->setValue(port);
}
void KnowledgePanel::setServer(const QString& host, quint16 port) {
    hostEdit_->setText(host.trimmed());
    portSpin_->setValue(port);
}
void KnowledgePanel::setRoomFilter(const QString& room) {
    roomEdit_->setText(room.trimmed());
}

QString KnowledgePanel::hostText() const { return hostEdit_->text().trimmed(); }
quint16 KnowledgePanel::portValue() const { return quint16(portSpin_->value()); }
QString KnowledgePanel::roomFilter() const { return roomEdit_->text().trimmed(); }

void KnowledgePanel::setBusy(bool on)
{
    refreshBtn_->setEnabled(!on);
    if (on) QApplication::setOverrideCursor(Qt::BusyCursor);
    else    QApplication::restoreOverrideCursor();
}

void KnowledgePanel::refresh()
{
    setBusy(true);
    table_->setRowCount(0);

    const QString hostStr = hostEdit_->text().trimmed();
    const QHostAddress host(hostStr);
    const quint16 port = quint16(portSpin_->value());
    const QString room = roomEdit_->text().trimmed();

    qInfo() << "[KB] refresh() host=" << hostStr << "port=" << port << "room=" << room;

    const QJsonObject recResp = KbClient::getRecordings(host, port, room);
    qInfo().noquote() << "[KB] recResp =" << QString::fromUtf8(QJsonDocument(recResp).toJson(QJsonDocument::Compact));

    if (!recResp.value("ok").toBool()) {
        QMessageBox::warning(this, QStringLiteral("查询失败"),
                             QStringLiteral("get_recordings 失败: %1").arg(recResp.value("msg").toString()));
        setBusy(false);
        return;
    }
    const QJsonArray recItems = recResp.value("items").toArray();
    qInfo() << "[KB] recordings count =" << recItems.size();

    int rowsAdded = 0;

    for (const auto& v : recItems) {
        const QJsonObject rec = v.toObject();
        const int recId = rec.value("id").toInt();
        const QString roomId = rec.value("room_id").toString();
        const QString title = rec.value("title").toString();
        const QString started = rec.value("started_at").toVariant().toString();
        const QString ended   = rec.value("ended_at").toVariant().toString();

        const QJsonObject filesResp = KbClient::getRecordingFiles(host, port, recId);
        qInfo().noquote() << "[KB] filesResp(recId=" << recId << ") ="
                          << QString::fromUtf8(QJsonDocument(filesResp).toJson(QJsonDocument::Compact));

        if (!filesResp.value("ok").toBool()) {
            qWarning() << "[KB] getRecordingFiles failed recId=" << recId
                       << "msg=" << filesResp.value("msg").toString();
            continue;
        }
        const QJsonArray files = filesResp.value("files").toArray();
        qInfo() << "[KB] files count for recId=" << recId << ":" << files.size();

        for (const auto& fv : files) {
            const QJsonObject f = fv.toObject();
            const QString user  = f.value("user").toString();
            const QString path  = f.value("file_path").toString();
            const QString kind  = f.value("kind").toString();

            const int row = table_->rowCount();
            table_->insertRow(row);

            auto put = [&](int col, const QString& text){
                auto* it = new QTableWidgetItem(text);
                it->setToolTip(text);
                table_->setItem(row, col, it);
            };
            put(0, QString::number(recId));
            put(1, roomId);
            put(2, user);
            put(3, path);
            put(4, kind);
            put(5, title);
            put(6, started);
            put(7, ended);

            ++rowsAdded;
        }
    }

    qInfo() << "[KB] rows added to table =" << rowsAdded;
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    setBusy(false);
}

QString KnowledgePanel::findFfplay() const
{
    const QString env = QString::fromLocal8Bit(qgetenv("FFPLAY_PATH"));
    if (!env.isEmpty() && QFileInfo(env).exists()) return env;

    const QStringList candidates{
        QStringLiteral("/usr/bin/ffplay"),
        QStringLiteral("/usr/local/bin/ffplay"),
        QStringLiteral("C:/ffmpeg/bin/ffplay.exe")
    };
    for (const auto& c : candidates) if (QFileInfo(c).exists()) return c;
    return QString();
}

bool KnowledgePanel::isValidKnowledgeRoot(const QString& root) const
{
    if (root.isEmpty()) return false;
    QDir d(root);
    return d.exists() && d.exists("knowledge");
}

QString KnowledgePanel::knowledgeRoot(bool interactive) const
{
    // 1) cache
    if (isValidKnowledgeRoot(rootCache_)) return rootCache_;

    // 2) env
    const QString env = QString::fromLocal8Bit(qgetenv("KNOWLEDGE_ROOT"));
    if (isValidKnowledgeRoot(env)) {
        rootCache_ = QDir(env).absolutePath();
        qInfo() << "[KB] knowledgeRoot from env =" << rootCache_;
        return rootCache_;
    }

    // 3) QSettings
    QSettings s("VideoClient", "ClientApp");
    const QString saved = s.value("knowledge/root").toString();
    if (isValidKnowledgeRoot(saved)) {
        rootCache_ = QDir(saved).absolutePath();
        qInfo() << "[KB] knowledgeRoot from settings =" << rootCache_;
        return rootCache_;
    }

    // 4) 自动探测：从可执行所在目录往上找，最多 5 层
    QString appDir = QCoreApplication::applicationDirPath();
    QDir dir(appDir);
    for (int i = 0; i < 6; ++i) {
        QString cand = dir.absolutePath();
        if (isValidKnowledgeRoot(cand)) {
            rootCache_ = cand;
            s.setValue("knowledge/root", rootCache_);
            qInfo() << "[KB] knowledgeRoot auto-detected =" << rootCache_;
            return rootCache_;
        }
        // 常见：.../client 与 .../server 并列
        if (dir.exists("server") && QDir(dir.filePath("server")).exists("knowledge")) {
            rootCache_ = dir.filePath("server");
            s.setValue("knowledge/root", rootCache_);
            qInfo() << "[KB] knowledgeRoot auto-detected (server sibling) =" << rootCache_;
            return rootCache_;
        }
        dir.cdUp();
    }

    // 5) 交互式选择
    if (interactive) {
        QMessageBox::information(nullptr, QStringLiteral("选择知识库根目录"),
                                 QStringLiteral("未检测到 KNOWLEDGE_ROOT。请选择包含“knowledge”子目录的服务器根目录。"));
        QString start = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
        QString pick = QFileDialog::getExistingDirectory(nullptr, QStringLiteral("选择服务器根目录（包含 knowledge 子目录）"), start);
        if (isValidKnowledgeRoot(pick)) {
            rootCache_ = QDir(pick).absolutePath();
            s.setValue("knowledge/root", rootCache_);
            qInfo() << "[KB] knowledgeRoot picked by user =" << rootCache_;
            return rootCache_;
        }
    }
    return QString();
}

QString KnowledgePanel::resolveAbsolutePath(const QString& relOrAbs, bool interactive) const
{
    QFileInfo fi(relOrAbs);
    if (fi.isAbsolute()) return fi.absoluteFilePath();

    // 相对路径：用 root 拼接
    const QString root = knowledgeRoot(interactive);
    if (!root.isEmpty()) {
        QString joined = QDir(root).filePath(relOrAbs);
        return QFileInfo(joined).absoluteFilePath();
    }
    // 没有 root 时，仍返回原字符串（让后续错误提示更直观）
    return relOrAbs;
}

void KnowledgePanel::playFile(const QString& filePath) const
{
    // URL 直接打开
    if (filePath.startsWith("http://") || filePath.startsWith("https://")) {
        QDesktopServices::openUrl(QUrl(filePath));
        return;
    }

    const QString abs = resolveAbsolutePath(filePath, /*interactive*/true);
    QFileInfo fi(abs);
    if (!fi.exists()) {
        QMessageBox::warning(nullptr, QStringLiteral("文件不存在"),
                             QStringLiteral("找不到文件:\n%1\n\n提示：如果这是相对路径，请设置环境变量 KNOWLEDGE_ROOT，"
                                            "或在弹出的选择框中选择包含 knowledge 子目录的服务器根目录。")
                                 .arg(abs));
        return;
    }

    const QString ffplay = findFfplay();
    if (!ffplay.isEmpty()) {
        QProcess::startDetached(ffplay, QStringList() << "-autoexit" << "-fs" << fi.absoluteFilePath());
        return;
    }
    // 没有 ffplay 时退回系统默认播放器
    QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absoluteFilePath()));
}

void KnowledgePanel::onTableDoubleClicked(int row, int /*col*/)
{
    if (row < 0) return;
    auto* it = table_->item(row, 3);
    if (!it) return;
    playFile(it->text());
}

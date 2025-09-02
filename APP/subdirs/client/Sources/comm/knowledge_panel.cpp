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

static QString buildPanelQss(const QString& theme) {
    // theme: "expert" | "factory" | "none"
    QString bgGrad, primary, primaryBorder, primaryHover, selBg, selFg;
    if (theme == "expert") {
        bgGrad        = "qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #0d47a1, stop:1 #bbdefb)";
        primary       = "#1976d2";
        primaryBorder = "#115293";
        primaryHover  = "#1e88e5";
        selBg         = "#90caf9";
        selFg         = "#0b1020";
    } else if (theme == "factory") {
        bgGrad        = "qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #1b5e20, stop:1 #c8e6c9)";
        primary       = "#2e7d32";
        primaryBorder = "#1b5e20";
        primaryHover  = "#388e3c";
        selBg         = "#a5d6a7";
        selFg         = "#0b1020";
    } else {
        bgGrad        = "qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #f3f4f6, stop:1 #e5e7eb)";
        primary       = "#6b7280";
        primaryBorder = "#4b5563";
        primaryHover  = "#7b8391";
        selBg         = "#cbd5e1";
        selFg         = "#0b1020";
    }

    const QString textMain    = "#111827";
    const QString fieldBg     = "#ffffff";
    const QString fieldBorder = "#cfd8dc";
    const QString tableBg     = "#ffffff";
    const QString tableText   = "#0b1020";
    const QString gridColor   = "#e5e7eb";
    const QString headerFg    = "#ffffff";

    QString qss =
        "#KnowledgePanelRoot { background:%1; color:%2; }"
        "#KnowledgePanelRoot QLineEdit, "
        "#KnowledgePanelRoot QSpinBox { background:%3; color:%2; border:1px solid %4; padding:6px; border-radius:10px; }"
        "#KnowledgePanelRoot QPushButton { background:%5; color:#ffffff; border:1px solid %6; padding:6px 12px; border-radius:10px; }"
        "#KnowledgePanelRoot QPushButton:hover { background:%7; }"
        "#KnowledgePanelRoot QTableWidget { background:%8; color:%9; gridline-color:%10; border:1px solid %10; border-radius:10px; }"
        "#KnowledgePanelRoot QHeaderView::section { background:%5; color:%11; border:none; padding:6px; }"
        "#KnowledgePanelRoot QTableWidget::item:selected { background:%12; color:%13; }";

    // 依次替换 %1..%13
    qss = qss
        .arg(bgGrad)      // %1
        .arg(textMain)    // %2
        .arg(fieldBg)     // %3
        .arg(fieldBorder) // %4
        .arg(primary)     // %5
        .arg(primaryBorder) // %6
        .arg(primaryHover)  // %7
        .arg(tableBg)       // %8
        .arg(tableText)     // %9
        .arg(gridColor)     // %10
        .arg(headerFg)      // %11
        .arg(selBg)         // %12
        .arg(selFg);        // %13

    return qss;
}

static QString detectThemeFromTopLevel(QWidget* w) {
    if (!w) return "none";
    QWidget* tlw = w->window();
    if (!tlw) {
        // 兜底：向上找
        tlw = w;
        while (tlw->parentWidget()) tlw = tlw->parentWidget();
    }
    const QString name = tlw->objectName();
    if (name == "ClientExpert")  return "expert";
    if (name == "ClientFactory") return "factory";
    return "none";
}

KnowledgePanel::KnowledgePanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("KnowledgePanelRoot");

    // 根据顶层窗口类型（ClientExpert/ClientFactory）应用主题渐变
    const QString theme = detectThemeFromTopLevel(this);
    setStyleSheet(buildPanelQss(theme));

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
    auto normalize = [](QString p){
        p = QDir::cleanPath(p);
#ifdef Q_OS_WIN
        p.replace('\\', '/');
#endif
        return p;
    };
    auto joinWithRoot = [&](const QString& rel)->QString {
        const QString root = knowledgeRoot(interactive);
        if (root.isEmpty()) return rel;
        QString joined = QDir(root).filePath(rel);
        return QFileInfo(joined).absoluteFilePath();
    };

    QFileInfo fi(relOrAbs);
    if (fi.isAbsolute()) {
        // 若为服务器上的绝对路径，尝试映射到本机的知识库根路径
        QString abs = normalize(fi.absoluteFilePath());
        int idx = abs.indexOf("/knowledge/");
        if (idx >= 0) {
            // 截取从 knowledge/ 起的相对部分
            QString rel = abs.mid(idx + 1); // 去掉开头的 '/'
            return joinWithRoot(rel);
        }
        return fi.absoluteFilePath();
    }

    // 相对路径：若已包含 knowledge/ 前缀，直接与本机根拼接；否则仍按相对路径与根拼接
    QString rel = normalize(relOrAbs);
    if (rel.startsWith("./")) rel = rel.mid(2);
    if (rel.startsWith("/"))  rel = rel.mid(1);
    return joinWithRoot(rel);
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

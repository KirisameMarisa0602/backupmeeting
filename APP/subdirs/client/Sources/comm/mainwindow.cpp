#include "mainwindow.h"

#include <QBuffer>
#include <QCamera>
#include <QCameraInfo>
#include <QCameraViewfinderSettings>

#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QImageWriter>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaObject>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRegExp>
#include <QScrollArea>
#include <QSet>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QTextEdit>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoProbe>
#include <QColorDialog>
#include <QtMath>
#include <cmath>

#include "knowledge_panel.h"   // [KB] 新增
#include <QHostAddress>        // [KB] 新增：onOpenKnowledge 里用到
#include "knowledge_tab_helper.h"
#include "annot.h"
#include "annotcanvas.h"
#include "protocol.h"
#include "udpmedia.h"
#include "volume_popup.h"

// ---------------------------- 小部件与帮助函数（聊天预览） ----------------------------

// 简单的可点击图片缩略标签，点击后弹出大图预览对话框
class ImageThumbLabel : public QLabel {
public:
    explicit ImageThumbLabel(QWidget* parent=nullptr) : QLabel(parent) {
        setCursor(Qt::PointingHandCursor);
    }
    void setFullImage(const QImage& img) { full_ = img; }
protected:
    void mouseReleaseEvent(QMouseEvent* ev) override {
        if (ev->button() == Qt::LeftButton && !full_.isNull()) {
            class PreviewImageLabel : public QLabel {
            public:
                explicit PreviewImageLabel(QWidget* parent=nullptr) : QLabel(parent) {
                    setAlignment(Qt::AlignCenter);
                }
                void setImage(const QImage& img) { img_ = img; updatePixmap(); }
            protected:
                void resizeEvent(QResizeEvent* e) override {
                    QLabel::resizeEvent(e);
                    updatePixmap();
                }
            private:
                void updatePixmap() {
                    if (img_.isNull()) { setPixmap(QPixmap()); return; }
                    const QSize target = QSize(int(width()*0.95), int(height()*0.95));
                    setPixmap(QPixmap::fromImage(img_).scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                }
                QImage img_;
            };

            QDialog dlg(this);
            dlg.setWindowTitle(QStringLiteral("图片预览"));
            dlg.resize(900, 700);

            QVBoxLayout lay(&dlg);
            PreviewImageLabel big(&dlg);
            big.setImage(full_);
            lay.addWidget(&big, 1);

            dlg.exec();
        }
        QLabel::mouseReleaseEvent(ev);
    }
private:
    QImage full_;
};

// 将图像按控件尺寸等比例缩放后设置
static void fitLabelImage(QLabel* lbl, const QImage& img) {
    if (!lbl) return;
    if (img.isNull()) { lbl->clear(); return; }
    const QSize s = lbl->size();
    if (s.width() < 2 || s.height() < 2) return;
    QPixmap pm = QPixmap::fromImage(img).scaled(s, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    lbl->setPixmap(pm);
    lbl->setText(QString());
}

static VideoTile* makeTile(QWidget* parent, const QString& nameText) {
    auto* box = new QWidget(parent);
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(2,2,2,2);
    v->setSpacing(2);

    auto* name = new QLabel(nameText, box);
    name->setAlignment(Qt::AlignCenter);
    name->setStyleSheet("font-weight:bold;");

    auto* video = new QLabel(QStringLiteral("等待视频/屏幕..."), box);
    video->setMinimumSize(200,150);
    video->setStyleSheet("border:1px solid #888;");
    video->setAlignment(Qt::AlignCenter);
    video->setScaledContents(false);
    video->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    video->setCursor(Qt::PointingHandCursor);

    // 底部行：名称 + 音量按钮
    auto* bottom = new QHBoxLayout();
    auto* volBtn = new QToolButton(box);
    volBtn->setText(QStringLiteral("音量"));
    volBtn->setToolTip(QStringLiteral("调节此路音量/麦克风"));
    volBtn->setCursor(Qt::PointingHandCursor);

    bottom->addWidget(name, 1);
    bottom->addWidget(volBtn, 0);

    v->addWidget(video, /*stretch*/1);
    v->addLayout(bottom,  /*stretch*/0);

    auto* t = new VideoTile;
    t->box = box; t->name = name; t->video = video; t->volBtn = volBtn;
    t->timer = new QTimer(box);
    t->timer->setSingleShot(true);
    t->camPrimary = false; // 默认屏幕为大图
    return t;
}

// 全局（文件内）聊天列表指针，避免改动头文件
static QListWidget* sChatList = nullptr;

// 根据顶层窗口对象名判定主题（仅UI用途）
static QString detectThemeFromTopLevel(QWidget* w) {
    if (!w) return "none";
    QWidget* tlw = w->window();
    if (!tlw) {
        tlw = w;
        while (tlw->parentWidget()) tlw = tlw->parentWidget();
    }
    const QString name = tlw->objectName();
    if (name == "ClientExpert")  return "expert";
    if (name == "ClientFactory") return "factory";
    return "none";
}

// 生成聊天列表的样式（渐变背景，不涉及功能）
static QString chatListQssForTheme(const QString& theme) {
    QString bgGrad, border;
    if (theme == "expert") {
        bgGrad = "qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #0d47a1, stop:1 #bbdefb)";
        border = "#115293";
    } else if (theme == "factory") {
        bgGrad = "qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #1b5e20, stop:1 #c8e6c9)";
        border = "#1b5e20";
    } else {
        bgGrad = "qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #f3f4f6, stop:1 #e5e7eb)";
        border = "#cbd5e1";
    }
    QString qss =
        "QListWidget, QListWidget::viewport {"
        "  background:%1;"
        "  border:1px solid %2;"
        "}"
        "QLabel{background:transparent;}";
    return qss.arg(bgGrad, border);
}

// 生成一个“气泡”外观的消息项容器（包含发送者标签与内容控件）
static QWidget* makeBubble(const QString& sender, QWidget* content, bool outgoing) {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 4, 8, 8);
    v->setSpacing(4);

    auto* header = new QLabel(sender, w);
    header->setStyleSheet("color:#999; font-size:10px;");
    v->addWidget(header, 0, outgoing ? Qt::AlignRight : Qt::AlignLeft);

    auto* bubble = new QFrame(w);
    bubble->setFrameShape(QFrame::NoFrame);
    bubble->setStyleSheet(outgoing
                          ? "background:#2e7d32; color:#fff; border-radius:8px;"
                          : "background:#2b2b2b; color:#ddd; border-radius:8px;");
    auto* bl = new QVBoxLayout(bubble);
    bl->setContentsMargins(10, 8, 10, 8);
    bl->addWidget(content);
    v->addWidget(bubble, 0, outgoing ? Qt::AlignRight : Qt::AlignLeft);
    return w;
}

static void chatAddWidget(const QString& sender, QWidget* content, bool outgoing) {
    if (!sChatList || !content) return;
    auto* item = new QListWidgetItem(sChatList);
    auto* bubble = makeBubble(sender, content, outgoing);
    item->setSizeHint(bubble->sizeHint());
    sChatList->addItem(item);
    sChatList->setItemWidget(item, bubble);
    sChatList->scrollToBottom();
}

static void chatAddText(const QString& sender, const QString& text, bool outgoing) {
    auto* lbl = new QLabel(text);
    lbl->setWordWrap(true);
    lbl->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    chatAddWidget(sender, lbl, outgoing);
}

static void chatAddImage(const QString& sender, const QImage& img, bool outgoing) {
    if (img.isNull()) return;
    auto* lbl = new ImageThumbLabel;
    lbl->setFullImage(img);
    const int maxW = 240;
    QPixmap thumb = QPixmap::fromImage(img.scaled(maxW, maxW, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    lbl->setPixmap(thumb);
    chatAddWidget(sender, lbl, outgoing);
}

static void chatAddFile(const QString& sender, const QString& filename, const QString& mime,
                        const QByteArray& data, bool outgoing)
{
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0,0,0,0);
    h->setSpacing(8);

    QLabel* icon = new QLabel(w);
    icon->setPixmap(QApplication::style()->standardIcon(QStyle::SP_FileIcon).pixmap(20,20));
    QLabel* name = new QLabel(QString("%1\n%2").arg(filename, mime), w);
    name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QPushButton* openBtn = new QPushButton(QStringLiteral("打开"), w);

    h->addWidget(icon, 0);
    h->addWidget(name, 1);
    h->addWidget(openBtn, 0);

    // 点击“打开”时落盘到下载目录并用系统默认程序打开
    QObject::connect(openBtn, &QPushButton::clicked, [filename, data]() {
        QString baseDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        if (baseDir.isEmpty()) baseDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        QDir dir(baseDir + "/VideoClientDownloads");
        dir.mkpath(".");
        QString outName = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_") + filename;
        outName.replace(QRegExp("[\\\\/:*?\"<>|]"), "_");
        QString full = dir.filePath(outName);
        QFile f(full);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(data);
            f.close();
            QDesktopServices::openUrl(QUrl::fromLocalFile(full));
        }
    });

    chatAddWidget(sender, w, outgoing);
}

// ---------------------------- MainWindow 逻辑 ----------------------------

QImage MainWindow::composeTileImage(const VideoTile* t, const QSize& target)
{
    if (!t || !target.isValid() || target.width()<2 || target.height()<2) return QImage();

    const bool hasCam    = !t->lastCam.isNull();
    const bool hasScreen = !t->lastScreen.isNull();

    QImage bg(target, QImage::Format_RGB32);
    bg.fill(Qt::black);

    QPainter p(&bg);
    p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform, true);

    auto drawFit = [&](const QImage& img, const QRect& rect){
        if (img.isNull() || rect.isEmpty()) return;
        QSize fitted = img.size();
        fitted.scale(rect.size(), Qt::KeepAspectRatio);
        QPoint topLeft(rect.x() + (rect.width()-fitted.width())/2,
                       rect.y() + (rect.height()-fitted.height())/2);
        p.drawImage(QRect(topLeft, fitted), img);
    };

    if (!hasCam && !hasScreen) { p.end(); return QImage(); }
    if (hasCam && !hasScreen)  { drawFit(t->lastCam,   QRect(QPoint(0,0), target)); p.end(); return bg; }
    if (!hasCam && hasScreen)  { drawFit(t->lastScreen, QRect(QPoint(0,0), target)); p.end(); return bg; }

    // PiP
    const QImage& bigImg   = t->camPrimary ? t->lastCam   : t->lastScreen;
    const QImage& smallImg = t->camPrimary ? t->lastScreen : t->lastCam;

    drawFit(bigImg, QRect(QPoint(0,0), target));

    const int margin = 8;
    int smallW = qMax(80, target.width() * 28 / 100);
    int smallH = smallW * smallImg.height() / qMax(1, smallImg.width());
    if (smallH > target.height() * 40 / 100) {
        smallH = target.height() * 40 / 100;
        smallW = smallH * smallImg.width() / qMax(1, smallImg.height());
    }
    QRect smallRect(target.width() - margin - smallW, margin, smallW, smallH);

    QColor panel(0,0,0,160);
    p.fillRect(smallRect.adjusted(-2,-2,2,2), panel);
    p.setPen(QPen(Qt::white, 2));
    p.drawRect(smallRect);
    drawFit(smallImg, smallRect);

    p.end();
    return bg;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    // 顶层采用左右布局：左侧视频会议，右侧聊天
    QWidget* w = new QWidget(this);
    auto* main = new QHBoxLayout(w);
    main->setContentsMargins(6,6,6,6);
    main->setSpacing(8);

    // 左侧（原有垂直内容）
    auto* left = new QVBoxLayout;
    left->setContentsMargins(0,0,0,0);
    left->setSpacing(6);

    // 连接栏
    QHBoxLayout *row1 = new QHBoxLayout;
    edHost = new QLineEdit("127.0.0.1");
    edPort = new QLineEdit("9000"); edPort->setMaximumWidth(80);
    QPushButton *btnConn = new QPushButton("连接");
    row1->addWidget(new QLabel("Host:")); row1->addWidget(edHost);
    row1->addWidget(new QLabel("Port:")); row1->addWidget(edPort);
    row1->addWidget(btnConn);
    left->addLayout(row1);

    // 加入/退出
    QHBoxLayout *row2 = new QHBoxLayout;
    edUser = new QLineEdit("user-A");
    edRoom = new QLineEdit("Room1");
    QPushButton *btnJoin = new QPushButton("加入房间");
    btnLeave_ = new QPushButton("退出房间");
    btnLeave_->setEnabled(false);
    row2->addWidget(new QLabel("User:"));  row2->addWidget(edUser);
    row2->addWidget(new QLabel("Room:"));  row2->addWidget(edRoom);
    row2->addWidget(btnJoin);
    row2->addWidget(btnLeave_);
    left->addLayout(row2);

    // 中央栈：Grid + Focus
    centerStack_ = new QStackedWidget(w);

    gridPage_ = new QWidget(centerStack_);
    gridLayout_ = new QGridLayout(gridPage_);
    gridLayout_->setContentsMargins(4,4,4,4);
    gridLayout_->setSpacing(6);
    centerStack_->addWidget(gridPage_);

    focusPage_ = new QWidget(centerStack_);
    auto* focusHLay = new QHBoxLayout(focusPage_);
    focusHLay->setContentsMargins(0,0,0,0);
    focusHLay->setSpacing(6);

    mainArea_ = new QWidget(focusPage_);
    auto* mainLay = new QVBoxLayout(mainArea_);
    mainLay->setContentsMargins(2,2,2,2);
    mainLay->setSpacing(4);

    mainVideo_ = new QLabel(QStringLiteral("点击右侧任意画面设为主画面"), mainArea_);
    mainVideo_->setAlignment(Qt::AlignCenter);
    mainVideo_->setStyleSheet("border:1px solid #444; background:#111; color:#ccc;");
    mainVideo_->setMinimumSize(400, 300);
    mainVideo_->setScaledContents(false);
    mainVideo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    mainVideo_->setCursor(Qt::PointingHandCursor);
    mainVideo_->installEventFilter(this);

    mainName_ = new QLabel("", mainArea_);
    mainName_->setAlignment(Qt::AlignCenter);
    mainName_->setStyleSheet("font-weight:bold;");

    mainLay->addWidget(mainVideo_, 1);
    mainLay->addWidget(mainName_, 0);

    auto* scroll = new QScrollArea(focusPage_);
    scroll->setWidgetResizable(true);
    focusThumbContainer_ = new QWidget(scroll);
    focusThumbLayout_ = new QGridLayout(focusThumbContainer_);
    focusThumbLayout_->setContentsMargins(4,4,4,4);
    focusThumbLayout_->setSpacing(6);
    scroll->setWidget(focusThumbContainer_);

    focusHLay->addWidget(mainArea_, /*stretch*/3);
    focusHLay->addWidget(scroll,    /*stretch*/1);

    centerStack_->addWidget(focusPage_);

    left->addWidget(centerStack_, 1);

    // 底部按钮 + 共享画质
    btnCamera_ = new QPushButton("开启摄像头");
    btnMic_    = new QPushButton("开启麦克风");
    btnShare_  = new QPushButton("开启共享屏幕");

    cbShareQ_  = new QComboBox(this);
    cbShareQ_->addItem(QStringLiteral("流畅 (848x480 @8fps q50)"));
    cbShareQ_->addItem(QStringLiteral("平衡 (960x540 @10fps q55)"));
    cbShareQ_->addItem(QStringLiteral("清晰 (1280x720 @8fps q60)"));
    cbShareQ_->addItem(QStringLiteral("高清 (1600x900 @8fps q55)"));
    cbShareQ_->setCurrentIndex(1);

    auto* rowBtn = new QHBoxLayout;
    rowBtn->addWidget(btnCamera_);
    rowBtn->addWidget(btnMic_);
    rowBtn->addWidget(btnShare_);
    rowBtn->addSpacing(12);
    rowBtn->addWidget(new QLabel(QStringLiteral("共享画质:")));
    rowBtn->addWidget(cbShareQ_);
    // [KB] 新增“企业知识库”按钮
    QPushButton* btnKb = new QPushButton(QStringLiteral("企业知识库"));
    rowBtn->addWidget(btnKb);
    rowBtn->addStretch(1);
    left->addLayout(rowBtn);

    // 标注工具栏
    auto* annotRow = new QHBoxLayout;
    btnAnnotOn_ = new QToolButton(this);
    btnAnnotOn_->setText(QStringLiteral("开始标注"));
    btnAnnotOn_->setCheckable(true);

    cbAnnotTool_ = new QComboBox(this);
    cbAnnotTool_->addItem(QStringLiteral("画笔"),   int(AnnotModel::Pen));
    cbAnnotTool_->addItem(QStringLiteral("矩形"),   int(AnnotModel::Rect));
    cbAnnotTool_->addItem(QStringLiteral("椭圆"),   int(AnnotModel::Ellipse));
    cbAnnotTool_->addItem(QStringLiteral("箭头"),   int(AnnotModel::Arrow));
    cbAnnotTool_->setCurrentIndex(0);

    cbAnnotWidth_ = new QComboBox(this);
    cbAnnotWidth_->addItems(QStringList() << "2" << "3" << "4" << "6" << "8");
    cbAnnotWidth_->setCurrentText("3");

    btnAnnotColor_ = new QToolButton(this);
    btnAnnotColor_->setText(QStringLiteral("颜色"));

    QToolButton* btnAnnotUndo = new QToolButton(this);
    btnAnnotUndo->setText(QStringLiteral("撤销上一步"));

    btnAnnotClear_ = new QToolButton(this);
    btnAnnotClear_->setText(QStringLiteral("清空此画面"));

    annotRow->addWidget(new QLabel(QStringLiteral("标注:")));
    annotRow->addWidget(btnAnnotOn_);
    annotRow->addSpacing(8);
    annotRow->addWidget(new QLabel(QStringLiteral("工具:")));
    annotRow->addWidget(cbAnnotTool_);
    annotRow->addWidget(new QLabel(QStringLiteral("线宽:")));
    annotRow->addWidget(cbAnnotWidth_);
    annotRow->addWidget(btnAnnotColor_);
    annotRow->addWidget(btnAnnotUndo);
    annotRow->addWidget(btnAnnotClear_);
    annotRow->addStretch(1);
    left->addLayout(annotRow);

    // 右侧聊天栏
    auto* right = new QVBoxLayout;
    right->setContentsMargins(0,0,0,0);
    right->setSpacing(6);

    QLabel* chatTitle = new QLabel(QStringLiteral("聊天"));
    chatTitle->setStyleSheet("font-weight:bold; padding:2px;");
    right->addWidget(chatTitle, 0);

    sChatList = new QListWidget(this);
    sChatList->setSelectionMode(QAbstractItemView::NoSelection);
    sChatList->setFocusPolicy(Qt::NoFocus);
    // 按主题启用右侧渐变背景
    {
        const QString theme = detectThemeFromTopLevel(this);
        sChatList->setStyleSheet(chatListQssForTheme(theme));
    }
    right->addWidget(sChatList, 1);

    // 发送栏（文本/图片/文件）
    QHBoxLayout *row3 = new QHBoxLayout;
    edInput = new QLineEdit;
    QPushButton *btnSend = new QPushButton("发送文本");
    QPushButton *btnSendImg = new QPushButton("发送图片");
    QPushButton *btnSendFile = new QPushButton("发送文件");
    row3->addWidget(edInput, 1);
    row3->addWidget(btnSend);
    row3->addWidget(btnSendImg);
    row3->addWidget(btnSendFile);
    right->addLayout(row3);

    // 拼装左右
    auto* leftWrap = new QWidget(w); leftWrap->setLayout(left);
    auto* rightWrap = new QWidget(w); rightWrap->setLayout(right);
    rightWrap->setMinimumWidth(300);
    rightWrap->setMaximumWidth(420);

    main->addWidget(leftWrap, 1);
    main->addWidget(rightWrap, 0);
    setCentralWidget(w);
    setWindowTitle("Multi-Party Video Client");

    // 主题主色系（用于按钮美化）
    const QString theme = detectThemeFromTopLevel(this);
    QString primary = "#6b7280", primaryDark = "#4b5563", primaryHover = "#7b8391";
    if (theme == "expert")  { primary = "#1976d2"; primaryDark = "#115293"; primaryHover = "#1e88e5"; }
    if (theme == "factory") { primary = "#2e7d32"; primaryDark = "#1b5e20"; primaryHover = "#388e3c"; }
    auto stylePrimaryBtn = [&](QPushButton* b){
        if (!b) return;
        b->setStyleSheet(QString(
            "QPushButton{background:%1;color:#fff;border:1px solid %2;border-radius:10px;padding:6px 12px;}"
            "QPushButton:hover{background:%3;}"
            "QPushButton:disabled{background:#cbd5e1;color:#6b7280;border-color:#cbd5e1;}"
        ).arg(primary, primaryDark, primaryHover));
    };
    // 应用到主要操作按钮
    stylePrimaryBtn(btnCamera_);
    stylePrimaryBtn(btnMic_);
    stylePrimaryBtn(btnShare_);
    stylePrimaryBtn(btnConn);
    stylePrimaryBtn(btnJoin);
    stylePrimaryBtn(btnSend);
    stylePrimaryBtn(btnSendImg);
    stylePrimaryBtn(btnSendFile);

    // 危险操作按钮：退出房间（红色）
    if (btnLeave_) {
        btnLeave_->setStyleSheet(
            "QPushButton{background:#d32f2f;color:#fff;border:1px solid #b71c1c;border-radius:10px;padding:6px 12px;}"
            "QPushButton:hover{background:#e53935;}"
            "QPushButton:disabled{background:#f8d7da;color:#b85c5c;border-color:#f1b0b7;}"
        );
    }

    // “开始标注”切换按钮（选中高亮为主题色）
    if (btnAnnotOn_) {
        btnAnnotOn_->setStyleSheet(QString(
            "QToolButton{background:#ffffff;border:1px solid #d1d5db;border-radius:10px;padding:6px 10px;}"
            "QToolButton:hover{background:#f8fafc;}"
            "QToolButton:checked{background:%1;color:#ffffff;border:1px solid %2;}"
        ).arg(primary, primaryDark));
    }

    // 本地 tile
    localTile_ = *makeTile(gridPage_, QStringLiteral("我（本地预览）"));
    localTile_.key = kLocalKey_;
    localTile_.volBtn->setText(QStringLiteral("麦克风"));
    localTile_.volBtn->setToolTip(QStringLiteral("调节我的麦克风音量"));
    localTile_.video->installEventFilter(this);
    localTile_.name->installEventFilter(this);

    // 媒体
    audio_ = new AudioChat(&conn_, this);
    connect(&conn_, &ClientConn::packetArrived, audio_, &AudioChat::onPacket);

    udp_ = new UdpMediaClient(this);

    share_ = new ScreenShare(&conn_, this);
    share_->setUdpClient(udp_);
    connect(share_, &ScreenShare::localFrameReady, this, &MainWindow::onLocalScreenFrame);

    // 绑定

    connect(btnConn,   &QPushButton::clicked, this, &MainWindow::onConnect);
    connect(btnJoin,   &QPushButton::clicked, this, &MainWindow::onJoin);
    connect(btnLeave_, &QPushButton::clicked, this, &MainWindow::onLeave);
    connect(btnSend,   &QPushButton::clicked, this, &MainWindow::onSendText);
    connect(btnSendImg,&QPushButton::clicked, this, &MainWindow::onSendImage);
    connect(btnSendFile,&QPushButton::clicked, this, &MainWindow::onSendFile);
    connect(btnCamera_,&QPushButton::clicked, this, &MainWindow::onToggleCamera);
    connect(btnShare_, &QPushButton::clicked, this, &MainWindow::onToggleShare);
    connect(btnKb, &QPushButton::clicked, this, &MainWindow::onOpenKnowledge);  // [KB]
    connect(&conn_,    &ClientConn::packetArrived, this, &MainWindow::onPkt);
    connect(&conn_,    &ClientConn::disconnected, this, [this]{
        btnLeave_->setEnabled(false);
        // 用户不需要日志，这里不输出
    });

    connect(btnMic_, &QPushButton::clicked, this, [this]{
        bool on = !audio_->isEnabled();
        audio_->setEnabled(on);
        btnMic_->setText(on ? "关闭麦克风" : "开启麦克风");
    });

    // 共享画质改变时，立即套用
    connect(cbShareQ_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int){ applyShareQualityPreset(); });

    bindVolumeButton(&localTile_, true);

    // UDP 收帧（整帧 JPEG 屏幕）
    connect(udp_, &UdpMediaClient::udpScreenFrame, this,
        [this](const QString& sender, const QByteArray& jpeg, int /*w*/, int /*h*/, qint64){
            if (sender.isEmpty() || sender == edUser->text()) return;
            VideoTile* t = ensureRemoteTile(sender);
            QBuffer buf(const_cast<QByteArray*>(&jpeg));
            buf.open(QIODevice::ReadOnly);
            QImageReader reader(&buf, "jpeg");
            reader.setAutoTransform(true);
            QImage img = reader.read().convertToFormat(QImage::Format_RGB32);
            if (!img.isNull()) {
                screenBack_[sender] = img; // 同步背板
                t->lastScreen = img;
                kickRemoteAlive(t);
                refreshTilePixmap(t);
                if (mainKey_ == sender) updateMainFromTile(t);
            }
        });

    // UDP 收帧（增量 DELTA 屏幕）
    connect(udp_, &UdpMediaClient::udpScreenDeltaFrame, this,
        [this](const QString& sender, const QByteArray& blob, int w, int h, qint64){
            if (sender.isEmpty() || sender == edUser->text()) return;
            VideoTile* t = ensureRemoteTile(sender);

            // 准备/校正背板尺寸
            QImage& back = screenBack_[sender];
            if (back.isNull() || back.size() != QSize(w, h)) {
                back = QImage(w, h, QImage::Format_RGB32);
                back.fill(Qt::black);
            }

            // 解析 DS01
            QDataStream ds(blob);
            ds.setByteOrder(QDataStream::BigEndian);
            quint32 magic=0; quint16 rectCount=0;
            ds >> magic >> rectCount;
            if (magic != 0x44533031) return; // 'DS01'

            for (int i = 0; i < rectCount; ++i) {
                quint16 x=0,y=0,rw=0,rh=0; quint32 clen=0;
                ds >> x >> y >> rw >> rh >> clen;
                if (ds.status()!=QDataStream::Ok) return;
                if (int(ds.device()->bytesAvailable()) < int(clen)) return;
                QByteArray comp; comp.resize(int(clen));
                ds.readRawData(comp.data(), clen);
                QByteArray raw = qUncompress(comp);
                if (raw.size() != int(rw) * int(rh) * 4) continue;

                // 写回到背板
                const char* src = raw.constData();
                for (int row = 0; row < rh; ++row) {
                    uchar* dst = back.scanLine(y + row) + x * 4;
                    memcpy(dst, src + row * rw * 4, rw * 4);
                }
            }

            // 显示更新
            t->lastScreen = back;
            kickRemoteAlive(t);
            refreshTilePixmap(t);
            if (mainKey_ == sender) updateMainFromTile(t);
        });

    lastSend_.start();

    // 初始共享画质参数
    applyShareQualityPreset();

    // 标注画布叠加到主画面标签上
    annotCanvas_ = new AnnotCanvas(mainVideo_);
    annotCanvas_->setGeometry(mainVideo_->rect());
    annotCanvas_->hide(); // 初始未选主画面时不显示

    // 标注工具绑定
    connect(btnAnnotOn_, &QToolButton::toggled, this, [this](bool on){
        annotCanvas_->setEnabledDrawing(on && !mainKey_.isEmpty());
        btnAnnotOn_->setText(on ? QStringLiteral("结束标注") : QStringLiteral("开始标注"));
        annotCanvas_->setVisible(!mainKey_.isEmpty());
    });
    connect(cbAnnotTool_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int){
        auto tool = AnnotModel::Tool(cbAnnotTool_->currentData().toInt());
        annotCanvas_->setTool(tool);
    });
    connect(cbAnnotWidth_, &QComboBox::currentTextChanged, this, [this](const QString& s){
        annotCanvas_->setWidth(s.toInt());
    });
    connect(btnAnnotColor_, &QToolButton::clicked, this, [this](){
        QColor c = QColorDialog::getColor(annotColor_, this, QStringLiteral("选择颜色"));
        if (c.isValid()) { annotColor_ = c; annotCanvas_->setColor(c); }
    });

    // 撤销
    connect(btnAnnotUndo, &QToolButton::clicked, this, [this](){
        if (mainKey_.isEmpty()) return;
        AnnotModel* m = modelFor(mainKey_);
        if (!m) return;
        if (!m->undoLastByOwner(edUser->text())) {
            return;
        }
        // 本地刷新
        updateMainFitted();
        if (mainKey_ == kLocalKey_) refreshTilePixmap(&localTile_);
        else {
            auto it = remoteTiles_.find(mainKey_);
            if (it != remoteTiles_.end()) refreshTilePixmap(it.value());
        }
        // 广播撤销
        QJsonObject ev{
            {"roomId", edRoom->text()},
            {"sender", edUser->text()},
            {"target", mainKey_},
            {"op", "undo"},
            {"ts", QDateTime::currentMSecsSinceEpoch()}
        };
        conn_.send(MSG_ANNOT, ev);
    });

    connect(btnAnnotClear_, &QToolButton::clicked, this, [this](){
        if (mainKey_.isEmpty()) return;
        QJsonObject ev{
            {"roomId", edRoom->text()},
            {"sender", edUser->text()},
            {"target", mainKey_},
            {"op", "clear"},
            {"ts", QDateTime::currentMSecsSinceEpoch()}
        };
        if (auto* m = modelFor(mainKey_)) { m->clear(); }
        conn_.send(MSG_ANNOT, ev);
        updateMainFitted();
        if (auto it = remoteTiles_.find(mainKey_); it != remoteTiles_.end()) refreshTilePixmap(it.value());
        if (mainKey_ == kLocalKey_) refreshTilePixmap(&localTile_);
    });

    // 画布事件 -> 本地应用 + 网络广播
    connect(annotCanvas_, &AnnotCanvas::annotateEvent, this, [this](QJsonObject ev){
        if (mainKey_.isEmpty()) return;
        ev["roomId"] = edRoom->text();
        ev["sender"] = edUser->text();

        if (auto* m = modelFor(ev.value("target").toString())) {
            m->applyEvent(ev);
        }

        QJsonObject evNet = ev;
        if (evNet.value("target").toString() == kLocalKey_) {
            evNet["target"] = edUser->text();
        }
        conn_.send(MSG_ANNOT, evNet);

        // 刷新
        updateMainFitted();
        const QString target = ev.value("target").toString();
        if (target == kLocalKey_) {
            refreshTilePixmap(&localTile_);
        } else {
            auto it = remoteTiles_.find(target);
            if (it != remoteTiles_.end()) refreshTilePixmap(it.value());
        }
    });

    setMainKey(QString());
    refreshGridOnly();
}

/* ---------- 事件处理 ---------- */
bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonDblClick) {
        auto findByWidget = [this, watched]() -> VideoTile* {
            if (localTile_.video == watched) return &localTile_;
            for (auto* t : remoteTiles_) if (t->video == watched) return t;
            if (watched == mainVideo_) {
                if (mainKey_.isEmpty()) return nullptr;
                if (mainKey_ == kLocalKey_) return &localTile_;
                auto it = remoteTiles_.find(mainKey_);
                if (it != remoteTiles_.end()) return it.value();
            }
            return nullptr;
        };
        if (VideoTile* t = findByWidget()) {
            if (watched != mainVideo_ && currentMode() == ViewMode::Grid) {
                setMainKey(t->key);
            }
            togglePiP(t);
            return true;
        }
    }
    if (event->type() == QEvent::MouseButtonRelease) {
        if (watched == mainVideo_) {
            setMainKey(QString());
            return true;
        }
        auto findByWidget = [this, watched]() -> VideoTile* {
            if (localTile_.video == watched || localTile_.name == watched)
                return &localTile_;
            for (auto* t : remoteTiles_) {
                if (t->video == watched || t->name == watched)
                    return t;
            }
            return nullptr;
        };
        if (VideoTile* t = findByWidget()) {
            setMainKey(t->key);
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::resizeEvent(QResizeEvent* ev)
{
    QMainWindow::resizeEvent(ev);
    updateAllThumbFitted();
    updateMainFitted();
    if (annotCanvas_) annotCanvas_->setGeometry(mainVideo_->rect());
}

/* ---------- 网络 ---------- */
void MainWindow::onConnect()
{
    conn_.connectTo(edHost->text(), edPort->text().toUShort());

    // 配置 UDP：服务器端口 = TCP + 1
    bool ok=false; quint16 tcp = edPort->text().toUShort(&ok);
    quint16 udpPort = ok ? (tcp + 1) : 0;
    udp_->configureServer(edHost->text(), udpPort);
}

void MainWindow::onJoin()
{
    QJsonObject j{{"roomId", edRoom->text()}, {"user", edUser->text()}};
    conn_.send(MSG_JOIN_WORKORDER, j);
    localTile_.name->setText(QString("我（%1）").arg(edUser->text()));

    audio_->setIdentity(edRoom->text(), edUser->text());
    share_->setIdentity(edRoom->text(), edUser->text());
    udp_->setIdentity(edRoom->text(), edUser->text());

    btnLeave_->setEnabled(true);
    applyShareQualityPreset();
}

void MainWindow::onLeave()
{
    // 停止本地媒体
    if (share_ && share_->isEnabled()) {
        share_->setEnabled(false);
        btnShare_->setText("开启共享屏幕");
    }
    if (camera_) stopCamera();
    if (audio_ && audio_->isEnabled()) {
        audio_->setEnabled(false);
        btnMic_->setText("开启麦克风");
    }
    if (udp_) udp_->stop();

    // 清空远端 tile
    for (auto it = remoteTiles_.begin(); it != remoteTiles_.end(); ) {
        QString key = it.key();
        removeRemoteTile(key);
        it = remoteTiles_.begin();
    }
    screenBack_.clear();

    // 清空标注
    for (auto* m : annotModels_) delete m;
    annotModels_.clear();
    if (annotCanvas_) {
        annotCanvas_->setActiveModel(nullptr);
        annotCanvas_->setTargetKey(QString());
        annotCanvas_->hide();
        btnAnnotOn_->setChecked(false);
    }

    // 清空本地预览
    localTile_.lastCam = QImage();
    localTile_.lastScreen = QImage();
    refreshTilePixmap(&localTile_);

    // 主画面重置 & 回网格
    setMainKey(QString());
    refreshGridOnly();

    // 断开与服务器的连接（服务端会广播 leave）
    conn_.disconnectFromServer();

    btnLeave_->setEnabled(false);
}

/* ---------- 聊天发送 ---------- */
void MainWindow::onSendText()
{
    const QString text = edInput->text().trimmed();
    if (text.isEmpty()) return;

    // 自己的气泡
    chatAddText(edUser->text(), text, /*outgoing*/true);

    QJsonObject j{{"roomId",  edRoom->text()},
                  {"sender",  edUser->text()},
                  {"content", text},
                  {"ts",      QDateTime::currentMSecsSinceEpoch()}};
    conn_.send(MSG_TEXT, j);
    edInput->clear();
}

void MainWindow::onSendImage()
{
    QString path = QFileDialog::getOpenFileName(this, tr("选择图片"),
                                                QString(),
                                                tr("图片 (*.png *.jpg *.jpeg *.bmp *.webp);;所有文件 (*)"));
    if (path.isEmpty()) return;

    QImageReader r(path);
    r.setAutoTransform(true);
    QImage img = r.read();
    if (img.isNull()) return;

    // 发送端重编码为 JPEG（限大小）
    constexpr int kMaxBytes = int(kMaxPacketLen) - 1024;
    int quality = 85;
    QSize curSize = img.size();

    auto encode = [&](const QImage& in, int q)->QByteArray{
        QByteArray data;
        QBuffer buf(&data);
        buf.open(QIODevice::WriteOnly);
        QImageWriter w(&buf, "jpeg");
        w.setQuality(q);
        w.setOptimizedWrite(true);
        w.write(in);
        return data;
    };

    QByteArray payload = encode(img, quality);
    while (payload.size() > kMaxBytes && (curSize.width() > 640 || curSize.height() > 480)) {
        curSize = curSize * 0.85;
        quality = qMax(60, quality - 5);
        payload = encode(img.scaled(curSize, Qt::KeepAspectRatio, Qt::SmoothTransformation), quality);
    }
    if (payload.size() > kMaxBytes) return;

    const QString baseName = QFileInfo(path).completeBaseName() + ".jpg";
    QJsonObject j{
        {"roomId",   edRoom->text()},
        {"sender",   edUser->text()},
        {"kind",     "image"},
        {"filename", baseName},
        {"mime",     "image/jpeg"},
        {"size",     payload.size()},
        {"ts",       QDateTime::currentMSecsSinceEpoch()}
    };
    conn_.send(MSG_FILE, j, payload);

    // 自己的预览
    chatAddImage(edUser->text(), QImage::fromData(payload, "JPEG"), /*outgoing*/true);
}

void MainWindow::onSendFile()
{
    QString path = QFileDialog::getOpenFileName(this, tr("选择文件"), QString(), tr("所有文件 (*)"));
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    QByteArray data = f.readAll();
    f.close();

    constexpr int kMaxBytes = int(kMaxPacketLen) - 1024;
    if (data.size() > kMaxBytes) return;

    QMimeDatabase db;
    QMimeType mt = db.mimeTypeForFile(path, QMimeDatabase::MatchContent);
    const QString mime = mt.isValid() ? mt.name() : "application/octet-stream";
    const QString baseName = QFileInfo(path).fileName();

    QJsonObject j{
        {"roomId",   edRoom->text()},
        {"sender",   edUser->text()},
        {"kind",     "file"},
        {"filename", baseName},
        {"mime",     mime},
        {"size",     data.size()},
        {"ts",       QDateTime::currentMSecsSinceEpoch()}
    };
    conn_.send(MSG_FILE, j, data);

    // 自己的预览
    chatAddFile(edUser->text(), baseName, mime, data, /*outgoing*/true);
}

/* ---------- 自适应/协议处理 ---------- */
void MainWindow::applyAdaptiveByMembers(int members)
{
    if (members <= 2) { sendSize_ = QSize(640,480); targetFps_ = 12; jpegQuality_ = 60; }
    else if (members <= 4) { sendSize_ = QSize(480,360); targetFps_ = 10; jpegQuality_ = 55; }
    else { sendSize_ = QSize(320,240); targetFps_ = 8;  jpegQuality_ = 50; }

    if (camera_) configureCamera(camera_);
    applyShareQualityPreset();
}

void MainWindow::onPkt(Packet p)
{
    switch (p.type)
    {
    case MSG_TEXT:
    {
        const QString room = p.json["roomId"].toString();
        if (room != edRoom->text()) break;
        const QString sender = p.json["sender"].toString();
        const QString content = p.json["content"].toString();
        if (sender.isEmpty() || content.isEmpty()) break;
        if (sender == edUser->text()) break; // 自己已本地显示
        chatAddText(sender, content, /*outgoing*/false);
        break;
    }

    case MSG_DEVICE_CONTROL:
    {
        const QString room = p.json.value("roomId").toString();
        if (room != edRoom->text()) break;
        const QString device = p.json.value("device").toString();
        const QString command= p.json.value("command").toString();
        const QString sender = p.json.value("sender").toString();
        const qint64 ts      = p.json.value("ts").toVariant().toLongLong();
        if (!device.isEmpty() && !command.isEmpty())
            emit deviceControlMessage(device, command, sender, ts);
        break;
    }

    case MSG_VIDEO_FRAME:
    {
        const QString sender = p.json.value("sender").toString();
        if (sender.isEmpty() || sender == edUser->text()) break;

        VideoTile* t = ensureRemoteTile(sender);

        QBuffer buf(const_cast<QByteArray*>(&p.bin));
        buf.open(QIODevice::ReadOnly);
        QImageReader reader(&buf);
        reader.setAutoTransform(true);
        QImage img = reader.read();

        const QString media = p.json.value("media").toString("camera");
        if (!img.isNull()) {
            if (media == "screen") t->lastScreen = img;
            else                    t->lastCam    = img;
            kickRemoteAlive(t);
            refreshTilePixmap(t);
            if (mainKey_ == sender) updateMainFromTile(t);
        }
        break;
    }

    case MSG_CONTROL:
    {
        const QString kind  = p.json.value("kind").toString();
        const QString state = p.json.value("state").toString();
        const QString sender = p.json.value("sender").toString();
        if (!sender.isEmpty() && sender != edUser->text()) {
            VideoTile* t = ensureRemoteTile(sender);
            if (kind == "视频" || kind == "video") {
                if (state == "off") t->lastCam = QImage();
                refreshTilePixmap(t);
                if (mainKey_ == sender) updateMainFromTile(t);
            } else if (kind == "screen") {
                if (state == "off") t->lastScreen = QImage();
                refreshTilePixmap(t);
                if (mainKey_ == sender) updateMainFromTile(t);
            }
        }
        break;
    }

    case MSG_SERVER_EVENT:
    {
        const QString kind = p.json.value("kind").toString();
        if (kind == "room") {
            QStringList members;
            for (auto v : p.json.value("members").toArray())
                members << v.toString();

            QSet<QString> shouldHave = QSet<QString>::fromList(members);
            shouldHave.remove(edUser->text());

            for (const QString& u : shouldHave) {
                VideoTile* t = ensureRemoteTile(u);
                if (t && t->lastCam.isNull() && t->lastScreen.isNull()) {
                    setTileWaiting(t, QStringLiteral("等待视频/屏幕..."));
                }
            }

            for (auto it = remoteTiles_.begin(); it != remoteTiles_.end(); ) {
                if (!shouldHave.contains(it.key())) {
                    if (mainKey_ == it.key()) setMainKey(QString());
                    removeRemoteTile(it.key());
                    it = remoteTiles_.begin();
                } else {
                    ++it;
                }
            }

            applyAdaptiveByMembers(members.size());

            if (currentMode() == ViewMode::Grid) refreshGridOnly();
            else refreshFocusThumbs();
        }
        break;
    }

    case MSG_ANNOT:
    {
        if (p.json.value("roomId").toString() != edRoom->text()) break;

        const QString rawTarget = p.json.value("target").toString();
        QString target = rawTarget;
        if (target == kLocalKey_) target = p.json.value("sender").toString();
        if (target.isEmpty()) break;

        if (auto* m = modelFor(target)) {
            if (m->applyEvent(p.json)) {
                if (mainKey_ == target) {
                    updateMainFitted();
                }
                if (target == kLocalKey_) {
                    refreshTilePixmap(&localTile_);
                } else {
                    auto it = remoteTiles_.find(target);
                    if (it != remoteTiles_.end()) refreshTilePixmap(it.value());
                }
            }
        }
        break;
    }

    case MSG_FILE:
    {
        if (p.json.value("roomId").toString() != edRoom->text()) break;

        const QString sender = p.json.value("sender").toString();
        if (sender.isEmpty()) break;

        const QString kind = p.json.value("kind").toString("file");
        QString filename = p.json.value("filename").toString();
        const QString mime = p.json.value("mime").toString();
        // 自己发送的已本地显示
        if (sender == edUser->text()) break;

        if (kind == "image") {
            QImage img = QImage::fromData(p.bin, "JPEG");
            if (!img.isNull()) chatAddImage(sender, img, /*outgoing*/false);
        } else {
            if (filename.isEmpty()) {
                filename = QString("%1_%2.bin").arg(sender).arg(QDateTime::currentMSecsSinceEpoch());
            }
            chatAddFile(sender, filename, mime, p.bin, /*outgoing*/false);
        }
        break;
    }

    default:
        break;
    }
}

/* ---------- 摄像头 ---------- */
void MainWindow::startCamera()
{
    if (camera_) return;

    const QList<QCameraInfo> cameras = QCameraInfo::availableCameras();
    if (cameras.isEmpty()) {
        return;
    }
    camera_ = new QCamera(cameras.first(), this);
    configureCamera(camera_);

    probe_ = new QVideoProbe(this);
    if (probe_->setSource(camera_)) {
        connect(probe_, &QVideoProbe::videoFrameProbed, this, &MainWindow::onVideoFrame);
    }

    camera_->start();

    btnCamera_->setText("关闭摄像头");

    QJsonObject j{{"roomId", edRoom->text()},
                  {"sender", edUser->text()},
                  {"kind", "video"},
                  {"state","on"},
                  {"ts", QDateTime::currentMSecsSinceEpoch()}};
    conn_.send(MSG_CONTROL, j);
}

void MainWindow::stopCamera()
{
    if (!camera_) return;

    camera_->stop();

    if (probe_) {
        probe_->setSource(static_cast<QMediaObject*>(nullptr));
        disconnect(probe_, &QVideoProbe::videoFrameProbed, this, &MainWindow::onVideoFrame);
        probe_->deleteLater();
        probe_ = nullptr;
    }

    camera_->deleteLater();
    camera_ = nullptr;

    localTile_.lastCam = QImage();
    refreshTilePixmap(&localTile_);
    if (mainKey_ == kLocalKey_) updateMainFromTile(&localTile_);

    btnCamera_->setText("开启摄像头");

    QJsonObject j{{"roomId", edRoom->text()},
                  {"sender", edUser->text()},
                  {"kind", "video"},
                  {"state","off"},
                  {"ts", QDateTime::currentMSecsSinceEpoch()}};
    conn_.send(MSG_CONTROL, j);

    if (mainKey_ == kLocalKey_ && localTile_.lastScreen.isNull()) setMainKey(QString());
}

void MainWindow::onToggleCamera()
{
    if (camera_) stopCamera();
    else         startCamera();
}

void MainWindow::onToggleShare()
{
    bool on = !share_->isEnabled();
    // 开关前确保套用当前预设
    applyShareQualityPreset();

    share_->setEnabled(on);
    btnShare_->setText(on ? "关闭共享屏幕" : "开启共享屏幕");

    if (!on) {
        localTile_.lastScreen = QImage();
        refreshTilePixmap(&localTile_);
        if (mainKey_ == kLocalKey_) updateMainFromTile(&localTile_);
        if (!camera_ && mainKey_ == kLocalKey_) setMainKey(QString());
    }
}

void MainWindow::configureCamera(QCamera* cam)
{
    QSize desiredRes = sendSize_;
    QList<QSize> resList = cam->supportedViewfinderResolutions();
    if (!resList.isEmpty()) {
        if (!resList.contains(desiredRes)) {
            if (resList.contains(QSize(640,480))) desiredRes = QSize(640,480);
            else if (resList.contains(QSize(480,360))) desiredRes = QSize(480,360);
            else if (resList.contains(QSize(320,240))) desiredRes = QSize(320,240);
            else desiredRes = resList.first();
        }
    } else {
        desiredRes = QSize();
    }

    QList<QVideoFrame::PixelFormat> fmts =
        cam->supportedViewfinderPixelFormats(QCameraViewfinderSettings());

    auto prefer = QList<QVideoFrame::PixelFormat>{
        QVideoFrame::Format_ARGB32,
        QVideoFrame::Format_ARGB32_Premultiplied,
        QVideoFrame::Format_RGB32,
        QVideoFrame::Format_RGB24,
        QVideoFrame::Format_BGR32,
        QVideoFrame::Format_BGR24,
        QVideoFrame::Format_YUYV
    };

    QVideoFrame::PixelFormat chosenFmt = QVideoFrame::Format_Invalid;
    if (!fmts.isEmpty()) {
        for (auto f : prefer) {
            if (fmts.contains(f)) { chosenFmt = f; break; }
        }
        if (chosenFmt == QVideoFrame::Format_Invalid) chosenFmt = fmts.first();
    }

    QCameraViewfinderSettings vs;
    if (desiredRes.isValid()) vs.setResolution(desiredRes);
    if (chosenFmt != QVideoFrame::Format_Invalid) vs.setPixelFormat(chosenFmt);

    if (desiredRes.isValid() || chosenFmt != QVideoFrame::Format_Invalid) {
        cam->setViewfinderSettings(vs);
    }
}

/* ---------- 帧处理 ---------- */
QImage MainWindow::makeImageFromFrame(const QVideoFrame &frame)
{
    if (!frame.isValid()) return QImage();

    QVideoFrame clone(frame);
    if (!clone.map(QAbstractVideoBuffer::ReadOnly)) {
        return QImage();
    }

    const auto fmt = clone.pixelFormat();
    const QImage::Format imf = QVideoFrame::imageFormatFromPixelFormat(fmt);
    if (imf != QImage::Format_Invalid) {
        QImage img(clone.bits(), clone.width(), clone.height(), clone.bytesPerLine(), imf);
        QImage copy = img.copy();
        clone.unmap();
        return copy;
    }

    if (fmt == QVideoFrame::Format_YUYV) {
        const uchar *base = clone.bits();
        const int width = clone.width();
        const int height = clone.height();
        const int stride = clone.bytesPerLine();

        QImage out(width, height, QImage::Format_RGB32);
        auto clip = [](int v){ return v < 0 ? 0 : (v > 255 ? 255 : v); };

        for (int y = 0; y < height; ++y) {
            const uchar* line = base + y * stride;
            QRgb* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
            for (int x = 0; x < width; x += 2) {
                const int i = x << 1;
                int y0 = line[i + 0] - 16;
                int u  = line[i + 1] - 128;
                int y1 = line[i + 2] - 16;
                int v  = line[i + 3] - 128;

                int r0 = clip((298 * y0 + 409 * v + 128) >> 8);
                int g0 = clip((298 * y0 - 100 * u - 208 * v + 128) >> 8);
                int b0 = clip((298 * y0 + 516 * u + 128) >> 8);

                int r1 = clip((298 * y1 + 409 * v + 128) >> 8);
                int g1 = clip((298 * y1 - 100 * u - 208 * v + 128) >> 8);
                int b1 = clip((298 * y1 + 516 * u + 128) >> 8);

                dst[x]     = qRgb(r0, g0, b0);
                if (x + 1 < width)
                    dst[x + 1] = qRgb(r1, g1, b1);
            }
        }
        clone.unmap();
        return out;
    }

    clone.unmap();
    return QImage();
}

void MainWindow::updateLocalPreview(const QImage& img)
{
    if (img.isNull()) return;
    localTile_.lastCam = img;
    refreshTilePixmap(&localTile_);
    if (mainKey_ == kLocalKey_) updateMainFromTile(&localTile_);
}

void MainWindow::onLocalScreenFrame(QImage img)
{
    if (img.isNull()) return;
    localTile_.lastScreen = img;
    refreshTilePixmap(&localTile_);
    if (mainKey_ == kLocalKey_) updateMainFromTile(&localTile_);
}

void MainWindow::sendImage(const QImage& img)
{
    if (img.isNull()) return;

    const qint64 intervalMs = 1000 / qMax(1, targetFps_);
    if (lastSend_.isValid() && lastSend_.elapsed() < intervalMs)
        return;
    lastSend_.restart();

    const QImage scaled = img.scaled(sendSize_, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "jpeg");
    writer.setQuality(jpegQuality_);
    writer.setOptimizedWrite(true);
    if (!writer.write(scaled)) {
        return;
    }
    buffer.close();

    QJsonObject j{{"roomId", edRoom->text()},
                  {"sender", edUser->text()},
                  {"media",  "camera"},
                  {"w", scaled.width()},
                  {"h", scaled.height()},
                  {"ts", QDateTime::currentMSecsSinceEpoch()}};
    conn_.send(MSG_VIDEO_FRAME, j, jpeg);
}

void MainWindow::onVideoFrame(const QVideoFrame &frame)
{
    if (!camera_ || !frame.isValid()) return;

    QImage img = makeImageFromFrame(frame);
    if (img.isNull()) return;

    updateLocalPreview(img);
    sendImage(img);
}

/* ---------- 视图/缩略图 ---------- */
MainWindow::ViewMode MainWindow::currentMode() const
{
    return (centerStack_->currentWidget() == gridPage_) ? ViewMode::Grid : ViewMode::Focus;
}

VideoTile* MainWindow::ensureRemoteTile(const QString& sender)
{
    auto it = remoteTiles_.find(sender);
    if (it != remoteTiles_.end()) return it.value();

    VideoTile* t = makeTile(gridPage_, sender);
    t->key = sender;
    t->video->installEventFilter(this);
    t->name->installEventFilter(this);

    connect(t->timer, &QTimer::timeout, this, [this, t](){
        setTileWaiting(t);
        if (mainKey_ == t->key) {
            mainVideo_->clear();
            mainVideo_->setText(QStringLiteral("等待视频/屏幕..."));
        }
    });

    remoteTiles_.insert(sender, t);
    bindVolumeButton(t, false);

    if (currentMode() == ViewMode::Grid) refreshGridOnly();
    else refreshFocusThumbs();

    return t;
}

void MainWindow::removeRemoteTile(const QString& sender)
{
    auto it = remoteTiles_.find(sender);
    if (it == remoteTiles_.end()) return;
    VideoTile* t = it.value();
    if (t->box->parent()) {
        if (currentMode() == ViewMode::Grid) gridLayout_->removeWidget(t->box);
        else focusThumbLayout_->removeWidget(t->box);
    }
    t->box->deleteLater();
    remoteTiles_.erase(it);

    if (audio_) audio_->dropPeer(sender);

    if (currentMode() == ViewMode::Grid) refreshGridOnly();
    else refreshFocusThumbs();
}

void MainWindow::refreshGridOnly()
{
    while (QLayoutItem* item = gridLayout_->takeAt(0)) {
        if (item->widget()) item->widget()->setParent(gridPage_);
        delete item;
    }

    QList<QWidget*> widgets;
    localTile_.box->setParent(gridPage_);
    localTile_.box->show();
    widgets << localTile_.box;

    for (auto* t : remoteTiles_) {
        t->box->setParent(gridPage_);
        t->box->show();
        widgets << t->box;
    }

    int n = widgets.size();
    int cols = qMax(1, static_cast<int>(ceil(sqrt(static_cast<double>(n)))));
    int rows = qMax(1, static_cast<int>(ceil(n / static_cast<double>(cols))));
    int idx = 0;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            if (idx >= n) break;
            gridLayout_->addWidget(widgets[idx++], r, c);
        }
    }
    for (int r = 0; r < rows; ++r) gridLayout_->setRowStretch(r, 1);
    for (int c = 0; c < cols; ++c) gridLayout_->setColumnStretch(c, 1);

    centerStack_->setCurrentWidget(gridPage_);
    updateAllThumbFitted();
}

void MainWindow::refreshFocusThumbs()
{
    while (QLayoutItem* item = focusThumbLayout_->takeAt(0)) {
        if (item->widget()) item->widget()->setParent(focusThumbContainer_);
        delete item;
    }

    QList<QWidget*> thumbs;

    auto handleTile = [&](VideoTile* t) {
        if (!t) return;
        t->box->setParent(focusThumbContainer_);
        if (t->key == mainKey_) {
            t->box->hide();
        } else {
            t->box->show();
            thumbs << t->box;
        }
    };

    handleTile(&localTile_);
    for (auto* t : remoteTiles_) handleTile(t);

    int n = thumbs.size();
    int rows = n;
    int idx = 0;
    for (int r = 0; r < rows; ++r) {
        if (idx >= n) break;
        focusThumbLayout_->addWidget(thumbs[idx++], r, 0);
    }
    for (int r = 0; r < rows; ++r) focusThumbLayout_->setRowStretch(r, 1);
    focusThumbLayout_->setColumnStretch(0, 1);

    centerStack_->setCurrentWidget(focusPage_);
    updateAllThumbFitted();
    updateMainFitted();
}

void MainWindow::setTileWaiting(VideoTile* t, const QString& text)
{
    t->timer->stop();
    t->lastCam = QImage();
    t->lastScreen = QImage();
    t->video->clear();
    t->video->setText(text);
}

void MainWindow::kickRemoteAlive(VideoTile* t)
{
    t->timer->start(2500);
}

void MainWindow::setMainKey(const QString& key)
{
    mainKey_ = key;

    if (mainKey_.isEmpty()) {
        mainVideo_->clear();
        mainVideo_->setText(QStringLiteral("点击右侧任意画面设为主画面"));
        mainName_->setText(QString());
        annotCanvas_->setTargetKey(mainKey_);
        annotCanvas_->setActiveModel(nullptr);
        annotCanvas_->setEnabledDrawing(false);
        annotCanvas_->hide(); // 无主画面时隐藏画布
        refreshGridOnly();
        return;
    }

    VideoTile* t = nullptr;
    if (mainKey_ == kLocalKey_) t = &localTile_;
    else {
        auto it = remoteTiles_.find(mainKey_);
        if (it != remoteTiles_.end()) t = it.value();
    }

    if (!t) {
        mainKey_.clear();
        annotCanvas_->setTargetKey(mainKey_);
        annotCanvas_->setActiveModel(nullptr);
        annotCanvas_->setEnabledDrawing(false);
        annotCanvas_->hide();
        refreshGridOnly();
        return;
    }

    mainName_->setText(QString("主画面：%1").arg(t->name->text()));
    updateMainFromTile(t);
    refreshFocusThumbs();

    // 标注目标与模型
    annotCanvas_->setTargetKey(mainKey_);
    annotCanvas_->setActiveModel(modelFor(mainKey_));
    annotCanvas_->setGeometry(mainVideo_->rect());
    annotCanvas_->setVisible(true);
    annotCanvas_->setEnabledDrawing(btnAnnotOn_->isChecked() && !mainKey_.isEmpty());
}

void MainWindow::updateMainFromTile(VideoTile* t)
{
    if (!t) return;
    QImage composed = composeTileImage(t, mainVideo_->size());
    if (composed.isNull()) {
        mainVideo_->clear();
        mainVideo_->setText(QStringLiteral("等待视频/屏幕..."));
        return;
    }
    fitLabelImage(mainVideo_, composed);
    // 主画面上的标注由 annotCanvas_ 叠加绘制
}

void MainWindow::updateAllThumbFitted()
{
    refreshTilePixmap(&localTile_);
    for (auto* t : remoteTiles_) refreshTilePixmap(t);
}

void MainWindow::updateMainFitted()
{
    if (mainKey_.isEmpty()) return;
    VideoTile* t = nullptr;
    if (mainKey_ == kLocalKey_) t = &localTile_;
    else {
        auto it = remoteTiles_.find(mainKey_);
        if (it != remoteTiles_.end()) t = it.value();
    }
    if (t) updateMainFromTile(t);
    if (annotCanvas_) annotCanvas_->update(); // 确保标注在主画面重绘
}

void MainWindow::refreshTilePixmap(VideoTile* t)
{
    if (!t || !t->video) return;
    QImage composed = composeTileImage(t, t->video->size());
    if (composed.isNull()) {
        t->video->clear();
        t->video->setText(QStringLiteral("等待视频/屏幕..."));
        return;
    }

    // 缩略图上叠加标注（主画面由 AnnotCanvas 绘制叠加）
    const bool isMain = (centerStack_->currentWidget() == focusPage_ && mainKey_ == t->key);
    if (!isMain) {
        if (auto* m = annotModels_.value(t->key, nullptr)) {
            QPainter p(&composed);
            m->paint(p, composed.size());
            p.end();
        }
    }

    fitLabelImage(t->video, composed);
}

void MainWindow::togglePiP(VideoTile* t)
{
    if (!t) return;
    const bool hasCam    = !t->lastCam.isNull();
    const bool hasScreen = !t->lastScreen.isNull();
    if (!(hasCam && hasScreen)) return;
    t->camPrimary = !t->camPrimary;
    refreshTilePixmap(t);
    if (mainKey_ == t->key) updateMainFromTile(t);
}

/* ---------- 共享画质预设 ---------- */
void MainWindow::applyShareQualityPreset()
{
    if (!share_ || !cbShareQ_) return;

    // 预设表：分辨率(宽x高) / fps / JPEG q
    QSize sz; int fps = 10; int q = 55; QString name;
    switch (cbShareQ_->currentIndex()) {
    case 0: sz = QSize(848, 480);  fps = 8;  q = 50; name = "流畅"; break;
    default:
    case 1: sz = QSize(960, 540);  fps = 10; q = 55; name = "平衡"; break;
    case 2: sz = QSize(1280, 720); fps = 8;  q = 60; name = "清晰"; break;
    case 3: sz = QSize(1600, 900); fps = 8;  q = 55; name = "高清"; break;
    }
    share_->setParams(sz, fps, q);
}

/* ---------- 音量弹窗绑定 ---------- */
void MainWindow::bindVolumeButton(VideoTile* t, bool isLocal)
{
    if (!t || !t->volBtn) return;
    connect(t->volBtn, &QToolButton::clicked, this, [this, t, isLocal](){
        auto* popup = new VolumePopup(this);
        popup->setAttribute(Qt::WA_DeleteOnClose, true);

        int initial = 100;
        if (audio_) {
            if (isLocal) initial = int(audio_->micGain() * 100.0f + 0.5f);
            else         initial = int(audio_->peerGain(t->key) * 100.0f + 0.5f);
        }
        popup->setValue(initial);
        popup->openFor(t->volBtn);

        connect(popup, &VolumePopup::valueChanged, this, [this, t, isLocal](int percent){
            percent = qBound(0, percent, 200);
            t->volPercent = percent;
            if (!audio_) return;
            float g = percent / 100.0f;
            if (isLocal) audio_->setMicGain(g);
            else         audio_->setPeerGain(t->key, g);
            t->volBtn->setToolTip(QString("%1：%2%")
                                  .arg(isLocal ? QStringLiteral("我的麦克风") : QStringLiteral("此路音量"))
                                  .arg(percent));
        });
    });
}

/* ---------- 标注模型获取 ---------- */
AnnotModel* MainWindow::modelFor(const QString& key)
{
    if (key.isEmpty()) return nullptr;
    auto it = annotModels_.find(key);
    if (it != annotModels_.end()) return it.value();
    auto* m = new AnnotModel();
    annotModels_.insert(key, m);
    return m;
}

void MainWindow::onOpenKnowledge()
{
    // 优先：将知识库面板挂到当前窗口里的 QTabWidget（如果存在）
    bool attachedIntoTab = false;

    if (QTabWidget* tabs = this->findChild<QTabWidget*>()) {
        // 在 tabs 中创建或复用标题为“企业知识库”的页，并把 KnowledgePanel 放进去
        AttachKnowledgePanelToTab(this, QStringLiteral("企业知识库"));

        // 找到刚挂进去的 KnowledgePanel 实例
        KnowledgePanel* kbInTab = tabs->findChild<KnowledgePanel*>();
        if (kbInTab) {
            kbInTab->setServer(edHost->text().trimmed(), 5555);
            kbInTab->setRoomFilter(edRoom->text().trimmed());
            kbInTab->refresh();

            // 切换到“企业知识库”标签页
            for (int i = 0; i < tabs->count(); ++i) {
                if (tabs->tabText(i) == QStringLiteral("企业知识库")) {
                    tabs->setCurrentIndex(i);
                    break;
                }
            }
            attachedIntoTab = true;
        }
    }

    // 回退：当前窗口层级没有 QTabWidget，就使用独立窗口
    if (!attachedIntoTab) {
        if (!kbPanel_) {
            kbPanel_ = new KnowledgePanel();
            kbPanel_->setAttribute(Qt::WA_DeleteOnClose, true);
            kbPanel_->resize(900, 560);
            kbPanel_->setWindowTitle(QStringLiteral("企业知识库"));
        }
        kbPanel_->setServer(edHost->text().trimmed(), 5555);
        kbPanel_->setRoomFilter(edRoom->text().trimmed());
        kbPanel_->show();
        kbPanel_->raise();
        kbPanel_->activateWindow();
        kbPanel_->refresh();
    }
}

void MainWindow::setJoinedContext(const QString& user, const QString& roomId) {
    // 与 β 保持一致：设置并只读
    edUser->setText(user);
    edRoom->setText(roomId);
    edUser->setReadOnly(true);
    edRoom->setReadOnly(true);
}

void MainWindow::sendDeviceControlBroadcast(const QString& device,
                                            const QString& command,
                                            qint64 ts)
{
    if (ts < 0) ts = QDateTime::currentMSecsSinceEpoch();
    QJsonObject j{
        {"roomId", edRoom->text()},
        {"sender", edUser->text()},
        {"device", device},
        {"command", command},
        {"ts", ts}
    };
    conn_.send(MSG_DEVICE_CONTROL, j);
    // 本端立即回显（服务器通常不回发自己）
    emit deviceControlMessage(device, command, edUser->text(), ts);
}

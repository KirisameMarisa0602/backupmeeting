#pragma once
#include <QtWidgets>

class VolumePopup : public QFrame {
    Q_OBJECT
public:
    explicit VolumePopup(QWidget* parent = nullptr)
        : QFrame(parent)
    {
        setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setObjectName("VolumePopup");

        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(8,8,8,8);
        lay->setSpacing(6);

        label_ = new QLabel("100%", this);
        label_->setAlignment(Qt::AlignCenter);

        slider_ = new QSlider(Qt::Vertical, this);
        slider_->setRange(0, 200);      // 0% ~ 200%
        slider_->setValue(100);
        slider_->setTickPosition(QSlider::NoTicks);
        slider_->setFixedHeight(160);

        lay->addWidget(label_, 0);
        lay->addWidget(slider_, 1);

        setStyleSheet(
            "#VolumePopup { background: rgba(30,30,30,220); border:1px solid #555; border-radius:6px; }"
            "QLabel { color:#fff; }"
            "QSlider::groove:vertical { background:#444; width:6px; border-radius:3px; }"
            "QSlider::handle:vertical { background:#ddd; height:12px; margin: -2px; border-radius:6px; }"
        );

        connect(slider_, &QSlider::valueChanged, this, [this](int v){
            label_->setText(QString("%1%").arg(v));
            emit valueChanged(v);
        });
    }

    void setValue(int percent) {
        percent = qBound(0, percent, 200);
        slider_->setValue(percent);
        label_->setText(QString("%1%").arg(percent));
    }
    int value() const { return slider_->value(); }

    void openFor(QWidget* anchor) {
        if (!anchor) { show(); return; }
        QPoint g = anchor->mapToGlobal(QPoint(anchor->width()/2, 0));
        QSize  s = sizeHint();
        move(g.x() - s.width()/2, g.y() - s.height() - 8);
        show();
        raise();
        activateWindow();
    }

signals:
    void valueChanged(int percent);

private:
    QLabel*  label_{};
    QSlider* slider_{};
};
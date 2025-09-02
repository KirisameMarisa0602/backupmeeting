#pragma once
#include <QtWidgets>
#include "mainwindow.h"

// 通讯主模块封装为 QWidget 便于嵌入主界面
class CommWidget : public QWidget {
    Q_OBJECT
public:
    explicit CommWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        mw_ = new MainWindow(nullptr);
        QVBoxLayout *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0,0,0,0);
        lay->addWidget(mw_);
        setLayout(lay);
    }
    MainWindow* mainWindow() { return mw_; }
private:
    MainWindow* mw_;
};
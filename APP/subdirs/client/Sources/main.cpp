#include <QApplication>
#include <QFile>
#include "login.h"

static void loadTheme()
{
    QFile f(":/qss/theme.qss");
    if (f.open(QIODevice::ReadOnly)) {
        qApp->setStyleSheet(QString::fromUtf8(f.readAll()));
    }
}

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    a.setQuitOnLastWindowClosed(false);

    loadTheme();
    Login w;
    w.show();
    return a.exec();
}
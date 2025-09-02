#ifndef REGIST_H
#define REGIST_H

#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui { class Regist; }
QT_END_NAMESPACE

namespace Ui {
class Regist;
}

class Regist : public QWidget
{
    Q_OBJECT

public:
    explicit Regist(QWidget *parent = nullptr);
    ~Regist();

signals:
    void registered(const QString &username, const QString &role);

public slots:
    void preset(const QString &role, const QString &user, const QString &pass);

private slots:
    void on_btnRegister_clicked();
    void on_btnBack_clicked();

private:
    bool sendRequest(const QJsonObject &obj, QJsonObject &reply, QString *errMsg = nullptr);
    QString selectedRole() const; // "expert" | "factory" | ""

private:
    Ui::Regist *ui;
};

void openRegistDialog(QWidget *parent, const QString &prefRole,
                      const QString &prefUser, const QString &prefPass);


#endif // REGIST_H

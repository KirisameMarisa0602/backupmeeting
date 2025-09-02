#pragma once

#include <QWidget>
#include <QHostAddress>

class QLineEdit;
class QSpinBox;
class QPushButton;
class QTableWidget;

class KnowledgePanel : public QWidget
{
    Q_OBJECT
public:
    explicit KnowledgePanel(QWidget* parent = nullptr);

    // 兼容旧调用
    void setServer(const QHostAddress& host, quint16 port);
    void setServer(const QString& host, quint16 port);
    void setRoomFilter(const QString& room);

    // 便于调试
    QString hostText() const;
    quint16 portValue() const;
    QString roomFilter() const;

public slots:
    void refresh();

private:
    // UI
    QLineEdit*    hostEdit_{nullptr};
    QSpinBox*     portSpin_{nullptr};
    QLineEdit*    roomEdit_{nullptr};
    QPushButton*  refreshBtn_{nullptr};
    QTableWidget* table_{nullptr};

    // helpers
    void setBusy(bool on);
    void playFile(const QString& filePath) const;
    QString findFfplay() const;

    // 路径解析增强
    QString knowledgeRoot(bool interactive = true) const;          // 返回包含 knowledge 子目录的“server 根目录”
    QString resolveAbsolutePath(const QString& relOrAbs,
                                bool interactive = true) const;    // 把相对路径转绝对路径
    bool isValidKnowledgeRoot(const QString& root) const;          // root/knowledge 必须存在

    // events
    void onTableDoubleClicked(int row, int col);

    // cache
    mutable QString rootCache_;
};

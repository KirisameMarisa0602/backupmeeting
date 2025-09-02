#pragma once
#include <QtCore>
#include <QtGui>

class AnnotModel {
public:
    enum Tool { Pen, Rect, Ellipse, Arrow, Text };

    struct Stroke {
        QString id;
        QString owner;
        Tool tool{Pen};
        QColor color{Qt::red};
        int width{3};
        QVector<QPointF> pts;
        QString text;
        bool finished{false};
    };

    static Tool toolFromString(const QString& s);

    bool applyEvent(const QJsonObject& e);

    void paint(QPainter& p, const QSize& size) const;

    void clear();

    bool undoLastByOwner(const QString& owner);

    static inline QPointF denorm(const QPointF& n, const QSize& size) {
        return QPointF(n.x() * size.width(), n.y() * size.height());
    }

private:
    static void drawArrow(QPainter& p, const QPointF& a, const QPointF& b, int width, const QColor& color);

    QHash<QString, Stroke> strokes_;
    QStringList order_;
};
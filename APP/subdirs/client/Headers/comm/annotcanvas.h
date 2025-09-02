#pragma once
#include <QtWidgets>
#include "annot.h"

class AnnotCanvas : public QWidget {
    Q_OBJECT
public:
    explicit AnnotCanvas(QWidget* parent=nullptr);

    void setActiveModel(AnnotModel* m) { model_ = m; update(); }

    void setEnabledDrawing(bool on);
    bool isDrawingEnabled() const { return drawingEnabled_; }

    void setTool(AnnotModel::Tool t) { tool_ = t; }
    void setColor(const QColor& c) { color_ = c; }
    void setWidth(int w) { penWidth_ = qBound(1, w, 30); }

    void setTargetKey(const QString& k) { targetKey_ = k; }

signals:
    void annotateEvent(const QJsonObject& ev);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    void emitBegin(const QPointF& npt);
    void emitUpdate(const QVector<QPointF>& npts);
    void emitEnd();

    QPointF normFromPos(const QPoint& pos) const;

    AnnotModel* model_{nullptr};
    bool drawingEnabled_{false};
    AnnotModel::Tool tool_{AnnotModel::Pen};
    QColor color_{Qt::red};
    int penWidth_{3};
    QString currentId_;
    QVector<QPointF> livePts_;
    QString targetKey_;
    quint64 seq_{1};
};
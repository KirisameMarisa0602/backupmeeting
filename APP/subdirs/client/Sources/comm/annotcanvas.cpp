#include "annotcanvas.h"
#include <cmath>  // for std::atan2

AnnotCanvas::AnnotCanvas(QWidget* parent) : QWidget(parent)
{
    // 初始：可见性由 MainWindow 控制，交互默认关闭（鼠标穿透）
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true); // 默认不拦截鼠标（未开启绘制）
    setMouseTracking(true);
    hide();
}

void AnnotCanvas::setEnabledDrawing(bool on)
{
    drawingEnabled_ = on;
    // 仅控制鼠标是否穿透，不改变可见性
    setAttribute(Qt::WA_TransparentForMouseEvents, !on);
    // 光标反馈（可选）
    if (on) setCursor(Qt::CrossCursor);
    else    unsetCursor();
}

void AnnotCanvas::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing, true);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    if (model_) model_->paint(p, size());

    // 画当前临时笔划预览
    if (!livePts_.isEmpty()) {
        QPen pen(color_, penWidth_, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);

        auto D = [&](int i){ return QPointF(livePts_[i].x()*width(), livePts_[i].y()*height()); };

        switch (tool_) {
        case AnnotModel::Rect:
        case AnnotModel::Ellipse: {
            if (livePts_.size() >= 2) {
                QRectF r; r.setTopLeft(D(0)); r.setBottomRight(D(livePts_.size()-1));
                r = r.normalized();
                if (tool_ == AnnotModel::Rect) p.drawRect(r);
                else                           p.drawEllipse(r);
            }
            break;
        }
        case AnnotModel::Arrow:
            if (livePts_.size() >= 2) {
                QLineF ln(D(0), D(livePts_.size()-1));
                p.drawLine(ln.p1(), ln.p2());
                const double ah = qMax(8.0, penWidth_ * 3.0);
                const double aw = qMax(6.0, penWidth_ * 2.0);
                const double angle = std::atan2(ln.dy(), ln.dx());
                QPointF b = ln.p2();
                QPointF p1 = b + QPointF(-ah * std::cos(angle) + aw * std::sin(angle),
                                         -ah * std::sin(angle) - aw * std::cos(angle));
                QPointF p2 = b + QPointF(-ah * std::cos(angle) - aw * std::sin(angle),
                                         -ah * std::sin(angle) + aw * std::cos(angle));
                QPolygonF tri; tri << b << p1 << p2;
                p.setBrush(QBrush(color_));
                p.drawPolygon(tri);
            }
            break;
        case AnnotModel::Pen:
            if (livePts_.size() >= 2) {
                QPainterPath path(D(0));
                for (int i=1;i<livePts_.size();++i) path.lineTo(D(i));
                p.drawPath(path);
            }
            break;
        case AnnotModel::Text:
            if (!livePts_.isEmpty()) {
                QPen pp(color_, 1);
                p.setPen(pp);
                p.drawEllipse(D(0), 3, 3);
            }
            break;
        }
    }
}

QPointF AnnotCanvas::normFromPos(const QPoint& pos) const
{
    if (width() <= 1 || height() <= 1) return QPointF(0,0);
    return QPointF(qBound(0.0, pos.x() / double(width()), 1.0),
                   qBound(0.0, pos.y() / double(height()), 1.0));
}

void AnnotCanvas::emitBegin(const QPointF& npt)
{
    currentId_ = QString("%1-%2").arg(QCoreApplication::applicationPid()).arg(++seq_);
    QJsonObject ev{
        {"op","begin"},
        {"id", currentId_},
        {"tool",
            tool_==AnnotModel::Rect?"rect":
            tool_==AnnotModel::Ellipse?"ellipse":
            tool_==AnnotModel::Arrow?"arrow":
            tool_==AnnotModel::Text?"text":"pen"},
        {"color", color_.name(QColor::HexRgb)},
        {"width", penWidth_},
        {"pts", QJsonArray{ QJsonArray{ npt.x(), npt.y() } }},
        {"target", targetKey_},
        {"ts", QDateTime::currentMSecsSinceEpoch()}
    };
    emit annotateEvent(ev);
}

void AnnotCanvas::emitUpdate(const QVector<QPointF>& npts)
{
    if (currentId_.isEmpty() || npts.isEmpty()) return;
    QJsonArray arr;
    for (const auto& p : npts) arr.push_back(QJsonArray{ p.x(), p.y() });
    QJsonObject ev{
        {"op","update"},
        {"id", currentId_},
        {"pts", arr},
        {"target", targetKey_},
        {"ts", QDateTime::currentMSecsSinceEpoch()}
    };
    emit annotateEvent(ev);
}

void AnnotCanvas::emitEnd()
{
    if (currentId_.isEmpty()) return;
    QJsonObject ev{
        {"op","end"},
        {"id", currentId_},
        {"target", targetKey_},
        {"ts", QDateTime::currentMSecsSinceEpoch()}
    };
    emit annotateEvent(ev);
    currentId_.clear();
}

void AnnotCanvas::mousePressEvent(QMouseEvent* e)
{
    if (!drawingEnabled_ || !model_ || targetKey_.isEmpty()) return;
    if (e->button() != Qt::LeftButton) return;

    QPointF np = normFromPos(e->pos());

    if (tool_ == AnnotModel::Text) {
        bool ok=false;
        QString t = QInputDialog::getText(this, tr("文本注释"), tr("内容:"), QLineEdit::Normal, QString(), &ok);
        if (!ok || t.trimmed().isEmpty()) return;

        QString id = QString("%1-%2").arg(QCoreApplication::applicationPid()).arg(++seq_);
        QJsonObject evBegin{
            {"op","begin"},
            {"id", id},
            {"tool","text"},
            {"color", color_.name(QColor::HexRgb)},
            {"width", penWidth_},
            {"pts", QJsonArray{ QJsonArray{ np.x(), np.y() } }},
            {"text", t},
            {"target", targetKey_},
            {"ts", QDateTime::currentMSecsSinceEpoch()}
        };
        emit annotateEvent(evBegin);
        QJsonObject evEnd{{"op","end"},{"id",id},{"target",targetKey_},{"ts",QDateTime::currentMSecsSinceEpoch()}};
        emit annotateEvent(evEnd);
        return;
    }

    livePts_.clear();
    livePts_ << np;
    emitBegin(np);
    update();
}

void AnnotCanvas::mouseMoveEvent(QMouseEvent* e)
{
    if (!drawingEnabled_ || currentId_.isEmpty()) return;

    QPointF np = normFromPos(e->pos());

    auto pix = [&](const QPointF& n){ return QPointF(n.x()*width(), n.y()*height()); };

    if (tool_ == AnnotModel::Pen) {
        if (livePts_.isEmpty()) {
            livePts_.push_back(np);
            emitUpdate({np});
        } else {
            const qreal dist = QLineF(pix(livePts_.last()), pix(np)).length();
            if (dist >= 2.0) {
                livePts_.push_back(np);
                emitUpdate({np});
            }
        }
    } else {
        if (livePts_.size() == 1) {
            livePts_.push_back(np);
            emitUpdate({np});
        } else {
            livePts_.last() = np;
            emitUpdate({np});
        }
    }
    update();
}

void AnnotCanvas::mouseReleaseEvent(QMouseEvent* e)
{
    if (!drawingEnabled_ || e->button() != Qt::LeftButton) return;
    emitEnd();
    livePts_.clear();
    update();
}

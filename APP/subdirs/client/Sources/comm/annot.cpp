#include "annot.h"
#include <QtGlobal>
#include <cmath>

static inline QString toLower(const QString& s){ auto t=s; t.detach(); return t.toLower(); }

AnnotModel::Tool AnnotModel::toolFromString(const QString& s) {
    const QString t = toLower(s);
    if (t=="rect" || t=="rectangle") return Rect;
    if (t=="ellipse" || t=="oval")   return Ellipse;
    if (t=="arrow")                  return Arrow;
    if (t=="text")                   return Text;
    return Pen;
}

void AnnotModel::drawArrow(QPainter& p, const QPointF& a, const QPointF& b, int width, const QColor& color)
{
    QPen pen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.drawLine(a, b);

    const QLineF line(a, b);
    if (line.length() < 1.0) return;

    const double ah = qMax<double>(8.0, double(width) * 3.0); // 箭头长度
    const double aw = qMax<double>(6.0, double(width) * 2.0); // 箭头半宽
    const double angle = std::atan2(line.dy(), line.dx());
    const QPointF tip = b;

    const QPointF p1 = tip + QPointF(-ah * std::cos(angle) + aw * std::sin(angle),
                                     -ah * std::sin(angle) - aw * std::cos(angle));
    const QPointF p2 = tip + QPointF(-ah * std::cos(angle) - aw * std::sin(angle),
                                     -ah * std::sin(angle) + aw * std::cos(angle));
    QPolygonF tri; tri << tip << p1 << p2;
    QBrush brush(color);
    p.setBrush(brush);
    p.drawPolygon(tri);
}

bool AnnotModel::undoLastByOwner(const QString& owner)
{
    if (order_.isEmpty()) return false;
    for (int i = order_.size() - 1; i >= 0; --i) {
        const QString& id = order_.at(i);
        auto it = strokes_.find(id);
        if (it != strokes_.end() && it->owner == owner) {
            strokes_.erase(it);
            order_.removeAt(i);
            return true;
        }
    }
    return false;
}

bool AnnotModel::applyEvent(const QJsonObject& e)
{
    const QString op = e.value("op").toString();
    if (op == "clear") {
        clear();
        return true;
    }
    if (op == "undo") {
        const QString who = e.value("sender").toString();
        return undoLastByOwner(who);
    }

    const QString id = e.value("id").toString();
    if (id.isEmpty()) return false;

    if (op == "begin") {
        Stroke s;
        s.id    = id;
        s.owner = e.value("sender").toString();
        s.tool  = toolFromString(e.value("tool").toString());
        s.color = QColor(e.value("color").toString("#FF0000"));
        s.width = qBound(1, e.value("width").toInt(3), 30);
        // 初始点
        for (auto v : e.value("pts").toArray()) {
            auto a = v.toArray();
            if (a.size() >= 2) s.pts << QPointF(a[0].toDouble(), a[1].toDouble());
        }
        s.text = e.value("text").toString();
        strokes_.insert(id, s);
        order_.removeAll(id);
        order_.push_back(id);
        return true;
    } else if (op == "update") {
        auto it = strokes_.find(id);
        if (it == strokes_.end()) return false;
        for (auto v : e.value("pts").toArray()) {
            auto a = v.toArray();
            if (a.size() >= 2) it->pts << QPointF(a[0].toDouble(), a[1].toDouble());
        }
        return true;
    } else if (op == "end") {
        auto it = strokes_.find(id);
        if (it == strokes_.end()) return false;
        it->finished = true;
        return true;
    }
    return false;
}

void AnnotModel::paint(QPainter& p, const QSize& size) const
{
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform, true);

    for (const QString& id : order_) {
        const auto it = strokes_.find(id);
        if (it == strokes_.end()) continue;
        const Stroke& s = it.value();

        QPen pen(s.color, s.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);

        auto D = [&](int i){ return denorm(s.pts[i], size); };

        switch (s.tool) {
        case Rect:
            if (s.pts.size() >= 2) {
                QRectF r;
                r.setTopLeft(D(0));
                r.setBottomRight(D(s.pts.size()-1));
                r = r.normalized();
                p.drawRect(r);
            }
            break;
        case Ellipse:
            if (s.pts.size() >= 2) {
                QRectF r;
                r.setTopLeft(D(0));
                r.setBottomRight(D(s.pts.size()-1));
                r = r.normalized();
                p.drawEllipse(r);
            }
            break;
        case Arrow:
            if (s.pts.size() >= 2) {
                drawArrow(p, D(0), D(s.pts.size()-1), s.width, s.color);
            }
            break;
        case Pen:
            if (s.pts.size() >= 2) {
                QPainterPath path(D(0));
                for (int i=1;i<s.pts.size();++i) path.lineTo(D(i));
                p.drawPath(path);
            }
            break;
        case Text:
            if (!s.pts.isEmpty()) {
                QFont f = p.font();
                f.setPointSizeF(qMax(12.0, size.height() * 0.035));
                p.setFont(f);
                p.setPen(QPen(s.color, 1));
                p.drawText(D(0), s.text);
            }
            break;
        }
    }
}

void AnnotModel::clear()
{
    strokes_.clear();
    order_.clear();
}

#include "ViewBox.hpp"

#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSceneWheelEvent>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <limits>

namespace awo::plot {

namespace {

// the items' parent, carrying the data -> view transform (pyqtgraph's ChildGroup)
class ChildGroup : public QGraphicsItem {
public:
    explicit ChildGroup(QGraphicsItem* parent) : QGraphicsItem(parent) {
        setFlag(ItemHasNoContents);
    }
    QRectF boundingRect() const override { return {}; }
    void paint(QPainter*, const QStyleOptionGraphicsItem*, QWidget*) override {}
};

void notifyItems(const QList<QGraphicsItem*>& items) {
    for (QGraphicsItem* item : items) {
        if (auto* aware = dynamic_cast<ViewAware*>(item)) {
            aware->viewTransformChanged();
        }
    }
}

}  // namespace

ViewBox::ViewBox(QGraphicsItem* parent) : QGraphicsWidget(parent), childGroup_(new ChildGroup(this)) {
    setFlag(ItemClipsChildrenToShape);
    // (pyqtgraph's: under the plot's axes and anything added to the scene at 0)
    setZValue(-100);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    target_[XAxis] = range_[XAxis];
    target_[YAxis] = range_[YAxis];
    updateMatrix();
}

void ViewBox::addItem(QGraphicsItem* item) {
    if (item->zValue() < zValue()) {
        item->setZValue(zValue() + 1);
    }
    item->setParentItem(childGroup_);
    added_.append(item);
    if (auto* aware = dynamic_cast<ViewAware*>(item)) {
        aware->viewTransformChanged();
    }
}

void ViewBox::removeItem(QGraphicsItem* item) {
    added_.removeAll(item);
    if (item->parentItem() == childGroup_) {
        item->setParentItem(nullptr);
        if (scene() != nullptr) {
            scene()->removeItem(item);
        }
    }
}

QRectF ViewBox::viewRect() const {
    return {QPointF(range_[XAxis].min, range_[YAxis].min), QPointF(range_[XAxis].max, range_[YAxis].max)};
}

void ViewBox::setXRange(double min, double max, std::optional<double> padding) {
    setAxisRange(XAxis, min, max, padding);
    updateViewRange();
}

void ViewBox::setYRange(double min, double max, std::optional<double> padding) {
    setAxisRange(YAxis, min, max, padding);
    updateViewRange();
}

void ViewBox::setRange(const QRectF& rect, std::optional<double> padding) {
    setAxisRange(XAxis, rect.left(), rect.right(), padding);
    setAxisRange(YAxis, rect.top(), rect.bottom(), padding);
    updateViewRange();
}

double ViewBox::suggestPadding(Axis axis) const {
    const double length = axis == XAxis ? size().width() : size().height();
    if (defaultPadding_ == 0) {
        return 0;
    }
    const double most = std::max(0.1, defaultPadding_);
    if (length > 0) {
        return std::clamp(50 * defaultPadding_ / std::sqrt(length), defaultPadding_, most);
    }
    return defaultPadding_;
}

void ViewBox::setLimits(const Limits& limits) {
    limits_ = limits;
    setAxisRange(XAxis, target_[XAxis].min, target_[XAxis].max, 0.0);
    setAxisRange(YAxis, target_[YAxis].min, target_[YAxis].max, 0.0);
    updateViewRange();
}

// pyqtgraph's setRange for one axis: padding, then the range limits, then the limits it may move within
void ViewBox::setAxisRange(Axis axis, double min, double max, std::optional<double> padding) {
    double mn = std::min(min, max);
    double mx = std::max(min, max);
    bool preserve = false;
    if (mn == mx) {
        // a range of nothing: keep the scale it had
        preserve = true;
        double dy = range_[axis].span();
        if (dy == 0) {
            dy = 1;
        }
        mn -= dy * 0.5;
        mx += dy * 0.5;
    }
    const double quantum = (mn + mx) * 1.5e-15;
    if (mx - mn < 2 * quantum) {
        mn -= quantum;
        mx += quantum;
    }
    if (!std::isfinite(mn) || !std::isfinite(mx)) {
        return;
    }
    if (!preserve) {
        const double pad = padding.value_or(suggestPadding(axis));
        const double p = (mx - mn) * pad;
        mn -= p;
        mx += p;
    }
    const std::optional<double> lmn = axis == XAxis ? limits_.xMin : limits_.yMin;
    const std::optional<double> lmx = axis == XAxis ? limits_.xMax : limits_.yMax;
    std::optional<double> minRange = axis == XAxis ? limits_.minXRange : limits_.minYRange;
    std::optional<double> maxRange = axis == XAxis ? limits_.maxXRange : limits_.maxYRange;
    if (lmn && lmx) {
        maxRange = maxRange ? std::min(*maxRange, *lmx - *lmn) : *lmx - *lmn;
    }
    if (minRange || maxRange) {
        const double diff = mx - mn;
        double delta = 0;
        if (maxRange && diff > *maxRange) {
            delta = *maxRange - diff;
        } else if (minRange && diff < *minRange) {
            delta = *minRange - diff;
        }
        mn -= delta / 2;
        mx += delta / 2;
    }
    if (lmn && mn < *lmn) {
        mx += *lmn - mn;
        mn = *lmn;
    } else if (lmx && mx > *lmx) {
        mn += *lmx - mx;
        mx = *lmx;
    }
    target_[axis] = {mn, mx};
}

// pyqtgraph's updateViewRange (no aspect lock): the target range held within the limits, then everything told
void ViewBox::updateViewRange() {
    Range view[2] = {target_[XAxis], target_[YAxis]};
    for (const Axis axis : {XAxis, YAxis}) {
        const std::optional<double> lmn = axis == XAxis ? limits_.xMin : limits_.yMin;
        const std::optional<double> lmx = axis == XAxis ? limits_.xMax : limits_.yMax;
        std::optional<double> minRange = axis == XAxis ? limits_.minXRange : limits_.minYRange;
        std::optional<double> maxRange = axis == XAxis ? limits_.maxXRange : limits_.maxYRange;
        if (lmn && lmx) {
            maxRange = maxRange ? std::min(*maxRange, *lmx - *lmn) : *lmx - *lmn;
        }
        const double span = view[axis].span();
        if (minRange && *minRange > span) {
            view[axis].max = view[axis].min + *minRange;
            target_[axis] = view[axis];
        }
        if (maxRange && *maxRange < span) {
            view[axis].max = view[axis].min + *maxRange;
            target_[axis] = view[axis];
        }
        if (lmn && view[axis].min < *lmn) {
            const double delta = *lmn - view[axis].min;
            view[axis].min += delta;
            view[axis].max += delta;
            target_[axis] = view[axis];
        }
        if (lmx && view[axis].max > *lmx) {
            const double delta = view[axis].max - *lmx;
            view[axis].min -= delta;
            view[axis].max -= delta;
            target_[axis] = view[axis];
        }
    }
    bool changed[2];
    for (const Axis axis : {XAxis, YAxis}) {
        const double threshold = view[axis].span() * 1.0e-9;
        changed[axis] = std::abs(view[axis].min - range_[axis].min) > threshold ||
                        std::abs(view[axis].max - range_[axis].max) > threshold;
        range_[axis] = view[axis];
    }
    if (!changed[XAxis] && !changed[YAxis]) {
        return;
    }
    updateMatrix();
    update();
    if (changed[XAxis]) {
        emit xRangeChanged(this, range_[XAxis]);
    }
    if (changed[YAxis]) {
        emit yRangeChanged(this, range_[YAxis]);
    }
    emit rangeChanged(this);
}

// the child group's transform: the range filling the view, y up (pyqtgraph's updateMatrix)
void ViewBox::updateMatrix() {
    const QRectF bounds = rect();
    const QRectF view = viewRect();
    if (view.width() == 0 || view.height() == 0) {
        return;
    }
    const double sx = bounds.width() / view.width();
    const double sy = -bounds.height() / view.height();
    QTransform matrix;
    matrix.translate(bounds.center().x(), bounds.center().y());
    matrix.scale(sx, sy);
    matrix.translate(-view.center().x(), -view.center().y());
    childGroup_->setTransform(matrix);
    notifyItems(added_);
}

QTransform ViewBox::viewTransform() const {
    return childGroup_->transform();
}

QPointF ViewBox::mapFromView(const QPointF& point) const {
    return viewTransform().map(point);
}

QPointF ViewBox::mapToView(const QPointF& point) const {
    return viewTransform().inverted().map(point);
}

QPointF ViewBox::mapViewToScene(const QPointF& point) const {
    return mapToScene(mapFromView(point));
}

QPointF ViewBox::mapSceneToView(const QPointF& point) const {
    return mapToView(mapFromScene(point));
}

QPointF ViewBox::viewPixelSize() const {
    const QSizeF size = this->size();
    if (size.width() <= 0 || size.height() <= 0) {
        return {};
    }
    return {range_[XAxis].span() / size.width(), range_[YAxis].span() / size.height()};
}

QRectF ViewBox::boundingRect() const {
    return rect().adjusted(0, 0, 0.5, 0.5);
}

QPainterPath ViewBox::shape() const {
    QPainterPath path;
    path.addRect(boundingRect());
    return path;
}

QRectF ViewBox::screenGeometry() const {
    if (scene() == nullptr || scene()->views().isEmpty()) {
        return sceneBoundingRect();
    }
    return QRectF(scene()->views().first()->mapFromScene(sceneBoundingRect()).boundingRect());
}

void ViewBox::setXLink(ViewBox* view) {
    if (xLink_ != nullptr) {
        disconnect(xLink_, nullptr, this, nullptr);
    }
    xLink_ = view;
    if (view != nullptr) {
        connect(view, &ViewBox::xRangeChanged, this, &ViewBox::linkedXChanged);
        connect(view, &ViewBox::resized, this, &ViewBox::linkedXChanged);
        linkedXChanged();
    }
}

// the linked view's x range, lined up with it on screen where the two overlap (pyqtgraph's linkedViewChanged)
void ViewBox::linkedXChanged() {
    if (xLink_ == nullptr) {
        return;
    }
    const Range vr = xLink_->xRange();
    const QRectF vg = xLink_->screenGeometry();
    const QRectF sg = screenGeometry();
    double x1 = vr.min;
    double x2 = vr.max;
    const double overlap = std::min(sg.right(), vg.right()) - std::max(sg.left(), vg.left());
    if (vg.width() > 0 && overlap >= std::min(vg.width() / 3, sg.width() / 3)) {
        const double upp = vr.span() / vg.width();
        x1 = vr.min + (sg.x() - vg.x()) * upp;
        x2 = x1 + sg.width() * upp;
    }
    setXRange(x1, x2, 0.0);
}

void ViewBox::setMouseEnabled(bool x, bool y) {
    mouseEnabled_[XAxis] = x;
    mouseEnabled_[YAxis] = y;
    setAcceptedMouseButtons(x || y ? Qt::LeftButton | Qt::MiddleButton : Qt::NoButton);
}

void ViewBox::resizeEvent(QGraphicsSceneResizeEvent* event) {
    QGraphicsWidget::resizeEvent(event);
    updateMatrix();
    emit resized(this);
}

void ViewBox::scaleBy(std::optional<double> x, std::optional<double> y, QPointF center) {
    if (!x && !y) {
        return;
    }
    const QRectF vr(QPointF(target_[XAxis].min, target_[YAxis].min), QPointF(target_[XAxis].max, target_[YAxis].max));
    const QPointF scale(x.value_or(1.0), y.value_or(1.0));
    const QPointF tl = center + QPointF((vr.left() - center.x()) * scale.x(), (vr.top() - center.y()) * scale.y());
    const QPointF br =
        center + QPointF((vr.right() - center.x()) * scale.x(), (vr.bottom() - center.y()) * scale.y());
    if (!x) {
        setYRange(tl.y(), br.y(), 0.0);
    } else if (!y) {
        setXRange(tl.x(), br.x(), 0.0);
    } else {
        setRange(QRectF(tl, br), 0.0);
    }
}

void ViewBox::translateBy(std::optional<double> x, std::optional<double> y) {
    if (x) {
        setAxisRange(XAxis, target_[XAxis].min + *x, target_[XAxis].max + *x, 0.0);
    }
    if (y) {
        setAxisRange(YAxis, target_[YAxis].min + *y, target_[YAxis].max + *y, 0.0);
    }
    updateViewRange();
}

void ViewBox::wheelEvent(QGraphicsSceneWheelEvent* event) {
    if (!mouseEnabled_[XAxis] && !mouseEnabled_[YAxis]) {
        event->ignore();
        return;
    }
    // pyqtgraph's: 1.02 to the power of an eighth of the wheel's turn, about the point under the mouse
    const double s = std::pow(1.02, event->delta() * (-1.0 / 8.0));
    const QPointF center = mapToView(event->pos());
    scaleBy(mouseEnabled_[XAxis] ? std::optional(s) : std::nullopt,
            mouseEnabled_[YAxis] ? std::optional(s) : std::nullopt, center);
    event->accept();
    emit rangeChangedManually();
}

void ViewBox::mousePressEvent(QGraphicsSceneMouseEvent* event) {
    if (!(event->button() & (Qt::LeftButton | Qt::MiddleButton)) ||
        (!mouseEnabled_[XAxis] && !mouseEnabled_[YAxis])) {
        event->ignore();
        return;
    }
    dragging_ = false;
    lastDragPos_ = event->pos();
    event->accept();
}

void ViewBox::mouseMoveEvent(QGraphicsSceneMouseEvent* event) {
    if (!dragging_) {
        // (a drag once it's moved as far as Qt's drag distance, like pyqtgraph's scene; a click until then)
        const QPointF moved = event->screenPos() - event->buttonDownScreenPos(Qt::LeftButton);
        if (std::hypot(moved.x(), moved.y()) < 5 && (event->buttons() & Qt::LeftButton)) {
            return;
        }
        dragging_ = true;
    }
    const QPointF dif = lastDragPos_ - event->pos();
    lastDragPos_ = event->pos();
    const QTransform inverse = viewTransform().inverted();
    const QPointF masked(mouseEnabled_[XAxis] ? dif.x() : 0, mouseEnabled_[YAxis] ? dif.y() : 0);
    const QPointF moved = inverse.map(masked) - inverse.map(QPointF(0, 0));
    translateBy(mouseEnabled_[XAxis] ? std::optional(moved.x()) : std::nullopt,
                mouseEnabled_[YAxis] ? std::optional(moved.y()) : std::nullopt);
    emit rangeChangedManually();
}

void ViewBox::mouseReleaseEvent(QGraphicsSceneMouseEvent* event) {
    dragging_ = false;
    event->accept();
}

}  // namespace awo::plot

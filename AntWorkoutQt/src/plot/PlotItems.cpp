#include "PlotItems.hpp"

#include <QGraphicsSceneMouseEvent>
#include <QPainter>
#include <QPainterPathStroker>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace awo::plot {

namespace {

ViewBox* viewOf(const QGraphicsItem* item) {
    // (added to a view: a child of its child group)
    const QGraphicsItem* group = item->parentItem();
    return group != nullptr ? dynamic_cast<ViewBox*>(group->parentItem()) : nullptr;
}

// pyqtgraph's symbols, a unit across
QPainterPath symbolPath(const QString& symbol) {
    QPainterPath path;
    if (symbol == QLatin1String("o")) {
        path.addEllipse(QRectF(-0.5, -0.5, 1, 1));
        return path;
    }
    if (symbol == QLatin1String("s")) {
        path.addRect(QRectF(-0.5, -0.5, 1, 1));
        return path;
    }
    QList<QPointF> points;
    if (symbol == QLatin1String("d")) {
        points = {{0., -0.5}, {-0.4, 0.}, {0, 0.5}, {0.4, 0}};
    } else if (symbol == QLatin1String("t")) {
        points = {{-0.5, -0.5}, {0, 0.5}, {0.5, -0.5}};
    } else {
        path.addEllipse(QRectF(-0.5, -0.5, 1, 1));
        return path;
    }
    path.moveTo(points[0]);
    for (qsizetype i = 1; i < points.size(); ++i) {
        path.lineTo(points[i]);
    }
    path.closeSubpath();
    return path;
}

}  // namespace

QPen mkPen(const QColor& color, double width) {
    QPen pen(QBrush(color), width);
    pen.setCosmetic(true);
    return pen;
}

// ---- CurveItem ----

CurveItem::CurveItem(QGraphicsItem* parent) : QGraphicsObject(parent), pen_(mkPen(QColor(200, 200, 200))) {
    setAcceptedMouseButtons(Qt::NoButton);
}

ViewBox* CurveItem::view() const {
    return viewOf(this);
}

void CurveItem::setData(const QList<double>& xs, const QList<double>& ys) {
    prepareGeometryChange();
    noteDataChange(xs, ys);
    xs_ = xs;
    ys_ = ys;
    path_ = QPainterPath();
    const qsizetype count = std::min(xs.size(), ys.size());
    sorted_ = std::is_sorted(xs.begin(), xs.begin() + count);
    if (count > 0) {
        path_.reserve(static_cast<int>(count));
        path_.moveTo(xs[0], ys[0]);
        for (qsizetype i = 1; i < count; ++i) {
            path_.lineTo(xs[i], ys[i]);
        }
        const auto [xMin, xMax] = std::minmax_element(xs.begin(), xs.begin() + count);
        const auto [yMin, yMax] = std::minmax_element(ys.begin(), ys.begin() + count);
        dataBounds_ = QRectF(QPointF(*xMin, *yMin), QPointF(*xMax, *yMax));
    } else {
        dataBounds_ = QRectF();
    }
    update();
}

void CurveItem::setData(const QList<double>& xs, const QList<double>& ys, const QPen& pen,
                        const std::optional<QPen>& shadowPen) {
    layerStale_ = layerStale_ || pen != pen_ || shadowPen != shadowPen_;
    pen_ = pen;
    shadowPen_ = shadowPen;
    setData(xs, ys);
}

void CurveItem::setPen(const QPen& pen) {
    prepareGeometryChange();
    layerStale_ = true;
    pen_ = pen;
    update();
}

void CurveItem::setShadowPen(const std::optional<QPen>& pen) {
    prepareGeometryChange();
    layerStale_ = true;
    shadowPen_ = pen;
    update();
}

void CurveItem::setClickable(bool clickable, double tolerance) {
    clickable_ = clickable;
    tolerance_ = tolerance;
    prepareGeometryChange();
}

double CurveItem::pixelPadding() const {
    double width = pen_.widthF();
    if (shadowPen_ && shadowPen_->style() != Qt::NoPen) {
        width = std::max(width, shadowPen_->widthF());
    }
    // (half the pen, 1 for the antialiasing, 1 for the image being up to half a pixel from where the view has it)
    return std::max(width, 1.0) / 2 + 2 + (clickable_ ? tolerance_ : 0);
}

double CurveItem::strokeReach() const {
    double width = pen_.widthF();
    if (shadowPen_ && shadowPen_->style() != Qt::NoPen) {
        width = std::max(width, shadowPen_->widthF());
    }
    return width * M_SQRT1_2 + 2;
}

QRectF CurveItem::boundingRect() const {
    if (xs_.isEmpty()) {
        return {};
    }
    const ViewBox* box = view();
    const QPointF px = box != nullptr ? box->viewPixelSize() : QPointF();
    const double pad = pixelPadding();
    return dataBounds_.normalized().adjusted(-pad * px.x(), -pad * px.y(), pad * px.x(), pad * px.y());
}

void CurveItem::viewTransformChanged() {
    prepareGeometryChange();
}

QPainterPath CurveItem::shape() const {
    if (!clickable_ || xs_.isEmpty()) {
        return QGraphicsObject::shape();
    }
    // the line in pixels, tolerance either side of it, back in data units
    const ViewBox* box = view();
    if (box == nullptr) {
        return {};
    }
    const QTransform toPixels = box->viewTransform();
    QPainterPathStroker stroker;
    stroker.setWidth(2 * tolerance_);
    return toPixels.inverted().map(stroker.createStroke(toPixels.map(path_)));
}

void CurveItem::noteDataChange(const QList<double>& xs, const QList<double>& ys) {
    if (layerStale_ || layer_.isNull()) {
        return;
    }
    const qsizetype count = std::min(xs.size(), ys.size());
    const qsizetype oldCount = std::min(xs_.size(), ys_.size());
    if (count == 0 || oldCount == 0 || !sorted_ || !std::is_sorted(xs.begin(), xs.begin() + count)) {
        layerStale_ = true;
        return;
    }
    // the points the old and new data share: from where the new data starts (older ones dropped) until they differ
    const qsizetype start = std::lower_bound(xs_.begin(), xs_.begin() + oldCount, xs[0]) - xs_.begin();
    qsizetype shared = 0;
    while (shared < count && start + shared < oldCount && xs[shared] == xs_[start + shared] &&
           ys[shared] == ys_[start + shared]) {
        ++shared;
    }
    if (shared == 0) {
        layerStale_ = true;
        return;
    }
    if (start > 0) {
        layerChanges_.append({xs_[0], xs[0]});
    }
    if (shared < count || start + shared < oldCount) {
        layerChanges_.append({xs[shared - 1], std::max(xs[count - 1], xs_[oldCount - 1])});
    }
}

int CurveItem::placeLayer(const QTransform& device, const QSize& deviceSize, double dpr) {
    const int width = deviceSize.width();
    const int height = deviceSize.height();
    // (the point in the middle of the device, where any difference in the scale from the image's balances out)
    const auto leftEdge = [&] {
        const double middle = (width / 2. - device.dx()) / device.m11();
        return static_cast<int>(std::lround(width / 2. - layerTransform_.map(QPointF(middle, 0)).x()));
    };
    bool fits = !layerStale_ && !layer_.isNull() && layer_.width() == 2 * width && layer_.height() == height &&
                layerDpr_ == dpr;
    if (fits) {
        // (a range's rounding moves the scale a little every time; it's only new once it would show)
        const double close = 0.05;
        fits = std::abs(device.m11() / layerTransform_.m11() - 1) * width < close &&
               std::abs(device.m22() / layerTransform_.m22() - 1) * height < close &&
               std::abs(device.dy() - layerTransform_.dy()) < close;
    }
    if (fits) {
        const int left = leftEdge();
        fits = left <= 0 && left + layer_.width() >= width;
    }
    if (!fits) {
        // as much to spare as half the device each way
        layer_ = QImage(2 * width, height, QImage::Format_ARGB32_Premultiplied);
        layerDpr_ = dpr;
        layerTransform_ = QTransform(device.m11(), 0, 0, device.m22(), device.dx() + width / 2, device.dy());
        layerStale_ = false;
        layerChanges_.clear();
        const double scale = layerTransform_.m11();
        drawLayer(-layerTransform_.dx() / scale, (layer_.width() - layerTransform_.dx()) / scale);
    }
    for (const auto& [fromX, toX] : std::as_const(layerChanges_)) {
        drawLayer(fromX, toX);
    }
    layerChanges_.clear();
    return leftEdge();
}

// the pixels of the image a stretch of the data shows in, cleared and drawn again from the points that reach them
void CurveItem::drawLayer(double fromX, double toX) {
    const double scale = layerTransform_.m11();
    const double offset = layerTransform_.dx();
    const double reach = strokeReach() * layerDpr_;
    const QRect clip = QRect(QPoint(static_cast<int>(std::floor(fromX * scale + offset - reach)), 0),
                             QPoint(static_cast<int>(std::ceil(toX * scale + offset + reach)), layer_.height() - 1))
                           .intersected(layer_.rect());
    if (clip.isEmpty()) {
        return;
    }
    QPainterPath path;
    if (sorted_) {
        const auto end = xs_.begin() + std::min(xs_.size(), ys_.size());
        // (and one past each side, for the lines that cross them)
        qsizetype first = std::lower_bound(xs_.begin(), end, (clip.left() - reach - offset) / scale) - xs_.begin();
        qsizetype last = std::upper_bound(xs_.begin(), end, (clip.right() + 1 + reach - offset) / scale) - xs_.begin();
        first = std::max<qsizetype>(first - 1, 0);
        last = std::min<qsizetype>(last, end - xs_.begin() - 1);
        path.reserve(static_cast<int>(last - first + 1));
        path.moveTo(xs_[first], ys_[first]);
        for (qsizetype i = first + 1; i <= last; ++i) {
            path.lineTo(xs_[i], ys_[i]);
        }
    } else {
        path = path_;
    }
    QPainter painter(&layer_);
    painter.setCompositionMode(QPainter::CompositionMode_Clear);
    painter.fillRect(clip, Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setClipRect(clip);
    painter.setTransform(layerTransform_);
    // (the image's a pixel a pixel, so the pens' cosmetic widths in its pixels)
    const auto stroke = [&](QPen pen) {
        pen.setWidthF(std::max(pen.widthF(), 1.0) * layerDpr_);
        painter.setPen(pen);
        painter.drawPath(path);
    };
    if (shadowPen_ && shadowPen_->style() != Qt::NoPen) {
        stroke(*shadowPen_);
    }
    stroke(pen_);
}

void CurveItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    if (xs_.isEmpty()) {
        return;
    }
    const QTransform world = painter->transform();
    if (world.type() > QTransform::TxScale || world.m11() <= 0 || world.m22() == 0) {
        // (nothing the app does: not something an image can shift to)
        painter->setRenderHint(QPainter::Antialiasing);
        if (shadowPen_ && shadowPen_->style() != Qt::NoPen) {
            painter->setPen(*shadowPen_);
            painter->drawPath(path_);
        }
        painter->setPen(pen_);
        painter->drawPath(path_);
        return;
    }
    const QPaintDevice* device = painter->device();
    const double dpr = device->devicePixelRatioF();
    // (a widget's size isn't in pixels, an image's is: big enough for either)
    const QSize size(static_cast<int>(std::ceil(device->width() * dpr)), static_cast<int>(std::ceil(device->height() * dpr)));
    const int left = placeLayer(world * QTransform::fromScale(dpr, dpr), size, dpr);
    painter->resetTransform();
    painter->drawImage(QRectF(left / dpr, 0, layer_.width() / dpr, layer_.height() / dpr), layer_);
    painter->setTransform(world);
}

bool CurveItem::click(const QPointF& scenePos, Qt::MouseButton button) {
    if (!clickable_ || button != Qt::LeftButton || !shape().contains(mapFromScene(scenePos))) {
        return false;
    }
    emit clicked(this);
    return true;
}

// ---- InfiniteLine ----

InfiniteLine::InfiniteLine(double pos, double angle, QGraphicsItem* parent)
    : QGraphicsObject(parent), pos_(pos), angle_(angle), pen_(mkPen(QColor(200, 200, 100))) {
    setAcceptedMouseButtons(Qt::NoButton);
}

ViewBox* InfiniteLine::view() const {
    return viewOf(this);
}

void InfiniteLine::setValue(double pos) {
    if (pos == pos_) {
        return;
    }
    prepareGeometryChange();
    pos_ = pos;
    update();
}

void InfiniteLine::setPen(const QPen& pen) {
    prepareGeometryChange();
    pen_ = pen;
    update();
}

// pyqtgraph's addMarker: each a unit across, pointing along the line ("^" toward its end) or across it
void InfiniteLine::addMarker(const QString& marker, double position, double size) {
    QPainterPath path;
    auto add = [&path](const QList<QPointF>& points) {
        path.addPolygon(QPolygonF(points));
        path.closeSubpath();
    };
    if (marker == QLatin1String("o")) {
        path.addEllipse(QRectF(-0.5, -0.5, 1, 1));
    }
    if (marker.contains(QLatin1String("<|"))) {
        add({{0.5, 0}, {0, -0.5}, {-0.5, 0}});
    }
    if (marker.contains(QLatin1String("|>"))) {
        add({{0.5, 0}, {0, 0.5}, {-0.5, 0}});
    }
    if (marker.contains(QLatin1String(">|"))) {
        add({{0.5, -0.5}, {0, 0}, {-0.5, -0.5}});
    }
    if (marker.contains(QLatin1String("|<"))) {
        add({{0.5, 0.5}, {0, 0}, {-0.5, 0.5}});
    }
    if (marker.contains(u'^')) {
        add({{0, -0.5}, {0.5, 0}, {0, 0.5}});
    }
    if (marker.contains(u'v')) {
        add({{0, -0.5}, {-0.5, 0}, {0, 0.5}});
    }
    prepareGeometryChange();
    markers_.append({path, position, size});
    maxMarkerSize_ = std::max(maxMarkerSize_, size / 2.);
    update();
}

void InfiniteLine::clearMarkers() {
    prepareGeometryChange();
    markers_.clear();
    maxMarkerSize_ = 0;
    update();
}

std::pair<QPointF, QPointF> InfiniteLine::endPoints() const {
    const ViewBox* box = view();
    if (box == nullptr) {
        return {};
    }
    const Range x = box->xRange();
    const Range y = box->yRange();
    if (std::fmod(angle_, 180) == 0) {
        return {{x.min, pos_}, {x.max, pos_}};
    }
    return {{pos_, y.min}, {pos_, y.max}};
}

QRectF InfiniteLine::boundingRect() const {
    const ViewBox* box = view();
    if (box == nullptr) {
        return {};
    }
    const auto [start, end] = endPoints();
    const QPointF px = box->viewPixelSize();
    const bool horizontal = std::fmod(angle_, 180) == 0;
    const double reach = maxMarkerSize_ + pen_.widthF() / 2 + 1;
    const QRectF line = QRectF(start, end).normalized();
    return horizontal ? line.adjusted(0, -reach * px.y(), 0, reach * px.y())
                      : line.adjusted(-reach * px.x(), 0, reach * px.x(), 0);
}

void InfiniteLine::viewTransformChanged() {
    prepareGeometryChange();
}

void InfiniteLine::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    if (view() == nullptr) {
        return;
    }
    const auto [left, right] = endPoints();
    QPen pen = pen_;
    pen.setJoinStyle(Qt::MiterJoin);
    painter->setPen(pen);
    painter->drawLine(left, right);
    if (markers_.isEmpty()) {
        return;
    }
    // the markers in pixels, along the line as drawn on screen (pyqtgraph's paint)
    const QTransform tr = painter->transform();
    const bool horizontal = std::fmod(angle_, 180) == 0;
    const QPointF start = tr.map(left);
    const QPointF end = tr.map(right);
    const QPointF up = tr.map(left + (horizontal ? QPointF(0, 1) : QPointF(-1, 0))) - start;
    const QPointF dif = end - start;
    const double length = std::hypot(dif.x(), dif.y());
    const double angle = std::atan2(dif.y(), dif.x()) * 180 / M_PI;
    painter->resetTransform();
    painter->translate(start);
    painter->rotate(angle);
    const double det = up.x() * dif.y() - dif.x() * up.y();
    painter->scale(1, det > 0 ? 1 : -1);
    painter->setBrush(pen_.color());
    const QTransform base = painter->transform();
    for (const Marker& marker : markers_) {
        painter->setTransform(base);
        painter->translate(length * marker.position, 0);
        painter->scale(marker.size, marker.size);
        painter->drawPath(marker.path);
    }
    painter->setTransform(tr);
}

// ---- ScatterItem ----

ScatterItem::ScatterItem(const QString& symbol, double size, QGraphicsItem* parent)
    : QGraphicsObject(parent),
      symbol_(symbol),
      size_(size),
      brush_(QColor(100, 100, 150)),
      pen_(mkPen(QColor(200, 200, 200))) {
    setAcceptedMouseButtons(Qt::NoButton);
}

ViewBox* ScatterItem::view() const {
    return viewOf(this);
}

void ScatterItem::setData(const QList<double>& xs, const QList<double>& ys) {
    prepareGeometryChange();
    points_.clear();
    for (qsizetype i = 0; i < std::min(xs.size(), ys.size()); ++i) {
        points_.append({xs[i], ys[i]});
    }
    update();
}

void ScatterItem::setBrush(const QBrush& brush) {
    brush_ = brush;
    image_ = QImage();
    update();
}

void ScatterItem::setPen(const QPen& pen) {
    prepareGeometryChange();
    pen_ = pen;
    image_ = QImage();
    update();
}

void ScatterItem::setSize(double size) {
    prepareGeometryChange();
    size_ = size;
    image_ = QImage();
    update();
}

QRectF ScatterItem::boundingRect() const {
    const ViewBox* box = view();
    if (points_.isEmpty() || box == nullptr) {
        return {};
    }
    double xMin = points_[0].x(), xMax = xMin, yMin = points_[0].y(), yMax = yMin;
    for (const QPointF& point : points_) {
        xMin = std::min(xMin, point.x());
        xMax = std::max(xMax, point.x());
        yMin = std::min(yMin, point.y());
        yMax = std::max(yMax, point.y());
    }
    const QPointF px = box->viewPixelSize();
    const double reach = (size_ + pen_.widthF()) / 2 + 1;
    return QRectF(QPointF(xMin, yMin), QPointF(xMax, yMax))
        .adjusted(-reach * px.x(), -reach * px.y(), reach * px.x(), reach * px.y());
}

void ScatterItem::viewTransformChanged() {
    prepareGeometryChange();
}

// the symbol drawn once, antialiased, then copied to each point (pyqtgraph's renderSymbol and fragment atlas)
const QImage& ScatterItem::symbolImage(double dpr) {
    if (image_.isNull() || image_.devicePixelRatio() != dpr) {
        const int penPx = std::max(static_cast<int>(std::ceil(pen_.widthF())), 1);
        const int side = static_cast<int>(std::ceil(dpr * (size_ + penPx)));
        image_ = QImage(side, side, QImage::Format_ARGB32_Premultiplied);
        image_.setDevicePixelRatio(dpr);
        image_.fill(Qt::transparent);
        QPainter p(&image_);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(side / dpr * 0.5, side / dpr * 0.5);
        p.scale(size_, size_);
        p.setPen(pen_);
        p.setBrush(brush_);
        p.drawPath(symbolPath(symbol_));
    }
    return image_;
}

void ScatterItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget* widget) {
    if (points_.isEmpty()) {
        return;
    }
    const double dpr = widget != nullptr ? widget->devicePixelRatioF() : 1.0;
    const QImage& image = symbolImage(dpr);
    const QPixmap pixmap = QPixmap::fromImage(image);
    const QTransform tr = painter->transform();
    painter->resetTransform();
    QList<QPainter::PixmapFragment> fragments;
    for (const QPointF& point : points_) {
        const QPointF at = tr.map(point);
        fragments.append(QPainter::PixmapFragment::create(at, QRectF(0, 0, image.width(), image.height()),
                                                          1 / dpr, 1 / dpr));
    }
    painter->drawPixmapFragments(fragments.constData(), static_cast<int>(fragments.size()), pixmap);
    painter->setTransform(tr);
}

}  // namespace awo::plot

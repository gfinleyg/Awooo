#include "SolidColoredColumns.hpp"

#include <QPainter>

#include <algorithm>

namespace awo::plot {

SolidColoredColumns::SolidColoredColumns(QGraphicsItem* parent) : QGraphicsObject(parent) {
    setAcceptedMouseButtons(Qt::NoButton);
}

void SolidColoredColumns::setData(const QList<double>& xs, const QList<double>& ys, const QList<double>& ranges,
                                  const QList<QColor>& colors) {
    colors_ = colors;
    colorer_ = nullptr;
    setColumns(xs, ys, ranges);
}

void SolidColoredColumns::setData(const QList<double>& xs, const QList<double>& ys, const QList<double>& ranges,
                                  const Colorer& colorer) {
    colors_.clear();
    colorer_ = colorer;
    setColumns(xs, ys, ranges);
}

void SolidColoredColumns::setColumns(const QList<double>& xs, const QList<double>& ys,
                                     const QList<double>& ranges) {
    prepareGeometryChange();
    xs_ = xs;
    ys_ = ys;
    ranges_ = ranges;
    bounds_ = QRectF();
    if (!xs.isEmpty()) {
        const auto [xMin, xMax] = std::minmax_element(xs.begin(), xs.end());
        double yMin = 0, yMax = 0;
        for (const double y : ys) {
            yMin = std::min(yMin, y);
            yMax = std::max(yMax, y);
        }
        bounds_ = QRectF(*xMin, yMin, *xMax - *xMin, yMax - yMin);
    }
    rectsDirty_ = true;
    update();
}

void SolidColoredColumns::recolor() {
    rectsDirty_ = true;
    update();
}

void SolidColoredColumns::clear() {
    setColumns({}, {}, {});
}

void SolidColoredColumns::buildRects() {
    rects_.clear();
    rectsDirty_ = false;
    const qsizetype columns = std::min(xs_.size() - 1, ys_.size());
    for (qsizetype i = 0; i < columns; ++i) {
        QColor color;
        if (colorer_) {
            color = colorer_(ys_[i], ranges_);
        } else {
            // how many ranges are below the value (the ranges go up)
            const auto index = std::lower_bound(ranges_.begin(), ranges_.end(), ys_[i]) - ranges_.begin();
            if (index >= colors_.size()) {
                continue;
            }
            color = colors_[index];
        }
        const QRectF rect(xs_[i], 0, xs_[i + 1] - xs_[i], ys_[i]);
        auto found = std::find_if(rects_.begin(), rects_.end(),
                                  [&](const auto& entry) { return entry.first.rgba() == color.rgba(); });
        if (found == rects_.end()) {
            rects_.append({color, {rect}});
        } else {
            found->second.append(rect);
        }
    }
}

QRectF SolidColoredColumns::boundingRect() const {
    return bounds_;
}

void SolidColoredColumns::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    if (xs_.isEmpty()) {
        return;
    }
    if (rectsDirty_) {
        buildRects();
    }
    painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter->setPen(Qt::NoPen);
    for (const auto& [color, rects] : rects_) {
        painter->setBrush(color);
        painter->drawRects(rects.constData(), static_cast<int>(rects.size()));
    }
}

}  // namespace awo::plot

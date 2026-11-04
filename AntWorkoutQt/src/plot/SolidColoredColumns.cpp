#include "SolidColoredColumns.hpp"

#include <QPainter>

#include <algorithm>

#include "HatchTile.hpp"

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

void SolidColoredColumns::setHatched(bool hatched) {
    if (hatched_ != hatched) {
        hatched_ = hatched;
        rectsDirty_ = true;
        update();
    }
}

void SolidColoredColumns::setHatchScale(double scale) {
    if (hatchScale_ != scale) {
        hatchScale_ = scale;
        update();
    }
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
                                  [&](const ColorRects& entry) { return entry.color.rgba() == color.rgba(); });
        if (found == rects_.end()) {
            rects_.append({color, hatched_ ? hatchInk(color) : QColor(), {rect}});
        } else {
            found->rects.append(rect);
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
    for (const ColorRects& entry : rects_) {
        painter->setBrush(entry.color);
        painter->drawRects(entry.rects.constData(), static_cast<int>(entry.rects.size()));
    }
    if (hatched_) {
        // on the screen's pixels: the tile would be stretched to the graph's scale otherwise, and resampled each frame
        const QTransform toScreen = painter->worldTransform();
        const double ratio = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        // (so the lines go along with the columns)
        const QPointF anchor(toScreen.map(QPointF(0, 0)).x(), 0);
        painter->save();
        painter->resetTransform();
        for (const ColorRects& entry : rects_) {
            const QPixmap tile = hatchTile(entry.ink, hatchScale_, ratio);
            for (const QRectF& rect : entry.rects) {
                drawHatch(*painter, toScreen.mapRect(rect).toRect(), tile, anchor);
            }
        }
        painter->restore();
    }
}

}  // namespace awo::plot

#include "AxisItem.hpp"

#include <QFontMetricsF>
#include <QGraphicsTextItem>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <numeric>

#include "PyCompat.hpp"

namespace awo::plot {

namespace {

// pyqtgraph's foreground: 'd', a mid grey
const QColor kForeground(150, 150, 150);

// how much of the axis the tick numbers may fill, by how many already have numbers (pyqtgraph's textFillLimits)
constexpr std::pair<int, double> kTextFillLimits[] = {{0, 0.8}, {2, 0.6}, {4, 0.4}, {6, 0.2}};
constexpr int kMaxTickLevel = 2;
constexpr int kMaxTextLevel = 2;

QPen cosmetic(const QColor& color) {
    QPen pen(color, 1);
    pen.setCosmetic(true);
    return pen;
}

}  // namespace

AxisItem::AxisItem(Orientation orientation, QGraphicsItem* parent)
    : QGraphicsWidget(parent),
      orientation_(orientation),
      label_(new QGraphicsTextItem(this)),
      pen_(cosmetic(kForeground)),
      textPen_(cosmetic(kForeground)),
      // (labels on a vertical axis may reach past its ends; on a horizontal one they're hidden there)
      hideOverlappingLabels_(!vertical()) {
    if (vertical()) {
        label_->setRotation(-90);
    }
    labelStyle_ = {{QStringLiteral("color"), textPen_.color().name()}};
    showLabel(false);
    updateLabel();
}

void AxisItem::linkToView(ViewBox* view) {
    if (view_ != nullptr) {
        disconnect(view_, nullptr, this, nullptr);
    }
    view_ = view;
    if (view == nullptr) {
        return;
    }
    if (vertical()) {
        connect(view, &ViewBox::yRangeChanged, this, &AxisItem::linkedViewChanged);
    } else {
        connect(view, &ViewBox::xRangeChanged, this, &AxisItem::linkedViewChanged);
    }
    connect(view, &ViewBox::resized, this, &AxisItem::linkedViewChanged);
    linkedViewChanged();
}

void AxisItem::linkedViewChanged() {
    if (view_ == nullptr) {
        return;
    }
    const Range range = vertical() ? view_->yRange() : view_->xRange();
    setRange(range.min, range.max);
}

void AxisItem::setRange(double min, double max) {
    if (!std::isfinite(min) || !std::isfinite(max)) {
        return;
    }
    range_ = {min, max};
    update();
}

void AxisItem::setPen(const QPen& pen) {
    pen_ = pen;
    setLabelColor(pen.color());
}

void AxisItem::setTextPen(const QPen& pen) {
    textPen_ = pen;
    setLabelColor(pen.color());
}

// the title's color, where it was in its style (pyqtgraph's labelStyle['color'])
void AxisItem::setLabelColor(const QColor& color) {
    auto found = std::find_if(labelStyle_.begin(), labelStyle_.end(),
                              [](const auto& entry) { return entry.first == QLatin1String("color"); });
    if (found != labelStyle_.end()) {
        found->second = color.name();
    } else {
        labelStyle_.append({QStringLiteral("color"), color.name()});
    }
    updateLabel();
}

void AxisItem::setTickPen(const std::optional<QPen>& pen) {
    tickPen_ = pen;
    updateLabel();
}

void AxisItem::setGrid(std::optional<int> alpha) {
    grid_ = alpha;
    prepareGeometryChange();
    update();
}

void AxisItem::setTickFont(const std::optional<QFont>& font) {
    tickFont_ = font;
    prepareGeometryChange();
    update();
}

void AxisItem::setTickSpacing(std::optional<double> major, std::optional<double> minor) {
    if (major) {
        tickSpacing_ = QList<TickLevel>{{*major, 0}, {minor.value_or(*major), 0}};
    } else {
        tickSpacing_.reset();
    }
    update();
}

void AxisItem::setLabel(const QString& text, const QString& units, const std::optional<Style>& style) {
    labelText_ = text;
    labelUnits_ = units;
    if (style && !style->isEmpty()) {
        labelStyle_ = *style;
    }
    showLabel(!text.isEmpty() || !units.isEmpty());
    updateLabel();
}

void AxisItem::showLabel(bool show) {
    labelShown_ = show;
    label_->setVisible(show);
    adjustSize();
}

void AxisItem::updateLabel() {
    // pyqtgraph's labelString: "<span style='...'>text (units)</span>", a space before the (empty) units too
    const QString units = labelUnits_.isEmpty() ? QString() : QStringLiteral("(%1)").arg(labelUnits_);
    QStringList style;
    for (const auto& [key, value] : labelStyle_) {
        style.append(key + QStringLiteral(": ") + value);
    }
    label_->setHtml(QStringLiteral("<span style='%1'>%2 %3</span>").arg(style.join(u';'), labelText_, units));
    adjustSize();
    update();
}

void AxisItem::setShown(bool show) {
    shown_ = show;
    setVisible(show);
    adjustSize();
}

void AxisItem::adjustSize() {
    if (vertical()) {
        updateWidth();
    } else {
        updateHeight();
    }
}

void AxisItem::updateHeight() {
    double h = 0;
    if (shown_) {
        h = textHeight_ + tickTextOffset_[1] + std::max(0, tickLength_);
        if (labelShown_) {
            h += label_->boundingRect().height() * 0.8;
        }
    }
    setMaximumHeight(h);
    setMinimumHeight(h);
}

void AxisItem::updateWidth() {
    double w = 0;
    if (shown_) {
        w = textWidth_ + tickTextOffset_[0] + std::max(0, tickLength_);
        if (labelShown_) {
            // (the bounding rect is usually an overestimate)
            w += label_->boundingRect().height() * 0.8;
        }
    }
    setMaximumWidth(w);
    setMinimumWidth(w);
}

// the room the tick numbers took across the axis: the axis grows to it, or shrinks when it's 10 px less
void AxisItem::updateMaxTextSize(double size) {
    double& current = vertical() ? textWidth_ : textHeight_;
    if (size > current || size < current - 10) {
        current = size;
        adjustSize();
    }
}

void AxisItem::resizeEvent(QGraphicsSceneResizeEvent* event) {
    QGraphicsWidget::resizeEvent(event);
    constexpr double nudge = 5;
    const QRectF br = label_->boundingRect();
    QPointF p(0, 0);
    const QSizeF size = this->size();
    switch (orientation_) {
        case Left:
            p = QPointF(-nudge, static_cast<int>(size.height() / 2 + br.width() / 2));
            break;
        case Right:
            p = QPointF(static_cast<int>(size.width() - br.height() + nudge),
                        static_cast<int>(size.height() / 2 + br.width() / 2));
            break;
        case Top:
            p = QPointF(static_cast<int>(size.width() / 2. - br.width() / 2.), -nudge);
            break;
        case Bottom:
            p = QPointF(static_cast<int>(size.width() / 2. - br.width() / 2.),
                        static_cast<int>(size.height() - br.height() + nudge));
            break;
    }
    label_->setPos(p);
}

QRectF AxisItem::boundingRect() const {
    const QRectF rect = QRectF(QPointF(0, 0), size());
    if (view_ == nullptr || !grid_) {
        const double m = hideOverlappingLabels_ ? 0 : 15;
        const double tl = tickLength_;
        switch (orientation_) {
            case Left:
                return rect.adjusted(0, -m, -std::min(0.0, tl), m);
            case Right:
                return rect.adjusted(std::min(0.0, tl), -m, 0, m);
            case Top:
                return rect.adjusted(-m, 0, m, -std::min(0.0, tl));
            case Bottom:
                return rect.adjusted(-m, std::min(0.0, tl), m, 0);
        }
    }
    return rect | view_->mapRectToItem(this, view_->boundingRect());
}

QList<AxisItem::TickLevel> AxisItem::tickSpacing(double minVal, double maxVal, double size) const {
    if (tickSpacing_) {
        return *tickSpacing_;
    }
    const double dif = std::abs(maxVal - minVal);
    if (dif == 0) {
        return {};
    }
    // axes longer than 300 px show more than the fewest major ticks (sub-linearly with the length)
    const double minNumberOfIntervals = std::max(2.25, 2.25 * std::sqrt(size / 300.));
    const double majorMaxSpacing = dif / minNumberOfIntervals;
    int exp2 = 0;
    std::frexp(majorMaxSpacing, &exp2);
    // a power of ten just under it, one more down so the scale factors are whole numbers 5 or more
    double p10unit = std::pow(10., std::floor((exp2 - 1) / 3.32192809488736) - 1);
    int majorScaleFactor = 10;
    if (100. * p10unit <= majorMaxSpacing) {
        majorScaleFactor = 10;
        p10unit *= 10.;
    } else {
        for (const int factor : {50, 20, 10}) {
            majorScaleFactor = factor;
            if (factor * p10unit <= majorMaxSpacing) {
                break;
            }
        }
    }
    const double majorInterval = majorScaleFactor * p10unit;
    // no more than one minor tick per two pixels
    const double minorMinSpacing = 2 * dif / size;
    const QList<int> minorTrials = majorScaleFactor == 10 ? QList<int>{5, 10} : QList<int>{10, 20, 50};
    double minorInterval = 0;
    for (const int factor : minorTrials) {
        minorInterval = factor * p10unit;
        if (minorInterval >= minorMinSpacing) {
            break;
        }
    }
    QList<TickLevel> levels{{majorInterval, 0}, {minorInterval, 0}};
    if constexpr (kMaxTickLevel >= 2) {
        QList<int> trials;
        if (majorScaleFactor == 10) {
            trials = {1, 2, 5, 10};
        } else if (majorScaleFactor == 20) {
            trials = {2, 5, 10, 20};
        } else if (majorScaleFactor == 50) {
            trials = {5, 10, 50};
        }
        double extraInterval = minorInterval;
        for (const int factor : trials) {
            extraInterval = factor * p10unit;
            if (extraInterval >= minorMinSpacing || extraInterval == minorInterval) {
                break;
            }
        }
        if (extraInterval < minorInterval) {
            levels.append({extraInterval, 0});
        }
    }
    return levels;
}

QList<AxisItem::Ticks> AxisItem::tickValues(double minVal, double maxVal, double size) const {
    if (minVal > maxVal) {
        std::swap(minVal, maxVal);
    }
    QList<Ticks> ticks;
    QList<double> allValues;
    for (const TickLevel& level : tickSpacing(minVal, maxVal, size)) {
        const double start = std::ceil((minVal - level.offset) / level.spacing) * level.spacing + level.offset;
        const int num = static_cast<int>((maxVal - start) / level.spacing) + 1;
        QList<double> values;
        for (int i = 0; i < num; ++i) {
            const double value = i * level.spacing + start;
            // (each value once: on the first level that has it)
            const bool close = std::any_of(allValues.begin(), allValues.end(), [&](double other) {
                return std::abs(other - value) <= level.spacing * 0.01;
            });
            if (!close) {
                values.append(value);
            }
        }
        allValues.append(values);
        ticks.append({level.spacing, values});
    }
    return ticks;
}

QStringList AxisItem::tickStrings(const QList<double>& values, double scale, double spacing) const {
    const int places = std::max(0, static_cast<int>(std::ceil(-std::log10(spacing * scale))));
    QStringList strings;
    for (const double v : values) {
        const double vs = v * scale;
        if (std::abs(vs) < .001 || std::abs(vs) >= 10000) {
            strings.append(py::formatG(vs));
        } else {
            strings.append(QString::number(vs, 'f', places));
        }
    }
    return strings;
}

// pyqtgraph's generateDrawSpecs: where the axis line, each tick (or grid line) and each tick number go
std::optional<AxisItem::DrawSpecs> AxisItem::generateDrawSpecs(const QFont& font) {
    const QRectF bounds(QPointF(0, 0), size());
    const QRectF tickBounds =
        view_ == nullptr || !grid_ ? bounds : view_->mapRectToItem(this, view_->boundingRect());
    QPointF span0, span1;
    double tickStart = 0, tickStop = 0;
    int tickDir = 1;
    int axis = 0;
    switch (orientation_) {
        case Left:
            span0 = bounds.topRight() + QPointF(-1, -1);
            span1 = bounds.bottomRight() + QPointF(-1, 1);
            tickStart = tickBounds.right();
            tickStop = bounds.right();
            tickDir = -1;
            axis = 0;
            break;
        case Right:
            span0 = bounds.topLeft() + QPointF(1, -1);
            span1 = bounds.bottomLeft() + QPointF(1, 1);
            tickStart = tickBounds.left();
            tickStop = bounds.left();
            tickDir = 1;
            axis = 0;
            break;
        case Top:
            span0 = bounds.bottomLeft() + QPointF(-1, -1);
            span1 = bounds.bottomRight() + QPointF(1, -1);
            tickStart = tickBounds.bottom();
            tickStop = bounds.bottom();
            tickDir = -1;
            axis = 1;
            break;
        case Bottom:
            span0 = bounds.topLeft() + QPointF(-1, 1);
            span1 = bounds.topRight() + QPointF(1, 1);
            tickStart = tickBounds.top();
            tickStop = bounds.top();
            tickDir = 1;
            axis = 1;
            break;
    }
    const QTransform toDevice = sceneTransform();
    const QPointF deviceSpan = toDevice.map(span1) - toDevice.map(span0);
    const double lengthInPixels = std::hypot(deviceSpan.x(), deviceSpan.y());
    if (lengthInPixels == 0) {
        return std::nullopt;
    }
    const QList<Ticks> tickLevels = tickValues(range_.min, range_.max, lengthInPixels);

    const double dif = range_.span();
    double xScale = 1, offset = 0;
    if (dif != 0) {
        if (axis == 0) {
            xScale = -bounds.height() / dif;
            offset = range_.min * xScale - bounds.height();
        } else {
            xScale = bounds.width() / dif;
            offset = range_.min * xScale;
        }
    }
    const double r0 = range_.min * xScale - offset;
    const double r1 = range_.max * xScale - offset;
    const double xMin = std::min(r0, r1);
    const double xMax = std::max(r0, r1);

    DrawSpecs specs;
    QList<QList<std::optional<double>>> tickPositions;
    for (int i = 0; i < tickLevels.size(); ++i) {
        tickPositions.append(QList<std::optional<double>>());
        const QList<double>& ticks = tickLevels[i].values;
        const double tickLength = tickLength_ / ((i * 0.5) + 1.0);
        double lineAlpha = 255. / (i + 1);
        if (grid_) {
            lineAlpha *= *grid_ / 255. * std::clamp(0.05 * lengthInPixels / (ticks.size() + 1), 0., 1.);
        }
        QPen tickPen = this->tickPen();
        if (tickPen.brush().style() == Qt::SolidPattern) {
            QColor color = tickPen.color();
            color.setAlpha(static_cast<int>(lineAlpha));
            tickPen.setColor(color);
        }
        for (const double v : ticks) {
            const double x = v * xScale - offset;
            if (x < xMin || x > xMax) {
                tickPositions[i].append(std::nullopt);
                continue;
            }
            tickPositions[i].append(x);
            double p1[2] = {x, x};
            double p2[2] = {x, x};
            p1[axis] = tickStart;
            p2[axis] = tickStop;
            if (!grid_) {
                p2[axis] += tickLength * tickDir;
            }
            specs.ticks.append({tickPen, QPointF(p1[0], p1[1]), QPointF(p2[0], p2[1])});
        }
    }
    specs.axisPen = pen_;
    specs.axisStart = span0;
    specs.axisEnd = span1;

    const int textOffset = tickTextOffset_[axis];
    const QFontMetricsF metrics(font);
    double lastTextSize2 = 0;
    QList<QRectF> textRects;
    const QRectF br = boundingRect();
    for (int i = 0; i < std::min<qsizetype>(tickLevels.size(), kMaxTextLevel + 1); ++i) {
        QStringList strings = tickStrings(tickLevels[i].values, 1.0, tickLevels[i].spacing);
        if (strings.isEmpty()) {
            continue;
        }
        QList<std::optional<QRectF>> rects;
        for (int j = 0; j < strings.size(); ++j) {
            if (!tickPositions[i][j]) {
                rects.append(std::nullopt);
                continue;
            }
            QRectF rect = metrics.boundingRect(QRectF(0, 0, 100, 100), Qt::AlignCenter, strings[j]);
            rect.setHeight(rect.height() * 0.8);
            rects.append(rect);
            textRects.append(rect);
        }
        double textSize = 0, textSize2 = 0;
        for (const QRectF& rect : textRects) {
            textSize += axis == 0 ? rect.height() : rect.width();
            textSize2 = std::max(textSize2, axis == 0 ? rect.width() : rect.height());
        }
        if (i > 0) {
            // the first level always has its numbers; the others while there's room for them
            const double textFillRatio = textSize / lengthInPixels;
            bool finished = false;
            for (const auto& [nTexts, limit] : kTextFillLimits) {
                if (specs.texts.size() >= nTexts && textFillRatio >= limit) {
                    finished = true;
                    break;
                }
            }
            if (finished) {
                break;
            }
        }
        lastTextSize2 = textSize2;
        for (int j = 0; j < strings.size(); ++j) {
            if (!rects[j]) {
                continue;
            }
            const double x = *tickPositions[i][j];
            const double height = rects[j]->height();
            const double width = rects[j]->width();
            const double textAt = std::max(0, tickLength_) + textOffset;
            QRectF rect;
            int alignFlags = 0;
            switch (orientation_) {
                case Left:
                    alignFlags = Qt::AlignRight | Qt::AlignVCenter;
                    rect = QRectF(tickStop - textAt - width, x - height / 2, width, height);
                    break;
                case Right:
                    alignFlags = Qt::AlignLeft | Qt::AlignVCenter;
                    rect = QRectF(tickStop + textAt, x - height / 2, width, height);
                    break;
                case Top:
                    alignFlags = Qt::AlignHCenter | Qt::AlignBottom;
                    rect = QRectF(x - width / 2., tickStop - textAt - height, width, height);
                    break;
                case Bottom:
                    alignFlags = Qt::AlignHCenter | Qt::AlignTop;
                    rect = QRectF(x - width / 2., tickStop + textAt, width, height);
                    break;
            }
            if (!br.contains(rect)) {
                continue;
            }
            specs.texts.append({rect, alignFlags | Qt::TextDontClip, strings[j]});
        }
    }
    updateMaxTextSize(lastTextSize2);
    return specs;
}

void AxisItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    const QFont font = tickFont_.value_or(QFont());
    const std::optional<DrawSpecs> specs = generateDrawSpecs(font);
    if (!specs) {
        return;
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, false);
    painter->setRenderHint(QPainter::TextAntialiasing, true);
    painter->setPen(specs->axisPen);
    painter->drawLine(specs->axisStart, specs->axisEnd);
    for (const auto& [pen, p1, p2] : specs->ticks) {
        painter->setPen(pen);
        painter->drawLine(p1, p2);
    }
    painter->setFont(font);
    painter->setPen(textPen_);
    painter->setClipRect(boundingRect().toAlignedRect(), Qt::IntersectClip);
    for (const auto& [rect, flags, text] : specs->texts) {
        painter->drawText(rect, flags, text);
    }
    painter->restore();
}

}  // namespace awo::plot

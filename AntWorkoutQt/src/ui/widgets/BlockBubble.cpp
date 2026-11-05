#include "BlockBubble.hpp"

#include <QFontMetricsF>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <utility>

#include "MaxStar.hpp"
#include "PyCompat.hpp"
#include "Theme.hpp"

namespace awo {

namespace {

QString value(std::optional<double> v, int decimals, const QString& unit) {
    return (v ? py::fixed(*v, decimals) : QStringLiteral("--")) + unit;
}

}  // namespace

BlockBubble::BlockBubble(double start, double end, const QList<Row>& rows, double target, const plot::ViewBox* timeBox,
                         std::function<double()> rideSeconds)
    : start_(start), end_(end), rows_(rows), target_(target), timeBox_(timeBox), rideSeconds_(std::move(rideSeconds)) {
    setAcceptedMouseButtons(Qt::NoButton);
    // (under the max stars and their bubbles)
    setZValue(990);
    setVisible(false);
    for (const Row& row : rows_) {
        cells_.append({row.name, value(row.avg, row.decimals, row.unit)});
    }
    fit();
}

QStringList BlockBubble::lines() const {
    QStringList lines;
    for (const QStringList& cells : cells_) {
        lines.append(cells.join(u' '));
    }
    return lines;
}

// the lines in columns, the numbers right aligned, a space apart
void BlockBubble::fit() {
    prepareGeometryChange();
    font_ = MaxBubble::textFont();
    const QFontMetricsF metrics(font_);
    lineHeight_ = metrics.height();
    columns_ = {0, 0};
    for (const QStringList& cells : cells_) {
        for (int i = 0; i < 2; ++i) {
            columns_[i] = std::max(columns_[i], metrics.horizontalAdvance(cells[i]));
        }
    }
    const double space = metrics.horizontalAdvance(u' ');
    width_ = columns_[0] + columns_[1] + space + 2 * kPad * lineHeight_;
    height_ = cells_.size() * lineHeight_ + 2 * kPad * lineHeight_;
}

QRectF BlockBubble::boundingRect() const {
    const QRectF rect(-width_ / 2 - 1, -height_ / 2 - 1, width_ + 2, height_ + 2);
    if (!popAt_) {
        return rect;
    }
    const double margin = soap::popMargin(rect);
    return rect.adjusted(-margin, -margin, margin, margin);
}

// a soap bubble as the max bubbles are: the theme's plot label background, its text in the theme's black
void BlockBubble::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    const QRectF rect(-width_ / 2, -height_ / 2, width_, height_);
    const soap::Content text = [this, rect](QPainter* p) {
        p->setPen(QColor(theme::current().mapHex(QStringLiteral("#000000"))));
        p->setFont(font_);
        const double space = QFontMetricsF(font_).horizontalAdvance(u' ');
        const double pad = kPad * lineHeight_;
        for (qsizetype line = 0; line < cells_.size(); ++line) {
            double x = rect.left() + pad;
            const double y = rect.top() + pad + line * lineHeight_;
            for (int i = 0; i < 2; ++i) {
                const Qt::Alignment align = (i == 1 ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter;
                p->drawText(QRectF(x, y, columns_[i], lineHeight_), align, cells_[line][i]);
                x += columns_[i] + space;
            }
        }
    };
    const QRectF film = rect.adjusted(0.5, 0.5, -0.5, -0.5);
    const double corner = lineHeight_ * kCorner;
    if (popAt_) {
        const double t = std::clamp((now_ - *popAt_) / kPopSeconds, 0.0, 1.0);
        soap::paintPop(painter, film, corner, now_, t, static_cast<unsigned>(start_ * 1000) + pops_, text);
    } else {
        soap::paintFilm(painter, film, corner, now_, text);
    }
}

double BlockBubble::sceneX(double seconds) const {
    return timeBox_->mapViewToScene(QPointF(seconds, 0)).x();
}

// the middle of its block (see findSpot for where else), kept on the graph (so moved right as a long block's middle goes
// off it) and from reaching more than kMaxAheadSeconds past the time now
double BlockBubble::naturalLeft() const {
    return (sceneX(start_) + sceneX(end_)) / 2 - width_ / 2 + shift_ * (sceneX(1) - sceneX(0));
}

double BlockBubble::left() const {
    const QRectF area = timeBox_->sceneBoundingRect();
    const double right = std::min(area.right(), sceneX(rideSeconds_() + kMaxAheadSeconds));
    return std::min(std::max(naturalLeft(), area.left()), right - width_);
}

// its block's end on the graph, and once the graph scrolls, either where it's placed in its block (one narrower than it
// too) or moved right and still inside its block
bool BlockBubble::onGraph() const {
    const QRectF area = timeBox_->sceneBoundingRect();
    if (sceneX(end_) > area.right()) {
        return false;
    }
    return timeBox_->xRange().min <= 0 || naturalLeft() >= area.left() || area.left() + width_ <= sceneX(end_);
}

// its bottom edge lift_ over the graph's bottom
QRectF BlockBubble::placed() const {
    const QRectF area = timeBox_->sceneBoundingRect();
    return {left(), std::max(area.top(), area.bottom() - lift_ - height_), width_, height_};
}

// the first spot clear of obstacles: the middle of its block on the graph's bottom, else along the bottom of its block
// (one narrower than it: only its middle), else the same higher up
bool BlockBubble::findSpot(const QList<QRectF>& obstacles) {
    const QRectF area = timeBox_->sceneBoundingRect();
    shift_ = 0;
    lift_ = 0;
    const double middle = naturalLeft();
    const double base = left();
    const double lowest = std::max(area.left(), sceneX(start_));
    const double highest = std::min({area.right(), sceneX(end_), sceneX(rideSeconds_() + kMaxAheadSeconds)}) - width_;
    QList<double> lefts{base};
    for (double step = width_ / 2; step > 0 && (base + step <= highest || base - step >= lowest); step += width_ / 2) {
        if (base + step <= highest) {
            lefts.append(base + step);
        }
        if (base - step >= lowest) {
            lefts.append(base - step);
        }
    }
    const double perSecond = sceneX(1) - sceneX(0);
    for (double lift = 0; area.bottom() - lift - height_ >= area.top(); lift += height_ + kGap) {
        for (const double x : lefts) {
            const QRectF rect(x, area.bottom() - lift - height_, width_, height_);
            const QRectF clear = rect.adjusted(-kClear, -kClear, kClear, kClear);
            if (std::none_of(obstacles.begin(), obstacles.end(), [&](const QRectF& other) { return clear.intersects(other); })) {
                shift_ = x == base || perSecond <= 0 ? 0.0 : (x - middle) / perSecond;
                lift_ = lift;
                return true;
            }
        }
    }
    return false;
}

// where it's placed; one that a rect in avoid touches pops, and comes in again where findSpot says (hidden while there's
// no spot, or it can't be kept on the graph: the time now too near the workout's start)
void BlockBubble::step(double now, QList<QRectF>& avoid, const QList<QRectF>& keepOff) {
    // (again in a new theme's font or scale)
    if (MaxBubble::textFont() != font_) {
        fit();
    }
    now_ = now;
    if (replacing_) {
        leave(now);
        return;
    }
    if (left() < timeBox_->sceneBoundingRect().left() - 0.5) {
        hide();
        return;
    }
    if (popAt_) {
        // (picked again mid-pop: a new one)
        prepareGeometryChange();
        popAt_.reset();
    }
    if (appearAt_) {
        const QRectF rect = placed();
        if (std::any_of(avoid.begin(), avoid.end(), [&](const QRectF& other) { return rect.intersects(other); })) {
            replacing_ = true;
            leave(now);
            return;
        }
    } else {
        if (!findSpot(avoid + keepOff)) {
            hide();
            return;
        }
        appearAt_ = now;
    }
    const QRectF rect = placed();
    rect_ = rect;
    avoid.append(rect);
    setPos(rect.center());
    // grows in, a little past its size and back (ease out back), as a max bubble does
    const double t = std::clamp((now - *appearAt_) / kAppearSeconds, 0.0, 1.0);
    constexpr double back = 1.7;
    setScale(0.5 + 0.5 * (1 + (back + 1) * std::pow(t - 1, 3) + back * std::pow(t - 1, 2)));
    setOpacity(std::min(1.0, 2 * t));
    setVisible(true);
    // (its colors drift)
    update();
}

// where it was, bursting (see soap::paintPop); hidden once it's gone
void BlockBubble::leave(double now) {
    now_ = now;
    if (!popAt_) {
        if (!isVisible()) {
            hide();
            return;
        }
        prepareGeometryChange();
        popAt_ = now;
        ++pops_;
        appearAt_.reset();
        setScale(1.0);
        setOpacity(1.0);
    }
    // (or stepped by another clock since)
    if (now - *popAt_ >= kPopSeconds || now < *popAt_) {
        hide();
        return;
    }
    update();
}

void BlockBubble::hide() {
    if (popAt_) {
        prepareGeometryChange();
        popAt_.reset();
    }
    setVisible(false);
    appearAt_.reset();
    replacing_ = false;
}

BlockBubble* BlockBubble::pick(const QList<BlockBubble*>& bubbles, double target, std::optional<double> upcoming) {
    BlockBubble* latest = nullptr;
    BlockBubble* sameTarget = nullptr;
    for (auto it = bubbles.rbegin(); it != bubbles.rend(); ++it) {
        BlockBubble* bubble = *it;
        if (!bubble->onGraph()) {
            continue;
        }
        if (!latest) {
            latest = bubble;
        }
        if (upcoming && bubble->target_ == *upcoming) {
            return bubble;
        }
        if (!sameTarget && bubble->target_ == target) {
            sameTarget = bubble;
        }
    }
    return sameTarget ? sameTarget : latest;
}

void BlockBubble::stepAll(const QList<BlockBubble*>& bubbles, double target, double now, QList<QRectF>& avoid,
                          std::optional<double> upcoming, const QList<QRectF>& keepOff) {
    BlockBubble* shown = pick(bubbles, target, upcoming);
    for (BlockBubble* bubble : bubbles) {
        if (bubble != shown) {
            bubble->leave(now);
        }
    }
    if (shown) {
        shown->step(now, avoid, keepOff);
    }
}

}  // namespace awo

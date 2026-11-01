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

QString value(std::optional<double> v, const QString& unit) {
    return (v ? py::fixed(*v, 0) : QStringLiteral("--")) + unit;
}

// the max bubbles' font: the theme's plot label font, sized to their height
QFont bubbleFont() {
    QFont font(QStringLiteral("Orbitron"));
    font.setBold(true);
    font = theme::current().mapFont(font);
    const double height = MaxStar::kRadius * theme::uiScale() * (1 + MaxStar::kBelow);
    font.setPixelSize(std::max(6, static_cast<int>(py::round(height * 0.72))));
    return font;
}

}  // namespace

BlockBubble::BlockBubble(double start, double end, const QList<Row>& rows, const LineEnd& target,
                         const QList<LineEnd>& curveEnds, const plot::ViewBox* timeBox, std::function<double()> rideSeconds)
    : start_(start), end_(end), rows_(rows), target_(target), ends_(QList<LineEnd>{target} + curveEnds),
      timeBox_(timeBox), rideSeconds_(std::move(rideSeconds)), connector_(new QGraphicsLineItem(this)) {
    setAcceptedMouseButtons(Qt::NoButton);
    // (under the max stars and their bubbles; its line under it)
    setZValue(990);
    setVisible(false);
    connector_->setFlag(QGraphicsItem::ItemStacksBehindParent);
    connector_->setVisible(false);
    for (const Row& row : rows_) {
        cells_.append({row.name, value(row.avg, row.unit)});
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
    font_ = bubbleFont();
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

// its block's end on the graph
bool BlockBubble::onGraph() const {
    const QRectF area = timeBox_->sceneBoundingRect();
    const double x = sceneX(end_);
    return x >= area.left() && x <= area.right();
}

// under the lowest line end, or over the highest when there's no room under it (above: whether over them)
double BlockBubble::top(bool* above) const {
    const QRectF area = timeBox_->sceneBoundingRect();
    double lowest = area.top(), highest = area.bottom();
    for (const auto& [vbox, y] : ends_) {
        const double sceneY = vbox->mapViewToScene(QPointF(end_, y)).y();
        lowest = std::max(lowest, sceneY);
        highest = std::min(highest, sceneY);
    }
    double top = lowest + kGap;
    const bool over = top + height_ > area.bottom();
    if (over) {
        top = std::max(area.top(), highest - kGap - height_);
    }
    if (above) {
        *above = over;
    }
    return top;
}

std::optional<BlockBubble::Room> BlockBubble::room() const {
    bool above = false;
    top(&above);
    if (!isVisible() || !above) {
        return std::nullopt;
    }
    // the highest line end's
    const LineEnd* highest = &ends_.front();
    double highestY = highest->first->mapViewToScene(QPointF(end_, highest->second)).y();
    for (const LineEnd& lineEnd : ends_) {
        const double y = lineEnd.first->mapViewToScene(QPointF(end_, lineEnd.second)).y();
        if (y < highestY) {
            highest = &lineEnd;
            highestY = y;
        }
    }
    // (and a pixel for its outline)
    return Room{highest->first, kGap + height_ + 1, highest->second};
}

// centered on its block's end, kept on the graph and from reaching more than kMaxAheadSeconds past the time now
// (hidden while it can't be: the time now too near the workout's start)
void BlockBubble::step(double now, QList<QRectF>& avoid) {
    // (again in a new theme's font or scale)
    if (bubbleFont() != font_) {
        fit();
    }
    const QRectF area = timeBox_->sceneBoundingRect();
    const double right = std::min(area.right(), sceneX(rideSeconds_() + kMaxAheadSeconds));
    const double left = std::min(std::max(sceneX(end_) - width_ / 2, area.left()), right - width_);
    if (left < area.left() - 0.5) {
        hide();
        return;
    }
    const QRectF rect(left, top(), width_, height_);
    now_ = now;
    if (popAt_) {
        // (picked again mid-pop: a new one)
        prepareGeometryChange();
        popAt_.reset();
    }
    if (!appearAt_) {
        appearAt_ = now;
    }
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
    connect(rect);
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
        connector_->setVisible(false);
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
}

// a thin line from it to the top of its block, unless it's under the top of its block already: diagonal (45 degrees,
// as far as its edge allows), to stand out from the graph's upright grid lines and bar edges
void BlockBubble::connect(const QRectF& rect) {
    const QRectF area = timeBox_->sceneBoundingRect();
    const double blockLeft = std::max(sceneX(start_), area.left());
    const double blockRight = sceneX(end_);
    const double blockTop = target_.first->mapViewToScene(QPointF(end_, target_.second)).y();
    if (rect.right() > blockLeft && rect.left() < blockRight && rect.top() >= blockTop - 0.5) {
        connector_->setVisible(false);
        return;
    }
    QPointF to(std::clamp(rect.center().x(), blockLeft, blockRight), blockTop);
    // (from its nearest edge, clear of the rounded corners)
    const double corner = lineHeight_ * kCorner;
    const double inLeft = rect.left() + corner, inRight = rect.right() - corner;
    QPointF from;
    if (to.y() > rect.bottom() || to.y() < rect.top()) {
        const double edge = to.y() > rect.bottom() ? rect.bottom() : rect.top();
        const double rise = std::abs(to.y() - edge);
        // across toward the side of it with more room
        const double side = inRight - to.x() >= to.x() - inLeft ? 1 : -1;
        from = {std::clamp(to.x() + side * rise, inLeft, inRight), edge};
        if (std::abs(from.x() - to.x()) < 1) {
            // (its block's top right under its middle and no room across: the block's end then)
            to.setX(to.x() == blockRight ? blockLeft : blockRight);
        }
    } else {
        from = {to.x() < rect.left() ? rect.left() : rect.right(), std::clamp(to.y() - corner, rect.top(), rect.bottom())};
    }
    QColor color(theme::current().mapHex(QStringLiteral("#000000")));
    color.setAlpha(215);
    QPen pen(color, 1);
    pen.setCosmetic(true);
    connector_->setPen(pen);
    connector_->setLine(QLineF(mapFromScene(from), mapFromScene(to)));
    connector_->setVisible(true);
}

BlockBubble* BlockBubble::pick(const QList<BlockBubble*>& bubbles, double target) {
    BlockBubble* latest = nullptr;
    for (auto it = bubbles.rbegin(); it != bubbles.rend(); ++it) {
        BlockBubble* bubble = *it;
        if (!bubble->onGraph()) {
            continue;
        }
        if (!latest) {
            latest = bubble;
        }
        if (bubble->target_.second == target) {
            return bubble;
        }
    }
    return latest;
}

void BlockBubble::stepAll(const QList<BlockBubble*>& bubbles, double target, double now, QList<QRectF>& avoid) {
    BlockBubble* shown = pick(bubbles, target);
    for (BlockBubble* bubble : bubbles) {
        if (bubble != shown) {
            bubble->leave(now);
        }
    }
    if (shown) {
        shown->step(now, avoid);
    }
}

}  // namespace awo

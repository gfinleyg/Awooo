#include "MaxStar.hpp"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "PyCompat.hpp"
#include "Theme.hpp"

namespace awo {

namespace {

double radians(double degrees) {
    return degrees * std::numbers::pi / 180;
}

// numpy's interp: ys at x along increasing xs, held at the ends
double interp(double x, const QList<double>& xs, const QList<double>& ys) {
    if (xs.isEmpty()) {
        return 0;
    }
    if (x <= xs.front()) {
        return ys.front();
    }
    if (x >= xs.back()) {
        return ys.back();
    }
    const auto upper = std::upper_bound(xs.begin(), xs.end(), x);
    const qsizetype i = upper - xs.begin();
    const double x0 = xs[i - 1], x1 = xs[i];
    if (x1 == x0) {
        return ys[i];
    }
    return ys[i - 1] + (ys[i] - ys[i - 1]) * (x - x0) / (x1 - x0);
}

QList<double> cumulativeLengths(const QList<double>& xs, const QList<double>& ys) {
    QList<double> lengths{0.0};
    for (qsizetype i = 1; i < xs.size(); ++i) {
        lengths.append(lengths.back() + std::hypot(xs[i] - xs[i - 1], ys[i] - ys[i - 1]));
    }
    return lengths;
}

}  // namespace

const double MaxStar::kBelow = std::sin(radians(54));
const QString MaxStar::kColor = QStringLiteral("#ffd23f");
const QString MaxStar::kEdgeColor = QStringLiteral("#3a2a00");

MaxStar::MaxStar(plot::CurveItem* curve, plot::ViewBox* vbox, std::function<plot::Range()> viewRange)
    : curve_(curve), vbox_(vbox), viewRange_(std::move(viewRange)), color_(kColor) {
    QPainterPath path;
    for (int i = 0; i < 10; ++i) {
        const double angle = -std::numbers::pi / 2 + i * std::numbers::pi / 5;
        const double radius = kRadius * (i % 2 ? kInner : 1);
        const QPointF point(radius * std::cos(angle), radius * std::sin(angle));
        if (i) {
            path.lineTo(point);
        } else {
            path.moveTo(point);
        }
    }
    path.closeSubpath();
    setPath(path);
    pen_ = QPen(QColor(kEdgeColor), kEdgeWidth);
    // (stays this wide while it twinkles)
    pen_.setCosmetic(true);
    setPen(pen_);
    setAcceptedMouseButtons(Qt::NoButton);
    // (over the synopsis' max labels too)
    setZValue(1001);
    setVisible(false);
}

void MaxStar::setColor(const QString& color) {
    if (color != color_) {
        color_ = color;
        update();
    }
}

// filled in the theme's color for its color, whatever theme is loaded now
void MaxStar::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(theme::themedPen(pen_));
    painter->setBrush(QColor(theme::current().mapHex(color_)));
    painter->drawPath(path());
}

// how far the curve's line (its halo included) reaches above the points it's drawn through, in pixels
double MaxStar::lineReach() const {
    double widest = 1.0;
    widest = std::max(widest, curve_->pen().widthF());
    if (curve_->shadowPen()) {
        widest = std::max(widest, curve_->shadowPen()->widthF());
    }
    return widest / 2;
}

double MaxStar::restOffset() const {
    const double below = kRadius * kBelow * theme::uiScale() * (1 + kTwinkle) + kEdgeWidth / 2;
    return below + std::max(lineReach(), markerReach) + kGap;
}

double MaxStar::headroom() const {
    return restOffset() + kRadius * theme::uiScale() * (1 + kTwinkle) + kEdgeWidth / 2 + kGap;
}

double MaxStar::halfWidth() const {
    return kRadius * theme::uiScale() * (1 + kTwinkle) * std::cos(radians(18)) + kEdgeWidth / 2;
}

QRectF MaxStar::restRect(const QPointF& point) const {
    const double radius = kRadius * theme::uiScale() * 1.3;
    const QPointF center(point.x(), point.y() - restOffset());
    return {center.x() - radius, center.y() - radius, 2 * radius, 2 * radius};
}

double MaxStar::topWithRoom(const plot::ViewBox* vbox, double top, const QList<std::pair<double, double>>& rooms,
                            double reserved) {
    // (the height the view's range is drawn over, in pixels)
    const double height = vbox->mapViewToScene(QPointF(0, 0)).y() -
                          vbox->mapViewToScene(QPointF(0, vbox->yRange().max)).y();
    for (const auto& [headroom, value] : rooms) {
        const double room = headroom + reserved;
        if (value > 0 && height > room) {
            top = std::max(top, value * height / (height - room));
        }
    }
    return top;
}

// smoothstep: its speed, 6t(1-t), is a parabola: still at both maxes, fastest halfway
double MaxStar::ease(double t) {
    return t * t * (3 - 2 * t);
}

// how far up toward its resting spot (1) from the line (0) it is: it hugs the line while travelling
double MaxStar::lift(double fromLift, double t) {
    return fromLift * std::pow(1 - t, 4) + std::pow(t, 4);
}

void MaxStar::goTo(double newX, double newY, double now) {
    if (!x) {
        x = newX;
        y = newY;
        tripFrom_ = at_ = newX;
        return;
    }
    const bool moved = newX != *x;
    x = newX;
    y = newY;
    if (moved) {
        // from wherever it is (mid-trip too), lifting off from as high as it was; longer trips on screen take longer
        const double t = progress(now);
        tripLift_ = t < 1 ? lift(tripLift_, t) : 1.0;
        tripFrom_ = at_;
        const auto [xs, ys] = tripPath();
        const QList<double> lengths = cumulativeLengths(xs, ys);
        tripSeconds_ = std::min(kTripMax, std::max(kTripMin, kTripSeconds + lengths.back() * kTripSecondsPerPx));
        tripStart_ = now;
    }
}

double MaxStar::progress(double now) const {
    return std::min(1.0, std::max(0.0, (now - tripStart_) / tripSeconds_));
}

// the view's data -> scene mapping as scene = origin + data * scale
MaxStar::Mapping MaxStar::viewToScene() const {
    const QPointF origin = vbox_->mapViewToScene(QPointF(0, 0));
    const QPointF unit = vbox_->mapViewToScene(QPointF(1, 1));
    return {origin.x(), unit.x() - origin.x(), origin.y(), unit.y() - origin.y()};
}

std::pair<QList<double>, QList<double>> MaxStar::tripPath() const {
    const double a = tripFrom_.value_or(*x);
    const double b = *x;
    const Mapping m = viewToScene();
    const QList<double>& xs = curve_->xData();
    const QList<double>& ys = curve_->yData();
    if (xs.isEmpty()) {
        return {{m.originX + b * m.scaleX}, {m.originY + *y * m.scaleY}};
    }
    const plot::Range inView = viewRange_();
    const double lo = std::max(std::min(a, b), inView.min);
    const double hi = std::min(std::max(a, b), inView.max);
    QList<double> times;
    if (hi <= lo) {
        times = {std::min(std::max(b, inView.min), inView.max)};
    } else {
        const auto first = std::upper_bound(xs.begin(), xs.end(), lo);
        const auto last = std::lower_bound(xs.begin(), xs.end(), hi);
        const qsizetype inner = std::max<qsizetype>(0, last - first);
        const qsizetype step =
            std::max<qsizetype>(1, static_cast<qsizetype>(inner / std::max(std::abs(m.scaleX) * (hi - lo), 1.0)));
        times.append(lo);
        for (qsizetype i = 0; i < inner; i += step) {
            times.append(*(first + i));
        }
        times.append(hi);
        if (a > b) {
            std::reverse(times.begin(), times.end());
        }
    }
    QList<double> sx, sy;
    for (const double t : times) {
        sx.append(m.originX + t * m.scaleX);
        sy.append(m.originY + interp(t, xs, ys) * m.scaleY);
    }
    return {sx, sy};
}

QList<QRectF> MaxStar::tripClearance(double now) const {
    QList<QRectF> rects;
    if (!x || !onGraph || progress(now) >= 1) {
        return rects;
    }
    const auto [xs, ys] = tripPath();
    const QList<double> lengths = cumulativeLengths(xs, ys);
    const double along = ease(progress(now)) * lengths.back();
    const double width = halfWidth();
    const double radius = kRadius * theme::uiScale() * (1 + kTwinkle) + kEdgeWidth / 2;
    const double above = restOffset() + radius;
    double next = along;
    for (qsizetype i = 0; i < xs.size(); ++i) {
        // (one a half width or so apart, and its end)
        if ((lengths[i] >= along && lengths[i] >= next) || i == xs.size() - 1) {
            rects.append(QRectF(xs[i] - width, ys[i] - above, 2 * width, above + radius));
            next = lengths[i] + width;
        }
    }
    return rects;
}

void MaxStar::step(double now, int phase) {
    if (!x || !onGraph) {
        setVisible(false);
        return;
    }
    const double target = restOffset();
    offset = offset ? *offset + (target - *offset) * 0.3 : target;
    const double t = progress(now);
    QPointF point;
    double lifted = 1.0;
    if (t < 1) {
        // as far along the curve on screen as smoothstep is along the trip
        const auto [xs, ys] = tripPath();
        const QList<double> lengths = cumulativeLengths(xs, ys);
        const double along = ease(t) * lengths.back();
        point = QPointF(interp(along, lengths, xs), interp(along, lengths, ys));
        lifted = lift(tripLift_, t);
        setScale(theme::uiScale());
        const Mapping m = viewToScene();
        at_ = (point.x() - m.originX) / m.scaleX;
    } else {
        point = vbox_->mapViewToScene(QPointF(*x, *y));
        setScale(theme::uiScale() * (1 + kTwinkle * std::sin(now * 3.6 + phase * 2.1)));
        at_ = x;
    }
    // (no higher than the graph's top, should its owner not have made room for it there)
    const QRectF area = vbox_->sceneBoundingRect();
    const double top = std::max(point.y() - *offset * lifted, area.top() + kRadius * theme::uiScale());
    setPos(point.x(), top);
    setVisible(area.contains(point));
}

// ---- MaxBubble ----

MaxBubble::MaxBubble(MaxStar* star, const QString& format) : star_(star), format_(format) {
    setAcceptedMouseButtons(Qt::NoButton);
    // (just under the stars)
    setZValue(1000);
    setVisible(false);
}

QRectF MaxBubble::boundingRect() const {
    const QRectF rect(-width_ / 2 - 1, -height_ / 2 - 1, width_ + 2, height_ + 2);
    if (state_ != State::Popping) {
        return rect;
    }
    const double margin = soap::popMargin(rect);
    return rect.adjusted(-margin, -margin, margin, margin);
}

// a soap bubble in the theme's plot label background, its text in the theme's black
void MaxBubble::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    const QRectF rect(-width_ / 2, -height_ / 2, width_, height_);
    const soap::Content text = [this, rect](QPainter* p) {
        p->setPen(QColor(theme::current().mapHex(QStringLiteral("#000000"))));
        p->setFont(font_);
        p->drawText(rect, Qt::AlignCenter, text_);
    };
    const QRectF film = rect.adjusted(0.5, 0.5, -0.5, -0.5);
    if (state_ == State::Popping) {
        soap::paintPop(painter, film, height_ * kCorner, now_, popT_, static_cast<unsigned>(qHash(text_)) + pops_, text);
    } else {
        soap::paintFilm(painter, film, height_ * kCorner, now_, text);
    }
}

QFont MaxBubble::textFont() {
    QFont font(QStringLiteral("Orbitron"));
    font.setBold(true);
    font = theme::current().mapFont(font);
    const double height = MaxStar::kRadius * theme::uiScale() * (1 + MaxStar::kBelow);
    font.setPixelSize(std::max(kMinTextPixels, static_cast<int>(py::round(height * 0.72))));
    return font;
}

double MaxBubble::fittedHeight() {
    return std::max(MaxStar::kRadius * theme::uiScale() * (1 + MaxStar::kBelow),
                    QFontMetricsF(textFont()).height() + 2 * kTextMargin);
}

void MaxBubble::fit(const QString& text) {
    prepareGeometryChange();
    text_ = text;
    height_ = fittedHeight();
    font_ = textFont();
    width_ = QFontMetricsF(font_).horizontalAdvance(text) + 2 * kPad * height_;
}

double MaxBubble::headroom() const {
    const double scale = MaxStar::kRadius * theme::uiScale();
    const double above = star_->restOffset() - scale * (MaxStar::kBelow - 1) / 2 + (1 + kPopGrowth) * fittedHeight() / 2;
    return std::max(star_->headroom(), above + 1 + MaxStar::kGap);
}

double MaxBubble::topRowHeight() {
    return MaxStar::kRadius * theme::uiScale() * (1 + MaxStar::kBelow) + 2 * kGap + kClear;
}

bool MaxBubble::needsTopRow() const {
    return star_->x && star_->onGraph && roomFor_.first && star_->x == roomFor_.first && star_->y == roomFor_.second;
}

void MaxBubble::step(double now, QList<QRectF>& avoid, const QList<QRectF>& keepOff) {
    MaxStar* star = star_;
    if (!star->x || !star->onGraph) {
        state_ = State::Hidden;
        setVisible(false);
        return;
    }
    now_ = now;
    // (again in a new theme's font or scale)
    if (!text_.isEmpty() && textFont() != font_) {
        fit(text_);
    }
    const QRectF area = star->vbox()->sceneBoundingRect();
    const double scale = MaxStar::kRadius * theme::uiScale();
    // where the star rests over the max it's beside now (held down from the graph's top as the star is, see
    // MaxStar::step)
    const auto starAt = [&](const QPointF& point) {
        return QPointF(point.x(), std::max(point.y() - star->offset.value_or(star->restOffset()), area.top() + scale));
    };
    // its rect at offset from level with that star, the star's height top to bottom; in the top row: offset across
    // from the star, kGap under the graph's top
    const auto rectAt = [&](const QPointF& point, const QPointF& offset, bool topRow) {
        if (topRow) {
            return QRectF(point.x() + offset.x() - width_ / 2, area.top() + kGap, width_, height_);
        }
        const QPointF center = starAt(point) + QPointF(0, scale * (MaxStar::kBelow - 1) / 2) + offset;
        return QRectF(center.x() - width_ / 2, center.y() - height_ / 2, width_, height_);
    };
    const auto touches = [](const QRectF& rect, const QList<QRectF>& others) {
        return std::any_of(others.begin(), others.end(), [&](const QRectF& other) { return rect.intersects(other); });
    };
    const QRectF inArea = area.adjusted(-0.5, -0.5, 0.5, 0.5);
    if (state_ == State::Appearing || state_ == State::Shown) {
        // its max moved, the graph's edge reached, or a star or a bubble placed before it touching it: pops, and a new
        // one comes in clear of them
        const QRectF rect = rectAt(star->vbox()->mapViewToScene(QPointF(*x_, *y_)), offset_, topRow_);
        if (star->x != x_ || star->y != y_ || rect.left() <= area.left() || !inArea.contains(rect) || touches(rect, avoid)) {
            // (room for its spray while it pops)
            prepareGeometryChange();
            state_ = State::Popping;
            stateStart_ = now;
            ++pops_;
        }
    }
    if (state_ == State::Popping && now - stateStart_ >= kPopSeconds) {
        prepareGeometryChange();
        state_ = State::Hidden;
    }
    if (state_ == State::Hidden) {
        if (star->progress(now) < 1) {
            setVisible(false);
            return;
        }
        // the star has arrived: a new one over its max, beside the star if it can be, else over, under or to a corner
        // of it, else further off, else in the graph's top row as near across as it can be (none clear of them: none,
        // and the top row asked for, until there's one)
        if (star->x != roomFor_.first || star->y != roomFor_.second) {
            roomFor_ = {};
        }
        const QPointF point = star->vbox()->mapViewToScene(QPointF(*star->x, *star->y));
        fit(format_.arg(py::fixed(*star->y, 0)));
        const double beside = star->halfWidth() + kGap + width_ / 2;
        const double over = scale * (1 + MaxStar::kTwinkle) + MaxStar::kEdgeWidth / 2 + kGap + height_ / 2 +
                            scale * (MaxStar::kBelow - 1) / 2;
        const double under = scale * MaxStar::kBelow * (1 + MaxStar::kTwinkle) + MaxStar::kEdgeWidth / 2 + kGap +
                             height_ / 2 - scale * (MaxStar::kBelow - 1) / 2;
        const double corner = height_ + kGap;
        QList<std::pair<QPointF, bool>> spots = {
            {{side_ * beside, 0}, false},        {{-side_ * beside, 0}, false},      {{0, -over}, false},
            {{0, under}, false},                 {{side_ * beside, -corner}, false}, {{side_ * beside, corner}, false},
            {{-side_ * beside, -corner}, false}, {{-side_ * beside, corner}, false},
        };
        // (further off, a half width or a row at a time)
        for (int step = 1; step <= kFurthestSteps; ++step) {
            const double across = beside + step * width_ / 2;
            for (const double dy : {0.0, -corner, corner}) {
                spots.append({{side_ * across, dy}, false});
                spots.append({{-side_ * across, dy}, false});
            }
            spots.append({{0, -over - step * corner}, false});
            spots.append({{0, under + step * corner}, false});
        }
        spots.append({{0, 0}, true});
        for (double across = width_ / 2; across < area.width(); across += width_ / 2) {
            spots.append({{side_ * across, 0}, true});
            spots.append({{-side_ * across, 0}, true});
        }
        const QList<QRectF> obstacles = avoid + keepOff;
        bool placed = false;
        for (const auto& [offset, topRow] : spots) {
            const QRectF rect = rectAt(point, offset, topRow);
            if (rect.left() > area.left() && inArea.contains(rect) &&
                !touches(rect.adjusted(-kClear, -kClear, kClear, kClear), obstacles)) {
                offset_ = offset;
                topRow_ = topRow;
                side_ = offset.x() < 0 ? -1 : offset.x() > 0 ? 1 : side_;
                placed = true;
                break;
            }
        }
        if (!placed) {
            // (kept for this max: so the graph doesn't keep growing and shrinking)
            roomFor_ = {star->x, star->y};
            setVisible(false);
            return;
        }
        x_ = star->x;
        y_ = star->y;
        state_ = State::Appearing;
        stateStart_ = now;
    }
    if (state_ == State::Appearing && now - stateStart_ >= kAppearSeconds) {
        state_ = State::Shown;
    }

    // (the old max's place while popping)
    const QPointF point = star->vbox()->mapViewToScene(QPointF(*x_, *y_));
    rect_ = rectAt(point, offset_, topRow_);
    // (one popping is on its way out: others needn't keep clear of it)
    if (state_ != State::Popping) {
        avoid.append(rect_);
    }
    setPos(rect_.center());

    const double t = std::min(1.0, (now - stateStart_) / (state_ == State::Popping ? kPopSeconds : kAppearSeconds));
    if (state_ == State::Popping) {
        // bursts (see soap::paintPop)
        popT_ = t;
        setScale(1.0);
        setOpacity(1.0);
    } else if (state_ == State::Appearing) {
        // grows in, a little past its size and back (ease out back)
        constexpr double back = 1.7;
        setScale(0.5 + 0.5 * (1 + (back + 1) * std::pow(t - 1, 3) + back * std::pow(t - 1, 2)));
        setOpacity(std::min(1.0, 2 * t));
    } else {
        setScale(1.0);
        setOpacity(1.0);
    }
    // (its colors drift)
    update();
    setVisible(area.contains(point));
}

}  // namespace awo

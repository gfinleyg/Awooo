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
    return {-width_ / 2 - 1, -height_ / 2 - 1, width_ + 2, height_ + 2};
}

// in the theme's colors for the plot labels: its background, and black for the border and text
void MaxBubble::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    painter->setRenderHint(QPainter::Antialiasing);
    const QColor black(theme::current().mapHex(QStringLiteral("#000000")));
    QColor border = black;
    border.setAlpha(215);
    painter->setPen(QPen(border, 1));
    painter->setBrush(QColor(theme::current().mapHex(theme::kPlotLabelBg)));
    const QRectF rect(-width_ / 2, -height_ / 2, width_, height_);
    const double corner = height_ * kCorner;
    painter->drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), corner, corner);
    painter->setPen(black);
    painter->setFont(font_);
    painter->drawText(rect, Qt::AlignCenter, text_);
}

// the text in the theme's plot label font, sized to fill the star's height
void MaxBubble::fit(const QString& text) {
    prepareGeometryChange();
    text_ = text;
    height_ = MaxStar::kRadius * theme::uiScale() * (1 + MaxStar::kBelow);
    QFont font(QStringLiteral("Orbitron"));
    font.setBold(true);
    font_ = theme::current().mapFont(font);
    font_.setPixelSize(std::max(6, static_cast<int>(py::round(height_ * 0.72))));
    width_ = QFontMetricsF(font_).horizontalAdvance(text) + 2 * kPad * height_;
}

double MaxBubble::headroom() const {
    const double scale = MaxStar::kRadius * theme::uiScale();
    const double height = scale * (1 + MaxStar::kBelow);
    const double above = star_->restOffset() - scale * (MaxStar::kBelow - 1) / 2 + (1 + kPopGrowth) * height / 2;
    return std::max(star_->headroom(), above + 1 + MaxStar::kGap);
}

void MaxBubble::step(double now, QList<QRectF>& avoid) {
    MaxStar* star = star_;
    if (!star->x || !star->onGraph) {
        state_ = State::Hidden;
        setVisible(false);
        return;
    }
    if ((state_ == State::Appearing || state_ == State::Shown) && (star->x != x_ || star->y != y_)) {
        state_ = State::Popping;
        stateStart_ = now;
    }
    if (state_ == State::Popping && now - stateStart_ >= kPopSeconds) {
        state_ = State::Hidden;
    }
    if (state_ == State::Hidden) {
        if (star->progress(now) < 1) {
            setVisible(false);
            return;
        }
        // the star has arrived: a new one over its max
        x_ = star->x;
        y_ = star->y;
        fit(format_.arg(py::fixed(*y_, 0)));
        state_ = State::Appearing;
        stateStart_ = now;
    }
    if (state_ == State::Appearing && now - stateStart_ >= kAppearSeconds) {
        state_ = State::Shown;
    }

    // level with where the star rests over its max (the old one while popping; held down from the graph's top as
    // the star is, see MaxStar::step), the star's height top to bottom
    const QPointF point = star->vbox()->mapViewToScene(QPointF(*x_, *y_));
    const double scale = MaxStar::kRadius * theme::uiScale();
    const QRectF area = star->vbox()->sceneBoundingRect();
    const double offset = star->offset.value_or(star->restOffset());
    const double centerY = std::max(point.y() - offset, area.top() + scale) + scale * (MaxStar::kBelow - 1) / 2;
    // (the star's half width)
    const double reach = scale * std::cos(radians(18)) + MaxStar::kEdgeWidth / 2 + kGap;
    auto rectOn = [&](int side) {
        const double centerX = point.x() + side * (reach + width_ / 2);
        return QRectF(centerX - width_ / 2, centerY - height_ / 2, width_, height_);
    };
    QList<int> fits;
    for (const int side : {side_, -side_}) {
        if (area.contains(rectOn(side))) {
            fits.append(side);
        }
    }
    if (fits.isEmpty()) {
        fits = {side_};
    }
    side_ = fits.front();
    for (const int side : fits) {
        const QRectF rect = rectOn(side);
        if (std::none_of(avoid.begin(), avoid.end(), [&](const QRectF& other) { return rect.intersects(other); })) {
            side_ = side;
            break;
        }
    }
    const QRectF rect = rectOn(side_);
    avoid.append(rect);
    setPos(rect.center());

    const double t = std::min(1.0, (now - stateStart_) / (state_ == State::Popping ? kPopSeconds : kAppearSeconds));
    if (state_ == State::Popping) {
        // swells and fades away
        setScale(1 + kPopGrowth * t);
        setOpacity(1 - t);
    } else if (state_ == State::Appearing) {
        // grows in, a little past its size and back (ease out back)
        constexpr double back = 1.7;
        setScale(0.5 + 0.5 * (1 + (back + 1) * std::pow(t - 1, 3) + back * std::pow(t - 1, 2)));
        setOpacity(std::min(1.0, 2 * t));
    } else {
        setScale(1.0);
        setOpacity(1.0);
    }
    setVisible(area.contains(point));
}

}  // namespace awo

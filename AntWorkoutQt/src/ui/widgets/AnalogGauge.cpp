#include "AnalogGauge.hpp"

#include <QConicalGradient>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFontMetrics>
#include <QGridLayout>
#include <QPainter>
#include <QRadialGradient>
#include <QSvgRenderer>
#include <QSvgWidget>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "HaloLabel.hpp"
#include "Performance.hpp"
#include "PyCompat.hpp"
#include "Theme.hpp"

namespace awo {

namespace {

double radians(double degrees) {
    return degrees * std::numbers::pi / 180;
}

// a color in the theme's colors for it (its alpha kept)
QColor themed(const QColor& color) {
    return theme::current().mapColor(color);
}

// between two colors, channel by channel (alpha too)
QColor mix(const QColor& a, const QColor& b, double t) {
    float ar, ag, ab, aa, br, bg, bb, ba;
    a.getRgbF(&ar, &ag, &ab, &aa);
    b.getRgbF(&br, &bg, &bb, &ba);
    return QColor::fromRgbF(static_cast<float>(ar + (br - ar) * t), static_cast<float>(ag + (bg - ag) * t),
                            static_cast<float>(ab + (bb - ab) * t), static_cast<float>(aa + (ba - aa) * t));
}

// the color at value from (value, color) stops sorted by value, blended between them (the first's below them, the
// last's above)
QColor colorAt(const QList<std::pair<double, QColor>>& stops, double value) {
    if (value <= stops.front().first) {
        return stops.front().second;
    }
    for (qsizetype i = 0; i + 1 < stops.size(); ++i) {
        const auto& [v0, c0] = stops[i];
        const auto& [v1, c1] = stops[i + 1];
        if (value <= v1) {
            const double t = v1 != v0 ? (value - v0) / (v1 - v0) : 1;
            return mix(c0, c1, t);
        }
    }
    return stops.back().second;
}

// text drawn centered on (x, y) as the original does it: a box its advance (plus 1) wide and its height tall, at
// whole pixels
QRect centeredBox(const QFontMetrics& fm, double x, double y, const QString& text) {
    const int w = fm.horizontalAdvance(text) + 1;
    const int h = fm.height();
    return {static_cast<int>(x) - w / 2, static_cast<int>(y) - h / 2, w, h};
}

void drawCentered(QPainter& painter, const QFontMetrics& fm, double x, double y, const QString& text) {
    painter.drawText(centeredBox(fm, x, y, text), Qt::AlignCenter, text);
}

// a part of the widget (in its coordinates) from a layer as big as the widget, to the same place
void drawPart(QPainter& painter, const QPixmap& layer, const QRect& part) {
    const double dpr = layer.devicePixelRatio();
    const QRect pixels =
        QRectF(QPointF(part.topLeft()) * dpr, QSizeF(part.size()) * dpr).toAlignedRect().intersected(layer.rect());
    painter.drawPixmap(QPointF(pixels.topLeft()) / dpr, layer, pixels);
}

// the same over what's there, replacing it (with nothing, where the layer has nothing)
void replacePart(QPixmap& target, const QPixmap& layer, const QRect& part) {
    QPainter painter(&target);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    drawPart(painter, layer, part);
}

void clearPart(QPixmap& layer, const QRect& part) {
    QPainter painter(&layer);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(part, Qt::transparent);
}

// a gear's outline around (0, 0): its teeth's tips at r, the gaps between them depth in
QPolygonF centerGear(double r, double depth) {
    const double root = r - depth;
    const double pitch = 2 * std::numbers::pi / AnalogGauge::kCenterGearTeeth;
    QPolygonF points;
    // each tooth: along the gap's floor, up a flank, across the tip (the next tooth's gap starts down the other)
    for (int k = 0; k < AnalogGauge::kCenterGearTeeth; ++k) {
        for (const auto& [part, radius] :
             {std::pair{0.0, root}, std::pair{0.35, root}, std::pair{0.5, r}, std::pair{0.85, r}}) {
            const double a = (k + part) * pitch;
            points.append(QPointF(radius * std::cos(a), radius * std::sin(a)));
        }
    }
    return points;
}

}  // namespace

// a heart's outline around (0, 0), a little wider than a circle of radius r: the classic heart curve (32 wide, 29 tall),
// moved up to center it and scaled to r, made fuller through its lower half so the reading sits well inside it, ending
// at a right angle: its sides run straight into a corner in place of the curve's thin point
QPolygonF AnalogGauge::centerHeartOutline(double r) {
    constexpr double kWider = 1.04;
    // how far below the middle the corner is, on the curve's scale
    constexpr double kCorner = 15.2;
    // (the sine's power, from the curve's 3: lower is fuller)
    constexpr double kFuller = 1.5;
    // how far along the curve from its point the fullness has gone again, in radians
    constexpr double kSharpOver = 0.75;
    constexpr double kLonger = 0.12;
    constexpr double kCurveBelowMiddle = 14.5;
    const auto smooth = [](double u) {
        u = std::clamp(u, 0.0, 1.0);
        return u * u * (3 - 2 * u);
    };
    QPolygonF points;
    for (int k = 0; k < kCenterHeartPoints; ++k) {
        const double t = 2 * std::numbers::pi * k / kCenterHeartPoints;
        const double fromPoint = std::abs(t - std::numbers::pi);
        const double power = 3 - kFuller * smooth(1 - fromPoint / (std::numbers::pi / 2)) * (1 - smooth(1 - fromPoint / kSharpOver));
        const double x = 16 * (std::sin(t) < 0 ? -1 : 1) * std::pow(std::abs(std::sin(t)), power);
        double y = -(13 * std::cos(t) - 5 * std::cos(2 * t) - 2 * std::cos(3 * t) - std::cos(4 * t)) - 2.5;
        if (y > 0) {
            const double down = y / kCurveBelowMiddle;
            y *= 1 + kLonger * down * down;
        }
        // (45 degrees either side of straight down from the corner, and what's past it gathered into it)
        y = std::min(y, kCorner);
        const QPointF point(std::clamp(x * kWider, y - kCorner, kCorner - y) * r / 16, y * r / 16);
        if (points.isEmpty() || points.back() != point) {
            points.append(point);
        }
    }
    return points;
}

QString imagePath(const QString& name) {
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("images/") + name);
}

// ---- AnalogGauge ----

AnalogGauge::AnalogGauge(QWidget* parent) : QWidget(parent), needleTimer_(new QTimer(this)) {
    // the easing's frame rate is the Performance setting (each frame only redraws the moving parts); 0 fps: no
    // easing, the needle jumps to each reading
    needleTimer_->setTimerType(Qt::PreciseTimer);
    connect(needleTimer_, &QTimer::timeout, this, &AnalogGauge::animateNeedle);
    needleClock_.start();
    applyPerformance();
    connect(&Performance::current(), &Performance::changed, this, &AnalogGauge::applyPerformance);
    setGaugeTheme(0);
    rescale();
    connect(this, &AnalogGauge::valueChanged, this, &AnalogGauge::redrawFrame);
    // (the tick marks, numbers and units in the theme's colors, the numbers in its Gauge numbers font)
    connect(&theme::notifier(), &theme::Notifier::changed, this, [this] {
        rescale();
        redraw();
    });
}

void AnalogGauge::setValue(double value) {
    reading_ = std::max(value, 0.0);
    if (value <= minValue_) {
        value_ = minValue_;
    } else if (value >= maxValue_) {
        value_ = maxValue_;
    } else {
        value_ = value;
    }
    if (Performance::current().gaugeFps() == 0) {
        // no easing: the needle is drawn right at the reading
        displayValue_ = value_;
    } else if (displayValue_ != value_ && !needleTimer_->isActive()) {
        needleLastStep_ = needleClock_.nsecsElapsed() / 1e9;
        needleTimer_->start();
    }
    emit valueChanged(static_cast<int>(value));
}

void AnalogGauge::snapNeedle() {
    needleTimer_->stop();
    displayValue_ = value_;
    redrawFrame();
}

void AnalogGauge::applyPerformance() {
    const int fps = Performance::current().gaugeFps();
    if (fps == 0) {
        // easing turned off part way through: the needle goes straight to the reading
        if (needleTimer_->isActive()) {
            needleTimer_->stop();
            displayValue_ = value_;
            redrawFrame();
        }
    } else {
        needleTimer_->setInterval(static_cast<int>(py::round(1000.0 / fps)));
    }
}

// ease the drawn needle toward the latest reading; the timer stops itself once the needle arrives (stepped by the time
// since the last step, so the needle moves at the same speed at any frame rate)
void AnalogGauge::animateNeedle() {
    const double now = needleClock_.nsecsElapsed() / 1e9;
    const double dt = now - needleLastStep_;
    needleLastStep_ = now;
    const double diff = value_ - displayValue_;
    // covers ~90% of the gap in a quarter second, about the time between ANT+ readings
    const double step = diff * (1 - std::exp(-dt / kNeedleEaseSeconds));
    if (std::abs(diff - step) < (maxValue_ - minValue_) * 0.0005) {
        displayValue_ = value_;
        needleTimer_->stop();
    } else {
        displayValue_ += step;
    }
    redrawFrame();
}

void AnalogGauge::setMinValue(double min) {
    if (value_ < min) {
        value_ = min;
        displayValue_ = min;
    }
    minValue_ = min >= maxValue_ ? maxValue_ - 1 : min;
    redraw();
}

void AnalogGauge::setMaxValue(double max) {
    if (value_ > max) {
        value_ = max;
        displayValue_ = max;
    }
    maxValue_ = max <= minValue_ ? minValue_ + 1 : max;
    redraw();
}

void AnalogGauge::setGaugeTheme(int theme) {
    if (theme == 3) {
        setScalePolygonColors(QGradientStops{{0.0, QColor(QStringLiteral("#FFFFFF"))}});
        needleCenterBg = {{0, QColor(QStringLiteral("#FFFFFF"))}};
        setOuterCircleBg({{0, QColor(QStringLiteral("#FFFFFF"))}});
        bigScaleMarker = QColor(QStringLiteral("#000000"));
        fineScaleColor = QColor(QStringLiteral("#000000"));
        return;
    }
    setScalePolygonColors(QGradientStops{{.00, QColor(QStringLiteral("#FF0000"))},
                                         {.1, QColor(QStringLiteral("#FFFF00"))},
                                         {.15, QColor(QStringLiteral("#00FF00"))},
                                         {1, QColor(QStringLiteral("#FFFFFFFF"))}});
    needleCenterBg = {{0, QColor(35, 40, 3, 255)},          {0.16, QColor(30, 36, 45, 255)},
                      {0.225, QColor(36, 42, 54, 255)},     {0.423963, QColor(19, 23, 29, 255)},
                      {0.580645, QColor(45, 53, 68, 255)},  {0.792627, QColor(59, 70, 88, 255)},
                      {0.935, QColor(30, 35, 45, 255)},     {1, QColor(35, 40, 3, 255)}};
    setOuterCircleBg({{0.0645161, QColor(30, 35, 45, 255)},
                      {0.37788, QColor(57, 67, 86, 255)},
                      {1, QColor(30, 36, 45, 255)}});
}

void AnalogGauge::setNeedleColor(const QColor& color) {
    needleColor_ = color;
    redraw();
}

void AnalogGauge::setScalaCount(int count) {
    scalaCount_ = std::max(count, 1);
    redraw();
}

void AnalogGauge::setEnableBarGraph(bool enable) {
    enableBarGraph_ = enable;
    redraw();
}

void AnalogGauge::setEnableBigScaleGrid(bool enable) {
    enableBigScaledMarker_ = enable;
    redraw();
}

void AnalogGauge::setEnableDrawNewArc(bool enable) {
    enableDrawNewArc_ = enable;
    redraw();
}

void AnalogGauge::setScalePolygonColors(const std::optional<QGradientStops>& stops) {
    scalePolygonColors_ = stops.value_or(QGradientStops{{0.0, QColor(Qt::transparent)}});
    redraw();
}

// where a value on the scale falls in the band's gradient (see drawFilledPolygon): the conical gradient starts just
// past the end of the scale and runs the other way round
double AnalogGauge::valueGradientPosition(double value) const {
    const double fraction = (value - minValue_) / (maxValue_ - minValue_);
    return (scaleAngleSize_ * (1 - fraction) + 1) / 360;
}

void AnalogGauge::setValueColors(QList<std::pair<double, QColor>> stops) {
    std::stable_sort(stops.begin(), stops.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (stops.isEmpty()) {
        setScalePolygonColors(std::nullopt);
        return;
    }
    QList<std::pair<double, QColor>> shown;
    for (const auto& [value, color] : stops) {
        if (minValue_ <= value && value <= maxValue_) {
            shown.append({value, color});
        }
    }
    const QList<std::pair<double, QColor>> ends = {{minValue_, colorAt(stops, minValue_)}, {maxValue_, stops.back().second}};
    QList<std::pair<double, QColor>> all = shown;
    for (const auto& end : ends) {
        const bool there = std::any_of(shown.begin(), shown.end(),
                                       [&](const auto& stop) { return std::abs(end.first - stop.first) < 1e-9; });
        if (!there) {
            all.append(end);
        }
    }
    std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    QGradientStops gradient;
    for (const auto& [value, color] : all) {
        gradient.append({valueGradientPosition(value), color});
    }
    setScalePolygonColors(gradient);
}

void AnalogGauge::setOuterCircleBg(const QGradientStops& stops) {
    if (stops != outerCircleBg_) {
        outerCircleBg_ = stops;
        redraw();
    }
}

void AnalogGauge::setCenterRings(const std::optional<QList<QColor>>& colors, double level) {
    std::optional<std::pair<QList<QColor>, double>> rings;
    if (colors) {
        rings = std::pair(*colors, level);
    }
    // (a small step in the level isn't worth a frame, but back to rest always is)
    if (centerRings_ && rings && centerRings_->first == rings->first && std::abs(centerRings_->second - level) < 0.02 &&
        (level != 0 || centerRings_->second == 0)) {
        return;
    }
    if (centerRings_ != rings) {
        centerRings_ = rings;
        redrawFrame();
    }
}

void AnalogGauge::setCenterGearAngle(std::optional<double> angle) {
    const std::optional<double> old = centerGearAngle_;
    centerGearAngle_ = angle;
    if (!centerRings_ || angle == old) {
        return;
    }
    if (!angle || !old) {
        redrawFrame();
        return;
    }
    const int fps = Performance::current().gaugeFps();
    const double now = needleClock_.nsecsElapsed() / 1e9;
    // (a few ms early still counts: the chainring's own frames are about the fastest rate's apart)
    if (fps != 0 && now - gearLastFrame_ >= 1.0 / fps - 0.004) {
        gearLastFrame_ = now;
        redrawFrame();
    }
}

int AnalogGauge::ringCount() const {
    if (!centerRingMinPx || *centerRingMinPx == 0) {
        return centerRingCount;
    }
    const double radius = widgetDiameter_ / bigNeedleCenterSize;
    return std::max(centerRingCount, 1 + static_cast<int>(radius * kCenterRingSpread / *centerRingMinPx));
}

void AnalogGauge::setArc(double centerValue, const QGradientStops& colors) {
    arcCenterValue = centerValue;
    newArcColors = colors;
    setEnableDrawNewArc(true);
}

void AnalogGauge::redraw() {
    paintCacheDirty_ = true;
    redrawFrame();
}

void AnalogGauge::redrawFrame() {
    frameDirty_ = true;
    update();
}

void AnalogGauge::rescale() {
    widgetDiameter_ = std::min(width(), height());
    const int reach = static_cast<int>(widgetDiameter_ / 2 * needleScaleFactor_);
    // from inside the center's edge, which covers where it starts (a heart's notch comes in to under half its radius, a
    // gear's gaps and a circle's edge stay beyond 0.8), along the taper from a base 8 wide 30 behind the middle
    const double start = widgetDiameter_ / bigNeedleCenterSize * (centerHeart ? 0.4 : 0.8);
    const double half = 4 - 2 * (30 + start) / (30.0 + reach);
    needle_ = QPolygonF({QPointF(half, -start), QPointF(-half, -start), QPointF(-2, -reach),
                         QPointF(0, -static_cast<int>(widgetDiameter_ / 2 * needleScaleFactor_ - 6)), QPointF(2, -reach)});
    redraw();
    // the font sizes from the gauge's size, in the theme's Gauge numbers font at its scale
    const theme::FontSettings settings = theme::current().fonts.value(theme::kRoleGauge);
    scaleFontname_ = valueFontname_ = settings.family;
    scaleFontsize_ = initialScaleFontsize * widgetDiameter_ / 400 * settings.scale / 100;
    valueFontsize_ = initialValueFontsize * widgetDiameter_ / 400 * settings.scale / 100;
}

// worked out again only when something it depends on changes, not on every needle frame
double AnalogGauge::fittedValueFontsize() {
    const auto key = std::make_tuple(widgetDiameter_, bigNeedleCenterSize, maxValue_, valueDecimals, valueFontsize_,
                                     valueFontname_, centerHeart);
    if (!fittedValueFontsize_ || fittedValueFontsize_->first != key) {
        fittedValueFontsize_ = std::pair(key, fitValueFontsize());
    }
    return fittedValueFontsize_->second;
}

// the largest font size (up to valueFontsize) where the widest possible value's glyphs stay inside the center circle (or
// heart)
double AnalogGauge::fitValueFontsize() const {
    const double radius = (widgetDiameter_ / bigNeedleCenterSize - 1) * 0.92 * (centerHeart ? kCenterHeartFit : 1);
    QString widest = py::fixed(maxValue_, valueDecimals);
    const QFont font(valueFontname_, static_cast<int>(valueFontsize_), QFont::Bold);
    const QFontMetrics fm(font);
    QChar widestDigit = u'0';
    for (const QChar digit : QStringLiteral("0123456789")) {
        if (fm.horizontalAdvance(digit) > fm.horizontalAdvance(widestDigit)) {
            widestDigit = digit;
        }
    }
    for (QChar& c : widest) {
        if (c.isDigit()) {
            c = widestDigit;
        }
    }
    const QRect rect = fm.tightBoundingRect(widest);
    const double corner = std::hypot(rect.width() / 2.0, rect.height() / 2.0);
    if (corner <= radius) {
        return valueFontsize_;
    }
    return std::max(1.0, valueFontsize_ * radius / corner);
}

int AnalogGauge::filledLength(int length) const {
    return enableBarGraph_ ? length
                           : static_cast<int>(py::round((length / (maxValue_ - minValue_)) * (displayValue_ - minValue_)));
}

QPolygonF AnalogGauge::createPolygonPie(double outerRadius, double innerRadius, double start, int length,
                                        bool barGraph) {
    // (one step a degree: other step sizes draw it wrong)
    constexpr double w = 360 / 360.;
    if (barGraph) {
        length = filledLength(length);
    }
    const auto key = std::make_tuple(outerRadius, innerRadius, start, length, angleOffset_);
    if (const auto found = pieCache_.find(key); found != pieCache_.end()) {
        return found->second;
    }
    QPolygonF pie;
    double x = 0, y = 0;
    // the outer edge from start to start + length, then the inner one back
    for (int i = 0; i <= length; ++i) {
        const double t = w * i + start - angleOffset_;
        x = outerRadius * std::cos(radians(t));
        y = outerRadius * std::sin(radians(t));
        pie.append(QPointF(x, y));
    }
    for (int i = 0; i <= length; ++i) {
        const double t = w * (length - i) + start - angleOffset_;
        x = innerRadius * std::cos(radians(t));
        y = innerRadius * std::sin(radians(t));
        pie.append(QPointF(x, y));
    }
    pie.append(QPointF(x, y));
    // (a resize makes a new set of shapes; the old ones are dropped once there are plenty)
    if (pieCache_.size() >= 1000) {
        pieCache_.clear();
    }
    pieCache_[key] = pie;
    return pie;
}

void AnalogGauge::drawFilledPolygon(int outlinePenWidth) {
    QPainter painter(paintTarget_);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(width() / 2.0, height() / 2.0);
    painter.setPen(Qt::NoPen);
    pen_.setWidth(outlinePenWidth);
    if (outlinePenWidth > 0) {
        painter.setPen(pen_);
    }
    const double radius = widgetDiameter_ / 2 - pen_.width() / 2.0;
    const QPolygonF band = createPolygonPie(radius * gaugeColorOuterRadiusFactor, radius * gaugeColorInnerRadiusFactor,
                                            scaleAngleStartValue_, static_cast<int>(scaleAngleSize_));
    QConicalGradient grad(QPointF(0, 0), -scaleAngleSize_ - scaleAngleStartValue_ + angleOffset_ - 1);
    for (const auto& [position, color] : scalePolygonColors_) {
        grad.setColorAt(position, color);
    }
    painter.setBrush(grad);
    painter.drawPolygon(band);
}

void AnalogGauge::drawBigScaledMarker() {
    QPainter painter(paintTarget_);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(width() / 2.0, height() / 2.0);
    pen_ = QPen(themed(bigScaleMarker));
    pen_.setWidth(2);
    painter.setPen(pen_);
    painter.rotate(scaleAngleStartValue_ - angleOffset_);
    const double stepSize = scaleAngleSize_ / scalaCount_;
    const double outerStart = widgetDiameter_ / 2;
    const double lineLength = widgetDiameter_ / 2 - widgetDiameter_ / 20;
    for (int i = 0; i <= scalaCount_; ++i) {
        painter.drawLine(static_cast<int>(lineLength), 0, static_cast<int>(outerStart), 0);
        painter.rotate(stepSize);
    }
}

void AnalogGauge::createScaleMarkerValuesText() {
    QPainter painter(paintTarget_);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(width() / 2.0, height() / 2.0);
    const QFont font(scaleFontname_, static_cast<int>(scaleFontsize_), QFont::Bold);
    const QFontMetrics fm(font);
    QPen pen;
    pen.setBrush(themed(scaleValueColor_));
    painter.setPen(pen);
    const double textRadius = widgetDiameter_ / 2 * 0.8;
    const int scalePerDiv = static_cast<int>((maxValue_ - minValue_) / scalaCount_);
    const double angleDistance = scaleAngleSize_ / scalaCount_;
    painter.setFont(font);
    for (int i = 0; i <= scalaCount_; ++i) {
        const QString text = QString::number(static_cast<long long>(minValue_ + scalePerDiv * i));
        const double angle = angleDistance * i + (scaleAngleStartValue_ - angleOffset_);
        const double x = textRadius * std::cos(radians(angle));
        const double y = textRadius * std::sin(radians(angle));
        drawCentered(painter, fm, x, y, text);
        faceRect_ |= textArea(centeredBox(fm, x, y, text), fm);
    }
}

void AnalogGauge::createFineScaledMarker() {
    QPainter painter(paintTarget_);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(width() / 2.0, height() / 2.0);
    painter.setPen(themed(fineScaleColor));
    painter.rotate(scaleAngleStartValue_ - angleOffset_);
    const int ticks = scalaCount_ * scalaSubdivCount;
    const double stepSize = scaleAngleSize_ / ticks;
    const double outerStart = widgetDiameter_ / 2;
    const double lineLength = widgetDiameter_ / 2 - widgetDiameter_ / 40;
    for (int i = 0; i <= ticks; ++i) {
        painter.drawLine(static_cast<int>(lineLength), 0, static_cast<int>(outerStart), 0);
        painter.rotate(stepSize);
    }
}

QRect AnalogGauge::textArea(const QRect& box, const QFontMetrics& fm) const {
    const int margin = fm.height() / 2 + 1;
    return box.translated(width() / 2, height() / 2).adjusted(-margin, -margin, margin, margin);
}

QFont AnalogGauge::valueFont() {
    const double fontsize = fitValueInCenter ? fittedValueFontsize() : valueFontsize_;
    return QFont(valueFontname_, static_cast<int>(fontsize), QFont::Bold);
}

QString AnalogGauge::valueText() const {
    return valueDecimals ? py::fixed(reading_, valueDecimals) : QString::number(static_cast<long long>(reading_));
}

std::optional<QColor> AnalogGauge::valueHalo() {
    // (only a reading in the center is over it)
    if (!enableCenterPoint_ || !altUnitsDisplay) {
        return std::nullopt;
    }
    QList<QColor> behind;
    if (centerRings_ && centerRings_->first.size() >= 2) {
        // the rings under the reading's middle and its ends
        const double radius = centerRadius();
        const double reach = QFontMetrics(valueFont()).horizontalAdvance(valueText()) / 2.0;
        const int count = ringCount();
        int outer = count - 1;
        while (outer > 0 && ringRadius(outer, radius) < reach) {
            --outer;
        }
        behind = {ringColor(count - 1), ringColor(outer)};
    } else if (!needleCenterBg.isEmpty()) {
        behind = {needleCenterBg.front().second};
    } else {
        return std::nullopt;
    }
    // (the one it stands out from least)
    const QColor& worst = *std::min_element(behind.begin(), behind.end(), [this](const QColor& a, const QColor& b) {
        return theme::contrast(displayValueColor_, a) < theme::contrast(displayValueColor_, b);
    });
    if (!halo::needed(displayValueColor_, worst)) {
        return std::nullopt;
    }
    return halo::color(displayValueColor_, worst, QColor(Qt::white));
}

// (working it out takes a while, and the rings' gear turning doesn't change it)
const std::optional<QColor>& AnalogGauge::currentHalo() {
    HaloKey key(valueText(), valueFont(), centerRings_, centerGearAngle_.has_value(), ringCount());
    if (haloKey_ != key) {
        haloKey_ = std::move(key);
        halo_ = valueHalo();
    }
    return halo_;
}

void AnalogGauge::createValuesText() {
    const std::optional<QColor> outline = currentHalo();
    QPainter painter(paintTarget_);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.translate(width() / 2.0, height() / 2.0);
    const QFont font = valueFont();
    const QFontMetrics fm(font);
    QPen pen;
    // (on the gauge's own white face: not themed)
    pen.setBrush(displayValueColor_);
    painter.setPen(pen);
    const double textRadius = altUnitsDisplay ? 0 : widgetDiameter_ / 2 * textRadiusFactor_;
    const QString text = valueText();
    painter.setFont(font);
    // halfway between the scale's start and end
    const double angleEnd = scaleAngleStartValue_ + scaleAngleSize_ - 360;
    const double angle = (angleEnd - scaleAngleStartValue_) / 2 + scaleAngleStartValue_;
    const double x = textRadius * std::cos(radians(angle));
    const double y = textRadius * std::sin(radians(angle));
    if (outline) {
        halo::draw(painter, centeredBox(fm, x, y, text), Qt::AlignCenter, text, *outline);
    }
    drawCentered(painter, fm, x, y, text);
    textRect_ |= textArea(centeredBox(fm, x, y, text), fm);
}

void AnalogGauge::createUnitsText() {
    QPainter painter(paintTarget_);
    painter.setRenderHint(QPainter::TextAntialiasing);
    // (at the bottom of the gauge with altUnitsDisplay)
    const double unitPosition = altUnitsDisplay ? 360 : 180;
    painter.translate(width() / 2.0, height() / 2.0);
    const QFont font(valueFontname_, static_cast<int>(valueFontsize_ / 2.5), QFont::Bold);
    const QFontMetrics fm(font);
    QPen pen;
    pen.setBrush(themed(displayValueColor_));
    painter.setPen(pen);
    const double textRadius = widgetDiameter_ / 1.9 * textRadiusFactor_;
    painter.setFont(font);
    const double angleEnd = scaleAngleStartValue_ + scaleAngleSize_ + unitPosition;
    const double angle = (angleEnd - scaleAngleStartValue_) / 2 + scaleAngleStartValue_;
    const double x = textRadius * std::cos(radians(angle));
    // (at the bottom: on the row of the scale's end numbers, see createScaleMarkerValuesText)
    const double y = altUnitsDisplay ? widgetDiameter_ / 2 * 0.8 * std::sin(radians(45.0))
                                     : textRadius * std::sin(radians(angle));
    drawCentered(painter, fm, x, y, units);
    textRect_ |= textArea(centeredBox(fm, x, y, units), fm);
}

QColor AnalogGauge::ringColor(int i) const {
    const auto& [colors, level] = *centerRings_;
    const double t = static_cast<double>(i) / (ringCount() - 1);
    // the colors spread evenly from t 0 (outermost) to 1 (innermost), blended between
    const double at = t * (colors.size() - 1);
    const qsizetype stop = std::min(static_cast<qsizetype>(at), colors.size() - 2);
    const QColor shade = mix(colors[stop], colors[stop + 1], at - stop);
    const auto [paleOuter, paleInner] = centerRingsPale;
    // paler toward the middle (the reading is drawn over it), less pale on the beat (around a gear, which stays as it
    // is, the rings inside it lose all of it)
    const double beat = !centerGearAngle_ ? 0.6 * level : i == 0 ? 0.0 : level;
    return mix(shade, QColor(QStringLiteral("#ffffff")), (paleOuter + (paleInner - paleOuter) * t) * (1 - beat));
}

double AnalogGauge::ringRadius(int i, double radius) const {
    const double t = static_cast<double>(i) / (ringCount() - 1);
    const double rest = radius * (1 - kCenterRingSpread * t);
    const double level = centerRings_->second;
    if (!centerGearAngle_) {
        return rest * (1 + 0.04 * level);
    }
    if (i == 0) {
        return rest;
    }
    // the rings inside a gear all swell by as much as leaves the outermost of them a pixel short of the gaps' floor,
    // the two outlines between them
    const double band = radius * kCenterRingSpread / (ringCount() - 1);
    return rest + level * std::max(0.0, band * kCenterGearDepth - ringOutlineWidth(radius) - 1);
}

// (thin enough to leave narrow rings their color)
double AnalogGauge::ringOutlineWidth(double radius) const {
    const double band = radius * kCenterRingSpread / (ringCount() - 1);
    return std::max(1.0, std::min(radius * 0.02, band * 0.15));
}

double AnalogGauge::centerRadius() const {
    return widgetDiameter_ / bigNeedleCenterSize - pen_.width() / 2.0;
}

void AnalogGauge::drawCenterRings(QPainter& painter, double radius) {
    const int count = ringCount();
    // (each ring's width)
    const double band = radius * kCenterRingSpread / (count - 1);
    for (int i = 0; i < count; ++i) {
        const double r = ringRadius(i, radius);
        const QColor color = ringColor(i);
        QRadialGradient grad(QPointF(0, 0), r);
        grad.setColorAt(0, color.lighter(112));
        grad.setColorAt(1, color.darker(118));
        painter.setBrush(grad);
        painter.setPen(QPen(color.darker(140), ringOutlineWidth(radius)));
        if (i == 0 && centerGearAngle_) {
            painter.save();
            painter.rotate(*centerGearAngle_);
            painter.drawPolygon(centerGear(r, band * kCenterGearDepth));
            painter.restore();
        } else if (centerHeart) {
            painter.drawPolygon(centerHeartOutline(r));
        } else {
            painter.drawEllipse(QPointF(0, 0), r, r);
        }
    }
}

void AnalogGauge::drawBigNeedleCenterPoint() {
    QPainter painter(paintTarget_);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(width() / 2.0, height() / 2.0);
    painter.setPen(Qt::NoPen);
    const double radius = centerRadius();
    if (centerRings_ && centerRings_->first.size() >= 2) {
        drawCenterRings(painter, radius);
        return;
    }
    const QPolygonF center = createPolygonPie(radius, 0, scaleAngleStartValue_, 360, false);
    QConicalGradient grad(QPointF(0, 0), 0);
    for (const auto& [position, color] : needleCenterBg) {
        grad.setColorAt(position, color);
    }
    painter.setBrush(grad);
    painter.drawPolygon(center);
}

void AnalogGauge::drawOuterCircle() {
    QPainter painter(paintTarget_);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(width() / 2.0, height() / 2.0);
    painter.setPen(Qt::NoPen);
    const QPolygonF ring = createPolygonPie(widgetDiameter_ / 2 - pen_.width(), widgetDiameter_ / 6,
                                            scaleAngleStartValue_ / 10, 360, false);
    QRadialGradient grad(QPointF(0, 0), width());
    for (const auto& [position, color] : outerCircleBg_) {
        grad.setColorAt(position, color);
    }
    painter.setBrush(grad);
    painter.drawPolygon(ring);
}

// the target arc, centered on arcCenterValue, wider the higher it is: from the center's edge (the outermost ring's, as
// it swells on a beat; a gear's, which doesn't, just under the floor of the gaps between its teeth, so it fills them)
// out to the outer ring's, which the widget's edge doesn't cut off when it points straight up
void AnalogGauge::drawNewArc() {
    QPainter painter(paintTarget_);
    painter.setRenderHint(QPainter::Antialiasing);
    int arcWidth = 8 + static_cast<int>(40 * increasePercentage * (arcCenterValue / maxValue_));
    // (rounded up to an even split)
    if (arcWidth % 2 == 1) {
        arcWidth += 1;
    }
    // the center value moved back by half the width
    const int start = 135 + static_cast<int>(270 * (arcCenterValue / maxValue_)) - arcWidth / 2;
    if (arcCenterValue == 0) {
        arcWidth = 0;
    }
    painter.translate(width() / 2.0, height() / 2.0);
    painter.setPen(Qt::NoPen);
    const double center = centerRadius();
    double inner = center;
    if (centerRings_ && centerRings_->first.size() >= 2) {
        inner = ringRadius(0, center);
        if (centerGearAngle_) {
            inner -= center * kCenterRingSpread / (ringCount() - 1) * kCenterGearDepth + 1;
        }
    }
    const QPolygonF arc = createPolygonPie(widgetDiameter_ / 2 - pen_.width(), inner, start, arcWidth, false);
    QRadialGradient grad(QPointF(0, 0), width());
    for (const auto& [position, color] : newArcColors) {
        grad.setColorAt(position, color);
    }
    painter.setBrush(grad);
    painter.drawPolygon(arc);
}

void AnalogGauge::drawNeedle() {
    QPainter painter(paintTarget_);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(width() / 2.0, height() / 2.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(needleColor_);
    painter.rotate(((displayValue_ - valueOffset_ - minValue_) * scaleAngleSize_ / (maxValue_ - minValue_)) + 90 +
                   scaleAngleStartValue_);
    painter.drawConvexPolygon(needle_);
}

void AnalogGauge::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const int count = ringCount();
    rescale();
    paintCacheDirty_ = true;
    if (ringCount() != count) {
        emit ringCountChanged(ringCount());
    }
}

QPixmap AnalogGauge::newLayer(const QSize& size, double dpr) const {
    QPixmap layer(size);
    layer.setDevicePixelRatio(dpr);
    layer.fill(Qt::transparent);
    return layer;
}

void AnalogGauge::paintEvent(QPaintEvent*) {
    const double dpr = devicePixelRatioF();
    const QSize size(static_cast<int>(py::round(width() * dpr)), static_cast<int>(py::round(height() * dpr)));
    if (paintCache_.isNull() || paintCache_.devicePixelRatio() != dpr || paintCache_.size() != size) {
        paintCache_ = newLayer(size, dpr);
        paintCacheDirty_ = true;
    }
    if (paintCacheDirty_) {
        // the gauge's circle (the numbers add themselves as they're drawn)
        const int reach = static_cast<int>(widgetDiameter_ / 2) + 3;
        faceRect_ = QRect(width() / 2 - reach, height() / 2 - reach, 2 * reach, 2 * reach);
        faceUnder_ = newLayer(size, dpr);
        drawLayer(faceUnder_, &AnalogGauge::drawFaceUnder);
        faceOver_ = newLayer(size, dpr);
        drawLayer(faceOver_, &AnalogGauge::drawFaceOver);
        face_ = newLayer(size, dpr);
        centerLayer_ = newLayer(size, dpr);
        faceBand_.reset();
        centerKey_.reset();
        haloKey_.reset();
        centerRect_ = QRect();
        paintCacheDirty_ = false;
        frameDirty_ = true;
    }
    if (frameDirty_) {
        drawLayer(paintCache_, &AnalogGauge::drawFrame);
        frameDirty_ = false;
    }
    QPainter painter(this);
    drawPart(painter, paintCache_, frameRect_);
}

void AnalogGauge::drawLayer(QPixmap& layer, void (AnalogGauge::*draw)()) {
    QPaintDevice* const target = paintTarget_;
    paintTarget_ = &layer;
    (this->*draw)();
    paintTarget_ = target;
}

// the cached face under and over the band, then the band and everything that moves over them, in the order the whole
// gauge was always drawn in, but for the arc: under the center, whose gear's teeth stand over it
void AnalogGauge::drawFaceUnder() {
    drawOuterCircle();
}

void AnalogGauge::drawFaceOver() {
    if (enableFineScaledMarker_) {
        createFineScaledMarker();
    }
    if (enableBigScaledMarker_) {
        drawBigScaledMarker();
    }
    if (enableScaleText_) {
        createScaleMarkerValuesText();
    }
}

void AnalogGauge::drawFace() {
    replacePart(face_, faceUnder_, faceRect_);
    if (enableFilledPolygon_) {
        drawFilledPolygon();
    }
    // drawing the big scale markers (cached in the face, so not drawn here) leaves the pen 2 wide after the band sets
    // it to 0, and the center point, the arc and the next face's outer circle are sized by that width
    if (enableBigScaledMarker_) {
        pen_.setWidth(2);
    }
    QPainter painter(paintTarget_);
    drawPart(painter, faceOver_, faceRect_);
}

bool AnalogGauge::valueOnCenter() const {
    return enableValueText_ && enableCenterPoint_ && altUnitsDisplay;
}

void AnalogGauge::drawCenterLayer() {
    clearPart(centerLayer_, centerRect_);
    // (the rings swell on a beat)
    const int reach = static_cast<int>(centerRadius() * 1.08) + 3;
    centerRect_ = QRect(width() / 2 - reach, height() / 2 - reach, 2 * reach, 2 * reach);
    if (enableCenterPoint_) {
        drawBigNeedleCenterPoint();
    }
    if (valueOnCenter()) {
        textRect_ = QRect();
        createValuesText();
        centerRect_ |= textRect_;
    }
}

void AnalogGauge::drawFrame() {
    const int band = enableFilledPolygon_ ? filledLength(static_cast<int>(scaleAngleSize_)) : 0;
    if (faceBand_ != band) {
        faceBand_ = band;
        drawLayer(face_, &AnalogGauge::drawFace);
    }
    CenterKey key(centerRings_, centerGearAngle_, centerRadius(), ringCount(), centerRingsPale, centerHeart,
                  needleCenterBg, valueOnCenter() ? valueText() : QString(), valueFont(), currentHalo());
    if (centerKey_ != key) {
        centerKey_ = std::move(key);
        drawLayer(centerLayer_, &AnalogGauge::drawCenterLayer);
    }
    // (over all of the last frame too: its text may have reached further)
    replacePart(paintCache_, face_, faceRect_ | centerRect_ | frameRect_);
    if (enableNeedlePolygon_) {
        drawNeedle();
    }
    if (enableDrawNewArc_) {
        drawNewArc();
    }
    {
        QPainter painter(paintTarget_);
        drawPart(painter, centerLayer_, centerRect_);
    }
    textRect_ = QRect();
    if (enableValueText_) {
        if (!valueOnCenter()) {
            createValuesText();
        }
        createUnitsText();
    }
    frameRect_ = faceRect_ | centerRect_ | textRect_;
}

// ---- RecentAverage ----

void RecentAverage::add(double value, double now) {
    // (the oldest stays while it reaches into the last kSeconds)
    while (samples_.size() >= 2 && samples_[1].since <= now - kSeconds) {
        samples_.pop_front();
    }
    samples_.push_back({value, now});
}

double RecentAverage::at(double now) const {
    double total = 0;
    double seconds = 0;
    for (size_t i = 0; i < samples_.size(); ++i) {
        const double from = std::max(samples_[i].since, now - kSeconds);
        const double to = i + 1 < samples_.size() ? samples_[i + 1].since : now;
        if (to > from && (zeroCounts_ || samples_[i].value > 0)) {
            total += samples_[i].value * (to - from);
            seconds += to - from;
        }
    }
    if (seconds > 0) {
        return total / seconds;
    }
    // (no time has passed since the first reading, or there's only no reading left)
    return samples_.empty() ? 0 : samples_.back().value;
}

// ---- Heartbeat ----

Heartbeat::Heartbeat(std::function<double()> bpmSource, QObject* parent)
    : QObject(parent), bpmSource_(std::move(bpmSource)), beatTimer_(new QTimer(this)), pulseTimer_(new QTimer(this)) {
    beatTimer_->setSingleShot(true);
    beatTimer_->setTimerType(Qt::PreciseTimer);
    connect(beatTimer_, &QTimer::timeout, this, &Heartbeat::nextPhase);
    beatTimer_->start(0);
    pulseTimer_->setTimerType(Qt::PreciseTimer);
    connect(pulseTimer_, &QTimer::timeout, this, &Heartbeat::pulseStep);
}

void Heartbeat::nextPhase() {
    const double bpm = bpmSource_();
    if (bpm <= 0) {
        // no reading: rest and check again shortly
        setBeating(false);
        beatTimer_->start(500);
        return;
    }
    const double periodMs = 60000 / bpm;
    if (beating_) {
        setBeating(false);
        beatTimer_->start(static_cast<int>(py::round(periodMs * (1 - kBeatFraction))));
    } else {
        setBeating(true);
        beatTimer_->start(static_cast<int>(py::round(periodMs * kBeatFraction)));
    }
}

void Heartbeat::setBeating(bool beating) {
    if (beating != beating_) {
        beating_ = beating;
        const int fps = Performance::current().gaugeFps();
        if (beating) {
            pulseClock_.start();
            emit pulsed(1.0);
            if (fps != 0) {
                pulseTimer_->start(static_cast<int>(py::round(1000.0 / fps)));
            }
        } else if (fps == 0) {
            emit pulsed(0.0);
        }
    }
}

void Heartbeat::pulseStep() {
    const double level = std::max(0.0, 1 - pulseClock_.nsecsElapsed() / 1e9 / kPulseSeconds);
    if (level == 0) {
        pulseTimer_->stop();
    }
    emit pulsed(level);
}

// ---- RotatingSvgWidget ----

RotatingSvgWidget::RotatingSvgWidget(const QString& svgFile, std::function<double()> rpmSource, QWidget* parent)
    : QWidget(parent),
      renderer_(new QSvgRenderer(svgFile, this)),
      rpmSource_(std::move(rpmSource)),
      spinTimer_(new QTimer(this)) {
    renderer_->setAspectRatioMode(Qt::KeepAspectRatio);
    clock_.start();
    lastStep_ = clock_.nsecsElapsed() / 1e9;
    spinTimer_->setTimerType(Qt::PreciseTimer);
    connect(spinTimer_, &QTimer::timeout, this, &RotatingSvgWidget::step);
    spinTimer_->start(kIdleMs);
}

// by the time since the last step rather than per tick, so the speed stays right even if frames are late
void RotatingSvgWidget::step() {
    const double now = clock_.nsecsElapsed() / 1e9;
    const double dt = now - lastStep_;
    lastStep_ = now;
    const double rpm = rpmSource_();
    if (rpm <= 0) {
        // stopped: nothing to redraw
        if (spinTimer_->interval() != kIdleMs) {
            spinTimer_->setInterval(kIdleMs);
            emit turned(angle_);
        }
        return;
    }
    if (const int frameMs = Performance::current().frameMs(); spinTimer_->interval() != frameMs) {
        spinTimer_->setInterval(frameMs);
        // (dt covered the idle wait: timing starts from here)
        return;
    }
    // rpm*360/60 degrees a second
    angle_ = py::mod(angle_ + rpm * 6 * dt, 360);
    update();
    emit turned(angle_);
}

void RotatingSvgWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    pixmap_ = QPixmap();
}

void RotatingSvgWidget::paintEvent(QPaintEvent*) {
    const int side = static_cast<int>(std::min(width(), height()) * scale);
    if (side <= 0) {
        return;
    }
    const double dpr = devicePixelRatioF();
    const int pixels = static_cast<int>(py::round(side * dpr));
    if (pixmap_.isNull() || pixmap_.devicePixelRatio() != dpr || pixmap_.width() != pixels) {
        pixmap_ = QPixmap(pixels, pixels);
        pixmap_.setDevicePixelRatio(dpr);
        pixmap_.fill(Qt::transparent);
        QPainter pixmapPainter(&pixmap_);
        pixmapPainter.setRenderHint(QPainter::Antialiasing);
        renderer_->render(&pixmapPainter, QRectF(0, 0, side, side));
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    // 80% transparent, so the gauge's tick marks and units show through
    painter.setOpacity(0.2);
    painter.translate(width() / 2.0, height() / 2.0);
    painter.rotate(angle_);
    painter.drawPixmap(QPointF(-side / 2.0, -side / 2.0), pixmap_);
}

// ---- Gauge ----

Gauge::Gauge(const QString& svgFile, const std::optional<Beat>& beat, bool spin, QWidget* parent)
    : QWidget(parent), gauge_(new AnalogGauge) {
    resize(400, 300);
    auto* layout = new QGridLayout(this);
    AnalogGauge* gauge = gauge_;
    recentClock_.start();
    const auto recentNow = [this] { return recentClock_.nsecsElapsed() / 1e9; };
    if (beat) {
        // the center point nested hearts in the color for the gauge's value (paler toward the middle, see
        // centerRingsPale), pulsing with each beat at its recent average
        recent_ = RecentAverage(false);
        connect(gauge, &AnalogGauge::valueChanged, this, [this, gauge, recentNow] {
            recent_.add(gauge->value() > gauge->minValue() ? gauge->value() : 0, recentNow());
        });
        heart_ = new Heartbeat([this, recentNow] { return recent_.at(recentNow()); }, this);
        gauge->centerHeart = true;
        heartColors_ = beat->colors;
        std::stable_sort(heartColors_.begin(), heartColors_.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        // (LayoutViewer lists them with the colors a theme can change)
        QStringList names;
        for (const auto& [value, color] : heartColors_) {
            names.append(color.name());
        }
        theme::setPaintedColors(gauge, names);
        connect(heart_, &Heartbeat::pulsed, this, [this](double level) {
            heartLevel_ = level;
            colorHeart();
        });
        connect(gauge, &AnalogGauge::valueChanged, this, &Gauge::colorHeart);
        connect(&theme::notifier(), &theme::Notifier::changed, this, &Gauge::colorHeart);
        connect(&theme::notifier(), &theme::Notifier::changed, this, &Gauge::colorArc);
        colorHeart();
    } else if (spin) {
        // the chainring spins at the recent average of the reading, in RPM
        connect(gauge, &AnalogGauge::valueChanged, this, [this, gauge, recentNow] {
            recent_.add(gauge->reading() > gauge->minValue() ? gauge->reading() : 0, recentNow());
        });
        auto* chainring =
            new RotatingSvgWidget(imagePath(svgFile), [this, recentNow] { return recent_.at(recentNow()); });
        // (with center rings, their outermost a gear turning with it)
        gauge->setCenterGearAngle(chainring->angle());
        connect(chainring, &RotatingSvgWidget::turned, gauge,
                [gauge](double angle) { gauge->setCenterGearAngle(angle); });
        background_ = chainring;
        layout->addWidget(background_, 0, 0);
    } else if (!svgFile.isEmpty()) {
        auto* svg = new QSvgWidget(imagePath(svgFile));
        svg->renderer()->setAspectRatioMode(Qt::KeepAspectRatio);
        background_ = svg;
        layout->addWidget(svg, 0, 0);
    }
    layout->addWidget(gauge_, 0, 0);
}

QColor Gauge::heartColor() const {
    return heartColors_.isEmpty() ? QColor() : themed(colorAt(heartColors_, gauge_->value()));
}

void Gauge::colorArc() {
    QList<std::pair<double, QColor>> stops = heartColors_;
    for (auto& [value, color] : stops) {
        color = themed(color);
    }
    gauge_->setValueColors(stops);
}

void Gauge::colorHeart() {
    const QColor color = heartColor();
    gauge_->setCenterRings(QList<QColor>{color, color}, heartLevel_);
}

}  // namespace awo

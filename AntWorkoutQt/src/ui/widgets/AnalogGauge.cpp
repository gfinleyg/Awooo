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

// text drawn centered on (x, y) as the original does it: a box its advance (plus 1) wide and its height tall, at
// whole pixels
void drawCentered(QPainter& painter, const QFontMetrics& fm, double x, double y, const QString& text) {
    const int w = fm.horizontalAdvance(text) + 1;
    const int h = fm.height();
    painter.drawText(static_cast<int>(x) - w / 2, static_cast<int>(y) - h / 2, w, h, Qt::AlignCenter, text);
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
    auto colorAt = [&stops](double value) {
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
    };
    QList<std::pair<double, QColor>> shown;
    for (const auto& [value, color] : stops) {
        if (minValue_ <= value && value <= maxValue_) {
            shown.append({value, color});
        }
    }
    const QList<std::pair<double, QColor>> ends = {{minValue_, colorAt(minValue_)}, {maxValue_, stops.back().second}};
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
    if (centerRings_ && rings && centerRings_->first == rings->first && std::abs(centerRings_->second - level) < 0.02) {
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
    needle_ = QPolygon({QPoint(4, 30), QPoint(-4, 30), QPoint(-2, -reach),
                        QPoint(0, -static_cast<int>(widgetDiameter_ / 2 * needleScaleFactor_ - 6)), QPoint(2, -reach)});
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
                                     valueFontname_);
    if (!fittedValueFontsize_ || fittedValueFontsize_->first != key) {
        fittedValueFontsize_ = std::pair(key, fitValueFontsize());
    }
    return fittedValueFontsize_->second;
}

// the largest font size (up to valueFontsize) where the widest possible value's glyphs stay inside the center circle
double AnalogGauge::fitValueFontsize() const {
    const double radius = (widgetDiameter_ / bigNeedleCenterSize - 1) * 0.92;
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

QPolygonF AnalogGauge::createPolygonPie(double outerRadius, double innerRadius, double start, int length,
                                        bool barGraph) {
    // (one step a degree: other step sizes draw it wrong)
    constexpr double w = 360 / 360.;
    if (!enableBarGraph_ && barGraph) {
        length = static_cast<int>(py::round((length / (maxValue_ - minValue_)) * (displayValue_ - minValue_)));
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
        drawCentered(painter, fm, textRadius * std::cos(radians(angle)), textRadius * std::sin(radians(angle)), text);
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

void AnalogGauge::createValuesText() {
    QPainter painter(paintTarget_);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.translate(width() / 2.0, height() / 2.0);
    const double fontsize = fitValueInCenter ? fittedValueFontsize() : valueFontsize_;
    const QFont font(valueFontname_, static_cast<int>(fontsize), QFont::Bold);
    const QFontMetrics fm(font);
    QPen pen;
    // (on the gauge's own white face: not themed)
    pen.setBrush(displayValueColor_);
    painter.setPen(pen);
    const double textRadius = altUnitsDisplay ? 0 : widgetDiameter_ / 2 * textRadiusFactor_;
    const QString text = valueDecimals ? py::fixed(reading_, valueDecimals)
                                       : QString::number(static_cast<long long>(reading_));
    painter.setFont(font);
    // halfway between the scale's start and end
    const double angleEnd = scaleAngleStartValue_ + scaleAngleSize_ - 360;
    const double angle = (angleEnd - scaleAngleStartValue_) / 2 + scaleAngleStartValue_;
    drawCentered(painter, fm, textRadius * std::cos(radians(angle)), textRadius * std::sin(radians(angle)), text);
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
    drawCentered(painter, fm, textRadius * std::cos(radians(angle)), textRadius * std::sin(radians(angle)), units);
}

void AnalogGauge::drawCenterRings(QPainter& painter, double radius) {
    const auto& [colors, level] = *centerRings_;
    const QColor white(QStringLiteral("#ffffff"));
    // the colors spread evenly from t 0 (outermost) to 1 (innermost), blended between
    auto shade = [&colors](double t) {
        const double at = t * (colors.size() - 1);
        const qsizetype i = std::min(static_cast<qsizetype>(at), colors.size() - 2);
        return mix(colors[i], colors[i + 1], at - i);
    };
    const auto [paleOuter, paleInner] = centerRingsPale;
    const int count = ringCount();
    // (each ring's width)
    const double band = radius * kCenterRingSpread / (count - 1);
    for (int i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) / (count - 1);
        const double r = radius * (1 - kCenterRingSpread * t) * (1 + 0.04 * level);
        // paler toward the middle (the reading is drawn over it), less pale on the beat
        const QColor color = mix(shade(t), white, (paleOuter + (paleInner - paleOuter) * t) * (1 - 0.6 * level));
        QRadialGradient grad(QPointF(0, 0), r);
        grad.setColorAt(0, color.lighter(112));
        grad.setColorAt(1, color.darker(118));
        painter.setBrush(grad);
        // (outlines thin enough to leave narrow rings their color)
        painter.setPen(QPen(color.darker(140), std::max(1.0, std::min(radius * 0.02, band * 0.15))));
        if (i == 0 && centerGearAngle_) {
            painter.save();
            painter.rotate(*centerGearAngle_);
            painter.drawPolygon(centerGear(r, band * kCenterGearDepth));
            painter.restore();
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
    const double radius = widgetDiameter_ / bigNeedleCenterSize - pen_.width() / 2.0;
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

// the target arc, centered on arcCenterValue, wider the higher it is
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
    const QPolygonF arc = createPolygonPie(widgetDiameter_ / 1.95 - pen_.width(), widgetDiameter_ / 6, start, arcWidth,
                                           false);
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
    auto drawLayer = [this](QPixmap& layer, void (AnalogGauge::*draw)()) {
        paintTarget_ = &layer;
        (this->*draw)();
        paintTarget_ = this;
    };
    if (paintCacheDirty_) {
        faceUnder_ = newLayer(size, dpr);
        drawLayer(faceUnder_, &AnalogGauge::drawFaceUnder);
        faceOver_ = newLayer(size, dpr);
        drawLayer(faceOver_, &AnalogGauge::drawFaceOver);
        paintCacheDirty_ = false;
        frameDirty_ = true;
    }
    if (frameDirty_) {
        paintCache_.fill(Qt::transparent);
        drawLayer(paintCache_, &AnalogGauge::drawFrame);
        frameDirty_ = false;
    }
    QPainter(this).drawPixmap(0, 0, paintCache_);
}

// the cached face under and over the band, then every frame the band and everything that moves over them, in the order
// the whole gauge was always drawn in
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

void AnalogGauge::drawFrame() {
    QPainter(paintTarget_).drawPixmap(0, 0, faceUnder_);
    if (enableFilledPolygon_) {
        drawFilledPolygon();
    }
    // drawing the big scale markers (cached in the face, so not drawn here) leaves the pen 2 wide after the band sets
    // it to 0, and the center point, the arc and the next face's outer circle are sized by that width
    if (enableBigScaledMarker_) {
        pen_.setWidth(2);
    }
    QPainter(paintTarget_).drawPixmap(0, 0, faceOver_);
    if (enableNeedlePolygon_) {
        drawNeedle();
    }
    if (enableCenterPoint_) {
        drawBigNeedleCenterPoint();
    }
    if (enableValueText_) {
        createValuesText();
        createUnitsText();
    }
    if (enableDrawNewArc_) {
        drawNewArc();
    }
}

// ---- BeatingSvgWidget ----

BeatingSvgWidget::BeatingSvgWidget(const QString& restSvgFile, const QString& beatSvgFile, const QColor& restColor,
                                   const QColor& beatColor, std::function<double()> bpmSource, QWidget* parent)
    : QWidget(parent), bpmSource_(std::move(bpmSource)), beatTimer_(new QTimer(this)) {
    restRenderer_ = coloredRenderer(restSvgFile, restColor);
    beatRenderer_ = coloredRenderer(beatSvgFile, beatColor);
    beatTimer_->setSingleShot(true);
    beatTimer_->setTimerType(Qt::PreciseTimer);
    connect(beatTimer_, &QTimer::timeout, this, &BeatingSvgWidget::nextPhase);
    beatTimer_->start(0);
}

QSvgRenderer* BeatingSvgWidget::coloredRenderer(const QString& svgFile, const QColor& color) {
    QFile file(svgFile);
    QByteArray svg;
    if (file.open(QIODevice::ReadOnly)) {
        svg = file.readAll();
    }
    svg.replace(QByteArrayLiteral("fill=\"#000000\""), QByteArrayLiteral("fill=\"") + color.name().toLatin1() + '"');
    auto* renderer = new QSvgRenderer(svg, this);
    renderer->setAspectRatioMode(Qt::KeepAspectRatio);
    return renderer;
}

// the rate is read again every beat, so it follows the heart rate as it changes
void BeatingSvgWidget::nextPhase() {
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

void BeatingSvgWidget::setBeating(bool beating) {
    if (beating != beating_) {
        beating_ = beating;
        update();
    }
}

void BeatingSvgWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    (beating_ ? beatRenderer_ : restRenderer_)->render(&painter, QRectF(rect()));
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
    if (spinTimer_->interval() != kFrameMs) {
        spinTimer_->setInterval(kFrameMs);
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
    if (beat) {
        // the heart beats at the gauge's value
        background_ = new BeatingSvgWidget(imagePath(svgFile), imagePath(beat->svgFile), beat->restColor,
                                           beat->beatColor,
                                           [gauge] { return gauge->value() > gauge->minValue() ? gauge->value() : 0; });
        layout->addWidget(background_, 0, 0);
    } else if (spin) {
        // the chainring spins at the reading, in RPM
        auto* chainring = new RotatingSvgWidget(
            imagePath(svgFile), [gauge] { return gauge->reading() > gauge->minValue() ? gauge->reading() : 0; });
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

}  // namespace awo

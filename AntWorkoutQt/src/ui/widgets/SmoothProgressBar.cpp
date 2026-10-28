#include "SmoothProgressBar.hpp"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

#include "Theme.hpp"

namespace awo {

SmoothProgressBar::SmoothProgressBar(QWidget* parent) : QProgressBar(parent) {
    connect(&theme::notifier(), &theme::Notifier::changed, this, qOverload<>(&QWidget::update));
}

void SmoothProgressBar::setBarColors(const QColor& chunkColor, const QColor& barColor) {
    chunkColor_ = chunkColor;
    barColor_ = barColor;
    themed_ = true;
    update();
}

QColor SmoothProgressBar::chunkColor() const {
    return themed_ ? theme::current().mapColor(chunkColor_) : chunkColor_;
}

std::optional<std::pair<QColor, QColor>> SmoothProgressBar::originalBarColors() const {
    if (!themed_) {
        return std::nullopt;
    }
    return std::pair{chunkColor_, barColor_};
}

QColor SmoothProgressBar::barColor() const {
    return themed_ ? theme::current().mapColor(barColor_) : barColor_;
}

void SmoothProgressBar::setFraction(double fraction) {
    fraction = std::min(std::max(fraction, 0.0), 1.0);
    if (fraction != fraction_) {
        fraction_ = fraction;
        update();
    }
}

void SmoothProgressBar::setValue(int value) {
    fraction_.reset();
    QProgressBar::setValue(value);
    update();
}

void SmoothProgressBar::setRange(int minimum, int maximum) {
    fraction_.reset();
    QProgressBar::setRange(minimum, maximum);
    update();
}

// an eighth of the height, at least 2px
double SmoothProgressBar::frameWidth() const {
    return std::max(2.0, std::round(height() / 8.0));
}

// the timer labels' rounding: 2px short of half the height
double SmoothProgressBar::cornerRadius() const {
    return std::max(0.0, height() / 2.0 - 2);
}

void SmoothProgressBar::paintEvent(QPaintEvent*) {
    double fraction = 0.0;
    if (fraction_) {
        fraction = *fraction_;
    } else {
        const int span = maximum() - minimum();
        fraction = span > 0 ? static_cast<double>(value() - minimum()) / span : 0.0;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    const double radius = cornerRadius();
    painter.setBrush(barColor());
    painter.drawRoundedRect(QRectF(rect()), radius, radius);
    // the fill sits inside a frame of the bar color, so it never touches the window behind (in a light theme the two
    // can be the same shade), rounded at its start and square at its leading edge
    const double frame = frameWidth();
    const QRectF inner = QRectF(rect()).adjusted(frame, frame, -frame, -frame);
    const double innerRadius = std::max(0.0, radius - frame);
    QPainterPath track;
    track.addRoundedRect(inner, innerRadius, innerRadius);
    QPainterPath filled;
    filled.addRect(QRectF(inner.x(), inner.y(), inner.width() * fraction, inner.height()));
    // (a path, not a clip, so the antialiasing blends the fractional pixel at the leading edge and the corners)
    painter.setBrush(chunkColor());
    painter.drawPath(track.intersected(filled));
}

}  // namespace awo

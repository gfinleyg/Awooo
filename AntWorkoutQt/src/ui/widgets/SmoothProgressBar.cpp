#include "SmoothProgressBar.hpp"

#include <QPainter>

#include <algorithm>

#include "Theme.hpp"

namespace awo {

SmoothProgressBar::SmoothProgressBar(QWidget* parent) : QProgressBar(parent) {
    // (the whole rect is painted every time)
    setAttribute(Qt::WA_OpaquePaintEvent);
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
    const QRectF rect(this->rect());
    painter.fillRect(rect, barColor());
    // drawRect (not fillRect) so the antialiasing blends the fractional pixel at the leading edge
    painter.setPen(Qt::NoPen);
    painter.setBrush(chunkColor());
    painter.drawRect(QRectF(rect.x(), rect.y(), rect.width() * fraction, rect.height()));
}

}  // namespace awo

#include "HatchedLabel.hpp"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

#include "HatchTile.hpp"
#include "Theme.hpp"

namespace awo {

namespace {

// past each end of the text: where the lines have fully come back, and how far they take to fade in, in design px
constexpr double kClearPast = 13;
constexpr double kFade = 9;

}  // namespace

HatchedLabel::HatchedLabel(const QString& text, QWidget* parent) : QLabel(text, parent) {}

void HatchedLabel::setHatchBackground(const QColor& color) {
    if (background_ != color) {
        background_ = color;
        update();
    }
}

QPixmap HatchedLabel::buildHatch(const QColor& ink, double scale) const {
    const double ratio = devicePixelRatioF();
    QPixmap pixmap(size() * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF box(rect());
    // (the lines run on from one box to the next)
    const QPointF anchor = -QPointF(mapTo(window(), QPoint()));
    QPainterPath shape;
    const double radius = std::min(std::max(1.0, std::round(kBoxRadius * scale)), box.height() / 2);
    shape.addRoundedRect(box, radius, radius);
    painter.setClipPath(shape);
    plot::drawHatch(painter, rect(), plot::hatchTile(ink, scale, ratio), anchor);
    painter.setClipping(false);
    // taken away behind the text
    int textWidth = 0;
    const QFontMetrics metrics = fontMetrics();
    const QStringList lines = text().split(u'\n');
    for (const QString& line : lines) {
        textWidth = std::max(textWidth, metrics.horizontalAdvance(line));
    }
    if (textWidth > 0) {
        const double clear = textWidth + 2 * kClearPast * scale;
        const double left = box.center().x() - clear / 2;
        const double fade = std::min(0.5, kFade * scale / clear);
        QLinearGradient gradient(left, 0, left + clear, 0);
        gradient.setColorAt(0, QColor(0, 0, 0, 0));
        gradient.setColorAt(fade, Qt::black);
        gradient.setColorAt(1 - fade, Qt::black);
        gradient.setColorAt(1, QColor(0, 0, 0, 0));
        painter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        painter.fillRect(QRectF(left, 0, clear, box.height()), gradient);
    }
    return pixmap;
}

void HatchedLabel::paintEvent(QPaintEvent* event) {
    QLabel::paintEvent(event);
    if (isEnabled() || !background_.isValid() || size().isEmpty()) {
        return;
    }
    const QColor ink = plot::hatchInk(theme::current().mapColor(background_));
    const double scale = theme::widgetScale(this);
    const QPoint at = mapTo(window(), QPoint());
    const QString key = QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8")
                            .arg(ink.rgba(), 0, 16)
                            .arg(scale, 0, 'f', 4)
                            .arg(devicePixelRatioF(), 0, 'f', 3)
                            .arg(width())
                            .arg(height())
                            .arg(fontMetrics().horizontalAdvance(text()))
                            .arg(at.x())
                            .arg(at.y());
    if (key != hatchKey_) {
        hatchKey_ = key;
        hatch_ = buildHatch(ink, scale);
        ++hatchBuilds_;
    }
    QPainter painter(this);
    painter.drawPixmap(0, 0, hatch_);
}

}  // namespace awo

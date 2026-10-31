#include "HaloLabel.hpp"

#include <QImage>
#include <QPainter>
#include <QStyle>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "Theme.hpp"
#include "ZoneColors.hpp"
#include "ZoneLegend.hpp"

namespace awo {

namespace {

// the copies of the text around it that make the halo
constexpr int kHaloSteps = 16;

}  // namespace

HaloLabel::HaloLabel(const QString& text, QWidget* parent) : QLabel(text, parent) {
    connect(&theme::notifier(), &theme::Notifier::changed, this, qOverload<>(&QWidget::update));
}

QRect HaloLabel::layoutRect() const {
    // (as QLabel lays out its text: inside its margin, and indented on the side it's aligned to)
    QRect rect = contentsRect().adjusted(margin(), margin(), -margin(), -margin());
    const int indent = std::max(0, this->indent());
    const Qt::Alignment align = QStyle::visualAlignment(layoutDirection(), alignment());
    if (align & Qt::AlignLeft) {
        rect.setLeft(rect.left() + indent);
    }
    if (align & Qt::AlignRight) {
        rect.setRight(rect.right() - indent);
    }
    if (align & Qt::AlignTop) {
        rect.setTop(rect.top() + indent);
    }
    if (align & Qt::AlignBottom) {
        rect.setBottom(rect.bottom() - indent);
    }
    return rect;
}

QRectF HaloLabel::textRect() const {
    const int flags = QStyle::visualAlignment(layoutDirection(), alignment());
    return fontMetrics().boundingRect(layoutRect(), flags, text());
}

bool HaloLabel::haloed() const {
    if (text().isEmpty()) {
        return false;
    }
    // (checked at both ends and the middle: the gradient runs across it)
    const QRectF rect = textRect();
    const QColor color = palette().color(foregroundRole());
    double lowest = 21;
    for (const QPointF& point : {QPointF(rect.left(), rect.center().y()), rect.center(),
                                 QPointF(rect.right(), rect.center().y())}) {
        lowest = std::min(lowest, theme::contrast(color, zones::windowBackgroundAt(this, point)));
    }
    return lowest < ZoneLegend::kMinContrast;
}

void HaloLabel::paintEvent(QPaintEvent* event) {
    if (haloed()) {
        // the text in the halo color all around where it's drawn (laid out as QLabel lays it out), on a layer of its
        // own so the overlaps don't add up, then the layer at the halo's alpha
        const double dpr = devicePixelRatioF();
        QImage layer((QSizeF(size()) * dpr).toSize(), QImage::Format_ARGB32_Premultiplied);
        layer.setDevicePixelRatio(dpr);
        layer.fill(Qt::transparent);
        QPainter haloPainter(&layer);
        haloPainter.setRenderHint(QPainter::TextAntialiasing);
        haloPainter.setFont(font());
        const QColor color = palette().color(foregroundRole());
        const QColor halo = ftpcolors::haloColor(color.name(), 0xff);
        haloPainter.setPen(halo);
        const int flags = QStyle::visualAlignment(layoutDirection(), alignment());
        const double radius = ZoneLegend::kHaloPx * theme::widgetScale(this);
        for (int step = 0; step < kHaloSteps; ++step) {
            const double angle = 2 * std::numbers::pi * step / kHaloSteps;
            const QPointF offset(radius * std::cos(angle), radius * std::sin(angle));
            haloPainter.drawText(QRectF(layoutRect()).translated(offset), flags, text());
        }
        haloPainter.end();
        QPainter painter(this);
        painter.setOpacity(ZoneLegend::kHaloAlpha / 255.0);
        painter.drawImage(QPointF(0, 0), layer);
    }
    // the text itself, over it
    QLabel::paintEvent(event);
}

}  // namespace awo

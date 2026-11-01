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

// the copies of the text around it that make the halo, one device pixel out: as thin as an outline can be
constexpr int kHaloSteps = 8;
constexpr double kHaloDevicePx = 1;
// how far the outline stands out from the text: WCAG's minimum for an edge to be seen (non-text contrast)
constexpr double kHaloContrast = 3.0;

// the lightest of the colors the theme gives the backgrounds (its colors but the text's)
QColor lightestBackground() {
    QColor lightest;
    for (const QString& key : theme::kThemeKeys) {
        if (key == u"#000000" || key == theme::kButtonText || key == theme::kLightText) {
            continue;
        }
        const QColor color(theme::current().mapHex(key));
        if (!lightest.isValid() || theme::luminance(color) > theme::luminance(lightest)) {
            lightest = color;
        }
    }
    return lightest;
}

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

QColor HaloLabel::haloColor() const {
    // the background behind the text, taken toward the theme's lightest background only as far as it needs to go to
    // stand out from the text (all the way, if even that doesn't)
    const QString text = palette().color(foregroundRole()).name();
    const QString behind = zones::windowBackgroundAt(this, textRect().center()).name();
    const QString lightest = lightestBackground().name();
    const auto standsOut = [&](double t) {
        return theme::contrast(QColor(colors::mix(behind, lightest, t)), QColor(text)) >= kHaloContrast;
    };
    if (!standsOut(1)) {
        return QColor(lightest);
    }
    double low = 0;
    double high = 1;
    for (int i = 0; i < 12; ++i) {
        const double mid = (low + high) / 2;
        (standsOut(mid) ? high : low) = mid;
    }
    return QColor(colors::mix(behind, lightest, high));
}

void HaloLabel::paintEvent(QPaintEvent* event) {
    if (haloed()) {
        // the text in the halo color all around where it's drawn (laid out as QLabel lays it out), on a layer of its
        // own so the copies' antialiased edges don't add up
        const double dpr = devicePixelRatioF();
        QImage layer((QSizeF(size()) * dpr).toSize(), QImage::Format_ARGB32_Premultiplied);
        layer.setDevicePixelRatio(dpr);
        layer.fill(Qt::transparent);
        QPainter haloPainter(&layer);
        haloPainter.setRenderHint(QPainter::TextAntialiasing);
        haloPainter.setFont(font());
        haloPainter.setPen(haloColor());
        const int flags = QStyle::visualAlignment(layoutDirection(), alignment());
        const double radius = kHaloDevicePx / dpr;
        for (int step = 0; step < kHaloSteps; ++step) {
            const double angle = 2 * std::numbers::pi * step / kHaloSteps;
            const QPointF offset(radius * std::cos(angle), radius * std::sin(angle));
            haloPainter.drawText(QRectF(layoutRect()).translated(offset), flags, text());
        }
        haloPainter.end();
        QPainter painter(this);
        painter.drawImage(QPointF(0, 0), layer);
    }
    // the text itself, over it
    QLabel::paintEvent(event);
}

}  // namespace awo

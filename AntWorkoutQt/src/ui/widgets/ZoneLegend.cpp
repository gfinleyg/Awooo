#include "ZoneLegend.hpp"

#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

#include "AppState.hpp"
#include "GradeWidgets.hpp"
#include "LongVLabels.hpp"
#include "PyCompat.hpp"
#include "Theme.hpp"
#include "ZoneColors.hpp"

namespace awo {

namespace {

const QString kTextColor = QStringLiteral("#000000");

QFont labelFont(double scale) {
    QFont font(QStringLiteral("Orbitron"));
    font.setPointSizeF(ZoneLegend::kLabelPoints);
    font.setWeight(QFont::Black);
    return theme::current().mapFont(font, scale);
}

}  // namespace

ZoneLegend::ZoneLegend(QWidget* parent) : QWidget(parent) {
    setFixedSize(LongVLabels::kLabelWidth, kHeight);
    // (LayoutViewer lists it with the colors a theme can change)
    theme::setPaintedColors(this, {kTextColor});
    connect(&theme::notifier(), &theme::Notifier::changed, this, qOverload<>(&QWidget::update));
    // redrawn when the zone colors change
    connect(&zones::notifier(), &zones::Notifier::changed, this, qOverload<>(&QWidget::update));
}

QStringList ZoneLegend::labelTexts() const {
    const QString unit =
        appState().targetUnit() == u"Watts" ? QStringLiteral("W") : appState().targetUnit().toLower();
    QStringList texts;
    for (double limit : zones::appRanges().limits) {
        texts.append(QString::number(static_cast<int>(py::round(limit))) + unit);
    }
    return texts;
}

// the zones side by side, each fading from its pale end to its color, a hard edge where the next starts
QLinearGradient ZoneLegend::barGradient(const QRectF& rect) const {
    const auto stops = zones::zoneStops(zones::appRanges(), kStopSteps);
    QLinearGradient gradient(rect.left(), 0, rect.right(), 0);
    for (int zone = 0; zone < ftpcolors::kZones; ++zone) {
        for (int i = 0; i <= kStopSteps; ++i) {
            double at = (zone + static_cast<double>(i) / kStopSteps) / ftpcolors::kZones;
            if (i == kStopSteps && zone < ftpcolors::kZones - 1) {
                at -= 1e-4;
            }
            gradient.setColorAt(at, stops[zone * (kStopSteps + 1) + i].second);
        }
    }
    return gradient;
}

QColor ZoneLegend::backgroundAt(const QPointF& point) const {
    const QWidget* top = window();
    const QPointF at = mapTo(top, point);
    const double t = (at.x() / std::max(1, top->width()) + at.y() / std::max(1, top->height())) / 2;
    const QString start = theme::current().mapHex(ftpcolors::kWindowGradient[0]);
    const QString end = theme::current().mapHex(ftpcolors::kWindowGradient[1]);
    return QColor(colors::mix(start, end, std::clamp(t, 0.0, 1.0)));
}

QList<QPointF> ZoneLegend::labelCenters(double* textTop, double* textHeight) const {
    const double top = kBarHeight * theme::widgetScale(this) + kTextGap * theme::widgetScale(this);
    const double rowHeight = height() - top;
    const int zones = ftpcolors::kZones;
    QList<QPointF> centers;
    for (int k = 1; k < zones; ++k) {
        centers.append({width() * static_cast<double>(k) / zones, top + rowHeight / 2});
    }
    if (textTop) {
        *textTop = top;
    }
    if (textHeight) {
        *textHeight = rowHeight;
    }
    return centers;
}

bool ZoneLegend::haloed() const {
    const QColor color(theme::current().mapHex(kTextColor));
    double lowest = 21;
    for (const QPointF& center : labelCenters()) {
        lowest = std::min(lowest, theme::contrast(color, backgroundAt(center)));
    }
    return lowest < kMinContrast;
}

void ZoneLegend::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const double scale = theme::widgetScale(this);
    const QRectF bar(0, 0, width(), kBarHeight * scale);
    painter.setPen(Qt::NoPen);
    painter.setBrush(barGradient(bar));
    painter.drawRoundedRect(bar, bar.height() / 2, bar.height() / 2);

    const QStringList texts = labelTexts();
    double textTop = 0;
    double textHeight = 0;
    const QList<QPointF> centers = labelCenters(&textTop, &textHeight);
    QFont font = labelFont(scale);
    // (all the same size: as big as the widest fits in a zone's width)
    const QFontMetricsF unfitted(font);
    const QString widest = *std::max_element(texts.begin(), texts.end(), [&](const QString& a, const QString& b) {
        return unfitted.horizontalAdvance(a) < unfitted.horizontalAdvance(b);
    });
    font = fittedFont(font, widest, width() / static_cast<double>(ftpcolors::kZones) * 0.92, textHeight);
    const QFontMetricsF metrics(font);
    const double baseline = textTop + (textHeight - metrics.height()) / 2 + metrics.ascent();
    QPainterPath path;
    for (qsizetype k = 0; k < texts.size() && k < centers.size(); ++k) {
        path.addText(QPointF(centers[k].x() - metrics.horizontalAdvance(texts[k]) / 2, baseline), font, texts[k]);
    }

    const QColor color(theme::current().mapHex(kTextColor));
    if (haloed()) {
        painter.setPen(QPen(ftpcolors::haloColor(color.name(), kHaloAlpha), 2 * kHaloPx * scale, Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPath(path);
}

}  // namespace awo

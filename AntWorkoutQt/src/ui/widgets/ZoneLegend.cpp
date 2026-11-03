#include "ZoneLegend.hpp"

#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

#include "GradeWidgets.hpp"
#include "HaloLabel.hpp"
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

// where each zone but the top one ends, as the block labels show a target
QStringList limitTexts(const ftpcolors::ZoneRanges& ranges, const QString& unit) {
    QStringList texts;
    for (double limit : ranges.limits) {
        texts.append(QString::number(static_cast<int>(py::round(limit))) + unit);
    }
    return texts;
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

QStringList ZoneLegend::rpmTexts() const {
    return limitTexts(ftpcolors::cadenceRanges(ftpcolors::kCadenceScaleBottomRpm), QStringLiteral("rpm"));
}

QStringList ZoneLegend::powerTexts() const {
    return limitTexts(zones::powerRanges(), QStringLiteral("W"));
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
    return zones::windowBackgroundAt(this, point);
}

QRectF ZoneLegend::rpmRow() const {
    return {0, 0, static_cast<double>(width()), kRowHeight * theme::widgetScale(this)};
}

QRectF ZoneLegend::barRect() const {
    const double scale = theme::widgetScale(this);
    return {0, rpmRow().bottom() + kTextGap * scale, static_cast<double>(width()), kBarHeight * scale};
}

QRectF ZoneLegend::powerRow() const {
    const double top = barRect().bottom() + kTextGap * theme::widgetScale(this);
    return {0, top, static_cast<double>(width()), height() - top};
}

QList<QPointF> ZoneLegend::labelCenters(const QRectF& row) const {
    const int zones = ftpcolors::kZones;
    QList<QPointF> centers;
    for (int k = 1; k < zones; ++k) {
        centers.append({row.width() * k / zones, row.center().y()});
    }
    return centers;
}

bool ZoneLegend::haloed() const {
    const QColor color(theme::current().mapHex(kTextColor));
    double lowest = 21;
    for (const QPointF& center : labelCenters(rpmRow()) + labelCenters(powerRow())) {
        lowest = std::min(lowest, theme::contrast(color, backgroundAt(center)));
    }
    return lowest < halo::kMinContrast;
}

void ZoneLegend::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const double scale = theme::widgetScale(this);
    const QRectF bar = barRect();
    painter.setPen(Qt::NoPen);
    painter.setBrush(barGradient(bar));
    painter.drawRoundedRect(bar, bar.height() / 2, bar.height() / 2);

    const QList<std::pair<QRectF, QStringList>> rows{{rpmRow(), rpmTexts()}, {powerRow(), powerTexts()}};
    // (all the same size: as big as the widest of either row fits in a zone's width)
    QFont font = labelFont(scale);
    const QFontMetricsF unfitted(font);
    QString widest;
    for (const auto& row : rows) {
        for (const QString& text : row.second) {
            if (unfitted.horizontalAdvance(text) > unfitted.horizontalAdvance(widest)) {
                widest = text;
            }
        }
    }
    font = fittedFont(font, widest, width() / static_cast<double>(ftpcolors::kZones) * 0.92,
                      std::min(rows[0].first.height(), rows[1].first.height()));
    const QFontMetricsF metrics(font);
    // (each number its own path: each one's outline takes the gradient behind it)
    QList<std::pair<QPainterPath, QPointF>> numbers;
    for (const auto& [row, texts] : rows) {
        const QList<QPointF> centers = labelCenters(row);
        const double baseline = row.top() + (row.height() - metrics.height()) / 2 + metrics.ascent();
        for (qsizetype k = 0; k < texts.size() && k < centers.size(); ++k) {
            QPainterPath path;
            path.addText(QPointF(centers[k].x() - metrics.horizontalAdvance(texts[k]) / 2, baseline), font, texts[k]);
            numbers.append({path, centers[k]});
        }
    }

    const QColor color(theme::current().mapHex(kTextColor));
    if (haloed()) {
        painter.setBrush(Qt::NoBrush);
        for (const auto& [path, center] : numbers) {
            painter.setPen(QPen(halo::color(color, backgroundAt(center)), 2 * halo::kDevicePx / devicePixelRatioF(),
                                Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawPath(path);
        }
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    for (const auto& [path, center] : numbers) {
        painter.drawPath(path);
    }
}

}  // namespace awo

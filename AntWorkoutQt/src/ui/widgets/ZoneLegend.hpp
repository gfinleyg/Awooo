#pragma once

// The workout screen's zone legend, under the coming up blocks and as wide as them: the zones' colors in a bar, each
// zone the same width and fading the way the gauges' bands do (zones::zoneStops), with where each zone ends under the
// edge between it and the next, in both units whatever the mode: the cadence zones' RPM above the bar (Musical Cadence
// Mode's zones, see ftpcolors::cadenceRanges) and the power zones' watts below it (the FTP's, zones::powerRanges).
//
// The numbers are in the theme's text color. Where that doesn't stand out from the window's gradient behind them
// (contrast under kMinContrast: a mid-tone gradient reads poorly with dark and light text alike), they get a halo,
// light under dark text and dark under light (the graph's sensor lines' halo colors, ftpcolors::haloColor).

#include <QRectF>
#include <QWidget>

class QLinearGradient;

namespace awo {

class ZoneLegend : public QWidget {
    Q_OBJECT

public:
    // design px: the RPM, the bar, then the watts
    static constexpr double kRowHeight = 22;
    static constexpr double kBarHeight = 20;
    static constexpr double kTextGap = 3;
    static constexpr int kHeight = static_cast<int>(2 * kRowHeight + 2 * kTextGap + kBarHeight);
    static constexpr double kLabelPoints = 11;
    // the bar's fade steps in each zone (as the gauges' bands)
    static constexpr int kStopSteps = 6;
    // WCAG AA for text: numbers that contrast less than this with the window's gradient behind them get a halo,
    // kHaloPx (design px) out from their outline
    static constexpr double kMinContrast = 4.5;
    static constexpr double kHaloPx = 2.5;
    static constexpr int kHaloAlpha = 0xd0;

    explicit ZoneLegend(QWidget* parent = nullptr);

    // where each zone but the top one ends: in RPM (the row above the bar) and in watts (the row below it)
    QStringList rpmTexts() const;
    QStringList powerTexts() const;
    // the window's background (its gradient corner to corner, through the theme) behind a point of this
    QColor backgroundAt(const QPointF& point) const;
    // whether the numbers are drawn over a halo (with the theme as it is and where this is in its window)
    bool haloed() const;
    // the bar, and the rows of numbers above and below it
    QRectF barRect() const;
    QRectF rpmRow() const;
    QRectF powerRow() const;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QLinearGradient barGradient(const QRectF& rect) const;
    // the centers of a row's numbers, one under each edge between zones
    QList<QPointF> labelCenters(const QRectF& row) const;
};

}  // namespace awo

#pragma once

// The analog gauges (ClassAnalogGauge.py, after Stefan Holstein's analoggaugewidget): the power, cadence and heart rate
// gauges on the workout screen. A scale with tick marks and numbers, a colored band up to the needle, a needle that
// eases toward each reading at the Performance setting's frame rate, the reading and units in the middle, nested
// center rings (the power gauge's recent blocks, the cadence gauge's beat), and the Ride At target arc.
//
// It's drawn in layers kept as pixmaps: the face under the band (the outer ring) and over it (ticks, numbers) are
// redrawn only by redraw(); each frame (redrawFrame(): a reading, the needle easing, the rings, the arc) only the
// moving parts are drawn over them. Repaints from an animated widget behind it just copy the last frame.
//
// Gauge: the gauge over a background image: a resting/beating heart (heart rate) or a spinning chainring (cadence).

#include <QElapsedTimer>
#include <QGradient>
#include <QPen>
#include <QPixmap>
#include <QPolygon>
#include <QWidget>

#include <functional>
#include <map>
#include <optional>
#include <tuple>

class QSvgRenderer;
class QTimer;

namespace awo {

class AnalogGauge : public QWidget {
    Q_OBJECT

public:
    // the time constant of the needle's easing toward a new reading
    static constexpr double kNeedleEaseSeconds = 0.11;
    // the rings fill the center from its edge in to this much of its radius (the innermost a disc that size)
    static constexpr double kCenterRingSpread = 0.6;

    explicit AnalogGauge(QWidget* parent = nullptr);

    // ---- The reading ----

    // the needle (held within the scale) and the reading in the middle (not: a heart rate gauge's 0 after a dropout
    // reads 0, not its 40 BPM minimum)
    void setValue(double value);
    double value() const { return value_; }
    double reading() const { return reading_; }
    // where the needle is drawn now, easing toward value()
    double displayValue() const { return displayValue_; }
    // the needle straight to value(), not on its way there (a picture of the gauge)
    void snapNeedle();
    void setMinValue(double min);
    void setMaxValue(double max);
    double minValue() const { return minValue_; }
    double maxValue() const { return maxValue_; }

    // ---- How it looks (set while it's built; redraw() after changing one later) ----

    // 0: the original's dark theme; 3: white with black ticks (the app's)
    void setGaugeTheme(int theme);
    void setNeedleColor(const QColor& color);
    void setScalaCount(int count);
    int scalaCount() const { return scalaCount_; }
    void setEnableBarGraph(bool enable);
    void setEnableBigScaleGrid(bool enable);
    void setEnableDrawNewArc(bool enable = true);
    // the band's colors as (position, color) stops along its conical gradient
    void setScalePolygonColors(const std::optional<QGradientStops>& stops);
    // the band from (value, color) stops: each color exactly where the needle points at its value; the end of the
    // scale has the highest value's color (even past the end), stops below the start blend into its color there
    void setValueColors(QList<std::pair<double, QColor>> stops);
    double valueGradientPosition(double value) const;
    // the ring behind the band (the power gauge's follows the power ridden): part of the face, redrawn on a change
    void setOuterCircleBg(const QGradientStops& stops);
    QGradientStops outerCircleBg() const { return outerCircleBg_; }
    // nested circles in place of the center point, shaded through colors from the outermost to the innermost (two or
    // more), or the center point back (none). level (0 to 1) brightens them and swells them a little, e.g. on a beat
    void setCenterRings(const std::optional<QList<QColor>>& colors, double level = 0.0);
    // how many rings: centerRingCount, or with centerRingMinPx as many as fit at its size with each that wide
    int ringCount() const;
    // the target arc: centered on arcCenterValue, wider for a higher value
    void setArc(double centerValue, const QGradientStops& colors);

    QString units = QStringLiteral("℃");
    int scalaSubdivCount = 5;
    int valueDecimals = 0;
    // the reading shrunk so the widest possible one fits in the center circle
    bool fitValueInCenter = false;
    // the units at the bottom and the reading in the middle
    bool altUnitsDisplay = false;
    // smaller: a bigger center circle
    double bigNeedleCenterSize = 8;
    double initialScaleFontsize = 14;
    double initialValueFontsize = 40;
    double gaugeColorOuterRadiusFactor = 1;
    double gaugeColorInnerRadiusFactor = 0.9;
    QGradientStops needleCenterBg;
    QColor bigScaleMarker{QStringLiteral("#000000")};
    QColor fineScaleColor{QStringLiteral("#000000")};
    QGradientStops newArcColors{{0, QColor(255, 255, 255, 155)}};
    double arcCenterValue = 600;
    // multiplier for the arc's widening with its value, 0 for none
    double increasePercentage = 1;
    int centerRingCount = 4;
    std::optional<double> centerRingMinPx;
    // how much the rings are blended toward white, outermost and innermost (the reading is drawn over the middle)
    std::pair<double, double> centerRingsPale{0.15, 0.7};

    // ---- Redrawing ----

    // everything again, its face included (a look changed)
    void redraw();
    // the moving parts again (needle, band, reading, center, arc) over the cached face
    void redrawFrame();
    // the font sizes again from the gauge's size and the theme's Gauge numbers font (on resize and theme changes)
    void rescale();

signals:
    void valueChanged(int value);
    // a resize changed how many center rings fit (centerRingMinPx): whatever colors them sets them again for it
    void ringCountChanged(int count);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void applyPerformance();
    void animateNeedle();
    double fittedValueFontsize();
    double fitValueFontsize() const;
    QPolygonF createPolygonPie(double outerRadius, double innerRadius, double start, int length, bool barGraph = true);

    void drawFaceUnder();
    void drawFaceOver();
    void drawFrame();
    void drawFilledPolygon(int outlinePenWidth = 0);
    void drawBigScaledMarker();
    void createScaleMarkerValuesText();
    void createFineScaledMarker();
    void createValuesText();
    void createUnitsText();
    void drawCenterRings(QPainter& painter, double radius);
    void drawBigNeedleCenterPoint();
    void drawOuterCircle();
    void drawNewArc();
    void drawNeedle();
    QPixmap newLayer(const QSize& size, double dpr) const;

    double minValue_ = 0, maxValue_ = 1000;
    double value_ = 0, reading_ = 0, displayValue_ = 0;
    double valueOffset_ = 0;
    double scaleAngleStartValue_ = 135, scaleAngleSize_ = 270, angleOffset_ = 0;
    int scalaCount_ = 10;
    QColor needleColor_{0, 0, 0};
    QColor scaleValueColor_{0, 0, 0};
    QColor displayValueColor_{0, 0, 0};
    QGradientStops scalePolygonColors_;
    QGradientStops outerCircleBg_;
    // (the original's one pen, whose width the drawing steps leave behind for the next: kept, as the sizes depend on it)
    QPen pen_{QColor(0, 0, 0)};
    QString scaleFontname_ = QStringLiteral("Orbitron"), valueFontname_ = QStringLiteral("Orbitron");
    double scaleFontsize_ = 14, valueFontsize_ = 40;
    double textRadiusFactor_ = 0.5;
    double needleScaleFactor_ = 0.8;
    double widgetDiameter_ = 0;
    QPolygon needle_;
    bool enableBarGraph_ = true;
    bool enableFilledPolygon_ = true;
    bool enableScaleText_ = true;
    bool enableValueText_ = true;
    bool enableCenterPoint_ = true;
    bool enableDrawNewArc_ = false;
    bool enableFineScaledMarker_ = true;
    bool enableBigScaledMarker_ = true;
    bool enableNeedlePolygon_ = true;
    std::optional<std::pair<QList<QColor>, double>> centerRings_;

    QPaintDevice* paintTarget_ = this;
    QPixmap paintCache_, faceUnder_, faceOver_;
    bool paintCacheDirty_ = true;
    bool frameDirty_ = true;
    std::map<std::tuple<double, double, double, int, double>, QPolygonF> pieCache_;
    std::optional<std::pair<std::tuple<double, double, double, int, double, QString>, double>> fittedValueFontsize_;
    QTimer* needleTimer_;
    QElapsedTimer needleClock_;
    double needleLastStep_ = 0;
};

// a background image that flips between a resting and a beating svg once per beat, at the rate bpmSource() returns;
// only repaints twice a beat, so it stays cheap under the gauge drawn over it
class BeatingSvgWidget : public QWidget {
    Q_OBJECT

public:
    // the portion of each beat spent showing the larger heart
    static constexpr double kBeatFraction = 0.35;

    BeatingSvgWidget(const QString& restSvgFile, const QString& beatSvgFile, const QColor& restColor,
                     const QColor& beatColor, std::function<double()> bpmSource, QWidget* parent = nullptr);
    QTimer* beatTimer() const { return beatTimer_; }
    bool beating() const { return beating_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QSvgRenderer* coloredRenderer(const QString& svgFile, const QColor& color);
    void nextPhase();
    void setBeating(bool beating);

    QSvgRenderer* restRenderer_;
    QSvgRenderer* beatRenderer_;
    std::function<double()> bpmSource_;
    bool beating_ = false;
    QTimer* beatTimer_;
};

// a background image that spins clockwise at the rate rpmSource() returns (drawn from a pixmap rendered once per size)
class RotatingSvgWidget : public QWidget {
    Q_OBJECT

public:
    // ~60 fps while spinning; how often to check for motion again while stopped
    static constexpr int kFrameMs = 16;
    static constexpr int kIdleMs = 250;

    RotatingSvgWidget(const QString& svgFile, std::function<double()> rpmSource, QWidget* parent = nullptr);
    // the image's size, as a part of the widget's smaller side
    double scale = 1.0;
    QTimer* spinTimer() const { return spinTimer_; }
    double angle() const { return angle_; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void step();

    QSvgRenderer* renderer_;
    std::function<double()> rpmSource_;
    double angle_ = 0.0;
    QPixmap pixmap_;
    QElapsedTimer clock_;
    double lastStep_ = 0;
    QTimer* spinTimer_;
};

// a gauge over its background image (the svgs are in the app's images folder)
class Gauge : public QWidget {
    Q_OBJECT

public:
    enum class Background { None, Image, Beating, Spinning };
    struct Beat {
        QString svgFile;
        QColor restColor, beatColor;
    };

    // svgFile: the background (none: no background); beat: beats at the gauge's value (heart rate); spin: spins at
    // the gauge's reading (cadence)
    explicit Gauge(const QString& svgFile = {}, const std::optional<Beat>& beat = std::nullopt, bool spin = false,
                   QWidget* parent = nullptr);

    AnalogGauge* gauge() const { return gauge_; }
    QWidget* background() const { return background_; }
    RotatingSvgWidget* chainring() const { return qobject_cast<RotatingSvgWidget*>(background_); }
    BeatingSvgWidget* heart() const { return qobject_cast<BeatingSvgWidget*>(background_); }

private:
    AnalogGauge* gauge_;
    QWidget* background_ = nullptr;
};

// the app's images folder (beside the exe)
QString imagePath(const QString& name);

}  // namespace awo

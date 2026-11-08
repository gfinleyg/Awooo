#pragma once

// The analog gauges (ClassAnalogGauge.py, after Stefan Holstein's analoggaugewidget): the power, cadence and heart rate
// gauges on the workout screen. A scale with tick marks and numbers, a colored band up to the needle, a needle that
// eases toward each reading at the Performance setting's frame rate, the reading and units in the middle, nested
// center rings (the power gauge's recent blocks, the cadence gauge's beat with its outermost a gear turning with the
// chainring, the heart rate gauge's nested hearts pulsing with the heartbeat), and the Ride At target arc.
//
// It's drawn in layers kept as pixmaps: the face under the band (the outer ring) and over it (ticks, numbers) are
// redrawn only by redraw(); those with the band between them again when the band's end moves a degree; the center with
// the reading on it (text has to be drawn on what's behind it to look the same) again when either changes. Each frame
// (redrawFrame(): a reading, the needle easing, the rings, the arc) only the needle, arc and units are drawn, among
// copies of those, and only in the part of the widget the gauge covers (a wide gauge's circle is much less than its
// widget). Repaints from an animated widget behind it just copy the last frame.
//
// Gauge: the gauge over a background image (a spinning chainring for cadence), or for heart rate with no background, its
// center nested hearts pulsing with the heartbeat and colored by the heart rate. The spin and the beat go at the last 3
// seconds' average, picked up every 2 seconds (SteadyReading), the needle and the reading at each reading.

#include <QElapsedTimer>
#include <QFont>
#include <QGradient>
#include <QPen>
#include <QPixmap>
#include <QPolygonF>
#include <QWidget>

#include <deque>
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
    // the outermost ring as a gear (setCenterGearAngle): this many teeth, each this much of a ring's width deep
    static constexpr int kCenterGearTeeth = 18;
    static constexpr double kCenterGearDepth = 0.5;
    // the rings as nested hearts (centerHeart): each outline from this many points
    static constexpr int kCenterHeartPoints = 144;
    // with them, the reading (fitValueInCenter) fitted in this much of the room a circle would give it
    static constexpr double kCenterHeartFit = 0.7;
    // a center heart's outline around (0, 0), a little wider than a circle of radius r
    static QPolygonF centerHeartOutline(double r);

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
    QGradientStops scalePolygonColors() const { return scalePolygonColors_; }
    // the band from (value, color) stops: each color exactly where the needle points at its value; the end of the
    // scale has the highest value's color (even past the end), stops below the start blend into its color there
    void setValueColors(QList<std::pair<double, QColor>> stops);
    double valueGradientPosition(double value) const;
    // the ring behind the band (the power gauge's follows the power ridden): part of the face, redrawn on a change
    void setOuterCircleBg(const QGradientStops& stops);
    QGradientStops outerCircleBg() const { return outerCircleBg_; }
    // nested circles in place of the center point, shaded through colors from the outermost to the innermost (two or
    // more), or the center point back (none). level (0 to 1) brightens them and swells them a little, e.g. on a beat
    // (with a gear, setCenterGearAngle: only the rings inside it, and more, as far as the floor of its gaps)
    void setCenterRings(const std::optional<QList<QColor>>& colors, double level = 0.0);
    // the outline the reading gets where it doesn't stand out from the center behind it (dark rings, say), none where
    // it does (see HaloLabel). The reading isn't in the theme's colors, nor is this: it's taken toward white, not the
    // theme's lightest background, which a dark theme's wouldn't show against the black reading
    std::optional<QColor> valueHalo();
    // the rings' colors (outermost first) and level, as last set
    const std::optional<std::pair<QList<QColor>, double>>& centerRings() const { return centerRings_; }
    // how many rings: centerRingCount, or with centerRingMinPx as many as fit at its size with each that wide
    int ringCount() const;
    // the outermost center ring drawn as a gear turned angle degrees clockwise (e.g. with the chainring behind it), or
    // a circle again (none). While it turns the gauge is redrawn for it at most at the Performance setting's frame rate
    // (0 fps: only along with whatever else redraws it)
    void setCenterGearAngle(std::optional<double> angle);
    std::optional<double> centerGearAngle() const { return centerGearAngle_; }
    // the target arc: centered on arcCenterValue, wider for a higher value, from the center's edge to the scale's
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
    // the rings drawn as nested hearts instead (e.g. the heart rate gauge's, see Gauge)
    bool centerHeart = false;

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
    void drawFace();
    void drawCenterLayer();
    void drawFrame();
    void drawLayer(QPixmap& layer, void (AnalogGauge::*draw)());
    // the band's length, for a scale this many degrees long
    int filledLength(int length) const;
    // the widget's part a text's box draws in, its glyphs' overhang and its outline included
    QRect textArea(const QRect& box, const QFontMetrics& fm) const;
    const std::optional<QColor>& currentHalo();
    // the reading in the center, kept with it (not: drawn with the units, every frame)
    bool valueOnCenter() const;
    void drawFilledPolygon(int outlinePenWidth = 0);
    void drawBigScaledMarker();
    void createScaleMarkerValuesText();
    void createFineScaledMarker();
    void createValuesText();
    QFont valueFont();
    QString valueText() const;
    // ring i of ringCount()'s color, and its radius in a center of the given radius
    QColor ringColor(int i) const;
    double ringRadius(int i, double radius) const;
    double ringOutlineWidth(double radius) const;
    double centerRadius() const;
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
    QPolygonF needle_;
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
    std::optional<double> centerGearAngle_;
    // (on the needle's clock)
    double gearLastFrame_ = 0;

    using Rings = std::optional<std::pair<QList<QColor>, double>>;
    using CenterKey = std::tuple<Rings, std::optional<double>, double, int, std::pair<double, double>, bool,
                                 QGradientStops, QString, QFont, std::optional<QColor>>;
    using HaloKey = std::tuple<QString, QFont, Rings, bool, int>;

    QPaintDevice* paintTarget_ = this;
    // face_: faceUnder_, the band and faceOver_
    QPixmap paintCache_, faceUnder_, faceOver_, face_, centerLayer_;
    bool paintCacheDirty_ = true;
    bool frameDirty_ = true;
    // what each layer was last drawn from
    std::optional<int> faceBand_;
    std::optional<CenterKey> centerKey_;
    std::optional<HaloKey> haloKey_;
    std::optional<QColor> halo_;
    // the parts of the widget they draw in, and the last frame did
    QRect faceRect_, centerRect_, textRect_, frameRect_;
    std::map<std::tuple<double, double, double, int, double>, QPolygonF> pieCache_;
    std::optional<std::pair<std::tuple<double, double, double, int, double, QString, bool>, double>> fittedValueFontsize_;
    QTimer* needleTimer_;
    QElapsedTimer needleClock_;
    double needleLastStep_ = 0;
};

// the average of a reading over its last kSeconds, by how long each value held (so it doesn't matter how often readings
// come, or whether they repeat); just the time since the first one, until there's that much. Readings of 0: with
// zeroCounts they're averaged in like any other (a chainring slows to a stop), until a reading comes again, which starts
// the average afresh (pedaling again isn't slow pedaling); without, that time is left out (no heart rate isn't a slow
// one), and it's 0 once there's no other reading left in the last kSeconds.
class RecentAverage {
public:
    static constexpr double kSeconds = 3.0;

    explicit RecentAverage(bool zeroCounts = true) : zeroCounts_(zeroCounts) {}
    void add(double value, double now);
    double at(double now) const;

private:
    struct Sample {
        double value;
        double since;
    };

    bool zeroCounts_;
    std::deque<Sample> samples_;
};

// a RecentAverage that's picked up only every kHoldSeconds, so what follows it changes now and then rather than all the
// time, and eased into over easeSeconds (0: straight to it); from nothing (0), the first reading is picked up at once
class SteadyReading {
public:
    static constexpr double kHoldSeconds = 2.0;

    explicit SteadyReading(bool zeroCounts = true, double easeSeconds = 0)
        : average_(zeroCounts), easeSeconds_(easeSeconds) {}
    void add(double value, double now) { average_.add(value, now); }
    double at(double now);

private:
    double eased(double now) const;

    RecentAverage average_;
    double easeSeconds_;
    double from_ = 0;
    double to_ = 0;
    std::optional<double> since_;
};

// a heartbeat at the rate bpmSource() returns (read again every beat, so it follows the heart rate as it changes):
// beating() for the first kBeatFraction of each beat. pulsed(level): 1 as each beat starts, fading to 0 over
// kPulseSeconds at the Performance setting's frame rate (0 fps: 1 for the beat, 0 at rest), e.g. for the gauge's center
// hearts
class Heartbeat : public QObject {
    Q_OBJECT

public:
    // the portion of each beat that's the beat
    static constexpr double kBeatFraction = 0.35;
    // (as long as the cadence gauge's beat)
    static constexpr double kPulseSeconds = 0.2;

    explicit Heartbeat(std::function<double()> bpmSource, QObject* parent = nullptr);
    QTimer* beatTimer() const { return beatTimer_; }
    QTimer* pulseTimer() const { return pulseTimer_; }
    // the rate it's beating at now (0: resting)
    double bpm() const { return bpmSource_(); }
    bool beating() const { return beating_; }
    void setBeating(bool beating);

signals:
    void pulsed(double level);

private:
    void nextPhase();
    void pulseStep();

    std::function<double()> bpmSource_;
    bool beating_ = false;
    QTimer* beatTimer_;
    QTimer* pulseTimer_;
    QElapsedTimer pulseClock_;
};

// a background image that spins clockwise at the rate rpmSource() returns (drawn from a pixmap rendered once per size);
// turned(angle) each frame it turns, and once when it stops (e.g. the gauge's center gear turns with it)
class RotatingSvgWidget : public QWidget {
    Q_OBJECT

public:
    // while spinning it steps every frame (Performance::frameMs); how often to check for motion again while stopped
    static constexpr int kIdleMs = 250;

    RotatingSvgWidget(const QString& svgFile, std::function<double()> rpmSource, QWidget* parent = nullptr);
    // the image's size, as a part of the widget's smaller side
    double scale = 1.0;
    QTimer* spinTimer() const { return spinTimer_; }
    double angle() const { return angle_; }
    // the rate it's spinning at now (0: stopped)
    double rpm() const { return rpmSource_(); }

signals:
    void turned(double angle);

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
    // the center's nested hearts, colored by the gauge's value from (value, color) stops, blended between (each color
    // "#rrggbb", through the theme)
    struct Beat {
        QList<std::pair<double, QColor>> colors;
    };
    static constexpr double kSpinEaseSeconds = 1.0;

    // svgFile: the background (none: no background); beat: no background, the center's nested hearts beat at the
    // average of the gauge's recent values (heart rate); spin: spins at the average of its recent readings (cadence),
    // easing into each new speed over kSpinEaseSeconds
    explicit Gauge(const QString& svgFile = {}, const std::optional<Beat>& beat = std::nullopt, bool spin = false,
                   QWidget* parent = nullptr);

    AnalogGauge* gauge() const { return gauge_; }
    QWidget* background() const { return background_; }
    RotatingSvgWidget* chainring() const { return qobject_cast<RotatingSvgWidget*>(background_); }
    Heartbeat* heart() const { return heart_; }
    // the hearts' color now (through the theme)
    QColor heartColor() const;
    // the outer arc in the hearts' colors, filled up to the needle (once the gauge's range is set; again as the theme
    // changes)
    void colorArc();

private:
    void colorHeart();

    AnalogGauge* gauge_;
    QWidget* background_ = nullptr;
    Heartbeat* heart_ = nullptr;
    QList<std::pair<double, QColor>> heartColors_;
    double heartLevel_ = 0.0;
    // the readings the spin or the beat goes at
    SteadyReading steady_;
    QElapsedTimer recentClock_;
};

// the app's images folder (beside the exe)
QString imagePath(const QString& name);

}  // namespace awo

#pragma once

// MaxStar (ClassMaxStar.py): a tiny star over the highest point of a graph curve. When the max moves (zooming or
// panning the synopsis, a new high reading or the old max scrolling off the workout screen) it rides the curve as
// drawn to the new max: still at both ends and fastest halfway (smoothstep), hugging the line on the way, then
// settling back just over the max, as close as it can without touching the line (or a marker there), where it
// twinkles. Its owner says where the max is (goTo), steps it about every frame (step) and keeps room for it at the
// graph's top (topWithRoom). MaxBubble: the max's value in a small box beside a star at rest, popping when the max
// moves.

#include <QFont>
#include <QGraphicsPathItem>

#include <functional>
#include <optional>

#include "PlotItems.hpp"
#include "ViewBox.hpp"

namespace awo {

class MaxStar : public QGraphicsPathItem {
public:
    // how often the owner steps its stars, in ms (about every frame)
    static constexpr int kStepMs = 16;
    // (at the app's design size: scaled with the rest of the screen, see theme::uiScale)
    static constexpr double kRadius = 8;
    // the inner corners' share of kRadius: plumper in the middle than a classic star's ~0.38
    static constexpr double kInner = 0.58;
    // its lowest points' depth below its center, as a share of kRadius (the two lower tips, 54 degrees down)
    static const double kBelow;
    // how much bigger it gets at the top of a twinkle
    static constexpr double kTwinkle = 0.14;
    static constexpr double kEdgeWidth = 1.5;
    // the gap left between it and the line (or marker) under it, in pixels
    static constexpr double kGap = 1.0;
    static const QString kColor;
    static const QString kEdgeColor;
    // a trip takes kTripSeconds plus kTripSecondsPerPx for each pixel of curve, within kTripMin - kTripMax seconds
    static constexpr double kTripSeconds = 0.45;
    static constexpr double kTripSecondsPerPx = 0.0009;
    static constexpr double kTripMin = 0.5, kTripMax = 1.4;

    // curve: the curve it rides; vbox: the view that's drawn in; viewRange(): the graph's shown times. Add it to the
    // graph's scene
    MaxStar(plot::CurveItem* curve, plot::ViewBox* vbox, std::function<plot::Range()> viewRange);

    void setColor(const QString& color);
    plot::ViewBox* vbox() const { return vbox_; }

    // how far a marker drawn on the max reaches above it, in pixels (0: none); the owner keeps it up to date
    double markerReach = 0.0;
    // whether its max is on the graph at all (hidden while not)
    bool onGraph = true;
    // the max it's over or riding to (graph coordinates), none until it has one
    std::optional<double> x, y;
    // how far above the max it rests now (pixels): eases to restOffset() when that changes, rather than hopping
    std::optional<double> offset;

    double lineReach() const;
    // how far above its max its center rests, in pixels: its lowest tips (at a twinkle's biggest) just clear of the
    // line and any marker there
    double restOffset() const;
    // how far above its max it reaches, in pixels: the room it needs at the top of the graph
    double headroom() const;
    QRectF restRect(const QPointF& point) const;

    // a y range top (from 0) of at least top, high enough for each (headroom, value): that resting over a max of
    // value fits under it, and under reserved pixels along the graph's top
    static double topWithRoom(const plot::ViewBox* vbox, double top, const QList<std::pair<double, double>>& rooms,
                              double reserved = 0.0);
    static double ease(double t);
    static double lift(double fromLift, double t);

    // the max is at (x, y) now (seconds on the monotonic clock): off there along the curve if it moved
    void goTo(double x, double y, double now);
    double progress(double now) const;
    // moves it to where it is at now (seconds); phase: its twinkle's, to keep stars out of step
    void step(double now, int phase = 0);

    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

private:
    struct Mapping {
        double originX, scaleX, originY, scaleY;
    };
    Mapping viewToScene() const;
    // the curve as drawn from the trip's start to its max, in scene coordinates at about a point per pixel
    std::pair<QList<double>, QList<double>> tripPath() const;

    plot::CurveItem* curve_;
    plot::ViewBox* vbox_;
    std::function<plot::Range()> viewRange_;
    QString color_;
    QPen pen_;
    // its trip: from tripFrom_ to x (times), from tripStart_ for tripSeconds_, lifting off from tripLift_; at_: the
    // time it's at now
    std::optional<double> tripFrom_, at_;
    double tripStart_ = 0.0, tripSeconds_ = 1.0, tripLift_ = 1.0;
};

class MaxBubble : public QGraphicsItem {
public:
    // pixels between it and its star
    static constexpr double kGap = 3;
    // its text's side padding, and its corners' radius, as shares of its height
    static constexpr double kPad = 0.3;
    static constexpr double kCorner = 0.3;
    static constexpr double kPopSeconds = 0.22;
    // how much bigger it gets by the end of its pop
    static constexpr double kPopGrowth = 0.35;
    static constexpr double kAppearSeconds = 0.28;

    // star: the MaxStar it goes with; format: its text, with %1 for the max's value (e.g. "Max Pwr: %1 W"). Add it to
    // the graph's scene
    MaxBubble(MaxStar* star, const QString& format);

    MaxStar* star() const { return star_; }
    QString text() const { return text_; }
    // how far above its max it and its star reach, in pixels (it at a pop's biggest)
    double headroom() const;
    // places it at now beside its star, on a side with room that keeps clear of the rects in avoid (scene
    // coordinates: the other stars and bubbles), and adds its own rect to avoid
    void step(double now, QList<QRectF>& avoid);

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

private:
    void fit(const QString& text);

    enum class State { Appearing, Shown, Popping, Hidden };
    MaxStar* star_;
    QString format_;
    QString text_;
    QFont font_;
    double width_ = 0, height_ = 0;
    State state_ = State::Hidden;
    double stateStart_ = 0;
    std::optional<double> x_, y_;
    int side_ = 1;
};

}  // namespace awo

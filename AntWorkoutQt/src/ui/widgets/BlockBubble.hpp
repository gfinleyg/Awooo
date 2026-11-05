#pragma once

// BlockBubble: a completed block's average power and cadence in a small box on the workout screen's graph. One shows at
// a time (stepAll): the latest block on the graph with the target coming up in the last seconds of a block, if there is
// one, else with the target being ridden now, else the latest block. It sits in the middle of its block with its bottom
// edge on the graph's bottom, and rides along with the block as the graph scrolls; once a long block's middle would take
// it off the graph it stays at the graph's left edge, inside the block. A soap bubble, as the max bubbles are (MaxBubble):
// one that's shown pops when another takes its place, or when the graph has scrolled so far that it no longer fits. One
// a star touches pops and comes in again where it's clear of the stars, along or higher up in its block. Switch single
// leg blocks have no bubble, and aren't targets to pick one by: riding one goes by the block before it's target.

#include <QFont>
#include <QGraphicsItem>

#include <functional>
#include <optional>

#include "SoapBubble.hpp"
#include "ViewBox.hpp"

namespace awo {

class BlockBubble : public QGraphicsItem {
public:
    // its text's padding, and its corners' radius, as shares of a line's height
    static constexpr double kPad = 0.35;
    static constexpr double kCorner = 0.8;
    static constexpr double kAppearSeconds = 0.28;
    static constexpr double kPopSeconds = soap::kPopSeconds;
    // pixels between the rows of spots it may be placed in, and kept clear around it when it's placed (it grows in a
    // little past its size)
    static constexpr double kGap = 3, kClear = 3;
    // how far past the time now (the graph's now line) it may reach
    static constexpr double kMaxAheadSeconds = 30;

    // one line: "Avg Pwr 245W", its number to that many decimals ("Avg Cad 88.3rpm"); avg none without readings ("--")
    struct Row {
        QString name, unit;
        std::optional<double> avg;
        int decimals = 0;
    };

    // start, end: the block's times; target: its target at its end; timeBox: a view of the graph to place it by;
    // rideSeconds(): the time now on the graph. Add it to the graph's scene
    BlockBubble(double start, double end, const QList<Row>& rows, double target, const plot::ViewBox* timeBox,
                std::function<double()> rideSeconds);

    double start() const { return start_; }
    double end() const { return end_; }
    // the block's target (at its end)
    double target() const { return target_; }
    QStringList lines() const;
    QFont font() const { return font_; }
    bool popping() const { return popAt_.has_value(); }
    // where it's placed (scene coordinates), at its full size
    QRectF placedRect() const { return rect_; }
    // the one to show (none: none on the graph): the latest on the graph (see onGraph) with upcoming as its target,
    // else with target as its target, else the latest on the graph
    static BlockBubble* pick(const QList<BlockBubble*>& bubbles, double target, std::optional<double> upcoming = std::nullopt);
    // shows that one at now (seconds on the owner's clock: it pops in each time it's picked again) and hides the rest
    // (one that was shown pops first). It pops if a rect in avoid (scene coordinates: the stars) touches it, and comes
    // in clear of those and of keepOff (what's on its way there); its rect is added to avoid (for the max bubbles)
    static void stepAll(const QList<BlockBubble*>& bubbles, double target, double now, QList<QRectF>& avoid,
                        std::optional<double> upcoming = std::nullopt, const QList<QRectF>& keepOff = {});

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

private:
    void fit();
    double sceneX(double seconds) const;
    double naturalLeft() const;
    double left() const;
    QRectF placed() const;
    bool findSpot(const QList<QRectF>& obstacles);
    bool onGraph() const;
    void step(double now, QList<QRectF>& avoid, const QList<QRectF>& keepOff);
    // not the one to show: pops if it's shown, else hides
    void leave(double now);
    void hide();

    double start_, end_;
    QList<Row> rows_;
    double target_;
    const plot::ViewBox* timeBox_;
    std::function<double()> rideSeconds_;
    std::optional<double> appearAt_, popAt_;
    // popping to come in again elsewhere
    bool replacing_ = false;
    // where in its block it's placed: seconds from the middle, and pixels over the graph's bottom
    double shift_ = 0, lift_ = 0;
    QRectF rect_;
    // the time it was last stepped at (its colors drift with it), and how many times it's popped
    double now_ = 0;
    unsigned pops_ = 0;
    QFont font_;
    // each line's cells ("Avg Pwr", "245W") and each column's width
    QList<QStringList> cells_;
    QList<double> columns_;
    double lineHeight_ = 0, width_ = 0, height_ = 0;
};

}  // namespace awo

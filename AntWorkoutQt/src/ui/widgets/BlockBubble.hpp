#pragma once

// BlockBubble: a completed block's average power and cadence in a small box on the workout screen's graph. One shows at a time (stepAll): the latest block on the graph with the target being ridden now, or
// else the latest block. It pops in where the lines (the readings' curves and the target bars) ended as its block did,
// at its end (kept on the graph, and from reaching more than kMaxAheadSeconds past the time now): under the lowest of
// them if it fits there, over the highest if not (the graph's owner keeps room for it at the top, see room()), and
// rides along with the block as the graph scrolls. One that isn't under the top of its own block has a thin diagonal
// line to it. A soap bubble, as the max bubbles are (MaxBubble): one that's shown pops when another takes its place.

#include <QFont>
#include <QGraphicsItem>
#include <QGraphicsLineItem>

#include <functional>
#include <optional>

#include "SoapBubble.hpp"
#include "ViewBox.hpp"

namespace awo {

class BlockBubble : public QGraphicsItem {
public:
    // pixels between it and the lines' ends
    static constexpr double kGap = 4;
    // its text's padding, and its corners' radius, as shares of a line's height
    static constexpr double kPad = 0.35;
    static constexpr double kCorner = 0.8;
    static constexpr double kAppearSeconds = 0.28;
    static constexpr double kPopSeconds = soap::kPopSeconds;
    // how far past the time now (the graph's now line) it may reach
    static constexpr double kMaxAheadSeconds = 30;

    // one line: "Avg Pwr 245W"; avg none without readings ("--")
    struct Row {
        QString name, unit;
        std::optional<double> avg;
    };
    // a line's end: its view box and value there
    using LineEnd = std::pair<const plot::ViewBox*, double>;

    // start, end: the block's times; target: its target bar's end; curveEnds: the readings' curves' ends; timeBox: a
    // view of the graph to place it by; rideSeconds(): the time now on the graph. Add it to the graph's scene
    BlockBubble(double start, double end, const QList<Row>& rows, const LineEnd& target, const QList<LineEnd>& curveEnds,
                const plot::ViewBox* timeBox, std::function<double()> rideSeconds);

    double start() const { return start_; }
    double end() const { return end_; }
    // the block's target (at its end)
    double target() const { return target_.second; }
    QStringList lines() const;
    // the thin line to its block (hidden while it's under the top of its block)
    const QGraphicsLineItem* connector() const { return connector_; }
    bool popping() const { return popAt_.has_value(); }
    // the room it needs over the graph's lines while it's shown over them: (its view box, pixels over the value, the
    // value)
    struct Room {
        const plot::ViewBox* vbox;
        double headroom, value;
    };
    std::optional<Room> room() const;
    // the one to show (none: none on the graph): the latest with its block on the graph and target as its target,
    // else the latest with its block on the graph
    static BlockBubble* pick(const QList<BlockBubble*>& bubbles, double target);
    // shows that one at now (seconds on the owner's clock: it pops in each time it's picked again) and hides the rest
    // (one that was shown pops first); its rect is added to avoid (scene coordinates: for the max bubbles)
    static void stepAll(const QList<BlockBubble*>& bubbles, double target, double now, QList<QRectF>& avoid);

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

private:
    void fit();
    double sceneX(double seconds) const;
    bool onGraph() const;
    double top(bool* above = nullptr) const;
    void step(double now, QList<QRectF>& avoid);
    // not the one to show: pops if it's shown, else hides
    void leave(double now);
    void hide();
    void connect(const QRectF& rect);

    double start_, end_;
    QList<Row> rows_;
    LineEnd target_;
    // the target's and the curves' ends
    QList<LineEnd> ends_;
    const plot::ViewBox* timeBox_;
    std::function<double()> rideSeconds_;
    QGraphicsLineItem* connector_;
    std::optional<double> appearAt_, popAt_;
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

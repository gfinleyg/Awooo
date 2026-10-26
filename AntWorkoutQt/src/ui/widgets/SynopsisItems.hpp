#pragma once

// What the synopsis draws on its graph besides the curves (ClassSynopsisWindow.py's PlotLabel, GradeRegion,
// BandStrip): a max value's label in a rounded box, a graded block's clickable span, and the strips of bands along
// the graph's top (the session's rides, Musical Cadence Mode's songs). Each takes clicks as pyqtgraph's scene hands
// them out (plot::Clickable), so the views under them still get drags.

#include <QColor>
#include <QFont>
#include <QGraphicsObject>
#include <QGraphicsRectItem>
#include <QGraphicsSimpleTextItem>

#include <functional>

#include "ViewBox.hpp"
#include "WorkoutGrading.hpp"

namespace awo {

// Text in a rounded box the color of the plot's background, in scene coordinates; clicking it calls onClick
class PlotLabel : public QGraphicsSimpleTextItem, public plot::Clickable {
public:
    static constexpr double kPadX = 7;
    static constexpr double kPadY = 3;
    static constexpr double kRadius = 7;
    // Orbitron size of the max labels (the synopsis' axis titles match it)
    static constexpr double kFontPoints = 11;
    // the border drawn in the theme's color for it: near (40, 40, 40) on the light label
    static const QColor kBorderColor;
    static constexpr double kBorderWidth = 1.2;

    explicit PlotLabel(const QString& text = {});

    std::function<void()> onClick;
    void setBorderColor(const QColor& color, double width);
    // the font as set, through the theme (again whenever it changes, see retheme)
    void setLabelFont(const QFont& font);
    void retheme();

    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;
    bool click(const QPointF& scenePos, Qt::MouseButton button) override;

private:
    QColor borderColor_ = kBorderColor;
    double borderWidth_ = kBorderWidth;
    QFont font_;
};

// A graded block's span on the graph (in its view's coordinates, its ride's target axis): drawn as nothing, under the
// curves so they still take their clicks; clicking it opens its grade
struct GradeRegionKey {
    int ride = 0;  // the ride's index in the session
    grading::GradeKey key;
};

class GradeRegion : public QGraphicsRectItem, public plot::Clickable {
public:
    GradeRegion(const QRectF& rect, const GradeRegionKey& key, plot::ViewBox* vbox,
                std::function<void(GradeRegion*)> onClick);

    const GradeRegionKey& key() const { return key_; }
    plot::ViewBox* vbox() const { return vbox_; }
    bool click(const QPointF& scenePos, Qt::MouseButton button) override;

private:
    GradeRegionKey key_;
    plot::ViewBox* vbox_;
    std::function<void(GradeRegion*)> onClick_;
};

// Bands along the top of the graph, each over its stretch of time in alternating shades, its title inside (cut short,
// or left out, where it doesn't fit). Hovering one shows its whole title and when it was; clicking one calls onClick
// with its index (the synopsis zooms to it)
class BandStrip : public QGraphicsObject, public plot::Clickable {
    Q_OBJECT

public:
    struct Band {
        double start = 0, end = 0;  // the graph's seconds
        QString title;
    };
    // the shades: a song strip's; the rides' are darker
    static const QColor kSongColors[2];
    static const QColor kRideColors[2];
    static constexpr double kPadX = 6;

    BandStrip(const QList<Band>& bands, const QColor (&colors)[2], plot::ViewBox* vbox, std::function<void(int)> onClick);

    const QList<Band>& bands() const { return bands_; }
    // how far down from the top of the plot area (px): under the strips above it
    double top = 0.0;
    double height() const { return height_; }
    const QFont& font() const { return font_; }
    // across the top of the plot area (as it's moved, resized or zoomed), at the max labels' size as it is now
    void layout();
    // (index, left, right) in the strip's own pixels of each band in view, cut to its ends
    QList<std::tuple<int, double, double>> spans() const;
    std::optional<int> bandAt(double x) const;

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;
    bool click(const QPointF& scenePos, Qt::MouseButton button) override;

protected:
    void hoverMoveEvent(QGraphicsSceneHoverEvent* event) override;

private:
    // bold Segoe UI at the max labels' size (through the theme's Text font size and the layout scale, as theirs is),
    // the strip as tall as it needs
    void setFont();

    QList<Band> bands_;
    QColor colors_[2];
    plot::ViewBox* vbox_;
    std::function<void(int)> onClick_;
    QFont font_;
    double height_ = 0;
    double width_ = 0;
};

}  // namespace awo

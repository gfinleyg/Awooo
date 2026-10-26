#pragma once

// A rectangle of the graph showing a range of data (pyqtgraph's ViewBox, the parts the app's graphs use): the items
// added to it are drawn in data coordinates, clipped to it. Its range is set (with pyqtgraph's padding and limits),
// follows another view's x range (setXLink), and can be dragged and wheel-zoomed along the axes the mouse is enabled
// for. pyqtgraph's y axis points up, so a higher value is drawn higher.

#include <QGraphicsWidget>
#include <QPointer>

#include <optional>

namespace awo::plot {

// an item added to a view that's told when the view's data -> pixel mapping changes (one sized in pixels, like a
// line's markers or a scatter's symbols)
class ViewAware {
public:
    virtual ~ViewAware() = default;
    virtual void viewTransformChanged() = 0;
};

// An item that takes clicks the way pyqtgraph's scene hands them out: a press and release in place, offered to each item
// under the mouse in turn, topmost first, until one takes it (true). It doesn't take Qt's press itself, so the view
// under it still gets drags; the view that got the press hands the click on (see ViewBox::clicked)
class Clickable {
public:
    virtual ~Clickable() = default;
    virtual bool click(const QPointF& scenePos, Qt::MouseButton button) = 0;
};

// offers a click at scenePos to the Clickable items there, topmost first; whether one took it
bool sendClick(QGraphicsScene* scene, const QPointF& scenePos, Qt::MouseButton button);

struct Range {
    double min = 0;
    double max = 1;
    double span() const { return max - min; }
    bool operator==(const Range&) const = default;
};

class ViewBox : public QGraphicsWidget {
    Q_OBJECT

public:
    enum Axis { XAxis = 0, YAxis = 1 };

    struct Limits {
        std::optional<double> xMin, xMax, minXRange, maxXRange;
        std::optional<double> yMin, yMax, minYRange, maxYRange;
    };

    explicit ViewBox(QGraphicsItem* parent = nullptr);

    // drawn in data coordinates (as children of childGroup()), in the order added within the same z value
    void addItem(QGraphicsItem* item);
    void removeItem(QGraphicsItem* item);
    const QList<QGraphicsItem*>& addedItems() const { return added_; }
    QGraphicsItem* childGroup() const { return childGroup_; }

    Range xRange() const { return range_[XAxis]; }
    Range yRange() const { return range_[YAxis]; }
    Range range(Axis axis) const { return range_[axis]; }
    // the range as a rect in data coordinates (top is the lower value)
    QRectF viewRect() const;
    // padding: the share of the range added at each end; none: suggestPadding (pyqtgraph's default)
    void setXRange(double min, double max, std::optional<double> padding = std::nullopt);
    void setYRange(double min, double max, std::optional<double> padding = std::nullopt);
    void setRange(const QRectF& rect, std::optional<double> padding = std::nullopt);
    double suggestPadding(Axis axis) const;
    void setDefaultPadding(double padding) { defaultPadding_ = padding; }
    void setLimits(const Limits& limits);
    const Limits& limits() const { return limits_; }

    // this view's x range follows view's (lined up where they overlap on screen); nullptr unlinks
    void setXLink(ViewBox* view);
    void setMouseEnabled(bool x, bool y);
    bool mouseEnabled(Axis axis) const { return mouseEnabled_[axis]; }

    // data <-> this item's coordinates
    QTransform viewTransform() const;
    QPointF mapFromView(const QPointF& point) const;
    QPointF mapToView(const QPointF& point) const;
    QPointF mapViewToScene(const QPointF& point) const;
    QPointF mapSceneToView(const QPointF& point) const;
    // where it is in its view, in whole pixels (pyqtgraph's screenGeometry, which lines up linked views)
    QRectF screenGeometry() const;
    // one pixel on screen, in data units (x, y), or 0 while the view has no size
    QPointF viewPixelSize() const;

    // (pyqtgraph's: half a pixel past its geometry at the right and bottom, which views sized from another's
    // sceneBoundingRect inherit, see WorkoutGraph::updateViews)
    QRectF boundingRect() const override;
    QPainterPath shape() const override;

signals:
    void xRangeChanged(awo::plot::ViewBox* view, awo::plot::Range range);
    void yRangeChanged(awo::plot::ViewBox* view, awo::plot::Range range);
    void rangeChanged(awo::plot::ViewBox* view);
    // the range was changed by a drag or the wheel
    void rangeChangedManually();
    void resized(awo::plot::ViewBox* view);
    // a click (press and release in place) on the view, after the items under it were offered it (taken: one took
    // it), as pyqtgraph's scene's sigMouseClicked
    void clicked(QPointF scenePos, Qt::MouseButton button, bool taken);

protected:
    void resizeEvent(QGraphicsSceneResizeEvent* event) override;
    void wheelEvent(QGraphicsSceneWheelEvent* event) override;
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;

private:
    void setAxisRange(Axis axis, double min, double max, std::optional<double> padding);
    void updateViewRange();
    void updateMatrix();
    void linkedXChanged();
    // view's x range changed: this one's lined up with it on screen (pyqtgraph's linkedViewChanged). Both ways: a view
    // follows the one it's linked to, and that one follows it back when the linked view is changed itself (dragged,
    // zoomed)
    void linkedViewChanged(ViewBox* view);
    void scaleBy(std::optional<double> x, std::optional<double> y, QPointF center);
    void translateBy(std::optional<double> x, std::optional<double> y);

    QGraphicsItem* childGroup_;
    QList<QGraphicsItem*> added_;
    Range range_[2];
    Range target_[2];
    Limits limits_;
    double defaultPadding_ = 0.02;
    bool mouseEnabled_[2] = {true, true};
    QPointer<ViewBox> xLink_;
    // (pyqtgraph's blockLink: no following back while this one is being lined up with it)
    bool linksBlocked_ = false;
    bool dragging_ = false;
    QPointF lastDragPos_;
};

}  // namespace awo::plot

#pragma once

// What the app's graphs draw in a ViewBox (pyqtgraph's PlotCurveItem, InfiniteLine and ScatterPlotItem, the parts the
// graphs use): a sensor's line over its halo, a vertical line with caret ends, and a symbol on a point. Each is in
// data coordinates; the line's markers and the symbols keep their size in pixels.

#include <QGraphicsObject>
#include <QImage>
#include <QPainterPath>
#include <QPen>

#include <optional>

#include "ViewBox.hpp"

namespace awo::plot {

// a pyqtgraph pen made from a color: cosmetic (as wide on screen whatever the scale), 1 px unless given
QPen mkPen(const QColor& color, double width = 1);

class CurveItem : public QGraphicsObject, public ViewAware {
    Q_OBJECT

public:
    explicit CurveItem(QGraphicsItem* parent = nullptr);

    // the points (as many xs as ys), drawn with pen over shadowPen (none: no shadow)
    void setData(const QList<double>& xs, const QList<double>& ys);
    void setData(const QList<double>& xs, const QList<double>& ys, const QPen& pen,
                 const std::optional<QPen>& shadowPen);
    void setPen(const QPen& pen);
    void setShadowPen(const std::optional<QPen>& pen);
    const QList<double>& xData() const { return xs_; }
    const QList<double>& yData() const { return ys_; }
    QPen pen() const { return pen_; }
    std::optional<QPen> shadowPen() const { return shadowPen_; }
    void clear() { setData({}, {}); }

    // clicks within tolerance pixels of the line emit clicked (the synopsis picks a curve that way)
    void setClickable(bool clickable, double tolerance = 10);
    QPainterPath shape() const override;

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;
    void viewTransformChanged() override;

signals:
    void clicked(awo::plot::CurveItem* curve);

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;

private:
    ViewBox* view() const;
    double pixelPadding() const;

    QList<double> xs_, ys_;
    QPainterPath path_;
    QRectF dataBounds_;
    QPen pen_;
    std::optional<QPen> shadowPen_;
    bool clickable_ = false;
    double tolerance_ = 10;
};

class InfiniteLine : public QGraphicsObject, public ViewAware {
    Q_OBJECT

public:
    // angle 90: vertical, at x = pos (the only kind the app uses), 0: horizontal at y = pos
    explicit InfiniteLine(double pos = 0, double angle = 90, QGraphicsItem* parent = nullptr);

    void setValue(double pos);
    double value() const { return pos_; }
    void setPen(const QPen& pen);
    QPen pen() const { return pen_; }
    // "v", "^", "<|", "|>", ">|", "|<", "o" (or combinations), at position 0 - 1 along the visible line, size px
    void addMarker(const QString& marker, double position = 0.5, double size = 10);
    void clearMarkers();

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;
    void viewTransformChanged() override;

private:
    ViewBox* view() const;
    // the line's ends along its length, in data units: the view's extent
    std::pair<QPointF, QPointF> endPoints() const;

    double pos_;
    double angle_;
    QPen pen_;
    struct Marker {
        QPainterPath path;
        double position;
        double size;
    };
    QList<Marker> markers_;
    double maxMarkerSize_ = 0;
};

class ScatterItem : public QGraphicsObject, public ViewAware {
    Q_OBJECT

public:
    // symbol: "d" (diamond), "o", "s", "t"; size in pixels
    explicit ScatterItem(const QString& symbol = QStringLiteral("o"), double size = 7, QGraphicsItem* parent = nullptr);

    void setData(const QList<double>& xs, const QList<double>& ys);
    void clear() { setData({}, {}); }
    void setBrush(const QBrush& brush);
    void setPen(const QPen& pen);
    void setSize(double size);

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;
    void viewTransformChanged() override;

private:
    ViewBox* view() const;
    const QImage& symbolImage(double dpr);

    QString symbol_;
    double size_;
    QBrush brush_;
    QPen pen_;
    QList<QPointF> points_;
    QImage image_;
};

}  // namespace awo::plot

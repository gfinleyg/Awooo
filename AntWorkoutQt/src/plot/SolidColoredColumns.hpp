#pragma once

// The workout's target bars (ClassSolidColoredColumns.py): a step plot filled down to 0, each column colored by its
// value, from the colors of the ranges it falls in or a function of the value.

#include <QColor>
#include <QGraphicsObject>

#include <functional>

#include "ViewBox.hpp"

namespace awo::plot {

class SolidColoredColumns : public QGraphicsObject {
    Q_OBJECT

public:
    // a column's color from its value and the ranges (e.g. zones::graphColor)
    using Colorer = std::function<QColor(double value, const QList<double>& ranges)>;

    explicit SolidColoredColumns(QGraphicsItem* parent = nullptr);

    // xs: the columns' edges, one more than ys. colors: one per range (ranges plus one: range 0 is value <=
    // ranges[0], range i is ranges[i-1] < value <= ranges[i]); or colorer, a color for each column
    void setData(const QList<double>& xs, const QList<double>& ys, const QList<double>& ranges,
                 const QList<QColor>& colors);
    void setData(const QList<double>& xs, const QList<double>& ys, const QList<double>& ranges,
                 const Colorer& colorer);
    const QList<double>& xData() const { return xs_; }
    const QList<double>& yData() const { return ys_; }
    // drawn again with the colorer's colors as they are now
    void recolor();
    void clear();

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

private:
    void setColumns(const QList<double>& xs, const QList<double>& ys, const QList<double>& ranges);
    void buildRects();

    QList<double> xs_, ys_, ranges_;
    QList<QColor> colors_;
    Colorer colorer_;
    QRectF bounds_;
    // one list of rectangles per color (a list of rectangles draws much faster than one path of them)
    QList<std::pair<QColor, QList<QRectF>>> rects_;
    bool rectsDirty_ = true;
};

}  // namespace awo::plot

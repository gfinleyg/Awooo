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
    // faint diagonal lines over the columns (see HatchTile), that move along with them; scale: their size's
    void setHatched(bool hatched);
    void setHatchScale(double scale);
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
    struct ColorRects {
        QColor color;
        // (of the lines over them)
        QColor ink;
        QList<QRectF> rects;
    };
    QList<ColorRects> rects_;
    bool rectsDirty_ = true;
    bool hatched_ = false;
    double hatchScale_ = 1.0;
};

}  // namespace awo::plot

#pragma once

// A plot and the view it's shown in (pyqtgraph's PlotItem, LabelItem and PlotWidget, the parts the app's graphs use):
// a title over a ViewBox with an axis on each side, laid out in a grid as pyqtgraph lays them out, filling a
// QGraphicsView with no frame or scroll bars.

#include <QGraphicsView>
#include <QGraphicsWidget>

#include <optional>

#include "AxisItem.hpp"
#include "ViewBox.hpp"

class QGraphicsGridLayout;
class QGraphicsTextItem;

namespace awo::plot {

// a line of text in a layout (the plot's title), its size its text's
class LabelItem : public QGraphicsWidget {
    Q_OBJECT

public:
    explicit LabelItem(QGraphicsItem* parent = nullptr);

    // the text with its color ("#rrggbb" or "#aarrggbb") and size ("20pt"), each kept from before when not given
    void setText(const QString& text, const std::optional<QString>& color = std::nullopt,
                 const std::optional<QString>& size = std::nullopt);
    QString text() const { return text_; }
    QString color() const { return color_; }
    QString fontSize() const { return size_; }
    QGraphicsTextItem* item() const { return item_; }
    QRectF itemRect() const;

protected:
    QSizeF sizeHint(Qt::SizeHint which, const QSizeF& constraint) const override;
    void resizeEvent(QGraphicsSceneResizeEvent* event) override;

private:
    void updateMin();

    QGraphicsTextItem* item_;
    QString text_, color_, size_;
};

class PlotItem : public QGraphicsWidget {
    Q_OBJECT

public:
    explicit PlotItem(QGraphicsItem* parent = nullptr);

    ViewBox* vb() const { return vb_; }
    QGraphicsGridLayout* gridLayout() const { return layout_; }
    AxisItem* axis(AxisItem::Orientation orientation) const { return axes_[orientation]; }
    // a different item for one side (e.g. one with time ticks)
    void setAxisItem(AxisItem::Orientation orientation, AxisItem* axis);
    // the axis' title, and the axis shown
    void setLabel(AxisItem::Orientation orientation, const QString& text,
                  const std::optional<AxisItem::Style>& style = std::nullopt);
    void showAxis(AxisItem::Orientation orientation, bool show = true);
    // none hides it
    void setTitle(const std::optional<QString>& title, const std::optional<QString>& color = std::nullopt,
                  const std::optional<QString>& size = std::nullopt);
    LabelItem* titleLabel() const { return title_; }
    // grid lines from the axes, at alpha (0 - 1; pyqtgraph's slider keeps 0 - 255 whole)
    void showGrid(std::optional<bool> x, std::optional<bool> y, std::optional<double> alpha = std::nullopt);

private:
    void updateGrid();

    QGraphicsGridLayout* layout_;
    ViewBox* vb_;
    LabelItem* title_;
    AxisItem* axes_[4] = {};
    bool xGrid_ = false, yGrid_ = false;
    int gridAlpha_ = 128;
};

class PlotWidget : public QGraphicsView {
    Q_OBJECT

public:
    explicit PlotWidget(QWidget* parent = nullptr);

    PlotItem* plotItem() const { return plotItem_; }
    ViewBox* vb() const { return plotItem_->vb(); }
    // none: transparent
    void setBackground(const std::optional<QColor>& color);
    void setXRange(double min, double max, std::optional<double> padding = std::nullopt) {
        vb()->setXRange(min, max, padding);
    }
    void setYRange(double min, double max, std::optional<double> padding = std::nullopt) {
        vb()->setYRange(min, max, padding);
    }

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void fitPlot(const QSize& size);

    PlotItem* plotItem_;
};

}  // namespace awo::plot

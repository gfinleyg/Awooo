#pragma once

// One side of a plot: its line, ticks, tick numbers and title (pyqtgraph's AxisItem, with its tick spacing, which ticks
// get numbers and how much room they're given), following a ViewBox's range. With the grid on, the ticks reach across
// the view as grid lines.

#include <QFont>
#include <QGraphicsWidget>
#include <QPen>
#include <QPointer>

#include <optional>

#include "ViewBox.hpp"

class QGraphicsTextItem;

namespace awo::plot {

class AxisItem : public QGraphicsWidget {
    Q_OBJECT

public:
    enum Orientation { Left, Right, Top, Bottom };

    // a level of ticks: (spacing, offset)
    struct TickLevel {
        double spacing;
        double offset = 0;
    };
    // the ticks of one level: (spacing, values)
    struct Ticks {
        double spacing;
        QList<double> values;
    };

    explicit AxisItem(Orientation orientation, QGraphicsItem* parent = nullptr);

    Orientation orientation() const { return orientation_; }
    bool vertical() const { return orientation_ == Left || orientation_ == Right; }

    void linkToView(ViewBox* view);
    ViewBox* linkedView() const { return view_; }
    void setRange(double min, double max);
    Range range() const { return range_; }

    // the axis line and (without a tick pen) its ticks and grid; the tick numbers and title
    void setPen(const QPen& pen);
    QPen pen() const { return pen_; }
    void setTextPen(const QPen& pen);
    QPen textPen() const { return textPen_; }
    void setTickPen(const std::optional<QPen>& pen);
    QPen tickPen() const { return tickPen_.value_or(pen_); }
    std::optional<QPen> ownTickPen() const { return tickPen_; }

    // grid lines at this alpha (0 - 255), or none
    void setGrid(std::optional<int> alpha);
    void setTickFont(const std::optional<QFont>& font);
    std::optional<QFont> tickFont() const { return tickFont_; }
    // the ticks at these spacings instead of pyqtgraph's automatic ones (none: automatic again)
    void setTickSpacing(std::optional<double> major, std::optional<double> minor = std::nullopt);

    // the title, with CSS style options for its <span> (in order); empty text and units hide it
    using Style = QList<std::pair<QString, QString>>;
    void setLabel(const QString& text, const QString& units = {}, const std::optional<Style>& style = std::nullopt);
    QString labelText() const { return labelText_; }
    QString labelUnits() const { return labelUnits_; }
    Style labelStyle() const { return labelStyle_; }
    QGraphicsTextItem* label() const { return label_; }
    void showLabel(bool show = true);

    void setShown(bool show);

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

    // pyqtgraph's automatic tick spacing for a range over a length in pixels, and the ticks at those spacings
    QList<TickLevel> tickSpacing(double minVal, double maxVal, double size) const;
    QList<Ticks> tickValues(double minVal, double maxVal, double size) const;
    // each tick's number (subclasses: other text, e.g. times)
    virtual QStringList tickStrings(const QList<double>& values, double scale, double spacing) const;

protected:
    void resizeEvent(QGraphicsSceneResizeEvent* event) override;

private:
    struct DrawSpecs {
        QPen axisPen;
        QPointF axisStart, axisEnd;
        QList<std::tuple<QPen, QPointF, QPointF>> ticks;
        QList<std::tuple<QRectF, int, QString>> texts;
    };
    std::optional<DrawSpecs> generateDrawSpecs(const QFont& font);
    void updateLabel();
    void setLabelColor(const QColor& color);
    void adjustSize();
    void updateWidth();
    void updateHeight();
    void updateMaxTextSize(double size);
    void linkedViewChanged();

    Orientation orientation_;
    QGraphicsTextItem* label_;
    QPointer<ViewBox> view_;
    Range range_{0, 1};
    QPen pen_, textPen_;
    std::optional<QPen> tickPen_;
    std::optional<int> grid_;
    std::optional<QFont> tickFont_;
    std::optional<QList<TickLevel>> tickSpacing_;
    QString labelText_, labelUnits_;
    Style labelStyle_;
    bool labelShown_ = false;
    bool shown_ = true;
    // the most room the tick numbers took across the axis, last drawn
    double textWidth_ = 30, textHeight_ = 18;
    // pyqtgraph's style options the app leaves at their defaults
    int tickTextOffset_[2] = {5, 2};
    int tickLength_ = -5;
    bool hideOverlappingLabels_;
};

}  // namespace awo::plot

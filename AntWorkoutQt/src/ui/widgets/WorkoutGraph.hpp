#pragma once

// The graph of a workout (ClassWorkoutGraph.py), the HUD's progress graph and the synopsis graph: power on the left
// axis; heart rate, cadence and speed on the right, over one time axis. Each scale has its own view box over the plot's,
// x-linked to it. Its title and axes follow the theme's Text font row and colors (applyTheme, AppTheme.theme_graph),
// again whenever the theme changes.

#include <optional>

#include "AxisItem.hpp"
#include "PlotItems.hpp"
#include "PlotWidget.hpp"

namespace awo {

// "5m", "1h 5m"; with seconds, "5m 05s", "1h 5m 05s"
QString hourMinuteFromSecond(double seconds);
QString hourMinuteSecondFromSecond(double seconds);

// the time axis: minutes, with the seconds once its ticks are closer than a minute apart (a zoomed in synopsis)
class TimeAxisItem : public plot::AxisItem {
    Q_OBJECT

public:
    using AxisItem::AxisItem;
    QStringList tickStrings(const QList<double>& values, double scale, double spacing) const override;
};

class WorkoutGraph : public plot::PlotWidget {
    Q_OBJECT

public:
    explicit WorkoutGraph(QWidget* parent = nullptr);

    plot::PlotItem* plotScreen() const { return plotItem(); }
    plot::ViewBox* powerVBox() const { return powerVBox_; }
    plot::ViewBox* hrCadenceVBox() const { return hrCadenceVBox_; }
    plot::AxisItem* axis(plot::AxisItem::Orientation orientation) const { return plotItem()->axis(orientation); }

    // the axis titles' style ("color", "font-size")
    const plot::AxisItem::Style& axisStyles() const { return axisStyles_; }
    // the right axis' title, with speed in the distance unit (see Units)
    void setSpeedLabel();
    // the extra views over the plot's view again (it resized)
    void updateViews();

    // axis titles' point size before theme scaling, if not the title's (the synopsis: its max labels' size)
    std::optional<double> axisTitlePoints;
    // the title and axes in the theme's Text font and colors, from their first look
    void applyTheme();

private:
    struct AxisLook {
        QFont tickFont;
        QString labelText, labelUnits;
        plot::AxisItem::Style labelStyle;
        QPen textPen, linePen;
        std::optional<QPen> tickPen;
    };
    struct TitleLook {
        QString text, size, color;
    };

    plot::AxisItem::Style axisStyles_;
    plot::ViewBox* powerVBox_;
    plot::ViewBox* hrCadenceVBox_;
    std::optional<TitleLook> titleLook_;
    std::optional<AxisLook> axisLooks_[4];
};

}  // namespace awo

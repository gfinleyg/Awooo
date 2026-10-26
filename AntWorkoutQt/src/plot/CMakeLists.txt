# awo_plot: the graphs' plot library, in place of pyqtgraph (PLAN.md §4.4, option A)
add_library(awo_plot STATIC
    AxisItem.cpp
    AxisItem.hpp
    PlotItems.cpp
    PlotItems.hpp
    PlotWidget.cpp
    PlotWidget.hpp
    SolidColoredColumns.cpp
    SolidColoredColumns.hpp
    ViewBox.cpp
    ViewBox.hpp
)
target_include_directories(awo_plot PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(awo_plot PUBLIC Qt6::Widgets awo_core)
awo_target_defaults(awo_plot)

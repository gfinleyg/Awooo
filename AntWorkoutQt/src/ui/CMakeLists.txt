# awo_ui: the screens and everything they're built from (PLAN.md §3.4 - §3.6)
add_library(awo_ui STATIC
    AppLayout.cpp
    AppLayout.hpp
    AppStyle.hpp
    CustomTitleBar.cpp
    CustomTitleBar.hpp
    Dialogs.cpp
    Dialogs.hpp
    KeyShortcuts.cpp
    KeyShortcuts.hpp
    MainWindow.cpp
    MainWindow.hpp
    SoundEffects.cpp
    SoundEffects.hpp
    Theme.cpp
    Theme.hpp
    ZoneColors.cpp
    ZoneColors.hpp
    widgets/AnalogGauge.cpp
    widgets/AnalogGauge.hpp
    widgets/GradeWidgets.cpp
    widgets/GradeWidgets.hpp
    widgets/LongVLabels.cpp
    widgets/LongVLabels.hpp
    widgets/MaxStar.cpp
    widgets/MaxStar.hpp
    widgets/PowerSpiral.cpp
    widgets/PowerSpiral.hpp
    widgets/SmoothProgressBar.cpp
    widgets/SmoothProgressBar.hpp
    widgets/WorkoutGraph.cpp
    widgets/WorkoutGraph.hpp
)
target_include_directories(awo_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/widgets")
target_link_libraries(awo_ui PUBLIC Qt6::Widgets Qt6::Svg Qt6::SvgWidgets awo_core awo_ant awo_plot PRIVATE miniaudio)
awo_target_defaults(awo_ui)

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
)
target_include_directories(awo_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(awo_ui PUBLIC Qt6::Widgets Qt6::Svg awo_core awo_ant PRIVATE miniaudio)
awo_target_defaults(awo_ui)

# awo_viewer: LayoutViewer's display and controls windows (LayoutViewer.py), and LayoutViewer.exe
add_library(awo_viewer STATIC
    ViewerColors.cpp
    ViewerColors.hpp
    ViewerControls.cpp
    ViewerControls.hpp
    ViewerDisplay.cpp
    ViewerDisplay.hpp
)
target_include_directories(awo_viewer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(awo_viewer PUBLIC awo_ui)
awo_target_defaults(awo_viewer)

qt_add_executable(LayoutViewer WIN32 main.cpp)
target_link_libraries(LayoutViewer PRIVATE awo_viewer)
awo_target_defaults(LayoutViewer)
add_dependencies(LayoutViewer awo_assets)

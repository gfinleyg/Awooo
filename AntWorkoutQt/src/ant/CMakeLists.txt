# awo_ant: the app's side of the ANT connection (PLAN.md §3.2), on AntNative's core directly: its Manager, Recovery
# and profiles, with a per-message callback and a Transport the tests can replace with a fake stick
add_library(awo_ant STATIC
    AntBridge.cpp
    AntBridge.hpp
    AntSticks.cpp
    AntSticks.hpp
    ReadingPipeline.cpp
    ReadingPipeline.hpp
)
target_include_directories(awo_ant PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(awo_ant PUBLIC Qt6::Core awo_core antnative_core)
awo_target_defaults(awo_ant)

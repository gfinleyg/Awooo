# awo_ant: the app's side of the ANT connection, over antnative.dll (PLAN.md §3.2)
add_library(awo_ant STATIC
    AntSticks.cpp
    AntSticks.hpp
)
target_include_directories(awo_ant PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(awo_ant PUBLIC Qt6::Core antnative)
awo_target_defaults(awo_ant)

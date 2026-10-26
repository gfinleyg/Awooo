# AntWorkout.exe
qt_add_executable(AntWorkout WIN32 main.cpp)
target_link_libraries(AntWorkout PRIVATE Qt6::Widgets awo_ant)
awo_target_defaults(AntWorkout)
add_dependencies(AntWorkout awo_assets)

# AntWorkout.exe
qt_add_executable(AntWorkout WIN32 main.cpp)
target_link_libraries(AntWorkout PRIVATE awo_ui)
awo_target_defaults(AntWorkout)
add_dependencies(AntWorkout awo_assets)

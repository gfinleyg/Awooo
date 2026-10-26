# Hardware checks against SimulANT+ (PLAN.md Phase 2 and 10)
qt_add_executable(ant_console ant_console.cpp)
target_link_libraries(ant_console PRIVATE awo_ant)
awo_target_defaults(ant_console)

# the C++ side of the SimulANT+ test (simulant_test.py starts it)
qt_add_executable(simulant_ride simulant_ride.cpp)
target_link_libraries(simulant_ride PRIVATE awo_ui)
awo_target_defaults(simulant_ride)

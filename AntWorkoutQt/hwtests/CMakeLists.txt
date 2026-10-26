# Hardware checks against SimulANT+ (PLAN.md Phase 2 and 10)
qt_add_executable(ant_console ant_console.cpp)
target_link_libraries(ant_console PRIVATE awo_ant)
awo_target_defaults(ant_console)

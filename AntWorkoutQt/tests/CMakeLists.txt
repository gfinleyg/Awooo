# Qt Test exes, run headless by ctest (PLAN.md D9). Each test runs with Qt's bin folder on PATH, as the build folder
# has no Qt DLLs of its own (only dist\ does, see build_windows.ps1).
set(AWO_QT_BIN "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")

function(awo_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE Qt6::Test)
    awo_target_defaults(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AWO_QT_BIN}")
endfunction()

awo_add_test(tst_ant_sticks tst_ant_sticks.cpp)
# (version.lib: the loaded C++ runtime's file version)
target_link_libraries(tst_ant_sticks PRIVATE Qt6::Widgets awo_ant version)

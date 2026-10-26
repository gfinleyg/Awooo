# Qt Test exes, run headless by ctest (PLAN.md D9). Each test runs with Qt's bin folder on PATH, as the build folder
# has no Qt DLLs of its own (only dist\ does, see build_windows.ps1).
set(AWO_QT_BIN "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")

function(awo_add_test name)
    add_executable(${name} ${ARGN} TestSupport.hpp)
    target_link_libraries(${name} PRIVATE Qt6::Test)
    target_compile_definitions(${name} PRIVATE
        AWO_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
        AWO_PYTHON_APP_DIR="${AWO_PYTHON_APP}")
    awo_target_defaults(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AWO_QT_BIN}")
endfunction()

awo_add_test(tst_ant_sticks tst_ant_sticks.cpp)
# (version.lib: the loaded C++ runtime's file version)
target_link_libraries(tst_ant_sticks PRIVATE Qt6::Widgets awo_ant version)

# awo_core, each checked against the Python app's results in golden/ (make_golden.py)
foreach(test core_text workout_files zwo ftp_test grading session settings logger ftp_colors workout_engine)
    awo_add_test(tst_${test} tst_${test}.cpp)
    target_link_libraries(tst_${test} PRIVATE awo_core)
endforeach()

# awo_ant on a fake stick (FakeAnt.hpp), with AntNative's real Manager and Recovery
foreach(test ant_bridge reading_pipeline)
    awo_add_test(tst_${test} tst_${test}.cpp FakeAnt.hpp)
    target_link_libraries(tst_${test} PRIVATE awo_ant)
endforeach()

# awo_ui, built offscreen
foreach(test theme key_shortcuts layout plot widgets setup)
    awo_add_test(tst_${test} tst_${test}.cpp FakeAnt.hpp)
    target_link_libraries(tst_${test} PRIVATE awo_ui)
endforeach()

# the side by side check against the Python app's widgets (parity/render_python.py renders the Python side)
add_executable(render_widgets parity/render_widgets.cpp)
target_link_libraries(render_widgets PRIVATE awo_ui)
target_compile_definitions(render_widgets PRIVATE AWO_PYTHON_APP_DIR="${AWO_PYTHON_APP}")
target_include_directories(render_widgets PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
awo_target_defaults(render_widgets)

# AntWorkoutQt: AntWorkout as a C++/Qt app (see PLAN.md). build_windows.ps1 configures, builds, tests and fills
# dist\AntWorkout\; this file can also be opened directly by any CMake-aware IDE with Qt 6.7 on CMAKE_PREFIX_PATH.

# 3.26 for copy_directory_if_different (Qt's own CMake, C:\Qt\Tools\CMake_64, is 3.27)
cmake_minimum_required(VERSION 3.26)
project(AntWorkoutQt VERSION 0.1 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "AntWorkoutQt targets Windows (the ANT SDK supplied with AntNative).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# every exe and DLL in one folder, so the app and the tests find antnative.dll and the assets beside them
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Qt 6.7 exactly: its GDI font engine is what the layouts and fonts were made with (PLAN.md, D2)
find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui Widgets Svg Concurrent Test)

# The Python app, where the fonts, sounds, images, themes and workouts still live while both apps are kept
set(AWO_PYTHON_APP "${CMAKE_CURRENT_SOURCE_DIR}/../AntWorkout")

# AntNative: antnative.dll and its header-only C++ wrapper (antnative.hpp), linked against the ANT SDK's ANT_LIB.lib
# built in the same configuration (its Debug build uses the debug C++ runtime)
set(ANTNATIVE_BUILD_PYTHON OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(ANT_LIB_CONFIG "Debug")
else()
    set(ANT_LIB_CONFIG "Release")
endif()
set(ANT_LIB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../antlib/ANT_LIB" CACHE PATH "Path to ANT_LIB")
set(ANT_LIB_OUTPUT_DIR "${ANT_LIB_ROOT}/x64/${ANT_LIB_CONFIG}")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../AntNative" antnative)

# moc and friends for this project's targets (after AntNative, which has no Qt in it)
qt_standard_project_setup()

# Warnings and source settings for this project's own targets (not AntNative's)
function(awo_target_defaults target)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endfunction()

add_subdirectory(third_party)
add_subdirectory(src/core)
add_subdirectory(src/ant)
add_subdirectory(src/ui)
add_subdirectory(src/app)
add_subdirectory(src/tools)
add_subdirectory(hwtests)

# The app's files from AntWorkout/, beside the exes: fonts, sounds and images it loads, and the themes and workouts
# to pick from (dist\ gets them from here, see build_windows.ps1)
set(AWO_BIN "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
add_custom_target(awo_assets ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different "${AWO_PYTHON_APP}/fonts" "${AWO_BIN}/fonts"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different "${AWO_PYTHON_APP}/sounds" "${AWO_BIN}/sounds"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different "${AWO_PYTHON_APP}/themes" "${AWO_BIN}/themes"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different "${AWO_PYTHON_APP}/awoWorkouts" "${AWO_BIN}/awoWorkouts"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${AWO_BIN}/images"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${AWO_PYTHON_APP}/Chainring.svg" "${AWO_PYTHON_APP}/Heart.svg" "${AWO_PYTHON_APP}/HeartSmaller.svg"
        "${AWO_PYTHON_APP}/Play.svg" "${AWO_PYTHON_APP}/Pause.svg" "${AWO_PYTHON_APP}/Stop.svg"
        "${AWO_BIN}/images/"
    COMMENT "Copying the app's fonts, sounds, images, themes and workouts from AntWorkout/"
    VERBATIM
)

enable_testing()
add_subdirectory(tests)

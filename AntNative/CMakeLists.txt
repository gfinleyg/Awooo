cmake_minimum_required(VERSION 3.21)
project(antnative LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "AntNative targets the Windows ANT SDK supplied with this project.")
endif()

# The ANT SDK, beside the Awooo checkout by default. Build its ANT_Lib.vcxproj first (build_windows.ps1 does).
set(ANT_LIB_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../antlib/ANT_LIB" CACHE PATH "Path to ANT_LIB")
set(ANT_LIB_CONFIG "Release" CACHE STRING "ANT_LIB configuration to link")
set(ANT_LIB_OUTPUT_DIR "${ANT_LIB_ROOT}/x64/${ANT_LIB_CONFIG}" CACHE PATH "ANT_LIB output directory")

option(ANTNATIVE_BUILD_PYTHON "Build the ant_native Python module (needs Python and pybind11)" ON)

# The core: ANT SDK transport and message decode, shared by both libraries below
add_library(antnative_core STATIC src/core/ant_core.cpp)
target_compile_features(antnative_core PUBLIC cxx_std_17)
target_include_directories(antnative_core
    PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/src/core"
    PRIVATE
        "${ANT_LIB_ROOT}/inc"
        "${ANT_LIB_ROOT}/software/serial"
        "${ANT_LIB_ROOT}/software/serial/device_management"
        "${ANT_LIB_ROOT}/software/system"
        "${ANT_LIB_ROOT}/software/USB"
        "${ANT_LIB_ROOT}/software/USB/devices"
        "${ANT_LIB_ROOT}/software/USB/device_handles"
)
target_link_libraries(antnative_core PUBLIC "${ANT_LIB_OUTPUT_DIR}/ANT_LIB.lib")
set_target_properties(antnative_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# antnative.dll: the C API (include/antnative.h), and the header-only C++ API over it (include/antnative.hpp)
add_library(antnative SHARED src/c_api/antnative_c.cpp)
target_include_directories(antnative PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(antnative PRIVATE antnative_core)

# The ant_native Python module
if(ANTNATIVE_BUILD_PYTHON)
    find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)
    find_package(pybind11 CONFIG REQUIRED)
    pybind11_add_module(ant_native MODULE src/python/ant_native_module.cpp)
    target_link_libraries(ant_native PRIVATE antnative_core)
endif()

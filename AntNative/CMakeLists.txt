cmake_minimum_required(VERSION 3.21)
project(ant_native LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "ANTNative targets the Windows ANT SDK supplied with this project.")
endif()

set(ANT_LIB_ROOT "${CMAKE_CURRENT_LIST_DIR}/../antlib/ANT_LIB" CACHE PATH "Path to ANT_LIB")

find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)
find_package(pybind11 CONFIG REQUIRED)

# This wrapper intentionally links the existing ANT_LIB static library.
# Build ANT_LIB.vcxproj first (Release or Debug as appropriate).
set(ANT_LIB_CONFIG "Release" CACHE STRING "ANT_LIB configuration to link")
set(ANT_LIB_OUTPUT_DIR "${ANT_LIB_ROOT}/${ANT_LIB_CONFIG}" CACHE PATH "ANT_LIB output directory")

add_library(ant_native MODULE src/ant_native.cpp)
target_compile_features(ant_native PRIVATE cxx_std_17)

target_include_directories(ant_native PRIVATE
    "${ANT_LIB_ROOT}/inc"
    "${ANT_LIB_ROOT}/software/serial"
    "${ANT_LIB_ROOT}/software/serial/device_management"
    "${ANT_LIB_ROOT}/software/system"
    "${ANT_LIB_ROOT}/software/USB"
    "${ANT_LIB_ROOT}/software/USB/devices"
    "${ANT_LIB_ROOT}/software/USB/device_handles"
)

target_link_libraries(ant_native PRIVATE
    pybind11::module
    "${ANT_LIB_OUTPUT_DIR}/ANT_LIB.lib"
)

set_target_properties(ant_native PROPERTIES
    PREFIX ""
    SUFFIX ".pyd"
    OUTPUT_NAME "ant_native"
)

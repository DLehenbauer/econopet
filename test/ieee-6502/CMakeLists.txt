# SPDX-License-Identifier: CC0-1.0
cmake_minimum_required(VERSION 3.21)
project(ieee-6502-probes C CXX)

if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR "ROM investigation probes require Debug")
endif()
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_executable(rom4040_probe
    probe.c
    "${ROOT}/fw/src/ieee/rom4040.c"
    "${ROOT}/fw/src/ieee/controller4040.c"
    "${ROOT}/fw/src/ieee/fifo4040.c"
    "${ROOT}/fw/src/ieee/diskimage.c"
    "${ROOT}/fw/src/cbm/petscii.c"
    "${ROOT}/test/support/d64_fixture.c"
    "${ROOT}/test/support/test_fatal.c")
target_include_directories(rom4040_probe PRIVATE
    "${ROOT}/fw/src" "${ROOT}/fw/test" "${ROOT}/test/support"
    "${ROOT}/test/external/chips/chips")
target_compile_options(rom4040_probe PRIVATE -Wall -Wextra -Werror)
target_compile_features(rom4040_probe PRIVATE c_std_11)

# Keep optimized Verilator/runtime objects separate from Debug integration code.
find_package(verilator REQUIRED)
if(verilator_FOUND)
    add_library(ieee4040_model STATIC)
    verilate(ieee4040_model
        SOURCES "${ROOT}/gw/EconoPET/src/common_pkg.sv" "${ROOT}/gw/EconoPET/src/ieee.sv"
        TOP_MODULE ieee PREFIX Vieee
        VERILATOR_ARGS --assert -O3 -Wno-fatal)
    target_compile_options(ieee4040_model PRIVATE -O3 -g -march=native -flto)
    target_compile_definitions(ieee4040_model PRIVATE NDEBUG)
    get_target_property(MODEL_INCLUDES ieee4040_model INTERFACE_INCLUDE_DIRECTORIES)
    target_include_directories(ieee4040_model SYSTEM INTERFACE ${MODEL_INCLUDES})
    set_property(TARGET ieee4040_model PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    add_executable(fifo4040_probe fifo_probe.cpp
        "${ROOT}/fw/src/ieee/ieee_drive.c"
        "${ROOT}/fw/src/ieee/rom4040.c"
        "${ROOT}/fw/src/ieee/controller4040.c"
        "${ROOT}/fw/src/ieee/fifo4040.c"
        "${ROOT}/fw/src/ieee/diskimage.c"
        "${ROOT}/fw/src/sd/path.c"
        "${ROOT}/fw/src/fatal.c"
        "${ROOT}/fw/src/cbm/petscii.c"
        "${ROOT}/test/support/d64_fixture.c"
        "${ROOT}/test/support/test_support.c"
        "${ROOT}/test/support/test_fatal.c"
        "${ROOT}/test/system/ieee/ieee_drive_log_stub.c")
    target_include_directories(fifo4040_probe PRIVATE
        "${ROOT}/fw/src" "${ROOT}/fw/test" "${ROOT}/test/support"
        "${ROOT}/test/external/chips/chips")
    target_compile_features(fifo4040_probe PRIVATE cxx_std_17 c_std_11)
    target_compile_options(fifo4040_probe PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(fifo4040_probe PRIVATE ieee4040_model)
    target_link_options(fifo4040_probe PRIVATE -Wl,--wrap=fopen)
endif()

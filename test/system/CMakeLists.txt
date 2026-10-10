# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

cmake_minimum_required(VERSION 3.20...3.31)
project(econopet-system-tests LANGUAGES C CXX)
# Frameworks and fixtures always retain their Debug-only paranoid checks.
if("${CMAKE_BUILD_TYPE}" STREQUAL "")
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build configuration" FORCE)
endif()
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR "Host system tests require CMAKE_BUILD_TYPE=Debug")
endif()
include(CTest)
option(BUILD_BOARD_TESTS "Build production-top board simulation tests with Verilator" OFF)

# Compile transport-independent fixtures without a board or firmware runtime.
get_filename_component(ECONOPET_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_library(econopet_disk_fixtures STATIC
    "${ECONOPET_ROOT}/test/support/test_fatal.c"
    "${ECONOPET_ROOT}/test/support/d64_fixture.c"
    "${ECONOPET_ROOT}/fw/src/ieee/diskimage.c"
    "${ECONOPET_ROOT}/fw/src/cbm/petscii.c")
target_include_directories(econopet_disk_fixtures PUBLIC "${ECONOPET_ROOT}/test/support")
target_include_directories(econopet_disk_fixtures PUBLIC "${ECONOPET_ROOT}/fw/src")
set_target_properties(econopet_disk_fixtures PROPERTIES C_STANDARD 11)

# Build digital peripheral adapters independently of the FPGA simulator.
set(CHIPS_INCLUDE_DIR "${ECONOPET_ROOT}/test/external/chips/chips")
if(NOT EXISTS "${CHIPS_INCLUDE_DIR}/m6520.h" OR NOT EXISTS "${CHIPS_INCLUDE_DIR}/m6522.h")
    message(FATAL_ERROR
        "Missing chips models. Run: git submodule update --init test/external/chips")
endif()
add_library(econopet_external_io STATIC io.cpp)
target_compile_features(econopet_external_io PUBLIC cxx_std_23)
target_include_directories(econopet_external_io SYSTEM PUBLIC "${CHIPS_INCLUDE_DIR}")
target_include_directories(econopet_external_io PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}" "${ECONOPET_ROOT}/fw/src")

if(NOT BUILD_TESTING)
    return()
endif()

# Keep the test framework out of the reusable fixture library.
include(FetchContent)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

# Discover fixture, value-type and peripheral invariants without a simulated board.
add_executable(econopet_system_tests disk_fixture_test.cpp io_test.cpp types_test.cpp registers_test.cpp)
target_compile_features(econopet_system_tests PRIVATE cxx_std_23)
target_link_libraries(econopet_system_tests PRIVATE
    econopet_disk_fixtures econopet_external_io GTest::gtest_main)
set(SYSTEM_DISCOVERY_TIMEOUT_SECONDS 60)
set(SYSTEM_TEST_TIMEOUT_SECONDS 300)
# Preserve the label list through GoogleTest's generated CTest property list.
gtest_discover_tests(econopet_system_tests
    TEST_PREFIX "system."
    DISCOVERY_TIMEOUT ${SYSTEM_DISCOVERY_TIMEOUT_SECONDS}
    PROPERTIES LABELS "system\\;host\\;unit" TIMEOUT ${SYSTEM_TEST_TIMEOUT_SECONDS})

include(hardware_contract.cmake)

if(BUILD_BOARD_TESTS)
    include(board.cmake)
endif()

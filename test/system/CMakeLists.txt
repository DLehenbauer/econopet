# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

cmake_minimum_required(VERSION 3.20...3.31)
project(econopet-system-tests LANGUAGES C CXX)
include(CTest)

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

# Discover each fixture invariant as an independently runnable host test.
add_executable(econopet_system_tests disk_fixture_test.cpp)
target_compile_features(econopet_system_tests PRIVATE cxx_std_17)
target_link_libraries(econopet_system_tests PRIVATE econopet_disk_fixtures GTest::gtest_main)
set(SYSTEM_DISCOVERY_TIMEOUT_SECONDS 60)
set(SYSTEM_TEST_TIMEOUT_SECONDS 300)
# Preserve the label list through GoogleTest's generated CTest property list.
gtest_discover_tests(econopet_system_tests
    TEST_PREFIX "system."
    DISCOVERY_TIMEOUT ${SYSTEM_DISCOVERY_TIMEOUT_SECONDS}
    PROPERTIES LABELS "system\\;host\\;unit" TIMEOUT ${SYSTEM_TEST_TIMEOUT_SECONDS})

include(hardware_contract.cmake)

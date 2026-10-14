# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

if(NOT EXISTS "${ECONOPET_ROOT}/fw/external/libyaml/include/yaml.h")
    message(FATAL_ERROR
        "Missing firmware headers. Run: git submodule update --init fw/external/libyaml")
endif()

# Compile real drive logic in Debug, replacing only platform transport and logging.
add_library(econopet_firmware_transport STATIC
    firmware.cpp
    ieee/ieee_drive_spi_adapter.cpp
    ieee/ieee_drive_log_stub.c
    "${ECONOPET_ROOT}/fw/src/ieee/ieee_drive.c"
    "${ECONOPET_ROOT}/fw/src/cbm/filename.c"
    "${ECONOPET_ROOT}/fw/src/fatal.c"
    "${ECONOPET_ROOT}/fw/src/sd/path.c"
    "${ECONOPET_ROOT}/test/support/test_support.c")
target_compile_features(econopet_firmware_transport PUBLIC cxx_std_23 PRIVATE c_std_11)
target_include_directories(econopet_firmware_transport PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_include_directories(econopet_firmware_transport PRIVATE
    "${ECONOPET_ROOT}/fw/external/libyaml/include")
# C frames must unwind when a C++ SPI callback or deadline throws.
target_compile_options(econopet_firmware_transport PRIVATE $<$<COMPILE_LANGUAGE:C>:-fexceptions>)
target_link_libraries(econopet_firmware_transport PUBLIC
    econopet_disk_fixtures econopet_fpga_model econopet_external_io)
# Firmware fopen reaches owned cookie-backed fixtures, not SPI mocks.
target_link_options(econopet_firmware_transport INTERFACE -Wl,--wrap=fopen)

# Isolate firmware globals and fopen wrapping from board-only and host-only tests.
add_executable(econopet_firmware_tests firmware_test.cpp framework/scheduler.cpp)
target_compile_features(econopet_firmware_tests PRIVATE cxx_std_23)
target_link_libraries(econopet_firmware_tests PRIVATE
    econopet_firmware_transport GTest::gtest_main GTest::gmock)
set_target_properties(econopet_firmware_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${BOARD_RUNTIME_DIR}")
gtest_discover_tests(econopet_firmware_tests
    TEST_PREFIX "firmware."
    WORKING_DIRECTORY "${BOARD_RUNTIME_DIR}"
    DISCOVERY_TIMEOUT ${SYSTEM_DISCOVERY_TIMEOUT_SECONDS}
    PROPERTIES LABELS "system\\;board\\;firmware" TIMEOUT ${SYSTEM_TEST_TIMEOUT_SECONDS})

# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

if(NOT EXISTS "${ECONOPET_ROOT}/fw/external/libyaml/include/yaml.h")
    message(FATAL_ERROR
        "Missing firmware headers. Run: git submodule update --init fw/external/libyaml")
endif()

# Build the bundled YAML parser without enabling its upstream tests.
function(add_system_test_yaml)
    set(BUILD_TESTING OFF)
    add_subdirectory(
        "${ECONOPET_ROOT}/fw/external/libyaml"
        "${CMAKE_CURRENT_BINARY_DIR}/libyaml"
        EXCLUDE_FROM_ALL)
endfunction()

add_system_test_yaml()

# Compile real firmware logic in Debug with host platform adapters and fixtures.
add_library(econopet_host_firmware STATIC
    firmware.cpp
    "${ECONOPET_ROOT}/fw/src/config/config.c"
    "${ECONOPET_ROOT}/fw/src/driver_io.c"
    ieee/ieee_drive_spi_adapter.cpp
    ieee/ieee_drive_log_stub.c
    "${ECONOPET_ROOT}/fw/src/ieee/ieee_drive.c"
    "${ECONOPET_ROOT}/fw/src/ieee/dos_media.c"
    "${ECONOPET_ROOT}/fw/src/ieee/dos_rel.c"
    "${ECONOPET_ROOT}/fw/src/ieee/dos.c"
    "${ECONOPET_ROOT}/fw/src/cbm/filename.c"
    "${ECONOPET_ROOT}/fw/src/fatal.c"
    "${ECONOPET_ROOT}/fw/src/sd/path.c"
    "${ECONOPET_ROOT}/test/support/test_support.c")
target_compile_features(econopet_host_firmware PUBLIC cxx_std_23 PRIVATE c_std_11)
target_include_directories(econopet_host_firmware PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
# C frames must unwind when a C++ simulator callback or deadline throws.
target_compile_options(econopet_host_firmware PRIVATE $<$<COMPILE_LANGUAGE:C>:-fexceptions>)
target_link_libraries(econopet_host_firmware PUBLIC
    econopet_disk_fixtures econopet_fpga_model econopet_external_io PRIVATE yaml)
# Firmware fopen reaches owned cookie-backed fixtures, not SPI mocks.
target_link_options(econopet_host_firmware INTERFACE -Wl,--wrap=fopen)

# Session tests need ROM-shaped arrays for host firmware, not private glyphs or menu code.
string(REPEAT "0x00,\n" 2048 MENU_TEST_FONT_HEADER)
string(REPEAT "0xea,\n" 256 MENU_TEST_ROM_HEADER)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/roms/901447_10.h" CONTENT "${MENU_TEST_FONT_HEADER}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/roms/menu_rom.h" CONTENT "${MENU_TEST_ROM_HEADER}")
add_library(econopet_menu_firmware STATIC
    "${ECONOPET_ROOT}/fw/src/input/pet_keyboard.c"
    "${ECONOPET_ROOT}/fw/src/driver_sync.c"
    "${ECONOPET_ROOT}/fw/src/system/system.c"
    "${ECONOPET_ROOT}/fw/src/system/state.c"
    "${ECONOPET_ROOT}/fw/src/roms/roms.c")
target_compile_options(econopet_menu_firmware PRIVATE -fexceptions -ffunction-sections)
target_link_libraries(econopet_menu_firmware PUBLIC econopet_host_firmware PRIVATE yaml)
target_include_directories(econopet_menu_firmware PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
add_executable(econopet_menu_tests menu_session_test.cpp menu_platform.cpp framework/scheduler.cpp)
target_compile_features(econopet_menu_tests PRIVATE cxx_std_23)
target_link_libraries(econopet_menu_tests PRIVATE econopet_menu_firmware GTest::gtest_main)
# Unused boot/storage entry points are not host platform APIs.
target_link_options(econopet_menu_tests PRIVATE -Wl,--gc-sections -Wl,--wrap=time_us_64)
set_target_properties(econopet_menu_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BOARD_RUNTIME_DIR}")
gtest_discover_tests(econopet_menu_tests TEST_PREFIX "menu."
    WORKING_DIRECTORY "${BOARD_RUNTIME_DIR}" DISCOVERY_TIMEOUT ${SYSTEM_DISCOVERY_TIMEOUT_SECONDS}
    PROPERTIES LABELS "sys\\;system\\;board\\;firmware"
        ${SYSTEM_TEST_ENV_PROPERTIES} TIMEOUT ${SYSTEM_TEST_TIMEOUT_SECONDS})

# Replay external reference transactions without making private ROMs CTest inputs.
add_executable(econopet_drive_reference ieee/reference_replay.cpp framework/scheduler.cpp)
target_compile_features(econopet_drive_reference PRIVATE cxx_std_23)
target_link_libraries(econopet_drive_reference PRIVATE econopet_host_firmware)
set_target_properties(econopet_drive_reference PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${BOARD_RUNTIME_DIR}")

# Isolate firmware globals and fopen wrapping from board-only and host-only tests.
add_executable(econopet_firmware_tests
    econopet_top_smoke.cpp firmware_test.cpp waterloo_keyboard_test.cpp os9_keyboard_test.cpp ieee/drive_test.cpp
    ieee/commodore_rom_test.cpp ieee/waterloo_rom_test.cpp framework/scheduler.cpp)
target_compile_features(econopet_firmware_tests PRIVATE cxx_std_23)
target_compile_definitions(econopet_firmware_tests PRIVATE
    ECONOPET_SDCARD_CONFIG="${ECONOPET_ROOT}/sdcard/config.yaml")
target_link_libraries(econopet_firmware_tests PRIVATE
    econopet_host_firmware GTest::gtest_main GTest::gmock)
set_target_properties(econopet_firmware_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${BOARD_RUNTIME_DIR}")
gtest_discover_tests(econopet_firmware_tests
    TEST_PREFIX "firmware."
    WORKING_DIRECTORY "${BOARD_RUNTIME_DIR}"
    DISCOVERY_TIMEOUT ${SYSTEM_DISCOVERY_TIMEOUT_SECONDS}
    PROPERTIES LABELS "sys\\;system\\;board\\;firmware"
        ${SYSTEM_TEST_ENV_PROPERTIES} TIMEOUT ${SYSTEM_TEST_TIMEOUT_SECONDS})

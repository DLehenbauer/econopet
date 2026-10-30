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
    "${ECONOPET_ROOT}/fw/src/ieee/rom4040.c"
    "${ECONOPET_ROOT}/fw/src/ieee/controller4040.c"
    "${ECONOPET_ROOT}/fw/src/ieee/fifo4040.c"
    "${ECONOPET_ROOT}/fw/src/cbm/filename.c"
    "${ECONOPET_ROOT}/fw/src/fatal.c"
    "${ECONOPET_ROOT}/fw/src/sd/path.c"
    "${ECONOPET_ROOT}/test/support/test_support.c")
target_compile_features(econopet_firmware_transport PUBLIC cxx_std_23 PRIVATE c_std_11)
target_include_directories(econopet_firmware_transport PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_include_directories(econopet_firmware_transport PRIVATE
    "${ECONOPET_ROOT}/fw/external/libyaml/include" "${CHIPS_INCLUDE_DIR}")
# C frames must unwind when a C++ SPI callback or deadline throws.
target_compile_options(econopet_firmware_transport PRIVATE $<$<COMPILE_LANGUAGE:C>:-fexceptions>)
target_link_libraries(econopet_firmware_transport PUBLIC
    econopet_disk_fixtures econopet_fpga_model econopet_external_io)
# Firmware fopen reaches owned cookie-backed fixtures, not SPI mocks.
target_link_options(econopet_firmware_transport INTERFACE -Wl,--wrap=fopen)

# Replay external reference transactions without making private ROMs CTest inputs.
add_executable(econopet_drive_reference ieee/reference_replay.cpp framework/scheduler.cpp)
target_compile_features(econopet_drive_reference PRIVATE cxx_std_23)
target_link_libraries(econopet_drive_reference PRIVATE econopet_firmware_transport)
set_target_properties(econopet_drive_reference PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${BOARD_RUNTIME_DIR}")

# Execute an independent peripheral implementation out-of-process when explicitly available.
add_executable(econopet_riot_reference ieee/riot_reference.cpp)
target_compile_features(econopet_riot_reference PRIVATE cxx_std_23)
target_link_libraries(econopet_riot_reference PRIVATE econopet_firmware_transport)
add_executable(econopet_controller_reference ieee/controller_reference.cpp)
target_compile_features(econopet_controller_reference PRIVATE cxx_std_23)
target_link_libraries(econopet_controller_reference PRIVATE econopet_firmware_transport)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME reference.riot_integrity COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/ieee/riot_reference_test.py"
    "$<TARGET_FILE:econopet_riot_reference>")
set_tests_properties(reference.riot_integrity PROPERTIES LABELS "system;reference" TIMEOUT 30)
add_test(NAME reference.controller_integrity COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/ieee/controller_reference_test.py")
set_tests_properties(reference.controller_integrity PROPERTIES LABELS "system;reference" TIMEOUT 30)
find_program(ECONOPET_MAME NAMES mame PATHS /usr/games)
if(ECONOPET_MAME)
    add_test(NAME reference.riot_mame COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_LIST_DIR}/ieee/riot_reference.py"
        --mame "${ECONOPET_MAME}" --replay "$<TARGET_FILE:econopet_riot_reference>")
    set_tests_properties(reference.riot_mame PROPERTIES
        LABELS "system;reference" TIMEOUT 120)
    foreach(scenario normal protected absent errors format)
        add_test(NAME "reference.controller_mame_${scenario}"
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/ieee/controller_reference.py"
                --mame "${ECONOPET_MAME}" --replay "$<TARGET_FILE:econopet_controller_reference>"
                --scenario "${scenario}")
        set_tests_properties("reference.controller_mame_${scenario}" PROPERTIES
            LABELS "system;reference;controller" TIMEOUT 240)
    endforeach()
    # Stock formatting exercises the independent emulator's mechanical/GCR path.
    # Isolate that heavier reference run, not production execution or acceptance timing.
    set_tests_properties(reference.controller_mame_format PROPERTIES RUN_SERIAL TRUE)
else()
    message(STATUS "MAME not found: independent RIOT/controller agreement is NOT tested")
endif()

# Isolate firmware globals and fopen wrapping from board-only and host-only tests.
add_executable(econopet_firmware_tests
    econopet_top_smoke.cpp firmware_test.cpp ieee/hardware4040_test.cpp
    ieee/rom4040_test.cpp framework/scheduler.cpp)
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

# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

find_package(verilator REQUIRED HINTS $ENV{VERILATOR_ROOT})

# Consume exactly the production design sources registered with Efinity.
set(GATEWARE_DIR "${ECONOPET_ROOT}/gw/EconoPET")
set(GATEWARE_PROJECT "${GATEWARE_DIR}/EconoPET.xml")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GATEWARE_PROJECT}")
file(STRINGS "${GATEWARE_PROJECT}" GATEWARE_ENTRIES REGEX "<efx:design_file[^>]*name=\"[^\"]+\"")
set(GATEWARE_SOURCES)
foreach(ENTRY IN LISTS GATEWARE_ENTRIES)
    string(REGEX REPLACE ".*name=\"([^\"]+)\".*" "\\1" SOURCE_PATH "${ENTRY}")
    list(APPEND GATEWARE_SOURCES "${GATEWARE_DIR}/${SOURCE_PATH}")
endforeach()

# Isolate generated models and runtime optimization from Debug framework sources.
set(VERILATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/verilated")
add_library(econopet_fpga_model STATIC)
target_compile_features(econopet_fpga_model PRIVATE cxx_std_23)
target_compile_options(econopet_fpga_model PRIVATE -O3 -march=native)
target_compile_definitions(econopet_fpga_model PRIVATE NDEBUG)
set_target_properties(econopet_fpga_model PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
verilate(econopet_fpga_model
    PREFIX Vsystem
    TOP_MODULE system
    DIRECTORY "${VERILATED_DIR}"
    SOURCES ${GATEWARE_SOURCES} "${GATEWARE_DIR}/sim/system.sv" "${GATEWARE_DIR}/verilator.vlt"
    # Match verilate.sh's warning policy without suppressing diagnostic output.
    VERILATOR_ARGS --assert -O3 --timescale 1ns/1ps -Wno-fatal
        "-I${GATEWARE_DIR}/external/m6502/rtl")
target_include_directories(econopet_fpga_model SYSTEM PUBLIC
    "${VERILATED_DIR}" "${VERILATOR_ROOT}/include")

# Keep board tests independent of ROM media, firmware transport and later APIs.
add_executable(econopet_board_tests board_test.cpp observation_test.cpp spi_cpu_test.cpp rom_test.cpp trace_board_test.cpp)
target_compile_features(econopet_board_tests PRIVATE cxx_std_23)
target_link_libraries(econopet_board_tests PRIVATE econopet_fpga_model econopet_external_io GTest::gtest_main)
set(BOARD_RUNTIME_DIR "${CMAKE_CURRENT_BINARY_DIR}/board")
set_target_properties(econopet_board_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BOARD_RUNTIME_DIR}")

# Resolve the SID model's relative lookup-table path beside the test executable.
file(MAKE_DIRECTORY "${BOARD_RUNTIME_DIR}/external/icesid/icesid")
configure_file("${GATEWARE_DIR}/external/icesid/icesid/curve_6581.hex"
    "${BOARD_RUNTIME_DIR}/external/icesid/icesid/curve_6581.hex" COPYONLY)
gtest_discover_tests(econopet_board_tests
    TEST_PREFIX "board."
    WORKING_DIRECTORY "${BOARD_RUNTIME_DIR}"
    DISCOVERY_TIMEOUT ${SYSTEM_DISCOVERY_TIMEOUT_SECONDS}
    PROPERTIES LABELS "system\\;board" TIMEOUT ${SYSTEM_TEST_TIMEOUT_SECONDS})

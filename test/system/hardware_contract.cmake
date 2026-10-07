# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

find_program(IVERILOG_EXECUTABLE iverilog REQUIRED)
find_program(VVP_EXECUTABLE vvp REQUIRED)

# Discover all exports through the preprocessor and reject unsupported declarations.
set(HARDWARE_CONTRACT_HEADER "${ECONOPET_ROOT}/fw/src/hardware_contract.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${HARDWARE_CONTRACT_HEADER}")
include("${CMAKE_CURRENT_LIST_DIR}/hardware_contract/discovery.cmake")
econopet_discover_hardware_contract("${HARDWARE_CONTRACT_HEADER}" HARDWARE_CONTRACT_NAMES)
set(HARDWARE_CONTRACT_C_OUTPUT "")
set(HARDWARE_CONTRACT_SV_OUTPUT "")
foreach(NAME IN LISTS HARDWARE_CONTRACT_NAMES)
    string(APPEND HARDWARE_CONTRACT_C_OUTPUT
        "    if (printf(\"${NAME}=%\" PRIu64 \"\\n\", (uint64_t) ECONOPET_${NAME}) < 0) {\n"
        "        perror(\"hardware contract output\");\n"
        "        return EXIT_FAILURE;\n"
        "    }\n")
    string(APPEND HARDWARE_CONTRACT_SV_OUTPUT
        "        $display(\"${NAME}=%0d\", common_pkg::${NAME});\n")
endforeach()

# Compile each language's expressions rather than interpreting source text.
configure_file("${CMAKE_CURRENT_LIST_DIR}/hardware_contract/values.c.in"
    hardware_contract_values.c @ONLY)
configure_file("${ECONOPET_ROOT}/gw/EconoPET/sim/hardware_contract_tb.sv.in"
    hardware_contract_tb.sv @ONLY)
add_executable(econopet_hardware_contract_values "${CMAKE_CURRENT_BINARY_DIR}/hardware_contract_values.c")
target_include_directories(econopet_hardware_contract_values PRIVATE "${ECONOPET_ROOT}/fw/src")
set_target_properties(econopet_hardware_contract_values PROPERTIES C_STANDARD 11)
set(HARDWARE_CONTRACT_IMAGE "${CMAKE_CURRENT_BINARY_DIR}/hardware_contract_tb.vvp")
add_custom_command(
    OUTPUT "${HARDWARE_CONTRACT_IMAGE}"
    COMMAND "${IVERILOG_EXECUTABLE}" -g2012 -s hardware_contract_tb
        -o "${HARDWARE_CONTRACT_IMAGE}"
        "${ECONOPET_ROOT}/gw/EconoPET/src/common_pkg.sv"
        "${CMAKE_CURRENT_BINARY_DIR}/hardware_contract_tb.sv"
    DEPENDS "${ECONOPET_ROOT}/gw/EconoPET/src/common_pkg.sv"
        "${CMAKE_CURRENT_BINARY_DIR}/hardware_contract_tb.sv"
    VERBATIM)
add_custom_target(econopet_hardware_contract_rtl ALL DEPENDS "${HARDWARE_CONTRACT_IMAGE}")

# Compare complete named output, reporting tool failures and mismatches distinctly.
add_test(NAME system.HardwareContract.MatchesCommonPackage
    COMMAND "${CMAKE_COMMAND}"
        "-DCONTRACT_VALUES=$<TARGET_FILE:econopet_hardware_contract_values>"
        "-DVVP_EXECUTABLE=${VVP_EXECUTABLE}"
        "-DCONTRACT_IMAGE=${HARDWARE_CONTRACT_IMAGE}"
        -P "${CMAKE_CURRENT_LIST_DIR}/hardware_contract/test.cmake")
set_tests_properties(system.HardwareContract.MatchesCommonPackage PROPERTIES
    LABELS "system;host;contract" TIMEOUT 60)

# Guard discovery against whitespace variations and unsupported macro forms.
add_test(NAME system.HardwareContract.Discovery
    COMMAND "${CMAKE_COMMAND}"
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DDISCOVERY_MODULE=${CMAKE_CURRENT_LIST_DIR}/hardware_contract/discovery.cmake"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/hardware_contract_discovery"
        -P "${CMAKE_CURRENT_LIST_DIR}/hardware_contract/discovery_test.cmake")
set_tests_properties(system.HardwareContract.Discovery PROPERTIES
    LABELS "system;host;contract" TIMEOUT 60)

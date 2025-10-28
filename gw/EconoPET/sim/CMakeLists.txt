# Verilog/SystemVerilog Simulation Tests using Icarus Verilog
cmake_minimum_required(VERSION 3.13)

project(EconoPET_Simulation_Tests NONE)

# Find iverilog and vvp
find_program(IVERILOG iverilog REQUIRED)
find_program(VVP vvp REQUIRED)

# Set paths
set(PROJ_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(WORK_SIM_DIR ${PROJ_DIR}/work_sim)
set(VVP_FILE ${WORK_SIM_DIR}/EconoPET.vvp)

# Enable testing
enable_testing()

# Custom target to compile the Verilog
add_custom_command(
    OUTPUT ${VVP_FILE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${WORK_SIM_DIR}
    COMMAND cd ${PROJ_DIR} && ${IVERILOG} -g2009 -s "sim" 
        -o ${VVP_FILE}
        -f ${WORK_SIM_DIR}/pkgs.f
        -f ${WORK_SIM_DIR}/EconoPET.f
        -f ${WORK_SIM_DIR}/timescale.f
        -I external/65xx
    WORKING_DIRECTORY ${PROJ_DIR}
    COMMENT "Compiling Verilog testbenches with iverilog"
    VERBATIM
)

add_custom_target(verilog_compile DEPENDS ${VVP_FILE})

# Add a test for each passing testbench
# Note: The sim.sv file runs all tests, so we run it once and check output
add_test(
    NAME verilog_simulation_tests
    COMMAND ${VVP} ${VVP_FILE}
    WORKING_DIRECTORY ${PROJ_DIR}
)

set_tests_properties(verilog_simulation_tests PROPERTIES
    LABELS "verilog;simulation;gateware"
    TIMEOUT 300
)

# Add dependency on compilation
add_test(
    NAME verilog_compile_check
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target verilog_compile
)

set_tests_properties(verilog_compile_check PROPERTIES
    LABELS "verilog;build"
    FIXTURES_SETUP verilog_fixture
)

set_tests_properties(verilog_simulation_tests PROPERTIES
    FIXTURES_REQUIRED verilog_fixture
)

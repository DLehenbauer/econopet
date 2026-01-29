# Verilog/SystemVerilog Simulation Tests using Icarus Verilog
cmake_minimum_required(VERSION 3.13)

project(EconoPET_Simulation_Tests NONE)

# Find iverilog and vvp
find_program(IVERILOG iverilog REQUIRED)
find_program(VVP vvp REQUIRED)

# Set paths
set(PROJ_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(WORK_SIM_DIR ${PROJ_DIR}/work_sim)

# Enable testing
enable_testing()

# Auto-discover all testbench files
file(GLOB TB_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_tb.sv")

message(STATUS "Found testbenches:")
foreach(TB_FILE ${TB_FILES})
    get_filename_component(TB_NAME ${TB_FILE} NAME_WE)
    message(STATUS "  - ${TB_NAME}")
endforeach()

# Create a test for each testbench
foreach(TB_FILE ${TB_FILES})
    get_filename_component(TB_NAME ${TB_FILE} NAME_WE)
    set(VVP_FILE ${WORK_SIM_DIR}/${TB_NAME}.vvp)

    # Custom command to compile the testbench
    add_custom_command(
        OUTPUT ${VVP_FILE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${WORK_SIM_DIR}
        COMMAND cd ${PROJ_DIR} && ${IVERILOG} -g2009 -s "${TB_NAME}"
            -o ${VVP_FILE}
            -f ${WORK_SIM_DIR}/pkgs.f
            -f ${WORK_SIM_DIR}/EconoPET.f
            -f ${WORK_SIM_DIR}/timescale.f
            -I external/65xx
            -DECONOPET_ROMS_DIR=\"${ECONOPET_ROMS_DIR}\"
        WORKING_DIRECTORY ${PROJ_DIR}
        COMMENT "Compiling ${TB_NAME} with iverilog"
        VERBATIM
    )

    # Create a target for compiling this testbench
    add_custom_target(${TB_NAME}_compile DEPENDS ${VVP_FILE})

    # Add build test (compiles the testbench)
    add_test(
        NAME ${TB_NAME}_build
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ${TB_NAME}_compile
    )
    set_tests_properties(${TB_NAME}_build PROPERTIES
        LABELS "verilog;build"
        FIXTURES_SETUP ${TB_NAME}_fixture
    )

    # Add run test (executes the compiled testbench)
    add_test(
        NAME ${TB_NAME}
        COMMAND ${VVP} ${VVP_FILE}
        WORKING_DIRECTORY ${PROJ_DIR}
    )
    set_tests_properties(${TB_NAME} PROPERTIES
        LABELS "verilog;simulation;gateware"
        TIMEOUT 300
        FIXTURES_REQUIRED ${TB_NAME}_fixture
    )
endforeach()

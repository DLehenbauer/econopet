# Verilog/SystemVerilog Simulation Tests using Icarus Verilog
cmake_minimum_required(VERSION 3.13)

project(EconoPET_Simulation_Tests NONE)

# Find iverilog and vvp
find_program(IVERILOG iverilog REQUIRED)
find_program(VVP vvp REQUIRED)

# Set paths
set(PROJ_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(WORK_SIM_DIR ${PROJ_DIR}/work_sim)
set(REPO_DIR ${PROJ_DIR}/../..)
set(SIM_FILELIST ${WORK_SIM_DIR}/EconoPET.f)
set(TIMESCALE_FILE ${WORK_SIM_DIR}/timescale.f)

# Enable testing
enable_testing()

# Track the sources registered with Efinity and headers beside those sources so
# changes rebuild cached Icarus images.
set(PROJECT_FILE "${PROJ_DIR}/EconoPET.xml")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_FILE}")
file(STRINGS "${PROJECT_FILE}" SIM_SOURCE_ENTRIES
    REGEX "<efx:(design|sim)_file[^>]*name=\"[^\"]+\""
)

set(SIM_SOURCE_FILES)
set(SIM_SOURCE_DIRS)
foreach(SOURCE_ENTRY IN LISTS SIM_SOURCE_ENTRIES)
    string(REGEX REPLACE ".*name=\"([^\"]+)\".*" "\\1" SOURCE_PATH "${SOURCE_ENTRY}")
    if(NOT SOURCE_PATH MATCHES "\\.svh$")
        list(APPEND SIM_SOURCE_FILES "${PROJ_DIR}/${SOURCE_PATH}")
    endif()
    get_filename_component(SOURCE_DIR "${PROJ_DIR}/${SOURCE_PATH}" DIRECTORY)
    list(APPEND SIM_SOURCE_DIRS "${SOURCE_DIR}")
endforeach()
list(REMOVE_DUPLICATES SIM_SOURCE_FILES)
list(REMOVE_DUPLICATES SIM_SOURCE_DIRS)

foreach(SOURCE_DIR IN LISTS SIM_SOURCE_DIRS)
    file(GLOB SOURCE_HEADERS CONFIGURE_DEPENDS
        "${SOURCE_DIR}/*.vh"
        "${SOURCE_DIR}/*.svh"
    )
    list(APPEND SIM_SOURCE_FILES ${SOURCE_HEADERS})
endforeach()
list(REMOVE_DUPLICATES SIM_SOURCE_FILES)

add_custom_command(
    OUTPUT
        ${SIM_FILELIST}
        ${TIMESCALE_FILE}
    COMMAND ${REPO_DIR}/sim.sh --update-only
    DEPENDS
        "${REPO_DIR}/sim.sh"
        "${PROJECT_FILE}"
    WORKING_DIRECTORY ${REPO_DIR}
    COMMENT "Generating gateware simulation file list"
    VERBATIM
)

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
        COMMAND cd ${PROJ_DIR} && ${IVERILOG} -g2012 -s "${TB_NAME}"
            -o ${VVP_FILE}
            -f ${WORK_SIM_DIR}/EconoPET.f
            -f ${WORK_SIM_DIR}/timescale.f
            -Iexternal/m6502/rtl
            -DECONOPET_ROMS_DIR=\"${ECONOPET_ROMS_DIR}\"
        WORKING_DIRECTORY ${PROJ_DIR}
        COMMENT "Compiling ${TB_NAME} with iverilog"
        DEPENDS
            ${SIM_FILELIST}
            ${SIM_SOURCE_FILES}
            ${TIMESCALE_FILE}
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

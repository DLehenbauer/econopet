cmake_minimum_required(VERSION 3.13)

project(EconoPET_FPGA NONE)

# ECONOPET_ROMS_DIR should be passed from parent CMakeLists.txt
if(NOT DEFINED ECONOPET_ROMS_DIR)
    if(NOT DEFINED ENV{ECONOPET_ROMS_DIR} OR "$ENV{ECONOPET_ROMS_DIR}" STREQUAL "")
        message(FATAL_ERROR "ECONOPET_ROMS_DIR must be set (either as CMake variable or environment variable).")
    endif()
    set(ECONOPET_ROMS_DIR "$ENV{ECONOPET_ROMS_DIR}")
endif()
message(STATUS "Gateware using ECONOPET_ROMS_DIR=${ECONOPET_ROMS_DIR}")

set(FPGA_PROJECT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/EconoPET)
set(FPGA_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/outflow)
set(FPGA_WORK_DIR ${CMAKE_CURRENT_BINARY_DIR}/work)

message(VERBOSE "FPGA_PROJECT_DIR: ${FPGA_PROJECT_DIR}")

# Track project settings without rebuilding for Efinity's run-status updates.
set(FPGA_PROJECT_FILE "${FPGA_PROJECT_DIR}/EconoPET.xml")
set(FPGA_PROJECT_CONFIG_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/project_config.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${FPGA_PROJECT_FILE}" "${FPGA_PROJECT_CONFIG_SCRIPT}")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${FPGA_PROJECT_CONFIG_SCRIPT}" "${FPGA_PROJECT_FILE}"
    OUTPUT_VARIABLE FPGA_PROJECT_CONFIG_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE FPGA_PROJECT_CONFIG_RESULT
)
if(NOT FPGA_PROJECT_CONFIG_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to fingerprint Efinity project configuration.")
endif()
set(FPGA_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_BINARY_DIR}/project-config.sha256")
file(GENERATE OUTPUT "${FPGA_PROJECT_CONFIG_FILE}" CONTENT "${FPGA_PROJECT_CONFIG_HASH}\n")

execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${FPGA_PROJECT_CONFIG_SCRIPT}" "${FPGA_PROJECT_FILE}" --sources
    OUTPUT_VARIABLE FPGA_PROJECT_SOURCES
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE FPGA_PROJECT_SOURCES_RESULT
)
if(NOT FPGA_PROJECT_SOURCES_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to read Efinity synthesis inputs.")
endif()
string(REPLACE "\n" ";" FPGA_PROJECT_SOURCES "${FPGA_PROJECT_SOURCES}")
set(FPGA_SRC_FILES)
foreach(f IN LISTS FPGA_PROJECT_SOURCES)
    list(APPEND FPGA_SRC_FILES "${FPGA_PROJECT_DIR}/${f}")
endforeach()

# Efinity's design-file list omits included headers and memory initialization data.
list(APPEND FPGA_SRC_FILES
    "${FPGA_PROJECT_DIR}/EconoPET.peri.xml"
    "${FPGA_PROJECT_DIR}/external/m6502/rtl/cpu_6502_instructions.vh"
    "${FPGA_PROJECT_DIR}/external/icesid/icesid/curve_6581.hex"
)
foreach(f IN LISTS FPGA_SRC_FILES)
    if(NOT EXISTS "${f}")
        message(FATAL_ERROR "Missing FPGA input: ${f}")
    endif()
endforeach()

message(VERBOSE "FPGA source files: ${FPGA_SRC_FILES}")

# Option to enforce clean EconoPET.xml (check for local edits before build, restore after build)
option(ECONOPET_GW_ENFORCE_CLEAN_XML "Check for local edits to EconoPET.xml and restore after build" OFF)

if(ECONOPET_GW_ENFORCE_CLEAN_XML)
    set(XML_CHECK_COMMAND COMMAND /bin/bash -lc "git update-index -q --refresh && git diff --quiet -- EconoPET/EconoPET.xml && git diff --quiet --cached -- EconoPET/EconoPET.xml || (echo '*** ERROR: EconoPET.xml has local edits. Aborting build. ***' >&2; exit 1)")
    set(XML_RESTORE_COMMAND COMMAND /bin/bash -lc "git restore --source=HEAD --worktree -- EconoPET/EconoPET.xml || true")
else()
    set(XML_CHECK_COMMAND "")
    set(XML_RESTORE_COMMAND "")
endif()

# Define the output bitstream file
set(FPGA_BITSTREAM_FILE "${FPGA_OUTPUT_DIR}/EconoPET.hex.bin")

# Produce the bitstream file only when sources are newer.  This takes a minute or two to run,
# so we declare 'USES_TERMINAL' to give some feedback that the build is progressing.
add_custom_command(
    COMMENT "Compiling FPGA bitstream using Efinity tools (be patient)"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ${XML_CHECK_COMMAND}
    # Use timing-directed P&R for CPU-slot control into the SID's falling-edge registers.
    COMMAND "./efx.sh" --flow compile --output_dir "${FPGA_OUTPUT_DIR}" --work_dir "${FPGA_WORK_DIR}"
        --pnr_opts seed=0 optimization_level=TIMING_3 placer_effort_level=3
    ${XML_RESTORE_COMMAND}
    OUTPUT ${FPGA_BITSTREAM_FILE}
    BYPRODUCTS "${FPGA_OUTPUT_DIR}" "${FPGA_WORK_DIR}"
    DEPENDS ${FPGA_SRC_FILES} "${FPGA_PROJECT_CONFIG_FILE}" "${CMAKE_CURRENT_SOURCE_DIR}/efx.sh"
    USES_TERMINAL
    VERBATIM
)

# Make the bitstream target part of the default build and depend on the output file
add_custom_target(fpga_bitstream ALL DEPENDS ${FPGA_BITSTREAM_FILE})

# Add simulation tests subdirectory
add_subdirectory(${FPGA_PROJECT_DIR}/sim ${CMAKE_CURRENT_BINARY_DIR}/sim)

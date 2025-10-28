cmake_minimum_required(VERSION 3.13)

project(EconoPET_FPGA NONE)

set(FPGA_PROJECT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/EconoPET)
set(FPGA_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/outflow)
set(FPGA_WORK_DIR ${CMAKE_CURRENT_BINARY_DIR}/work)

message(VERBOSE "FPGA_PROJECT_DIR: ${FPGA_PROJECT_DIR}")

file(GLOB FPGA_SRC_FILES CONFIGURE_DEPENDS
    # Unfortunately, running the 'compile' flow modifies the EconoPET.xml file, which will
    # cause unnecessary rebuilds if we include it here.
    #"${FPGA_PROJECT_DIR}/EconoPET.xml"

    # Top-level project/config files
    "${FPGA_PROJECT_DIR}/EconoPET.peri.xml"
    "${FPGA_PROJECT_DIR}/*.sdc"

    # HDL sources
    "${FPGA_PROJECT_DIR}/src/*"
    "${FPGA_PROJECT_DIR}/src/**/*"
)

# Append external source files to FPGA_SRC_FILES, ignoring .git directories from submodules.
# First, we gather all files from the external directory:
file(GLOB FPGA_EXTERNAL_SRC_FILES
    "${FPGA_PROJECT_DIR}/external/*"
    "${FPGA_PROJECT_DIR}/external/**/*"
)

# Then append only those that are not in .git directories:
foreach(f ${FPGA_EXTERNAL_SRC_FILES})
    if(NOT f MATCHES "/\\.git(/|$)")
        list(APPEND FPGA_SRC_FILES "${f}")
    else()
        message(VERBOSE "Ignoring: ${f}")
    endif()
endforeach()

message(VERBOSE "FPGA source files: ${FPGA_SRC_FILES}")

# Define the output bitstream file
set(FPGA_BITSTREAM_FILE "${FPGA_OUTPUT_DIR}/EconoPET.hex.bin")

# Produce the bitstream file only when sources are newer.  This takes a minute or two to run,
# so we declare 'USES_TERMINAL' to give some feedback that the build is progressing.
add_custom_command(
    COMMENT "Compiling FPGA bitstream using Efinity tools (be patient)"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMAND /bin/bash -lc "git update-index -q --refresh && git diff --quiet -- EconoPET/EconoPET.xml && git diff --quiet --cached -- EconoPET/EconoPET.xml || (echo '*** ERROR: EconoPET.xml has local edits. Aborting build. ***' >&2; exit 1)"
    COMMAND "./efx.sh" --flow compile --output_dir "${FPGA_OUTPUT_DIR}" --work_dir "${FPGA_WORK_DIR}"
    COMMAND /bin/bash -lc "git restore --source=HEAD --worktree -- EconoPET/EconoPET.xml || true"
    OUTPUT ${FPGA_BITSTREAM_FILE}
    BYPRODUCTS "${FPGA_OUTPUT_DIR}" "${FPGA_WORK_DIR}"
    DEPENDS ${FPGA_SRC_FILES}
    USES_TERMINAL
    VERBATIM
)

# Make the bitstream target part of the default build and depend on the output file
add_custom_target(fpga_bitstream ALL DEPENDS ${FPGA_BITSTREAM_FILE})

# Add simulation tests subdirectory
add_subdirectory(${FPGA_PROJECT_DIR}/sim ${CMAKE_CURRENT_BINARY_DIR}/sim)

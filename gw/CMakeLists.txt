cmake_minimum_required(VERSION 3.13)

project(EconoPET_FPGA NONE)

set(FPGA_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/outflow)
set(FPGA_WORK_DIR ${CMAKE_CURRENT_BINARY_DIR}/work)

# Define the FPGA bitstream build target
add_custom_target(fpga_bitstream
    COMMENT "Compiling FPGA bitstream using Efinity tools"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMAND "./efx.sh" --flow compile --output_dir "${FPGA_OUTPUT_DIR}" --work_dir "${FPGA_WORK_DIR}"
    BYPRODUCTS "${FPGA_OUTPUT_DIR}" "${FPGA_WORK_DIR}"
    VERBATIM
)

# Make the bitstream target part of the default build
add_custom_target(fpga ALL DEPENDS fpga_bitstream)

# Ensure FPGA directories are cleaned with standard CMake clean
set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES
    "${FPGA_OUTPUT_DIR}"
    "${FPGA_WORK_DIR}"
)

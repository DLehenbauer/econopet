cmake_minimum_required(VERSION 3.13)

project(EconoPET_FPGA NONE)

# Define the FPGA bitstream build target
add_custom_target(fpga_bitstream
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/efx.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building FPGA bitstream using Efinity tools"
    VERBATIM
)

# Make the bitstream target part of the default build
add_custom_target(fpga ALL DEPENDS fpga_bitstream)

# Add a clean target for FPGA build artifacts
add_custom_target(fpga_clean
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/efx.sh --clean
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Cleaning FPGA build artifacts"
    VERBATIM
)

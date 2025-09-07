# PET Clone - Open hardware implementation of the Commodore PET
# by Daniel Lehenbauer and contributors.
# 
# https://github.com/DLehenbauer/commodore-pet-clone
#
# To the extent possible under law, I, Daniel Lehenbauer, have waived all
# copyright and related or neighboring rights to this project. This work is
# published from the United States.
#
# @copyright CC0 http://creativecommons.org/publicdomain/zero/1.0/
# @author Daniel Lehenbauer <DLehenbauer@users.noreply.github.com> and contributors

set(CMAKE_VERBOSE_MAKEFILE ON)
cmake_minimum_required(VERSION 3.13.1)

project(sdcard)

# Output directories on the SD image
set(sdcard_root_dir ${CMAKE_CURRENT_BINARY_DIR}/sdcard_root)
set(sdcard_rom_dir ${sdcard_root_dir}/roms)

# Determine ROM source directory from environment (required)
if(NOT DEFINED ENV{ECONOPET_ROMS_DIR} OR "$ENV{ECONOPET_ROMS_DIR}" STREQUAL "")
    message(FATAL_ERROR "ECONOPET_ROMS_DIR environment variable is not set. Set it to the directory containing ROM files.")
endif()
set(roms_source_dir "$ENV{ECONOPET_ROMS_DIR}")

message(STATUS "Using ROMs from: ${roms_source_dir}")

# Ensure source directory exists
if(NOT IS_DIRECTORY "${roms_source_dir}")
    message(FATAL_ERROR "ROM source directory not found: ${roms_source_dir}. Check the ECONOPET_ROMS_DIR environment variable is set to a valid directory.")
endif()

# Create destination directories

# Copy static files from the source 'sdcard' directory to the build output
# during configuration.
file(MAKE_DIRECTORY "${sdcard_rom_dir}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/config.yaml" DESTINATION "${sdcard_root_dir}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/bootloader.uf2" DESTINATION "${sdcard_root_dir}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/ukm" DESTINATION "${sdcard_root_dir}")
file(COPY "${roms_source_dir}/" DESTINATION "${sdcard_rom_dir}")

# Copy firmware UF2 produced by fw/firmware.elf into the SD card root at build time
# Ensure we depend on the actual firmware.elf target so the UF2 exists before copy
set(FW_TARGET firmware.elf)
add_custom_command(
    OUTPUT "${sdcard_root_dir}/firmware.uf2"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${sdcard_root_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE_DIR:${FW_TARGET}>/${FW_TARGET}.uf2"
            "${sdcard_root_dir}/firmware.uf2"
    DEPENDS ${FW_TARGET}
    COMMENT "Copying ${FW_TARGET}.uf2 to SD card root"
    VERBATIM
)

# Copy FPGA bitstream produced by gw target into sdcard_root/fpga
set(GW_BITSTREAM "${CMAKE_BINARY_DIR}/gw/outflow/EconoPET.hex.bin")
add_custom_command(
    OUTPUT "${sdcard_root_dir}/fpga/EconoPET.hex.bin"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${sdcard_root_dir}/fpga"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${GW_BITSTREAM}"
            "${sdcard_root_dir}/fpga/EconoPET.hex.bin"
    DEPENDS fpga_bitstream "${GW_BITSTREAM}"
    COMMENT "Copying EconoPET.hex.bin to SD card fpga directory"
    VERBATIM
)

# Define 'sdcard' target that performs the copy; included in default 'all' build
add_custom_target(sdcard ALL
    DEPENDS "${sdcard_root_dir}/firmware.uf2"
            "${sdcard_root_dir}/fpga/EconoPET.hex.bin"
)

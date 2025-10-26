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
if(NOT IS_DIRECTORY "${roms_source_dir}")
    message(FATAL_ERROR "ROM source directory not found: ${roms_source_dir}. Check the ECONOPET_ROMS_DIR environment variable is set to a valid directory.")
endif()

# Copy static files from the source 'sdcard' directory to the build output
# during configuration.
file(MAKE_DIRECTORY "${sdcard_rom_dir}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/config.yaml" DESTINATION "${sdcard_root_dir}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/bootloader.uf2" DESTINATION "${sdcard_root_dir}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/ukm" DESTINATION "${sdcard_root_dir}")
file(COPY "${roms_source_dir}/" DESTINATION "${sdcard_rom_dir}")

# Get firmware and FPGA paths from CMake variables (set by super-project)
# These should be passed in as -DFIRMWARE_UF2=... and -DFPGA_BITSTREAM=...
if(NOT DEFINED FIRMWARE_UF2)
    message(FATAL_ERROR "FIRMWARE_UF2 must be defined (path to firmware.elf.uf2)")
endif()

if(NOT DEFINED FPGA_BITSTREAM)
    message(FATAL_ERROR "FPGA_BITSTREAM must be defined (path to EconoPET.hex.bin)")
endif()

message(STATUS "Firmware UF2: ${FIRMWARE_UF2}")
message(STATUS "FPGA Bitstream: ${FPGA_BITSTREAM}")

# Copy firmware UF2 into the SD card root at build time
add_custom_command(
    OUTPUT "${sdcard_root_dir}/firmware.uf2"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${sdcard_root_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FIRMWARE_UF2}"
            "${sdcard_root_dir}/firmware.uf2"
    DEPENDS "${FIRMWARE_UF2}"
    COMMENT "Copying firmware.uf2 to SD card root"
    VERBATIM
)

# Copy FPGA bitstream into sdcard_root/fpga
add_custom_command(
    OUTPUT "${sdcard_root_dir}/fpga/EconoPET.hex.bin"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${sdcard_root_dir}/fpga"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FPGA_BITSTREAM}"
            "${sdcard_root_dir}/fpga/EconoPET.hex.bin"
    DEPENDS "${FPGA_BITSTREAM}"
    COMMENT "Copying EconoPET.hex.bin to SD card fpga directory"
    VERBATIM
)

# Create a zip archive of the SD card contents
string(TIMESTAMP ECONOPET_DATE "%y%m%d")
set(sdcard_zip_name "EconoPET-40-8096-A-firmware-${ECONOPET_DATE}.0.zip")
set(sdcard_zip_path "${CMAKE_CURRENT_BINARY_DIR}/${sdcard_zip_name}")

add_custom_command(
    OUTPUT "${sdcard_zip_path}"
    # Zip only the contents of ${sdcard_root_dir}
    COMMAND ${CMAKE_COMMAND} -E tar cf "${sdcard_zip_path}" --format=zip .
    WORKING_DIRECTORY "${sdcard_root_dir}"
    DEPENDS "${sdcard_root_dir}/firmware.uf2"
            "${sdcard_root_dir}/fpga/EconoPET.hex.bin"
    COMMENT "Zipping SD card contents to ${sdcard_zip_path}"
    VERBATIM
)

# Define 'sdcard' target that performs the copy; included in default 'all' build
add_custom_target(sdcard ALL
    DEPENDS "${sdcard_zip_path}"
)

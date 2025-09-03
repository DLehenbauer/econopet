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
file(MAKE_DIRECTORY "${sdcard_rom_dir}")

# Copy all ROM files verbatim (no renaming, no checksum verification)
file(COPY "${roms_source_dir}/" DESTINATION "${sdcard_rom_dir}")

# Copy config.yaml to the sdcard_root directory
file(COPY "${CMAKE_CURRENT_LIST_DIR}/config.yaml" DESTINATION "${sdcard_root_dir}")

# Copy ukm to the sdcard_root directory
file(COPY "${CMAKE_CURRENT_LIST_DIR}/ukm" DESTINATION "${sdcard_root_dir}")

# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

set(CMAKE_VERBOSE_MAKEFILE ON)
cmake_minimum_required(VERSION 3.13.1)

project(sdcard)

# ECONOPET_MEDIA_DIR is required and contains roms/ and disks/.
if(NOT DEFINED ECONOPET_MEDIA_DIR OR "${ECONOPET_MEDIA_DIR}" STREQUAL "")
    if(DEFINED ENV{ECONOPET_MEDIA_DIR} AND NOT "$ENV{ECONOPET_MEDIA_DIR}" STREQUAL "")
        set(ECONOPET_MEDIA_DIR "$ENV{ECONOPET_MEDIA_DIR}")
    endif()
endif()

if(NOT DEFINED ECONOPET_MEDIA_DIR OR "${ECONOPET_MEDIA_DIR}" STREQUAL "")
    message(FATAL_ERROR "ECONOPET_MEDIA_DIR must be set to the directory containing roms and disks.")
endif()

foreach(required_var FIRMWARE_UF2 FPGA_BITSTREAM BOOTLOADER_UF2)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} must be defined.")
    endif()
endforeach()

# Rebuild staging on every package invocation, including additions and removals.
get_filename_component(build_root "${CMAKE_CURRENT_BINARY_DIR}" DIRECTORY)
add_custom_target(sdcard ALL
    COMMAND ${CMAKE_COMMAND}
            "-DSDCARD_ROOT_DIR=${CMAKE_CURRENT_BINARY_DIR}/sdcard_root"
            "-DECONOPET_MEDIA_DIR=${ECONOPET_MEDIA_DIR}"
            "-DROM_BIN_DIR=${build_root}/rom/bin"
            "-DFIRMWARE_UF2=${FIRMWARE_UF2}"
            "-DFPGA_BITSTREAM=${FPGA_BITSTREAM}"
            "-DBOOTLOADER_UF2=${BOOTLOADER_UF2}"
            -P "${CMAKE_CURRENT_LIST_DIR}/Stage.cmake"
    COMMAND ${CMAKE_COMMAND}
            "-DSDCARD_ROOT_DIR=${CMAKE_CURRENT_BINARY_DIR}/sdcard_root"
            "-DSDCARD_ZIP_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            -P "${CMAKE_CURRENT_LIST_DIR}/Package.cmake"
    DEPENDS "${FIRMWARE_UF2}" "${FPGA_BITSTREAM}" "${BOOTLOADER_UF2}"
    COMMENT "Staging and packaging SD card contents"
    VERBATIM
)

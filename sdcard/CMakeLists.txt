# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

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
file(COPY "${CMAKE_CURRENT_LIST_DIR}/README.md" DESTINATION "${sdcard_root_dir}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/../NOTICE.md" DESTINATION "${sdcard_root_dir}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/ukm" DESTINATION "${sdcard_root_dir}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/prgs/README.md" DESTINATION "${sdcard_root_dir}/prgs")
file(COPY "${roms_source_dir}/" DESTINATION "${sdcard_rom_dir}")

# Optional IEEE-488 drive images: sdcard/disks/drive0.d80|d64, drive1.d80|d64
# (gitignored, like roms). Copied into the package when present.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/disks")
    file(COPY "${CMAKE_CURRENT_LIST_DIR}/disks" DESTINATION "${sdcard_root_dir}")
endif()

# Copy any ROM .bin files produced by the 'rom' target into sdcard/roms
# (exclude the special firmware-embedded 'menu.rom'). Print status for each.
# The sdcard project builds under ${CMAKE_CURRENT_BINARY_DIR} which is
# typically ${TOP}/build/sdcard. The ROM binaries live under ${TOP}/build/rom/bin.
get_filename_component(build_root "${CMAKE_CURRENT_BINARY_DIR}" DIRECTORY)
set(rom_bin_dir "${build_root}/rom/bin")
file(GLOB rom_bin_files "${rom_bin_dir}/*.bin")
message(STATUS "Scanning ROM bin directory: ${rom_bin_dir}")
message(STATUS "Found ROM bins: ${rom_bin_files}")

set(sdcard_rom_outputs)
foreach(bin_file ${rom_bin_files})
    get_filename_component(bin_name "${bin_file}" NAME)
    add_custom_command(
        OUTPUT "${sdcard_rom_dir}/${bin_name}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${sdcard_rom_dir}"
        COMMAND ${CMAKE_COMMAND} -E echo "Including ROM: ${bin_name}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${bin_file}"
                "${sdcard_rom_dir}/${bin_name}"
        DEPENDS "${bin_file}"
        COMMENT "Copying ${bin_name} to SD card roms"
        VERBATIM
    )
    list(APPEND sdcard_rom_outputs "${sdcard_rom_dir}/${bin_name}")
endforeach()

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
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/GitVersion.cmake")

string(TIMESTAMP ECONOPET_DATE "%y%m%d" UTC)
string(TIMESTAMP sdcard_build_time "%Y-%m-%dT%H:%M:%SZ" UTC)
set(sdcard_version "${ECONOPET_DATE}")

if(GIT_DIRTY)
    set(sdcard_dirty "true")
else()
    set(sdcard_dirty "false")
endif()

# Record what went into the package, so that a downloaded zip can be traced back
# to the commit it was built from.
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/build-info.yaml.in"
    "${sdcard_root_dir}/build-info.yaml"
    @ONLY
)

# The commit is part of the name so that packages built from different commits
# on the same day remain distinguishable.
set(sdcard_zip_name "EconoPET-40-8096-A-firmware-${sdcard_version}-${GIT_HASH}.zip")
set(sdcard_zip_path "${CMAKE_CURRENT_BINARY_DIR}/${sdcard_zip_name}")

# Because the name above changes with the commit and the date, reconfiguring
# would otherwise leave the previous package sitting beside the new one.
file(GLOB sdcard_stale_zips "${CMAKE_CURRENT_BINARY_DIR}/EconoPET-*.zip")
list(REMOVE_ITEM sdcard_stale_zips "${sdcard_zip_path}")
if(sdcard_stale_zips)
    message(STATUS "Removing superseded package(s): ${sdcard_stale_zips}")
    file(REMOVE ${sdcard_stale_zips})
endif()

add_custom_command(
    OUTPUT "${sdcard_zip_path}"
    # Zip only the contents of ${sdcard_root_dir}
    COMMAND ${CMAKE_COMMAND} -E tar cf "${sdcard_zip_path}" --format=zip .
    WORKING_DIRECTORY "${sdcard_root_dir}"
    DEPENDS "${sdcard_root_dir}/firmware.uf2"
            "${sdcard_root_dir}/fpga/EconoPET.hex.bin"
            "${sdcard_root_dir}/build-info.yaml"
            ${sdcard_rom_outputs}
    COMMENT "Zipping SD card contents to ${sdcard_zip_path}"
    VERBATIM
)

# Define 'sdcard' target that performs the copy; included in default 'all' build
add_custom_target(sdcard ALL
    DEPENDS "${sdcard_zip_path}"
)

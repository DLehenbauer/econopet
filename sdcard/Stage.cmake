# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

# Refresh package inputs at build time, rather than only on child configuration.
foreach(required_var SDCARD_ROOT_DIR ECONOPET_MEDIA_DIR ROM_BIN_DIR FIRMWARE_UF2 FPGA_BITSTREAM BOOTLOADER_UF2 DRIVE_TEST_PRG)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} must be defined.")
    endif()
endforeach()

# Sanity-check *.prgs files shipping on the SD card.
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/ValidatePrg.cmake")

# PRGs started with RUN must load at the PET's BASIC start address ($0401).
# Diagnostic programs must reside below the mailbox address at $7FF0.
validate_prg("${DRIVE_TEST_PRG}" 0x0401 0x7FF0 0x0401)

# Reject FAT-incompatible ROM filename collisions before replacing the previous package root.
file(GLOB rom_bin_files "${ROM_BIN_DIR}/*.bin")
file(GLOB media_rom_files "${ECONOPET_MEDIA_DIR}/rom/*")
set(rom_files ${rom_bin_files} ${media_rom_files})
foreach(rom_file IN LISTS rom_files)
    get_filename_component(rom_name "${rom_file}" NAME)
    string(TOLOWER "${rom_name}" rom_name_lower)
    foreach(other_rom_file IN LISTS rom_files)
        if(NOT "${rom_file}" STREQUAL "${other_rom_file}")
            get_filename_component(other_rom_name "${other_rom_file}" NAME)
            string(TOLOWER "${other_rom_name}" other_rom_name_lower)
            if("${rom_name_lower}" STREQUAL "${other_rom_name_lower}")
                message(FATAL_ERROR "ROM filename conflict: ${rom_file} and ${other_rom_file}")
            endif()
        endif()
    endforeach()
endforeach()

# Discard removed or renamed inputs as well as obsolete generated ROMs.
file(REMOVE_RECURSE "${SDCARD_ROOT_DIR}")
file(MAKE_DIRECTORY "${SDCARD_ROOT_DIR}/rom")
file(COPY
    "${CMAKE_CURRENT_LIST_DIR}/config.yaml"
    "${CMAKE_CURRENT_LIST_DIR}/README.md"
    "${CMAKE_CURRENT_LIST_DIR}/../NOTICE.md"
    "${ECONOPET_MEDIA_DIR}/rom"
    "${ECONOPET_MEDIA_DIR}/disk"
    DESTINATION "${SDCARD_ROOT_DIR}")
configure_file("${BOOTLOADER_UF2}" "${SDCARD_ROOT_DIR}/bootloader.uf2" COPYONLY)
file(COPY "${CMAKE_CURRENT_LIST_DIR}/prg/README.md"
    DESTINATION "${SDCARD_ROOT_DIR}/prg")
file(MAKE_DIRECTORY "${SDCARD_ROOT_DIR}/disk")
file(MAKE_DIRECTORY "${SDCARD_ROOT_DIR}/test")
file(COPY
    "${CMAKE_CURRENT_LIST_DIR}/test/README.md"
    DESTINATION "${SDCARD_ROOT_DIR}/test")
configure_file("${DRIVE_TEST_PRG}" "${SDCARD_ROOT_DIR}/test/drive-test.prg" COPYONLY)

# Add generated ROMs to the package. The embedded menu.rom is not an SD asset.
foreach(bin_file IN LISTS rom_bin_files)
    get_filename_component(bin_name "${bin_file}" NAME)
    configure_file("${bin_file}" "${SDCARD_ROOT_DIR}/rom/${bin_name}" COPYONLY)
endforeach()

# Keep intermediate images beside the package root, outside the SD-card contents.
get_filename_component(stage_dir "${SDCARD_ROOT_DIR}" DIRECTORY)
set(FPGA_IMAGE "${stage_dir}/fpga.flash.bin")

# Add our custom bitstream header and obtain the shared partition's XIP address.
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PrepareFpgaImage.cmake")

# Require picotool for UF2 conversion and combining.
find_program(PICOTOOL_EXECUTABLE NAMES picotool)
if(NOT PICOTOOL_EXECUTABLE)
    message(FATAL_ERROR "picotool is required to package firmware and FPGA images")
endif()

# Address the raw image explicitly and mark its UF2 blocks for the RP2040 family.
execute_process(
    COMMAND "${PICOTOOL_EXECUTABLE}" uf2 convert "${FPGA_IMAGE}" "${stage_dir}/fpga.uf2"
        --offset "${ECONOPET_FPGA_ADDRESS}" --family 0xe48bff56 --platform rp2040
    RESULT_VARIABLE convert_result)

# Stop before combining if the FPGA UF2 could not be generated.
if(NOT convert_result EQUAL 0)
    message(FATAL_ERROR "Failed to convert FPGA image to UF2")
endif()

# Package one firmware-and-FPGA update without modifying the input firmware UF2.
# The bootloader stays separate because its reserved addresses are not SD-updatable.
execute_process(
    COMMAND "${PICOTOOL_EXECUTABLE}" uf2 combine "${FIRMWARE_UF2}" "${stage_dir}/fpga.uf2"
        "${SDCARD_ROOT_DIR}/firmware.uf2"
    RESULT_VARIABLE combine_result)

# Fail staging rather than publish an unsuccessful combined update.
if(NOT combine_result EQUAL 0)
    message(FATAL_ERROR "Failed to combine firmware and FPGA UF2 files")
endif()

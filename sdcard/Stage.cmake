# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

# Refresh package inputs at build time, rather than only on child configuration.
foreach(required_var SDCARD_ROOT_DIR ECONOPET_MEDIA_DIR ROM_BIN_DIR FIRMWARE_UF2 FPGA_BITSTREAM)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} must be defined.")
    endif()
endforeach()

# Discard removed or renamed inputs as well as obsolete generated ROMs.
file(REMOVE_RECURSE "${SDCARD_ROOT_DIR}")
file(MAKE_DIRECTORY "${SDCARD_ROOT_DIR}/roms")
file(COPY
    "${CMAKE_CURRENT_LIST_DIR}/config.yaml"
    "${CMAKE_CURRENT_LIST_DIR}/bootloader.uf2"
    "${CMAKE_CURRENT_LIST_DIR}/README.md"
    "${CMAKE_CURRENT_LIST_DIR}/../NOTICE.md"
    "${CMAKE_CURRENT_LIST_DIR}/ukm"
    "${ECONOPET_MEDIA_DIR}/roms"
    "${ECONOPET_MEDIA_DIR}/disks"
    DESTINATION "${SDCARD_ROOT_DIR}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/prgs/README.md"
    DESTINATION "${SDCARD_ROOT_DIR}/prgs")

# Generated ROMs override media copies. The embedded menu.rom is not an SD asset.
file(GLOB rom_bin_files "${ROM_BIN_DIR}/*.bin")
foreach(bin_file IN LISTS rom_bin_files)
    get_filename_component(bin_name "${bin_file}" NAME)
    configure_file("${bin_file}" "${SDCARD_ROOT_DIR}/roms/${bin_name}" COPYONLY)
endforeach()

# Preserve the public firmware and FPGA filenames independently of input names.
configure_file("${FIRMWARE_UF2}" "${SDCARD_ROOT_DIR}/firmware.uf2" COPYONLY)
configure_file("${FPGA_BITSTREAM}" "${SDCARD_ROOT_DIR}/fpga/EconoPET.hex.bin" COPYONLY)

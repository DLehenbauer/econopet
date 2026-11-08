# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

# Prepare an FPGA .hex.bin bitstream for storage in the RP2040's external flash.
# The RP2040 normally reads this image on boot and configures the FPGA over SPI.
# The combined 'firmware.uf2' includes the image alongside the RP2040 firmware.
#
# Required caller-supplied paths:
#   FPGA_BITSTREAM: raw input bitstream.
#   FPGA_IMAGE: output binary containing the image header and bitstream.
# The FPGA_IMAGE parent directory must exist. Preparation errors stop the caller.

# Use the same partition capacity and address as firmware and bootloader builds.
include("${CMAKE_CURRENT_LIST_DIR}/FlashLayout.cmake")

# Require all path variables before invoking the image writer.
foreach(required FPGA_BITSTREAM FPGA_IMAGE)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} must be defined")
    endif()
endforeach()

# Locate Python for the shared image-format implementation.
find_program(PYTHON_EXECUTABLE NAMES python3)
if(NOT PYTHON_EXECUTABLE)
    message(FATAL_ERROR "python3 is required to prepare the FPGA flash image")
endif()

# Prepend the little-endian magic, image-format version, payload length and CRC-32 header.
# The writer rejects empty bitstreams and images larger than the FPGA reservation.
execute_process(
    COMMAND "${PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../tools/fpga_image.py"
        "${FPGA_BITSTREAM}" "${FPGA_IMAGE}" --capacity "${ECONOPET_FPGA_SIZE_BYTES}"
    RESULT_VARIABLE image_result)

# Abort on writer failure rather than deploy an unsuccessful image.
if(NOT image_result EQUAL 0)
    message(FATAL_ERROR "Failed to prepare FPGA flash image")
endif()

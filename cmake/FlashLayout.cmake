# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

# Single source of truth for the build-time flash layout, shared by firmware,
# bootloader configuration, SWD programming and SD-card packaging. Change the FPGA
# reservation here, then rebuild and deploy matching firmware and FPGA images.

# XIP-mapped base address of physical flash.
set(ECONOPET_FLASH_BASE 0x10000000)

# Physical capacity in bytes (16 MiB), distinct from the firmware linker limit.
set(ECONOPET_FLASH_SIZE_BYTES 16777216)

# Stage-3 bootloader reservation at the end of flash, in bytes (64 KiB).
set(ECONOPET_BOOTLOADER_SIZE_BYTES 65536)

# FPGA image header and bitstream reservation, in bytes (720 KiB).
set(ECONOPET_FPGA_SIZE_BYTES 737280)

# Smallest flash erase unit in bytes (4 KiB), used to align partition boundaries.
set(ECONOPET_FLASH_SECTOR_SIZE 4096)

# Place the FPGA partition immediately below stage 3. This byte offset from the
# flash base also limits the firmware linker region, keeping both reservations free.
math(EXPR ECONOPET_FPGA_OFFSET
    "${ECONOPET_FLASH_SIZE_BYTES} - ${ECONOPET_BOOTLOADER_SIZE_BYTES} - ${ECONOPET_FPGA_SIZE_BYTES}")

# Convert the offset to an absolute XIP address for firmware reads and deployment.
math(EXPR ECONOPET_FPGA_ADDRESS "${ECONOPET_FLASH_BASE} + ${ECONOPET_FPGA_OFFSET}" OUTPUT_FORMAT HEXADECIMAL)

# Require positive sizes and a positive firmware limit, all aligned to erase
# sectors. Reject invalid layouts before consumers build or package their images.
foreach(size ECONOPET_FLASH_SIZE_BYTES ECONOPET_BOOTLOADER_SIZE_BYTES ECONOPET_FPGA_SIZE_BYTES ECONOPET_FPGA_OFFSET)
    # Each entry names a variable, so nested expansion retrieves its byte value.
    math(EXPR remainder "${${size}} % ${ECONOPET_FLASH_SECTOR_SIZE}")

    # A nonzero remainder means a partition boundary would split an erase sector.
    if(${size} LESS_EQUAL 0 OR NOT remainder EQUAL 0)
        message(FATAL_ERROR "${size} must be positive and flash-sector aligned")
    endif()
endforeach()

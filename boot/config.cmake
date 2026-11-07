# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

# Retain the upstream defaults except for EconoPET's flash and SD wiring.
include("${CMAKE_CURRENT_LIST_DIR}/../fw/external/pico-sdcard-bootloader/config.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/FlashLayout.cmake")
set(PICO_FLASH_SIZE_BYTES "${ECONOPET_FLASH_SIZE_BYTES}")
set(BOOTLOADER_SIZE "${ECONOPET_BOOTLOADER_SIZE_BYTES}")

# Match the SD SPI1 wiring in fw/src/hw.h (card presence is detected through SPI).
set(BOOTLOADER_SD_SPI 1)
set(BOOTLOADER_SD_SPI_SCK_PIN 14)
set(BOOTLOADER_SD_SPI_TX_PIN 11)
set(BOOTLOADER_SD_SPI_RX_PIN 12)
set(BOOTLOADER_SD_SPI_CSN_PIN 9)
set(BOOTLOADER_SD_DETECT_PIN 8)
set(BOOTLOADER_SD_USE_DETECT false)

# Keep boot diagnostics on the Pico-compatible UART0 pins and activity LED.
set(BOOTLOADER_USE_UART true)

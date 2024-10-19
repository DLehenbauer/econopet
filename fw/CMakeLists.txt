cmake_minimum_required(VERSION 3.13)

project("firmware")
set(FAMILY rp2040)
set(BOARD pico_sdk)

add_subdirectory("external/PicoDVI/software/libdvi")
add_subdirectory("external/no-OS-FatFS-SD-SPI-RPi-Pico/FatFs_SPI")

add_executable(${PROJECT_NAME}
    driver.c
    main.c
    menu/menu.c
    pet.c
    sd/sd.c
    term.c
    test/mem.c
    usb/cdc_app.c
    usb/hid_app.c
    usb/keyboard.c
    usb/msc_app.c
    usb/usb.c
    video/video.c
)

# EconoPET 40/8096 Rev. A: xosc requires a little extra time to stabilize.
# On tested sample (45240-60), 8ms was intermittent and 16ms was reliable.
# 64ms provides a large tolerance.
target_compile_definitions(${PROJECT_NAME} PUBLIC PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

# Not required for RP2040B2 silicon and conflicts with use of GPIO15 (FPGA_CLK)
# (See https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf#errata-e5)
# target_compile_definitions(${PROJECT_NAME} PRIVATE PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1)

# Expose tusb_config.h to TinyUSB
target_include_directories(${PROJECT_NAME} PUBLIC
        ${CMAKE_CURRENT_LIST_DIR})

target_precompile_headers(${PROJECT_NAME} PRIVATE pch.h)

# Configure PicoDVI library for monochrome
target_compile_definitions(${PROJECT_NAME} PRIVATE
    DVI_VERTICAL_REPEAT=2
    DVI_N_TMDS_BUFFERS=3
    DVI_MONOCHROME_TMDS=1)

# pull in common dependencies
target_link_libraries(${PROJECT_NAME} PUBLIC
    hardware_dma
    hardware_pio
    hardware_pwm
    hardware_spi
    libdvi
    pico_multicore
    pico_stdlib
    pico_util
    FatFs_SPI
    tinyusb_host
    tinyusb_board
)

# We have a lot in SRAM4 (particularly TMDS LUT) but don't need much stack on
# core 1. Probably even 256 bytes would be fine.
target_compile_definitions(${PROJECT_NAME} PRIVATE PICO_CORE1_STACK_SIZE=0x200)

# Include external libraries
include_directories(
    "external/PicoDVI/software/assets"
    "external/PicoDVI/software/include"
    "external/no-OS-FatFS-SD-SPI-RPi-Pico/FatFS_SPI/include")

# create map/bin/hex/uf2 file etc.
pico_add_extra_outputs(${PROJECT_NAME})

cmake_minimum_required(VERSION 3.13)

set(FAMILY rp2040)
set(BOARD pico_sdk)

add_subdirectory("external/PicoDVI/software/libdvi")
add_subdirectory("external/no-OS-FatFS-SD-SPI-RPi-Pico/FatFs_SPI")

add_executable(firmware
    driver.c
    main.c
    sd/sd.c
    test/mem.c
    video/video.c
)

target_precompile_headers(firmware PRIVATE pch.h)

# Configure PicoDVI library for monochrome
target_compile_definitions(firmware PRIVATE
	DVI_VERTICAL_REPEAT=2
	DVI_N_TMDS_BUFFERS=3
	DVI_MONOCHROME_TMDS=1)

# pull in common dependencies
target_link_libraries(
    firmware
    hardware_dma
    hardware_pio
    hardware_pwm
    hardware_spi
    libdvi
    pico_multicore
    pico_stdlib
    pico_util
    FatFs_SPI
)

# We have a lot in SRAM4 (particularly TMDS LUT) but don't need much stack on
# core 1. Probably even 256 bytes would be fine.
target_compile_definitions(firmware PRIVATE PICO_CORE1_STACK_SIZE=0x200)

# Include external libraries
include_directories(
    "external/PicoDVI/software/assets"
    "external/PicoDVI/software/include"
    "external/no-OS-FatFS-SD-SPI-RPi-Pico/FatFS_SPI/include")

# create map/bin/hex/uf2 file etc.
pico_add_extra_outputs(firmware)

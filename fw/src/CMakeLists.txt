cmake_minimum_required(VERSION 3.13)

project("firmware" C CXX ASM)
add_compile_options(-Wall -Wextra -Wnull-dereference)

# Include ROM header generation function
include(${FW_DIR}/cmake/AddRomHeader.cmake)

# Include external projects
set(FW_EXTERNAL_DIR ${FW_DIR}/external)

add_subdirectory(
    ${FW_EXTERNAL_DIR}/PicoDVI/software/libdvi
    ${CMAKE_BINARY_DIR}/libdvi)

add_subdirectory(
    ${FW_EXTERNAL_DIR}/pico-vfs
    ${CMAKE_BINARY_DIR}/pico-vfs)

# Temporarily disable BUILD_TESTING for libyaml
set(BUILD_TESTING OFF CACHE BOOL "Disable testing for libyaml" FORCE)
add_subdirectory(
    ${FW_EXTERNAL_DIR}/libyaml
    ${CMAKE_BINARY_DIR}/libyaml)
# Restore BUILD_TESTING to its original value
set(BUILD_TESTING ON CACHE BOOL "Enable testing for the rest of the project" FORCE)

# Include source files
set(FW_SRC_DIR ${FW_DIR}/src)

# 'picotool' requires that the executable name ends with '.elf' extension
set(FW_EXECUTABLE_NAME "${PROJECT_NAME}.elf")

add_executable(${FW_EXECUTABLE_NAME}
    ${FW_SRC_DIR}/display/char_encoding.c
    ${FW_SRC_DIR}/driver.c
    ${FW_SRC_DIR}/fatal.c
    ${FW_SRC_DIR}/global.c
    ${FW_SRC_DIR}/main.c
    ${FW_SRC_DIR}/menu/menu.c
    ${FW_SRC_DIR}/menu/menu_config.c
    ${FW_SRC_DIR}/display/window.c
    ${FW_SRC_DIR}/system_state.c
    ${FW_SRC_DIR}/pet.c
    ${FW_SRC_DIR}/roms/roms.c
    ${FW_SRC_DIR}/roms/checksum.c
    ${FW_SRC_DIR}/sd/sd.c
    ${FW_SRC_DIR}/term.c
    ${FW_SRC_DIR}/config/config.c
    ${FW_SRC_DIR}/diag/mem.c
    ${FW_SRC_DIR}/diag/log/log.c
    ${FW_SRC_DIR}/usb/cdc_app.c
    ${FW_SRC_DIR}/usb/hid_app.c
    ${FW_SRC_DIR}/usb/keyboard.c
    ${FW_SRC_DIR}/usb/keyscan.c
    ${FW_SRC_DIR}/usb/keystate.c
    ${FW_SRC_DIR}/usb/msc_app.c
    ${FW_SRC_DIR}/usb/usb.c
    ${FW_SRC_DIR}/display/dvi/dvi.c
    ${FW_SRC_DIR}/display/dvi/tmds_encode.c
    ${FW_SRC_DIR}/display/dvi/tmds_encode.S
)

# Extend crystal oscillator startup time to improve cold power-on reliability. The CBM/PET
# powers mainboard and monitor from the same transformer, potentially causing transient
# voltage dips when the CRT and DC filter cap are fully discharged.
#
# We increase the oscillator startup delay to the maximum (174) to help 40/8096 Rev A.
# boards tolerate this. (See https://github.com/DLehenbauer/econopet/issues/3)
target_compile_definitions(${FW_EXECUTABLE_NAME} PUBLIC PICO_XOSC_STARTUP_DELAY_MULTIPLIER=1)

# The W25Q128JV has 16 MiB of storage (65,536 programmable pages of 256 bytes each).
target_compile_definitions(${FW_EXECUTABLE_NAME} PUBLIC PICO_FLASH_SIZE_BYTES=16777216)

# Not required for RP2040B2 silicon and conflicts with use of GPIO15 (FPGA_CLK)
# (See https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf#errata-e5)
#target_compile_definitions(${FW_EXECUTABLE_NAME} PRIVATE PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1)

# Expose tusb_config.h to TinyUSB
target_include_directories(${FW_EXECUTABLE_NAME} PUBLIC ${FW_SRC_DIR})

target_precompile_headers(${FW_EXECUTABLE_NAME} PRIVATE ${FW_SRC_DIR}/pch.h)

# Configure PicoDVI library.  Setting DVI_VERTICAL_REPEAT=2 halves the vertical resolution to
# 240p by displaying each scanline twice, which buys us more time to prepare each frame.
#
# We increase DVI_N_TMDS_BUFFERS from the default of 3 to 4.  One buffer is permanently reserved
# for a precalculated blank scanline (see 'blank_tmdsbuf' in dvi.c), used for non-visible regions
# and margins. This leaves 3 buffers in the free queue for active video scanlines.
target_compile_definitions(${FW_EXECUTABLE_NAME} PRIVATE
    DVI_VERTICAL_REPEAT=2
    DVI_N_TMDS_BUFFERS=4)

# pull in common dependencies
target_link_libraries(${FW_EXECUTABLE_NAME} PUBLIC
    hardware_dma
    hardware_pio
    hardware_pwm
    hardware_spi
    libdvi
    pico_multicore
    pico_stdlib
    pico_util
    tinyusb_host
    tinyusb_board

    # libyaml
    yaml

    # pico-vfs
    blockdevice_flash
    blockdevice_heap
    blockdevice_loopback
    blockdevice_sd
    filesystem_fat
    filesystem_littlefs
    filesystem_vfs
)

# We have a lot in SRAM4 (particularly TMDS LUT) but don't need much stack on
# core 1. Probably even 256 bytes would be fine.
target_compile_definitions(${FW_EXECUTABLE_NAME} PRIVATE PICO_CORE1_STACK_SIZE=0x200)

# Add include directories for external libraries
include_directories(
    "${FW_EXTERNAL_DIR}/libyaml/include"
    "${FW_EXTERNAL_DIR}/PicoDVI/software/include")

pico_enable_filesystem(${FW_EXECUTABLE_NAME})

# Generate ROM headers from binary files
# These are generated in the build directory and only updated if content changes
add_rom_header(${FW_EXECUTABLE_NAME}
    ${ECONOPET_ROMS_DIR}/characters-2.901447-10.bin
    ${CMAKE_CURRENT_BINARY_DIR}/roms/901447_10.h
)

add_rom_header(${FW_EXECUTABLE_NAME}
    ${ECONOPET_ROM_BIN_DIR}/menu.rom
    ${CMAKE_CURRENT_BINARY_DIR}/roms/menu_rom.h
)

# Add build directory to include path for generated ROM headers
target_include_directories(${FW_EXECUTABLE_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# create map/bin/hex/uf2 file etc.
pico_add_extra_outputs(${FW_EXECUTABLE_NAME})

project("firmware-test"
    VERSION 0.1.0
    LANGUAGES C)

# Load version generation and locate external dependencies for the host target.
include(${FW_DIR}/cmake/AddVersionHeader.cmake)

# Include external projects
set(EXTERNAL_DIR ${FW_DIR}/external)

# Temporarily disable BUILD_TESTING for libyaml
set(BUILD_TESTING OFF CACHE BOOL "Disable testing for libyaml" FORCE)
add_subdirectory(
    ${EXTERNAL_DIR}/libyaml
    ${CMAKE_BINARY_DIR}/libyaml)
# Restore BUILD_TESTING to its original value
set(BUILD_TESTING ON CACHE BOOL "Enable testing for the rest of the project" FORCE)

FIND_PATH(CHECK_INCLUDE_DIR check.h)
FIND_LIBRARY(CHECK_LIBRARIES NAMES check)

# Combine firmware logic, firmware-specific mocks, and shared host support.
set(SRC_DIR ${FW_DIR}/src)
set(TEST_DIR ${FW_DIR}/test)
get_filename_component(TEST_SUPPORT_DIR "${FW_DIR}/../test/support" ABSOLUTE)
get_filename_component(PRGS_DIR "${FW_DIR}/../prgs" ABSOLUTE)

add_executable(${PROJECT_NAME}
    ${SRC_DIR}/config/config.c
    ${SRC_DIR}/cbm/filename.c
    ${SRC_DIR}/cbm/petscii.c
    ${SRC_DIR}/cpu.c
    ${SRC_DIR}/diag/log/log.c
    ${SRC_DIR}/diag/diag.c
    ${SRC_DIR}/diag/drive.c
    ${SRC_DIR}/diag/mem.c
    ${SRC_DIR}/diag/sd_confirm.c
    ${SRC_DIR}/diag/runner.c
    ${SRC_DIR}/diag/target.c
    ${SRC_DIR}/display/char_encoding.c
    ${SRC_DIR}/display/display.c
    ${SRC_DIR}/display/window.c
    ${SRC_DIR}/driver_sync.c
    ${SRC_DIR}/fatal.c
    ${SRC_DIR}/fpga_image.c
    ${SRC_DIR}/roms/roms.c
    ${SRC_DIR}/roms/checksum.c
    ${SRC_DIR}/sd/path.c
    ${SRC_DIR}/sd/activity.c
    ${SRC_DIR}/input/input_controls.c
    ${SRC_DIR}/input/input.c
    ${SRC_DIR}/input/input_actions.c
    ${SRC_DIR}/input/input_routing.c
    ${SRC_DIR}/input/input_uart.c
    ${SRC_DIR}/input/pet_keyboard.c
    ${SRC_DIR}/input/keyboard_core.c
    ${SRC_DIR}/input/term_inject.c
    ${SRC_DIR}/pet.c
    ${SRC_DIR}/global.c
    ${SRC_DIR}/menu/menu_config.c
    ${SRC_DIR}/menu/menu.c
    ${SRC_DIR}/menu/menu_browser.c
    ${SRC_DIR}/menu/menu_ui.c
    ${SRC_DIR}/system/state.c
    ${SRC_DIR}/system/system.c
    ${SRC_DIR}/ui/tui.c
    ${SRC_DIR}/ui/cli.c
    ${SRC_DIR}/ui/console.c
    ${SRC_DIR}/breakpoint.c
    ${SRC_DIR}/tape_dir.c
    ${SRC_DIR}/ieee/diskimage.c
    ${SRC_DIR}/ieee/diskimage_file.c
    ${SRC_DIR}/ieee/dos_media.c
    ${SRC_DIR}/ieee/dos_rel.c
    ${SRC_DIR}/ieee/dos.c
    ${SRC_DIR}/ieee/ieee_drive.c
    ${SRC_DIR}/usb/keyscan.c
    ${SRC_DIR}/usb/keyboard.c
    ${SRC_DIR}/usb/keymap.c
    ${SRC_DIR}/usb/keystate.c
    ${TEST_DIR}/breakpoint_test.c
    ${TEST_DIR}/char_encoding_test.c
    ${TEST_DIR}/config_parser_test.c
    ${TEST_DIR}/cpu_test.c
    ${TEST_DIR}/crtc_test.c
    ${TEST_DIR}/display_test.c
    ${TEST_DIR}/diag_test.c
    ${TEST_DIR}/diag_sd_stub.c
    ${TEST_DIR}/fatal_error_test.c
    ${TEST_DIR}/diskimage_test.c
    ${TEST_DIR}/dos_media_test.c
    ${TEST_DIR}/dos_rel_test.c
    ${TEST_DIR}/dos_test.c
    ${TEST_DIR}/global_test.c
    ${TEST_DIR}/fpga_image_test.c
    ${TEST_DIR}/ieee_drive_test.c
    ${TEST_DIR}/input_test.c
    ${TEST_DIR}/keyscan_test.c
    ${TEST_DIR}/keyboard_test.c
    ${TEST_DIR}/keystate_test.c
    ${TEST_DIR}/log_test.c
    ${TEST_DIR}/main.c
    ${TEST_DIR}/menu_browser_test.c
    ${TEST_DIR}/menu_ui_test.c
    ${TEST_DIR}/menu_session_test.c
    ${TEST_SUPPORT_DIR}/test_support.c
    ${TEST_SUPPORT_DIR}/test_files.c
    ${TEST_SUPPORT_DIR}/test_fatal.c
    ${TEST_SUPPORT_DIR}/d64_fixture.c
    ${TEST_DIR}/mock.c
    ${TEST_DIR}/petscii_test.c
    ${TEST_DIR}/roms_test.c
    ${TEST_DIR}/system_state_test.c
    ${TEST_DIR}/system_session_test.c
    ${TEST_DIR}/sd_activity_test.c
    ${TEST_DIR}/tape_dir_test.c
    ${TEST_DIR}/test_support_test.c
    ${TEST_DIR}/tui_test.c
    ${TEST_DIR}/window_test.c
)

# ROM boot-routing tests do not execute CPU instructions or inspect glyph content.
foreach(header 901447_10 menu_rom)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/roms/${header}.h"
        CONTENT "// SPDX-License-Identifier: CC0-1.0\n0x00,\n")
endforeach()

include(${FW_DIR}/cmake/AddRomHeader.cmake)
add_rom_header(${PROJECT_NAME}
    ${SRC_DIR}/usb/us.ukm
    ${CMAKE_CURRENT_BINARY_DIR}/usb/us_keymap.h
)

# Link the host test dependencies before enabling fixture-backed stdio.
target_link_libraries(${PROJECT_NAME}
    ${CHECK_LIBRARIES}
    subunit
    m
    yaml)

# Route firmware fopen() calls through test_files.c so registered disk fixtures use
# in-memory cookie-backed FILE handles, while other paths use the real fopen().
target_link_options(${PROJECT_NAME} PRIVATE
    -Wl,--wrap=fopen -Wl,--wrap=time_us_64 -Wl,--wrap=term_inject_char
    -Wl,--wrap=ieee_drive_task -Wl,--wrap=menu_enter)
target_link_options(${PROJECT_NAME} PRIVATE -Wl,--wrap=gpio_put -Wl,--wrap=sleep_us)
target_link_options(${PROJECT_NAME} PRIVATE -Wl,--wrap=gpio_init -Wl,--wrap=gpio_set_dir)
target_link_options(${PROJECT_NAME} PRIVATE -Wl,--wrap=fatal_halt)
target_link_options(${PROJECT_NAME} PRIVATE -Wl,--wrap=diag_loop -Wl,--wrap=diag_complete_halt)
target_link_options(${PROJECT_NAME} PRIVATE
    -Wl,--wrap=display_task -Wl,--wrap=display_term_begin -Wl,--wrap=display_term_end
    -Wl,--wrap=display_term_refresh)
target_link_options(${PROJECT_NAME} PRIVATE
    -Wl,--wrap=opendir -Wl,--wrap=lstat -Wl,--wrap=readdir)
# Preserve useful stack frames in Debug tests.
target_compile_options(${PROJECT_NAME} PRIVATE -fno-omit-frame-pointer)
target_include_directories(${PROJECT_NAME} PRIVATE "${TEST_DIR}" "${TEST_SUPPORT_DIR}" "${PRGS_DIR}" "${PRGS_DIR}/include")
target_include_directories(${PROJECT_NAME} PRIVATE "${EXTERNAL_DIR}/pico-vfs/include")

# Exercise report handling with TinyUSB's real HID definitions, without its hardware runtime.
target_include_directories(${PROJECT_NAME} PRIVATE "${PICO_SDK_PATH}/lib/tinyusb/src")
target_compile_definitions(${PROJECT_NAME} PRIVATE ECONOPET_TEST_TINYUSB CFG_TUSB_MCU=OPT_MCU_NONE)

# Exercise the physical fatal renderer with the linked ROM, display and SPI fixtures.
target_compile_definitions(${PROJECT_NAME} PRIVATE ECONOPET_TEST_FATAL_DISPLAY)

# Generate version header in build directory
add_version_header(${PROJECT_NAME})

# Include directories
include_directories(
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${EXTERNAL_DIR}/libyaml/include"
    "${SRC_DIR}")

# Enable testing
enable_testing()

# Add the test executable as a CTest test
add_test(
    NAME firmware_tests
    COMMAND ${PROJECT_NAME}
)

# Set environment variable to point to sdcard directory
set_tests_properties(firmware_tests PROPERTIES
    ENVIRONMENT "ECONOPET_TEST_SDCARD_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/../../sdcard"
)

# Exercise filesystem identity with real FatFs, not the stdio fixture's alias table.
set(FATFS_DIR "${EXTERNAL_DIR}/pico-vfs/vendor/ff15/source")
add_executable(sd-path-test
    sd_path_test.c
    "${SRC_DIR}/sd/resolve.c"
    "${FATFS_DIR}/ff.c"
    "${FATFS_DIR}/ffsystem.c"
    "${FATFS_DIR}/ffunicode.c")
target_include_directories(sd-path-test PRIVATE
    "${TEST_SUPPORT_DIR}" "${SRC_DIR}" "${FATFS_DIR}"
    "${EXTERNAL_DIR}/pico-vfs/include/filesystem/ChaN")
target_link_libraries(sd-path-test PRIVATE ${CHECK_LIBRARIES} subunit m)
add_test(NAME sd_path_tests COMMAND sd-path-test)

add_executable(sd-diag-test
    sd_diag_test.c
    sd_diag_mock.c
    "${SRC_DIR}/diag/diag.c"
    "${SRC_DIR}/diag/sd.c"
    "${SRC_DIR}/sd/sd.c"
    "${SRC_DIR}/sd/path.c"
    "${SRC_DIR}/sd/resolve.c"
    "${SRC_DIR}/display/char_encoding.c"
    "${SRC_DIR}/display/display.c"
    "${SRC_DIR}/display/window.c"
    "${SRC_DIR}/system/state.c"
    "${SRC_DIR}/global.c"
    "${TEST_SUPPORT_DIR}/test_support.c"
    "${TEST_SUPPORT_DIR}/test_fatal.c"
    "${EXTERNAL_DIR}/pico-vfs/src/blockdevice/heap.c"
    "${EXTERNAL_DIR}/pico-vfs/src/filesystem/fat.c"
    "${FATFS_DIR}/ff.c"
    "${FATFS_DIR}/ffsystem.c"
    "${FATFS_DIR}/ffunicode.c")
# Keep fake SDK declarations and storage interposition exclusive to the host fixture.
target_include_directories(sd-diag-test PRIVATE
    "${TEST_DIR}/sd_diag_mocks"
    "${TEST_SUPPORT_DIR}" "${TEST_DIR}" "${SRC_DIR}"
    "${FATFS_DIR}"
    "${EXTERNAL_DIR}/pico-vfs/include"
    "${EXTERNAL_DIR}/pico-vfs/include/filesystem/ChaN")
target_compile_options(sd-diag-test PRIVATE -fno-omit-frame-pointer)
target_link_options(sd-diag-test PRIVATE
    -Wl,--wrap=calloc
    -Wl,--wrap=sd_blockdevice -Wl,--wrap=sd_fat_filesystem
    -Wl,--wrap=sd_mount_root -Wl,--wrap=sd_unmount_root
    -Wl,--wrap=open -Wl,--wrap=close -Wl,--wrap=read -Wl,--wrap=write
    -Wl,--wrap=lseek -Wl,--wrap=fstat -Wl,--wrap=stat
    -Wl,--wrap=mkdir -Wl,--wrap=rename -Wl,--wrap=unlink -Wl,--wrap=rmdir
    -Wl,--wrap=remove -Wl,--wrap=fopen)
target_link_libraries(sd-diag-test PRIVATE ${CHECK_LIBRARIES} subunit m)
add_test(NAME sd_diag_tests COMMAND sd-diag-test)

# Exercise production initialization without interposing its root-state helpers.
add_executable(sd-init-test
    sd_init_test.c
    "${SRC_DIR}/sd/sd.c"
    "${SRC_DIR}/sd/path.c"
    "${SRC_DIR}/sd/activity.c"
    "${SRC_DIR}/global.c"
    "${TEST_SUPPORT_DIR}/test_support.c"
    "${TEST_SUPPORT_DIR}/test_fatal.c"
    "${EXTERNAL_DIR}/pico-vfs/src/blockdevice/heap.c"
    "${EXTERNAL_DIR}/pico-vfs/src/filesystem/fat.c"
    "${FATFS_DIR}/ff.c"
    "${FATFS_DIR}/ffsystem.c"
    "${FATFS_DIR}/ffunicode.c")
target_include_directories(sd-init-test PRIVATE
    "${TEST_SUPPORT_DIR}" "${SRC_DIR}"
    "${FATFS_DIR}"
    "${EXTERNAL_DIR}/pico-vfs/include"
    "${EXTERNAL_DIR}/pico-vfs/include/filesystem/ChaN")
target_link_options(sd-init-test PRIVATE -Wl,--wrap=filesystem_fat_create)
target_link_libraries(sd-init-test PRIVATE ${CHECK_LIBRARIES} subunit m)
add_test(NAME sd_init_tests COMMAND sd-init-test)

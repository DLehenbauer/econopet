# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

cmake_minimum_required(VERSION 3.21...3.27)

# Root super-project that orchestrates building all subprojects
project(EconoPET_SuperProject NONE)

# Include ExternalProject module for building subprojects
include(ExternalProject)

# Enable testing at the super-project level
enable_testing()

# Resolve ECONOPET_MEDIA_DIR (documented in CMakePresets.json)
if(NOT DEFINED ECONOPET_MEDIA_DIR OR "${ECONOPET_MEDIA_DIR}" STREQUAL "")
    if(DEFINED ENV{ECONOPET_MEDIA_DIR} AND NOT "$ENV{ECONOPET_MEDIA_DIR}" STREQUAL "")
        set(ECONOPET_MEDIA_DIR "$ENV{ECONOPET_MEDIA_DIR}" CACHE PATH "Path to SD card media files")
    else()
        message(FATAL_ERROR "ECONOPET_MEDIA_DIR must be set (via preset cache variable or environment variable).")
    endif()
endif()
message(STATUS "ECONOPET_MEDIA_DIR=${ECONOPET_MEDIA_DIR}")

# ROMs always live in the roms subdirectory of the media tree.
set(ECONOPET_ROMS_DIR "${ECONOPET_MEDIA_DIR}/roms")
message(STATUS "ECONOPET_ROMS_DIR=${ECONOPET_ROMS_DIR}")

# Determine if we should build all subprojects or specific ones
option(BUILD_ROM "Build the 6502 ROM subproject" ON)
option(BUILD_FPGA "Build the FPGA gateware subproject" ON)
option(BUILD_FIRMWARE "Build the RP2040 firmware subproject" ON)
option(BUILD_BOOTLOADER "Build the EconoPET SD-card bootloader" ON)
option(BUILD_TESTS "Build the firmware unit tests" ON)
option(BUILD_SYSTEM_TESTS "Build host, board, firmware integration and hardware contract tests" ON)
option(BUILD_SDCARD "Build the SD card package" ON)
option(BUILD_PRGS "Build the test programs" ON)

# Common build directory
set(SUPER_BUILD_DIR ${CMAKE_BINARY_DIR})

# Shared script for SWD programming that halts the MCU, compares image checksums
# against flash, and programs images that have changed. On success, it resets
# and runs the MCU even if no writes were needed.
set(PROGRAM_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/Program.cmake")

# Reset is an explicit-only operation with no image-building or programming dependencies.
add_custom_target(reset
    COMMAND openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg
        -c "adapter speed 5000" -c "init" -c "poll off" -c "reset run" -c "shutdown"
    COMMENT "Resetting MCU over SWD"
    USES_TERMINAL
    VERBATIM
)

# ============================================================================
# Bootloader Subproject (RP2040 using Pico SDK)
# ============================================================================
set(BOOTLOADER_UF2 "" CACHE FILEPATH "External bootloader UF2 when BUILD_BOOTLOADER is disabled")
set(BOOTLOADER_TEST_ARGS)
if(BUILD_BOOTLOADER)
    ExternalProject_Add(bootloader_project
        SOURCE_DIR "${CMAKE_SOURCE_DIR}/fw/external/pico-sdcard-bootloader"
        BINARY_DIR "${SUPER_BUILD_DIR}/boot"
        CMAKE_ARGS
            -G Ninja
            -DCMAKE_BUILD_TYPE=RelWithDebInfo
            -DPICO_PLATFORM=rp2040
            -DPICO_SDK_PATH=$ENV{PICO_SDK_PATH}
            -DCMAKE_TOOLCHAIN_FILE=$ENV{PICO_SDK_PATH}/cmake/preload/toolchains/pico_arm_cortex_m0plus_gcc.cmake
            "-DBOOTLOADER_CONFIG_FILE=${CMAKE_SOURCE_DIR}/boot/config.cmake"
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target bootloader
        INSTALL_COMMAND ""
        BUILD_ALWAYS TRUE
    )
    set(BOOTLOADER_UF2 "${SUPER_BUILD_DIR}/boot/src/boot3/bootloader.uf2")
    set(BOOTLOADER_ELF "${SUPER_BUILD_DIR}/boot/src/boot3/bootloader.elf")
    set(BOOTLOADER_TEST_ARGS
        --bootloader-elf "${BOOTLOADER_ELF}")

    # Add the 'prog-boot' target to program the bootloader via SWD. Its bootloader
    # dependency refreshes the ELF first and prevents programming if the build fails.
    #
    # Note: Programming is an explicit 'prog-*' target and is never part of
    #       normal build or test targets such as 'all'.
    add_custom_target(prog-boot
        COMMAND "${CMAKE_COMMAND}" "-DBOOTLOADER_ELF=${BOOTLOADER_ELF}" -P "${PROGRAM_SCRIPT}"
        DEPENDS bootloader_project
        COMMENT "Programming and verifying bootloader over SWD"
        USES_TERMINAL
        VERBATIM
    )
endif()

if(BOOTLOADER_UF2)
    list(APPEND BOOTLOADER_TEST_ARGS --bootloader "${BOOTLOADER_UF2}")
endif()

# ============================================================================
# ROM Subproject (6502 Assembly using CC65)
# ============================================================================
if(BUILD_ROM)
    ExternalProject_Add(rom_project
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/rom
        BINARY_DIR ${SUPER_BUILD_DIR}/rom
        CMAKE_ARGS
            -G Ninja
            -DCMAKE_BUILD_TYPE=RelWithDebInfo
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR>
        INSTALL_COMMAND ""
        BUILD_ALWAYS TRUE
    )
    set(ROM_BINARY ${SUPER_BUILD_DIR}/rom/bin/menu.rom)
endif()

# ============================================================================
# FPGA Subproject (Verilog using Efinity)
# ============================================================================
if(BUILD_FPGA)
    ExternalProject_Add(fpga_project
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/gw
        BINARY_DIR ${SUPER_BUILD_DIR}/gw
        CMAKE_ARGS
            -G Ninja
            -DECONOPET_ROMS_DIR=${ECONOPET_ROMS_DIR}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target fpga_bitstream
        INSTALL_COMMAND ""
        BUILD_ALWAYS TRUE
    )
    set(FPGA_BITSTREAM ${SUPER_BUILD_DIR}/gw/outflow/EconoPET.hex.bin)
    set(PROGRAM_FPGA_ARGS
        "-DFPGA_BITSTREAM=${FPGA_BITSTREAM}"
        "-DFPGA_IMAGE=${SUPER_BUILD_DIR}/gw/fpga.flash.bin"
    )

    ExternalProject_Add(gateware_models
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/gw/simulator
        BINARY_DIR ${SUPER_BUILD_DIR}/gw-test
        CMAKE_ARGS
            -G Ninja
            -DCMAKE_BUILD_TYPE=RelWithDebInfo
            -DECONOPET_ROMS_DIR=${ECONOPET_ROMS_DIR}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target gateware_models
        INSTALL_COMMAND ""
        BUILD_ALWAYS TRUE
    )

    # Add the 'prog-gw' target to program gateware via SWD. Its gateware dependency
    # refreshes the bitstream first and prevents programming if the build fails.
    #
    # Note: Programming is an explicit 'prog-*' target and is never part of
    #       normal build or test targets such as 'all'.
    add_custom_target(prog-gw
        COMMAND "${CMAKE_COMMAND}" ${PROGRAM_FPGA_ARGS} -P "${PROGRAM_SCRIPT}"
        DEPENDS fpga_project
        COMMENT "Programming and verifying gateware over SWD"
        USES_TERMINAL
        VERBATIM
    )
endif()

# ============================================================================
# Firmware Subproject (RP2040 using Pico SDK)
# ============================================================================
if(BUILD_FIRMWARE)
    # Firmware requires special configuration with Pico SDK
    set(FW_CMAKE_ARGS
        -G Ninja
        -DCMAKE_BUILD_TYPE=RelWithDebInfo
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        -DPICO_PLATFORM=rp2040
        -DPICO_SDK_PATH=$ENV{PICO_SDK_PATH}
        -DCMAKE_TOOLCHAIN_FILE=$ENV{PICO_SDK_PATH}/cmake/preload/toolchains/pico_arm_cortex_m0plus_gcc.cmake
        -DECONOPET_ROMS_DIR=${ECONOPET_ROMS_DIR}
        -DECONOPET_ROM_BIN_DIR=${SUPER_BUILD_DIR}/rom/bin
    )
    
    ExternalProject_Add(firmware_project
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/fw
        BINARY_DIR ${SUPER_BUILD_DIR}/fw
        CMAKE_ARGS ${FW_CMAKE_ARGS}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target firmware.elf
        INSTALL_COMMAND ""
        BUILD_ALWAYS TRUE
    )
    set(FIRMWARE_ELF ${SUPER_BUILD_DIR}/fw/src/firmware.elf)
    set(FIRMWARE_UF2 ${SUPER_BUILD_DIR}/fw/src/firmware.elf.uf2)
    if(BUILD_ROM)
        add_dependencies(firmware_project rom_project)
    endif()

    # Add the 'prog-fw' target to program firmware via SWD. Its firmware dependency
    # refreshes the ELF first and prevents programming if the build fails.
    #
    # Note: Programming is an explicit 'prog-*' target and is never part of
    #       normal build or test targets such as 'all'.
    add_custom_target(prog-fw
        COMMAND "${CMAKE_COMMAND}" "-DFIRMWARE_ELF=${FIRMWARE_ELF}" -P "${PROGRAM_SCRIPT}"
        DEPENDS firmware_project
        COMMENT "Programming and verifying firmware over SWD"
        USES_TERMINAL
        VERBATIM
    )
endif()

# ============================================================================
# Combined bootloader, firmware and gateware SWD programming (explicit-only)
# ============================================================================
if(BUILD_BOOTLOADER AND BUILD_FIRMWARE AND BUILD_FPGA)
    # Add the 'prog' target to program bootloader, firmware and gateware via
    # SWD. Its dependencies refresh all three images first and prevent
    # programming if any build fails.
    #
    # Note: Programming is an explicit 'prog' target and is never part of
    #       normal build or test targets such as 'all'.
    add_custom_target(prog
        COMMAND "${CMAKE_COMMAND}" "-DBOOTLOADER_ELF=${BOOTLOADER_ELF}"
            "-DFIRMWARE_ELF=${FIRMWARE_ELF}"
            ${PROGRAM_FPGA_ARGS} -P "${PROGRAM_SCRIPT}"
        DEPENDS bootloader_project firmware_project fpga_project
        COMMENT "Programming and verifying bootloader, firmware and gateware over SWD"
        USES_TERMINAL
        VERBATIM
    )
endif()

# ============================================================================
# Firmware Tests Subproject (Linux host tests)
# ============================================================================
if(BUILD_TESTS)
    set(TEST_CMAKE_ARGS
        -G Ninja
        -DCMAKE_BUILD_TYPE=Debug
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        -DPICO_PLATFORM=host
        -DPICO_NO_HARDWARE=1
        -DPICO_SDK_PATH=$ENV{PICO_SDK_PATH}
    )
    
    ExternalProject_Add(test_project
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/fw
        BINARY_DIR ${SUPER_BUILD_DIR}/test
        CMAKE_ARGS ${TEST_CMAKE_ARGS}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR>
        INSTALL_COMMAND ""
        BUILD_ALWAYS TRUE
    )
endif()

# ============================================================================
# System Tests Subproject (host fixtures, hardware contract, board and firmware)
# ============================================================================
if(BUILD_SYSTEM_TESTS)
    ExternalProject_Add(system_test_project
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/test/system
        BINARY_DIR ${SUPER_BUILD_DIR}/system
        CMAKE_ARGS
            -G Ninja
            -DCMAKE_BUILD_TYPE=Debug
            -DBUILD_TESTING=ON
            -DBUILD_BOARD_TESTS=ON
            -DECONOPET_MEDIA_DIR=${ECONOPET_MEDIA_DIR}
            -DECONOPET_TEST_SEED_FILE=${SUPER_BUILD_DIR}/test.seed
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR>
        INSTALL_COMMAND ""
        BUILD_ALWAYS TRUE
    )
endif()

# ============================================================================
# SD Card Package (depends on ROM, FPGA, and Firmware)
# ============================================================================
if(BUILD_SDCARD AND BUILD_FIRMWARE AND BUILD_FPGA)
    if(NOT BOOTLOADER_UF2)
        message(FATAL_ERROR "SD-card packaging requires BUILD_BOOTLOADER=ON or an explicit BOOTLOADER_UF2")
    endif()
    ExternalProject_Add(sdcard_project
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/sdcard
        BINARY_DIR ${SUPER_BUILD_DIR}/sdcard
        DEPENDS firmware_project fpga_project
        CMAKE_ARGS
            -G Ninja
            -DECONOPET_MEDIA_DIR=${ECONOPET_MEDIA_DIR}
            -DFIRMWARE_UF2=${FIRMWARE_UF2}
            -DFPGA_BITSTREAM=${FPGA_BITSTREAM}
            "-DBOOTLOADER_UF2=${BOOTLOADER_UF2}"
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR>
        INSTALL_COMMAND ""
        BUILD_ALWAYS TRUE
    )
    if(BUILD_BOOTLOADER)
        add_dependencies(sdcard_project bootloader_project)
    endif()
endif()

# ============================================================================
# Test Programs (CC65)
# ============================================================================
if(BUILD_PRGS)
    ExternalProject_Add(prgs_project
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/prgs
        BINARY_DIR ${SUPER_BUILD_DIR}/prgs
        CMAKE_ARGS
            -G Ninja
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR>
        INSTALL_COMMAND ""
        BUILD_ALWAYS TRUE
    )
endif()

# ============================================================================
# Site/Documentation target
# ============================================================================
add_custom_target(site
    COMMAND npm install
    COMMAND npm run build
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/site
    COMMENT "Building static site with documentation"
)

# ============================================================================
# Convenience targets
# ============================================================================

# Target to build everything
add_custom_target(all_projects
    DEPENDS 
        $<$<BOOL:${BUILD_ROM}>:rom_project>
        $<$<BOOL:${BUILD_FPGA}>:fpga_project>
        $<$<BOOL:${BUILD_FIRMWARE}>:firmware_project>
        $<$<BOOL:${BUILD_BOOTLOADER}>:bootloader_project>
        $<$<BOOL:${BUILD_TESTS}>:test_project>
        $<$<BOOL:${BUILD_SYSTEM_TESTS}>:system_test_project>
        $<$<BOOL:${BUILD_SDCARD}>:sdcard_project>
        $<$<BOOL:${BUILD_PRGS}>:prgs_project>
    COMMENT "Building all enabled subprojects"
)

# Make all_projects the default target
add_custom_target(default_target ALL DEPENDS all_projects)

# ============================================================================
# Testing Integration
# ============================================================================

# One pseudo-random seed per ctest run, shared by every test so a failing run can be
# replayed in full via ECONOPET_TEST_SEED. A pre-test hook (rather than a fixture) so
# the seed always prints, even when every test passes. See docs/dev/testing.md.
set(ECONOPET_TEST_SEED_FILE "${CMAKE_BINARY_DIR}/test.seed")
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/CTestCustom.cmake.in"
    "${CMAKE_BINARY_DIR}/CTestCustom.cmake"
    @ONLY
)

# Applied to every test so seed.sh can resolve the shared seed and the gateware
# scripts can derive the ROM path from the configured media tree.
set(ECONOPET_TEST_ENV
    "ECONOPET_TEST_SEED_FILE=${ECONOPET_TEST_SEED_FILE};ECONOPET_MEDIA_DIR=${ECONOPET_MEDIA_DIR}"
)

# Add firmware tests to root CTest
if(BUILD_TESTS)
    add_test(
        NAME firmware_tests
        COMMAND ${SUPER_BUILD_DIR}/test/test/firmware-test
    )
    set_tests_properties(firmware_tests PROPERTIES
        LABELS "fw;unit"
        DEPENDS test_project
        ENVIRONMENT "ECONOPET_TEST_SDCARD_ROOT=${CMAKE_SOURCE_DIR}/sdcard;${ECONOPET_TEST_ENV}"
    )
endif()

# Check the separately built firmware image and SD bootloader flash contract.
if(BUILD_FIRMWARE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    include("${CMAKE_SOURCE_DIR}/cmake/FlashLayout.cmake")
    add_test(
        NAME firmware_flash_layout
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/fw/test/flash_layout_test.py"
            --elf "${FIRMWARE_ELF}"
            --firmware "${FIRMWARE_UF2}"
            ${BOOTLOADER_TEST_ARGS}
            --map "${FIRMWARE_ELF}.map"
            --fpga-address "${ECONOPET_FPGA_ADDRESS}"
    )
    set_tests_properties(firmware_flash_layout PROPERTIES LABELS "flash")
    if(BUILD_SDCARD AND BUILD_FPGA)
        add_test(
            NAME sdcard_flash_layout
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/fw/test/flash_layout_test.py"
                --elf "${FIRMWARE_ELF}"
                --firmware "${FIRMWARE_UF2}"
                ${BOOTLOADER_TEST_ARGS}
                --map "${FIRMWARE_ELF}.map"
                --fpga-address "${ECONOPET_FPGA_ADDRESS}"
                --combined "${SUPER_BUILD_DIR}/sdcard/sdcard_root/firmware.uf2"
                --fpga-image "${SUPER_BUILD_DIR}/sdcard/fpga.flash.bin"
        )
        set_tests_properties(sdcard_flash_layout PROPERTIES LABELS "flash")
    endif()
endif()

# Load standalone test registrations at CTest time, after build-time discovery.
if(BUILD_SYSTEM_TESTS)
    configure_file(
        "${CMAKE_SOURCE_DIR}/cmake/SystemTests.cmake.in"
        "${CMAKE_BINARY_DIR}/SystemTests.cmake"
        @ONLY)
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
        "${CMAKE_BINARY_DIR}/SystemTests.cmake")
endif()

# Add Verilog simulation tests to root CTest
if(BUILD_FPGA)
    # When reasonable to do so, benches run under both Icarus Verilog and Verilator.
    # The following tests are Verilator-only because they run slowly under Icarus Verilog:
    set(GW_VERILATOR_ONLY_TESTS
        stock6502_boot_tb
        superpet_waterloo_tb
        video_crtc_timing_tb
    )
    set(GW_TEST_EXTRA_LABELS_stock6502_boot_tb boot)
    set(GW_TEST_EXTRA_LABELS_superpet_waterloo_tb boot)
    set(GW_RAND_RESET_DEFAULT 0)

    add_test(
        NAME gateware_filelists
        COMMAND ${CMAKE_SOURCE_DIR}/sim.sh --update-only
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(gateware_filelists PROPERTIES
        LABELS "sim_setup"
        FIXTURES_SETUP gateware_filelists
    )

    # Simulator generation and compilation stay outside the root build graph.
    add_dependencies(all_projects gateware_models)

    # Discover testbenches and add each as a separate test
    file(GLOB TB_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/gw/EconoPET/sim/*_tb.sv")
    foreach(TB_FILE ${TB_FILES})
        get_filename_component(TB_NAME ${TB_FILE} NAME_WE)

        if(DEFINED GW_RAND_RESET_${TB_NAME})
            set(TB_RAND_RESET ${GW_RAND_RESET_${TB_NAME}})
        else()
            set(TB_RAND_RESET ${GW_RAND_RESET_DEFAULT})
        endif()

        set(TB_EXECUTABLE "${SUPER_BUILD_DIR}/gw-test/bin/${TB_NAME}_vl")

        if(TB_NAME IN_LIST GW_VERILATOR_ONLY_TESTS)
            add_test(
                NAME ${TB_NAME}
                COMMAND ${CMAKE_SOURCE_DIR}/verilate.sh
                    "${TB_EXECUTABLE}" ${TB_RAND_RESET}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            )

            set(TB_LABELS "gw;sim;vl")
            if(DEFINED GW_TEST_EXTRA_LABELS_${TB_NAME})
                list(APPEND TB_LABELS ${GW_TEST_EXTRA_LABELS_${TB_NAME}})
            endif()

            set_tests_properties(${TB_NAME} PROPERTIES
                LABELS "${TB_LABELS}"
                TIMEOUT 1800
                ENVIRONMENT "${ECONOPET_TEST_ENV}"
            )
        else()
            add_test(
                NAME ${TB_NAME}
                COMMAND ${CMAKE_SOURCE_DIR}/sim.sh --no-update ${TB_NAME}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            )
            set_tests_properties(${TB_NAME} PROPERTIES
                LABELS "gw;sim;iv"
                TIMEOUT 300
                FIXTURES_REQUIRED gateware_filelists
                ENVIRONMENT "${ECONOPET_TEST_ENV}"
            )

            # Execution has its own budget, independent of model compilation.
            add_test(
                NAME ${TB_NAME}_vl
                COMMAND ${CMAKE_SOURCE_DIR}/verilate.sh
                    "${TB_EXECUTABLE}" ${TB_RAND_RESET}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            )
            set_tests_properties(${TB_NAME}_vl PROPERTIES
                LABELS "gw;sim;vl"
                TIMEOUT 600
                ENVIRONMENT "${ECONOPET_TEST_ENV}"
            )
        endif()
    endforeach()
endif()

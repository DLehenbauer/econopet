# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Verilator + GoogleTest harness for the gateware simulation testbenches.
#
# Each self-checking SystemVerilog testbench (gw/EconoPET/sim/*_tb.sv) is
# Verilated into its own model (one per TOP_MODULE) and linked into a single
# gw_sim_tests executable. A generic C++ runner (sim_harness.h) drives each model
# to $finish/$fatal, and gtest_discover_tests registers one CTest case per
# testbench so they run (and parallelize) under `ctest --preset gw -j`.

cmake_minimum_required(VERSION 3.19)

project(gw_sim_tests CXX)

set(CMAKE_CXX_STANDARD 20)          # Verilator --timing requires C++20 coroutines
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Testbenches migrated to the Verilator + GoogleTest harness.
#
# To migrate another testbench, add its module name here (the file must be
# gw/EconoPET/sim/<name>.sv with a matching top module). Testbenches that rely on
# tri-state (inout / 'z') or 4-state (===) constructs are not yet supported by
# Verilator and must be reworked first (tracked separately).
#
# The list may be provided by the parent project (root CMakeLists.txt); fall back
# to a default when building this directory standalone.
# ---------------------------------------------------------------------------
if(NOT DEFINED VERILATED_TESTBENCHES)
    set(VERILATED_TESTBENCHES
        address_decoding_tb
        bram_tb
        breakpoint_tb
        cpu_data_mux_tb
        keyboard_tb
        memory_map_tb
        mock_sram_tb
        open_bus_tb
        ram_tb
        register_file_tb
        spi1_tb
        spi_tb
        timing_tb
        top_tb
        video_crtc_reg_tb
        video_crtc_tb
        video_tb
        wbc_mux_tb
        wbp_mux_tb
    )
endif()

# EconoPET project root (the directory Verilator's -y/-I paths are relative to).
set(GW_PROJ_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
cmake_path(NORMAL_PATH GW_PROJ_DIR)

# top_tb (the full-system testbench) instantiates the SOFT_CPU m6502 core and the
# icesid SID. These external sources cannot be found via Verilator's -y search:
# the m6502 core imports its package in the module header (so the package must be
# compiled first) and the icesid module names do not match their file names. List
# them explicitly, package first (matching the Efinity project's design_file order).
set(GW_M6502_PKG ${GW_PROJ_DIR}/external/m6502/rtl/cpu_6502_pkg.sv)
file(GLOB GW_ICESID_SOURCES ${GW_PROJ_DIR}/external/icesid/icesid/*.v)

# ---------------------------------------------------------------------------
# Verilator warning policy.
#
# Warnings are fatal (-Wall) EXCEPT for a curated set that is inherent to the
# shared package and the legacy delay-based testbench style. Each waiver below
# is a whole category that produces only false positives in this per-testbench
# context; genuinely dangerous lints (LATCH, CASEINCOMPLETE, BLKANDNBLK, etc.)
# remain fatal.
# ---------------------------------------------------------------------------
set(GW_SIM_WNO
    -Wno-UNUSEDPARAM    # common_pkg exposes many params; each testbench uses a subset
    -Wno-UNUSEDSIGNAL   # legacy testbench scaffolding signals
    -Wno-UNUSEDLOOP     # driver loop indices used only for timing waits
    -Wno-IMPORTSTAR     # testbenches use `import common_pkg::*`
    -Wno-INITIALDLY     # delay-based drivers use <= inside tasks run from initial
    -Wno-VARHIDDEN      # task arguments intentionally shadow testbench-level names
    -Wno-WIDTHEXPAND    # intentional narrow literals in testbench stimulus
    -Wno-WIDTHTRUNC     # intentional narrow literals in testbench stimulus
    -Wno-PROCASSINIT    # RTL signals with an initializer also assigned procedurally
    -Wno-UNDRIVEN       # testbench-only observation signals left undriven
    -Wno-DECLFILENAME   # some RTL modules do not match their file name (e.g. address_decoding.sv)
    -Wno-PINCONNECTEMPTY # testbenches intentionally leave unused DUT outputs unconnected (.pin())
    -Wno-EOFNEWLINE     # a few testbench sources lack a trailing newline
    -Wno-SYNCASYNCNET   # testbench observation signals sampled in several always @(posedge) blocks
    -Wno-BLKSEQ         # SRAM model commits writes with blocking '=' from a clocked task
)

# ---------------------------------------------------------------------------
# GoogleTest (FetchContent, pinned tarball, hash-verified). SYSTEM keeps its
# headers out of any -Wall -Wextra -Werror gate on our own C++.
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    googletest
    URL      https://github.com/google/googletest/releases/download/v1.15.2/googletest-1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    SYSTEM
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ---------------------------------------------------------------------------
# Verilator.
# ---------------------------------------------------------------------------
find_package(verilator HINTS $ENV{VERILATOR_ROOT})
if(NOT verilator_FOUND)
    message(FATAL_ERROR "Verilator was not found. Set VERILATOR_ROOT or install Verilator.")
endif()

# Generate one TEST() per testbench from the list above (single source of truth).
set(GW_SIM_INCLUDES "")
set(GW_SIM_TESTS "")
foreach(tb ${VERILATED_TESTBENCHES})
    string(APPEND GW_SIM_INCLUDES "#include \"V${tb}.h\"\n")
    string(APPEND GW_SIM_TESTS "TEST(gw, ${tb}) { sim_harness::run<V${tb}>(); }\n")
endforeach()
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/sim_tests.cpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/sim_tests.cpp
    @ONLY
)

add_executable(gw_sim_tests
    ${CMAKE_CURRENT_BINARY_DIR}/sim_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vl_overrides.cpp
)
target_include_directories(gw_sim_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gw_sim_tests PRIVATE GTest::gtest GTest::gtest_main)

# Provide our own vl_fatal (see vl_overrides.cpp) instead of the default that
# calls std::exit() on $fatal.
target_compile_definitions(gw_sim_tests PRIVATE VL_USER_FATAL)

# Verilate each testbench as its own model within the shared executable.
foreach(tb ${VERILATED_TESTBENCHES})
    # Most testbenches need only their own source (Verilator finds the rest via -y).
    # top_tb additionally needs the m6502 package (compiled first) and the icesid
    # SID sources listed explicitly, plus a PINMISSING waiver (mock_system leaves
    # many top-level I/O pins unconnected on purpose).
    set(tb_sources ${GW_PROJ_DIR}/sim/${tb}.sv)
    set(tb_extra_args "")
    if(tb STREQUAL "top_tb")
        list(PREPEND tb_sources ${GW_M6502_PKG} ${GW_ICESID_SOURCES})
        set(tb_extra_args -Wno-PINMISSING)
    endif()

    verilate(gw_sim_tests
        SOURCES ${tb_sources}
        PREFIX V${tb}
        TOP_MODULE ${tb}
        VERILATOR_ARGS
            --timing
            --assert
            --sv
            -Wall ${GW_SIM_WNO} ${tb_extra_args}
            -y ${GW_PROJ_DIR}/src
            -y ${GW_PROJ_DIR}/sim
            -y ${GW_PROJ_DIR}/external/m6502/rtl
            -I${GW_PROJ_DIR}
            -I${GW_PROJ_DIR}/external/m6502/rtl
            -DECONOPET_ROMS_DIR=\"${ECONOPET_ROMS_DIR}\"
    )
endforeach()

# ---------------------------------------------------------------------------
# Register one CTest case per testbench (parallelizable via `ctest -j`).
# ---------------------------------------------------------------------------
enable_testing()
include(GoogleTest)
gtest_discover_tests(gw_sim_tests
    TEST_PREFIX "gw/"
    PROPERTIES LABELS "gw\;verilator" TIMEOUT 300
    DISCOVERY_TIMEOUT 120
)

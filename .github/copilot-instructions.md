# EconoPET Copilot Instructions

## Project Overview

EconoPET is an open hardware mainboard replacement for Commodore PET/CBM computers. The system has three main components that work together:

| Component | Location | Language | Toolchain |
|-----------|----------|----------|-----------|
| **Firmware** | `/fw` | C (RP2040) | Pico SDK, ARM GCC |
| **Gateware** | `/gw` | SystemVerilog | Efinity (Efinix FPGA) |
| **ROMs** | `/rom` | 6502 Assembly | CC65, ACME |

## Architecture

**Communication flow:** The RP2040 MCU (firmware) controls the FPGA (gateware) via SPI. The MCU uploads the FPGA bitstream on power-on, then uses SPI commands to read/write to the PET's address space and control system state. The FPGA manages the 6502 CPU bus, RAM, video timing, and I/O.

Key interfaces:
- [fw/src/driver.h](fw/src/driver.h) - MCU-to-FPGA SPI protocol (firmware side)
- [gw/EconoPET/src/spi.sv](gw/EconoPET/src/spi.sv) - SPI protocol (gateware side)
- [fw/src/hw.h](fw/src/hw.h) - GPIO and hardware pin definitions

## Build Commands

```sh
cmake --preset default              # Configure (run first)
cmake --build --preset fw           # Build firmware only
cmake --build --preset fw_test      # Build firmware tests
ctest --preset fw                   # Run firmware tests
ctest --preset gw                   # Run gateware simulations (fast)
cmake --build --preset gw           # Build FPGA bitstream (slow, ~2 min)
```

## Development Workflows

### Firmware changes (`/fw/src`)
1. `cmake --build --preset fw` - build firmware
2. `cmake --build --preset fw_test` - build tests
3. `ctest --preset fw` - run tests

### Gateware changes (`/gw/EconoPET/src`)
1. `ctest --preset gw` - run Verilog simulations (Icarus Verilog)
2. Only if tests pass: `cmake --build --preset gw` - build bitstream

**Critical:** Never start gateware build when simulation tests are failing. The gateware build is slow and simulation catches most issues.

## Code Conventions

### File headers
New source files should include the CC0 license header with author attribution (see fw/src/main.c for example).

### Firmware C conventions (`/fw`)
- Header files: never include `pch.h`; `#pragma once` must be the first non-comment line
- Source files include order: `pch.h`, corresponding header, blank line, standard library `<...>` (alphabetized), blank line, external `"..."` (alphabetized), blank line, project `"..."` (alphabetized)
- Include paths: don’t use relative paths like `../../pch.h` (the include path already contains `fw/src`); use simple includes like `#include "pch.h"` or `#include "subdir/header.h"`

### Firmware patterns
- Tests use [Check](https://libcheck.github.io/check/) framework - see [fw/test/main.c](fw/test/main.c)
- Platform abstraction via `PICO_PLATFORM`: real hardware vs host testing
- Precompiled header: `pch.h` included in all firmware `.c` source files (never from headers)

### Gateware patterns
- Testbenches named `*_tb.sv` in [gw/EconoPET/sim/](gw/EconoPET/sim/)
- `top.sv` normalizes hardware signals, `main.sv` contains core logic
- Active-high internal signals (top module inverts active-low pins)

## External Dependencies

Directories `/fw/external` and `/gw/EconoPET/external` are **git submodules**. Do not modify files in these paths without explicit confirmation. When changes seem to require modifying submodule code:
1. Propose alternatives first (wrappers, adapters, compile flags)
2. Request confirmation before proceeding

## Environment Variables

- `ECONOPET_ROMS_DIR` points to PET ROM files (BASIC, KERNAL, etc.)
- `PICO_SDK_PATH` points to the Raspberry Pi Pico SDK (typically `/opt/pico-sdk`). You may read files from this path to understand Pico SDK APIs, even though it is outside the VS Code workspace.

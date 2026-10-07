# EconoPET Agent Instructions

## Project Overview

EconoPET is an open hardware mainboard replacement for Commodore PET/CBM computers with three main components:

| Component | Location | Language | Toolchain |
|-----------|----------|----------|-----------|
| **Firmware** | `/fw` | C (RP2040) | Pico SDK, ARM GCC |
| **Gateware** | `/gw` | SystemVerilog | Efinity (Efinix FPGA), Icarus Verilog |
| **ROMs** | `/rom` | 6502 Assembly | CC65, ACME |

## Architecture

- The RP2040 MCU (firmware) controls the FPGA (gateware) via SPI
- On power-on, the MCU uploads the FPGA bitstream, then uses SPI commands to read/write the PET's address space and control system state
- The FPGA manages the 6502 CPU bus, RAM, video timing, and I/O

Key interfaces:
- [fw/src/driver.h](fw/src/driver.h) - MCU-to-FPGA SPI protocol (firmware side)
- [gw/EconoPET/src/spi.sv](gw/EconoPET/src/spi.sv) - SPI protocol (gateware side)
- [fw/src/hw.h](fw/src/hw.h) - GPIO and hardware pin definitions

## Build

Build production with CMake `RelWithDebInfo`. Build tests, simulator integration, frameworks,
and their dependencies with `Debug`, including production sources compiled for
tests. Use root presets to apply this split. Do not add Release builds or
cross-configuration test matrices.

Build Verilator-generated models and runtime separately with optimized
`RelWithDebInfo` flags (Verilator optimization, native CPU tuning, and LTO).
Preserve SV assertions. Keep linked test, framework, and firmware code in
`Debug`. Do not propagate model optimization flags or `NDEBUG` to those sources.

```sh
cmake --preset default              # Configure (run first)
cmake --build --preset fw           # Build firmware only
cmake --build --preset fw-test      # Build firmware tests
ctest --preset fw --parallel        # Run firmware tests
ctest --preset gw --parallel        # Run gateware simulations (fast)
cmake --build --preset gw           # Build FPGA bitstream (slow, ~2 min)
cmake --build --preset sys-test     # Build host fixtures and hardware contract tests
ctest --preset sys --parallel       # Run host fixtures and hardware contract tests
```

## Code Conventions

### Plain ASCII Text

- Use ASCII in generated prose: straight quotes (`'`, `"`), hyphens (`-`), and `...` (no smart quotes, em/en dashes, or Unicode decoratives)
- Avoid semi-colons.
- Use parentheses (not dashes) to set off clauses

### File Headers

Include the SPDX license header in all new source files (see `fw/src/main.c` for example).

### Linting

- Fix lint causes. Suppress only demonstrated false positives, at the narrowest
  scope available. Explain each suppression beside it.

### Stubs

- Stubs must assert with a useful message, never return placeholders or succeed.

### Documentation

- Document every function, method, and task concisely: explain its purpose and,
  when not obvious, its callers or users.
- Precede each step of a multi-step procedure with a concise comment explaining
  its intent and any non-obvious rationale. Do not number steps.

### Magic Numbers

- Name states, modes, register addresses, bit masks, and limits instead of using
  numeric literals.
- Reuse existing definitions.

## Environment Variables

| Variable | Purpose |
|----------|---------|
| `ECONOPET_MEDIA_DIR` | Path to the directory containing `roms/` and `disks/` for the SD-card package |
| `PICO_SDK_PATH` | Path to Raspberry Pi Pico SDK (typically `/opt/pico-sdk`). Read files here to understand Pico SDK APIs, even though it is outside the workspace. |

# EconoPET Copilot Instructions

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

## Build Commands

```sh
cmake --preset default              # Configure (run first)
cmake --build --preset fw           # Build firmware only
cmake --build --preset fw_test      # Build firmware tests
ctest --preset fw                   # Run firmware tests
ctest --preset gw                   # Run gateware simulations (fast)
cmake --build --preset gw           # Build FPGA bitstream (slow, ~2 min)
```

## Code Conventions

### Plain ASCII Text

- Use plain ASCII in all generated prose (commit messages, PR text, docs, comments, user-facing strings)
- Never emit smart/curly quotes, long dashes, ellipsis, or other typographic Unicode:
  - Use `'` and `"` (not curly quotes)
  - Use `-` (not en-dash or em-dash)
  - Use `...` (not ellipsis character)
- Never use decorative Unicode (bullet, arrow, non-breaking spaces) unless explicitly requested or required by a technical format
- Avoid overuse of semi-colons, as these are rarely used in English
- Do not use hyphens or double hyphens to set off clauses - like this - or -- like this --. Use parentheses instead (like this).

### File Headers

Include the SPDX license header in all new source files (see `fw/src/main.c` for example).

## Environment Variables

| Variable | Purpose |
|----------|---------|
| `ECONOPET_ROMS_DIR` | Path to PET ROM files (BASIC, KERNAL, etc.) |
| `PICO_SDK_PATH` | Path to Raspberry Pi Pico SDK (typically `/opt/pico-sdk`). Read files here to understand Pico SDK APIs, even though it is outside the workspace. |

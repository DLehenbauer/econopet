# EconoPET Copilot Instructions

## Project Overview

EconoPET is an open hardware mainboard replacement for Commodore PET/CBM computers. The system has three main components that work together:

| Component | Location | Language | Toolchain |
|-----------|----------|----------|-----------|
| **Firmware** | `/fw` | C (RP2040) | Pico SDK, ARM GCC |
| **Gateware** | `/gw` | SystemVerilog | Efinity (Efinix FPGA), Icarus Verilog |
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

## Code Conventions

### Plain ASCII text (avoid "fancy" Unicode)
- When generating prose (commit messages, PR text, docs, comments, user-facing strings), prefer plain ASCII characters.
- Do not emit smart/curly quotes, long dashes, ellipsis, or other typographic Unicode that humans typically don't type:
  - Use `'` and `"` (not curly quotes)
  - Use `-` (not en-dash or em-dash)
  - Use `...` (not ellipsis character)
- Avoid other decorative Unicode in text (e.g., bullet, arrow, non-breaking spaces) unless the user explicitly asks for it or it is required by a technical format.
- Avoid overuse of semi-colons, as these are rarely used in English.

### File headers
New source files should include the CC0 license header with author attribution (see fw/src/main.c for example).

## Environment Variables

- `ECONOPET_ROMS_DIR` points to PET ROM files (BASIC, KERNAL, etc.)
- `PICO_SDK_PATH` points to the Raspberry Pi Pico SDK (typically `/opt/pico-sdk`). You may read files from this path to understand Pico SDK APIs, even though it is outside the VS Code workspace.

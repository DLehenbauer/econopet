---
description: 'SystemVerilog coding conventions and patterns for FPGA gateware development'
applyTo: 'gw/**/*.sv, gw/**/*.v, gw/**/*.svh'
---

# Gateware Development Guidelines

Instructions for developing SystemVerilog gateware for the Efinix FPGA.

## Module Organization

| Module | Purpose |
|--------|---------|
| `top.sv` | Normalizes hardware signals (inverts active-low pins) |
| `main.sv` | Contains core logic with active-high internal signals |

## Signal Conventions

- Use active-high signals internally
- The `top` module inverts active-low hardware pins at the boundary
- Use "controller" and "peripheral" instead of deprecated "master" and "slave" terminology for all buses (Wishbone, SPI, I2C, etc.):

| Use | Instead of |
|-----|------------|
| Controller | Master |
| Peripheral | Slave |

### Naming Suffixes

| Suffix | Meaning | Example |
|--------|---------|---------|
| `_i` | Input port | `clock_i`, `data_i` |
| `_o` | Output port | `data_o`, `valid_o` |
| `_n` | Active-low  | `reset_n` |
| `_ni` | Active-low input | `cs_ni` |
| `_no` | Active-low output | `irq_no` |
| `_en` | Enable signal | `clk8_en`, `write_en` |
| `_oe` | Output enable | `data_oe` |

### Wishbone Bus Prefixes

| Prefix | Meaning | Example |
|--------|---------|---------|
| `wb_` | Shared Wishbone bus signals | `wb_addr`, `wb_cycle` |
| `wbc_` | Wishbone controller signals | `wbc_addr_i`, `wbc_cycle_i` |
| `wbp_` | Wishbone peripheral signals | `wbp_din_o`, `wbp_ack_o` |

### SPI Signal Names

For SPI signals specifically, follow the [OSHWA resolution](https://oshwa.org/resources/a-resolution-to-redefine-spi-signal-names/):

| Use | Instead of |
|-----|------------|
| `SDO` / `SDI` | `MOSI` / `MISO` |
| `PICO` / `POCI` | (for bidirectional devices) |
| `CS` | `SS` |

## Testbenches

- Name testbenches with `*_tb.sv` suffix
- Place testbenches in `gw/EconoPET/sim/`
- Each testbench is standalone and auto-discovered by CTest
- Use TB_INIT and assertion macros from `gw/EconoPET/sim/tb.svh`:
  - `` `TB_INIT `` - emits an `initial` block that sets up VCD dumping, runs the `run` task, and calls `$finish`
  - `` `assert_equal(actual, expected) `` - compare with detailed error messages
  - `` `assert_exact_equal(actual, expected) `` - strict equality check
  - `` `assert_compare(actual, op, expected) `` - comparison with operator

## Running Testbenches

```sh
./sim.sh --update    # Regenerate `gw/EconoPET/work_sim/EconoPET.f` after modifying `EconoPET.xml`
./sim.sh             # List available tests
./sim.sh --all       # Run all tests in parallel (fastest)
./sim.sh spi_tb      # Run a single test with output logged to console
```

## Adding New Files

When adding new `*.sv` or `*.v` files, they must be registered in the project:

- Source files (`src/*.sv`): Add `<efx:design_file name="src/mymodule.sv" .../>` in the `<efx:design_info>` section
- Simulation files (`sim/*.sv`): Add `<efx:sim_file name="sim/mymodule_tb.sv"/>` in the `<efx:sim_info>` section
- Run `./sim.sh --update` from the project root to regenerate `gw/EconoPET/work_sim/EconoPET.f`

**Critical:** New source files and testbenches are omitted from the build and test until registered in `EconoPET.xml` and `./sim.sh --update` is run to update the file list in `gw/EconoPET/work_sim/EconoPET.f`.

## Verification Checklist

- [ ] If the change is testable, add or update a `*_tb.sv` file in `gw/EconoPET/sim/` to add coverage
- [ ] Update `gw/EconoPET/EconoPET.xml` to register new source and test files or remove deleted files
- [ ] Run `./sim.sh --update` to regenerate `gw/EconoPET/work_sim/EconoPET.f`
- [ ] Run `./sim.sh --all` and verify all tests pass
- [ ] Only if tests pass, run `cmake --build --preset gw`

**Critical:** Never start gateware build when simulation tests are failing. The gateware build is slow (~2 min) and simulation catches most issues.

## External Dependencies

The `/gw/EconoPET/external` directory contains git submodules. Do not modify files in this path without explicit confirmation.
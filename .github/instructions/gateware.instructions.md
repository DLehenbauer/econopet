---
description: 'SystemVerilog coding conventions and patterns for FPGA gateware development'
applyTo: 'gw/**/*.sv, gw/**/*.v'
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

## Testbenches

- Name testbenches with `*_tb.sv` suffix
- Place testbenches in [gw/EconoPET/sim/](gw/EconoPET/sim/)

## Adding New Files

When adding new `*.sv` or `*.v` files, they must be registered in the project:

1. **Edit [EconoPET.xml](gw/EconoPET/EconoPET.xml)** - Add the file to the appropriate section:
   - Source files (`src/*.sv`): Add `<efx:design_file name="src/mymodule.sv" .../>` in the `<efx:design_info>` section
   - Simulation files (`sim/*.sv`): Add `<efx:sim_file name="sim/mymodule_tb.sv"/>` in the `<efx:sim_info>` section

2. **Regenerate EconoPET.f** - Run `sim.sh --update` from the project root to regenerate the file list used by Icarus Verilog.

## Development Workflow

1. `ctest --preset gw` - run Verilog simulations (Icarus Verilog)
2. Only if tests pass: `cmake --build --preset gw` - build bitstream

**Critical:** Never start gateware build when simulation tests are failing. The gateware build is slow (~2 min) and simulation catches most issues.

## Verification Checklist

Complete these steps in order for all gateware changes:

1. **Compile check** - Ensure changes compile without errors using Icarus Verilog (happens automatically when running simulations).

2. **Create or update testbench** - If the change is testable, add or update a `*_tb.sv` file in [gw/EconoPET/sim/](gw/EconoPET/sim/). Use existing assertion macros from [assert.svh](gw/EconoPET/sim/assert.svh):
   - `` `assert_equal(actual, expected) `` - compare with detailed error messages
   - `` `assert_exact_equal(actual, expected) `` - strict equality check
   - `` `assert_compare(actual, op, expected) `` - comparison with operator

3. **Register new testbenches** - Add new testbenches to [sim.sv](gw/EconoPET/sim/sim.sv):
   - Add a `` `define TEST_MYMODULE `` flag near the top
   - Add a conditional instantiation block: `` `ifdef TEST_MYMODULE ... `endif ``
   - Add a call to the testbench's `run` task in the `initial` block

4. **Run simulations** - Execute `ctest --preset gw` and verify all tests pass.

5. **Build bitstream** (last step) - Only after all simulations pass, run `cmake --build --preset gw`. This step takes ~2 minutes.

### When a testbench may be skipped

A new or updated testbench is not practical for:
- Pin assignment changes in `top.sv`
- Timing-only adjustments that require real hardware
- Pure formatting or comment changes
- Changes already covered by existing testbenches

## External Dependencies

The `/gw/EconoPET/external` directory contains git submodules. Do not modify files in this path without explicit confirmation. When changes seem to require modifying submodule code:

1. Propose alternatives first (wrappers, adapters, compile flags)
2. Request confirmation before proceeding

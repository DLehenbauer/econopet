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
- Each testbench is standalone and auto-discovered by CTest

## Running Tests

```sh
ctest --preset gw -j$(nproc)    # Run all gw tests in parallel (fastest)
ctest --preset gw               # Run all gw tests sequentially
./sim.sh spi_tb                 # Run a single test
./sim.sh -v spi_tb              # View waveform for a test
./sim.sh                        # List available tests
```

## Adding New Files

When adding new `*.sv` or `*.v` files, they must be registered in the project:

1. **Edit [EconoPET.xml](gw/EconoPET/EconoPET.xml)** - Add the file to the appropriate section:
   - Source files (`src/*.sv`): Add `<efx:design_file name="src/mymodule.sv" .../>` in the `<efx:design_info>` section
   - Simulation files (`sim/*.sv`): Add `<efx:sim_file name="sim/mymodule_tb.sv"/>` in the `<efx:sim_info>` section

2. **Regenerate EconoPET.f** - Run `sim.sh --update` from the project root to regenerate the file list used by Icarus Verilog.

## Development Workflow

1. `ctest --preset gw -j$(nproc)` - run Verilog simulations in parallel
2. Only if tests pass: `cmake --build --preset gw` - build bitstream

**Critical:** Never start gateware build when simulation tests are failing. The gateware build is slow (~2 min) and simulation catches most issues.

## Verification Checklist

Complete these steps in order for all gateware changes:

1. **Compile check** - Ensure changes compile without errors using Icarus Verilog (happens automatically when running simulations).

2. **Create or update testbench** - If the change is testable, add or update a `*_tb.sv` file in [gw/EconoPET/sim/](gw/EconoPET/sim/). Use existing assertion macros from [assert.svh](gw/EconoPET/sim/assert.svh):
   - `` `assert_equal(actual, expected) `` - compare with detailed error messages
   - `` `assert_exact_equal(actual, expected) `` - strict equality check
   - `` `assert_compare(actual, op, expected) `` - comparison with operator

3. **Make testbench standalone** - Each testbench must include an initial block:
   ```systemverilog
   initial begin
       $dumpfile({`__FILE__, ".vcd"});
       $dumpvars(0, mymodule_tb);
       run;
       $finish;
   end
   ```

4. **Run simulations** - Execute `ctest --preset gw -j$(nproc)` and verify all tests pass.

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

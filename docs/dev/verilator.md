# Running testbenches under Verilator

A standalone simulator CMake project compiles testbenches through Verilator's
`verilate()` integration. The root super-project orchestrates it without
generating its C++ targets during root configuration.
`./verilate.sh TEST_NAME [RAND_RESET]` runs a previously built testbench with
the shared seed. Verilator 5.028+.

Build generated models and the Verilator runtime with optimized
`RelWithDebInfo` flags: Verilator `-O3`, C++ `-O3 -g -DNDEBUG`, and native CPU
tuning. Disable link-time optimization to reduce CI build time.
Keep SV assertions enabled with `--assert`.
These locally built executables target the build host, not portable distribution.
Avoid unsafe math optimizations that change simulation semantics.

Compile linked test, framework, and firmware sources separately in `Debug`
(without `NDEBUG`). Do not apply generated-model flags to an entire integrated
test target. These model targets contain only generated code and the runtime.

Verilator's compiled model runs the long boot-style benches orders of
magnitude faster than iverilog, making them practical to run routinely.

## Via CTest

Quick benches are registered twice: `<name>` on Icarus (label `iv`) and
`<name>_vl` on Verilator (label `vl`). Full-boot benches are Verilator-only
and take the `boot` label.

```sh
cmake --build --preset gw-test --parallel  # compile models through one Ninja scheduler
ctest --preset gw --parallel       # quick benches under both simulators
ctest --preset gw-iv --parallel     # quick benches under Icarus only
ctest --preset gw-vl --parallel     # quick benches under Verilator only
ctest --preset gw-boot --parallel   # full-boot benches (slow)
```

Compilation and execution are separate stages. `cmake --build --preset all`
includes the simulator models, so the existing CI build prepares them before
running CTest without an additional CI step. To build only the Verilator models,
use `cmake --build --preset gw-test` before a Verilator execution preset.
It compiles every model, including boot benches. After the subproject has been
configured, build an individual model with
`cmake --build build/gw-test --target <name>_vl`.
One Ninja scheduler controls
compilation and linking across models without
nested Make builds or per-model all-core worker pools. Use
`CMAKE_BUILD_PARALLEL_LEVEL=N` to limit simulator build jobs on memory-constrained
runners. Set the environment variable when using root build presets so the
nested build inherits the limit. Root `--parallel N` limits orchestration jobs,
not the nested simulator's jobs. Other subprojects can build concurrently and
do not share the simulator's job budget.
Each executable links as soon as its own model objects are ready,
without an all-model compilation barrier or a separate link pool.
Initial Verilator generation happens during simulator subproject configuration
when its build is requested, with subsequent generation and C++ rebuilds tracked
by CMake's native dependencies. Firmware-only and bitstream-only builds do not
configure the simulator.
Compilation consumes the build stage's budget, not the CTest execution timeout.
Verilator execution never builds models and has its own
timeout (600 seconds for dual-simulator benches, 1800 for Verilator-only benches).
If the build stage fails, stop rather than running potentially stale executables.

`build/gw-test/CMakeFiles/<name>_vl_model.dir/` caches the generated model and C++ objects.
Executables are in `build/gw-test/bin/`. An unchanged rebuild does no compiler
work, while a cold build stage pays the C++ build. That build dominates the short
benches -- `wbp_mux_tb` is ~4s to compile and 2ms to simulate -- so Verilator
only wins where simulated time is large. Do not run independent model builds
concurrently or rebuild a model while it is executing.

After building, use `./verilate.sh TEST_NAME [RAND_RESET]` to run a bench directly.
The wrapper fails with build instructions if the executable is missing and
never compiles or refreshes source lists. CTest supplies the exact target path
so execution also works with a non-default CMake binary directory.

`sim.sh --update-only` atomically replaces generated simulation file lists only
when their contents change. Parallel subproject Icarus build fixtures can
therefore share those lists without observing a partially written file or
invalidating unchanged cached Icarus images.

## Reset randomization

Pass `+verilator+rand+reset+` mode as the second argument:

Verilator is two-state. The runner maps explicit X values according to
`--x-assign unique` and uses this mode to pick what an uninitialized variable
becomes:

- `0` -- zeros. This is the default and is required for the m6502 core, whose
  `handle_irq` would otherwise power up set and count a spurious IRQ.
- `1` -- all ones.
- `2` -- random.

Override individual benches with `GW_RAND_RESET_<name>` when an upstream model
requires a particular power-up state.

Mode `2` draws from the seed described in [testing.md](testing.md), which
`verilate.sh` passes as `+verilator+seed+N`.

## Known constraints

- Verilator resolves a released (`'z`) net to a defined level, so bus ownership
  cannot be inferred from the bus value. `sim/mock_sram.sv`, `sim/mock_cpu.sv`,
  and `sim/mock_bus.sv` therefore carry an explicit output-enable alongside
  each data value and assert `$onehot0` over the enables at the sampling edge.
- `sim/mock_sram.sv` must latch write data during the WE-low window: the
  WB->RAM bridge drops its data output enable on the edge WE rises, and
  Verilator's evaluation order otherwise commits a released bus.
- Unsized decimal literals wider than 32 bits are rejected
  (`#(64'd5000000000)`, not `#(5000000000)`).
- An event control on a task-set variable never schedules under `--timing`
  (see `sim/clock_gen.sv`).
- A comment line beginning with the word "Verilator" is parsed as a
  metacomment.

# Running testbenches under Verilator

`./verilate.sh TEST_NAME [RAND_RESET]` compiles a testbench with
`verilator --binary --timing` and runs it. Verilator 5.028+.

Verilator's compiled model runs the long boot-style benches orders of
magnitude faster than iverilog, making them practical to run routinely.

## Via CTest

Quick benches are registered twice: `<name>` on Icarus (label `iv`) and
`<name>_vl` on Verilator (label `vl`). Full-boot benches are Verilator-only
and take the `boot` label.

```sh
ctest --preset gw --parallel        # quick benches under both simulators
ctest --preset gw-iv --parallel     # quick benches under Icarus only
ctest --preset gw-vl --parallel     # quick benches under Verilator only
ctest --preset gw-boot --parallel   # full-boot benches (slow)
```

`work_sim/obj_<name>/` caches the compiled model, so a rerun costs almost
nothing while a cold run pays the C++ build. That build dominates the short
benches -- `wbp_mux_tb` is ~4s to compile and 2ms to simulate -- so Verilator
only wins where simulated time is large. Two concurrent `ctest` runs will
collide over those directories.

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

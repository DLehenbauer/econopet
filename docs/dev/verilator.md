# Running testbenches under Verilator

`./verilate.sh TEST_NAME [RAND_RESET]` compiles a testbench with
`verilator --binary --timing` and runs it. Verilator 5.028+.

Verilator's compiled model runs the long boot-style benches orders of
magnitude faster than iverilog, making them practical to run routinely.

## Via CTest

Every bench is registered twice: `<name>` on Icarus (label `iv`) and
`<name>_vl` on Verilator (label `vl`). Full-boot benches are Verilator-only
and take the `boot` label.

    ctest --preset gw        # Icarus, the default suite
    ctest --preset gw-vl     # same benches under Verilator
    ctest --preset gw-boot   # full-boot benches, minutes rather than seconds

`work_sim/obj_<name>/` caches the compiled model, so a rerun costs almost
nothing while a cold run pays the C++ build. That build dominates the short
benches -- `wbp_mux_tb` is ~4s to compile and 2ms to simulate -- so Verilator
only wins where simulated time is large. Two concurrent `ctest` runs will
collide over those directories.

## Reset randomization

Pass `+verilator+rand+reset+` mode as the second argument:

Verilator is two-state, so this picks what an uninitialized variable becomes in
place of X:

- `0` -- zeros. What CTest uses: every in-repo bench passes with it, including
  the mc6809 ones (`superpet_irq_tb`, `swi_vector_tb`). The m6502 core needs it
  too, or `handle_irq` powers up set and counts a spurious IRQ.
- `1` -- all ones (not random).
- `2` -- random.

The non-zero modes need work in the mocks before they are usable: under all
ones, `mock_bus` reports false data-bus contention because its control inputs
power up asserted. Override per bench with `GW_RAND_RESET_<name>`.

## Known constraints

- `sim/mock_sram.sv` must latch write data during the WE-low window: the
  WB->RAM bridge drops its data output enable on the edge WE rises, and
  Verilator's evaluation order otherwise commits a released bus.
- Unsized decimal literals wider than 32 bits are rejected
  (`#(64'd5000000000)`, not `#(5000000000)`).
- An event control on a task-set variable never schedules under `--timing`
  (see `sim/clock_gen.sv`).
- A comment line beginning with the word "Verilator" is parsed as a
  metacomment.

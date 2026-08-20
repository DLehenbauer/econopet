# Running testbenches under Verilator

`./verilate.sh TEST_NAME [RAND_RESET]` compiles a testbench with
`verilator --binary --timing` and runs it. Verilator 5.028+.

Verilator's compiled model runs the long boot-style benches orders of
magnitude faster than iverilog, making them practical to run routinely.

## Reset randomization

Pass `+verilator+rand+reset+` mode as the second argument:

Verilator is two-state. The runner maps explicit X values according to
`--x-assign unique` and uses this mode to pick what an uninitialized variable
becomes:

- `0` -- zeros. This is the default and is required for the m6502 core, whose
  `handle_irq` would otherwise power up set and count a spurious IRQ.
- `1` -- all ones.
- `2` -- random.

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

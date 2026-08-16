# Running testbenches under Verilator

`./verilate.sh TEST_NAME [RAND_RESET]` compiles a testbench with
`verilator --binary --timing` and runs it. Verilator 5.028+.

Verilator's compiled model runs the long boot-style benches orders of
magnitude faster than iverilog, making them practical to run routinely.

## Reset randomization

Pass `+verilator+rand+reset+` mode as the second argument:

- `0` (default) -- zero-initialized state; required for the m6502 core,
  whose `handle_irq` would otherwise power up set and count a spurious IRQ.
- `1` -- randomized state; required for cores that hang from all-zero
  X-resolution (e.g. the mc6809).

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

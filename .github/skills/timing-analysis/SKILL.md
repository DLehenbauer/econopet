---
name: timing-analysis
description: FPGA timing analysis and constraint management for EconoPET gateware.
  Use for any task involving SDC files, timing constraints, timing violations, setup
  or hold failures, clock definitions, I/O delay constraints, false paths, multicycle
  paths, timing reports, timing closure, or static timing analysis (STA).
---

# Timing Closure

Investigate and resolve timing failures for the EconoPET FPGA gateware using the
Efinix Efinity static timing analysis (STA) tools.

## When to Use

- Timing summary reports setup or hold violations after a bitstream build
- User asks to investigate, debug, or fix timing failures
- User asks to add or update clock or I/O constraints
- User wants to review or tighten timing margins
- User asks to review the SDC file for correctness or completeness

## Prerequisites

- The FPGA bitstream must have been built at least once (`cmake --build --preset gw`)
  so that timing data exists for analysis.

## Key Files

| File | Purpose |
|------|---------|
| [./reference/design.md](./reference/design.md) | Hardware design specification (clocks, components, timing parameters). **Read this first.** |
| [gw/EconoPET/EconoPET.sdc](gw/EconoPET/EconoPET.sdc) | Project SDC file (clock definitions, I/O delays, false paths) |
| [build/gw/outflow/EconoPET.pt.sdc](build/gw/outflow/EconoPET.pt.sdc) | Auto-generated constraint template listing every port and clock that the Interface Designer expects to be constrained |
| [build/gw/outflow/EconoPET.pt_timing.rpt](build/gw/outflow/EconoPET.pt_timing.rpt) | Timing report from the most recent gateware build (GPIO pad delays, PLL info) |

## Running the STA Shell

The STA shell is an interactive TCL REPL. Do not launch the STA shell as a
background process. Always pipe commands non-interactively:

```sh
echo '<tcl commands>; exit' \
    | .github/skills/timing-analysis/scripts/sta-repl.sh 2>&1
```

## Evidence Log

Before reading files, running commands, or editing constraints, create or
update `.cache/ai/timing-analysis.md`:

```sh
touch .cache/ai/timing-analysis.md
```

Keep a running log for the entire task. After every step, append:

- The action you took
- The evidence you gathered (file path and line numbers, exact command, report
  excerpt, or RTL/SDC snippet)
- The observation or inference supported by that evidence
- The decision you made (or the next question to answer)

Every constraint recommendation, SDC edit, and timing-closure conclusion must
be evidence based. If the log does not contain evidence that justifies a
decision, do not make that decision yet. Gather more evidence first and record
it in the log.

## Workflow

### 1. Gather Context (parallel reads and log the evidence)

Read these files in parallel before doing anything else. All are required to
make correct constraint decisions:

1. [./reference/design.md](./reference/design.md) - clocks, components, timing
   parameters, bus arbitration, signal mapping
2. [gw/EconoPET/EconoPET.sdc](gw/EconoPET/EconoPET.sdc) - current constraints
3. [build/gw/outflow/EconoPET.pt.sdc](build/gw/outflow/EconoPET.pt.sdc) -
   constraint template (port/clock completeness checklist)

Record in `.cache/ai/timing-analysis.md` what each file says that is relevant to
the current issue. Cite exact paths, line numbers, clocks, ports, and timing
assumptions.

### 2. Assess Current State and log the findings

Run `check_timing` first. It is the single most informative command, reporting
exactly how many ports are unconstrained:

```sh
echo 'check_timing; exit' \
    | .github/skills/timing-analysis/scripts/sta-repl.sh 2>&1
```

Add `-verbose` to list every violating pin. The output table shows:

| Section | What it means |
|---------|---------------|
| `no_clock` | Register pins without a clock constraint (should be 0) |
| `unconstrained_internal_endpoints` | Internal paths with no timing check (should be 0) |
| `no_input_delay` | Input ports missing `set_input_delay` or `set_false_path` |
| `no_output_delay` | Output ports missing `set_output_delay` or `set_false_path` |
| `multiple_clock` | Pins driven by multiple clocks (CDC concern) |

**Target: 0 unconstrained inputs and 0 unconstrained outputs.** Ports with
`set_false_path` count as "constrained" (reported separately in the output).

Then run timing summary and the worst setup/hold paths:

```sh
echo 'report_timing_summary; exit' \
    | .github/skills/timing-analysis/scripts/sta-repl.sh 2>&1
```

```sh
echo 'report_timing -from_clock sys_clock_i -to_clock sys_clock_i -setup; exit' \
    | .github/skills/timing-analysis/scripts/sta-repl.sh 2>&1
```

```sh
echo 'report_timing -from_clock sys_clock_i -to_clock sys_clock_i -hold; exit' \
    | .github/skills/timing-analysis/scripts/sta-repl.sh 2>&1
```

If you need more than one path, use `-npaths` and optionally `-nworst`:

```sh
echo 'report_timing -from_clock sys_clock_i -to_clock sys_clock_i -setup -npaths 10 -nworst 1; exit' \
  | .github/skills/timing-analysis/scripts/sta-repl.sh 2>&1
```

To check CDC paths between clock domains:

```sh
echo 'report_cdc -details; exit' \
    | .github/skills/timing-analysis/scripts/sta-repl.sh 2>&1
```

To list all post-synthesis ports (ports optimized away by synthesis are removed):

```sh
echo 'puts "INPUTS:"; puts [all_inputs]; puts "OUTPUTS:"; puts [all_outputs]; exit' \
    | .github/skills/timing-analysis/scripts/sta-repl.sh 2>&1
```

Log each command, the important output, and what it proves. Note: Efinity does
not support `-max_paths`. Use `-npaths` / `-nworst` instead.

### 3. Classify Ports for Constraint Assignment

Every port in the post-synthesis netlist needs a constraint. Read the RTL
(`gw/EconoPET/src/top.sv`, `main.sv`, and relevant submodules) to classify each
port. This step is critical -- the constraint type depends on the signal driver.
Record the RTL evidence for each classification decision in the log.

#### Output Port Classification

| Category | Constraint | How to identify |
|----------|------------|-----------------|
| Interface-synchronized output | Copy `set_output_delay` from `<project>.pt.sdc` | Template already includes interface and clock-network timing, often with `-reference_pin` |
| Registered (`always_ff`) | `set_output_delay` relative to the real or virtual interface clock | Driven by `always_ff @(posedge <clock>)` |
| Combinational from a clocked domain | `set_output_delay` relative to the real or virtual interface clock | `assign` or `always_comb` fed by registers in one domain |
| True multicycle synchronous path | `set_multicycle_path` | Same logical clock relationship, but capture intentionally occurs N cycles later |
| Multi-cycle external protocol with no meaningful STA launch/capture clock | `set_false_path` only with RTL/protocol proof | Example: externally observed OE or bus phase signal that is not sampled on the same STA clock edge |
| Constant or tied off | `set_false_path` | `assign port = 0` or `assign port = 1` |
| Different clock domain | `set_output_delay -clock <other_clock>` | Driven by registers in another clock domain |
| Debug / unused | `set_false_path` | Not connected to functional external devices |

#### Input Port Classification

| Category | Constraint | How to identify |
|----------|------------|-----------------|
| Interface-synchronized input | Copy `set_input_delay` from `<project>.pt.sdc` | Template already includes interface and clock-network timing, often with `-reference_pin` |
| Source-synchronous (e.g., SPI SDI) | `set_input_delay -clock <source_clock>` | Sampled on external clock edge |
| True multicycle synchronous path | `set_multicycle_path` | Same logical clock relationship, capture intentionally occurs N cycles later |
| Bus-protocol synchronized with no meaningful STA clock relation | `set_false_path` only with RTL/protocol proof | Sampled during FPGA-controlled slots where ordinary edge-based STA does not model the protocol |
| Async / quasi-static | `set_false_path` | DIP switches, open-drain signals, async external signals |
| CDC with synchronizer | `set_false_path` from the port | Crossed via 2-FF synchronizer into sys_clock_i domain |
| Unused / optimized away | No constraint needed | Removed by synthesis (not in `all_inputs` output) |

#### Efinix Tristate OE Signals

Efinix FPGAs split tristate I/O into `_o` (data), `_oe` (output enable), and
`_i` (input) ports. OE signals frequently have long combinational paths through
address decoding gated by `cpu_be_o`. Because the bus arbiter toggles BE
multiple `sys_clock_i` cycles before data transitions, OE changes have a
multi-cycle budget. Use `set_false_path -to [get_ports {*_oe[*]}]` for these.

**Trap:** Constraining OE signals with `set_output_delay` causes setup
violations on paths like `cpu_be_o -> address_decode -> cpu_addr_oe[*]` because
the combinational depth exceeds one `sys_clock_i` period. This does not indicate
a real timing problem.

Do not generalize this to all synchronous outputs. Prefer `set_multicycle_path`
for genuinely synchronous multi-cycle behavior. Use `set_false_path` only when
the path should not be analyzed at all.

### 4. Cross-Reference with the Constraint Template

Read [build/gw/outflow/EconoPET.pt.sdc](build/gw/outflow/EconoPET.pt.sdc).
It is regenerated each time the gateware is compiled and contains a commented-out
`set_input_delay` or `set_output_delay` stub for every port/clock pair that the
Interface Designer expects to be constrained. It also contains `create_clock`
statements for PLL and pad-entry clocks.

Use it as a checklist:

- Verify that every `create_clock` in the template has a corresponding definition
  (or an intentional omission with a comment) in the project SDC.
- Verify that every port listed in the template is covered by a `set_input_delay`,
  `set_output_delay`, or `set_false_path` in the project SDC. Unconstrained ports
  are left with default timing and may cause silent failures.
- Watch for new ports that appear after Interface Designer changes (they show up
  in the template but may be missing from the project SDC).
- Ports that synthesis optimizes away will not appear in the post-synthesis
  netlist, but keeping constraints for them in the SDC is harmless.

### 5. Edit the SDC File

Edit [gw/EconoPET/EconoPET.sdc](gw/EconoPET/EconoPET.sdc). After editing, you
can re-read the SDC to check for syntax errors and re-analyze timing against the
existing netlist:

```sh
echo 'reset_timing; delete_timing_results; read_sdc; report_timing_summary; exit' \
    | .github/skills/timing-analysis/scripts/sta-repl.sh 2>&1
```

This validates the constraints parse correctly and gives a first look at timing
impact, but the netlist has not been re-optimized for the new constraints. Setup
violations against the old netlist may resolve after a full rebuild (step 6).

Before each SDC edit, write the proposed change in the log, cite the supporting
evidence, and explain why the chosen constraint is the correct model. If you
reject an alternative (`set_false_path`, `set_multicycle_path`, different clock,
etc.), record why.

If a setup violation appears on an output path that was previously unconstrained,
check whether the path has a multi-cycle budget. If so, use `set_false_path`
instead of `set_output_delay`.

### 6. Rebuild and Verify

Constraint changes that affect which paths are optimized (adding/removing output
delays, false paths, clock definitions) require a full rebuild so that synthesis
and place-and-route can optimize for the new constraints:

```sh
cmake --build --preset gw
```

After the rebuild completes, re-run the STA shell to confirm timing is clean:

```sh
echo 'report_timing_summary; exit' \
    | .github/skills/timing-analysis/scripts/sta-repl.sh 2>&1
```

Then run `check_timing` again to verify 0 unconstrained ports remain. Append the
final verification results, residual risks, and the final decision summary to
`.cache/ai/timing-analysis.md`.

## SDC Conventions

### Constraint Order

Order constraints exactly as follows (dependencies require this sequence):

1. Primary clocks (`create_clock`)
2. Virtual clocks (`create_clock` without a target)
3. Generated clocks (`create_generated_clock`)
4. Clock groups (`set_clock_groups`)
5. Input and output delays (`set_input_delay`, `set_output_delay`)
6. Timing exceptions in this order:
   a. False paths (`set_false_path`)
   b. Max/min delays (`set_max_delay`, `set_min_delay`)
   c. Multicycle paths (`set_multicycle_path`)

Always define a clock before referencing it in any I/O delay or exception
constraint. A constraint referencing an undefined clock is silently ignored.

### Exception Priority

When the same path has multiple exceptions, Efinity applies them in this order
(highest priority first):

1. `set_clock_groups`
2. `set_false_path`
3. `set_max_delay` / `set_min_delay`
4. `set_multicycle_path`

### SDC Syntax Rules

- SDC is **case sensitive**. Use lowercase net names (synthesis converts all
  names to lowercase).
- `#` starts a comment.
- `\` at end of line continues the command on the next line.
- `*` is a wildcard (e.g., `Oled*` matches `Oled0`, `Oled_en`).
- Use `get_ports`, `get_pins`, `get_cells`, `get_nets` object specifiers.
  The pipe `|` separates instance name from port name (e.g., `FF1|Q`).
- Perl regex is available with `-regexp` option on object specifiers. Escape
  brackets when using regex.

### Virtual Clocks and Real Clocks

Use a virtual clock as the `set_input_delay`/`set_output_delay` reference when
the board clock is off-chip. This removes interface clock latency and uncertainty
from I/O constraints. Put the virtual clock and core clock in the same clock
group so the timer analyzes transfers between them.

Use a real pad clock when the relationship is to the FPGA pad clock. Use
`-reference_pin` for synchronized or forwarded-clock interfaces.

### Input and Output Delay Values

Default rule:

1. If the interface is synchronized by Interface Designer, copy the
   `set_input_delay` / `set_output_delay` constraints from
   `<project>.pt.sdc`. Keep `-reference_pin` when present.
2. If the interface is not synchronized (bypass mode), calculate delays from the
   external device timing, board delay, and GPIO timing values in
   `<project>.pt_timing.rpt`.
3. Only use simplified board-delay-only constraints when the design's RTL and
   protocol prove that ordinary edge-based STA is not the right model.

#### Delay Equations

Synchronized I/O (copy from `<project>.pt.sdc`):

| Direction | Max | Min |
|-----------|-----|-----|
| Input | `DDATA_max + tCO + DCLK_INTERFACE_max` | `DDATA_min + tCO + DCLK_INTERFACE_min` |
| Output | `DDATA_max + tSETUP - DCLK_INTERFACE_max` | `DDATA_min - tHOLD - DCLK_INTERFACE_min` |

**Negative minimum output delay is valid.** Do not clamp to zero.

Unsynchronized I/O (bypass mode, receive clock):

| Direction | Max | Min |
|-----------|-----|-----|
| Input | `board_max + GPIO_IN_max` | `board_min + GPIO_IN_min` |
| Output | `board_max + GPIO_OUT_max` | `board_min + GPIO_OUT_min` |

For forward-clock I/O, add `-reference_pin` and include `GPIO_CLK_OUT`. See
the Efinity Timing Closure User Guide for the four forward-clock modes.
`GPIO_CLK_IN` delay is in `set_clock_latency`, not in I/O delay constraints.

#### EconoPET-Specific Exception

Some CPU-bus-facing signals use false paths or simplified delays because the bus
arbiter in `timing.sv` creates a protocol-level multi-cycle budget. Justify from
[./reference/design.md](./reference/design.md) and RTL, not as a general rule.

### Clock Definitions

Match the template form with explicit `-name` and `[get_ports]`:

```tcl
create_clock -period $period -name sys_clock_i [get_ports {sys_clock_i}]
```

Common mistake: using `-name` without a target creates a virtual clock. That is
correct only when you intentionally want an off-chip reference clock. Efinity
prints an info message when it finds a virtual clock definition.

Use `-add` to define multiple clocks on the same target (e.g., dynamic clock
muxes). Without `-add`, a later `create_clock` on the same target overwrites
the previous one.

### set_clock_groups

```tcl
# Explicit all-groups form (preferred for this project)
set_clock_groups -asynchronous -group {sys_clock_i} -group {spi0_sck_i}

# Single-group form: cuts this group to all other clocks
set_clock_groups -asynchronous -group {spi0_sck_i}
```

Default (no flag) is `-exclusive`. Use `-asynchronous` for independent clock
sources. Use `-exclusive` for clocks that never operate simultaneously (e.g.,
dynamic mux). Prefer the all-groups form: it documents known relationships and
requires explicit updates when adding clocks.

### Multicycle Paths

Default single-cycle: `setup = 1, hold = 0`. To shift by N cycles with width M:
`setup = N`, `hold = M - 1`.

For different clock frequencies, use `-start`/`-end` to select the reference:

- **Launch faster than capture**: `-start` for setup (hold defaults to `-start`)
- **Launch slower than capture**: `-end` for hold (setup defaults to `-end`)

Do not use both `-start` and `-end` on the same constraint.

## Common Fix Patterns

### Setup Violations

| Cause | Fix |
|-------|-----|
| Long combinational output path (e.g., OE through address decode) | Use `set_false_path` only if the path should not be timed. Otherwise use `set_multicycle_path` or change RTL |
| Genuinely tight internal path | Add pipeline register in RTL |
| Overly tight output delay | Recalculate from interface timing, external setup/hold, board delay, and `pt_timing.rpt` |
| Path between unrelated clocks | `set_clock_groups` or `set_false_path` |

### Hold Violations

| Cause | Fix |
|-------|-----|
| Short registered path (e.g., FF to BRAM port) | P&R placement issue, not SDC -- try seed sweep or pipeline register |
| Negative min output delay | Recalculate it. Negative min can be correct for synchronized outputs |
| Missing clock group declaration | Add `set_clock_groups -asynchronous` |

### Hold Slack on Internal Paths

Internal register-to-register hold failures are P&R placement issues, not SDC
problems. Options: seed sweep, `set_min_delay` (sparingly), or pipeline register.

**Warning:** `set_min_delay` and `set_max_delay` override clock-derived timing.
They can mask real violations -- the software honors your input without error,
but the issue appears on the board. Avoid unless no alternative exists.

### Common SDC Mistakes

| Mistake | Symptom | Fix |
|---------|---------|-----|
| Latch inferred from incomplete `if`/`case` | Combinational loop warning, timing graph cut at arbitrary point | Complete all assignments, use `default` in `case` |
| Unintended virtual clock (`-name` without target) | Info message about virtual clock, I/O paths not analyzed | Add `[get_ports {...}]` target |
| Undefined clock | Unconstrained registers, unoptimized logic | Define all clocks, check `check_timing` for `no_clock` entries |
| Wrong constraint order | Silently ignored constraints, unexpected timing | Follow the ordering in [Constraint Order](#constraint-order) |

## Timing Closure Strategies

When constraints are correct but timing still fails, try these approaches in
order:

1. **Seed sweep**: Re-run P&R with different seeds. fMAX varies 10-20% between
   seeds. Try 5-10 seeds before concluding a design change is needed.
2. **Optimization level**: Set `--optimization_level` in the P&R settings.
   Options: `TIMING_1` through `TIMING_3` (non-congested designs),
   `CONGESTION_1` through `CONGESTION_3` (congested designs).
3. **Placer effort**: Increase `--placer_effort_level` (1-5, default 2). Higher
   values increase runtime but may improve placement quality.
4. **Synthesis options**: Try `--retiming 2` (advanced retiming),
   `--fanout-limit N` (duplicate high-fanout nets), `--mode speed` (default).
5. **RTL changes**: Add pipeline registers on critical paths, reduce
   combinational depth, split wide muxes.

## Efinix-Specific Notes

### Interface Designer and Core-Level Constraints

Efinix FPGAs apply timing constraints at the **core level**, not package pins.
The Interface Designer generates `<project>.pt.sdc` (regenerated each build).

- Copy clock and I/O constraints from the template into the project SDC.
- Do **not** add `<project>.pt.sdc` to the project SDC list.
- Preserve negative `set_clock_latency` values for PLLs from the template.

### Clock Latency

`set_clock_latency` captures clock source-to-FPGA delay. The `<project>.pt.sdc`
provides templates. Formula: `max = board_max + GPIO_CLK_IN_max - PLL_comp_max`,
`min = board_min + GPIO_CLK_IN_min - PLL_comp_min`. PLL compensation is 0 in
Local Feedback mode. Negative latency values from Core Feedback mode are correct.

### Attributes

| Attribute | Syntax | Purpose |
|-----------|--------|---------|
| Synchronizer | `(* async_reg = "true" *)` | Prevent optimization of synchronization registers and keep them physically close |
| Preserve net | `(* syn_keep = "true" *)` | Prevent synthesis from removing nets needed for probing or manual constraints |
| Preserve signal | `(* syn_preserve = "true" *)` | Keep signal through synthesis optimization (does not prevent downstream optimization) |

### Non-Expandable Clocks (The 0.001 ns Trap)

The timer defaults to **0.001 ns** when it cannot find a common period within
1000 cycles. Use `set_clock_groups` or `set_max_delay` to override.

### Optimized-Away Ports

Synthesis removes unconnected ports. They will not appear in `all_inputs` /
`all_outputs`. Keeping SDC constraints for them is harmless.

## STA Shell Command Reference

### Report Commands

| Command | Description |
|---------|-------------|
| `check_timing` | Check for unconstrained ports/clocks. Options: `-verbose` (list pins), `-file <f>`, `-exclude {checks}`. **Run first.** |
| `report_timing_summary` | Critical path report. Options: `-file <f>`, `-setup`, `-hold` |
| `report_timing` | Slack paths. Options: `-from_clock`, `-to_clock`, `-from`, `-to`, `-through`, `-setup`/`-hold`, `-npaths N`, `-nworst N`, `-detail {summary\|path_only\|path_and_clock\|full_path}`, `-less_than_slack <val>`, `-show_routing` |
| `report_path` | Propagation delay. Options: `-from`, `-to`, `-through`, `-npaths N`, `-min_path`, `-show_routing` |
| `report_clocks` | List all defined clocks |
| `report_cdc` | CDC paths. Options: `-from <clocks>`, `-to <clocks>`, `-details` (shows synchronizer checks, missing `async_reg`) |
| `report_bus_skew` | Bus skew for `set_bus_skew` constraints. Options: `-npaths N`, `-nworst N` |

### Session Commands

| Command | Description |
|---------|-------------|
| `reset_timing` | Clear all timing data, constraints, and reported paths |
| `delete_timing_results` | Delete reported results only (keep constraints) |
| `read_sdc` | Read SDC file (no arg = project SDC list) |
| `write_sdc <file>` | Write constraints in memory to file |
| `set_timing_model` / `get_timing_model` / `get_available_timing_model` | Set or query operating conditions |

### SDC Constraint Commands

| Command | Key Options |
|---------|-------------|
| `create_clock` | `-period`, `-name`, `-waveform {rise fall}`, `[get_ports]`, `-add` |
| `create_generated_clock` | `-source`, `-divide_by`/`-multiply_by`, `-phase`, `-offset`, `-edges`/`-edge_shift`, `-invert` |
| `set_clock_groups` | `-exclusive`/`-asynchronous` (default: `-exclusive`), `-group {clocks}` |
| `set_clock_uncertainty` | `-setup`/`-hold`, `-from`/`-to` (adds to default, does not replace) |
| `set_clock_latency` | `-source` (required), `-setup`/`-hold`, `-rise`/`-fall` |
| `set_false_path` | `-from`, `-to`, `-through`, `-setup`/`-hold` |
| `set_input_delay` | `-clock`, `-max`/`-min`, `-reference_pin`, `-clock_fall`, `-add_delay` |
| `set_output_delay` | `-clock`, `-max`/`-min`, `-reference_pin`, `-clock_fall`, `-add_delay` |
| `set_max_delay` / `set_min_delay` | `-from`, `-to`, `-through` (**risky: can mask real violations**) |
| `set_multicycle_path` | `-setup`/`-hold`, `-start`/`-end`, `-from`, `-to`, `-through` |
| `set_bus_skew` | `-from`, `-to`, `-through` |

### Query Commands

| Command | Description |
|---------|-------------|
| `all_clocks`, `all_inputs`, `all_outputs`, `all_registers` | Return all objects of the given type |
| `get_cells`, `get_clocks`, `get_nets`, `get_pins`, `get_ports` | Return objects matching a pattern. Pin format: `cell\|port` |
| `get_fanouts`, `get_fanins` | Trace fanout/fanin from a start point. Options: `-no_logic`, `-through` |

## Additional Resources
- [Efinix Efinity Timing Closure User Guide](https://www.efinixinc.com/docs/efinity-timing-closure-v7.4.pdf)

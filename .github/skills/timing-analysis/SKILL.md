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

## Prerequisites

- The FPGA bitstream must have been built at least once (`cmake --build --preset gw`)
  so that timing data exists for analysis.

## Key Files

| File | Purpose |
|------|---------|
| [./reference/design.md](./reference/design.md) | Hardware design specification (clocks, components, timing parameters) |
| [gw/EconoPET/EconoPET.sdc](gw/EconoPET/EconoPET.sdc) | Project SDC file (clock definitions, I/O delays, false paths) |
| [gw/EconoPET/outflow/](gw/EconoPET/outflow/) | Build outputs including generated timing constraints |

## Running the STA Shell

Always use the launcher script. Never construct the Efinity command manually:

```sh
./scripts/sta-repl.sh
```

The STA shell is an interactive TCL REPL. It is not a bash shell. To pipe
commands non-interactively:

```sh
echo '<tcl commands>; exit' | ./scripts/sta-repl.sh 2>&1
```

Do not launch the STA shell as a background process. It is an interactive REPL
that cannot receive commands after startup when run in the background. Always
pipe commands via stdin as shown above or run it in a foreground terminal.

## Workflow

### 1. Investigate Timing (read-only)

The STA shell can re-read the SDC and re-analyze timing against an existing
build without rebuilding. This is useful for reading reports, but constraint
changes that affect synthesis optimization require a full rebuild (see step 3).

```sh
echo 'report_timing_summary; exit' \
    | ./scripts/sta-repl.sh 2>&1
```

```sh
echo 'report_timing -from_clock sys_clock_i -to_clock sys_clock_i -setup; exit' \
    | ./scripts/sta-repl.sh 2>&1
```

```sh
echo 'report_timing -from_clock sys_clock_i -to_clock sys_clock_i -hold; exit' \
    | ./scripts/sta-repl.sh 2>&1
```

Note: Efinity does not support `-max_paths`. Each `report_timing` call returns
the single worst path for the given clock relationship.

### 2. Edit the SDC File

Edit [gw/EconoPET/EconoPET.sdc](gw/EconoPET/EconoPET.sdc). After editing, you
can re-read the SDC to check for syntax errors and re-analyze timing against the
existing netlist:

```sh
echo 'reset_timing; delete_timing_results; read_sdc; report_timing_summary; exit' \
    | ./scripts/sta-repl.sh 2>&1
```

This validates the constraints parse correctly and gives a first look at timing
impact, but the netlist has not been re-optimized for the new constraints.

### 3. Rebuild and Verify

Constraint changes that affect which paths are optimized (adding/removing output
delays, false paths, clock definitions) require a full rebuild so that synthesis
and place-and-route can optimize for the new constraints:

```sh
cmake --build --preset gw
```

After the rebuild completes, re-run the STA shell to confirm timing is clean:

```sh
echo 'report_timing_summary; exit' \
    | ./scripts/sta-repl.sh 2>&1
```

Common setup fixes:

- Add pipeline registers to break long combinational paths
- Relax unnecessary output delay constraints
- Use `set_multicycle_path` for paths that have multiple clock cycles to settle

Common hold fixes:

- Add `set_min_delay` constraints
- Adjust `-min` values on `set_output_delay` / `set_input_delay`
- Verify that clock groups are correctly declared for asynchronous domains

## Efinix-Specific Workflows

### Interface Designer & Core-Level Constraints

Efinix FPGAs (Trion, Titanium) apply timing constraints at the **core level**, not at
package pins. The Interface Designer configures I/O blocks and generates a constraint
template file: `<project>.pt.sdc`.

- Copy clock and I/O constraints from `<project>.pt.sdc` into the project SDC
- Do not constrain package pins directly unless using virtual clocks
- Preserve negative `set_clock_latency` values for PLLs (e.g., `-3.5 ns`) from
  `<project>.pt.sdc`. Only modify these when adding board-level delays in External
  Feedback mode.

### Attributes

| Attribute | Syntax | Purpose |
|-----------|--------|---------|
| Synchronizer | `(* async_reg = "true" *)` | Prevent optimization of synchronization registers and keep them physically close |
| Preserve net | `(* syn_keep = "true" *)` | Prevent synthesis from removing nets needed for probing or manual constraints |

### Non-Expandable Clocks (The 0.001 ns Trap)

The timer declares clocks "non-expandable" when it cannot find a common period within
1000 cycles, defaulting to an impossible **0.001 ns** requirement. Use
`set_clock_groups -exclusive` or `set_max_delay` to override this for unrelated or
rationally unrelated clocks.

### Tristate OE Signals

Efinix FPGAs use separate `_o` (data) and `_oe` (output enable) signals for
tristate I/O pads. The OE signals control the FPGA pad direction.

### Optimized-Away Ports

Synthesis removes input ports that are unconnected in the RTL.

### set_clock_groups

Always list all groups explicitly. With only one `-group` argument, meaning is
tool-dependent:

```tcl
# Correct: both groups listed
set_clock_groups -asynchronous -group {sys_clock_i} -group {spi0_sck_i}

# Ambiguous: only one group
set_clock_groups -asynchronous -group {spi0_sck_i}
```

## STA Shell Command Reference

### Timing Commands

| Command | Description |
|---------|-------------|
| `delete_timing_results` | Delete all the reported timing and path results from memory |
| `get_available_timing_model` | Returns the list of available timing model for the current device |
| `get_timing_model` | Returns the current timing model being set |
| `read_sdc` | Read SDC file |
| `report_clocks` | Generate clock report |
| `report_path` | Generate propagation delay path report |
| `report_timing` | Generate slack path report |
| `report_timing_summary` | Run timing analysis and generate the critical path report |
| `check_timing` | Run timing checks |
| `report_cdc` | Generate clock domain crossing (CDC) paths report |
| `reset_timing` | Clear timing data from memory |
| `set_timing_model` | Specify timing model for the current device |
| `write_sdc` | Write constraints in memory into SDC file |

### SDC Commands

| Command | Description |
|---------|-------------|
| `all_clocks` | Retrieve the name of all clocks in the design |
| `all_inputs` | Retrieve the name of all input ports in the design |
| `all_registers` | Retrieve the name of all register instances in the design |
| `all_outputs` | Retrieve the name of all output ports in the design |
| `create_clock` | Specify a clock constraint |
| `create_generated_clock` | Specify a generated clock constraint |
| `get_cells` | Retrieve the list of instances in the design |
| `get_clocks` | Retrieve the list of clocks in the design |
| `get_fanouts` | Retrieve the list of ports and registers in fanout of specified sources |
| `get_nets` | Retrieve the list of nets in the design |
| `get_pins` | Retrieve the list of pins in the design |
| `get_ports` | Retrieve the list of ports in the design |
| `set_clock_groups` | Create a group of clocks |
| `set_clock_uncertainty` | Set clock uncertainty between clocks |
| `set_clock_latency` | Set clock latency to clocks |
| `set_false_path` | Set false path between clocks |
| `set_input_delay` | Set delay to input ports |
| `set_max_delay` | Set maximum delay between clock domain |
| `set_min_delay` | Set minimum delay between clock domain |
| `set_multicycle_path` | Set multicycle path in the design |
| `set_output_delay` | Set delay to output ports |

## Additional Resources

* [Efinix Efinity Timing Closure User Guide](https://www.efinixinc.com/docs/efinity-timing-closure-v7.4.pdf)
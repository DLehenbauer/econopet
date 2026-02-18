# Hybrid Software/Hardware Breakpoints

## Overview

Unlimited breakpoints using a hybrid approach: the MCU patches SRAM with the
W65C02S STP opcode ($DB) at each breakpoint address, and the FPGA detects
opcode fetches of STP to halt the CPU before it executes the instruction.

```
1. MCU writes $DB (STP) at each breakpoint address in SRAM.
2. CPU fetches $DB during an opcode fetch (SYNC=1).
3. FPGA detects $DB on the data bus while SYNC is high.
4. FPGA deasserts RDY before the falling PHI2 edge (within tDSR).
5. FPGA sets a status bit and latches the CPU address.
6. MCU polls the status bit via SPI, sees breakpoint hit.
7. MCU restores the original instruction and clears the halt.
8. FPGA reasserts RDY. CPU refetches (now the original instruction) and resumes.
```

## Rationale

- **Unlimited breakpoints.** Every breakpoint is a patched byte in SRAM, so
  the only limit is the number of addressable locations.
- **No hardware comparators.** The FPGA does not need address-match registers
  for each breakpoint. It only watches for $DB during opcode fetches.
- **Transparent to the CPU.** The CPU never actually executes STP. RDY is
  deasserted before the falling PHI2 edge that would latch the opcode, so the
  CPU stretches the read cycle and re-reads when RDY is reasserted.

## Components

### 1. Gateware: STP Detection Module (`breakpoint.sv`)

A new module that monitors opcode fetches and halts the CPU when it is about
to execute a STP instruction.

**Inputs:**

| Signal             | Source      | Description                                        |
|--------------------|-------------|----------------------------------------------------|
| `sys_clock_i`      | PLL         | 64 MHz system clock                                |
| `cpu_sync_i`       | W65C02S     | High during opcode fetch (T1 cycle)                |
| `cpu_data_i[7:0]`  | Data bus    | Instruction byte fetched by the CPU                |
| `cpu_data_strobe_i`| timing.sv   | One-cycle pulse when data bus is valid              |
| `cpu_be_i`         | timing.sv   | Bus enable (high when CPU owns the bus)             |
| `cpu_ready_i`      | register_file | MCU-controlled RDY (from REG_CPU)                |
| `clear_i`          | register_file | MCU writes to clear the breakpoint halt           |

**Outputs:**

| Signal                  | Destination       | Description                                    |
|-------------------------|-------------------|------------------------------------------------|
| `cpu_ready_o`           | top.sv (RDY pin)  | Gated RDY: deasserted when breakpoint is hit   |
| `halted_o`              | register_file     | Status bit: 1 = CPU halted on a breakpoint     |
| `addr_o[15:0]`          | register_file     | Latched CPU address at the breakpoint          |

**Logic:**

```systemverilog
localparam STP_OPCODE = 8'hDB;

logic halted = 1'b0;
logic [CPU_ADDR_WIDTH-1:0] bp_addr;

always_ff @(posedge sys_clock_i) begin
    if (clear_i) begin
        halted <= 1'b0;
    end else if (cpu_data_strobe_i && cpu_sync_i && cpu_data_i == STP_OPCODE) begin
        halted  <= 1'b1;
        bp_addr <= cpu_addr_i;
    end
end

assign cpu_ready_o = cpu_ready_i && !halted;
assign halted_o    = halted;
assign addr_o      = bp_addr;
```

### 2. Gateware: Register File Changes

Extend the register file to expose breakpoint status and the captured address.

**New/changed registers:**

| Register        | Addr      | Bits  | R/W | Description                              |
|-----------------|-----------|-------|-----|------------------------------------------|
| `REG_STATUS`    | `0x40000` | `[3]` | R   | Breakpoint hit (1 = halted on STP)       |
| `REG_BP_ADDR_LO`| `0x40003`| `[7:0]`| R  | Low byte of breakpoint address           |
| `REG_BP_ADDR_HI`| `0x40004`| `[7:0]`| R  | High byte of breakpoint address          |

**`REG_STATUS` bit layout (updated):**

| Bit | Name          | Description                                      |
|-----|---------------|--------------------------------------------------|
|  0  | `GRAPHICS`    | VIA CA2 (0 = graphics, 1 = text)                 |
|  1  | `CRT`         | Display type (0 = 12"/CRTC, 1 = 9"/non-CRTC)    |
|  2  | `KEYBOARD`    | Keyboard type (0 = business, 1 = graphics)       |
|  3  | `BP_HALT`     | Breakpoint halt (1 = CPU halted on STP fetch)    |

**Clearing the breakpoint halt:**

The MCU writes to REG_CPU with `ready=1` while `BP_HALT` is set. The register
file detects this transition and asserts `clear_o` for one cycle to the
breakpoint module. This avoids adding a dedicated clear register.

Alternative: add a `REG_BP_CTL` register where writing bit 0 clears the halt.
This is cleaner if the MCU needs to resume with `ready=0` (unlikely but
possible for single-stepping).

**Recommended approach:** Add `REG_BP_CTL` as a write-only register at
`0x40003`, sharing the address with `REG_BP_ADDR_LO` (the two are
distinguished by write vs. read). Writing bit 0 clears the halt. This keeps
the clear mechanism independent from the ready bit.

_Decision needed: Which clearing mechanism do we prefer?_

### 3. Gateware: RDY Gating in `main.sv`

Currently `cpu_ready_o` from `register_file` is connected directly to the
CPU's RDY pin. With the breakpoint module, it becomes:

```
register_file.cpu_ready_o ──┐
                             ├── breakpoint.cpu_ready_o ──> top.sv RDY pin
breakpoint.halted ──────────┘
```

The breakpoint module gates the register file's `cpu_ready_o` with `!halted`.

### 4. Timing Analysis

**Critical path: STP detection to RDY deassert.**

The FPGA generates PHI2 from `timing.sv`. Key timing events (in sys_clock
cycles from `CPU_BE_START`):

| Event               | Expression             | Description                    |
|---------------------|------------------------|--------------------------------|
| `CPU_DATA_STROBE`   | `CPU_PHI_END - 2`      | Data bus is valid (latch)      |
| `CPU_PHI_END`       | `CPU_BE_END - tDHx`    | Falling edge of PHI2           |

Between `CPU_DATA_STROBE` and `CPU_PHI_END` there are exactly **2 sys_clock
cycles** (2 x 15.625ns = 31.25ns at 64 MHz).

The detection and deassert sequence:

| Cycle offset | Event                                                     |
|--------------|-----------------------------------------------------------|
| +0           | `cpu_data_strobe` fires. `cpu_data_i` == $DB, `cpu_sync_i` == 1 detected. |
| +1           | `halted` register goes high (posedge sys_clock). RDY pin deasserts. |
| +2           | PHI2 falls (`CPU_PHI_END`).                               |

RDY is stable low for **1 sys_clock cycle (15.625ns)** before the falling PHI2
edge. The W65C02S requires tDSR >= 15ns setup time for RDY before the falling
PHI2 edge.

**15.625ns >= 15ns** -- the timing requirement is met, but with only 0.625ns
of margin (before accounting for routing delays within the FPGA).

**Mitigation options (if margin is insufficient):**

1. **Move `CPU_DATA_STROBE` one cycle earlier** (to `CPU_PHI_END - 3`). This
   gives 2 full cycles (31.25ns) of margin but reduces the data setup window
   for normal reads. Check that tDSR for RAM data is still met.

2. **Combinational RDY gating.** Instead of registering `halted`, use:
   ```systemverilog
   assign cpu_ready_o = cpu_ready_i && !(cpu_data_strobe_i && cpu_sync_i && cpu_data_i == STP_OPCODE) && !halted;
   ```
   This deasserts RDY in the same cycle as detection (0 register delay),
   giving the full 2 cycles (31.25ns). However, combinational paths through
   `cpu_data_i` to `cpu_ready_o` may cause glitches. Only consider if the
   registered approach fails timing.

3. **Sample data one cycle before `cpu_data_strobe`.** If RAM data settles
   earlier than the worst-case tAA budget, we can add an earlier detection
   strobe. This requires verifying actual data valid timing.

_Decision needed: Is 0.625ns of margin acceptable given FPGA internal routing,
or should we adopt mitigation option 1?_

### 5. Firmware: Breakpoint Manager

New module (`fw/src/breakpoint.c` / `breakpoint.h`) managing the breakpoint
table and SPI interactions.

**Data structures:**

```c
typedef struct breakpoint_s {
    uint16_t addr;          // PET address ($0000-$FFFF)
    uint8_t  original;      // Original instruction byte
    bool     active;        // Currently patched with STP?
} breakpoint_t;

#define MAX_BREAKPOINTS 64  // Arbitrary limit for the table (not a hardware limit)

static breakpoint_t breakpoints[MAX_BREAKPOINTS];
static int breakpoint_count = 0;
```

**Operations:**

| Function | Description |
|----------|-------------|
| `bp_set(addr)` | Read original byte, save it, write $DB to addr |
| `bp_clear(addr)` | Restore original byte, remove from table |
| `bp_poll()` | Read REG_STATUS, return true if BP_HALT set |
| `bp_hit_addr()` | Read REG_BP_ADDR_HI/LO, return 16-bit address |
| `bp_resume()` | Restore original byte, clear halt, (optionally re-arm) |

**Poll loop integration:**

The breakpoint poll should run in the MCU's main loop alongside other periodic
tasks (keyboard scan, video sync, etc.). When a breakpoint is detected:

```c
if (bp_poll()) {
    uint16_t pc = bp_hit_addr();
    bp_resume();        // Restores instruction, clears halt, CPU resumes
    // Notify debugger (e.g., via USB CDC, terminal, or future debug protocol)
}
```

### 6. Firmware: Driver Layer Changes

Add to `driver.h` / `driver.c`:

```c
#define REG_BP_CTL      (ADDR_REG | 0x00003)
#define REG_BP_ADDR_LO  (ADDR_REG | 0x00003)
#define REG_BP_ADDR_HI  (ADDR_REG | 0x00004)

#define REG_STATUS_BP_HALT (1 << 3)
#define REG_BP_CTL_CLEAR   (1 << 0)
```

### 7. Resume Sequence (Detailed)

```
Time  MCU                          FPGA                     CPU
----  ---------------------------  -----------------------  -------------------
 t0   (polling)                    halted=1, RDY=0          Halted (stretching
                                   BP_HALT=1 in REG_STATUS  read cycle at bp addr)
 t1   Reads REG_STATUS, sees       (unchanged)              (halted)
      BP_HALT=1
 t2   Reads REG_BP_ADDR_HI/LO     (unchanged)              (halted)
      -> gets breakpoint PC
 t3   Looks up original byte       (unchanged)              (halted)
      in breakpoint table
 t4   spi_write_at(pc, original)   Writes original byte     (halted)
                                   to SRAM via WB bridge
 t5   Writes REG_BP_CTL to         clear_i pulses,          (halted)
      clear halt                   halted=0, RDY reasserted
 t6   (optionally re-arms bp)      (normal operation)       Refetches from pc,
                                                            gets original opcode,
                                                            executes normally
```

**Re-arming:** If the breakpoint should remain active, the MCU must write $DB
back to the address after the CPU has moved past it. The simplest approach:

1. Clear the halt (CPU resumes and executes the original instruction).
2. Wait for the CPU to advance at least one instruction (a few microseconds).
3. Write $DB back to the breakpoint address.

This creates a brief window where the breakpoint is disarmed. For most
debugging use cases this is acceptable.

_Decision needed: Is the simple re-arm (with a timing gap) acceptable, or do
we need a single-step mechanism (execute exactly one instruction then re-halt)?_

## Open Questions

1. **Timing margin.** Is 0.625ns of internal FPGA routing margin between RDY
   deassert and falling PHI2 sufficient? (See Section 4.)

2. **Clear mechanism.** Dedicated `REG_BP_CTL` register (recommended) vs.
   overloading the write to `REG_CPU`?

3. **Re-arming strategy.** Simple delay-based re-arm vs. single-step support?

4. **Address capture width.** 16 bits (CPU address bus) is sufficient for the
   current PET memory map. Do we need to capture the decoded 17-bit RAM
   address (including the $FFF0 banking register)?

5. **STP in user code.** If a PET program legitimately uses STP ($DB), the
   breakpoint mechanism will trap it. This is acceptable since STP is not used
   by any known PET software and would otherwise permanently halt the system.

## Implementation Order

1. **Gateware: `breakpoint.sv`** -- STP detection and RDY gating.
2. **Gateware: `register_file.sv`** -- Add BP_HALT status bit, address
   registers, and clear mechanism.
3. **Gateware: `main.sv`** -- Wire the breakpoint module between register_file
   and the RDY output pin.
4. **Gateware: `common_pkg.sv`** -- Add new register and bit constants.
5. **Gateware: testbench** -- `breakpoint_tb.sv` verifying detection, halt,
   clear, and resume.
6. **Firmware: `driver.c`** -- Add register definitions and low-level access.
7. **Firmware: `breakpoint.c`** -- Breakpoint table and high-level operations.
8. **Firmware: main loop** -- Integrate `bp_poll()` into the polling loop.
9. **Firmware: tests** -- Unit tests for breakpoint table management.

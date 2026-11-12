# Resumable CPU suspension for the menu

## Implementation and validation status

The firmware session/scanner, bounded display/font borrowing, explicit runtime
exits, diagnostic completion protocol, and minimal suspend/audio gateware are
implemented. Back preserves the guest instead of executing the diagnostic ROM
or reloading configuration. The sections below retain the design contract and
acceptance requirements, not a claim that every hardware gate has passed.

Production firmware and ROM builds, the firmware suite, all 70 gateware tests,
and 528 selected system/board/menu/firmware-transport tests passed. The broader
native-drive system run was stopped and is not a full-suite pass. Production-top
menu tests exercise real SPI sequencing and external chip models. Their renderer
adapter models ownership, not actual DVI/core-1 scanout. Diagnostic ROM execution
and VIA shift-register sound remain limited by unsupported chip-model behavior.

Board startup subsequently timed out with marker `$00` and a breakpoint at
`$0CA8`, outside the diagnostic ROM. Investigation found the new soft-core
read-data enable changing on its own capture-clock edge. Routed delays and a
delayed-route regression demonstrate missed captures. The latch now samples
the pulse's falling edge, after a full system cycle, using held bus ownership.
Both simulators pass repeated reset, nested subroutine returns, and NMI/RTI,
and the system tests include suspended pending-data preservation. This identifies
and corrects a hardware hazard, but does not yet prove the reported board boots.

The corrected routed bitstream built without changing constraint commands. C3 STA
reports positive analyzed setup/hold relationships, with worst system-clock
setup slack of +1.380 ns and hold slack of +0.086 ns. The captured-data paths
to the 6502 and 6809 have setup slack of +27.624 ns and +36.883 ns respectively.
Constraint coverage is not complete:
the existing 6809 NMI latch has two unclocked pins and two unconstrained endpoints,
and `pia1_clock_o` has no output delay. Eight CPU-data inputs retain their explicit
one-system-period maximum-delay bound rather than input delays. No speculative
constraint changes were made to hide these findings.

Fitted rev-a acceptance remains open. It must establish physical W65C02S
RDY/BE/resume timing, measured keyboard settling, ordinary EOI/motor behavior,
actual DVI transitions and failure handling, and fitted W65C22N PCR/CB2 sound
behavior. The 100 us keyboard dwell is provisional. The corrected bitstream has
not been tested on the board, and software/model results are not hardware signoff.

## Scope and required behavior

Replace the runtime menu's ROM-and-reset transition with a suspend, borrow,
and restore operation. The running CPU remains selected and retains its
execution state. Firmware renders the menu and scans the physical keyboard
through the `io_controller.sv` Wishbone peripheral. Returning from the root
menu with Back restores the display and borrowed I/O state, then resumes the
same program with successful disk changes still applied.

The implementation scope is rev-a only. Keep its shared CPU/PIA/VIA PHI2 running
throughout suspension, and do not snapshot or restore VIA timers. Rev-b has not
been manufactured. Independent PIA1 clocking is future work, not an implementation
requirement, capability-provisioning task, or acceptance gate for this plan.
Use RDY to stop the 6502 and BE low to put the physical CPU's address, data,
and R/W pins in high impedance during MCU I/O accesses. The 6809 soft core
uses its existing E/Q pause mechanism, with the physical CPU kept off the bus.

| User action | Required result |
|---|---|
| Menu button long press or SHIFT-SHIFT at runtime | Suspend the selected CPU, preserve memory, mute runtime audio, and open the dashboard |
| Back within a submenu | Navigate to the parent, leaving the CPU suspended |
| Back at the dashboard root | Restore borrowed state and resume without reset or configuration reload |
| Mount or eject a disk | Apply the existing live media operation immediately, retaining successful changes on Back |
| Activate a machine configuration | Leave the picker, apply that configuration, and reset immediately, without returning to the dashboard or waiting for Back |
| Activate the currently selected configuration | Treat explicit activation as a configuration reload and reset too |
| Explicit Reset action | Restore the current machine's borrowed state, then reset it, retaining current disk mounts |
| Boot configuration picker | No resumable guest exists. Choose a configuration and reset into it |
| Fatal error that starts the diagnostic ROM | Abandon resumability explicitly. Never later resume the old CPU context |

"Immediately" for configuration selection means the selection commits the
transition in that dispatch cycle. Necessary storage completion, configuration
loading, and reset sequencing still occur before execution starts.

The guarantee is CPU execution continuity and RAM preservation, plus restoration
of readable configuration borrowed by the menu. Hidden PIA1 ORA latch bits
configured as inputs are an explicit exception. This is not a cycle-exact snapshot
of the whole computer or its external devices. On rev-a, VIA timers and physical
interrupt sources continue to evolve. Neither clock-driven state nor external
interrupt inputs are frozen. Restoring a control register does not guarantee
restoration of an internal control-pin handshake/pulse phase.

Accept coalesced periodic interrupts and missed PIA1 retraces. Leave unborrowed
devices' pending flags and interrupt enables alone so the CPU can service
remaining enabled requests after resume. PIA1 keyboard accesses can acknowledge
retrace and cassette-read flags, and those flags are not reconstructed. Active
cassette transfers are outside the resume compatibility guarantee. External
serial peers, cassette hardware, and IEEE devices do not stop time when the menu
opens. Do not add a synthetic IRQ or pending-flag overlay for this baseline.

## Previous implementation and why it could not resume

The following source paths describe the baseline before this change. Paths are
relative to the repository root.

| Path | Relevant behavior |
|---|---|
| `fw/src/input/input_controls.c`, `fw/src/usb/keyscan.c` | Recognize menu gestures and suppress repeated held gestures |
| `fw/src/pet.c` | `pet_task()` waits for storage inactivity, then calls `menu_enter(false)` |
| `fw/src/menu/menu.c` | `menu_enter()` calls `tape_deinit()`, halts, clears video RAM, starts the menu ROM, and always calls `pet_reset()` on exit |
| `fw/src/roms/roms.c` | `start_menu_rom()` selects the soft 6502, changes video configuration, loads the built-in font and ROM, overwrites reset vectors, and resets |
| `fw/src/menu/menu_config.c` | Waits for the ROM to select lowercase, runs input/drive work, and unconditionally calls `load_config()` on exit |
| `fw/src/menu/menu_ui.c` | Config selection already exits the controller immediately. Reset and dashboard Back both merely end the controller, so the session owner cannot distinguish them |
| `rom/src/main.s` | Initializes PIAs, VIA, and CRTC, selects lowercase, continuously scans the keyboard, and implements happy/uh-oh tunes |
| `fw/src/display/display.c` | Firmware-owned display refresh writes the full 8 KiB at physical SRAM `$8000-$9FFF`. Runtime menu uploads must instead be bounded to 1 KiB |
| `fw/src/menu/menu_screen.c` | Clearing a menu already overwrites 2 KiB of physical video backing memory |
| `fw/src/driver_sync.c`, `fw/src/driver.c` | `sync_state()` reads a snooped keyboard matrix. `pet_io_read/write()` already access physical I/O through the MCU Wishbone window |
| `gw/EconoPET/src/io_controller.sv` | Reserves a CPU slot, takes the physical bus, and holds ACK until the SPI controller can observe it. It currently has no backup/restore facility |
| `gw/EconoPET/src/main.sv` | A bridge reservation removes physical BE and CPU bus ownership. Ordinary 6502 bus slots remain enabled when READY is clear |
| `gw/EconoPET/src/main.sv`, `timing.sv` | PIA1 PHI2 is currently tied to shared PHI2, which runs independently of READY. Independent menu clocking is not implemented |
| `gw/EconoPET/src/keyboard.sv` | Matrix observations depend on CPU PIA1 accesses. The selected-column tracking is not a complete DDR-aware PIA state shadow |
| `gw/EconoPET/src/timing_6809.sv` | Clearing READY pauses complete E/Q cycles without reset. Reset deliberately continues clocking the core |
| `gw/EconoPET/src/audio.sv` | Mixes SID and VIA CB2 into a signed sample and feeds a delta-sigma DAC, with no audio-enable control |

`load_config()` is destructive even when `apply_mounts` is false. It prefills
`$9000-$FFFF`, executes load/patch/copy actions, changes machine options and CPU
selection, and refreshes the font. Skipping disk remounts does not make it a
restore operation. None of this belongs on the Back/resume path.

Selecting another CPU is also destructive. `main.sv` holds unselected soft cores
in reset. Running the menu on a different core and then switching back cannot
preserve the original 6809 or soft 6502 context.

## Design boundaries and ownership

Use one firmware session owner, preferably a new `fw/src/menu/menu_session.c`
and header, called by `menu_enter()`. Separate the following responsibilities:

- Gateware owns safe CPU/bus suspension and complete device-clock pulses under
  the selected clock policy. Do not add I/O shadows or a save/restore sequencer.
- Firmware owns the menu lifecycle, physical SRAM/font backup, video setup,
  readable PIA1/PCR save/restore, keyboard scan scheduling, input routing, and
  the final exit decision.
- Existing media APIs own mount/eject operations. A menu snapshot must never
  contain disk state that would undo a successful media change.
- The boot/diagnostic path may still run a small 6502 ROM. Runtime entry must
  not call it.

Use explicit states such as `IDLE`, `SUSPENDING`, `SAVING`, `MENU_ACTIVE`,
`RESTORING`, and `COMMITTING_CONFIG`. Publish snapshot validity only after all
required backups succeed. Reject nested sessions. Record which resources have
been borrowed so partial entry can unwind without restoring uninitialized data.

Give the UI an explicit result rather than relying on `config_changed`:
`MENU_EXIT_RESUME`, `MENU_EXIT_RESET`, or `MENU_EXIT_LOAD_CONFIG`, with a selected
configuration index for the last case. Only the session owner performs resets
and configuration loads. A disk picker success is not an exit result.

## CPU suspension and BE ownership

### Acknowledged suspension

The dedicated `REG_SUSPEND` byte at Wishbone `$40007` retains request in bit 0
and reports acknowledged isolation in bit 1. Request/acknowledgment disagreement
denotes an entry/release transition. Firmware bounds its wait to 100 ms rather
than adding busy/error bits. `REG_AUDIO` at `$40006`, bit 0, controls output-only
mute. Both contracts are defined in `common_pkg.sv` and `hardware_contract.h`.

The suspend state machine must:

- Accept a request without modifying CPU selection, RESET, NMI, bank registers,
  or the breakpoint latch.
- Apply the 6502 RDY hold at the defined safe phase. Finish any already committed
  transfer and its post-PHI2 hold interval before acknowledging suspension.
- In 6809 mode, wait for E and Q low and completion of the final data/hold
  events. Use `cpu6809_halted` as part of the acknowledgement condition.
- Remove guest bus ownership for the entire acknowledged interval. Keep physical
  `cpu_be_o` low, including idle gaps between MCU requests. Qualify guest SRAM,
  PIA/VIA, SID, CRTC, keyboard-snoop, bank-register, and other peripheral strobes
  so a stalled instruction cannot repeat its side effects.
- Keep SPI arbitration, video fetching, raw bus-slot scheduling, and DAC
  operation alive. Keep rev-a shared PHI2 running throughout suspension.
- Release only at a safe CPU-slot boundary after the bridge has finished its
  final hold interval. Present the pending guest access and valid read data
  before allowing that CPU cycle to advance.

Prefer a separate suspend hold on the existing READY path so the guest's control
register is retained. Save and verify the original CPU control and CPU-select
values in firmware. Do not resume by blindly writing `CPU_READY`, since the
machine may already have been halted or stopped at a breakpoint.

The old raw-I/O API also serves non-menu users. Preserve its halted-CPU use case,
but strengthen admission to require a safe bus boundary. Runtime menu access
must require the new suspend acknowledgement. A release attempted while an I/O
transaction or restore is active must remain pending or return a named error,
not enable the CPU early.

### Rev-a clock policy

Keep shared PHI2 and raw arbitration, address/data/hold strobes, and the bridge
state machine operating throughout menu dwell and physical I/O access. The
current bridge waits for raw slot BE in `WAIT_SLOT` and completes at the hold
strobe. Disabling that scheduling would deadlock MCU access. Preserve chip
select, address, R/W, and data through the requested device's setup, pulse-width,
and hold intervals.

CPU execution suspension is separate from peripheral clocking. Do not use
physical `cpu_be_o` as a guest-running predicate, since it remains low when a
soft CPU is selected. Qualify guest execution and side effects without stopping
rev-a PHI2 or disabling bridge scheduling. Reset must still receive all required
physical and soft-core clocks. A pre-existing READY/breakpoint halt must not be
released by leaving the menu.

Firmware does not read, park, reload, or compensate VIA timers. Necessary PCR
accesses for charset selection are distinct from a timer snapshot.

### Future independent PIA1 clocking (out of scope)

The unmanufactured [rev-b I/O schematic](../../hw/rev-b/mainboard/kicad/IO.kicad_sch)
exposes `BPHI2_PIA1` separately from `BPHI2`. That could later allow PIA1-only
keyboard accesses while shared PHI2 is stopped, improving clock-driven VIA
continuity without timer restoration. Actual board wiring, stopped-clock
behavior, complete pulses, capability selection, and reset/resume timing would
need separate validation. Do not implement or require that path for rev-a.

### Physical W65C02S

RDY and BE have separate jobs. RDY retains execution state. BE low releases the
physical address/data/R-W outputs so the FPGA can perform the I/O access.
Neither signal resets the processor. The bridge already masks BE through
`io_ctl_bus_reserve`, but the menu needs this protection throughout suspension,
not only while one individual request is active.

Verify suspension on read, write, opcode-fetch, stack, and read-modify-write
cycles. A stopped CPU can continue presenting an access while PHI2 runs, so
suppressing repeated peripheral selects and SRAM write strobes is mandatory.
Do not generalize W65C02S behavior to unsupported NMOS socketed CPUs.

### Soft 6502

Keep the core selected and use its RDY input. Verify internal state as well as
bus address stability, including instruction/operand capture and interrupt
sampling. `soft_cpu_data_q` currently protects paused 6809 read data, but its
6502 path is not protected in the same way. Qualify pending-data capture so
bridge and video traffic cannot become a paused 6502's operand. Verify the
first resumed read is recaptured at the correct point.

Do not edit the vendored core as the first approach. Put ownership and data
qualification in the existing wrapper/top-level integration and prove whether
any additional core adapter is needed.

### Soft 6809 and SuperPET state

Reuse the existing whole-cycle E/Q pause. Do not substitute the 6809 HALT pin,
reset the core, switch to a 6502 for the menu, or change the machine-type bit.
Preserve all architectural and internal execution state, including pending
interrupt sampling and the wrapper's MMU FIRQ handling.

Preserve `address_decoding.sv` state in place: 8096 memory control, SuperPET bank,
write protection, flat mapping, SYNC disable, and pending flat-mode transitions.
Suspension acknowledgement must include completion of a committed deferred
mapping change. The SYNC/MMU trigger is not simply a CPU write strobe, so check
it explicitly for changes while suspended. Preserve the ACIA and dongle state
without reset. Serial activity may continue under either peripheral clock policy.

Apply these requirements to SuperPET I/O enabled with a 6502 as well as to the
6809. No banked memory copy is needed if the menu never writes those banks.

### Assertions and transaction completion

Extend `io_controller.sv` and top-level assertions to check:

- MCU I/O ownership implies physical BE low and no guest bus owner.
- At most one PIA/VIA select is active for a bridge transaction.
- Bridge traffic cannot produce a guest SRAM write or guest keyboard observation.
- Captured addresses, data, and direction survive until the device hold interval
  ends, independently of Wishbone arbitration returning to SPI.
- ACK corresponds to one completed external transfer. Repeated strobe visibility
  cannot duplicate a write.
- No CPU release occurs before restore completion and bridge idle.

Retain the current distinction between raw arbiter timing (needed to service
MCU accesses while suspended) and guest-qualified timing (which must stop).

## Firmware I/O backup and restore

### Scope of the saved context

The firmware session owns one snapshot of the readable borrowed state and
sequences setup and restoration through the existing physical I/O bridge.
Keep the physical `$E810-$E87F` window and its aliases compatible. Do not add
gateware save/restore commands, register shadows, or mirrored command offsets.
Each write must complete its external transfer and hold interval before the
session advances. Reject nested sessions and track partial acquisition in
firmware. A failed restore must not release the CPU.

| State | Capture method | Restore rule |
|---|---|---|
| PIA1 CRA/CRB writable control fields | Read controls and mask out read-only IRQ flags | Restore writable fields, including DDR/data selection and C2 modes |
| PIA1 DDRA/DDRB | Read under DDR selection, retaining the original controls | Restore directions while minimizing unrelated output changes |
| PIA1 ORA | Read under data selection | Restore output-configured bits. Hidden latch bits configured as inputs cannot be recovered |
| PIA1 ORB | Do not write the output latch | Preserve in place, even while temporarily changing DDRB |
| VIA PCR | Read the non-destructive register | Restore the complete original value after a masked CA2 override |
| PIA1 pending IRQ flags | Not part of the saved context | Accept acknowledgement by intentional keyboard accesses, do not synthesize replacement flags |
| PIA2, VIA timers/SR/ACR/IFR/IER, IEEE lines | Leave untouched by menu setup | Preserve state in place subject to the clock/external-event policy, do not replay a bulk register dump |

A generic "read all registers, write them all back" implementation is invalid.
PIA data addresses select DDR or data according to control bit 2. Port reads
can acknowledge interrupts and return input levels for input-configured bits.
The MVP explicitly accepts loss of those hidden ORA latch bits, rather than
adding shadows or temporarily driving input pins merely to inspect them.
Software that later changes those bits to outputs can observe the changed
latch contents. Ordinary keyboard row bits configured as outputs remain
readable and restorable. ORB remains untouched, so its hidden latch is retained.
A hardware reset or destructive transition invalidates the firmware snapshot.

### Keyboard scan setup and restoration

Before changing PIA1, snapshot the writable state and inspect the current modes.
Configure the low nibble of port A to select the ten keyboard rows and
port B for keyboard input. Preserve unrelated port A high-nibble values and
C2 output functions. Do not copy the ROM's broad `init_io` sequence, which also
changes cassette and IEEE signals.

Use explicit masks for CRA/CRB data-selection bits and DDRA row-select bits.
Configure DDRs through their correct access mode, then select data access.
Scan by writing the selected row to ORA and reading PIA1 port B through
`pet_io_read/write()`. Restore the observable ORA and direction/control state
before releasing the CPU, including when the CPU was paused just before its
own port-B read.

Restore observable ORA and directions in a documented order that minimizes
transiently driving input pins or toggling unrelated outputs. Avoid writing ORB
unless the selected implementation actually changed it. Include nonstandard
DDR/control values in tests, not only the BASIC ROM's usual setup.

The WDC PIA documentation distinguishes DDR selection from data access and
specifies interrupt clearing on peripheral data reads. These semantics require
DDR-aware sequencing and the explicit interrupt/latch-loss policy, rather than
a register dump.
[W65C21 datasheet](https://www.westerndesigncenter.com/wdc/documentation/w65c21.pdf)

### SPI read-ahead must not touch extra I/O registers

The previous `pet_io_read()` called `spi_read_at()`. That queued the requested read,
then retrieves its result with `READ_NEXT`, which also queues a physical read
of the next address. For example, fetching CRA can also read port B and clear
its pending flags. Fetching VIA PCR can also read IFR. Reading the upper bridge
boundary can prefetch outside the decoded window and stall. The driver's current
range restriction avoids that boundary, but does not prevent side effects inside
the window.

`spi_read_once()` in `fw/src/driver_io.c` retrieves the completed byte as
the first received byte of a full-duplex absolute read of a harmless FPGA status
register. This queues a harmless fabric read rather than an adjacent physical
I/O access. The request/drain pair does not yield core 0's SPI ownership.
Production-top tests execute this same firmware sequencing through the narrow
`spi_read_command()` transport seam and count physical accesses, including
aliases and the upper bridge boundary.

If that mechanism cannot satisfy the contract, add an explicit result-only SPI
operation or a side-effect-free command/result mailbox in `io_controller`.
Update the selected protocol's constants, firmware helpers, and contract/system
tests together. Do not substitute `READ_SAME`, which would repeat the destructive
read instead. Select and prove one mechanism before implementing the scanner.

The chosen mechanism must leave SPI flow control idle and the bridge released
after returning. Test one requested I/O read produces exactly one physical
select/read, with no adjacent-register access, including at decode boundaries.
Accepted PIA1 interrupt loss is not permission to touch unrelated registers.

### Pending interrupts and accepted losses

Physical PIA IRQ flags are not ordinary writable backup state. Keyboard reads
can clear a pre-existing retrace interrupt on port B. Rewriting CRB cannot put
it back. The baseline explicitly accepts this loss and does not implement a
pending-flag overlay, synthetic IRQ, missed-event counter, or interrupt replay.

The [rev-a I/O schematic](../../hw/rev-a/mainboard/kicad/IO.kicad_sch) connects
PIA1 CA1 to `CASS1_READ` and CB1 to `BVERT` (vertical retrace). CA2 drives
`EOI_OUT`, and CB2 normally controls cassette 1 motor output. CA2/CB2 input
modes can also generate flags and have the phase limitations below. The keyboard
matrix itself does not generate interrupts, and PIA1 has no timers.

- Accept coalesced or acknowledged PIA1 retraces. Do not catch up the guest's
  software clock or keyboard scans after menu dwell.
- Accept cassette-read flag loss from PIA1 port A accesses. Active cassette
  loads/saves cannot be expected to continue after CPU suspension, regardless
  of clock policy. Preserve tape configuration and breakpoint patches, but do
  not promise transfer continuity.
- Leave VIA and PIA2 interrupt enables and flags untouched. On rev-a an armed
  VIA timer can expire while suspended. Its sticky flag remains pending, and
  IRQ remains asserted if enabled until acknowledged. Repeated expirations
  coalesce rather than becoming a queue. Guest interrupt masking still controls
  service after resume.
- Preserve normal IRQ routing to the selected CPU. Do not clear, mask, inject,
  or replay unrelated IRQ, FIRQ, or NMI sources as a menu convenience. Verify
  each core's sampling behavior during suspension and the first resumed cycle.

This policy accepts missed events, not accidental writes or changes to unrelated
device configuration. It is not an external-device compatibility guarantee.

### PIA1 handshake and output modes

Active PIA1 C2 handshake/pulse phases are another limitation. Restoring a control
byte alone does not reconstruct an in-progress handshake or undo an external
EOI edge. This is separate from VIA CA2 charset selection below. Do not claim
arbitrary peripheral transparency from latch equality.
Use best-effort restoration without mode-based entry rejection or an active-phase
detector. Preserve configured C2 fields and avoid broad initialization writes,
but do not promise internal phase or external-edge continuity in unusual modes.
Unsupported cassette transfer continuity does not permit unnecessary motor or
EOI changes. Independent PIA1 clocking would not make active phases writable.

### Lowercase character set

PCR is the VIA Peripheral Control Register at `$E84C`. It controls CA1/CB1
interrupt edge selection and CA2/CB2 control-pin modes, not timer countdowns.
The PET uses VIA CA2 to select the character set. On rev-a it drives the
`GRAPHIC` video-selection net, not PIA1's IEEE EOI or cassette motor outputs.

Fixed CA2 output is the expected normal PET use, rather than a handshake with
an external peripheral. As representative primary evidence,
[PET 4.0 KERNAL initialization](https://github.com/mist64/cbmsrc/blob/master/KERNAL_PET_4.0_REC/oa1src)
writes PCR `$0C` (fixed low), and the
[PET editor's CRTSET routine](https://github.com/mist64/cbmsrc/blob/master/EDITOR_PET_EURO/edt2src)
changes the charset field while retaining the upper PCR bits. The local
`rom/src/main.s` menu initialization writes `$0E` (fixed high/lowercase).
These examples support the ordinary fixed-output path, not a claim that every
PET, SuperPET, or third-party program uses it. CA2 handshake/pulse use is an
unusual software-controlled video effect for this wiring, not a normal
external-handshake requirement.

Use firmware PCR save/restore for the MVP. Save PCR before menu entry and
override only its CA2 mode field to fixed high (lowercase). Preserve CB2 mode,
CA1/CB1 edge selection, and all unrelated bits. "PCR restoration" means writing
that saved register value back before resume to restore the guest's
character-set/control-pin configuration, not restoring VIA timers or pending
interrupts. Never write the ROM's literal `$0E` over an unknown guest PCR and
never mute by altering VIA SR or ACR.

Do not add runtime active-phase detection, CA2-mode-based entry rejection, or a
gateware charset override for the MVP. PCR exposes the configured mode, not the
current internal handshake phase. Restore even unusual saved mode fields, but
make their control-pin phase continuity explicitly unsupported. A restored PCR
byte can match while a CA2/CB2 handshake or pulse level differs from its pre-menu
state. Apply the same best-effort phase policy to PIA1 while minimizing changes
to its physical outputs.

Verify on the fitted VIA that changing CA2 while retaining CB2's mode does not
disturb ordinary CB2 sound operation, including shift-register-driven sound.
Test graphics/lowercase restoration and saved PCR values with nonzero CB2 bits.
This is a narrow PCR-write-effect check, not a prerequisite to implementing a
general handshake-state detector. An effective video-only override can be
reconsidered later if real supported software demonstrates a need for stronger
phase preservation.

`sync_state()` reads the raw CA2 status into `system_state.video_text_mode`.
After each PCR change, allow the output/status/display path to settle before
publishing the corresponding menu or restored DVI state.

## Firmware keyboard scanning

The cooperative physical scanner in `fw/src/input/pet_keyboard.c` is used
only after suspension and I/O setup have completed. Reuse existing matrix
constants and `keyscan_sample()` navigation decoding. Do not conflate the
repository's row/column terminology with new array dimensions.

The scanner should retain a row index, settle deadline, previous sample, and a
bounded retry count. Select one row, allow the board signals to settle, and
sample until stable or until the bounded retry expires. Advance over all ten
rows and publish a complete matrix coherently. The old ROM has a substantial
settling loop and two-read stability check, so one immediate read is not a
validated replacement. Measure the necessary delay on hardware.

The implementation uses a provisional 100 us dwell and at most three samples
per row. An unstable row retains its last complete observation instead of
manufacturing a release. Exit requires a fresh complete, stable scan with all
physical keys released. Input and drive service continues while this guard
waits (including indefinitely held or persistently noisy keys).

While the menu scanner owns the matrix, `sync_state()` must not overwrite its
complete-round publication with row-at-a-time observations from
`WB_KBD_BASE_ADDR`. Raw bridge reads must bypass USB injection and be
recorded as physical input. USB and UART navigation continue through their own
existing routing paths.

The FPGA keyboard cache observes executed PIA1 accesses from either the CPU
or the SPI I/O bridge. A shared row latch, DDRA, and CRA/CRB data-selection
tracking distinguish actual scans from DDR/control accesses. Bridge samples
remain raw, while CPU samples retain the existing USB-injected value. Physical
PIA reset resets this decoding state without clearing the last-observed matrix.
USB Wishbone writes still update injection only, not the observation cache.

Reset-era gesture suppression belongs to physical acquisition, not the generic
hold timer. `WB_KBD_VALID_BASE_ADDR + column` reports whether that cached column
has been physically observed since PIA reset. Reset clears these read-only
validity bits without manufacturing a released matrix. Runtime acquisition
checks the configured Shift column's validity before copying the matrix, then
re-arms gestures only after observing both Shifts released. Repeated reads of a
pre-reset released cache cannot re-arm a held chord. Direct scanning can
establish that release only from a complete stable round. Ordinary routing
transitions consume current holds without invalidating the physical source.

The final released scan refreshes that cache before the scanner stops, including
at boot. Keep menu input routing throughout the release guard, then retire
navigation and consume gestures at the runtime handoff. The first subsequent
`sync_state()` cannot resurrect the pre-menu chord while the guest has not
yet started scanning.

On entry and exit, retire menu navigation, consume the trigger gesture, and
suppress held keys until release. Neutralize stale injected matrix state before
resuming. Do not replay the keys used to navigate the menu into the suspended
program. Physical keys remain real electrical inputs, so define a release guard
for the exit key before resume, or explicitly accept that the guest can see a
still-held physical key. Prefer the release guard while continuing menu service.

Physical SHIFT-SHIFT recognition before entry still depends on the running
program scanning PIA1. The existing matrix snooper cannot detect new physical
keys when the program never scans them. Keep that limitation explicit. Button
and USB menu entry remain available. Background intrusive keyboard polling of
a running guest is a separate feature and is not required for this plan.

## Memory and display preservation

Preserve memory in place wherever possible. Do not copy all 128 KiB of SRAM to
the MCU. Since no menu ROM runs at runtime, zero page, stack, ROM backing SRAM,
ordinary RAM, and expansion banks need no writes and no restoration.

The menu does overwrite display storage, so save all affected physical bytes
before even calling `menu_screen_clear()`:

- The first 1 KiB physical SRAM interval `$8000-$83FF`, read using SPI after
  acknowledged suspension. Bound every runtime menu clear and periodic upload
  to that interval, regardless of the guest video mask. Leave other bytes alone.
- The first 2 KiB character BRAM, read using SPI before installing the menu font.
  Select the first two quadrants for the menu and leave the upper quadrants alone.
- The video configuration register, defined CRTC values that the menu changes,
  and firmware video metadata used by DVI. Do not read/restore all 32 entries
  when some are uninitialized or undefined. Preserve the guest address latch.
- Any menu-specific USB keymap replacement. Prefer direct menu navigation that
  leaves the runtime keymap untouched, rather than loading `us.bin` on entry.

Allocate dedicated bounded storage before taking the CPU over, preferably
static buffers with explicit ownership. The two memory images require 3 KiB
plus metadata. Check RP2040 RAM usage including both cores, DVI buffers, media
caches, and stack margins. Do not keep a shared temporary buffer acquired for
an arbitrarily long menu session.

SPI accesses are physical SRAM/BRAM accesses, not banked guest virtual accesses.
Use that distinction in names and tests, especially with 6809 flat mode and
8096 mappings. Test the entire physical SRAM for unintended changes.

Configure the menu's existing 40x25 geometry and built-in font from firmware.
Use a read-only contrasting menu attribute plane in DVI rather than initializing
the guest color plane at framebuffer offset `$800`. Keep guest color bytes
unchanged in SRAM and in the host framebuffer. The 3 KiB backup bound is valid
only if all runtime menu writers honor these intervals.
Use the CRTC Wishbone register window, which does not change the guest's CRTC
address latch `ar`. Do not emulate CPU writes to `$E880/$E881` merely to program
menu geometry, since doing so would disturb the guest's pending indexed write.
Retain fixed-CRT behavior and validate both display types.

Keep CRT sync running. Publish DVI buffer/font/geometry changes at a defined
frame boundary with a core-1 acknowledgement, rather than relying on a shared
struct being updated atomically. The current charset settle delay documents a
real visibility issue but is not a general snapshot protocol. Stop menu-owned
display uploads before restoring VRAM so a later `display_task()` cannot write
menu characters over the restored program screen.

## Audio enable

Add one MCU-visible `AUDIO_ENABLE` bit. When set, pass the existing mixed SID
and CB2 sample to the DAC unchanged. When clear, send signed zero to the DAC.
Use a sample mux before the DAC converts the signed sample to unsigned bias.
Both channels must follow this bit. No volume levels, shifts, multiplier,
separate menu-mute override, or gain ramp are needed.

Do not force the DAC output pin low to disable audio. Leave the delta-sigma
accumulator running so signed zero produces its normal midscale output. Preserve
the current mixer arithmetic and overflow behavior. Audio disable must not
reset SID or write VIA sound registers.

Save the previous enable bit in the menu session and clear it before any
I/O/display changes. Restore that saved value only after state restoration,
so a machine that was already silent remains silent on Back. Define the
power-on default as enabled. Explicit reset of the current machine retains its
saved enable state. Configuration activation and boot diagnostics explicitly
enable audio after retiring any old menu session.

SID and VIA may continue advancing while the CPU is suspended. The objective
is silence while browsing, not whole-machine waveform phase continuation.
Check audible on/off transitions on hardware.

Boot happy/uh-oh diagnostics are explicit exceptions to runtime-menu silence.
Enable audio before playing a boot tune so a previous disabled state cannot
suppress it.

## Entry, resume, reset, and configuration procedures

### Runtime entry

- Wait for the existing storage inactivity condition while continuing drive
  service. This is a readiness check, not proof that every external protocol
  transaction is idle.
- Establish session ownership, capture original CPU control/selection, and
  save and clear `AUDIO_ENABLE`.
- Request suspension and wait with a bounded timeout for bus-safe acknowledgement.
  Keep rev-a shared PHI2 running during and after that acknowledgement.
  Do not clear RESET/NMI or change CPU selection as part of this operation.
- Save video/font memory and metadata before any clear or render. Capture the
  readable PIA1/PCR state in firmware before borrowing it.
- Borrow PIA1 and the charset/display state, then enable direct keyboard scanning.
- Route input to the menu, suppress the entry gesture, publish menu display state,
  and run the existing UI and cooperative media service.

### Back/resume

- Stop accepting navigation and wait for the physical exit key release guard.
  Finish any in-flight keyboard read and stop the scanner.
- Stop firmware uploads of menu video and restore font, VRAM, CRTC, video
  configuration, and DVI metadata from the valid snapshot.
- Sequence physical I/O restore and wait for completion, including restoration of the
  guest's selected keyboard row and register access modes. Do not reconstruct
  accepted PIA1 flag losses or acknowledge unrelated pending interrupts.
- Clear menu input queues and reconcile injected keys without resetting the
  machine. Complete the display publication handshake while still muted.
- Release the suspend request at a safe CPU-slot boundary. Restore the prior
  run/halt state, retaining any pre-existing breakpoint halt. Shared PHI2 remains
  running. Restore the saved `AUDIO_ENABLE` bit and retire the snapshot.
- Keep current media state. Do not call `load_config()`, `pet_reset()`,
  `ieee_drive_reset()`, `tape_deinit()`, or `start_menu_rom()` on this path.

### Explicit reset of the current machine

Restore borrowed memory/display/I/O first, but keep the CPU suspended. Retire
the session and invoke the existing `pet_reset()` lifecycle with a defined
suspend-to-reset handoff. Preserve current mounts. Allow the reset pulse and
necessary core clocks even when execution was suspended. Do not briefly run the
old program between restore and reset.

### Configuration selection

The picker returns `MENU_EXIT_LOAD_CONFIG` immediately when activated. Stop
scanning/rendering, restore borrowed state under suspension, then permanently
invalidate the old resume context before beginning destructive configuration
actions. Restoring first avoids leaving menu bytes in video RAM or a partially
loaded font when the next configuration does not initialize every byte.

Tear down old tape/breakpoint configuration at this destructive boundary, load
the selected configuration with its configured mounts, and use `pet_reset()`
to start it. Release suspension as part of that reset handoff, never as an
intermediate resume. Reset still belongs to `pet_reset()` so drive and input
reset ordering stays centralized.

After the first destructive load/patch/copy/mount action, failure cannot safely
return to the old program. Present the established fatal/recovery path with no
resume option. Before that boundary, recoverable entry/setup failures should
restore what was borrowed and return to the original machine.

## Storage, tape, debugger, and asynchronous work

Retain `menu_service()` drive processing and existing busy/error handling for
mount/eject. Successful disk changes are already applied by
`ieee_drive_try_mount()`. Do not rerun configuration mount actions on Back.
A busy media operation remains a recoverable menu error and must not be forced
through by resetting the drive.

`sd_activity_quiet()` is not a proof of IEEE bus quiescence. Do not add bus resets,
line releases, forced channel closes, or FIFO flushes as a menu convenience.
Any required protocol changes need the ROM/hardware evidence workflow in
[IEEE compatibility guidance](../../.github/instructions/ieee.instructions.md),
including the selected 4040 or 8050/8250 ROM and PET KERNAL behavior. This plan
makes no new claim of external-drive compatibility during an indefinite pause.

Retain tape breakpoint patches and configuration on runtime entry. The current
`tape_deinit()` removes patches from guest memory, which violates preservation.
Do not run `bp_task()` callbacks that can alter RAM or release the CPU while the
menu owns suspension. If entry is deferred until a tape callback completes,
document that boundary and test a gesture received during the callback.
Preserve a breakpoint that was already hit, including its original-byte and
rearm state. Raw breakpoint control must not accidentally release menu ownership.

UART input can still reach the CLI in the current routing design. Audit reset,
NMI, CPU-select, RAM-write, and debugger commands reachable while a menu is
active. Make them reject, defer, or explicitly invalidate the session through
the session owner. An unrelated command must not silently make a saved context
unsafe. Read-only diagnostics can continue.

## Reducing menu.rom to boot diagnostics

Move keyboard scanning, display setup, lowercase selection, and menu readiness
out of the ROM. Keep a small boot diagnostic program for the happy and uh-oh
tunes and only the initialization those tunes actually require. Its zero-page,
stack, vectors, and VIA changes are acceptable before a guest is running or
after a fatal path has abandoned that guest.

Replace the normal ROM's endless keyscan loop with a bounded tune protocol and
an idle/completion indication. Add named boot-diagnostic request/done status,
or use a documented existing mechanism that does not require reading a
side-effectful I/O register. Firmware must wait for completion or time out and
halt the diagnostic CPU before taking over physical PIA access.

The diagnostic ROM now writes `$A5` to SRAM `$0200` after initialization and
after the interactive happy tune, then idles without scanning PIA1. Firmware
clears that byte before reset/NMI and waits at most two seconds. The boot picker
then acknowledges suspension and starts the firmware scanner without taking a
guest snapshot. Boot/fatal-only I/O and CRTC initialization remains in the small
ROM, keeping the fatal renderer independent of SD-card access.

The interactive boot picker should become the same MCU-rendered, MCU-scanned
UI used at runtime, but without a guest snapshot or Resume result. Preserve the
current silent default-configuration fast path unless deliberately changing the
product behavior. Keep uh-oh rendering usable without SD-card/keymap access
and preserve the recursion guard in `fatal.c`.

Do not leave the diagnostic soft 6502 running concurrently with menu I/O. It
must be halted before the MCU scans PIA1. A future MCU tune sequencer could
remove this ROM dependency entirely, but is not necessary for resumable menus.

## Expected code changes

| Area | Expected changes |
|---|---|
| `gw/EconoPET/src/common_pkg.sv`, `fw/src/hardware_contract.h` | Named suspend and audio-enable registers and masks, shared status semantics |
| Firmware SPI helpers, `spi1_controller.sv` and SPI command definitions only if needed | Single-effect physical reads with a validated harmless pipeline drain or result-only interface |
| `gw/EconoPET/src/register_file.sv` | Control/status plumbing, defaults, valid address handling, and register-count/width updates |
| `gw/EconoPET/src/main.sv`, `timing_6809.sv`, potentially a new suspend controller | Safe acknowledgement with rev-a PHI2 continuing, BE isolation, guest strobe qualification, pending-data protection, reset handoff |
| `gw/EconoPET/src/io_controller.sv` | Safe physical transfer admission and completion during acknowledged suspension, no shadows or restore sequencer |
| `gw/EconoPET/src/keyboard.sv`, CPU data mux and IRQ integration | Correct DDR-aware observations, paused-data protection, and unchanged normal interrupt routing, no pending-flag overlay |
| `gw/EconoPET/src/audio.sv` | Single-bit selection of the unchanged mixed sample or signed zero before DAC bias |
| `gw/EconoPET/src/video.sv`, `video_crtc_reg.sv` only as needed | Safe menu geometry publication while preserving guest CRTC address latch, no MVP gateware charset override |
| `fw/src/driver.c`, `driver_io.c`, `driver.h`, `driver_sync.c` | SPI platform seam, suspend commands, raw I/O admission, single-effect reads, split keyboard/status synchronization, audio accessors |
| New `fw/src/menu/menu_session.c/.h` | Own save/borrow/restore/commit and failure unwind |
| New physical scanner module, `fw/src/input/input.c`, `usb/keyscan.c` | Cooperative direct PIA1 scanning, coherent matrix publication, held-key transition rules |
| `fw/src/menu/menu.c`, `menu_config.c` | Remove runtime ROM startup and unconditional reload/reset, dispatch explicit exit results |
| `fw/src/menu/menu_ui.c/.h` | Distinguish Back, Reset, and configuration activation while retaining immediate media actions |
| `fw/src/display/*`, `system_state.*`, `roms/roms.*` | Dedicated backups, core-1 handoff, reusable menu font/geometry setup independent of CPU execution |
| `fw/src/pet.c`, `tape.c`, breakpoint and CLI integration | Central reset handoff, retain tape state on resume, prevent conflicting mutations |
| `rom/src/main.s`, ROM build/embedding rules, `fatal.c` | Boot-only tune service and firmware-owned diagnostic display setup |
| Gateware, firmware, and system test manifests | Register new tests and shared contract values |

The session and scanner are implemented in `menu_session` and `pet_keyboard`.
Control register values are shared by the firmware and gateware contract headers.
Prefer extending existing ownership boundaries over duplicate SPI or reset APIs.
Document each new function/task and explain ordering at each multi-step boundary.

## Implementation order and acceptance gates

### Decisions and evidence required before implementation

The rev-a continuous-clock and interrupt-loss policies above are selected. The
remaining gates are how to implement them safely, not whether to add VIA timer
restoration or support unmanufactured rev-b hardware:

| Gate | Required decision or evidence |
|---|---|
| Device timing | Check fitted W65C02S/W65C21N/W65C22N documentation and rev-a hardware for complete shared PHI2 pulses, bus setup/hold, and external-edge behavior while the CPU is suspended |
| CPU suspension boundary | Trace RDY, soft-core enables, pending read data, IRQ/FIRQ/NMI sampling, and reset across entry/resume in all CPU modes. Raw bridge timing must remain live without advancing guest execution |
| PIA1 borrowing | Establish DDR-aware readable-state restore order, leave ORB untouched, and minimize EOI/motor changes. Accept hidden ORA input-bit latch loss and best-effort control-pin phase restoration without mode-based rejection |
| Charset selection | Implement firmware-only masked PCR save/restore. Verify ordinary fixed-output charset and CB2 sound behavior. No MVP active-phase detector or gateware charset override, unusual phase continuity is unsupported |
| Single-effect SPI reads | Prove the harmless absolute-read pipeline drain returns the intended byte with one physical access, preserves serialization, and leaves bridge/flow control idle. Use a result-only interface if it fails |
| Keyboard settling | Measure row-select settling and bounded stable-read behavior on actual rev-a keyboards |
| External protocols and tape callbacks | Identify safe callback/ownership boundaries without resetting drives or removing tape patches. Verify any claimed external compatibility against selected ROMs and hardware, not interrupt coalescing alone |

Do not block initial CPU/bridge tests on broad external compatibility research.
Resolve each gate before implementing or enabling the affected behavior, and
keep unsupported transfer continuity explicit.

### Establish the peripheral contract first

Write failing tests for the required exit semantics and preservation contract.
Audit rev-a wiring, fitted PIA/VIA register behavior, and IRQ/handshake limitations
before promising transparent I/O restore. Test the selected coalescing/PIA1-loss
policy, verify PCR write effects for the ordinary charset/sound path, and
establish what happens when an external transaction prevents safe borrowing.
These are correctness gates, not optional polish after UI integration.

### Add the hardware contract and safe suspend mechanism

Implement request/acknowledgement, physical BE isolation, guest bus-strobe
qualification, continuous rev-a PHI2, and reset handoff for physical 6502,
soft 6502, and soft 6809. Extend contract tests and low-level simulations.
Demonstrate no repeated writes, no reset, stable context, correct device pulses,
and correct first resumed transfer before adding a menu.

### Add I/O context ownership and direct keyboard scanning

First implement and test single-effect physical I/O reads. Then implement firmware-owned
readable-state save/setup/restore, the accepted PIA1 flag/latch-loss policy,
and bounded session handling. Add a cooperative scanner and stop
`sync_state()` from overwriting its matrix. Validate physical-only observations,
USB separation, settling, and restored row/DDR/control state.

### Add audio and display preservation

Implement audio enable, dedicated memory backups, firmware menu geometry/font
setup, and the DVI publication handshake. Verify the 1 KiB screen and 2 KiB font
are restored and all unborrowed regions remain unchanged across repeated entry/exit.
Preserve CRT sync and test both display
types before connecting normal menu gestures.

### Integrate explicit menu outcomes

Refactor the UI result and session owner. Make root Back resume, Reset perform
the centralized reset, and config activation immediately load/reset. Retain live
disk changes. Remove tape teardown and configuration reload from runtime Back.
Exercise timeout/unwind and conflicting CLI/debugger commands.

### Shrink the boot ROM and unify menu input

Retain necessary diagnostic initialization and tunes, introduce completion
signaling, and remove the ROM keyboard loop. Firmware owns picker input after
completion. Verify interactive boot, silent default boot, and fatal diagnostics
independently from runtime suspend.

### Validate the complete machine and board

Run end-to-end programs and real hardware tests, then build gateware and follow
the timing-analysis workflow for the changed ownership paths. Only enable the
runtime feature by default after the physical CPU and both soft modes satisfy
the same acceptance criteria. Do not ship a soft-core-only success as proof of
physical BE/PIA timing.

## Verification details

### Gateware tests

Extend `io_controller_tb.sv`, CPU data mux/keyboard tests, and relevant top-level
simulations. Add focused suspend and audio benches. Cover arbitrary request
phase, reads/writes, delayed Wishbone grants, invalid commands,
and release during an active bridge transaction. Cover nested sessions and
restore completion in firmware tests. Count
external bus operations rather than merely checking returned data.

Exercise the rev-a shared-clock topology in all selected CPU modes. Assert
continuous scheduled PHI2 throughout suspension, including idle, display traffic,
and PIA1 scans. Cover complete physical access pulses, post-pulse hold, reset,
safe CPU release, and no suspended soft-core advancement from bridge activity.

Check both enable states with positive/negative samples and extrema. Assert
unchanged mixed samples when enabled, exact signed zero at the DAC input when
disabled, continued DAC clocking, identical left/right output, and no SID/VIA
reset caused by disabling audio. Verify restoration of both prior enable states
and explicit audio enable for configuration activation and boot diagnostics.

### Firmware tests

Add session tests with operation-recording mocks. Assert ordering and forbidden
calls, not only final booleans. Cover Back with no changes, Back with mount/eject,
Reset, a new configuration, reselecting the current configuration, failed media
operations, setup/restore timeouts, repeated sessions, and pre-existing mute or
breakpoint halt. Ensure controller exit reasons cannot be overwritten by later
queued navigation in the same input burst.

Cover both keyboard layouts, held entry and exit keys, USB/UART interaction,
scanner timeout, stale snoop data, and cooperative directory scans. Verify
failure cleanup never restores an incomplete snapshot and never resumes after
a destructive configuration action.

### System integration tests

Extend `test/system/spi_cpu_test.cpp` and add a menu-suspend suite using the real
SPI/Wishbone path and fitted PIA/VIA models. Existing
`ClearingReadyPauses6809WithoutResetAndResumesMemoryWrites` is useful groundwork,
but it does not establish full menu preservation. The separate-clock fixtures
in `test/system/io_test.cpp` are not rev-a acceptance tests.

For each CPU mode, run a program that maintains register/stack sentinels, changes
RAM, and accesses I/O. Suspend at varied points, perform many real keyboard
transactions and display uploads, restore, then verify the subsequent program
results and transfer trace. For soft cores also compare architectural/internal
state where the fixture exposes it. For physical hardware use a diagnostic
program and bus captures rather than pretending SPI can read all CPU registers.

Test these cases explicitly:

- Read-modify-write and stack cycles, pending reads, and interrupts at entry.
- Full physical SRAM equality before entry versus immediately before resume,
  including hidden video bytes, ROM vectors, and all expansion banks.
- All font bytes, 40/80 columns, video masks, custom CRTC start addresses/font
  quadrants, and a suspended write to the previously selected CRTC register.
- Non-default PIA ORA/ORB/DDR/control values and restore while a subsequent
  guest port read is poised. Verify intentional PIA1 flag acknowledgement,
  accepted retrace coalescing, unchanged unrelated flags/enables, and no replay.
  Check exact ORA restoration only for output-configured bits, document hidden
  input-bit latch loss, and verify the untouched ORB latch remains unchanged.
- VIA PCR with graphics/lowercase fixed outputs, nonzero CB2 bits, and active
  sound, without changes to SR/ACR. For unusual CA2 modes, verify restoration of
  the saved PCR byte without asserting phase continuity or requiring detection.
- 6809 banked and flat mappings, write protection, SYNC/FIRQ handling, and
  Waterloo and OS-9 keyboard paths.
- SuperPET I/O with a 6502, retained tape patches, and existing breakpoint halts.
- Mounted media changes surviving Back, with no incidental drive reset.
- Rev-a long menu dwell with shared PHI2 running, one-shot timer expiry and
  coalesced free-running timer flags serviced after resume. Do not assert frozen
  counters or one interrupt per elapsed period.

### Build and timing validation during implementation

Use the root presets and the repository's required configuration split:

```sh
cmake --preset default
cmake --build --preset fw
cmake --build --preset fw-test
ctest --preset fw --parallel --output-on-failure
ctest --preset gw --parallel --output-on-failure
cmake --build --preset sys-test
ctest --preset sys --parallel --output-on-failure
cmake --build --preset gw
```

Production uses `RelWithDebInfo`. Tests, fixtures, frameworks, and firmware
compiled into tests use `Debug`. Keep optimized Verilator models/runtime
separate with assertions preserved. Run both available simulator variants for
changed gateware. Do not introduce Release or cross-configuration matrices.

For ownership/timing changes, follow
[the timing-analysis skill](../../.github/skills/timing-analysis/SKILL.md).
Log evidence, inspect current RTL, SDC, generated template, and pad timing,
then run `check_timing`, setup/hold, and CDC analysis on a fresh implementation
build. Preserve the existing generated-clock relationships unless RTL evidence
requires a change. Do not add broad false paths to hide a BE or data-hold failure.
This planning document makes no STA closure claim.

On rev-a hardware, capture shared PHI2, BE, RDY, R/W, address/data, I/O selects,
and relevant IRQ/output lines across entry, direct scanning, restore, and resume.
Verify continuous shared PHI2, complete access pulses and safe CPU release,
high impedance and turnaround, stable select/data through hold time, continuous
CRT sync, keyboard settling, restored program execution, and audible silence.
Repeat with an occupied and empty CPU socket, both soft cores, and long menu
sessions. Confirm external-device limitations with the actual selected ROM and
hardware before claiming compatibility.

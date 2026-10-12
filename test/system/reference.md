# Host test authoring reference

## Building fixture tests

Build production with `RelWithDebInfo`. Build tests, simulator integration,
frameworks, and their dependencies with `Debug`, including production sources
compiled for host tests. Root presets apply this split.

Build Verilator-generated models and runtime separately with optimized
`RelWithDebInfo` flags and SV assertions enabled. Keep linked test, framework,
and firmware sources in `Debug`, without model optimization flags or `NDEBUG`.

Use GoogleTest or Check assertions for test expectations. Use `vet()` for
invariants required in production and `assert()` for test-covered paranoid
checks that are intentionally elided from production. Do not undefine `NDEBUG`
or put required work or side effects inside assertions. Host fatal checks use
[`test_fatal.c`](../support/test_fatal.c).
The host C++ framework requires C++23. Firmware and external dependencies keep
their existing language standards.

The root presets require CMake 3.21 or later and build and run this suite,
including the hardware contract test:

```sh
cmake --preset default
cmake --build --preset sys-test
ctest --preset sys
```

Both `all` presets include this suite, so it runs in CI. Root builds place its
generated files in `build/system`. The standalone commands below use
`build/host-fixtures` instead.

The shared D64 fixtures, checked framework value types, external PIA/VIA models,
and their C++ tests run
without Verilator, a simulated board, ROM media, or firmware transport.
Standalone configuration defaults to `BUILD_BOARD_TESTS=OFF`. Root presets
enable the production-top board tests as well, requiring Verilator but no ROM
media or firmware transport.

Standalone builds require CMake 3.20 or later.
Initialize the pinned peripheral model dependency before configuring:

```sh
git submodule update --init test/external/chips
```

The hardware contract consistency test requires Icarus Verilog (`iverilog`
and `vvp`). It elaborates only the production constants, not a simulated board.

```sh
cmake -S test/system -B build/host-fixtures -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host-fixtures
ctest --test-dir build/host-fixtures --output-on-failure
```

To include board simulation in a standalone build, configure with
`-DBUILD_BOARD_TESTS=ON`. Its SID lookup table is copied into the generated
board-test runtime directory, so tests do not rely on the source working directory.

The firmware Check tests also use the shared C builder. Build and run those
with `cmake --build --preset fw-test` and `ctest --preset fw`.

Keep regression tests in the suite for the feature they exercise. Register
expected-abort tests with the forked runner, using a separate fatal suite in the
same feature test file when its normal suite runs without forking.

## Production-top simulator foundation

[`system.h`](system.h) owns an independent Verilator context, the production
FPGA model, physical SRAM, and fitted PIA/VIA devices. The simulation-only
[`system.sv`](../../gw/EconoPET/sim/system.sv) wraps production `top.sv`
without replacing SPI, Wishbone, address decoding, CPU selection, or video logic.
[`SystemTest`](framework/system_test.h) gives each test a fresh board.

`tick(Cycles)` advances whole 64 MHz system clocks. It resolves CPU/FPGA data
ownership, previews read data, completes peripheral accesses at falling PHI2,
and commits SRAM data when write enable releases. Fitted devices receive the
physical reset, selects and jiffy input, and feed IRQ, text-mode and audio levels
back to the FPGA. The diagnostic switch also drives fitted PIA1 PA7 without
replacing the other port-A input levels. `RawStimulus::audio_det_i` is active-high
(true means a jack is inserted). The wrapper converts it to the production
active-low `audio_det_n_i` pin, which `top` normalizes back to active-high for `main`.
PIA1 follows its isolated keyboard-scanning clock while PIA2
and the VIA follow CPU PHI2. `peek` and `poke` access physical SRAM for fixture setup,
not CPU-visible addresses or the SPI bridge.
The VIA CA2 level reaches the FPGA and snapshots as `text_mode_i`
(high selects upper/lowercase text, low selects uppercase/graphics).

If a tick rejects an access after the FPGA advances, it suspends at the exact
phase rather than rolling state back or rounding the simulator counter.
`clock_faulted()` identifies this state, `half_ticks()` retains its exact
timestamp, and `time()` remains readable as the last completed whole cycle.
Snapshots, SRAM and peripheral inspection remain available. Further ticks
require external reset assertion. `set_external_reset(true)` followed by
`tick(Cycles{1})` cancels the rejected access and completes the retained cycle
(including only its remaining half when interrupted in the high phase).
Remove invalid stimulus before releasing reset. A zero-cycle tick does not
recover the clock.
Reset does not override normal SRAM or FPGA bus resolution, so SPI memory
inspection remains available while either reset driver holds the CPU.
Only an unsupported VIA shift-register read canceled by reset is treated as
undriven data instead of rejecting recovery.

Use `drive_physical_cpu`, external reset/interrupt setters, and
`raw_stimulus(callback)` for pin-level setup. Raw edits commit only after
successful completion and cannot mutate or clock the board during the callback.
`drive_spi` drives only SPI0 pin levels and cannot steal a scoped transaction's
pins. Changes do not evaluate the FPGA until `tick`.
`snapshot()` and `peek_stimulus()` return editable, assignable detached values
that can outlive their board. Fixture-owned inputs have one access path,
`snapshot().stimulus`. Resolved bus data, device feedback, harness inputs and
FPGA outputs remain separate snapshot members. Editing a snapshot does not
edit the board. The board is noncopyable/nonmovable, and borrowed `io()` devices must
not outlive it.

## Observation, deadlines and failures

`observe(callback)` returns a move-only `ObserverSubscription`. Keep it alive
for the interval to observe. Callbacks receive `const System&` after each full
cycle's FPGA evaluation, peripheral updates, SRAM commits and time advancement.
No callback runs for `tick(0)`. Subscribers run in registration order.
Disconnecting a subscription is idempotent and takes effect during dispatch,
including when a callback disconnects itself or a later subscriber. An executing
callback retains its captures until it returns. A subscription may outlive its
board, but does not keep the board alive.

Observers and completion predicates cannot mutate or clock the board through
captured references, register observers, or mutate retained fitted devices.
Read-only snapshots, SRAM/device inspection and diagnostics remain available.
An observer exception propagates unchanged and stops that cycle's dispatch.
The completed cycle is retained and the board remains clockable after the
throwing subscription is removed. This differs from an interrupted hardware
access, which still requires the external-reset recovery described above.

`run_until(predicate, budget, reason)` checks completion immediately, then
after each cycle through the inclusive final boundary. `service_until` uses
the same predicate contract, but passes a mutable board and the remaining
`Cycles` to an active service callback. If service makes no clock progress,
completion is checked again, then one cycle is clocked if still incomplete.
An already complete zero-budget wait succeeds without work. An incomplete
zero-budget wait times out without service or clocks. Oversized clock requests
are rejected before advancing. Nested waits inherit the tighter deadline and
restore the enclosing deadline on every exit. Predicate and service exceptions
propagate unchanged, and guards are restored. Clock health is checked before
every completion predicate. If active service catches a hardware rejection,
it must complete external-reset recovery before completion can be observed.
Merely asserting reset without completing the retained cycle is not recovery.

```cpp
System board;
const auto start = board.time();
unsigned samples = 0;
auto subscription = board.observe([&](const System& observed) {
    EXPECT_EQ(observed.time(), start + econopet::Cycles{++samples});
});
board.run_until([&](const System& observed) {
    return observed.time() == start + econopet::Cycles{7};
}, econopet::Cycles{7}, "seven observed cycles");
subscription.reset();
```

The default simulation seed is 1, or the decimal value of `ECONOPET_SIM_SEED`.
Valid seeds are in `[1, INT_MAX]`. `System(seed)` supplies a per-board override.
Startup is zero-initialized unless `System(seed, System::InitialState::Random)`
is requested. The seed and policy are installed before model construction and
do not alter a peer board's RNG. These are simulation seeds, not FPGA placement
seeds. `SystemTest` records the actual seed in test properties and assertion
traces, and reports diagnostics when a test fails.

`diagnostic(operation, detail)` formats read-only evidence with the completed
cycle, half-cycle phase, CPU selection, seed, startup policy, reset/fault state,
bus address, SPI levels and up to 16 recent fixture/deadline/SRAM events.
Successful raw-stimulus edits record changed fields and their requested values,
including CPU direction/data and indexed spare pins. Unchanged fields, no-op
edits and discarded callbacks do not add history entries.
Timeouts and board-side access rejections attach this evidence while preserving
their standard exception category. `failure<Exception>(operation, detail)`
constructs an exception with the same evidence for later framework layers.
Diagnostics do not clock, dispatch callbacks or acknowledge peripheral reads.

## External PIA/VIA models

[`io.h`](io.h) and [`io.cpp`](io.cpp) adapt the pinned
[`chips`](../external/chips) digital models. The `econopet_external_io` library
builds in Debug, independently of GoogleTest and the FPGA simulator. Its tests
are in [`io_test.cpp`](io_test.cpp).

The submodule requires the [DLehenbauer/chips fork](https://github.com/DLehenbauer/chips),
pinned to [`264ddc2`](https://github.com/DLehenbauer/chips/commit/264ddc2fe7a5127eeb0e87e44af6ca2314818706),
rather than upstream `floooh/chips`. The adapters depend on its:

- MOS 6520 model and `m6520_peek`/`m6522_peek` functions, which preview
  register reads with current input levels without advancing the original
  model or acknowledging its interrupts or handshakes.
- PIA C2 fixes for manual levels, read-A/write-B handshake strobes, one-cycle
  pulses, and preservation of externally driven control-input levels.
- VIA fixes for independent control-input edge sampling, preservation of
  input levels, IRQ summary gating after enable changes, and ignoring IFR
  summary-bit writes when acknowledging source flags.
- VIA timer-load pipeline restart, underflow detection only on actual count
  edges (including maximum loads), and reset suppression of one-shot
  interrupts until the timers are rearmed.
- T2 pulse counting from resolved PB6 levels across DDR/output changes,
  independently of external/output disagreement or the port input latch.

Dependency updates must preserve them and pass the peripheral regressions.
PIA/VIA control and GPIO pin encodings must also match (checked at compile time).

`Pia6520` models DDR/output-latch isolation, mixed GPIO, both control-input
edge polarities, interrupt flags/enables, and fixed/handshake/pulse C2 outputs.
`Via6522` models unlatched GPIO, T1 one-shot/free-running timers, timed or
PB6-counted T2, IFR/IER, control-input interrupts, and fixed/handshake C2
outputs. VIA shift-register accesses, input latching, PB7 timer output, and C2
pulse-output modes throw before changing device state or write observations.
These are digital models, not analog loading or propagation-delay models.
T2 pulse counting observes the resolved PB6 pin: DDR selects the external
input or output latch. External transitions cannot count while PB6 is driven
as an output, and stable levels never generate repeated pulses. Output-latch
and direction changes count only when they produce a resolved falling edge.

Access fitted devices with `pia1()`, `pia2()`, and `via()`. Use named
`PiaRegister`, `ViaRegister`, control flags, and interrupt sources.
PIA control composites such as `PiaC2High` pass directly to `set_control`.
Use `.bits()` only when encoding a raw bus or assembly byte.
Use `ChipSelect::None`, `ChipSelect::Pia1`, `ChipSelect::Pia2`, and
`ChipSelect::Via` directly when constructing `ChipSelects` bus selections.

```cpp
using namespace econopet;
using namespace econopet::io;

Io devices;
auto& pia = devices.pia1();
const CycleTime at{Cycles{0}};

// Configure external inputs and a mixed-direction port.
pia.inputs([](Inputs& levels) { levels.port_a = 0xa5; });
pia.set_control(PiaRegister::ControlA, PiaDdrAccess, at);
write(pia, PiaRegister::PortA, 0x0f, at + Cycles{1});
// Select port access and a fixed high CA2 output, then drive the output latch.
pia.set_control(PiaRegister::ControlA, PiaC2High, at + Cycles{2});
write(pia, PiaRegister::PortA, 0x03, at + Cycles{3});
// Observe without acknowledgment, then complete a read.
const auto presented = pia.peek(PiaRegister::PortA);
const auto completed = read(pia, PiaRegister::PortA, at + Cycles{4});
```

`read(device, reg, at)` and `write(device, reg, data, at)` each advance that
peripheral by one clock, not the board clock. Reads return the presented value
before acknowledging flags or handshakes. `peek` and `peek_inputs` change
nothing. Keep `clock(access)` for timestamped PHI2 bus integration.

`inputs(callback)` edits detached levels and commits only after successful
callback completion and mutation-guard checks. Callback or guard failures
discard the edit. Nested edits and peripheral mutation during a fitted
device's callback are rejected across all fitted devices.

`Pia6520`, `Via6522`, and `Io` are noncopyable and nonmovable. Device references
must not outlive their `Io` owner. Copy `state()` or use `peek_inputs()` for
detached snapshots. Reset preserves externally driven inputs, injected faults,
and write history.

`Io::sample` captures the preceding stable bus sample once at falling PHI2.
`BusSample::pia1_phi2` optionally supplies the isolated PIA1 clock (otherwise
PIA1 follows CPU PHI2). Isolated PIA1 edges do not advance PIA2, the VIA or the
CPU PHI2 clock count.
Repeated high/low samples do not duplicate accesses, deselected devices still
clock their timers, and reset assertion cancels pending accesses even with
overlapping selects or an invalid register. Held reset does not repeatedly
reset the devices. Multiple physical chip selects are rejected when reset is
inactive. `writes().last()` is optional, so no write
is distinct from a write of zero. Completed-write records carry the caller's
full-cycle timestamp, and explicit observation clearing does not reset devices.
The selected access is validated before any peripheral advances. Unsupported
VIA modes or out-of-range registers leave every device, write history, cycle
count, and pending bus sample unchanged. Retrying the rejected falling edge
rejects again without ticking. Present a corrected high sample before completing
the access, or assert reset to cancel it.

`Via6522::Timer1Fault::StuckInterruptFlag` is an explicit synthetic overlay.
It does not modify normal timer state, survives reset, and raises IRQ only
when the corresponding source is enabled. Removing the fault exposes the
normal underlying interrupt flags again.

Run the standalone peripheral checks with:

```sh
ctest --test-dir build/system \
  -R '^system\.External(Types|Pia|Via|Bus)\.' --output-on-failure
```

Owned write publication and real-CPU peripheral-write capture are described
under [Owned traces](#owned-traces). Full CPU/peripheral IRQ integration suites
remain later incremental layers.

## Checked SPI and CPU lifecycle

[`registers.h`](registers.h) defines named FPGA registers, SPI commands,
status flags, PET peripheral addresses and native reset vectors.
`StatusBit::Text` uses `ECONOPET_REG_STATUS_TEXT_MODE_MASK` for the asserted
video-mode bit (clear means graphics).
`spi().read` and `spi().write` use the production SPI/Wishbone path, not a
memory shortcut. Use `fpga::address(SramAddress)` for direct SRAM access
(including the upper bank), or a named `fpga::Register`. Valid but unmapped
Wishbone addresses reach the real decoder and time out rather than being
rejected as out of range. These operations advance simulation time and work
while the CPU is held in reset.

`spi().command(ByteView)` accepts exactly one complete command, rejects
reserved bits and wrong lengths before clocks or pin changes, and returns
the first received byte. Production reads are pipelined. `spi().read`
seeks with `ReadAt`, then clocks the result with `ReadNext`.
`spi().transaction(callback)` holds CS across complete commands. Its callback
receives only `SpiCommands::command`, not raw shifting or release controls.
The scope drains the controller and releases CS before returning.
Void and value results (including owning move-only values) are supported.
References, raw pointers and
`reference_wrapper` results are rejected. Other returned values must not
retain callback-local or board-borrowed state. These restrictions reject common
borrowed forms, not all borrowing types. Spans, views and reference-capturing
callables still require the caller to ensure every referenced object outlives
its use. The API does not prove that a returned value is detached.

```cpp
System board;
const auto address = fpga::address(econopet::SramAddress{0x12345});
board.spi().write(address, 0x42);
EXPECT_EQ(board.spi().read(address), 0x42);

const std::array<uint8_t, 2> write_next{
    fpga::command_byte(fpga::SpiCommand::WriteNext), 0x5a};
board.spi().transaction([&](auto& commands) {
    commands.command(write_next);
});
```

`spi().raw_transaction(half_period)` explicitly permits partial commands,
bit/byte shifting and custom positive timing. Call `finish()` to drain the
controller before releasing CS, or `abort()` for a deliberate reusable release.
Abort does not roll back an admitted write. Default SCK half-periods are two
system cycles, and release settles for four. Readiness waits accept the
inclusive final cycle and inherit enclosing deadlines. No phase is shortened
to fit a deadline.

An unfinished owner or exception escaping acquisition, work, drain or release
fails closed: CS is raised, SCK lowered and ownership released without cleanup
clocks, FPGA evaluation or observer calls. The original exception and timestamp
are retained. `spi().peek_failed()` then rejects further clocks and active
transport/deadline work permanently, even if a service callback catches the
exception. Discard that board and construct another. Passive snapshots, SRAM,
CPU observations and diagnostics remain usable. This transport policy is
distinct from recoverable hardware clock suspension.

`cpu()` offers shared reset, selection/control reads and passive inspection.
Selection reads decode only the CPU field, independently of the SuperPET I/O
mode flag. Selection, preparation and startup preserve that independent flag.
CPU-control reads mask unspecified upper register bits before constructing
typed flags. Raw SPI register reads still return the complete byte.
`cpu(CPU_SOFT_6502)` or `cpu(CPU_SOFT_6809)` binds core-specific lifecycle
operations. `prepare()` asserts reset, drains admitted activity and selects
the core. `write_vector(entry)` requires that bound core quiescent in reset.
`start(entry)` prepares, installs a native-endian vector, holds reset and
releases it. `start()` instead uses an already installed vector.
Startup does not imply program completion.

6502 reset vectors use little-endian bytes at $FFFC, while 6809 vectors use
big-endian bytes at $FFFE. Reset hold is 1,280 system cycles. Release sets READY
for both physical and soft 6502, clears it for soft 6809, and verifies resolved reset.
External reset cannot be overridden. Selection requires quiescent reset, and
stale bound views cannot release another core or overwrite its vector.
Direct vector installation also requires an idle, unowned SPI bus. It drains
outstanding controller work before patching SRAM, then rechecks CPU
quiescence. Raw CS release settles for four cycles before checking both SPI
and admitted Wishbone/SRAM activity. Calling it inside a checked or raw
transaction is rejected before
clocks or memory changes.
Physical-6502 startup requires explicit fixture vector/memory setup, and its
fetch history is unsupported. `write_control_raw(byte)` is the escape hatch
for deliberately unusual control sequences.

```cpp
auto cpu = board.cpu(CPU_SOFT_6502);
cpu.prepare();
// Install native machine-code bytes in physical SRAM before writing the vector.
board.poke(econopet::SramAddress{0x0300}, 0x4c);
board.poke(econopet::SramAddress{0x0301}, 0x00);
board.poke(econopet::SramAddress{0x0302}, 0x03);
cpu.write_vector(econopet::CpuAddress{0x0300});
const auto checkpoint = cpu.peek_fetch_checkpoint();
cpu.release_reset();
board.run_until([&](const System& observed) {
    return observed.cpu(CPU_SOFT_6502).peek_fetched_at(
        econopet::CpuAddress{0x0300}, checkpoint);
}, econopet::Cycles{10000}, "6502 loop fetch");
```

Native address observations are not necessarily opcode fetches. Fetch history
is per-address, monotonically checkpointed and preserved across reset and
selection changes. Soft 6502 records SYNC opcode consumption at its advancing
falling clock edge,
using that edge's READY and pre-advance address. A core held on SYNC does not
advance checkpoints, even as bus arbitration continues.
Soft 6809 records native SYNC (not exact opcode attribution). Inactive
cores and SPI accesses do not create CPU observations. Snapshots expose these
simulation-only native signals without changing production RTL.

SPI/CPU views borrow a persistent board and must not outlive it. Factories
reject temporary boards. Const-board views expose only passive inspection.
Active operations and captured transaction handles remain subject to observer
and input-edit mutation guards.

## Atomic ROM fixtures

`load_rom(path, SramAddress, size)` and `install_rom_images(images)` install
exact-size binary images into physical SRAM. The whole batch is validated and
read into detached buffers before any bytes or fixture ownership change.
Missing files, incorrect sizes, read failures, invalid mappings and overlaps
throw with board diagnostics. Success and failure both leave simulation time
unchanged.

Installation requires asserted CPU reset, a healthy clock, no active SPI
owner, idle boundary pins and no outstanding SPI/Wishbone/SRAM work. After
manual `drive_spi()` edits, first clock at least four release cycles with CS
high and SCK low, then explicitly drain outstanding work. Installation rejects
unsettled transport without silently advancing, resetting or selecting a CPU.
Changing the external reset pin through `set_external_reset()` or
`raw_stimulus()` also requires a completed tick before installation. The last
evaluated reset output alone is not evidence of quiescence after a pin edit.
No-op edits do not invalidate settlement, and `tick(0)` does not establish it.

The default `FixtureOverlap::Reject` disallows replacing fixture-installed
bytes. `FixtureOverlap::Replace` permits replacing earlier fixtures, but never
permits overlapping images within the incoming batch. Adjacent images are
allowed, and all physical SRAM banks are available.

```cpp
system.load_rom("custom.bin", econopet::SramAddress{0xc000}, 4096);
system.load_rom_set(System::RomSet::Upgrade, "/local/media/roms",
                    System::FixtureOverlap::Replace);
```

Supported complete manifests and revision entry metadata are declared in
[`roms.h`](roms.h). Each immutable descriptor binds metadata to static image
arrays. `images(set)` returns a read-only span with static lifetime rather
than allocating a container. The directory-free `load_rom_set(set)` resolves
`$ECONOPET_MEDIA_DIR/roms` only when called, never during test discovery.
Synthetic loader tests require no ROM media. Pure manifest and complete
routine-entry checks in [`rom_catalog_test.cpp`](rom_catalog_test.cpp) run in
the host target with `BUILD_BOARD_TESTS=OFF`, without Verilator. Both supported
Commodore revisions provide CIOUT, so only the revision-specific TRANS1 entry
is optional. Actual ROM execution belongs to later layers.

`selected_rom()` describes the declared base revision, not a content hash.
Complete-set metadata commits atomically with its bytes. A failed installation
preserves the prior selection. Deliberate `poke()` patches and reset preserve
this base identity. Generic installation outside the selected set also
preserves it, while replacement touching any selected-set byte invalidates it.
The const-lvalue accessor borrows optional storage from its board. The rvalue
accessor returns owned optional storage. Revision entries have static lifetime.

## Owned traces

[`framework/trace.h`](framework/trace.h) provides detached source-bound
windows, captures, shared event storage and weak event publication.
[`framework/observations.h`](framework/observations.h) adds scoped board
recorders. No scheduler, assembler, program execution or IEEE wire decoder
is required.

```cpp
auto writes = system.trace_peripheral_writes();
auto video = system.trace_av(test_observation::AvSampling::EveryCycle);
const auto start = system.time();
system.tick(100);
const auto window = system.trace_window(start, system.time());
const auto captured = writes.capture(window);
captured.require_absent("no physical peripheral writes");
```

Windows use `(start, end]` boundaries and a retained identity unique to each
board, not merely matching timestamps. Foreign, reversed, future, early and
stopped-recorder intervals are rejected. An empty covered interval is valid.
`capture()` without a window preserves all retained records, including the
initial A/V sample. Selecting a window excludes that initial sample at its
start and includes its endpoint.

Recorders subscribe to completed-cycle boundaries, do not clock the board,
and are movable but not copyable. Move assignment stops the destination's
previous subscription. `stop()` is idempotent and preserves past evidence.
Event sources prune expired and stopped registrations when attaching new
recorders as well as when emitting, so idle sources do not accumulate stale
subscription storage across repeated recorder lifetimes.
Recorders and detached captures can outlive their board. Const-lvalue access
borrows records and timestamps. Rvalue recorder access copies shared storage
without disrupting active sampling. Rvalue capture access transfers owned
storage and invalidates the source's coverage, as does moving a capture.
An emptied or moved-from capture cannot prove absence.

Coverage is explicit. A skipped observer boundary, detector exception or
event-publication failure cannot turn incomplete evidence into a passing
zero-count assertion. Failed physical I/O sampling invalidates all active
physical-write subscribers, even if a chip write committed before model
publication threw. Reset recovery does not restore the affected recorder's
coverage. Exceptions propagate without replacing detector payloads.
After a detector failure, stop its recorder before continuing. Assertions
include the requested interval. `require_count()` retains repeated events,
including several events at the same cycle, and `require_order()` compares a
projection of the captured values against an exact sequence.
Selecting a window validates live storage before copying and binary-searches
its timestamp range. Both live recorders and detached captures copy only the
requested records, rather than copying all retained history first.

Available board sources are:

- `trace_peripheral_writes()` for completed physical PIA1/PIA2/VIA bus writes
  in dispatch order (canonical register addresses, including mirrored accesses).
- `trace_pia1_writes()`, `trace_pia2_writes()` and `trace_via_writes()` for all
  completed writes to a fitted chip, including direct host model setup.
  Clearing model counters or resetting a chip does not erase subscribed
  evidence. Direct model writes are not physical CPU-bus events. Supply the
  board's current time for timestamp-explicit setup writes.
- `trace_signal(Signal)` for transitions of reset, IRQ, NMI, READY, video,
  horizontal/vertical drive, jiffy, left/right audio and fitted-device IRQs.
  The initial level establishes a baseline, not an invented transition.
  CPU reset is active-high assertion state. CPU IRQ/NMI retain their FPGA
  output pin polarity, and fitted-device IRQs are active-high assertions.
- `trace_av()` for digital video, horizontal/vertical drive, jiffy and stereo
  audio levels. `ChangesOnly` records an initial sample and changes to any
  bundled level. `EveryCycle` additionally samples each completed cycle.
  These are digital boundary samples, not rendered frames or analog audio.
- `trace_events<Event>(detector)` for an observation-only detector returning
  `optional<Event>` at completed-cycle boundaries. Detectors and trace factory
  creation obey the existing observer and input-edit mutation guards.

`EventTrace<Event>` is a manual timestamped log, not a coverage-bearing
recorder. It only appends events, rejects backwards timestamps and exposes
owned records. Coverage-bearing count, order and absence assertions belong
to `TraceCapture`, not the manual log.
`require_stable()` and `require_absent()` check observation-only predicates
initially and at every completed cycle through the inclusive endpoint.
They do not observe pulses between cycle boundaries.

Pure trace storage and model-publication tests run without Verilator in
[`trace_core_test.cpp`](trace_core_test.cpp). Board sampling and lifetime tests
are in [`trace_board_test.cpp`](trace_board_test.cpp). Phase-bound windows,
firmware sources and IEEE wire captures remain later
incremental layers. Configured board runs are described below.

## Deterministic scheduling and configured runs

[`scheduler.h`](scheduler.h) exposes `Scheduler`, a scoped absolute-time
orchestrator. [`framework/run.h`](framework/run.h) supplies the single-use
configuration returned by `system.execution()`. Neither is a second clock
owner. Both borrow a persistent board, and callback captures must remain alive
through execution. Scheduler and configuration owners cannot be copied or moved.
Both classes are concrete `System` collaborators, not generic board interfaces.
The scheduler's non-template implementation is
[`framework/scheduler.cpp`](framework/scheduler.cpp). Explicit registration-order
overrides are private configuration plumbing. Public authors use `at()` and
`every()`, which assign registration order automatically.

```cpp
const auto start = system.time();
Scheduler schedule(system);
schedule.at(start + econopet::Cycles{2}, [](System& board) {
    board.set_external_interrupts(true, false);
});
schedule.every(start + econopet::Cycles{3}, econopet::Cycles{3},
               MissedService::Reject, [](System& board, econopet::Cycles remaining) {
    // Service may use SPI or tick within the shared remaining budget.
    board.tick(std::min(econopet::Cycles{1}, remaining));
});
schedule.run_until([start](const System& board) {
    return board.time() == start + econopet::Cycles{10};
}, 10, "scheduled board run");
```

One scheduler can attach to a board at a time. Destruction detaches pending
work without clocks. Stimulus events are ordered by absolute timestamp, then
registration order, and execute before evaluation of the next full cycle.
This includes cycles advanced by SPI. The stimulus interval is `[start, end)`:
an event at the endpoint stays pending until the next cycle or explicit
service pass. `tick(0)` does not dispatch it.

Stimulus callbacks may change fixture inputs but cannot tick, run active waits,
start SPI, drive SPI pins, transfer/release a captured transaction, or change
and reenter their schedule. Completed-cycle observers still run after hardware
and external device sampling. A stimulus exception retains its payload and
consumes that event without replay. The scheduler then fails closed until it
is destroyed. A pre-evaluation stimulus failure is not a hardware clock fault.
Hardware sampling failures retain the existing exact half-cycle suspension
and external reset recovery contract. Scheduler work rejects a suspended clock,
and boundary stimulus is not replayed when completing an interrupted high phase.

Periodic service is cooperative active work. It only runs when `step()` or
`run_until()` polls it, never recursively during clocks inside an active
service or SPI. Due services are ordered by due timestamp and registration
order. Zero-clock peers run at their shared due time before fallback ticking.
If no work is due, one fallback cycle advances the board.

`MissedService::Reject` reports late work before invoking it, including when
its prior callback overran the next slot. `Coalesce` invokes once at the current
time and advances to the first original-phase slot strictly after completion.
Service receives the earliest inherited deadline's remaining budget, not its
period. Callback errors propagate and make the scheduler unusable rather than
silently retrying partially completed work.
Health is checked after active callbacks and before accepting completion.
Catching a stimulus exception inside service does not repair the failed
scheduler or allow a successful run. This applies to anchored and after-slice
services, and to checked lifecycle operations that catch service failures.
Exceptions from configured service (including after-slice work and rejected
strict deadlines) leave the schedule failed even when the lifecycle catches
them. No subsequent service, completion check or successful return repairs it.
Periodic work is exhausted after its final representable occurrence instead
of failing when its next timestamp would exceed the simulation clock range.

```cpp
auto trace = system.trace_av(test_observation::AvSampling::EveryCycle);
const auto window = system.execution()
    .within(10)
    .after(2, [](System& board) { board.set_external_interrupts(true, false); })
    .every(3, MissedService::Coalesce, [](System&, econopet::Cycles) {})
    .run("configured interval");
trace.capture(window).require_count(10, "one sample per completed cycle");
```

Configuration does not install callbacks or clock until `run()`. `within()`
is required, and zero budget is valid. Without `until()`, execution advances
exactly the elapsed budget. With an observation-only predicate, success is
checked on entry and after each service pass, including the exact deadline.
False at the deadline throws. Terminal execution consumes the configuration,
including failures during setup. No automatic reset, selection, vector
installation or CPU start occurs.

`after()` and phase-anchored `every()` default to `RunAnchor::RunEntry`, measured
from terminal operation entry (not configuration construction). The first
periodic service is one period after its anchor. `RunAnchor::CpuRelease`
requires an explicitly supplied lifecycle and is rejected by board-only
`run()`, even if a core is selected or already running. The shared `execute()`
plumbing supports a later checked lifecycle's one-shot release hook and
preserves author registration order across anchors. This layer tests the hook
with existing CPU controls, without introducing program builders or phases.
Successful `execute()` requires that the hook was called exactly once when a
release lifecycle is declared, even if no release-anchored work was registered.
Missing release throws instead of discarding pending anchored work. Completion
checks preserve void, owning value and reference result types, and do not
replace exceptions escaping the lifecycle operation. A returned reference must
still outlive its use.

The alternate `every(slice, ServiceTiming::AfterSlice, work)` mode advances
a whole slice and then performs active work. Callback duration shifts the next
slice, unlike anchored periodic service. A partial final slice reaches the
deadline without calling work. A complete final slice may invoke zero-budget
work, but that work cannot clock beyond the inherited deadline. Boundary
stimulus is applied before after-slice work. Predicates observe after the
slice and its work, not at intermediate cycle boundaries. Only one after-slice
service is allowed, and it cannot mix with phase-anchored services.
Before any slice clocks, service validates the owning board and requires an
unowned SPI bus. A release-aware lifecycle must call its release hook before
starting after-slice service. Invalid service calls fail without clocking either
board or invoking pending stimulus/work.

The board-only regression suites are
[`scheduling_test.cpp`](scheduling_test.cpp) and
[`configured_run_test.cpp`](configured_run_test.cpp). Mixed parked program,
phase and IEEE tests are excluded until their consuming layers land.

## Checked layouts and owned memory

[`framework/asm.h`](framework/asm.h) publishes layout primitives
(`test_asm::Scenario`, `Region` and `RegionKind`) and the explicit CPU-specific
assembly APIs described below. Complete program builders and execution remain
later layers.
[`framework/memory.h`](framework/memory.h) supplies lower-bank views, named
byte/word/buffer handles and detached captures.

```cpp
test_asm::Scenario layout(system);
const auto data = layout.reserve("workspace", test_asm::RegionKind::Workspace,
                                 econopet::CpuAddress{0x400}, 4);
auto view = layout.lower_bank(data);
const auto pointer = view.word("pointer", 0, test_asm::ByteOrder::LittleEndian);
const auto payload = view.buffer("payload", 2, 2);
const std::array<uint8_t, 2> bytes{0x42, 0xff};
view.initialize(pointer, econopet::CpuAddress{0x1234});
view.initialize(payload, bytes);
const auto captured = view.capture();
EXPECT_EQ(captured[pointer], 0x1234);
EXPECT_EQ(captured[payload.byte(1)], 0xff);
```

Reservations have a nonempty unique name, explicit purpose and nonzero CPU
extent. Overlap is rejected for every purpose, including contained and enclosing
ranges. Adjacent reservations are valid. A region may end at `$FFFF`, but
`at()` never returns a one-past address. `slice()` narrows an existing
reservation and retains its identity and purpose (it does not reserve an alias).
Whole-span validation precedes backing writes.
`layout.region(name)` looks up any reservation purpose. It does not impose the
ordinary-data restriction of `lower_bank()`.

Layouts borrow persistent backing memory and cannot copy their identity.
Moving a layout transfers its identity and reservations. All source layout
operations then reject. Previously obtained regions, operands and views remain
valid against the destination. Host-only tests use a small memory adapter with
the same `peek`, `poke` and diagnostic contract as `System`. Views borrow that
backing memory, which must outlive them, but retain their layout identity after
the layout itself is destroyed. Views can be copied. Moved-from views reject
handle construction, reads, writes and capture.

`lower_bank()` accepts only Data, Results and Workspace reservations. Code,
Stack, Mailbox and Vectors remain reserved for their dedicated uses. Host
access maps CPU addresses directly to physical SRAM bank zero, regardless of
CPU selection, banking or I/O decoding. It never uses SPI, clocks or simulated
CPU writes. A view rejects foreign layout owners and bytes outside its extent,
including siblings in the same layout. `Scenario::write()` additionally supports
bounded raw installation in any owned region without adding an assembler.
Empty writes at a region's endpoint are allowed, but nonempty writes must fit.
Actual backing mutation still obeys the board's observation guards.

Named operands own their diagnostic names and CPU addresses. Buffer selection,
sub-buffers and complete words must fit. Words explicitly declare little- or
big-endian storage independently of the host or selected CPU. Initializers
reject negative or oversized integers and validate both word bytes or the whole
buffer before mutation. Buffer input must match its exact length.
Bulk reads, capture copies and initialization validate the extent once and
allocate output storage once where needed. Direct address iteration still calls
the backing object's `peek`/`poke` for each byte, retaining board guards without
per-byte operand/name construction. Valid scalar writes do not format failure
diagnostics.

`view.capture()` snapshots the entire view without clocks. Capture reads require
the same layout identity and bytes present in that snapshot. Same-owner bytes
outside the captured view and foreign operands reject. Captures and operands
can outlive both layout and board, and subsequent backing changes do not modify
captured data. Lvalue `raw_bytes()` borrows immutable storage. Mutable temporary
access transfers bytes, immutable temporary access copies them. Moving or
draining a capture invalidates its source for subsequent reads. View captures
are always nonempty, so MemoryCapture has no `empty()` state query.
Complete program builders also support named allocation and selective capture
(see the checked execution section below).

Pure contract tests are in [`layout_test.cpp`](layout_test.cpp), included even
with `BUILD_BOARD_TESTS=OFF`. Board backing, diagnostic and observation-guard
checks are in [`layout_board_test.cpp`](layout_board_test.cpp). Program
memory cases are in [`memory_test.cpp`](memory_test.cpp). Phase memory cases
are in [`phase_integration_test.cpp`](phase_integration_test.cpp).

## CPU-specific assembly and fluent instructions

Explicit `test_asm::mos6502::Program` and `test_asm::mc6809::Program` assemble
into a previously reserved Code region. They borrow a persistent `Scenario`,
which in turn borrows its backing memory. Both must outlive the program.
Assembly and `install()` do not automatically reset or select a CPU, initialize
its stack or execute code. Checked execution is a separate terminal operation.
Cooperative pause/resume is available through the builder's restricted facade,
not through an explicit standalone assembler.

```cpp
test_asm::Scenario layout(system);
const auto code = layout.reserve("code", test_asm::RegionKind::Code,
                                 econopet::CpuAddress{0x300}, 32);
test_asm::mos6502::Program program(layout, code);
const auto subroutine = program.label("subroutine");
const auto stopped = program.label("stopped");
program.sei().ldx(test_asm::imm(0xff)).txs().jsr(subroutine).jmp(stopped);
program.bind(subroutine);
program.lda(test_asm::imm(0x42)).sta(econopet::CpuAddress{0x200}).rts();
program.bind(stopped);
program.jmp(stopped);
program.install();
```

This example demonstrates assembly and installation only. `install()` does not
start the CPU. Authors may use the existing explicit CPU lifecycle themselves,
or use the complete checked startup/completion API below.

Fluent operands distinguish byte immediates (`imm()`), word immediates
(`imm16()`), CPU addresses, 6502 zero-page (`zp()`) and 6809 direct-page offsets
(`dp()`). Checked constructors reject negative values, narrowing and high
address bits. Ordinary CPU addresses always select absolute/extended encoding,
even below `$100`. Direct-page offsets do not set the live 6809 DP register.
Instructions and labels retain their CPU tag, so cross-ISA emission and calls
are compile-time errors. Low-level encoder functions return fixed byte arrays
tagged with their CPU, and accept their declared byte/word scalar types.
Use the checked immediate helpers before narrowing caller-supplied integers.

6502 fluency includes loads/stores, X initialization, stack-pointer transfer,
interrupt control/return, subroutine return/calls, jumps and equality branches.
6809 fluency includes A/B/D/X/S loads, condition-code flags, A/B extended stores,
direct-page X stores, calls/returns, relative branches and indexed clears.
6502 address relocations are little-endian, 6809 address relocations big-endian.
Literal 6809 branch displacements accept exactly `-128..127`, and X-offset
clears accept exactly `-16..15`. `clr_x()` emits `,X`, not an arbitrary postbyte.
`raw_clr_indexed_postbyte()` is deliberately unchecked and may require trailing
bytes supplied by the author.

`label()` creates a named forward reference. Names must be nonempty and unique
per program. `bind()` assigns its current cursor once, and `mark()` creates a
label already bound at that cursor. Foreign and moved-from label handles reject.
The subroutine returns to the jump over its body and then enters the explicit
stopping loop, rather than falling through into a second return.
Labels may outlive their handle's original scope but do not extend layout or
backing lifetime. All created labels (even unused ones) must be bound before
installation. Relative displacement is calculated from the address following
the branch instruction using modular 16-bit PC arithmetic, including branches
across `$FFFF/$0000`, with signed-range checking before any SRAM mutation.
Invalid or wrong-CPU relocation kinds reject before instruction emission.

Programs own their emitted code. Each instruction and compound word/buffer
store checks the entire requested capacity before appending. Full regions,
including the last CPU byte at `$FFFF`, install successfully, but no cursor or
label may refer to the one-past endpoint. `size()` remains valid at capacity.
`raw_bytes()` deliberately bypasses opcode validation, not reservation bounds.
`install()` resolves all references into detached bytes before writing only the
emitted prefix through the layout's lower-bank `poke` operations. Repeated
installation is allowed. Board mutation guards remain active, and invalid
assembly leaves backing memory and simulation time unchanged.

The common `Program<Cpu, Memory>` directly inherits its CPU-specific fluent
surface. Namespace aliases preserve the author-facing names, and shared append
logic delegates to the checked emitter. Mixin ownership/diagnostic helpers are
private implementation plumbing.

Programs cannot copy. Moving a program transfers code and label ownership, and
all source authoring/installation operations reject. Moving the borrowed
layout invalidates the existing program's layout reference rather than silently
rebinding it. Create a new program against the destination layout if needed.

Owned byte loads/stores check layout identity. `store(word, imm16(value))` and
pointer stores emit two real loads/stores using the word's declared byte order,
clobbering A and load-related flags. `store(buffer, bytes)` requires exact input
length. `store_zp()` explicitly selects 6502 zero-page stores and checks both
addresses before emitting anything. These methods emit CPU operations into code
(they do not initialize host data).

[`assembly_test.cpp`](assembly_test.cpp) checks exact encodings, typed surfaces,
labels, signed limits, moves, code bounds, store validation and installation
without a simulator. [`assembly_board_test.cpp`](assembly_board_test.cpp) checks
clock-free lower-bank installation, backing preservation, board evidence and
observation guards. Layout and assembly suites share the simulator-independent
[`host_memory.h`](host_memory.h) test backing.

## Complete checked program execution

Use lvalue-only `system.program6502()` or `system.program6809()` to create a
single-use `ProgramBuilder<Cpu>` backed by `System`. The underlying layout and
assembler remain generic for simulator-independent memory tests.
Direct builder construction enforces the same active-access guard as the
factories, including heap allocation from observation predicates or callbacks.
Construction, reservations and allocation
are clock-free. The board must outlive the builder and its callbacks.

```cpp
auto test = system.program6502();
const auto answer = test.byte("answer");
const auto pointer = test.word("pointer");
const auto input = test.data("input", econopet::CpuAddress{0x400}, 2,
                             test_asm::Capture::Yes);
// Initialize explicit host backing before execution (not a simulated CPU write).
test.lower_bank("input").initialize(input.word(0, test_asm::ByteOrder::LittleEndian),
                                    0x1234);
// Assembly receives a restricted CPU-specific fluent surface.
const auto result = test.within(20000).run([&](auto& p) {
    p.lda(test_asm::imm(0x42)).sta(answer).store(pointer, input.address());
});
EXPECT_EQ(result[answer], 0x42);
EXPECT_EQ(result[pointer], input.address().value());
EXPECT_EQ(result[input], (std::vector<uint8_t>{0x34, 0x12}));
```

`byte()`, `word()` and `buffer()` allocate uniquely named owned operands in the
Results arena, captured by default. `word()` selects the CPU's native byte order.
`data()` reserves external ordinary data, uncaptured by default. Both allocation
surfaces accept `Capture::Yes` or `Capture::No`. `capture()` opts an existing byte,
word or sub-buffer into the result without creating another reservation.
Adjacent, overlapping and duplicate selections are unioned, so a word or buffer
is readable if its complete extent was selected. Holes and foreign owners
reject. Captures sample each selected address once without additional clocks.

`Layout::Pet` defaults to a stack at `$0100` (256 bytes), Results at `$0200`
(255 bytes), a dedicated private completion byte at `$02FF`, Code at `$0300`
(256 bytes), and Vectors at `$FFF0` (16 bytes). `LayoutOverrides` replaces these
extents and adds named Workspace reservations. Every extent is nonempty,
nonwrapping and disjoint. Vectors must contain the selected native reset vector.
The 6502 hardware stack must be exactly `$0100-$01FF`. The 6809 stack uses a
representable one-past descending-stack pointer. `lower_bank(name)` admits only
ordinary Data, Results or Workspace, never the private completion mailbox.

`run()` consumes the builder before invoking assembly. The assembly callback
cannot install, run, finish or allocate data through its receiver. Recursive
runs and subsequent builder mutation reject, including after callback failure.
Assembly may explicitly borrow an ordinary workspace for operands. All labels,
relocations, ownership, terminal capacity and vector ownership validate before
the runner changes memory or clocks. Explicit host writes made by user callbacks
are still user-controlled mutation, not rollback transactions.

The runner masks interrupts and initializes the stack (6502 SEI/LDX/TXS,
6809 ORCC/LDS), appends a private completion store and terminal park, then enters
one elapsed deadline covering reset, CPU selection, installation, vector setup,
release and completion polling. It establishes reset quiescence before writing
code, freshly zeros Results, preserves Data initialization and resets the marker
to pending on every start. The native vector is written through the checked CPU
lifecycle, and release is reported exactly once. Zero budget does not clock or
install anything. Completion at the final allowed observation boundary succeeds.
Timeouts, callback errors and model/transport failures propagate without returning
a success-shaped result. Existing board/SPI reset recovery rules still apply.

`after()`, `every()` and `until()` share the standalone configured-run vocabulary,
including RunEntry and CpuRelease anchors and after-slice active work. Startup
counts against RunEntry schedules, whereas CpuRelease schedules and slices start
only after release. `until()` is an additional condition, never a replacement for
mandatory fresh completion. An independent `system.execution()` configuration
can be passed to `run(assemble, configuration)`, but cannot replace a builder's
authored schedule. `run(assemble, service)` adapts polling work through the same
`RunConfiguration::service()` and terminal execution path, with no separate
budget owner. Per-pass service may share stimulus and `until()` conditions, but
cannot be mixed with periodic or after-slice active work. Invalid service
configuration rejects before assembly. Inherited caller deadlines always win.

Both builders and explicit Scenario execution return `ExecutionResult`, which
owns detached selected bytes, `started()`, `completed()` and a
source-bound `window()` covering the complete run. Operands and captured results
can outlive both board and builder. Missing or foreign operands throw, and
`empty()` identifies a valid run with no selected data. Moving invalidates source
data access. Use `trace.capture(result.window())` to assert CPU I/O during the run.
All captures use one sorted, disjoint, adjacency-merged span representation.
`MemoryCapture::raw_bytes()` deliberately preserves contiguous view capture
behavior (borrow from lvalues, transfer from mutable temporaries, copy from const
temporaries). It also accepts a contiguous execution capture or an empty result,
but rejects disjoint selected spans instead of flattening away address gaps.
The CPU remains released in a terminal park loop after successful completion.

Explicit layouts may issue a one-byte Mailbox `completion()` marker, call
`program.finish(marker)`, and execute with `scenario.run(program, [results,]
budget, reason[, service])` or a `RunConfiguration`. Results may be a Results or
Data reservation, or omitted entirely. Explicit authors supply their own
stack/interrupt prologue if needed. `finish()` checks its whole terminal sequence
before appending, and further instruction emission rejects. `install()` alone
remains clock-free and does not execute even a finished program.

[`execution_core_test.cpp`](execution_core_test.cpp) verifies marker ownership,
terminal encoding, exact capacity, moves and post-finish rejection without
Verilator. [`program_test.cpp`](program_test.cpp), [`memory_test.cpp`](memory_test.cpp)
and [`program_run_test.cpp`](program_run_test.cpp) cover both CPUs, private startup,
captured data, deadlines, configured anchors, actual repeated I/O and failures.
ROM-specific runners remain deferred.

## Cooperative pause/resume

Use `builder.pause(assemble)` instead of `run(assemble)` to preassemble a
cooperative program. The same fluent callback may call `p.pause("name")` between
instructions. Every inline pause, its continuation and the terminal `"complete"`
pause is assembled and validated before clocks or runner memory mutation.
Names must be nonempty, unique and different from `"complete"`. Each boundary
uses one unique nonzero byte token, so at most 254 inline pauses plus the terminal
pause are supported.

```cpp
auto test = system.program6809(test_asm::ProgramMode::Phased);
const auto answer = test.word("answer");
// Assemble both continuations before starting the CPU.
auto first = test.within(20000).pause([&](auto& p) {
    p.store(answer, test_asm::imm16(0x1234)).pause("ready");
    p.store(answer, test_asm::imm16(0x5678));
});
const auto initial = test.capture(first);
EXPECT_EQ(initial[answer], 0x1234);
// Service or observe the board while the CPU polls its closed private gate.
system.tick(500);
auto last = test.resume(first, 20000);
EXPECT_TRUE(last.complete());
EXPECT_EQ(test.capture(last)[answer], 0x5678);
EXPECT_EQ(initial[answer], 0x1234);
```

`pause()` uses the complete runner's reset/selection, result initialization,
code/vector installation and native stack prologue. Its one RunConfiguration
owns the full startup-through-acknowledgment budget. The phase emitter saves A
and flags on the native stack, stores its token in the private acknowledgment
mailbox, then repeatedly loads and compares the private release byte. Opening
the gate restores A and flags before the preassembled continuation. Other
registers are untouched and the stack pointer is restored. A stack must have
space for both saved bytes.

Select `ProgramMode::Phased` at construction, before allocating or binding data.
Default phased Results contain 254 bytes (`$0200-$02FD`), with a separate private
release Mailbox at `$02FE` and acknowledgment Mailbox at `$02FF`.
`LayoutOverrides::results` always denotes ordinary data only. Use
`LayoutOverrides::release` to relocate the separate one-byte release mailbox.
Reservations reject overlapping overrides, workspace aliases and data aliases.
Neither host nor assembly `lower_bank()` views expose either mailbox, and data
allocation/capture need no special gate exclusions. Complete builders retain
255 default Results bytes and reject `pause()` without consuming the builder.
The framework resets Results and both mailboxes before release, and never clears
user data between phases. The terminal handshake itself is the permanent park
(no unreachable ordinary completion sequence is appended).

Acknowledgment requires a fresh real selected-CPU SRAM write of the current
token, followed by a real lower-bank read of the still-closed release gate.
That read must follow a fresh opcode fetch of this phase's preassembled gate-load
after the token write, not arbitrary user-code mailbox traffic or a data read of
the handshake's code. The 6502 uses its opcode history. A simulation-only witness
qualifies the pinned 6809 core's first-byte fetch state. Both are exposed through
`CpuView::peek_opcode_fetched_at(address, checkpoint)`, using the existing
monotonic `peek_fetch_checkpoint()`. The existing `peek_fetched_at()` retains
native 6809 SYNC history (SYNC is an instruction wait, not an opcode-fetch marker).
A mailbox value, opcode-fetch guess or SPI read alone cannot issue a capability.
Skipped phase tokens, stale RAM and host-spoofed acknowledgments cannot replace
the expected bus sequence. After successful acknowledgment, the CPU remains
released and polls its gate while the host may advance clocks.

`PhaseHandle` is move-only and bound to the issuing builder and current phase.
`resume(handle, budget)` opens only that phase, consumes its capability and waits
for the next verified closed gate. It does not reset/select the CPU, patch code,
replace vectors or reinitialize ordinary data. The automatically appended terminal
phase cannot be resumed. Zero resume budget does not open the gate or clock.
Exact final-boundary acknowledgment succeeds, and inherited deadlines always win.
Timeout or escaping callback/transport/model failure invalidates the phase run,
so it cannot subsequently yield successful capture or resume.
Shared phase invariants retain the first reset, selection, transport, clock or
mailbox failure cause. Capture and resume report that cause even if the external
state is subsequently restored.
Escaping clock and SPI exceptions sample board faults before falling back to a
generic execution failure, even during startup before CPU monitoring begins.
The original exception still escapes unchanged. Subsequent capture/resume
reports the retained failure rather than masking it with a consumed-handle error.
Data and acknowledgment timestamps freeze on the observer cycle that verifies
the closed gate, not when `pause()` or `resume()` returns. A multi-cycle service
may finish later, and `until()` may deliberately wait longer. Neither extends
the result/phase windows or resamples data modified after acknowledgment.

`pause(assemble, configuration)` and `resume(handle, configuration)` accept fresh
board-owned RunConfigurations. Startup RunEntry anchors precede reset work,
whereas startup CpuRelease anchors begin at checked CPU release. On resume,
RunEntry is resume entry and CpuRelease is the gate opening (not a new CPU reset).
`until()` is an extra observation condition, never a replacement for verified
park. Periodic, after-slice and per-pass services retain their configured-run
semantics. `pause(assemble, service)` and `resume(handle, budget, service)` adapt
polling work through the same configuration path. Invalid foreign, missing-budget
or conflicting configurations reject before releasing a valid capability.
Owned/nonidle SPI transport also rejects before gate opening.

Reset or CPU-selection changes invalidate the monitored lifecycle, even if later
restored. Unclocked external reset changes, failed clocks/transport, altered
acknowledgments and externally opened gates also reject. Detected lifecycle/gate
failure is sticky. Resume and builder construction remain active operations
forbidden from observation callbacks, while frozen phase capture is read-only.
Destroying the builder unsubscribes its monitor without opening the gate.
As with complete execution, builders borrow a persistent board.

`capture(handle)` copies the selected `ExecutionResult` sampled exactly at the
verified acknowledgment (no clocks or later resampling). The result's timestamps
and window cover original startup through that boundary, while the handle's
`window()` covers only the current startup/resume interval, excluding host dwell.
Use `trace.capture(handle)` for per-phase I/O assertions, or
`trace.capture(test.capture(handle))` for cumulative execution evidence.
Detached captures and copied windows outlive the builder and board. Consumed
handles retain their diagnostic names/windows but cannot authorize capture or
another resume.

[`phase_core_test.cpp`](phase_core_test.cpp) checks exact handshake encoding,
native-endian gate addresses and atomic capacity without Verilator.
[`phase_test.cpp`](phase_test.cpp) executes both CPU handshakes and exercises
register/stack preservation, tokens, stale/spoofed acknowledgment rejection,
capability lifetimes, reset invalidation and exact deadlines.
[`phase_integration_test.cpp`](phase_integration_test.cpp) covers owned capture,
configured startup/resume, actual peripheral traces, closed-gate host intervals
and cross-layer failure contracts. ROM fixtures and ROM-specific phases remain
deferred to their consuming layers.

## Shared hardware contract

[`hardware_contract.h`](../../fw/src/hardware_contract.h) owns the
SDK-independent C/C++ hardware encodings used by production firmware.
Corresponding definitions in
[`common_pkg.sv`](../../gw/EconoPET/src/common_pkg.sv) have the same names
without the `ECONOPET_` C namespace prefix and are grouped between
`BEGIN SHARED HARDWARE CONTRACT` and `END SHARED HARDWARE CONTRACT`.
`_WIDTH` counts bits, `_BIT` is a bit index, `_MASK` is a bit mask, and
`_ADDR` is a full byte address. `REG_*` values are indices, and
`WB_*_DECODE_PREFIX` values are decode prefixes (not full addresses).
`WB_*_BASE_ADDR` values are full window addresses.

The C header exports values used by firmware and the host framework, not every
RTL definition. Decode prefixes and other RTL-only construction details stay
in `common_pkg.sv`. The contract covers data/address widths, Wishbone windows,
register addresses, control/status masks, CPU selections, configuration pin levels,
SPI encodings, IEEE register bounds and addresses, and the
IEEE TX burst capacity guaranteed by the room indication. Control-read and
control-write masks are distinct. IEEE status bit 7 means talk starvation,
not RX EOI. Firmware policy such as `CPU_AUTO` is not a hardware value.
Firmware display and keyboard model enums use the shared configuration pin
encodings. The driver's `read_pet_model` stores the decoded status bits directly,
while RTL uses the named CRT level for fixed timings and output polarity.
Keyboard status forwards the raw pin level without interpreting it.

USB keymap files use the same business-first, graphics-second model order,
so firmware indexes them directly with the keyboard model enum. Install the
regenerated keymaps together with the aligned firmware. Older graphics-first
files are incompatible. Startup defaults remain Graphics keyboard and Fixed
display.

Use unsigned literals for every C contract definition, including addresses,
masks, and counts. Keep derivations in RTL, where applicable. The consistency
test checks the compiled C values against elaborated RTL, keeping macro expansion
simple and hardware values easy to inspect without duplicating construction logic.

The video RAM field mask derives from both RTL bit boundaries and is used by
firmware to reject values that would spill outside the field before encoding.
This checks the complete field layout without exporting an unused high-bit index.

`system.HardwareContract.MatchesCommonPackage` compares compiled C values
with independently elaborated SystemVerilog values using a CMake runner.
Discovery uses the GNU/Clang C preprocessor, accepting legal whitespace,
comments, and line continuations. Every exported `ECONOPET_*` macro must be
object-like, use an uppercase name, and contain an unsigned decimal or hexadecimal
literal with a `u` suffix. Unsupported exports fail configuration instead of
escaping coverage. A missing RTL counterpart fails compilation and a changed
value fails CTest. `system.HardwareContract.Discovery` tests these discovery rules.
Changing either file rebuilds the relevant evaluator.

Run through CTest so the CMake runner checks evaluator execution and compares
their complete named output:

```sh
cmake --build build/host-fixtures
ctest --test-dir build/host-fixtures \
  -R '^system\.HardwareContract\.' --output-on-failure
```

The generated `build/host-fixtures/hardware_contract_values.c` and
`build/host-fixtures/hardware_contract_tb.sv` files expose each evaluated
constant for review. The comparison itself is in
[`test.cmake`](hardware_contract/test.cmake).

## Checked framework value types

Include [`types.h`](types.h) for the standalone C++23 types in `econopet`.
Construction and arithmetic checks throw standard exceptions independently
of `NDEBUG`.
They do not depend on `assert`, board state, or a
firmware fatal handler. [`types_test.cpp`](types_test.cpp) tests these contracts
as the independently runnable `BoardTypes` suite:

```sh
ctest --test-dir build/system -R '^system\.BoardTypes\.' --output-on-failure
```

Use `build/host-fixtures` instead for the standalone build. Both suites remain
part of `ctest --preset sys`. Use standard facilities such as `std::span` and
`std::to_underlying` rather than equivalent custom utilities.
Hardware encodings come from the production contract, with independent numeric
expectations alongside the C/RTL consistency tests. Durations and borrowed byte
views are framework utilities, not hardware encodings.

### Address domains

`CpuAddress`, `SramAddress`, and `WishboneAddress` are distinct types with capacities
of 64 KiB, 128 KiB, and 1 MiB respectively. Integer construction is explicit,
rejects negatives and values outside the domain before narrowing, and permits
no implicit conversion between domains or back to integers. Use `value()` at
an encoding boundary.

Adding an integer offset checks both endpoints, including extreme signed and
unsigned inputs. Subtracting two addresses in the same domain returns a signed
displacement. Invalid construction or offsets throw `std::out_of_range` with
the domain name, address, and (for offsets) operand.

The Wishbone domain is the FPGA's byte-addressed bus space, accessed over SPI,
not a separate SPI address space. SPI and peripheral register mappings belong
to later framework layers.

Fixture code can explicitly choose lower-bank SRAM backing with
`SramAddress{cpu_address.value()}`. This copies the CPU address bits, not a
live CPU bank translation or I/O decoder.

### Durations and timestamps

`Cycles` measures full system-clock cycles, not CPU PHI2 cycles. Integer
durations are checked and may convert implicitly for concise duration
arguments. `Maximum` leaves room for two simulation half ticks per cycle.
`half_ticks()` converts without overflow, and `from_half_ticks()` rejects an
incomplete cycle with `std::logic_error`.

Duration addition and multiplication throw `std::overflow_error` on overflow.
Multiplication accepts only integral operands and rejects signed negatives with
`std::out_of_range`, even for a zero duration. Floating-point multipliers do not
compile.
Subtraction throws `std::out_of_range` on underflow. `CycleTime` is an explicit
timestamp constructed from elapsed `Cycles`, not an interchangeable duration
or integer. Add a duration to obtain a deadline, or subtract ordered timestamps
to obtain elapsed cycles. Stream diagnostics include units.

### Named flags and encodings

Use the production `cpu_type_t` from [`driver.h`](../../fw/src/driver.h) and
`pet_video_type_t` and `pet_keyboard_model_t` from
[`system_state.h`](../../fw/src/system_state.h) directly. Hardware CPU-selection
APIs must reject `CPU_AUTO`, which is firmware policy, and reserved values.
USB keymap files use
business-first, graphics-second model order and firmware indexes them directly
with the keyboard model enum.
`std::to_underlying(enum_value)` from `<utility>`
extracts an enum's underlying integer at a hardware boundary, but does not
validate arbitrary enum casts. The production enums have non-fixed underlying
types, so casting integers outside their representable enum ranges is undefined
behavior in C++.

`Flags<Bit>` represents a byte-sized flag domain whose enum declares `All`.
`CpuControl` uses `CpuControlBit::{Ready, Reset, Nmi}`. Combine named bits with
`|`, query individual or composite masks with `contains`, and use `bits()` at a
wire boundary. `from_bits()` and enum construction reject unknown bits and
values exceeding byte capacity with `std::invalid_argument` before narrowing,
even when a wider enum's `All` mask includes high bits. Different flag domains
cannot mix.

### Borrowed bytes

`ByteView` is a checked wrapper around `std::span<const uint8_t>`. It borrows a
byte C array, `std::array`, `std::vector`, or mutable/immutable byte span with
its extent. C arrays, standard arrays, and vectors must be mutable or const
lvalues, including empty standard containers. Both mutable and const rvalue
arrays and vectors are rejected to prevent borrowing storage that expires with
the construction expression. Fixed-extent spans and span subranges are supported, including
temporary span objects whose backing storage remains alive. It deliberately
has no direct raw pointer/count constructor. A span input supplies its own
extent, whose validity and backing storage lifetime remain the caller's
responsibility.

`size()` reports the borrowed extent and indexed reads throw
`std::out_of_range` outside it, including for empty spans and containers.
These checks do not depend on `NDEBUG` (C++23's plain `std::span`
indexing does not provide this exception contract). Keep the source storage
alive and do not resize or otherwise invalidate it while the view is in use.
The view does not own or freeze its bytes, and accepting a span does not extend
the source lifetime.

## Reusable disk-image fixtures

[`d64.h`](../support/d64.h) owns a checked 35-track D64 image independently of
the board, firmware transport, filesystem mocks, and test framework.

Geometry comes from the pure production helpers in
[`diskimage.c`](../../fw/src/ieee/diskimage.c). Image sizes,
sector layout, directory slot sizes, and file types use
[`diskimage.h`](../../fw/src/ieee/diskimage.h). Production geometry is tested
directly against independent ROM-derived zones and fixed byte offsets in
[`diskimage_test.c`](../../fw/test/diskimage_test.c), not only through fixture
round trips. The 8050 D80 checks use the single-sided `NTRK80`/`NSEC80` tables in
[DOS 2.7 ROMTBL](https://github.com/mist64/cbmsrc/blob/master/DOS_8250/romtbl).
Formatting, BAM allocation, file installation, and corruption remain test-only.

Fixture internal invariants use `vet` for consistent fatal diagnostics,
independent of `NDEBUG`. These framework-independent checks reuse the
SDK-independent host fatal implementation in
[`test_fatal.c`](../support/test_fatal.c), which checks expected diagnostics and aborts.

```cpp
#include "d64.h"

using disk_fixture::D64;

auto disk = D64::empty();
disk.prg("BASIC", {0x42, 0x43, 0x44, 0x45});
const auto& image = disk.bytes();
```

Construct `D64{}` or the explicitly named `D64::empty()`. Both produce a
formatted 174848-byte image with BAM, disk name `TEST DISK`, ID `00`, and an
empty directory.

Add files with `prg(name, bytes)`. Each call encodes a closed PRG directory
entry, block count, chain links, exact final-sector length, and BAM allocation.
Payload bytes are preserved exactly (no implicit PRG load address). Zero-byte
payloads have one empty terminal sector, a synthetic fixture representation
rather than the CR that native DOS writes when closing an unwritten file.
`seq(name, bytes)` and `usr(name, bytes)` share this contract, with closed
directory type bytes `$81` and `$83` respectively.

Read immutable storage through `bytes()` for independent assertions or a
consumer that copies the image. On a named lvalue fixture, `bytes()` borrows
storage for the fixture's lifetime. On a mutable rvalue it returns an owned
vector and invalidates the fixture, and on a const rvalue it returns an owned
copy. Fluent file and corruption methods preserve rvalue ownership, so
`D64::empty().prg("HELLO", payload).bytes()` also returns an owned vector.
Keep that vector in a named variable before passing it to `ByteView`.

Copies own independent images and allocation state.
Move construction and assignment explicitly invalidate the source and
empty its bytes. Moved-from fixtures reject further construction or corruption,
including after copying or moving that invalid state. Assigning a valid fixture
restores usability. Self-move assignment leaves the fixture unchanged.

Names contain 1..16 printable ASCII characters, excluding DOS delimiters
`:,=*?"`. The existing pure PETSCII encoder folds letters into uppercase and
maps punctuation (for example `_` becomes `$A4`). Names are padded with `$A0`.
This is an ASCII authoring interface, not the production lookup encoding.
`diskimage_find()` accepts PETSCII names received from IEEE traffic and does not
transcode ASCII punctuation. To query a fixture using its authoring name, call
`d64_fixture_name()`, copy the encoded bytes before the first `$A0` padding byte
into a NUL-terminated query, and pass that PETSCII query to production lookup.
Duplicate encoded names, directory exhaustion (144 files), data exhaustion
(664 sectors), and oversize payloads reject before changing bytes or ownership.
Directory storage uses track 18. Data allocation is deterministic (tracks
17 down to 1, then 19 through 35) but does not emulate drive interleave.
The 21/19/18/17-sector zones follow 4040 DOS 2
[`TRKTBL` and `SECTRK`](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/romtbl).

For deliberately malformed media, build valid files first, then call
`corrupt(byte_offset, bytes)` to replace a checked byte range.
`D64::offset(track, sector)` validates coordinates. Further checked file
installation rejects after corruption, because the metadata can no longer
serve as allocation authority. Empty corruption ranges are no-ops, including
at image end. Consumers may still read the raw bytes, with no implicit repair.

Firmware C unit tests use the same
[`d64_fixture.h`](../support/d64_fixture.h) construction core and diagnostics.
Check every returned diagnostic (`NULL` means success). The C API also permits
explicit checked sector lists for sparse-chain tests. Nonempty payloads that
overlap image storage reject before mutation. Zero-length payloads may alias.
Build ordinary files before applying direct
malformed-media patches. The specialized REL fixture builds a checked sparse
USR chain, then explicitly patches its type and record length for whitebox
tests (no REL side sectors).

D80 and extended/error-table D64 construction are not supported by this builder.

The fixture format rules use these sources (no production protocol change):

| Behavior | Target peripheral/ROM | Primary source and routine | Required fixture result | EconoPET code and test |
|----------|-----------------------|----------------------------|-------------------------|-----------------------|
| Empty directory and BAM | 4040 DOS 2 | [NEW, N110/NEWMAP/USEDTS](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/new), [FRETS](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/frets) | Directory 18/1 ends with `0,$FF`, 18/0 and 18/1 are allocated, free counts agree with bitmaps | [Shared core](../support/d64_fixture.c), `DiskFixture.EmptyIsFormattedWithoutDirectoryEntries` |
| Directory name and closed PRG metadata | 4040 DOS 2 | [ADDFIL AF20/AF25](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/addfil), [TRNAME TN10](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/trnsfr), [CLSDIR CLSD5/CLSD6](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/close) | Type `$82`, 16-byte `$A0` padding, start T/S and little-endian sector count | [Fixture tests](disk_fixture_test.cpp), `PrgOwnsNamePayloadAndDirectoryMetadata` |
| Nonempty final-sector length | 4040 DOS 2 | [CLSWRT CLSW20](https://github.com/mist64/cbmsrc/blob/master/DOS_4040/close) | Final track zero, byte 1 is payload length plus one (255 for 254 bytes) | [Parser round trip](../../fw/test/diskimage_test.c), `test_shared_d64_builder_round_trips_files` |

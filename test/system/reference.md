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

The firmware Check tests also use the shared C builder. Build and run those
with `cmake --build --preset fw-test` and `ctest --preset fw`.

Keep regression tests in the suite for the feature they exercise. Register
expected-abort tests with the forked runner, using a separate fatal suite in the
same feature test file when its normal suite runs without forking.

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

Trace publication, board wiring, and real-CPU bus/IRQ integration tests belong
to later incremental layers.

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

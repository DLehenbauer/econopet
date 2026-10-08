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

The shared D64 fixtures, checked framework value types, and their C++ tests run
without Verilator, a simulated board, ROM media, or firmware transport.

Standalone builds require CMake 3.20 or later.

The hardware contract consistency test requires Icarus Verilog (`iverilog`
and `vvp`). It elaborates only the production constants, not a simulated board.

```sh
cmake -S test/system -B build/host-fixtures -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host-fixtures
ctest --test-dir build/host-fixtures --output-on-failure
```

The firmware Check tests also use the shared C builder. Build and run those
with `cmake --build --preset fw-test` and `ctest --preset fw`.

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
not RX EOI. Firmware policy such as `CPU_AUTO`, and firmware model enums with
different encodings from configuration pins, are not hardware values.
Firmware maps configuration pin levels through `system_state_set_config_pins`,
while RTL uses the named CRT level for fixed timings and output polarity.
Keyboard status forwards the raw pin level without interpreting it.

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
Subtraction throws `std::out_of_range` on underflow. `CycleTime` is an explicit
timestamp constructed from elapsed `Cycles`, not an interchangeable duration
or integer. Add a duration to obtain a deadline, or subtract ordered timestamps
to obtain elapsed cycles. Stream diagnostics include units.

### Named flags and encodings

`Cpu`, `Display`, and `Keyboard` name production CPU selections and
configuration pin levels. `std::to_underlying(enum_value)` from `<utility>`
extracts an enum's underlying integer at a hardware boundary, but does not
validate arbitrary enum casts.

`Flags<Bit>` represents a byte-sized flag domain whose enum declares `All`.
`CpuControl` uses `CpuControlBit::{Ready, Reset, Nmi}`. Combine named bits with
`|`, query individual or composite masks with `contains`, and use `bits()` at a
wire boundary. `from_bits()` and enum construction reject unknown bits with
`std::invalid_argument` before narrowing. Different flag domains cannot mix.

### Borrowed bytes

`ByteView` is a checked wrapper around `std::span<const uint8_t>`. It borrows a
byte C array, `std::array`, `std::vector`, or mutable/immutable byte span with
its extent. Fixed-extent spans and span subranges are supported. It deliberately
has no direct raw pointer/count constructor. A span input supplies its own
extent, whose validity remains the caller's responsibility.

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
[`test_fatal.c`](../support/test_fatal.c), which logs and aborts on invariant failures.

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
consumer that copies the image. Copies own independent images and allocation
state. Move construction and assignment explicitly invalidate the source and
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

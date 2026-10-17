# EconoPET integration tests

Write ordinary tests with a complete CPU-specific builder, named memory
handles, an explicit budget, and an assembly lambda. Keep assertions outside
the lambda as ordinary GoogleTest assertions:

```cpp
#include "framework/firmware_test.h"

using test_asm::imm;
using test_asm::Layout;

// Execute a real 6502 SRAM store and enable the firmware drive.
TEST_F(FirmwareTest, ProgramAndDrive) {
    auto test = system.program6502(Layout::Pet);
    const auto answer = test.byte("answer");
    const auto result = test.within(econopet::Cycles{20000}).run([&](auto& p) {
        p.lda(imm(0x42)).sta(answer);
    });
    EXPECT_EQ(result[answer], 0x42);

    firmware.mount(disk_fixture::D64::empty());
    EXPECT_TRUE(system.ieee().read_control().contains(fpga::IeeeControlStatusBit::Enabled));
}
```

The builder owns stack, code, vectors, private completion, and result capture.
It validates assembly before board mutation, establishes reset before
installation, and shares one deadline across startup and execution. No
completion byte or reset-vector reservation belongs in this ordinary test.
The full [smoke test](econopet_top_smoke.cpp) also checks CPU-selection readback
through production SPI/Wishbone.

These tests instantiate the complete production FPGA. Firmware integration
runs selected real host-compiled firmware modules through FPGA SPI pins
(not RP2040 SSP/DMA execution). Full instantiation does not imply complete
physical-board fidelity. Consult the [capability inventory](reference.md#external-piavia-models)
before designing a scenario.

## Build and run

Production uses CMake `RelWithDebInfo` (optimized code with debug symbols).
Tests, simulator integration, frameworks, and their dependencies use `Debug`,
including production modules compiled for host tests. Use GoogleTest or Check
assertions for test expectations, `vet()` for production-enforced invariants,
and `assert()` for test-covered paranoid production checks. Do not undefine
`NDEBUG` or put required work or side effects inside assertions. Host fatal
checks use `test/support/test_fatal.c`. The host framework requires C++23.
Build Verilator-generated models and runtime separately with optimized
`RelWithDebInfo` flags and SV assertions enabled. Keep linked test, framework,
and firmware sources in `Debug`, without model optimization flags or `NDEBUG`.

```sh
# Initialize the recorded dependency revisions, not their current branch tips.
git submodule update --init --recursive

# Configure and build host, board and isolated firmware executables.
cmake --preset default
cmake --build --preset sys-test --parallel 2

# Run the complete nested suite through the root wrapper.
ctest --preset sys --output-on-failure --no-tests=error

# Rebuild after edits, then select individual cases in the nested build.
cmake --build --preset sys-test --parallel 2
ctest --test-dir build/system --parallel 2 --output-on-failure --no-tests=error \
  -R 'SpiRegistersCpuAndDriveMount|waterloo_atn_down_case'

# List nested cases without executing them. CTest does not build.
ctest --test-dir build/system -N
```

Set `ECONOPET_MEDIA_DIR` before configuring. ROM cases require legally
supplied images under its `roms/` directory. Historical `DISABLED_` cases stay
excluded. The root preset selects the `system_tests` wrapper, not individual
`firmware.*` or `board.*` names. Use `--test-dir build/system` for case filters
or bounded inner parallelism. Each discovered case runs in an isolated process.
The wrapper has a 30-minute aggregate budget, while individual host, board and
firmware cases retain five-minute limits.

Requirements, standalone/offline builds, optional board integration, and
seed replay are in the [build reference](reference.md#building-fixture-tests).

## One authoring style

- Use `SystemTest` for a fresh board, or `FirmwareTest` when firmware is needed.
  Include the corresponding fixture header. Both expose `system`, and the latter
  also exposes `firmware`.
- Use `system.program6502(Layout::Pet)` or `system.program6809(Layout::Pet)`,
  or a revision-bound ROM fixture's `program()`. Name results with `byte`,
  `word`, or `buffer`, then assert `result[handle]`. Allocate nothing for a
  run that needs no captured data.
- Chain short configuration and instruction sequences. Use familiar mnemonics
  with typed operands (`imm`, `zp`, checked absolute addresses, and labels).
  Absolute addressing stays absolute, never silently shortened to zero page.
- Use lambdas for owned assembly, stimulus edits, and checked SPI transaction
  scopes. Configuration prepares work, and a terminal operation executes it.
  Do not clock or modify the board from an assembly lambda.
- Use `within`, `after`, `every`, `until`, and terminal `run` for execution.
  Use `pause` and `resume` for cooperative phases. Do not introduce another
  scheduler or a hand-written completion loop for ordinary integration tests.
- Keep enum categories and named flags (`KernalRoutine`, `ServiceTiming`,
  `Eoi`, CPU selection). Payload bytes and expected numeric results can remain
  literals such as `0x42` or `'A'`.
- Use `read` for bus operations and `peek` for passive observations. A getter-like
  SPI read still consumes simulation time.
- Leave expectations as `EXPECT_*`, `ASSERT_*`, or `EXPECT_THAT`. Active helpers
  such as FIFO draining are actions, not passive assertions.

Add a source to the existing executable in [board.cmake](board.cmake) or
[firmware.cmake](firmware.cmake), or the host-only executable in
[CMakeLists.txt](CMakeLists.txt). Keep suite/case names unique across
executables. Follow the [existing smoke test](econopet_top_smoke.cpp) rather
than creating another convenience spelling.

The representative cases retain their original subjects and expectations:

| Example | Real case | Status |
|---------|-----------|--------|
| Smoke program | [SpiRegistersCpuAndDriveMount](econopet_top_smoke.cpp) | Enabled |
| ROM leaf | [waterloo_atn_down_case](ieee/waterloo_rom_test.cpp) | Enabled |
| Serviced transaction | [StreamsDosStatusOverProductionTop](ieee/drive_test.cpp) | Enabled |
| Multi-phase program | [ProgramPhases6502/6809PreserveAccumulatorAndOrder](phase_test.cpp) | Enabled |
| Multi-phase ROM transaction | [DISABLED_Rom2_CIOUT_buffered_EOI](ieee/commodore_rom_test.cpp) | Disabled |

## Peripheral write traces

Use `system.trace_pia1_writes()`, `system.trace_pia2_writes()`, and
`system.trace_via_writes()` to record one fitted device's completed model
writes. Each recorder subscribes independently and retains events across
resets and observation clearing. Use `trace_peripheral_writes()` instead
when the evidence must come from CPU bus writes rather than direct model setup.

## Call a ROM routine

A ROM fixture binds the revision, CPU, entries, and workspace. Initialization
is explicit and emits real CPU instructions. This is the body of the active
[Waterloo ATNDown case](ieee/waterloo_rom_test.cpp):

```cpp
using namespace ieee_test;
firmware.mount(disk_fixture::D64::empty(), "kernal.d64");
WaterlooFixture rom(system);
constexpr uint8_t ViaExternalIdle = pet::Ndac | pet::NrfdInput | pet::Dav;
system.io().via().inputs([](auto& inputs) { inputs.port_b = ViaExternalIdle; });

auto test = rom.program();
test.within(WaterlooBudget).run([&](auto& p) {
    rom.init_ieee(p);
    p.jsr(rom.entry(WaterlooRoutine::AtnDown));
});
EXPECT_EQ(system.ieee().peek_via_orb() & ViaIdle, pet::Nrfd);
EXPECT_NE(system.io().pia2().state().pa.cr & M6520_CR_C2_MODE, 0);
```

Include [ieee/fixture.h](ieee/fixture.h). Commodore uses
`KernalFixture rom(system, System::RomSet::Rom4)` and `KernalRoutine` in the
same pattern. `rom.device(p)` and `rom.file(p, bytes)` emit setup stores, not
host substitutes. A test of `IEEEInit` itself calls that entry directly,
without an extra `rom.init_ieee(p)`.

## Service firmware during execution

Attach `firmware.service_adapter()` directly to the run configuration (or a
CPU/ROM builder when servicing a program). The active
[DOS-status transaction](ieee/drive_test.cpp) sends real IEEE commands, services
the drive through SPI, and checks every returned byte and its EOI:

```cpp
auto rx = firmware.trace_rx();
controller.command(Talk(Device{DeviceAddress}));
controller.command(Secondary(Channel{CommandChannel}));
system.execution().within(ByteBudget)
    .service(firmware.service_adapter())
    .until([](const System& board) { return !board.snapshot().ieee_rx_ready_o; })
    .run("service status-channel transaction");
EXPECT_THAT(rx.records(), testing::ElementsAre(
    fifo_record(command(Talk(Device{DeviceAddress}))),
    fifo_record(command(Secondary(Channel{CommandChannel})))));
```

This excerpt starts after the mounted file's OPEN transaction has been serviced,
as shown in the full test. Include `<gmock/gmock.h>`,
[ieee/controller.h](ieee/controller.h), and
[ieee/trace.h](ieee/trace.h). The pending commands ensure the first service
pass is required. See the
[transport reference](reference.md#host-firmware-through-production-spi) for
recorder ownership and supported FIFO evidence.

For periodic CPU work, consult the
[execution reference](reference.md#deterministic-scheduling-and-configured-runs).
The [Waterloo coverage reference](reference.md#waterloo-rom-integration-coverage)
records the historical termination cases' preserved setup and after-slice policy.

## Inspect a cooperative phase

Preassemble dependent calls and put `p.pause(name)` between them. In the
historical [ROM 2 CIOUT case](ieee/commodore_rom_test.cpp), LISTEN/secondary
addressing, buffered writes, and final UNLISTEN remain in one CPU session:

```cpp
using namespace ieee_test;
constexpr econopet::Cycles BufferedByteBudget = 100000;
KernalFixture rom(system, System::RomSet::Upgrade);
auto wire = system.trace_ieee();
auto test = rom.program(test_asm::ProgramMode::Phased);
auto addressed = test.within(KernalBudget).pause([&](auto& p) {
    rom.init_ieee(p);
    rom.device(p);
    p.lda(imm(0)).jsr(rom.entry(KernalRoutine::Listen))
     .lda(imm(IEEE_CMD_SECONDARY(FileChannel))).jsr(rom.entry(KernalRoutine::Secondary));
    p.pause("listener addressed");
    p.lda(imm(0)).sta(test_asm::zp(KernalWorkspace::BufferedByte))
     .lda(imm(0x41)).jsr(rom.entry(KernalRoutine::Ciout));
    p.pause("first byte buffered");
    p.lda(imm(0x42)).jsr(rom.entry(KernalRoutine::Ciout));
    p.pause("first byte sent");
    p.jsr(rom.entry(KernalRoutine::Unlisten));
});
ASSERT_EQ(addressed.name(), "listener addressed");
std::vector<Record> expected{
    command(Listen(Device{DeviceAddress})), command(Secondary(Channel{FileChannel}))};
require_drained_sequence(system, fifo_records(expected), ByteBudget, "addressing");
EXPECT_THAT(wire.records(), testing::ElementsAreArray(expected));
auto buffered = test.resume(addressed, BufferedByteBudget);
ASSERT_EQ(buffered.name(), "first byte buffered");
require_drained_sequence(system, {}, ByteBudget, "first CIOUT must only buffer");
EXPECT_THAT(wire.records(), testing::ElementsAreArray(expected));
auto transmitted = test.resume(buffered, BufferedByteBudget);
ASSERT_EQ(transmitted.name(), "first byte sent");
require_drained_sequence(system, fifo_records({data(0x41)}), ByteBudget, "first byte");
expected.push_back(data(0x41));
EXPECT_THAT(wire.records(), testing::ElementsAreArray(expected));
auto flushed = test.resume(transmitted, BufferedByteBudget);
ASSERT_TRUE(flushed.complete());
require_drained_sequence(system, fifo_records({data(0x42, Eoi::Last), command(Unlisten)}),
    ByteBudget, "final flush");
expected.push_back(data(0x42, Eoi::Last));
expected.push_back(command(Unlisten));
EXPECT_THAT(wire.records(), testing::ElementsAreArray(expected));
```

This body assumes the mounted `FirmwareTest` setup used by the ROM suite.
Include `<vector>`, `<gmock/gmock.h>`, [ieee/fixture.h](ieee/fixture.h),
[ieee/fifo.h](ieee/fifo.h), and [ieee/trace.h](ieee/trace.h), and import
`test_asm::imm`. The historical test remains disabled. A phase handle proves acknowledgment
followed by a safe parked read, not just a mailbox write. `resume` releases
the private gate without resetting CPU or IEEE state. Use
`test.capture(phase)[handle]` for detached data at a verified boundary.
The [phase reference](reference.md#cooperative-pauseresume) covers ownership,
per-resume budgets, and custom scheduled work.

## Ownership and borrowed views

Keep a named `System` alive while using its bound views, program builders or
execution configuration. Consult the authoritative contracts for
[board proxies and SPI callbacks](reference.md#checked-spi-and-cpu-lifecycle),
[recorders and detached captures](reference.md#owned-traces), and
[borrowed byte storage](reference.md#borrowed-bytes) before retaining a view.

## Whitebox observations and interrupts

Choose a recorder before execution, then bind its capture to a result or phase:

```cpp
auto writes = system.trace_peripheral_writes();
auto test = system.program6502(Layout::Pet);
const auto result = test.within(econopet::Cycles{20000}).run([&](auto& p) {
    p.lda(imm(0x55)).sta(pet::ViaPortB);
});
const auto captured = writes.capture(result);
ASSERT_TRUE(captured.complete());
ASSERT_EQ(captured.records().size(), 1);
EXPECT_EQ(captured.records().front().address, pet::ViaPortB);
EXPECT_EQ(captured.records().front().data, 0x55);
```

Include [framework/trace.h](framework/trace.h) when using detached capture helpers.
Use `snapshot()`, const chip state, and `peek` for passive checks, not generated
Verilator members or forced RTL state. Exact absence/count/order requires a
capture with complete interval coverage.

Interrupt-entry scenarios need separate main and handler programs, an explicit
IRQ vector, modeled interrupt stimulus, handler acknowledgment and RTI.
Do not replace those subjects with host interrupt-flag writes or a builder's
normal completion sequence. Keep explicit scenarios for unusual reset,
self-modifying code and boundary tests, not as a second ordinary style.

## Reference

- [Complete builders and explicit scenarios](reference.md#complete-checked-program-execution)
- [Layouts and owned memory](reference.md#checked-layouts-and-owned-memory)
- [Execution, anchors, and service timing](reference.md#deterministic-scheduling-and-configured-runs)
- [Lifecycle, stimulus and SPI scopes](reference.md#checked-spi-and-cpu-lifecycle)
- [Scoped traces and temporal evidence](reference.md#owned-traces)
- [Reusable checked D64 fixtures and deliberate corruption](reference.md#reusable-disk-image-fixtures)
- [Firmware SPI transport coverage](reference.md#host-firmware-through-production-spi)
- [Capabilities and external-device fidelity](reference.md#external-piavia-models)
- [Failure diagnostics and reproducible seeds](reference.md#observation-deadlines-and-failures)
- [ROM/IEEE fixture and transaction details](ieee/README.md)

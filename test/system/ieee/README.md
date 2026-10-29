# IEEE integration recipes

Use the [system author guide](../README.md) for build commands and fixture
setup. The [reference](../reference.md) is authoritative for API contracts,
evidence limits and enabled/disabled coverage. This guide shows how to apply
those contracts without maintaining a second inventory.

## Send and check a transaction

Construct the CPU-backed controller before enabling the firmware drive. This
complete test sends a filename transaction and checks both observation sources
without asking a listener to return data:

```cpp
#include <vector>

#include <gmock/gmock.h>

#include "framework/firmware_test.h"
#include "ieee/controller.h"
#include "ieee/fifo.h"
#include "ieee/trace.h"

using namespace ieee_test;

// Check the OPEN frame without executing drive-side filename handling.
TEST_F(FirmwareTest, SendsOpenTransaction) {
    IeeeController controller(system);
    firmware.mount(disk_fixture::D64::empty());
    auto wire = system.trace_ieee();

    // Send the complete listener transaction before inspecting its records.
    controller.command(Listen(Device{8}));
    controller.command(Open(Channel{2}));
    controller.data('A');
    controller.data('B', Eoi::Last);
    controller.command(Unlisten);

    // Compare wire flags, then explicitly project the expectation for the FIFO.
    const std::vector<Record> expected{
        command(Listen(Device{8})), command(Open(Channel{2})),
        data('A'), data('B', Eoi::Last), command(Unlisten)};
    EXPECT_THAT(wire.records(), testing::ElementsAreArray(expected));
    require_drained_sequence(system, fifo_records(expected), ByteBudget, "OPEN frame");
}
```

This recipe deliberately drains RX itself, rather than servicing firmware.
It checks transmission, not successful file opening. See the
[wire/FIFO contract](../reference.md#ieee-controller-and-protocol-evidence)
for the projection and active-drain semantics.

## Receive data from firmware

Follow [StreamsDosStatusOverProductionTop](drive_test.cpp) for a complete
receive sequence: mount media, send and service OPEN, address TALK with its
secondary, service firmware to supply outgoing bytes, then call
`controller.receive()` and check the returned bytes against the wire trace.
Do not append `receive()` to the listener-only recipe above.

Use the [service recipe](../README.md#service-firmware-during-execution) for
bounded cooperative work. The authoritative
[transport contract](../reference.md#host-firmware-through-production-spi)
defines what the host-compiled firmware and SPI adapter execute.

## Call a selected ROM entry

Use `KernalFixture` or `WaterlooFixture` from [fixture.h](fixture.h), select
`program()`, emit explicit initialization and routine calls, then assert the
result or transaction. Start with the
[native ROM-call recipe](../README.md#call-a-rom-routine).

For dependent calls with host observations between them, use the
[CIOUT phase recipe](../README.md#inspect-a-cooperative-phase). Keep first-call
buffering separate from second-call transmission and the final EOI flush.
Use the [revision-bound ROM contract](../reference.md#revision-bound-rom-programs)
for image identity, workspace, initialization and Waterloo hook requirements.
Unusual raw-installation scenarios should retain their explicit lifecycle
rather than being rewritten as ordinary recipes.

## Capture and replay an original-drive reference

Use the [drive reference toolkit](../../../tools/drive-ref/README.md) to build
instrumented MAME, capture approved original-ROM transactions, and compare their
replay through production SPI, firmware, and FPGA simulation. Its README covers
prerequisites, commands, artifacts, tests, and evidence limits.

## Find the authoritative detail

- [Controller, transaction values, FIFO operations and wire evidence](../reference.md#ieee-controller-and-protocol-evidence)
- [Compatibility evidence and primary ROM sources](../reference.md#compatibility-evidence)
- [Commodore ROM coverage and historical disabled cases](../reference.md#commodore-rom-integration-coverage)
- [Waterloo ROM coverage and historical setup/service timing](../reference.md#waterloo-rom-integration-coverage)
- [Recorder ownership, capture coverage and lifetime](../reference.md#owned-traces)
- [Scheduling anchors and service timing](../reference.md#deterministic-scheduling-and-configured-runs)
- [External-device fidelity](../reference.md#external-piavia-models)

Preserve historical disabled status when editing examples or migrating APIs.
The coverage sections above distinguish measured behavior from historical
failure notes and unverified compatibility claims.

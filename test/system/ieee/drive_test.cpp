// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>

#include "framework/firmware_test.h"
#include "ieee/controller.h"
#include "ieee/trace.h"

namespace {
using namespace ieee_test;
using disk_fixture::D64;
constexpr uint8_t ViaIeeeIdle = pet::Ndac | pet::NrfdInput | pet::Dav | ViaIdle;
constexpr uint8_t DataChannel = 2;

// Bound production cooperative service, including all physical SPI clocks.
void service_rx(System& system, FirmwareSession& firmware, const char* context) {
    system.execution().within(ByteBudget)
        .service(firmware.service_adapter())
        .until([](const System& board) { return !board.snapshot().ieee_rx_ready_o; })
        .run(context);
}

// Send and independently verify a native OPEN for the shared BASIC fixture.
void open_basic(System& system, IeeeController& controller, Channel channel = Channel{LoadChannel}) {
    auto wire = system.trace_ieee();
    controller.command(Listen(Device{DeviceAddress}));
    controller.command(Open(channel));

    // Native filename collection sets CMDWAT only after receiving EOI.
    constexpr std::string_view Filename = "BASIC";
    for (size_t i = 0; i < Filename.size(); ++i) {
        controller.data(static_cast<uint8_t>(Filename[i]),
            i + 1 == Filename.size() ? Eoi::Last : Eoi::More);
    }
    controller.command(Unlisten);

    require_sequence(wire.records(), {
        command(Listen(Device{DeviceAddress})), command(Open(channel)),
        data('B'), data('A'), data('S'), data('I'), data('C', Eoi::Last), command(Unlisten)
    }, "native OPEN wire sequence");
}

// Give each file length a fresh board and independent firmware session.
class DrivePrgTest : public FirmwareTest, public testing::WithParamInterface<size_t> {};

// Exercise the native one-byte EOF bug with ordinary, zero and high-bit data.
class DriveOneByteTest : public FirmwareTest, public testing::WithParamInterface<uint8_t> {};

} // namespace

// A temporary checked fixture is copied at mount and survives until file EOF.
TEST_P(DrivePrgTest, StreamsCheckedPrgFixtureOverProductionTop) {
    std::vector<uint8_t> payload(GetParam());
    std::vector<Record> expected;
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(0x42 + i);
        expected.push_back(data(payload[i], i + 1 == payload.size() ? Eoi::Last : Eoi::More));
    }

    // OPEN the stored bytes through real CPU I/O and production firmware SPI.
    IeeeController controller(system);
    firmware.mount(D64{}.prg("BASIC", payload));
    open_basic(system, controller);
    service_rx(system, firmware, "service PRG OPEN transaction");

    // Select the load channel and fill the data FIFO within the service deadline.
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Channel{LoadChannel}));
    service_rx(system, firmware, "service PRG TALK transaction");
    auto wire = system.trace_ieee();
    std::vector<Record> received;

    // Both the CPU and passive DAV-edge trace must see EOI only on the final byte.
    for (size_t i = 0; i < payload.size(); ++i) received.push_back(controller.receive());
    require_sequence(received, expected, "checked PRG fixture payload");
    require_sequence(wire.records(), expected, "PRG wire payload");
    EXPECT_TRUE(system.ieee().read_status().contains(fpga::IeeeStatusBit::TxEmpty));
}

// Cover a short file, the original smoke payload and both sides of a sector link.
INSTANTIATE_TEST_SUITE_P(FileLengths, DrivePrgTest, testing::Values(size_t{2}, size_t{4},
    size_t{254}, size_t{255}));

// Preserve the native ordinary-file OPEN bug in 4040 and shared 8050/8250 DOS 2.7.
// OR30 prefetches the first byte through RDBYT, then unconditionally replaces
// its channel readiness with RDYTLK. If the whole file contains just one byte X,
// that prefetch already set EOIOUT, but OR30 discards it. X is sent without EOI.
//
// GETBYT also wrapped the buffer pointer to offset zero when it fetched X.
// Because the lost EOI permits further reads, the drive then transmits the
// final sector's two link bytes: 0 (no next track), then 2 (last occupied byte
// offset). The pointer now reaches the payload again, so X is transmitted a
// second time. This RDBYT sets EOIOUT without another OR30 overwrite.
// Thus the wire sequence is X, 0, 2, X, with EOI only on the fourth byte.
// An EOF-driven reader consumes three extra bytes but eventually stops
// normally (BASIC ST has bit 64 set on the fourth read, not the first).
//
// The threshold is exactly one stored byte in the entire SEQ/PRG/USR file.
// Two or more bytes, including a multi-sector file with a one-byte final
// sector, retain normal final-byte EOI. A one-byte SEQ is legitimate, although
// a one-byte PRG lacks the usual complete two-byte load address. REL records
// and the non-native .hdd extension are outside this ordinary-stream quirk.
// This is unrelated to the required EOI on the final OPEN filename byte.
//
// Source: mist64/cbmsrc revision 01bd60f162ef92212ef0cb67546ae8f42be34168,
// DOS_4040/opchnl and DOS_8250/opchnl (OR30), DOS_4040/opchnl and
// DOS_8250/tst2 (GETBYT/RDBYT), and their ordinary ieee TALK paths.
TEST_P(DriveOneByteTest, PreservesNativeEofBugOverProductionTop) {
    const uint8_t byte = GetParam();
    const std::vector<Record> expected = {
        data(byte), data(0x00), data(0x02), data(byte, Eoi::Last)
    };
    const Channel channel{DataChannel};

    // A legitimate one-byte SEQ file uses an ordinary channel, not PRG LOAD.
    IeeeController controller(system);
    firmware.mount(D64{}.seq("BASIC", {byte}));
    open_basic(system, controller, channel);
    service_rx(system, firmware, "service one-byte SEQ OPEN transaction");
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(channel));
    service_rx(system, firmware, "service one-byte SEQ TALK transaction");
    auto wire = system.trace_ieee();
    std::vector<Record> received;

    // Bound the read independently of the firmware so lost EOF cannot hang it.
    constexpr size_t MaximumWireBytes = 4;
    do {
        received.push_back(controller.receive());
    } while (received.back().eoi != Eoi::Last && received.size() < MaximumWireBytes);
    require_sequence(received, expected, "native one-byte SEQ CPU payload through EOF");
    require_sequence(wire.records(), expected, "native one-byte SEQ wire payload through EOF");
    EXPECT_TRUE(system.ieee().read_status().contains(fpga::IeeeStatusBit::TxEmpty));
}

INSTANTIATE_TEST_SUITE_P(StoredBytes, DriveOneByteTest,
    testing::Values(uint8_t{0x00}, uint8_t{0x0d}, uint8_t{0x41}, uint8_t{0xff}));

// Reads DOS status with final-byte EOI after opening the fixture file.
TEST_F(FirmwareTest, StreamsDosStatusOverProductionTop) {
    IeeeController controller(system);
    firmware.mount(D64{}.prg("BASIC", {0x42, 0x43, 0x44, 0x45}));
    ASSERT_TRUE(system.ieee().read_control().contains(fpga::IeeeControlStatusBit::Enabled));
    auto fifo_trace = firmware.trace_rx();
    auto duplicate = firmware.trace_rx();
    // Open the fixture, then select the drive's status channel as talker.
    open_basic(system, controller);
    service_rx(system, firmware, "service OPEN transaction");
    EXPECT_EQ(duplicate.records(), fifo_trace.records());
    EXPECT_THAT(fifo_trace.records(), testing::ElementsAreArray(fifo_records({
        command(Listen(Device{DeviceAddress})), command(Open(Channel{LoadChannel})),
        data('B'), data('A'), data('S'), data('I'), data('C', Eoi::Last), command(Unlisten)
    })));
    // Select the status channel after production firmware has resolved the OPEN.
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Channel{CommandChannel}));
    service_rx(system, firmware, "service status-channel transaction");
    const std::string expected = "00, OK,00,00\r";
    auto wire = system.trace_ieee();
    std::vector<Record> status;
    // Check every status byte and the exact EOI position.
    for (size_t i = 0; i < expected.size(); ++i) {
        SCOPED_TRACE(i);
        status.push_back(controller.receive());
        EXPECT_EQ(status.back(), data(static_cast<uint8_t>(expected[i]),
            i + 1 == expected.size() ? Eoi::Last : Eoi::More));
    }
    EXPECT_THAT(wire.records(), testing::ElementsAreArray(status));
}

// Checks CPU-written VIA state and the FPGA's injected external line inputs.
TEST_F(FirmwareTest, CpuViaWriteAndInjectedRead) {
    IeeeController controller(system);
    firmware.mount(disk_fixture::D64::empty());
    controller.write(pet::ViaPortB, ViaIdle);
    EXPECT_EQ(system.ieee().peek_via_orb(), ViaIdle);
    // Ignore unrelated pins, especially PB5's running vertical-retrace input.
    EXPECT_EQ(controller.read(pet::ViaPortB) & ViaIeeeIdle, ViaIeeeIdle);
}

// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
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

// Send and independently verify a native OPEN for a selected device and filename.
void open_file(System& system, IeeeController& controller, Channel channel = Channel{LoadChannel},
               std::string_view filename = "BASIC", Device device = Device{DeviceAddress}) {
    auto wire = system.trace_ieee();
    controller.command(Listen(device));
    controller.command(Open(channel));

    // Native filename collection sets CMDWAT only after receiving EOI.
    std::vector<Record> expected{command(Listen(device)), command(Open(channel))};
    for (size_t i = 0; i < filename.size(); ++i) {
        const auto byte = static_cast<uint8_t>(filename[i]);
        const auto eoi = i + 1 == filename.size() ? Eoi::Last : Eoi::More;
        controller.data(byte, eoi);
        expected.push_back(data(byte, eoi));
    }
    controller.command(Unlisten);
    expected.push_back(command(Unlisten));
    require_sequence(wire.records(), expected, "native OPEN wire sequence");
}

// Bound EOF-driven reads independently of the source and compare CPU and wire evidence.
void receive_payload(System& system, IeeeController& controller, const std::vector<Record>& expected) {
    auto wire = system.trace_ieee();
    std::vector<Record> received;
    for (size_t i = 0; i < expected.size(); ++i) {
        received.push_back(controller.receive());
        if (received.back().eoi == Eoi::Last) break;
    }
    require_sequence(received, expected, "CPU payload");
    require_sequence(wire.records(), expected, "wire payload");
}

// An exhausted source must remain empty while an armed listener advertises readiness.
void expect_no_payload(System& system, FirmwareSession& firmware, IeeeController& controller) {
    constexpr econopet::Cycles QuietBudget = 20000;
    EXPECT_TRUE(system.ieee().read_status().contains(fpga::IeeeStatusBit::TxEmpty));
    auto wire = system.trace_ieee();
    controller.write(pet::Pia2ControlA, PiaC2Low.bits());
    controller.write(pet::ViaPortB, static_cast<uint8_t>(BusReleased & ~pet::Nrfd));
    controller.write(pet::ViaPortB, BusReleased);
    firmware.service();
    system.tick(QuietBudget);
    firmware.service();
    const auto capture = wire.capture();
    EXPECT_TRUE(capture.complete());
    EXPECT_GE(capture.window().completed() - capture.window().started(), QuietBudget);
    EXPECT_THAT(capture.records(), testing::IsEmpty());
    EXPECT_TRUE(system.snapshot().ieee_bus_dav_no);
}

// Bound status input and independently check its wire bytes and final CR/EOI.
std::string receive_status(System& system, FirmwareSession& firmware, IeeeController& controller,
                           Device device) {
    constexpr size_t StatusByteLimit = IEEE_STATUS_TEXT_MAX + 1;
    controller.command(Talk(device));
    controller.command(Secondary(Channel{CommandChannel}));
    service_rx(system, firmware, "service status request");
    auto wire = system.trace_ieee();
    std::vector<Record> records;
    std::string text;
    for (size_t i = 0; i < StatusByteLimit; ++i) {
        records.push_back(controller.receive());
        require_sequence({records.back()}, {data(records.back().data, records.back().eoi)},
            "status data flags");
        text.push_back(static_cast<char>(records.back().data));
        if (records.back().eoi == Eoi::Last) break;
    }
    require_sequence(wire.records(), records, "status CPU and wire payload");
    require_sequence({records.back()}, {data('\r', Eoi::Last)}, "status final CR/EOI");
    controller.command(Untalk);
    service_rx(system, firmware, "service status UNTALK");
    return text;
}

// Give each file length a fresh board and independent firmware session.
class DrivePrgTest : public FirmwareTest, public testing::WithParamInterface<size_t> {};

// Exercise the native one-byte EOF bug with ordinary, zero and high-bit data.
class DriveOneByteTest : public FirmwareTest, public testing::WithParamInterface<uint8_t> {};

// Select model, unit and drive independently rather than assuming all DOS status formats agree.
class DriveStatusTest : public FirmwareTest,
                        public testing::WithParamInterface<std::tuple<bool, uint8_t, uint8_t>> {};

// Check native token expansion independently of model-specific numeric fields.
class DriveStatusTextTest : public FirmwareTest, public testing::WithParamInterface<bool> {};

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
    open_file(system, controller);
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
    open_file(system, controller, channel);
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
    open_file(system, controller);
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

// PR #53: an accepted sequential EOF leaves GET0's channel NOTRDY, not CR+EOI.
TEST_F(FirmwareTest, DISABLED_SequentialRetalkAfterEofDoesNotInventData) {
    IeeeController controller(system);
    firmware.mount(D64{}.seq("BASIC", {0x42, 0x43}));
    open_file(system, controller, Channel{DataChannel});
    service_rx(system, firmware, "service SEQ OPEN");
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "service first SEQ TALK");
    receive_payload(system, controller, {data(0x42), data(0x43, Eoi::Last)});

    // Request the exhausted channel again without reopening or flushing its data.
    controller.command(Untalk);
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "service exhausted SEQ TALK");
    expect_no_payload(system, firmware, controller);
    controller.command(Untalk);
    service_rx(system, firmware, "service exhausted SEQ UNTALK");
    EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}),
        "00, OK,00,00\r");
}

// PR #53: a failed successor cannot manufacture a successful CR+EOI response.
TEST_F(FirmwareTest, DISABLED_SequentialReadErrorDoesNotInventEof) {
    std::vector<uint8_t> payload(DISKIMAGE_SECTOR_PAYLOAD_SIZE + 1);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i + 2);
    auto image = D64{}.seq("BASIC", payload);
    constexpr size_t Directory = 18;
    constexpr size_t FirstDirectorySector = 1;
    constexpr size_t StartTrackOffset = 3;
    constexpr size_t StartSectorOffset = 4;
    const auto entry = D64::offset(Directory, FirstDirectorySector);
    const auto first_sector = D64::offset(image.bytes()[entry + StartTrackOffset],
        image.bytes()[entry + StartSectorOffset]);
    image.corrupt(first_sector, {DISKIMAGE_D64_TRACK_COUNT + 1, 0});

    // Consume only the valid first-sector prefix through the real CPU acceptor.
    IeeeController controller(system);
    firmware.mount(image);
    open_file(system, controller, Channel{DataChannel});
    service_rx(system, firmware, "service corrupt SEQ OPEN");
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "service corrupt SEQ TALK");
    std::vector<Record> expected;
    for (size_t i = 0; i < DISKIMAGE_SECTOR_PAYLOAD_SIZE; ++i)
        expected.push_back(data(payload[i]));
    receive_payload(system, controller, expected);
    expect_no_payload(system, firmware, controller);

    // The emulator reports its backing-chain failure as status 23, not clean EOF.
    controller.command(Untalk);
    service_rx(system, firmware, "service corrupt SEQ UNTALK");
    EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}),
        "23,READ ERROR,00,00\r");
}

// PR #54: SETLST retains the first NUL of an empty record and RDREL marks it EOI.
TEST_F(FirmwareTest, DISABLED_EmptyRelativeRecordReturnsNulThenAdvances) {
    constexpr uint8_t RecordLength = 129;
    constexpr size_t DirectoryTrack = 18;
    constexpr size_t DirectorySector = 1;
    constexpr size_t FileTypeOffset = 2;
    constexpr uint8_t ClosedRelativeFile = 0x80 | DISKIMAGE_FTYPE_REL;
    std::vector<uint8_t> payload(2 * RecordLength);
    for (size_t i = RecordLength; i < payload.size(); ++i)
        payload[i] = static_cast<uint8_t>(0xa0 + i / DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    auto image = D64{}.seq("BASIC", payload);
    const auto entry = D64::offset(DirectoryTrack, DirectorySector);
    // Like the unit fixture, this probes the data-chain engine, not side-sector validation.
    image.corrupt(entry + FileTypeOffset, {ClosedRelativeFile})
         .corrupt(entry + DISKIMAGE_DIRECTORY_REL_LENGTH_OFFSET, {RecordLength});

    IeeeController controller(system);
    firmware.mount(image);
    open_file(system, controller, Channel{DataChannel}, "BASIC,L");
    service_rx(system, firmware, "service REL OPEN");
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "service empty REL TALK");
    receive_payload(system, controller, {data(0x00, Eoi::Last)});

    // The next TALK returns record two, including its physical sector crossing.
    controller.command(Untalk);
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "service following REL TALK");
    std::vector<Record> expected;
    for (size_t i = RecordLength; i < payload.size(); ++i)
        expected.push_back(data(payload[i], i + 1 == payload.size() ? Eoi::Last : Eoi::More));
    receive_payload(system, controller, expected);
}

// PR #55's drive suffix belongs to 8050 DOS 2.7, not 4040 DOS 2.
TEST_P(DriveStatusTest, PreservesModelSpecificStatusAndUnitDriveIdentity) {
    const auto [d80, address, drive] = GetParam();
    const Device device{address};
    const Device peer{static_cast<uint8_t>(address == DeviceAddress ? address + 1 : DeviceAddress)};
    constexpr unsigned DrivesPerUnit = 2;
    const unsigned slot = (address - DeviceAddress) * DrivesPerUnit + drive;
    const unsigned peer_drive = drive ^ 1u;
    const unsigned peer_slot = (peer.value() - DeviceAddress) * DrivesPerUnit + peer_drive;
    const char* const name = d80 ? "status.d80" : "status.d64";
    IeeeController controller(system);
    // Empty D80 lookup needs only the zero-terminated directory, not BAM allocation.
    firmware.mount(d80 ? std::vector<uint8_t>(DISKIMAGE_D80_SIZE) : D64::empty().bytes(), name);
    ASSERT_TRUE(ieee_drive_mount(slot, name));
    ASSERT_TRUE(ieee_drive_mount(peer_slot, name));

    // Leave independent pending errors on two units, with opposite local drive numbers.
    open_file(system, controller, Channel{DataChannel},
        std::to_string(drive) + ":MISSING", device);
    service_rx(system, firmware, "service selected-unit missing file");
    open_file(system, controller, Channel{DataChannel},
        std::to_string(peer_drive) + ":MISSING", peer);
    service_rx(system, firmware, "service peer-unit missing file");
    const std::string suffix = d80 ? "," + std::to_string(drive) : "";
    const std::string peer_suffix = d80 ? "," + std::to_string(peer_drive) : "";
    EXPECT_THAT(receive_status(system, firmware, controller, device),
        testing::MatchesRegex("62,[^,\r]+,00,00" + suffix + "\r"));
    EXPECT_THAT(receive_status(system, firmware, controller, peer),
        testing::MatchesRegex("62,[^,\r]+,00,00" + peer_suffix + "\r"));
    EXPECT_THAT(receive_status(system, firmware, controller, device),
        testing::MatchesRegex("00,[^,\r]+,00,00" + suffix + "\r"));
}

INSTANTIATE_TEST_SUITE_P(ModelsUnitsDrives, DriveStatusTest,
    testing::Combine(testing::Bool(), testing::Values(uint8_t{8}, uint8_t{9}, uint8_t{10}, uint8_t{11}),
        testing::Values(uint8_t{0}, uint8_t{1})));

// Both native token expanders insert a space before the initial FILE token.
TEST_P(DriveStatusTextTest, PreservesNativeFileNotFoundText) {
    const bool d80 = GetParam();
    IeeeController controller(system);
    firmware.mount(d80 ? std::vector<uint8_t>(DISKIMAGE_D80_SIZE) : D64::empty().bytes(),
        d80 ? "status.d80" : "status.d64");
    open_file(system, controller, Channel{DataChannel}, "MISSING");
    service_rx(system, firmware, "service missing file for native status text");
    const std::string expected = d80
        ? "62, FILE NOT FOUND,00,00,0\r" : "62, FILE NOT FOUND,00,00\r";
    EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}), expected);
}

INSTANTIATE_TEST_SUITE_P(Models, DriveStatusTextTest, testing::Bool());

// Checks CPU-written VIA state and the FPGA's injected external line inputs.
TEST_F(FirmwareTest, CpuViaWriteAndInjectedRead) {
    IeeeController controller(system);
    firmware.mount(disk_fixture::D64::empty());
    controller.write(pet::ViaPortB, ViaIdle);
    EXPECT_EQ(system.ieee().peek_via_orb(), ViaIdle);
    // Ignore unrelated pins, especially PB5's running vertical-retrace input.
    EXPECT_EQ(controller.read(pet::ViaPortB) & ViaIeeeIdle, ViaIeeeIdle);
}

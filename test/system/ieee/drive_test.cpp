// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <gmock/gmock.h>

#include "framework/firmware_test.h"
#include "ieee/controller.h"
#include "ieee/trace.h"

extern "C" {
#include "sd/sd.h"
}

namespace {
using namespace ieee_test;
using disk_fixture::D64;
constexpr uint8_t ViaIeeeIdle = pet::Ndac | pet::NrfdInput | pet::Dav | ViaIdle;
constexpr uint8_t DataChannel = 2;
constexpr size_t DirectoryStartTrack = 3;
constexpr size_t DirectoryStartSector = 4;
constexpr size_t D80SectorsPerTrack = 29;
constexpr size_t D80DirectoryTrack = 39;
constexpr uint8_t D80DataTrack = 38;
constexpr size_t TerminalPayloadLimit = DISKIMAGE_SECTOR_PAYLOAD_SIZE + 1;
constexpr size_t SequentialFixtureLimit = 3 * DISKIMAGE_SECTOR_PAYLOAD_SIZE;
constexpr econopet::Cycles RomServicePeriod = 1000;
// Allow the full native timer/software wait, successful prefix and readdressing.
constexpr econopet::Cycles TerminalRomBudget = 12'000'000;
constexpr econopet::CpuAddress NativePayload{0x0800}, NativeLastStatus{0x0900};
constexpr econopet::CpuAddress Pet2InputTimeout{0xf141}, Pet4InputTimeout{0xf15b};
constexpr econopet::CpuAddress WaterlooInputTimeout{0xbf7c};
constexpr econopet::CpuAddress WaterlooGlobalStatus{0x006a}, WaterlooActiveRole{0x0077};

// Resolve the single directory sector independently of the production parser.
size_t sequential_directory(bool d80) {
    return d80
        ? ((D80DirectoryTrack - 1) * D80SectorsPerTrack + 1) * DISKIMAGE_SECTOR_SIZE
        : D64::offset(18, 1);
}

// Locate BASIC's first data sector from the fixture's raw directory entry.
size_t sequential_first_sector(const std::vector<uint8_t>& image, bool d80) {
    const auto directory = sequential_directory(d80);
    const auto track = image.at(directory + DirectoryStartTrack);
    const auto sector = image.at(directory + DirectoryStartSector);
    return d80 ? ((track - 1) * D80SectorsPerTrack + sector) * DISKIMAGE_SECTOR_SIZE
               : D64::offset(track, sector);
}

// Build a bounded SEQ chain for both native models without using the drive parser.
std::vector<uint8_t> sequential_image(bool d80, const std::vector<uint8_t>& payload) {
    if (payload.size() < 2 || payload.size() > SequentialFixtureLimit)
        throw std::invalid_argument("SEQ fixture requires 2..762 payload bytes");
    if (!d80) return D64{}.seq("BASIC", payload).bytes();
    std::vector<uint8_t> image(DISKIMAGE_D80_SIZE);
    const auto directory = sequential_directory(true);
    constexpr uint8_t ClosedSequentialFile = 0x80 | DISKIMAGE_FTYPE_SEQ;
    constexpr size_t FileTypeOffset = 2, FilenameOffset = 5, FilenameLength = 16;
    image[directory + 1] = 0xff;
    image[directory + FileTypeOffset] = ClosedSequentialFile;
    image[directory + DirectoryStartTrack] = D80DataTrack;
    std::fill_n(image.begin() + directory + FilenameOffset, FilenameLength, 0xa0);
    const std::string filename = "BASIC";
    std::copy(filename.begin(), filename.end(), image.begin() + directory + FilenameOffset);

    // Keep the bounded chain on track 38, whose 29-sector geometry is ROM-defined.
    size_t used = 0;
    for (size_t sector = 0; used < payload.size(); ++sector) {
        const auto offset = ((D80DataTrack - 1) * D80SectorsPerTrack + sector)
            * DISKIMAGE_SECTOR_SIZE;
        const auto count = std::min(payload.size() - used, size_t{DISKIMAGE_SECTOR_PAYLOAD_SIZE});
        const bool last = used + count == payload.size();
        image[offset] = last ? 0 : D80DataTrack;
        image[offset + 1] = static_cast<uint8_t>(last
            ? count + DISKIMAGE_SECTOR_LINK_SIZE - 1 : sector + 1);
        std::copy_n(payload.begin() + used, count,
            image.begin() + offset + DISKIMAGE_SECTOR_LINK_SIZE);
        used += count;
    }
    return image;
}

// Decorate a bounded data chain for REL engine tests, without inventing side sectors.
std::vector<uint8_t> relative_image(bool d80, const std::vector<uint8_t>& payload, uint8_t reclen) {
    auto image = sequential_image(d80, payload);
    constexpr size_t FileTypeOffset = 2;
    image[sequential_directory(d80) + FileTypeOffset] = 0x80 | DISKIMAGE_FTYPE_REL;
    image[sequential_directory(d80) + DISKIMAGE_DIRECTORY_REL_LENGTH_OFFSET] = reclen;
    return image;
}

// Add a SEQ alias of the REL payload for mixed-channel readiness controls.
std::vector<uint8_t> relative_sequential_image(bool d80, const std::vector<uint8_t>& payload) {
    auto image = relative_image(d80, payload, 2);
    const auto directory = sequential_directory(d80);
    const auto sequential = directory + DISKIMAGE_DIRECTORY_ENTRY_SIZE;
    constexpr size_t FileTypeOffset = 2, FilenameOffset = 5;
    std::copy_n(image.begin() + directory, DISKIMAGE_DIRECTORY_ENTRY_SIZE,
        image.begin() + sequential);
    image[sequential + FileTypeOffset] = 0x80 | DISKIMAGE_FTYPE_SEQ;
    std::fill_n(image.begin() + sequential + FilenameOffset, DISKIMAGE_FILENAME_SIZE,
        DISKIMAGE_FILENAME_PADDING);
    const std::string name = "PLAIN";
    std::copy(name.begin(), name.end(), image.begin() + sequential + FilenameOffset);
    return image;
}

// Compare the full persisted fixture independently of channel read responses.
void expect_persisted_image(const char* filename, const std::vector<uint8_t>& expected) {
    char path[SD_PATH_MAX];
    sd_make_path(path, SD_DIR_DISKS, filename);
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::fopen(path, "rb"), &std::fclose);
    ASSERT_TRUE(file);
    std::vector<uint8_t> actual(expected.size());
    ASSERT_EQ(std::fread(actual.data(), 1, actual.size(), file.get()), actual.size());
    EXPECT_EQ(std::fgetc(file.get()), EOF);
    EXPECT_FALSE(std::ferror(file.get()));
    EXPECT_EQ(actual, expected);
}

// Repair the mounted backing file in place, without resetting the drive session.
void repair_sequential_image(const char* name, const std::vector<uint8_t>& image) {
    char path[SD_PATH_MAX];
    sd_make_path(path, SD_DIR_DISKS, name);
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::fopen(path, "r+b"), &std::fclose);
    if (!file) throw std::runtime_error("cannot open terminal SEQ fixture for repair");
    if (std::fwrite(image.data(), 1, image.size(), file.get()) != image.size()
        || std::fflush(file.get()) != 0)
        throw std::runtime_error("cannot write repaired terminal SEQ fixture");
    if (std::fclose(file.release()) != 0)
        throw std::runtime_error("cannot close repaired terminal SEQ fixture");
}

// Bound production cooperative service, including all physical SPI clocks.
void service_rx(System& system, FirmwareSession& firmware, const char* context) {
    system.execution().within(ByteBudget)
        .service(firmware.service_adapter())
        .until([](const System& board) { return !board.snapshot().ieee_rx_ready_o; })
        .run(context);
}

// Send a binary native P command through the production RX/SPI path.
void position_relative(System& system, FirmwareSession& firmware, IeeeController& controller,
        uint16_t record, uint8_t offset, Channel channel = Channel{DataChannel},
        Device device = Device{DeviceAddress}) {
    controller.command(Listen(device));
    controller.command(Secondary(Channel{CommandChannel}));
    const std::vector<uint8_t> bytes{'P', static_cast<uint8_t>(IEEE_CMD_SECONDARY(channel.value())),
        static_cast<uint8_t>(record), static_cast<uint8_t>(record >> 8), offset};
    for (size_t i = 0; i < bytes.size(); ++i)
        controller.data(bytes[i], i + 1 == bytes.size() ? Eoi::Last : Eoi::More);
    controller.command(Unlisten);
    service_rx(system, firmware, "service native REL P");
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

// Exercise ordinary terminal state and in-place recovery on D64 and D80 media.
class DriveTerminalTest : public FirmwareTest, public testing::WithParamInterface<bool> {};

// Share record-engine contracts between D64/4040 and D80/single-sided 8050.
class DriveRelativeTest : public FirmwareTest, public testing::WithParamInterface<bool> {};

// Keep each model/length matrix inside the ordinary integration-test timeout.
class DriveCommandLengthTest : public FirmwareTest,
    public testing::WithParamInterface<std::tuple<bool, size_t>> {};

// Cross native models, interruption phases, and same/status/channel/unit ownership.
class DriveRelativeContinuationTest : public FirmwareTest,
    public testing::WithParamInterface<std::tuple<bool, int, int>> {};

// Cross media, NRFD phase, and final-byte state while parking a live response.
class DriveRelativeParkingTest : public FirmwareTest,
    public testing::WithParamInterface<std::tuple<bool, bool, bool>> {};

// Cross media, SEQ/REL formats, and commands that preserve the selected talker.
class DrivePreservedTalkTest : public FirmwareTest,
    public testing::WithParamInterface<std::tuple<bool, bool, bool>> {};

// Execute each selected native controller against both media models.
class DriveRelativeRomTest : public FirmwareTest,
    public testing::WithParamInterface<std::tuple<bool, test_rom::Set>> {};

// Execute wildcard OPEN and reads through each native controller and media model.
class DriveWildcardRomTest : public FirmwareTest,
    public testing::WithParamInterface<std::tuple<bool, test_rom::Set>> {};

// Exercise counted binary OPEN 15 through the actual PET 2 and PET 4 KERNALs.
class DriveCommandRomTest : public FirmwareTest,
    public testing::WithParamInterface<std::tuple<bool, test_rom::Set>> {};

// Cross native controller revisions with media model and EOF versus read failure.
class DriveRomTerminalTest : public FirmwareTest,
    public testing::WithParamInterface<std::tuple<bool, bool, test_rom::Set>> {};

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
    EXPECT_THAT(fifo_trace.records(), testing::ElementsAreArray(std::vector<Record>{
        command(Listen(Device{DeviceAddress})), command(Open(Channel{LoadChannel})),
        data('B'), data('A'), data('S'), data('I'), data('C', Eoi::Last), command(Unlisten)
    }));
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
TEST_P(DriveTerminalTest, SequentialRetalkAfterEofDoesNotInventData) {
    const bool d80 = GetParam();
    IeeeController controller(system);
    firmware.mount(sequential_image(d80, {0x42, 0x43}), d80 ? "terminal.d80" : "terminal.d64");
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
        d80 ? "00, OK,00,00,0\r" : "00, OK,00,00\r");

    // Reopening the same file restores readiness and its complete payload.
    open_file(system, controller, Channel{DataChannel});
    service_rx(system, firmware, "service exhausted SEQ reopen");
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "service reopened SEQ TALK");
    receive_payload(system, controller, {data(0x42), data(0x43, Eoi::Last)});
}

// PR #53: a failed successor cannot manufacture a successful CR+EOI response.
TEST_P(DriveTerminalTest, SequentialReadErrorDoesNotInventEof) {
    const bool d80 = GetParam();
    std::vector<uint8_t> payload(DISKIMAGE_SECTOR_PAYLOAD_SIZE + 1);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i + 2);
    const auto repaired = sequential_image(d80, payload);
    auto image = repaired;
    const auto first_sector = sequential_first_sector(image, d80);
    image[first_sector] = (d80 ? DISKIMAGE_D80_TRACK_COUNT : DISKIMAGE_D64_TRACK_COUNT) + 1;
    image[first_sector + 1] = 0;

    // Consume only the valid first-sector prefix through the real CPU acceptor.
    IeeeController controller(system);
    const char* const name = d80 ? "terminal.d80" : "terminal.d64";
    firmware.mount(image, name);
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
        d80 ? "23,READ ERROR,00,00,0\r" : "23,READ ERROR,00,00\r");

    // The failed channel stays released after status acknowledgement.
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "service failed SEQ retalk");
    expect_no_payload(system, firmware, controller);
    controller.command(Untalk);
    service_rx(system, firmware, "service failed SEQ UNTALK");
    EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}),
        d80 ? "00, OK,00,00,0\r" : "00, OK,00,00\r");

    // Repairing storage alone does not recreate the released channel.
    repair_sequential_image(name, repaired);
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "service repaired but closed SEQ TALK");
    expect_no_payload(system, firmware, controller);
    controller.command(Untalk);
    service_rx(system, firmware, "service repaired SEQ UNTALK");

    // Only OPEN restores readability, including the previously missing successor.
    open_file(system, controller, Channel{DataChannel});
    service_rx(system, firmware, "service repaired SEQ reopen");
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "service repaired SEQ TALK");
    expected.push_back(data(payload.back(), Eoi::Last));
    receive_payload(system, controller, expected);
    controller.command(Untalk);
    service_rx(system, firmware, "service recovered SEQ UNTALK");
    EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}),
        d80 ? "00, OK,00,00,0\r" : "00, OK,00,00\r");
}

INSTANTIATE_TEST_SUITE_P(Models, DriveTerminalTest, testing::Bool());

// Native receive code consumes the valid stream, then reaches its real timeout branch.
TEST_P(DriveRomTerminalTest, NativeReceiveTimesOutOnTerminalChannel) {
    const auto [d80, failed, revision] = GetParam();
    const size_t size = failed ? TerminalPayloadLimit : 2;
    std::vector<uint8_t> payload(size);
    for (size_t i = 0; i < size; ++i) payload[i] = static_cast<uint8_t>(i + 2);
    auto image = sequential_image(d80, payload);
    if (failed) {
        const auto first_sector = sequential_first_sector(image, d80);
        image[first_sector] = (d80 ? DISKIMAGE_D80_TRACK_COUNT : DISKIMAGE_D64_TRACK_COUNT) + 1;
        image[first_sector + 1] = 0;
        payload.pop_back();
    }
    IeeeController controller(system);
    firmware.mount(image, d80 ? "native.d80" : "native.d64");
    open_file(system, controller, Channel{DataChannel});
    service_rx(system, firmware, "service native terminal SEQ OPEN");
    controller.write(pet::ViaPortB, BusReleased);
    system.cpu().assert_reset();
    auto wire = system.trace_ieee();
    const auto checkpoint = system.cpu().peek_fetch_checkpoint();

    if (revision != test_rom::Set::Waterloo) {
        KernalFixture rom(system, revision);
        auto test = rom.program();
        const auto received = test.data("native payload", NativePayload, payload.size(), test_asm::Capture::Yes);
        const auto last_status = test.data("last payload status", NativeLastStatus, 1, test_asm::Capture::Yes);
        const auto status = test.byte("timeout status");
        const auto value = test.byte("timeout return");
        const auto result = test.within(TerminalRomBudget)
            .every(RomServicePeriod, ServiceTiming::AfterSlice, firmware.service_adapter())
            .run([&](auto& p) {
                rom.init_ieee(p);
                rom.device(p);
                p.lda(test_asm::imm(DeviceAddress)).jsr(rom.entry(KernalRoutine::Talk))
                 .lda(test_asm::imm(IEEE_CMD_SECONDARY(DataChannel))).jsr(rom.entry(KernalRoutine::Tksa));
                for (size_t i = 0; i < payload.size(); ++i)
                    p.jsr(rom.entry(KernalRoutine::Acptr)).sta(received.byte(i));
                p.lda(test_asm::zp(KernalWorkspace::Status)).sta(last_status.byte(0))
                 .lda(test_asm::imm(0)).sta(test_asm::zp(KernalWorkspace::Status))
                 .jsr(rom.entry(KernalRoutine::Untalk))
                 .lda(test_asm::imm(DeviceAddress)).jsr(rom.entry(KernalRoutine::Talk))
                 .lda(test_asm::imm(IEEE_CMD_SECONDARY(DataChannel))).jsr(rom.entry(KernalRoutine::Tksa))
                 .jsr(rom.entry(KernalRoutine::Acptr)).sta(value)
                 .lda(test_asm::zp(KernalWorkspace::Status)).sta(status);
            });
        EXPECT_EQ(result[received], payload);
        EXPECT_EQ(result[last_status.byte(0)], failed ? 0 : 0x40);
        EXPECT_EQ(result[status], 0x02);
        EXPECT_EQ(result[value], '\r');
        const auto timeout = revision == test_rom::Set::Upgrade ? Pet2InputTimeout : Pet4InputTimeout;
        EXPECT_TRUE(system.cpu(CPU_SOFT_6502).peek_opcode_fetched_at(timeout, checkpoint));
    } else {
        WaterlooFixture rom(system);
        auto test = rom.program();
        const auto received = test.data("native payload", NativePayload, payload.size(), test_asm::Capture::Yes);
        const auto flags = test.byte("last payload flags");
        const auto status = test.byte("timeout status");
        const auto result = test.within(TerminalRomBudget)
            .every(RomServicePeriod, ServiceTiming::AfterSlice, firmware.service_adapter())
            .run([&](auto& p) {
                rom.init_ieee(p);
                rom.device(p, DeviceAddress, DataChannel);
                p.lda(test_asm::imm(0)).sta(WaterlooActiveRole).sta(WaterlooWorkspace::DiskFlags)
                 .sta(WaterlooGlobalStatus)
                 .lda(test_asm::imm(1)).sta(WaterlooWorkspace::WaitParameterLow)
                 .ldd(test_asm::imm16(WaterlooWorkspace::Block)).jsr(rom.entry(WaterlooRoutine::Talk));
                for (size_t i = 0; i < payload.size(); ++i)
                    p.jsr(rom.entry(WaterlooRoutine::Input)).stb(received.byte(i).address());
                p.lda(WaterlooWorkspace::DiskFlags).sta(flags)
                 .jsr(rom.entry(WaterlooRoutine::Untalk))
                 .ldd(test_asm::imm16(WaterlooWorkspace::Block)).jsr(rom.entry(WaterlooRoutine::Talk))
                 .jsr(rom.entry(WaterlooRoutine::Input))
                 .lda(WaterlooWorkspace::Block).sta(status);
            });
        EXPECT_EQ(result[received], payload);
        EXPECT_EQ(result[flags] & 1, failed ? 0 : 1);
        EXPECT_EQ(result[status], 3);
        EXPECT_EQ(system.peek(econopet::SramAddress{WaterlooGlobalStatus.value()}), 3);
        EXPECT_TRUE(system.cpu(CPU_SOFT_6809).peek_opcode_fetched_at(WaterlooInputTimeout, checkpoint));
    }
    std::vector<Record> expected{command(Talk(Device{DeviceAddress})),
        command(Secondary(Channel{DataChannel}))};
    for (size_t i = 0; i < payload.size(); ++i)
        expected.push_back(data(payload[i], !failed && i + 1 == payload.size() ? Eoi::Last : Eoi::More));
    expected.push_back(command(Untalk));
    expected.push_back(command(Talk(Device{DeviceAddress})));
    expected.push_back(command(Secondary(Channel{DataChannel})));
    require_sequence(wire.records(), expected, "native terminal receive wire bytes");
    EXPECT_TRUE(system.snapshot().ieee_bus_dav_no);
    EXPECT_TRUE(system.ieee().read_status().contains(fpga::IeeeStatusBit::TxEmpty));
}

INSTANTIATE_TEST_SUITE_P(ModelsFailuresRevisions, DriveRomTerminalTest,
    testing::Combine(testing::Bool(), testing::Bool(),
        testing::Values(test_rom::Set::Upgrade, test_rom::Set::Rom4, test_rom::Set::Waterloo)));

// PR #54: SETLST retains the first NUL of an empty record and RDREL marks it EOI.
TEST_P(DriveRelativeTest, EmptyRelativeRecordReturnsNulThenAdvances) {
    const bool d80 = GetParam();
    constexpr uint8_t RecordLength = 129;
    constexpr size_t FileTypeOffset = 2;
    constexpr uint8_t ClosedRelativeFile = 0x80 | DISKIMAGE_FTYPE_REL;
    std::vector<uint8_t> payload(2 * RecordLength);
    for (size_t i = RecordLength; i < payload.size(); ++i)
        payload[i] = static_cast<uint8_t>(0xa0 + i / DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    auto image = sequential_image(d80, payload);
    const auto entry = sequential_directory(d80);
    // Like the unit fixture, this probes the data-chain engine, not side-sector validation.
    image[entry + FileTypeOffset] = ClosedRelativeFile;
    image[entry + DISKIMAGE_DIRECTORY_REL_LENGTH_OFFSET] = RecordLength;

    IeeeController controller(system);
    firmware.mount(image, d80 ? "relative.d80" : "relative.d64");
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

// Each empty record is a distinct NUL/EOI response, even across a sector link.
TEST_P(DriveRelativeTest, ConsecutiveEmptyRelativeRecordsRemainSeparate) {
    const bool d80 = GetParam();
    constexpr uint8_t RecordLength = 129;
    constexpr size_t RecordCount = 3, FileTypeOffset = 2;
    constexpr uint8_t ClosedRelativeFile = 0x80 | DISKIMAGE_FTYPE_REL;
    std::vector<uint8_t> payload(RecordCount * RecordLength);
    constexpr uint8_t Marker = 0xa5;
    payload[2 * RecordLength] = Marker;
    auto image = sequential_image(d80, payload);
    const auto entry = sequential_directory(d80);
    image[entry + FileTypeOffset] = ClosedRelativeFile;
    image[entry + DISKIMAGE_DIRECTORY_REL_LENGTH_OFFSET] = RecordLength;

    IeeeController controller(system);
    firmware.mount(image, d80 ? "empty-records.d80" : "empty-records.d64");
    open_file(system, controller, Channel{DataChannel}, "BASIC,L");
    service_rx(system, firmware, "service consecutive empty REL OPEN");

    // Readdress twice without position commands: neither empty record is skipped.
    for (size_t record = 0; record < RecordCount; ++record) {
        controller.command(Talk(Device{DeviceAddress}));
        controller.command(Secondary(Channel{DataChannel}));
        service_rx(system, firmware, "service consecutive empty REL TALK");
        receive_payload(system, controller, {data(record < 2 ? 0 : Marker, Eoi::Last)});
        controller.command(Untalk);
        service_rx(system, firmware, "service consecutive empty REL UNTALK");
    }
    EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}),
        d80 ? "00, OK,00,00,0\r" : "00, OK,00,00\r");
}

// Check zero-suffix pointer wrap and invalid-P replacement through actual IEEE traffic.
TEST_P(DriveRelativeTest, PositionSuffixAndOverflowFollowNativePointers) {
    const bool d80 = GetParam();
    constexpr uint8_t RecordLength = 129, Marker = 0xa5, Successor = 0x7b;
    std::vector<uint8_t> payload(3 * RecordLength);
    payload[0] = Marker;
    payload[DISKIMAGE_SECTOR_PAYLOAD_SIZE] = Successor;
    IeeeController controller(system);
    firmware.mount(relative_image(d80, payload, RecordLength), d80 ? "position.d80" : "position.d64");
    open_file(system, controller, Channel{DataChannel}, "BASIC,L");
    service_rx(system, firmware, "service P fixture OPEN");
    position_relative(system, firmware, controller, 1, RecordLength);
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "service zero-suffix REL TALK");
    std::vector<Record> expected;
    for (size_t i = RecordLength - 1; i <= DISKIMAGE_SECTOR_PAYLOAD_SIZE; ++i)
        expected.push_back(data(payload[i],
            i == DISKIMAGE_SECTOR_PAYLOAD_SIZE ? Eoi::Last : Eoi::More));
    receive_payload(system, controller, expected);
    controller.command(Untalk);
    position_relative(system, firmware, controller, 1, RecordLength + 1);
    EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}),
        d80 ? "51,OVERFLOW IN RECORD,00,00,0\r" : "51,OVERFLOW IN RECORD,00,00\r");
    controller.command(Untalk);
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "service overflow-recovered REL TALK");
    receive_payload(system, controller, {data(Marker, Eoi::Last)});
}

// Queued A/B selections must never expose A's bytes while the fabric serves B.
TEST_P(DriveRelativeTest, QueuedSelectionsPauseInitiallyUnboundData) {
    const bool d80 = GetParam();
    constexpr uint8_t RecordLength = 3;
    constexpr Channel Original{DataChannel}, Peer{DataChannel + 1};
    const std::vector<uint8_t> payload{0x11, 0x12, 0x13, 0x21, 0x22, 0x23};
    IeeeController controller(system);
    firmware.mount(relative_image(d80, payload, RecordLength),
        d80 ? "queued-selections.d80" : "queued-selections.d64");
    open_file(system, controller, Original, "BASIC,L");
    service_rx(system, firmware, "service original queued-selection OPEN");
    open_file(system, controller, Peer, "BASIC,L");
    service_rx(system, firmware, "service peer queued-selection OPEN");
    position_relative(system, firmware, controller, 2, 1, Peer);

    // Let hardware reach B before firmware handles A's earlier selection.
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Original));
    controller.command(Untalk);
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Peer));
    auto wire = system.trace_ieee();
    controller.write(pet::Pia2ControlA, PiaC2Low.bits());
    controller.write(pet::ViaPortB, BusReleased);
    service_rx(system, firmware, "service accumulated A-to-B selections");
    const std::vector<Record> expected{
        data(payload[3]), data(payload[4]), data(payload[5], Eoi::Last)};
    std::vector<Record> received;
    for (size_t i = 0; i < expected.size(); ++i) received.push_back(controller.receive());
    require_sequence(received, expected, "queued selection B CPU payload");
    require_sequence(wire.records(), expected, "queued selection B wire isolation");

    // A's untouched prepared response must still start at its first byte.
    controller.command(Untalk);
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Original));
    service_rx(system, firmware, "service original queued-selection continuation");
    receive_payload(system, controller,
        {data(payload[0]), data(payload[1]), data(payload[2], Eoi::Last)});
}

// OPEN on the command channel dispatches binary P rather than treating it as a filename.
TEST_P(DriveRelativeTest, CommandChannelOpenExecutesBinaryPosition) {
    const bool d80 = GetParam();
    constexpr uint8_t RecordLength = 2;
    constexpr size_t RecordCount = 45;
    std::vector<uint8_t> payload(RecordCount * RecordLength);
    for (size_t index = 0; index < payload.size(); ++index)
        payload[index] = static_cast<uint8_t>(index + 1);
    IeeeController controller(system);
    firmware.mount(relative_image(d80, payload, RecordLength),
        d80 ? "command-open.d80" : "command-open.d64");
    open_file(system, controller, Channel{DataChannel}, "BASIC,L");
    service_rx(system, firmware, "service command-OPEN REL file");

    // Record-number bytes may contain CR or comma, and the high byte is zero.
    for (const uint8_t record : {uint8_t{2}, uint8_t{'\r'}, uint8_t{','}}) {
        const std::string payload_command{
            'P', static_cast<char>(IEEE_CMD_SECONDARY(DataChannel)),
            static_cast<char>(record), '\0', '\1',
        };
        auto rx = firmware.trace_rx();
        open_file(system, controller, Channel{CommandChannel}, payload_command);
        service_rx(system, firmware, "service binary P through command OPEN");
        std::vector<Record> expected{
            command(Listen(Device{DeviceAddress})), command(Open(Channel{CommandChannel})),
        };
        for (size_t index = 0; index < payload_command.size(); ++index)
            expected.push_back(data(static_cast<uint8_t>(payload_command[index]),
                index + 1 == payload_command.size() ? Eoi::Last : Eoi::More));
        expected.push_back(command(Unlisten));
        EXPECT_THAT(rx.capture().records(), testing::ElementsAreArray(expected));

        // Independent CPU/wire status and payload checks observe RECORD's actual effect.
        EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}),
            d80 ? "00, OK,00,00,0\r" : "00, OK,00,00\r");
        controller.command(Talk(Device{DeviceAddress}));
        controller.command(Secondary(Channel{DataChannel}));
        service_rx(system, firmware, "service positioned REL TALK after command OPEN");
        const size_t offset = (record - 1) * RecordLength;
        receive_payload(system, controller,
            {data(payload[offset], Eoi::More), data(payload[offset + 1], Eoi::Last)});
        controller.command(Untalk);
        service_rx(system, firmware, "service command-OPEN data UNTALK");
    }
}

// RECORD reports missing and non-REL channels distinctly, without poisoning recovery.
TEST_P(DriveRelativeTest, CommandChannelErrorsAndRecovery) {
    const bool d80 = GetParam();
    constexpr Channel Sequential{DataChannel + 1}, Missing{DataChannel + 2};
    const std::vector<uint8_t> payload{1, 2, 3, 4, 5, 6};
    auto image = relative_sequential_image(d80, payload);
    firmware.mount(image, d80 ? "command-errors.d80" : "command-errors.d64");
    IeeeController controller(system);
    open_file(system, controller, Channel{DataChannel}, "BASIC,L");
    service_rx(system, firmware, "service REL error-control OPEN");
    open_file(system, controller, Sequential, "PLAIN");
    service_rx(system, firmware, "service sequential error-control OPEN");

    for (const auto entry : {Open(Channel{CommandChannel}), Secondary(Channel{CommandChannel})}) {
        for (const auto channel : {Missing, Sequential, Channel{CommandChannel}}) {
            position_relative(system, firmware, controller, 1, 1);
            controller.command(Listen(Device{DeviceAddress}));
            controller.command(entry);
            for (const auto byte : {'P', static_cast<char>(IEEE_CMD_SECONDARY(channel.value())),
                    '\3', '\0'})
                controller.data(static_cast<uint8_t>(byte));
            controller.data(1, Eoi::Last);
            controller.command(Unlisten);
            service_rx(system, firmware, "service invalid-channel P");
            const std::string error = channel.value() == Missing.value()
                ? "70,NO CHANNEL,00,00" : "64, FILE TYPE MISMATCH,00,00";
            EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}),
                error + (d80 ? ",0\r" : "\r"));
            controller.command(Talk(Device{DeviceAddress}));
            controller.command(Secondary(Channel{DataChannel}));
            service_rx(system, firmware, "service unchanged REL after invalid P");
            receive_payload(system, controller, {data(1), data(2, Eoi::Last)});
            controller.command(Untalk);

            // Recover through the same entry, retaining exact binary zero bytes.
            const std::string command_bytes{'P', static_cast<char>(IEEE_CMD_SECONDARY(DataChannel)),
                '\3', '\0', '\1'};
            if (entry.category() == CommandCategory::Open) {
                open_file(system, controller, Channel{CommandChannel}, command_bytes);
                service_rx(system, firmware, "service recovery command OPEN");
            } else {
                position_relative(system, firmware, controller, 3, 1);
            }
            EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}),
                d80 ? "00, OK,00,00,0\r" : "00, OK,00,00\r");
            controller.command(Talk(Device{DeviceAddress}));
            controller.command(Secondary(Channel{DataChannel}));
            service_rx(system, firmware, "service recovered REL");
            receive_payload(system, controller, {data(5), data(6, Eoi::Last)});
            controller.command(Untalk);
        }
    }
}

// Crossing from an unfinished command to ordinary OPEN consumes the same input.
TEST_P(DriveRelativeTest, OrdinaryOpenConsumesPartialCommand) {
    const bool d80 = GetParam();
    const std::vector<uint8_t> payload{1, 2, 3, 4, 5, 6};
    firmware.mount(relative_image(d80, payload, 2), d80 ? "shared-input.d80" : "shared-input.d64");
    IeeeController controller(system);
    open_file(system, controller, Channel{DataChannel}, "BASIC,L");
    service_rx(system, firmware, "service shared-input REL OPEN");
    for (const auto entry : {Open(Channel{CommandChannel}), Secondary(Channel{CommandChannel})}) {
        controller.command(Listen(Device{DeviceAddress}));
        controller.command(entry);
        for (const auto byte : {'P', static_cast<char>(IEEE_CMD_SECONDARY(DataChannel)), '\3', '\0', '\1'})
            controller.data(static_cast<uint8_t>(byte));
        controller.command(Unlisten);
        service_rx(system, firmware, "service partial P before ordinary OPEN");
        open_file(system, controller, Channel{DataChannel + 1}, "MISSING");
        service_rx(system, firmware, "service ordinary OPEN over retained command");
        controller.command(Listen(Device{DeviceAddress}));
        controller.command(entry);
        for (const auto byte : {'P', static_cast<char>(IEEE_CMD_SECONDARY(DataChannel)), '\2', '\0'})
            controller.data(static_cast<uint8_t>(byte));
        controller.data(1, Eoi::Last);
        controller.command(Unlisten);
        service_rx(system, firmware, "service fresh P after ordinary OPEN");
        controller.command(Talk(Device{DeviceAddress}));
        controller.command(Secondary(Channel{DataChannel}));
        service_rx(system, firmware, "service shared-input positioned REL");
        receive_payload(system, controller, {data(3), data(4, Eoi::Last)});
        controller.command(Untalk);
        service_rx(system, firmware, "service shared-input UNTALK");
    }
}

// EOI, not UNLISTEN, completes a retained binary command exactly once.
TEST_P(DriveRelativeTest, CommandCompletionRequiresReceivedEoi) {
    const bool d80 = GetParam();
    const std::vector<uint8_t> payload{1, 2, 3, 4, 5, 6, 7, 8};
    firmware.mount(relative_image(d80, payload, 2), d80 ? "command-eoi.d80" : "command-eoi.d64");
    IeeeController controller(system);
    open_file(system, controller, Channel{DataChannel}, "BASIC,L");
    service_rx(system, firmware, "service EOI-control REL OPEN");

    for (const auto entry : {Open(Channel{CommandChannel}), Secondary(Channel{CommandChannel})}) {
        position_relative(system, firmware, controller, 1, 1);
        {
            auto wire = system.trace_ieee();
            auto rx = firmware.trace_rx();
            controller.command(Listen(Device{DeviceAddress}));
            controller.command(entry);
            for (const auto byte : {'P', static_cast<char>(IEEE_CMD_SECONDARY(DataChannel)), '\3'})
                controller.data(static_cast<uint8_t>(byte));
            service_rx(system, firmware, "service unfinished P prefix");
            controller.data(0);
            controller.data(1);
            controller.command(Unlisten);
            service_rx(system, firmware, "service P without EOI");
            const std::vector<Record> expected{
                command(Listen(Device{DeviceAddress})), command(entry),
                data('P'), data(0x62), data(3), data(0), data(1), command(Unlisten)};
            require_sequence(wire.records(), expected, "missing-EOI command wire");
            EXPECT_EQ(rx.records(), expected);
        }
        controller.command(Talk(Device{DeviceAddress}));
        controller.command(Secondary(Channel{DataChannel}));
        service_rx(system, firmware, "service unpositioned REL");
        receive_payload(system, controller, {data(1), data(2, Eoi::Last)});
        controller.command(Untalk);

        // Readdress and finish with CR/EOI, then TALK without any UNLISTEN.
        controller.command(Listen(Device{DeviceAddress}));
        controller.command(entry);
        controller.data('\r', Eoi::Last);
        service_rx(system, firmware, "service EOI-completed P before UNLISTEN");
        controller.command(Talk(Device{DeviceAddress}));
        controller.command(Secondary(Channel{DataChannel}));
        service_rx(system, firmware, "service immediate TALK before idle dispatch");
        receive_payload(system, controller, {data(3), data(4, Eoi::Last)});
        controller.command(Untalk);
        controller.command(Unlisten);
        controller.command(Talk(Device{DeviceAddress}));
        controller.command(Secondary(Channel{DataChannel}));
        service_rx(system, firmware, "service REL after late UNLISTEN");
        receive_payload(system, controller, {data(5), data(6, Eoi::Last)});
        controller.command(Untalk);
        controller.command(Unlisten);
        controller.command(Talk(Device{DeviceAddress}));
        controller.command(Secondary(Channel{DataChannel}));
        service_rx(system, firmware, "service REL after one-time pending dispatch");
        receive_payload(system, controller, {data(7), data(8, Eoi::Last)});
        controller.command(Untalk);
    }
}

// All EOI payloads in one LISTEN remain one native CMDWAT buffer until IDLE.
TEST_P(DriveRelativeTest, PendingCommandsDispatchOnlyAtIdle) {
    const bool d80 = GetParam();
    const std::vector<uint8_t> payload{1, 2, 3, 4, 5, 6};
    firmware.mount(relative_image(d80, payload, 2), d80 ? "pending.d80" : "pending.d64");
    IeeeController controller(system);
    open_file(system, controller, Channel{DataChannel}, "BASIC,L");
    service_rx(system, firmware, "service pending-command OPEN");
    for (const auto entry : {Open(Channel{CommandChannel}), Secondary(Channel{CommandChannel})}) {
        for (const bool immediate_talk : {false, true}) {
            position_relative(system, firmware, controller, 1, 1);
            controller.command(Listen(Device{DeviceAddress}));
            controller.command(entry);
            for (const uint8_t record : {uint8_t{2}, uint8_t{3}}) {
                for (const auto byte : {uint8_t{'P'}, uint8_t{IEEE_CMD_SECONDARY(DataChannel)},
                        record, uint8_t{0}})
                    controller.data(byte);
                controller.data(1, Eoi::Last);
                service_rx(system, firmware, "service EOI without idle dispatch");
            }
            if (immediate_talk) {
                // Keep UNLISTEN/TALK/SECONDARY in one ATN phase, as the ROM decoder does.
                controller.raw_send(command(Unlisten));
                controller.command(Talk(Device{DeviceAddress}));
                controller.command(Secondary(Channel{DataChannel}));
                controller.release_atn();
                service_rx(system, firmware, "service continuous-ATN immediate TALK");
                receive_payload(system, controller, {data(1), data(2, Eoi::Last)});
                controller.command(Untalk);
            } else {
                controller.command(Unlisten);
            }
            service_rx(system, firmware, "service IDLE pending-command dispatch");
            controller.command(Talk(Device{DeviceAddress}));
            controller.command(Secondary(Channel{DataChannel}));
            service_rx(system, firmware, "service first P record");
            receive_payload(system, controller, {data(3), data(4, Eoi::Last)});
            controller.command(Untalk);
            service_rx(system, firmware, "service pending-command UNTALK");
        }
    }
}

// Unopened TALK returns via NOTLK to IDLE while retaining TALK ownership.
TEST_P(DriveRelativeTest, PendingCommandDispatchesAfterUnopenedTalk) {
    const bool d80 = GetParam();
    const std::vector<uint8_t> payload{1, 2, 3, 4, 5, 6, 7, 8};
    firmware.mount(relative_image(d80, payload, 2), d80 ? "notlk.d80" : "notlk.d64");
    IeeeController controller(system);
    open_file(system, controller, Channel{DataChannel}, "BASIC,L");
    service_rx(system, firmware, "open NOTLK REL target");
    enum Selection { UnopenedChannel, ClosedChannel, DefaultChannel };
    for (const auto entry : {Open(Channel{CommandChannel}), Secondary(Channel{CommandChannel})}) {
        for (const auto selection : {UnopenedChannel, ClosedChannel, DefaultChannel}) {
            if (selection == ClosedChannel) {
                open_file(system, controller, Channel{DataChannel + 1}, "BASIC,L");
                service_rx(system, firmware, "open NOTLK close control");
                controller.command(Listen(Device{DeviceAddress}));
                controller.command(Close(Channel{DataChannel + 1}));
                controller.command(Unlisten);
                service_rx(system, firmware, "close NOTLK secondary");
            }
            position_relative(system, firmware, controller, 1, 1);
            controller.command(Listen(Device{DeviceAddress}));
            controller.command(entry);
            const std::vector<uint8_t> position{'P', IEEE_CMD_SECONDARY(DataChannel), 3, 0, 1};
            for (size_t index = 0; index < position.size(); ++index)
                controller.data(position[index], index + 1 == position.size() ? Eoi::Last : Eoi::More);
            service_rx(system, firmware, "retain EOI-completed P while listening");
            controller.raw_send(command(Unlisten));
            controller.command(Talk(Device{DeviceAddress}));
            if (selection != DefaultChannel)
                controller.command(Secondary(Channel{DataChannel + 1}));
            controller.release_atn();
            service_rx(system, firmware, "return unopened TALK to IDLE");
            controller.command(Talk(Device{DeviceAddress}));
            controller.command(Secondary(Channel{DataChannel}));
            service_rx(system, firmware, "reselect REL without UNTALK");
            receive_payload(system, controller, {data(5), data(6, Eoi::Last)});
            controller.command(Untalk);
            service_rx(system, firmware, "verify completed P cannot repeat");
            controller.command(Talk(Device{DeviceAddress}));
            controller.command(Secondary(Channel{DataChannel}));
            service_rx(system, firmware, "read following record after NOTLK dispatch");
            receive_payload(system, controller, {data(7), data(8, Eoi::Last)});
            controller.command(Untalk);
        }
    }
}

// Native WRTREL finishes on EOI and keeps LISTEN active for the next record.
TEST_P(DriveRelativeTest, RelativeEoiCompletesEachRecordWithoutUnlisten) {
    const bool d80 = GetParam();
    for (const uint8_t length : {uint8_t{2}, uint8_t{129}, uint8_t{DISKIMAGE_REL_MAX_RECORD_LENGTH}}) {
        SCOPED_TRACE(::testing::Message() << "record length " << unsigned{length});
        const std::string filename = "relative-eoi-" + std::to_string(length)
            + (d80 ? ".d80" : ".d64");
        std::vector<uint8_t> payload(3 * size_t{length}, 0xa0);
        firmware.mount(relative_image(d80, payload, length), filename.c_str());
        IeeeController controller(system);
        open_file(system, controller, Channel{DataChannel}, "BASIC,L");
        service_rx(system, firmware, "open EOI write target");
        position_relative(system, firmware, controller, 1, 2);
        position_relative(system, firmware, controller, 1, 1, Channel{DataChannel + 1});
        controller.command(Listen(Device{DeviceAddress}));
        controller.command(Secondary(Channel{DataChannel}));
        for (unsigned int completed = 1; completed <= 3; ++completed) {
            if (completed <= 2) {
                if (completed == 2) {
                    controller.data(0xb5, Eoi::More);
                    service_rx(system, firmware, "retain partial second REL record");
                }
                controller.data(completed == 1 ? 0xa5 : 0xb6, Eoi::Last);
                service_rx(system, firmware, "complete REL record without UNLISTEN");
                const size_t start = (completed - 1) * size_t{length};
                payload[start + 1] = completed == 1 ? 0xa5 : 0xb6;
                if (completed == 2) payload[start] = 0xb5;
                std::fill(payload.begin() + start + 2,
                    payload.begin() + start + length, 0);
                EXPECT_TRUE(system.ieee().read_status().contains(fpga::IeeeStatusBit::Listening));
            } else {
                controller.command(Unlisten);
                service_rx(system, firmware, "UNLISTEN without an empty successor write");
            }
            // Compare the entire persisted image, not merely a prepared read response.
            expect_persisted_image(filename.c_str(), relative_image(d80, payload, length));
        }
        EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}),
            d80 ? "00, OK,00,00,0\r" : "70,NO CHANNEL,00,00\r");
        position_relative(system, firmware, controller, 1, 1);
        controller.command(Talk(Device{DeviceAddress}));
        controller.command(Secondary(Channel{DataChannel}));
        service_rx(system, firmware, "read first EOI-written record");
        receive_payload(system, controller, {data(0xa0), data(0xa5, Eoi::Last)});
        controller.command(Untalk);
        position_relative(system, firmware, controller, 2, 1);
        controller.command(Talk(Device{DeviceAddress}));
        controller.command(Secondary(Channel{DataChannel}));
        service_rx(system, firmware, "read second EOI-written record");
        receive_payload(system, controller, {data(0xb5), data(0xb6, Eoi::Last)});
        controller.command(Untalk);
        service_rx(system, firmware, "finish REL EOI write regression");
    }
}

// Both kinds of REL EOI retain prepared CHNDAT when another channel writes.
TEST_P(DriveRelativeTest, EoiRetainsPreparedSuccessorAcrossPeerWrite) {
    const bool d80 = GetParam();
    constexpr Channel PeerChannel{DataChannel + 1};
    for (const bool write_eoi : {false, true}) {
        SCOPED_TRACE(write_eoi ? "write EOI" : "read EOI");
        const std::vector<uint8_t> payload{0, 0, 0xa5, 0, 0xcc, 0xdd};
        const char* const filename = write_eoi ? "prepared-write.img" : "prepared-read.img";
        firmware.mount(relative_image(d80, payload, 2), filename);
        IeeeController controller(system);
        open_file(system, controller, Channel{DataChannel}, "BASIC,L");
        service_rx(system, firmware, "open prepared-response channel");
        open_file(system, controller, PeerChannel, "BASIC,L");
        service_rx(system, firmware, "open prepared-response peer");
        if (write_eoi) {
            controller.command(Listen(Device{DeviceAddress}));
            controller.command(Secondary(Channel{DataChannel}));
            controller.data(0x80, Eoi::Last);
            service_rx(system, firmware, "prepare successor after write EOI");
        } else {
            controller.command(Talk(Device{DeviceAddress}));
            controller.command(Secondary(Channel{DataChannel}));
            service_rx(system, firmware, "read empty first record");
            receive_payload(system, controller, {data(0, Eoi::Last)});
            controller.command(Untalk);
            service_rx(system, firmware, "prepare successor after read EOI");
        }
        position_relative(system, firmware, controller, 2, 1, PeerChannel);
        controller.command(Listen(Device{DeviceAddress}));
        controller.command(Secondary(PeerChannel));
        controller.data('Z');
        controller.data('Y', Eoi::Last);
        controller.command(Unlisten);
        service_rx(system, firmware, "overwrite prepared successor through peer");
        controller.command(Talk(Device{DeviceAddress}));
        controller.command(Secondary(Channel{DataChannel}));
        service_rx(system, firmware, "restore prepared response without P");
        receive_payload(system, controller, {data(0xa5, Eoi::Last)});
        controller.command(Untalk);
        position_relative(system, firmware, controller, 2, 1);
        controller.command(Talk(Device{DeviceAddress}));
        controller.command(Secondary(Channel{DataChannel}));
        service_rx(system, firmware, "P replaces prepared response with backing data");
        receive_payload(system, controller, {data('Z'), data('Y', Eoi::Last)});
        controller.command(Untalk);
        service_rx(system, firmware, "finish prepared-response control");
    }
}

// Native CLSALL closes only the addressed unit, preserving a peer's live source.
TEST_P(DriveRelativeTest, Close15ReleasesChannelsAndPreservesPeerTalk) {
    const bool d80 = GetParam();
    const std::vector<uint8_t> payload{0xa5, 0xa6, 0x5a, 0x5b};
    const char* const filename = "close15.img";
    firmware.mount(relative_sequential_image(d80, payload), filename);
    constexpr Device Peer{DeviceAddress + 1};
    constexpr Channel Second{DataChannel + 1}, Sequential{DataChannel + 2};
    ASSERT_TRUE(ieee_drive_mount(2, filename));
    IeeeController controller(system);
    for (const bool peer_relative : {true, false}) {
        SCOPED_TRACE(peer_relative ? "peer REL" : "peer SEQ");
        open_file(system, controller, Channel{DataChannel}, "BASIC,L");
        service_rx(system, firmware, "open first CLOSE-15 target");
        open_file(system, controller, Second, "BASIC,L");
        service_rx(system, firmware, "open second CLOSE-15 target");
        open_file(system, controller, Sequential, "PLAIN");
        service_rx(system, firmware, "open ordinary CLOSE-15 target");
        position_relative(system, firmware, controller, 1, 1, Channel{DataChannel + 3});
        open_file(system, controller, Channel{DataChannel}, peer_relative ? "BASIC,L" : "PLAIN", Peer);
        service_rx(system, firmware, "open CLOSE-15 peer control");
        controller.command(Talk(Peer));
        controller.command(Secondary(Channel{DataChannel}));
        service_rx(system, firmware, "select peer before unit-local CLOSE");
        ASSERT_EQ(controller.receive(), data(0xa5));
        controller.command(Listen(Device{DeviceAddress}));
        controller.command(Close(Channel{CommandChannel}));
        controller.command(Unlisten);
        service_rx(system, firmware, "close all channels without dropping peer response");
        receive_payload(system, controller, peer_relative
            ? std::vector<Record>{data(0xa6, Eoi::Last)}
            : std::vector<Record>{data(0xa6), data(0x5a), data(0x5b, Eoi::Last)});
        controller.command(Untalk);
        EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}),
            d80 ? "00, OK,00,00,0\r" : "00, OK,00,00\r");
        for (const Channel channel : {Channel{DataChannel}, Second, Sequential}) {
            position_relative(system, firmware, controller, 1, 1, channel);
            EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}),
                d80 ? "70,NO CHANNEL,00,00,0\r" : "70,NO CHANNEL,00,00\r");
        }
        open_file(system, controller, Channel{DataChannel}, "BASIC,L");
        service_rx(system, firmware, "reuse a channel after CLOSE 15");
        controller.command(Talk(Device{DeviceAddress}));
        controller.command(Secondary(Channel{DataChannel}));
        service_rx(system, firmware, "read reopened channel");
        receive_payload(system, controller, {data(0xa5), data(0xa6, Eoi::Last)});
        controller.command(Untalk);
        service_rx(system, firmware, "finish CLOSE-15 reuse");
    }
}

// Native CLSALL drive selection follows descending SAs and persistent written flags.
TEST_P(DriveRelativeTest, Close15SelectsMixedDriveByWrittenChannel) {
    const bool d80 = GetParam();
    const std::vector<uint8_t> payload{0xa0, 0xa1, 0xa5, 0, 0xcc, 0xdd};
    firmware.mount(relative_sequential_image(d80, payload), "close-drive0.img");
    firmware.mount(relative_sequential_image(d80, payload), "close-drive1.img");
    ASSERT_TRUE(ieee_drive_mount(1, "close-drive1.img"));
    ASSERT_TRUE(ieee_drive_mount(0, "close-drive0.img"));
    IeeeController controller(system);
    enum class Pattern { NoWrites, LowerWritten, UpperWritten, BothWritten, Reopened, Armed };
    constexpr Channel Lower{DataChannel}, Upper{DataChannel + 1}, Sequential{0};
    const auto status = [d80](unsigned int drive) {
        return std::string("00, OK,00,00") + (d80 ? "," + std::to_string(drive) : "") + "\r";
    };
    for (const unsigned int lower_drive : {0u, 1u}) {
        for (const auto pattern : {Pattern::NoWrites, Pattern::LowerWritten, Pattern::UpperWritten,
                Pattern::BothWritten, Pattern::Reopened, Pattern::Armed}) {
            SCOPED_TRACE(::testing::Message() << "lower drive " << lower_drive
                << ", pattern " << static_cast<int>(pattern));
            const unsigned int upper_drive = lower_drive ^ 1u;
            const bool lower_written = pattern == Pattern::LowerWritten
                || pattern == Pattern::BothWritten || pattern == Pattern::Reopened;
            const bool upper_written = pattern == Pattern::UpperWritten
                || pattern == Pattern::BothWritten || pattern == Pattern::Reopened
                || pattern == Pattern::Armed;

            // Both allocations start clean, then completed writes set channel-local DYFILE.
            for (const Channel channel : {Lower, Upper}) {
                const auto drive = channel.value() == Lower.value() ? lower_drive : upper_drive;
                open_file(system, controller, channel, std::to_string(drive) + ":BASIC,L");
                service_rx(system, firmware, "open mixed-drive CLOSE target");
            }
            for (const Channel channel : {Lower, Upper}) {
                if (!(channel.value() == Lower.value() ? lower_written : upper_written)) continue;
                controller.command(Listen(Device{DeviceAddress}));
                controller.command(Secondary(channel));
                controller.data(0x5a, Eoi::Last);
                controller.command(Unlisten);
                service_rx(system, firmware, "complete mixed-drive REL write");
            }
            for (const Channel channel : {Lower, Upper})
                position_relative(system, firmware, controller, 2, 1, channel);
            if (pattern == Pattern::Reopened) {
                // CLOSE/reopen resets the written flag even when the allocation is reused.
                controller.command(Listen(Device{DeviceAddress}));
                controller.command(Close(Lower));
                controller.command(Unlisten);
                service_rx(system, firmware, "release written lower-SA allocation");
                EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}),
                    status(lower_drive));
                open_file(system, controller, Lower, std::to_string(lower_drive) + ":BASIC,L");
                service_rx(system, firmware, "reopen lower-SA allocation unwritten");
            } else if (pattern == Pattern::Armed) {
                // LISTEN without PUT must not mark a clean channel as written.
                controller.command(Listen(Device{DeviceAddress}));
                controller.command(Secondary(Lower));
                controller.command(Unlisten);
                service_rx(system, firmware, "arm lower-SA channel without writing");
            }
            const auto selected_drive = !d80 || (lower_written && pattern != Pattern::Reopened)
                ? lower_drive : upper_drive;
            const auto prior_drive = pattern == Pattern::NoWrites
                ? selected_drive : selected_drive ^ 1u;
            // A lower-SA SEQ reader selects prior DRVNUM but does not update it during close.
            open_file(system, controller, Sequential, std::to_string(prior_drive) + ":PLAIN");
            service_rx(system, firmware, "select conflicting status drive with SEQ reader");
            EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}),
                status(prior_drive));
            for (unsigned int repetition = 0; repetition < 2; ++repetition) {
                controller.command(Listen(Device{DeviceAddress}));
                controller.command(Close(Channel{CommandChannel}));
                controller.command(Unlisten);
                service_rx(system, firmware, "close mixed-drive unit in native SA order");
                EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}),
                    status(selected_drive));
            }
        }
    }
}

// Revision-12 fputrec writes two records under one LISTEN, then native input reads them.
TEST_P(DriveRelativeTest, WaterlooRecordWritePersistsAndReadsBack) {
    const bool d80 = GetParam();
    constexpr uint8_t RecordLength = 4, EoiFlag = 1, WriteAccess = 2;
    constexpr econopet::CpuAddress Fputrec{0xb367}, Syswrite{0xc52c}, Sysnl{0xc6c6};
    constexpr test_asm::mc6809::Instruction<2> PushD{{0x34, 0x06}}, DropWord{{0x32, 0x62}};
    const uint8_t filename[] = {'B', 'A', 'S', 'I', 'C', ',', 'L', 0};
    const uint8_t written[] = {0xa5, 0x5a, 0xb5, 0xb6};
    std::vector<uint8_t> payload(3 * RecordLength, 0xa0);
    const char* const backing = "waterloo-write.img";
    firmware.mount(relative_image(d80, payload, RecordLength), backing);
    WaterlooFixture rom(system);
    auto test = rom.program(test_asm::ProgramMode::Phased);
    const auto source = test.data("write records", NativePayload, sizeof(written));
    const auto fcb = test.data("text FCB", econopet::CpuAddress{0x0a00}, 6);
    const auto received = test.data("record readback", econopet::CpuAddress{0x0b00}, 6,
        test_asm::Capture::Yes);
    const auto flags = test.data("readback EOI", econopet::CpuAddress{0x0b10}, 6,
        test_asm::Capture::Yes);
    const auto status = test.byte("record write status");
    const auto checkpoint = system.cpu(CPU_SOFT_6809).peek_fetch_checkpoint();
    auto wire = system.trace_ieee();
    auto execution = test.within(TerminalRomBudget)
        .every(RomServicePeriod, ServiceTiming::AfterSlice, firmware.service_adapter())
        .pause([&](auto& p) {
            rom.init_ieee(p);
            rom.device(p, DeviceAddress, DataChannel);
            rom.file(p, filename);
            p.lda(test_asm::imm(0)).sta(WaterlooActiveRole).sta(WaterlooGlobalStatus)
             .lda(test_asm::imm(1)).sta(WaterlooWorkspace::WaitParameterLow)
             .ldd(test_asm::imm16(WaterlooWorkspace::Block)).jsr(rom.entry(WaterlooRoutine::Open));
            p.pause("Waterloo write OPEN unlistened");
            // fputrec takes an FCB, not the transport parameter block in D.
            const uint8_t descriptor[] = {'t', 0, 0, 0, 1, 0};
            p.store(fcb, descriptor).store(source, written)
             .lda(test_asm::imm(WriteAccess)).sta(WaterlooWorkspace::Block + 1)
             .lda(test_asm::imm(0)).sta(WaterlooWorkspace::Block + 3)
             .lda(test_asm::imm(1)).sta(WaterlooWorkspace::Block + 4);
            for (unsigned int record = 0; record < 2; ++record) {
                // Stack arguments are buffer, count above JSR's return address.
                p.ldd(test_asm::imm16(2)).emit(PushD)
                 .ldd(test_asm::imm16(source.byte(2 * record).address())).emit(PushD)
                 .ldd(test_asm::imm16(fcb.address())).jsr(Fputrec)
                 .emit(DropWord).emit(DropWord)
                 .lda(WaterlooWorkspace::Block).sta(status);
                p.pause(record == 0 ? "first Waterloo record EOI" : "second Waterloo record EOI");
            }
            p.ldd(test_asm::imm16(1)).emit(PushD)
             .ldd(test_asm::imm16(WaterlooWorkspace::Block)).jsr(rom.entry(WaterlooRoutine::RelPosition))
             .emit(DropWord);
            p.pause("Waterloo readback P unlistened");
            for (unsigned int record = 0; record < 2; ++record) {
                p.ldd(test_asm::imm16(WaterlooWorkspace::Block)).jsr(rom.entry(WaterlooRoutine::Talk))
                 .lda(test_asm::imm(0)).sta(WaterlooWorkspace::DiskFlags);
                for (unsigned int index = 0; index < 3; ++index)
                    p.jsr(rom.entry(WaterlooRoutine::Input))
                     .stb(received.byte(3 * record + index).address())
                     .lda(WaterlooWorkspace::DiskFlags).sta(flags.byte(3 * record + index));
                p.jsr(rom.entry(WaterlooRoutine::Untalk));
            }
        });
    ASSERT_EQ(execution.name(), "Waterloo write OPEN unlistened");
    service_rx(system, firmware, "dispatch native Waterloo write OPEN");
    for (unsigned int record = 0; record < 2; ++record) {
        auto configuration = system.execution();
        configuration.within(TerminalRomBudget)
            .every(RomServicePeriod, ServiceTiming::AfterSlice, firmware.service_adapter());
        execution = test.resume(execution, configuration);
        ASSERT_EQ(execution.name(), record == 0 ? "first Waterloo record EOI" : "second Waterloo record EOI");
        service_rx(system, firmware, "persist native Waterloo record EOI");
        const size_t start = record * RecordLength;
        payload[start] = written[2 * record];
        payload[start + 1] = written[2 * record + 1];
        payload[start + 2] = '\r';
        payload[start + 3] = 0;
        expect_persisted_image(backing, relative_image(d80, payload, RecordLength));
        EXPECT_TRUE(system.ieee().read_status().contains(fpga::IeeeStatusBit::Listening));
    }
    auto position_execution = system.execution();
    position_execution.within(TerminalRomBudget)
        .every(RomServicePeriod, ServiceTiming::AfterSlice, firmware.service_adapter());
    execution = test.resume(execution, position_execution);
    ASSERT_EQ(execution.name(), "Waterloo readback P unlistened");
    service_rx(system, firmware, "dispatch native Waterloo readback P");
    auto read_execution = system.execution();
    read_execution.within(TerminalRomBudget)
        .every(RomServicePeriod, ServiceTiming::AfterSlice, firmware.service_adapter());
    const auto complete = test.resume(execution, read_execution);
    ASSERT_TRUE(complete.complete());
    const auto result = test.capture(complete);
    EXPECT_EQ(result[status], 0);
    EXPECT_EQ(result[received], (std::vector<uint8_t>{0xa5, 0x5a, '\r', 0xb5, 0xb6, '\r'}));
    for (unsigned int index = 0; index < received.size(); ++index)
        EXPECT_EQ(result[flags.byte(index)] & EoiFlag, index % 3 == 2 ? EoiFlag : 0);
    EXPECT_TRUE(system.cpu(CPU_SOFT_6809).peek_opcode_fetched_at(Fputrec, checkpoint));
    EXPECT_TRUE(system.cpu(CPU_SOFT_6809).peek_opcode_fetched_at(Syswrite, checkpoint));
    EXPECT_TRUE(system.cpu(CPU_SOFT_6809).peek_opcode_fetched_at(Sysnl, checkpoint));
    const std::vector<Record> expected{
        command(Listen(Device{DeviceAddress})), command(Open(Channel{DataChannel})),
        data('B'), data('A'), data('S'), data('I'), data('C'), data(','), data('L', Eoi::Last),
        command(Unlisten), command(Listen(Device{DeviceAddress})), command(Secondary(Channel{DataChannel})),
        data(0xa5), data(0x5a), data('\r', Eoi::Last),
        data(0xb5), data(0xb6), data('\r', Eoi::Last),
        command(Unlisten), command(Listen(Device{DeviceAddress})), command(Secondary(Channel{CommandChannel})),
        data('P'), data(DataChannel), data(1), data(0), data(0, Eoi::Last), command(Unlisten),
        command(Talk(Device{DeviceAddress})), command(Secondary(Channel{DataChannel})),
        data(0xa5), data(0x5a), data('\r', Eoi::Last), command(Untalk),
        command(Talk(Device{DeviceAddress})), command(Secondary(Channel{DataChannel})),
        data(0xb5), data(0xb6), data('\r', Eoi::Last), command(Untalk),
    };
    require_sequence(wire.records(), expected, "Waterloo fputrec persistence and native input");
}

// EOF queued in transport remains ready, but its final GET makes SEQ NOTRDY.
TEST_P(DriveRelativeTest, PendingCommandDispatchesAfterSequentialEof) {
    const bool d80 = GetParam();
    constexpr Channel Sequential{DataChannel + 1};
    const std::vector<uint8_t> payload{1, 2, 3, 4, 5, 6, 7, 8};
    firmware.mount(relative_sequential_image(d80, payload), d80 ? "eof-idle.d80" : "eof-idle.d64");
    IeeeController controller(system);
    open_file(system, controller, Channel{DataChannel}, "BASIC,L");
    service_rx(system, firmware, "open REL EOF-dispatch target");
    for (const auto entry : {Open(Channel{CommandChannel}), Secondary(Channel{CommandChannel})}) {
        for (const bool consumed : {true, false}) {
            position_relative(system, firmware, controller, 1, 1);
            open_file(system, controller, Sequential, "PLAIN");
            service_rx(system, firmware, "open SEQ EOF control");
            controller.command(Talk(Device{DeviceAddress}));
            controller.command(Secondary(Sequential));
            service_rx(system, firmware, "queue complete SEQ including EOF");
            const std::vector<Record> expected{
                data(1), data(2), data(3), data(4), data(5), data(6), data(7), data(8, Eoi::Last),
            };
            if (consumed) receive_payload(system, controller, expected);
            controller.command(Listen(Device{DeviceAddress}));
            controller.command(entry);
            const std::vector<uint8_t> position{'P', IEEE_CMD_SECONDARY(DataChannel), 3, 0, 1};
            for (size_t index = 0; index < position.size(); ++index)
                controller.data(position[index], index + 1 == position.size() ? Eoi::Last : Eoi::More);
            service_rx(system, firmware, "retain P before SEQ TALK");
            controller.raw_send(command(Unlisten));
            controller.command(Talk(Device{DeviceAddress}));
            controller.command(Secondary(Sequential));
            controller.release_atn();
            service_rx(system, firmware, "select queued or consumed SEQ EOF");
            if (!consumed) {
                // Early P dispatch would discard this unread FIFO, not just change timing.
                receive_payload(system, controller, expected);
                service_rx(system, firmware, "return completed SEQ TALK to IDLE");
            }
            controller.command(Talk(Device{DeviceAddress}));
            controller.command(Secondary(Channel{DataChannel}));
            service_rx(system, firmware, "reselect REL without UNTALK after SEQ EOF");
            receive_payload(system, controller, {data(5), data(6, Eoi::Last)});
            controller.command(Untalk);
            service_rx(system, firmware, "finish EOF dispatch control");
        }
    }
}

// Native TLK20 returns directly to IDLE when NDAC releases without an NRFD GET.
TEST_P(DriveRelativeTest, PendingCommandDispatchesAfterTalkWithoutGet) {
    const bool d80 = GetParam();
    firmware.mount(relative_image(d80, {1, 2, 3, 4, 5, 6}, 2),
        d80 ? "no-get.d80" : "no-get.d64");
    IeeeController controller(system);
    open_file(system, controller, Channel{DataChannel}, "BASIC,L");
    service_rx(system, firmware, "open REL no-GET target");
    for (const auto entry : {Open(Channel{CommandChannel}), Secondary(Channel{CommandChannel})}) {
        for (const Channel source : {Channel{DataChannel}, Channel{CommandChannel}}) {
            position_relative(system, firmware, controller, 1, 1);
            controller.command(Listen(Device{DeviceAddress}));
            controller.command(entry);
            const std::vector<uint8_t> position{'P', IEEE_CMD_SECONDARY(DataChannel), 3, 0, 1};
            for (size_t index = 0; index < position.size(); ++index)
                controller.data(position[index], index + 1 == position.size() ? Eoi::Last : Eoi::More);
            controller.raw_send(command(Unlisten));
            controller.command(Talk(Device{DeviceAddress}));
            controller.command(Secondary(source));
            controller.release_atn();
            service_rx(system, firmware, "defer P during ready TALK");
            controller.write(pet::Pia2ControlA, PiaC2Low.bits());
            controller.write(pet::ViaPortB, BusReleased);
            system.run_until([](const System& board) { return !board.snapshot().ieee_bus_dav_no; },
                ByteBudget, "wait for no-GET DAV");
            controller.write(pet::Pia2ControlA, PiaC2High.bits());
            system.run_until([](const System& board) { return board.snapshot().ieee_bus_dav_no; },
                ByteBudget, "NDAC-only termination releases DAV");
            controller.write(pet::ViaPortB, static_cast<uint8_t>(BusReleased & ~pet::Nrfd));
            service_rx(system, firmware, "dispatch at native unread TALK return");
            controller.command(Talk(Device{DeviceAddress}));
            controller.command(Secondary(Channel{DataChannel}));
            service_rx(system, firmware, "reselect without UNTALK after no GET");
            receive_payload(system, controller, {data(5), data(6, Eoi::Last)});
            controller.command(Untalk);
            service_rx(system, firmware, "finish no-GET dispatch control");
        }
    }
}

// An unallocated listener returns to IDLE after DAV, not merely after addressing.
TEST_P(DriveRelativeTest, PendingCommandDispatchesAfterUnopenedListen) {
    const bool d80 = GetParam();
    firmware.mount(relative_image(d80, {1, 2, 3, 4, 5, 6, 7, 8}, 2),
        d80 ? "listener-idle.d80" : "listener-idle.d64");
    IeeeController controller(system);
    open_file(system, controller, Channel{DataChannel}, "BASIC,L");
    service_rx(system, firmware, "open listener-IDLE REL target");
    constexpr Channel DiscardListener{IEEE_SAVE_CHANNEL};
    for (const auto entry : {Open(Channel{CommandChannel}), Secondary(Channel{CommandChannel})}) {
        // SA1 follows LSN25 and keeps accepting, unlike the unopened SA3 exit.
        for (const Channel listener : {Channel{DataChannel + 1}, DiscardListener}) {
            for (const bool send_byte : {false, true}) {
                position_relative(system, firmware, controller, 1, 1);
                controller.command(Listen(Device{DeviceAddress}));
                controller.command(entry);
                const std::vector<uint8_t> position{'P', IEEE_CMD_SECONDARY(DataChannel), 3, 0, 1};
                for (size_t index = 0; index < position.size(); ++index)
                    controller.data(position[index], index + 1 == position.size() ? Eoi::Last : Eoi::More);
                controller.raw_send(command(Unlisten));
                controller.command(Listen(Device{DeviceAddress}));
                controller.command(Secondary(listener));
                controller.release_atn();
                service_rx(system, firmware, "retain P while listener waits for DAV");
                if (send_byte) {
                    controller.write(pet::Pia2PortB, static_cast<uint8_t>(~uint8_t{0x55}));
                    controller.write(pet::Pia2ControlB, PiaC2Low.bits());
                    service_rx(system, firmware, "service discarded data while DAV remains asserted");
                    EXPECT_FALSE(system.snapshot().ieee_bus_dav_no);
                    controller.write(pet::Pia2ControlB, PiaC2High.bits());
                    controller.write(pet::Pia2PortB, BusReleased);
                    service_rx(system, firmware, "dispatch only after listener DAV releases");
                }
                EXPECT_TRUE(system.ieee().read_status().contains(fpga::IeeeStatusBit::Listening));
                controller.command(Talk(Device{DeviceAddress}));
                controller.command(Secondary(Channel{DataChannel}));
                service_rx(system, firmware, "select REL after native listener checkpoint");
                const bool dispatched = send_byte && listener.value() != DiscardListener.value();
                receive_payload(system, controller, dispatched
                    ? std::vector<Record>{data(5), data(6, Eoi::Last)}
                    : std::vector<Record>{data(1), data(2, Eoi::Last)});
                controller.command(Untalk);
                controller.command(Unlisten);
                service_rx(system, firmware, "finish listener dispatch control");
                controller.command(Talk(Device{DeviceAddress}));
                controller.command(Secondary(Channel{DataChannel}));
                service_rx(system, firmware, "verify listener dispatch occurs exactly once");
                receive_payload(system, controller, dispatched
                    ? std::vector<Record>{data(7), data(8, Eoi::Last)}
                    : std::vector<Record>{data(5), data(6, Eoi::Last)});
                controller.command(Untalk);
            }
        }
    }
}

// ATN abandons LSN21 before its DAV-release return can dispatch pending work.
TEST_P(DriveRelativeTest, InterruptedListenerDavDefersPendingCommand) {
    const bool d80 = GetParam();
    firmware.mount(relative_image(d80, {1, 2, 3, 4, 5, 6, 7, 8}, 2),
        d80 ? "listener-interrupted.d80" : "listener-interrupted.d64");
    IeeeController controller(system);
    open_file(system, controller, Channel{DataChannel}, "BASIC,L");
    service_rx(system, firmware, "open interrupted-listener REL target");
    for (const auto entry : {Open(Channel{CommandChannel}), Secondary(Channel{CommandChannel})}) {
        position_relative(system, firmware, controller, 1, 1);
        controller.command(Listen(Device{DeviceAddress}));
        controller.command(entry);
        const std::vector<uint8_t> position{'P', IEEE_CMD_SECONDARY(DataChannel), 3, 0, 1};
        for (size_t index = 0; index < position.size(); ++index)
            controller.data(position[index], index + 1 == position.size() ? Eoi::Last : Eoi::More);
        controller.raw_send(command(Unlisten));
        controller.command(Listen(Device{DeviceAddress}));
        controller.command(Secondary(Channel{DataChannel + 1}));
        controller.release_atn();
        service_rx(system, firmware, "retain P during unopened listener");

        // Interrupt the native DAV wait before releasing DAV or selecting TALK.
        controller.write(pet::Pia2PortB, static_cast<uint8_t>(~uint8_t{0x55}));
        controller.write(pet::Pia2ControlB, PiaC2Low.bits());
        EXPECT_FALSE(system.snapshot().ieee_bus_dav_no);
        controller.write(pet::ViaPortB, static_cast<uint8_t>(BusReleased & ~pet::Atn));
        controller.write(pet::Pia2ControlB, PiaC2High.bits());
        controller.write(pet::Pia2PortB, BusReleased);
        controller.raw_send(command(Unlisten));
        controller.command(Talk(Device{DeviceAddress}));
        controller.command(Secondary(Channel{DataChannel}));
        controller.release_atn();
        service_rx(system, firmware, "defer P across interrupted listener and ready TALK");
        receive_payload(system, controller, {data(1), data(2, Eoi::Last)});

        // A subsequent real IDLE must still execute the pending command once.
        controller.command(Untalk);
        service_rx(system, firmware, "dispatch retained P after TALK ends");
        controller.command(Talk(Device{DeviceAddress}));
        controller.command(Secondary(Channel{DataChannel}));
        service_rx(system, firmware, "read record selected at real IDLE");
        receive_payload(system, controller, {data(5), data(6, Eoi::Last)});
        controller.command(Untalk);
        service_rx(system, firmware, "finish interrupted-listener regression");
        controller.command(Talk(Device{DeviceAddress}));
        controller.command(Secondary(Channel{DataChannel}));
        service_rx(system, firmware, "verify retained P executes once");
        receive_payload(system, controller, {data(7), data(8, Eoi::Last)});
        controller.command(Untalk);
        service_rx(system, firmware, "finish interrupted-listener continuation");
    }
}

// A peer's deferred OPEN must not discard the final selected REL response.
TEST_P(DriveRelativeTest, DeferredPeerOpenPreservesSelectedRelativeResponse) {
    const bool d80 = GetParam();
    const char* const peer_name = d80 ? "deferred-peer.d80" : "deferred-peer.d64";
    firmware.mount(relative_sequential_image(d80, {0xa1, 0xa2, 0xa3, 0xa4}), peer_name);
    ASSERT_TRUE(ieee_drive_mount(2, peer_name));
    firmware.mount(relative_image(d80, {1, 2, 3, 4}, 2),
        d80 ? "deferred-source.d80" : "deferred-source.d64");
    IeeeController controller(system);
    const Device source{DeviceAddress}, peer{DeviceAddress + 1};
    constexpr Channel PeerChannel{DataChannel + 1};
    open_file(system, controller, Channel{DataChannel}, "BASIC,L", source);
    service_rx(system, firmware, "open deferred-OPEN source");
    for (const std::string_view filename : {"MISSING", "BASIC,L", "PLAIN"}) {
        position_relative(system, firmware, controller, 1, 1);
        controller.command(Listen(peer));
        controller.command(Open(PeerChannel));
        for (size_t index = 0; index < filename.size(); ++index)
            controller.data(static_cast<uint8_t>(filename[index]),
                index + 1 == filename.size() ? Eoi::Last : Eoi::More);
        controller.raw_send(command(Unlisten));
        controller.command(Talk(source));
        controller.command(Secondary(Channel{DataChannel}));
        controller.release_atn();
        service_rx(system, firmware, "resolve peer OPEN after source selection");
        receive_payload(system, controller, {data(1), data(2, Eoi::Last)});
        controller.command(Untalk);
        controller.command(Talk(source));
        controller.command(Secondary(Channel{DataChannel}));
        service_rx(system, firmware, "continue source after peer OPEN");
        receive_payload(system, controller, {data(3), data(4, Eoi::Last)});
        controller.command(Untalk);
        const std::string expected_status = std::string(filename == "MISSING"
            ? "62, FILE NOT FOUND,00,00" : "00, OK,00,00") + (d80 ? ",0\r" : "\r");
        EXPECT_EQ(receive_status(system, firmware, controller, peer), expected_status);
        if (filename != "MISSING") {
            controller.command(Talk(peer));
            controller.command(Secondary(PeerChannel));
            service_rx(system, firmware, "verify deferred peer OPEN actually resolved");
            receive_payload(system, controller, filename == "BASIC,L"
                ? std::vector<Record>{data(0xa1), data(0xa2, Eoi::Last)}
                : std::vector<Record>{data(0xa1), data(0xa2), data(0xa3), data(0xa4, Eoi::Last)});
            controller.command(Untalk);
        }
    }
}

// Model-specific receive saturation precedes the shared 58-byte CMDSET check.
TEST_P(DriveCommandLengthTest, CommandLengthBoundariesPreservePositionAndRecover) {
    const auto [d80, length] = GetParam();
    const std::vector<uint8_t> payload{1, 2, 3, 4, 5, 6};
    firmware.mount(relative_image(d80, payload, 2), d80 ? "limits.d80" : "limits.d64");
    IeeeController controller(system);
    open_file(system, controller, Channel{DataChannel}, "BASIC,L");
    service_rx(system, firmware, "service length-control OPEN");
    constexpr size_t NativeLimit = 58, ServiceChunk = 8;
    const size_t receive_capacity = d80 ? 59 : 41;
    for (const auto entry : {Open(Channel{CommandChannel}), Secondary(Channel{CommandChannel})}) {
        for (size_t trailing = 0; trailing <= 3; ++trailing) {
            position_relative(system, firmware, controller, 1, 1);
            controller.command(Listen(Device{DeviceAddress}));
            controller.command(entry);
            constexpr uint8_t Position[] = {'P', IEEE_CMD_SECONDARY(DataChannel), 2, 0, 1};
            for (size_t index = 0; index < length; ++index) {
                const uint8_t byte = trailing != 0 && index == length - trailing
                    ? '\r' : index < sizeof(Position) ? Position[index] : 'X';
                controller.data(byte, index + 1 == length ? Eoi::Last : Eoi::More);
                if ((index + 1) % ServiceChunk == 0)
                    service_rx(system, firmware, "service bounded command chunk");
            }
            controller.command(Unlisten);
            service_rx(system, firmware, "service CMDSET length check");
            size_t retained = std::min(length, receive_capacity);
            if (trailing != 0 && length - trailing == retained - 1) --retained;
            else if (trailing != 0 && length - trailing == retained - 2) retained -= 2;
            const bool overlong = retained > NativeLimit;
            EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}),
                std::string(overlong ? "32,SYNTAX ERROR,00,00" : "00, OK,00,00")
                    + (d80 ? ",0\r" : "\r"));
            controller.command(Talk(Device{DeviceAddress}));
            controller.command(Secondary(Channel{DataChannel}));
            service_rx(system, firmware, "service length-control position");
            receive_payload(system, controller, overlong
                ? std::vector<Record>{data(1), data(2, Eoi::Last)}
                : std::vector<Record>{data(3), data(4, Eoi::Last)});
            controller.command(Untalk);
            position_relative(system, firmware, controller, 3, 1);
            controller.command(Talk(Device{DeviceAddress}));
            controller.command(Secondary(Channel{DataChannel}));
            service_rx(system, firmware, "service recovery after length check");
            receive_payload(system, controller, {data(5), data(6, Eoi::Last)});
            controller.command(Untalk);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(ModelsLengths, DriveCommandLengthTest,
    testing::Combine(testing::Bool(), testing::Values<size_t>(40, 41, 42, 57, 58, 59, 60)));

// Status is acknowledged on terminal-CR GET, never on queueing or TXS flush.
TEST_P(DriveRelativeTest, PartialStatusSurvivesReselectionAndPeerStatus) {
    const bool d80 = GetParam();
    const std::vector<uint8_t> payload{1, 2};
    firmware.mount(relative_image(d80, payload, 2), d80 ? "status-tail.d80" : "status-tail.d64");
    IeeeController controller(system);
    open_file(system, controller, Channel{DataChannel}, "BASIC,L");
    service_rx(system, firmware, "service status-progress OPEN");
    const std::string error = std::string("70,NO CHANNEL,00,00") + (d80 ? ",0\r" : "\r");
    const std::string ok = std::string("00, OK,00,00") + (d80 ? ",0\r" : "\r");
    for (const size_t prefix : {size_t{0}, size_t{3}, error.size() - 1}) {
        controller.command(Listen(Device{DeviceAddress}));
        controller.command(Secondary(Channel{CommandChannel}));
        for (const auto byte : {uint8_t{'P'}, uint8_t{IEEE_CMD_SECONDARY(DataChannel + 1)},
                uint8_t{1}, uint8_t{0}})
            controller.data(byte);
        controller.data(1, Eoi::Last);
        controller.command(Unlisten);
        service_rx(system, firmware, "service status-progress error");
        controller.command(Talk(Device{DeviceAddress}));
        controller.command(Secondary(Channel{CommandChannel}));
        service_rx(system, firmware, "queue unread error status");
        for (size_t index = 0; index < prefix; ++index)
            ASSERT_EQ(controller.receive(), data(static_cast<uint8_t>(error[index])));
        controller.command(Untalk);
        service_rx(system, firmware, "retain partially fetched error status");
        const Device peer{DeviceAddress + 1};
        const std::string peer_status = receive_status(system, firmware, controller, peer);
        EXPECT_EQ(peer_status, prefix == 0 ? "73,CBM DOS V2,00,00\r" : "00, OK,00,00\r");
        EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}),
            error.substr(prefix));
        EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}), ok);
    }

    // Native GETERC acknowledges CR on prefetch, before the controller's NDAC.
    controller.command(Listen(Device{DeviceAddress}));
    controller.command(Secondary(Channel{CommandChannel}));
    for (const auto byte : {uint8_t{'P'}, uint8_t{IEEE_CMD_SECONDARY(DataChannel + 1)},
            uint8_t{1}, uint8_t{0}})
        controller.data(byte);
    controller.data(1, Eoi::Last);
    controller.command(Unlisten);
    service_rx(system, firmware, "service terminal-GET error");
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Channel{CommandChannel}));
    service_rx(system, firmware, "queue terminal-GET status");
    for (size_t index = 0; index + 1 < error.size(); ++index)
        ASSERT_EQ(controller.receive(), data(static_cast<uint8_t>(error[index])));
    controller.write(pet::ViaPortB, BusReleased);
    system.run_until([](const System& board) { return !board.snapshot().ieee_bus_dav_no; },
        ByteBudget, "status CR before NDAC");
    controller.write(pet::ViaPortB, static_cast<uint8_t>(BusReleased & ~pet::Nrfd));
    ASSERT_EQ(static_cast<uint8_t>(~controller.read(pet::Pia2PortA)), '\r');
    ASSERT_EQ(controller.read(pet::Pia1PortA) & EoiInput, 0);
    controller.command(Untalk);
    service_rx(system, firmware, "acknowledge CR GET without NDAC");
    EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}), ok);
}

// CMDSET parsing-size adjustment must not change RECORD's raw binary operands.
TEST_P(DriveRelativeTest, BinaryPositionRetainsCrValuedFields) {
    const bool d80 = GetParam();
    constexpr uint8_t RecordLength = 13;
    std::vector<uint8_t> payload(3 * RecordLength);
    for (size_t index = 0; index < payload.size(); ++index)
        payload[index] = static_cast<uint8_t>(index + 1);
    IeeeController controller(system);
    firmware.mount(relative_image(d80, payload, RecordLength), d80 ? "binary-cr.d80" : "binary-cr.d64");
    open_file(system, controller, Channel{DataChannel}, "BASIC,L");
    service_rx(system, firmware, "open CR-valued binary operand fixture");
    for (const auto entry : {Open(Channel{CommandChannel}), Secondary(Channel{CommandChannel})}) {
        for (const uint8_t high : {uint8_t{0}, uint8_t{'\r'}}) {
            controller.command(Listen(Device{DeviceAddress}));
            controller.command(entry);
            for (const auto byte : {uint8_t{'P'}, uint8_t{IEEE_CMD_SECONDARY(DataChannel)},
                    uint8_t{2}, high})
                controller.data(byte);
            controller.data('\r', Eoi::Last);
            controller.command(Unlisten);
            service_rx(system, firmware, "dispatch CR-valued binary fields");
            const std::string expected = std::string(high == 0
                ? "00, OK,00,00" : "50,RECORD NOT PRESENT,00,00") + (d80 ? ",0\r" : "\r");
            EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}), expected);
            if (high == 0) {
                controller.command(Talk(Device{DeviceAddress}));
                controller.command(Secondary(Channel{DataChannel}));
                service_rx(system, firmware, "serve position thirteen");
                receive_payload(system, controller, {data(payload[2 * RecordLength - 1], Eoi::Last)});
                controller.command(Untalk);
                service_rx(system, firmware, "park position thirteen");
            }
        }
    }
}

// A response retains its format until CR, even when the mounted model changes.
TEST_P(DriveRelativeTest, PartialStatusRetainsFormatAcrossModelChange) {
    const bool d80 = GetParam();
    const std::vector<uint8_t> payload{1, 2};
    IeeeController controller(system);
    firmware.mount(relative_image(d80, payload, 2), d80 ? "before.d80" : "before.d64");
    position_relative(system, firmware, controller, 1, 1);
    const std::string error = d80 ? "70,NO CHANNEL,00,00,0\r" : "70,NO CHANNEL,00,00\r";
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Channel{CommandChannel}));
    service_rx(system, firmware, "queue status before model replacement");
    for (size_t index = 0; index + 1 < error.size(); ++index)
        ASSERT_EQ(controller.receive(), data(static_cast<uint8_t>(error[index])));
    firmware.mount(relative_image(!d80, payload, 2), d80 ? "after.d64" : "after.d80");
    EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}), "\r");
    EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}),
        d80 ? "00, OK,00,00\r" : "00, OK,00,00,0\r");
}

// A retained peer LISTEN accepts the selected source without a global UNLISTEN.
TEST_P(DriveRelativeTest, RetainedPeerListenerReceivesSelectedTalker) {
    const bool d80 = GetParam();
    const std::vector<uint8_t> source{0xa5, 0xa6, 0x51, 0x52};
    const std::vector<uint8_t> destination{0x11, 0x12, 0x13, 0x14};
    const char* const peer_name = d80 ? "peer-write.d80" : "peer-write.d64";
    firmware.mount(relative_image(d80, destination, destination.size()), peer_name);
    ASSERT_TRUE(ieee_drive_mount(2, peer_name));
    firmware.mount(relative_image(d80, source, source.size()),
        d80 ? "peer-source.d80" : "peer-source.d64");
    IeeeController controller(system);
    const Device talker{DeviceAddress}, peer{DeviceAddress + 1}, foreign{DeviceAddress - 1};
    open_file(system, controller, Channel{DataChannel}, "BASIC,L", peer);
    service_rx(system, firmware, "open peer REL writer");
    open_file(system, controller, Channel{DataChannel}, "BASIC,L", talker);
    service_rx(system, firmware, "open peer REL source");
    controller.command(Listen(peer));
    controller.command(Secondary(Channel{DataChannel}));
    controller.command(Listen(foreign));
    controller.command(Talk(talker));
    controller.command(Secondary(Channel{DataChannel}));
    controller.release_atn();
    service_rx(system, firmware, "select concurrent virtual source and acceptor");
    receive_payload(system, controller,
        {data(source[0]), data(source[1]), data(source[2]), data(source[3], Eoi::Last)});
    controller.command(Unlisten);
    controller.command(Untalk);
    service_rx(system, firmware, "commit peer's received REL record");
    position_relative(system, firmware, controller, 1, 1, Channel{DataChannel}, peer);
    controller.command(Talk(peer));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "read back peer's written REL record");
    receive_payload(system, controller,
        {data(source[0]), data(source[1]), data(source[2]), data(source[3], Eoi::Last)});
}

// A peer-only transfer must leave DAV idle when delayed service fills RX.
TEST_P(DriveRelativeTest, PeerTransferBackpressuresBeforeRxCapacity) {
    const bool d80 = GetParam();
    constexpr size_t RecordLength = 40, RxCapacity = 32, ListenerCheckpoints = 1;
    constexpr size_t PayloadCapacity = RxCapacity - ListenerCheckpoints;
    constexpr econopet::Cycles DelayedService = 20000;
    std::vector<uint8_t> source(RecordLength);
    std::vector<Record> expected;
    for (size_t index = 0; index < source.size(); ++index) {
        source[index] = static_cast<uint8_t>(index + 1);
        expected.push_back(data(source[index], index + 1 == source.size() ? Eoi::Last : Eoi::More));
    }
    const char* const peer_name = d80 ? "backpressure-write.d80" : "backpressure-write.d64";
    firmware.mount(relative_image(d80, std::vector<uint8_t>(RecordLength, 0x55), RecordLength), peer_name);
    ASSERT_TRUE(ieee_drive_mount(2, peer_name));
    firmware.mount(relative_image(d80, source, RecordLength),
        d80 ? "backpressure-source.d80" : "backpressure-source.d64");
    IeeeController controller(system);
    const Device talker{DeviceAddress}, peer{DeviceAddress + 1}, foreign{DeviceAddress - 1};
    open_file(system, controller, Channel{DataChannel}, "BASIC,L", peer);
    service_rx(system, firmware, "open backpressured peer REL writer");
    open_file(system, controller, Channel{DataChannel}, "BASIC,L", talker);
    service_rx(system, firmware, "open backpressured REL source");
    controller.command(Listen(peer));
    controller.command(Secondary(Channel{DataChannel}));
    controller.command(Listen(foreign));
    controller.command(Talk(talker));
    controller.command(Secondary(Channel{DataChannel}));
    controller.release_atn();
    service_rx(system, firmware, "select peer-only transfer while controller is busy");
    auto wire = system.trace_ieee();
    controller.write(pet::Pia2ControlA, PiaC2High.bits());
    controller.write(pet::ViaPortB, BusReleased);
    system.tick(DelayedService);
    ASSERT_EQ(wire.records().size(), PayloadCapacity);
    ASSERT_TRUE(system.snapshot().ieee_rx_ready_o);
    ASSERT_TRUE(system.snapshot().ieee_device_dav_no);
    require_sequence(wire.records(), std::vector<Record>(expected.begin(), expected.begin() + PayloadCapacity),
        "peer transfer paused before RX overflow");
    system.execution().within(ByteBudget).service(firmware.service_adapter())
        .until([&](const System& board) {
            return wire.records().size() == RecordLength && !board.snapshot().ieee_rx_ready_o
                && board.snapshot().ieee_device_dav_no;
        }).run("resume peer transfer after delayed firmware service");
    require_sequence(wire.records(), expected, "complete peer-only wire transfer");
    controller.command(Unlisten);
    controller.command(Untalk);
    service_rx(system, firmware, "commit backpressured peer REL write");
    position_relative(system, firmware, controller, 1, 1, Channel{DataChannel}, peer);
    controller.command(Talk(peer));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "read back full backpressured peer record");
    receive_payload(system, controller, expected);
}

// Historical status selections cannot publish bytes under the final unit's tracker.
TEST_P(DriveRelativeTest, QueuedStatusSelectionsRetainResponseOwnership) {
    const bool d80 = GetParam();
    const std::vector<uint8_t> payload{1, 2};
    IeeeController controller(system);
    firmware.mount(relative_image(d80, payload, 2), d80 ? "queued-status.d80" : "queued-status.d64");
    const Device original{DeviceAddress}, peer{DeviceAddress + 1};
    const std::string original_error = d80 ? "70,NO CHANNEL,00,00,0\r" : "70,NO CHANNEL,00,00\r";
    const std::string peer_error = "62, FILE NOT FOUND,00,00\r";
    for (const size_t prefix : {size_t{0}, size_t{3}}) {
        position_relative(system, firmware, controller, 1, 1);
        open_file(system, controller, Channel{DataChannel}, "MISSING", peer);
        service_rx(system, firmware, "prepare peer error before queued status");
        if (prefix != 0) {
            controller.command(Talk(original));
            controller.command(Secondary(Channel{CommandChannel}));
            service_rx(system, firmware, "queue initial partial status");
            for (size_t index = 0; index < prefix; ++index)
                ASSERT_EQ(controller.receive(), data(static_cast<uint8_t>(original_error[index])));
            controller.command(Untalk);
            service_rx(system, firmware, "park partial original status");
        }
        controller.command(Talk(original));
        controller.command(Secondary(Channel{CommandChannel}));
        controller.command(Untalk);
        controller.command(Talk(peer));
        controller.command(Secondary(Channel{CommandChannel}));
        if (prefix != 0) {
            controller.command(Untalk);
            controller.command(Talk(peer));
            controller.command(Secondary(Channel{CommandChannel}));
            controller.write(pet::Pia2ControlA, PiaC2Low.bits());
            controller.write(pet::ViaPortB, BusReleased);
        }
        service_rx(system, firmware, "service queued cross-unit status selections");
        for (size_t index = 0; index < peer_error.size(); ++index)
            ASSERT_EQ(controller.receive(), data(static_cast<uint8_t>(peer_error[index]),
                index + 1 == peer_error.size() ? Eoi::Last : Eoi::More));
        controller.command(Untalk);
        service_rx(system, firmware, "acknowledge only peer status");
        EXPECT_EQ(receive_status(system, firmware, controller, original), original_error.substr(prefix));
        EXPECT_EQ(receive_status(system, firmware, controller, peer), "00, OK,00,00\r");
    }
}

INSTANTIATE_TEST_SUITE_P(Models, DriveRelativeTest, testing::Bool());

// Native TLK20/TLK30 advancement survives interruption and reuse of the shared FIFO.
TEST_P(DriveRelativeContinuationTest, ResumesAtNativePrefetchBoundary) {
    const auto [d80, phase, owner] = GetParam();
    constexpr uint8_t RecordLength = 129;
    constexpr int BeforeDav = 0, BeforeNrfd = 1, AfterNrfd = 2, PartialRecord = 3;
    constexpr size_t ConsumedPrefix = 5;
    std::vector<uint8_t> payload(3 * RecordLength);
    for (size_t i = phase == PartialRecord ? 0 : RecordLength; i < payload.size(); ++i)
        payload[i] = static_cast<uint8_t>(i | 1);
    const auto image = relative_image(d80, payload, RecordLength);
    const char* const name = d80 ? "continuation.d80" : "continuation.d64";
    IeeeController controller(system);
    firmware.mount(image, name);
    const Device peer{static_cast<uint8_t>(owner == 2 ? DeviceAddress + 1 : DeviceAddress)};
    const Channel peer_channel{static_cast<uint8_t>(owner == 1 ? DataChannel + 1 : DataChannel)};
    if (owner == 2) ASSERT_TRUE(ieee_drive_mount(2, name));
    open_file(system, controller, Channel{DataChannel}, "BASIC,L");
    service_rx(system, firmware, "service interrupted REL OPEN");
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "service interrupted REL TALK");

    // Hold native advancement independently of a private PET data-register read.
    if (phase == PartialRecord) {
        for (size_t i = 0; i < ConsumedPrefix; ++i)
            ASSERT_EQ(controller.receive(), data(payload[i]));
    } else {
        controller.write(pet::Pia2ControlA, PiaC2Low.bits());
        controller.write(pet::ViaPortB, static_cast<uint8_t>(BusReleased & ~pet::Nrfd));
        if (phase != BeforeDav) {
            controller.write(pet::ViaPortB, BusReleased);
            system.run_until([](const System& board) { return !board.snapshot().ieee_bus_dav_no; },
                ByteBudget, "empty REL DAV before interruption");
            if (phase == BeforeNrfd)
                ASSERT_EQ(static_cast<uint8_t>(~controller.read(pet::Pia2PortA)), 0);
            if (phase == AfterNrfd)
                controller.write(pet::ViaPortB, static_cast<uint8_t>(BusReleased & ~pet::Nrfd));
        }
    }
    const uint8_t lines = phase == BeforeNrfd ? BusReleased
        : static_cast<uint8_t>(BusReleased & ~pet::Nrfd);
    controller.write(pet::ViaPortB, static_cast<uint8_t>(lines & ~pet::Atn));
    controller.command(Untalk);
    service_rx(system, firmware, "service interrupted REL UNTALK");

    // A status switch preserves FIFO contents, while OPEN forces a hardware discard.
    if (owner == 0) {
        EXPECT_EQ(receive_status(system, firmware, controller, Device{DeviceAddress}),
            d80 ? "00, OK,00,00,0\r" : "00, OK,00,00\r");
    } else {
        open_file(system, controller, peer_channel, "BASIC,L", peer);
        service_rx(system, firmware, "service peer REL OPEN");
        controller.command(Talk(peer));
        controller.command(Secondary(peer_channel));
        controller.write(pet::Pia2ControlA, PiaC2Low.bits());
        controller.write(pet::ViaPortB, BusReleased);
        system.tick(1000);
        EXPECT_TRUE(system.snapshot().ieee_bus_dav_no);
        service_rx(system, firmware, "service peer REL TALK");
        EXPECT_EQ(controller.receive(), data(payload[0],
            phase == PartialRecord ? Eoi::More : Eoi::Last));
        if ((phase == PartialRecord || phase == AfterNrfd) && owner == 1) {
            // A write through the other channel changes the shared backing file,
            // not the original channel's already-prefetched pending response.
            controller.command(Untalk);
            controller.command(Listen(peer));
            controller.command(Secondary(peer_channel));
            controller.data('Z', Eoi::Last);
            controller.command(Unlisten);
            service_rx(system, firmware, "service peer write while REL response parked");
        }
    }

    // Returning without firmware service must not expose the peer's unread bytes.
    controller.command(Untalk);
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Channel{DataChannel}));
    auto resumed_wire = system.trace_ieee();
    if (owner != 0) {
        controller.write(pet::Pia2ControlA, PiaC2Low.bits());
        controller.write(pet::ViaPortB, BusReleased);
        system.tick(1000);
        EXPECT_TRUE(system.snapshot().ieee_bus_dav_no);
    }
    service_rx(system, firmware, "service resumed REL TALK");
    std::vector<Record> expected;
    if (phase == BeforeDav || phase == BeforeNrfd) {
        expected.push_back(data(0, Eoi::Last));
    } else {
        const size_t first = phase == PartialRecord ? ConsumedPrefix : RecordLength;
        const size_t end = phase == PartialRecord ? RecordLength : 2 * RecordLength;
        for (size_t i = first; i < end; ++i)
            expected.push_back(data(payload[i], i + 1 == end ? Eoi::Last : Eoi::More));
    }
    std::vector<Record> received;
    for (size_t i = 0; i < expected.size(); ++i) received.push_back(controller.receive());
    require_sequence(received, expected, "resumed REL CPU payload");
    require_sequence(resumed_wire.records(), expected, "resumed REL wire payload");
}

INSTANTIATE_TEST_SUITE_P(ModelsPhasesOwners, DriveRelativeContinuationTest,
    testing::Combine(testing::Bool(), testing::Values(0, 1, 2, 3), testing::Values(0, 1, 2)));

// A stale B selection must not withdraw A's byte before the PET samples it.
TEST_P(DriveRelativeParkingTest, KeepsPresentationThroughNdacWhileDeferringReuse) {
    const auto [d80, prefetched, final_byte] = GetParam();
    constexpr uint8_t RecordLength = 3;
    constexpr Channel Original{DataChannel}, Peer{DataChannel + 1};
    const std::vector<uint8_t> payload{0xa5, 0xa6, 0xa7, 0x51, 0x52, 0x53};
    IeeeController controller(system);
    firmware.mount(relative_image(d80, payload, RecordLength),
        d80 ? "parking.d80" : "parking.d64");
    open_file(system, controller, Original, "BASIC,L");
    service_rx(system, firmware, "service parking original OPEN");
    open_file(system, controller, Peer, "BASIC,L");
    service_rx(system, firmware, "service parking peer OPEN");
    position_relative(system, firmware, controller, 2, 1, Peer);
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Original));
    service_rx(system, firmware, "service parking initial TALK");
    const size_t selected = final_byte ? RecordLength - 1 : 0;
    for (size_t i = 0; i < selected; ++i)
        ASSERT_EQ(controller.receive(), data(payload[i]));

    // Hardware resumes A while B/A selections remain queued for the MCU.
    controller.command(Untalk);
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Peer));
    controller.command(Untalk);
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Original));
    controller.write(pet::Pia2ControlA, PiaC2Low.bits());
    controller.write(pet::ViaPortB, BusReleased);
    system.run_until([](const System& board) { return !board.snapshot().ieee_bus_dav_no; },
        ByteBudget, "parking original byte DAV");
    if (prefetched)
        controller.write(pet::ViaPortB, static_cast<uint8_t>(BusReleased & ~pet::Nrfd));

    // A cooperative pass must return without draining commands or dropping DAV.
    firmware.service();
    EXPECT_TRUE(system.snapshot().ieee_rx_ready_o);
    EXPECT_FALSE(system.snapshot().ieee_bus_dav_no);
    if (!prefetched) {
        controller.write(pet::ViaPortB, static_cast<uint8_t>(BusReleased & ~pet::Nrfd));
        firmware.service();
        EXPECT_TRUE(system.snapshot().ieee_rx_ready_o);
        EXPECT_FALSE(system.snapshot().ieee_bus_dav_no);
    }

    // KERNAL ACPTR samples EOI/DIO only after asserting NRFD.
    EXPECT_EQ((controller.read(pet::Pia1PortA) & EoiInput) == 0, final_byte);
    EXPECT_EQ(static_cast<uint8_t>(~controller.read(pet::Pia2PortA)), payload[selected]);
    controller.write(pet::Pia2ControlA, PiaC2High.bits());
    system.run_until([](const System& board) { return board.snapshot().ieee_bus_dav_no; },
        ByteBudget, "parking original byte NDAC acknowledgement");
    controller.write(pet::Pia2ControlA, PiaC2Low.bits());
    service_rx(system, firmware, "service deferred B/A selections");

    // Exactly one GET advances A, including the EOI-to-next-record boundary.
    const size_t end = final_byte ? payload.size() : RecordLength;
    std::vector<Record> expected;
    for (size_t i = selected + 1; i < end; ++i)
        expected.push_back(data(payload[i], i + 1 == end ? Eoi::Last : Eoi::More));
    receive_payload(system, controller, expected);
}

INSTANTIATE_TEST_SUITE_P(ModelsPrefetchFinal, DriveRelativeParkingTest,
    testing::Combine(testing::Bool(), testing::Bool(), testing::Bool()));

// UNLISTEN and foreign LISTEN must resume queued data without TALK/secondary.
TEST_P(DrivePreservedTalkTest, ContinuesWithoutReaddressingAfterUnlisten) {
    const auto [d80, relative, foreign_listen] = GetParam();
    constexpr uint8_t RecordLength = 4;
    constexpr Device ForeignDevice{DeviceAddress - 1};
    const std::vector<uint8_t> payload{0xa5, 0xa6, 0x51, 0x52};
    IeeeController controller(system);
    firmware.mount(relative
            ? relative_image(d80, payload, RecordLength)
            : sequential_image(d80, payload),
        d80 ? "preserved-talker.d80" : "preserved-talker.d64");
    open_file(system, controller, Channel{DataChannel}, relative ? "BASIC,L" : "BASIC");
    service_rx(system, firmware, "service preserved-talker OPEN");
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "service preserved-talker TALK");
    ASSERT_EQ(controller.receive(), data(payload.front()));

    // Repeated interruptions retain the channel, exact unread tail, and final EOI.
    for (size_t i = 1; i < payload.size(); ++i) {
        if (foreign_listen) controller.command(Listen(ForeignDevice));
        controller.command(Unlisten);
        service_rx(system, firmware, "service talk-preserving commands");
        firmware.service();
        receive_payload(system, controller,
            {data(payload[i], i + 1 == payload.size() ? Eoi::Last : Eoi::More)});
    }
    EXPECT_TRUE(system.ieee().read_status().contains(fpga::IeeeStatusBit::TxEmpty));
}

// Another emulated peripheral must not steal the selected talker's role or SA.
TEST_P(DrivePreservedTalkTest, VirtualPeerListenPreservesTalkerAndChannel) {
    const auto [d80, relative, status_secondary] = GetParam();
    const std::vector<uint8_t> payload{0xa5, 0xa6, 0x51, 0x52};
    const char* const name = d80 ? "virtual-peer.d80" : "virtual-peer.d64";
    firmware.mount(relative ? relative_image(d80, payload, payload.size())
                            : sequential_image(d80, payload), name);
    constexpr uint8_t UnitCount = 4, DrivesPerUnit = 2;
    for (uint8_t unit = 1; unit < UnitCount; ++unit)
        ASSERT_TRUE(ieee_drive_mount(unit * DrivesPerUnit, name));
    IeeeController controller(system);
    for (uint8_t unit = 0; unit < UnitCount; ++unit) {
        const Device talker{static_cast<uint8_t>(DeviceAddress + unit)};
        for (uint8_t other = 0; other < UnitCount; ++other) {
            if (other == unit) continue;
            const Device peer{static_cast<uint8_t>(DeviceAddress + other)};
            open_file(system, controller, Channel{DataChannel}, relative ? "BASIC,L" : "BASIC", talker);
            service_rx(system, firmware, "service virtual-peer talker OPEN");
            controller.command(Talk(talker));
            controller.command(Secondary(Channel{DataChannel}));
            service_rx(system, firmware, "service virtual-peer talker selection");
            ASSERT_EQ(controller.receive(), data(payload.front()));
            controller.command(Listen(peer));
            controller.command(Secondary(Channel{status_secondary ? CommandChannel : DataChannel + 1}));
            controller.command(Unlisten);
            service_rx(system, firmware, "service virtual-peer listener and secondary");
            receive_payload(system, controller,
                {data(payload[1]), data(payload[2]), data(payload[3], Eoi::Last)});
            controller.command(Untalk);
            service_rx(system, firmware, "service virtual-peer UNTALK");
        }
    }
}

INSTANTIATE_TEST_SUITE_P(ModelsFormatsCommands, DrivePreservedTalkTest,
    testing::Combine(testing::Bool(), testing::Bool(), testing::Bool()));

// Execute native PET/Waterloo input for consecutive empty records, then nonempty data.
TEST_P(DriveRelativeRomTest, NativeReceivePreservesEmptyRecordsAndEoi) {
    const auto [d80, revision] = GetParam();
    constexpr uint8_t RecordLength = 129, Marker = 0xa5;
    std::vector<uint8_t> payload(3 * RecordLength);
    payload[2 * RecordLength] = Marker;
    IeeeController controller(system);
    firmware.mount(relative_image(d80, payload, RecordLength), d80 ? "native-rel.d80" : "native-rel.d64");
    open_file(system, controller, Channel{DataChannel}, "BASIC,L");
    service_rx(system, firmware, "service native empty REL OPEN");
    controller.write(pet::ViaPortB, BusReleased);
    system.cpu().assert_reset();
    auto wire = system.trace_ieee();
    if (revision != test_rom::Set::Waterloo) {
        KernalFixture rom(system, revision);
        auto test = rom.program();
        const auto received = test.data("REL payload", NativePayload, 3, test_asm::Capture::Yes);
        const auto flags = test.data("REL status", NativeLastStatus, 3, test_asm::Capture::Yes);
        const auto result = test.within(TerminalRomBudget)
            .every(RomServicePeriod, ServiceTiming::AfterSlice, firmware.service_adapter())
            .run([&](auto& p) {
                rom.init_ieee(p);
                rom.device(p);
                for (size_t record = 0; record < 3; ++record) {
                    p.lda(test_asm::imm(0)).sta(test_asm::zp(KernalWorkspace::Status))
                     .lda(test_asm::imm(DeviceAddress)).jsr(rom.entry(KernalRoutine::Talk))
                     .lda(test_asm::imm(IEEE_CMD_SECONDARY(DataChannel))).jsr(rom.entry(KernalRoutine::Tksa))
                     .jsr(rom.entry(KernalRoutine::Acptr)).sta(received.byte(record))
                     .lda(test_asm::zp(KernalWorkspace::Status)).sta(flags.byte(record))
                     .jsr(rom.entry(KernalRoutine::Untalk));
                }
            });
        EXPECT_EQ(result[received], (std::vector<uint8_t>{0, 0, Marker}));
        EXPECT_EQ(result[flags], (std::vector<uint8_t>{0x40, 0x40, 0x40}));
    } else {
        WaterlooFixture rom(system);
        auto test = rom.program();
        const auto received = test.data("REL payload", NativePayload, 3, test_asm::Capture::Yes);
        const auto flags = test.data("REL flags", NativeLastStatus, 3, test_asm::Capture::Yes);
        const auto result = test.within(TerminalRomBudget)
            .every(RomServicePeriod, ServiceTiming::AfterSlice, firmware.service_adapter())
            .run([&](auto& p) {
                rom.init_ieee(p);
                rom.device(p, DeviceAddress, DataChannel);
                p.lda(test_asm::imm(0)).sta(WaterlooActiveRole).sta(WaterlooGlobalStatus)
                 .lda(test_asm::imm(1)).sta(WaterlooWorkspace::WaitParameterLow);
                for (size_t record = 0; record < 3; ++record) {
                    p.lda(test_asm::imm(0)).sta(WaterlooWorkspace::DiskFlags)
                     .ldd(test_asm::imm16(WaterlooWorkspace::Block)).jsr(rom.entry(WaterlooRoutine::Talk))
                     .jsr(rom.entry(WaterlooRoutine::Input)).stb(received.byte(record).address())
                     .lda(WaterlooWorkspace::DiskFlags).sta(flags.byte(record))
                     .jsr(rom.entry(WaterlooRoutine::Untalk));
                }
            });
        EXPECT_EQ(result[received], (std::vector<uint8_t>{0, 0, Marker}));
        for (size_t record = 0; record < 3; ++record)
            EXPECT_EQ(result[flags.byte(record)] & 1, 1);
    }
    std::vector<Record> expected;
    for (const uint8_t value : {uint8_t{0}, uint8_t{0}, Marker}) {
        expected.push_back(command(Talk(Device{DeviceAddress})));
        expected.push_back(command(Secondary(Channel{DataChannel})));
        expected.push_back(data(value, Eoi::Last));
        expected.push_back(command(Untalk));
    }
    require_sequence(wire.records(), expected, "native empty REL wire bytes");
}

// Both selected drive comparators resolve B?S?C to BASIC before ordinary reads.
TEST_P(DriveWildcardRomTest, NativeOpenAndReadResolveSingleCharacterWildcards) {
    const auto [d80, revision] = GetParam();
    const std::vector<uint8_t> payload{0xa5, 0};
    constexpr uint8_t Filename[] = {'B', '?', 'S', '?', 'C'};
    firmware.mount(sequential_image(d80, payload), d80 ? "wildcard.d80" : "wildcard.d64");
    auto wire = system.trace_ieee();

    if (revision != test_rom::Set::Waterloo) {
        KernalFixture rom(system, revision);
        auto test = rom.program();
        const auto received = test.data("wildcard payload", NativePayload, payload.size(), test_asm::Capture::Yes);
        const auto flags = test.data("wildcard status", NativeLastStatus, payload.size(), test_asm::Capture::Yes);
        const auto result = test.within(TerminalRomBudget)
            .every(RomServicePeriod, ServiceTiming::AfterSlice, firmware.service_adapter())
            .run([&](auto& p) {
                rom.init_ieee(p);
                rom.device(p);
                rom.file(p, Filename, DataChannel);
                p.jsr(rom.entry(KernalRoutine::Openi))
                 .lda(test_asm::imm(DeviceAddress)).jsr(rom.entry(KernalRoutine::Talk))
                 .lda(test_asm::imm(IEEE_CMD_SECONDARY(DataChannel))).jsr(rom.entry(KernalRoutine::Tksa));
                for (size_t i = 0; i < payload.size(); ++i)
                    p.jsr(rom.entry(KernalRoutine::Acptr)).sta(received.byte(i))
                     .lda(test_asm::zp(KernalWorkspace::Status)).sta(flags.byte(i));
                p.jsr(rom.entry(KernalRoutine::Untalk));
            });
        EXPECT_EQ(result[received], payload);
        EXPECT_EQ(result[flags], (std::vector<uint8_t>{0, 0x40}));
    } else {
        WaterlooFixture rom(system);
        auto test = rom.program();
        const auto received = test.data("wildcard payload", NativePayload, payload.size(), test_asm::Capture::Yes);
        const auto flags = test.data("wildcard flags", NativeLastStatus, payload.size(), test_asm::Capture::Yes);
        const auto result = test.within(TerminalRomBudget)
            .every(RomServicePeriod, ServiceTiming::AfterSlice, firmware.service_adapter())
            .run([&](auto& p) {
                rom.init_ieee(p);
                rom.device(p, DeviceAddress, DataChannel);
                const uint8_t terminated[] = {'B', '?', 'S', '?', 'C', 0};
                rom.file(p, terminated);
                p.lda(test_asm::imm(0)).sta(WaterlooActiveRole).sta(WaterlooGlobalStatus)
                 .lda(test_asm::imm(1)).sta(WaterlooWorkspace::WaitParameterLow)
                 .ldd(test_asm::imm16(WaterlooWorkspace::Block)).jsr(rom.entry(WaterlooRoutine::Open))
                 .ldd(test_asm::imm16(WaterlooWorkspace::Block)).jsr(rom.entry(WaterlooRoutine::Talk));
                for (size_t i = 0; i < payload.size(); ++i)
                    p.jsr(rom.entry(WaterlooRoutine::Input)).stb(received.byte(i).address())
                     .lda(WaterlooWorkspace::DiskFlags).sta(flags.byte(i));
                p.jsr(rom.entry(WaterlooRoutine::Untalk));
            });
        EXPECT_EQ(result[received], payload);
        for (size_t i = 0; i < payload.size(); ++i)
            EXPECT_EQ(result[flags.byte(i)] & 1, i + 1 == payload.size() ? 1 : 0);
        EXPECT_EQ(system.peek(econopet::SramAddress{WaterlooWorkspace::Block.value()}), 0);
    }

    // Observe literal pattern transmission and exact data/EOI independently of ROM results.
    std::vector<Record> expected{command(Listen(Device{DeviceAddress})), command(Open(Channel{DataChannel}))};
    for (size_t i = 0; i < sizeof(Filename); ++i)
        expected.push_back(data(Filename[i], i + 1 == sizeof(Filename) ? Eoi::Last : Eoi::More));
    expected.push_back(command(Unlisten));
    expected.push_back(command(Talk(Device{DeviceAddress})));
    expected.push_back(command(Secondary(Channel{DataChannel})));
    for (size_t i = 0; i < payload.size(); ++i)
        expected.push_back(data(payload[i], i + 1 == payload.size() ? Eoi::Last : Eoi::More));
    expected.push_back(command(Untalk));
    require_sequence(wire.records(), expected, "native wildcard OPEN/read wire bytes");
}

// A real PET KERNAL OPENI must transmit the embedded-zero P command intact.
TEST_P(DriveCommandRomTest, NativeOpen15ExecutesBinaryPosition) {
    const auto [d80, revision] = GetParam();
    const std::vector<uint8_t> payload{1, 2, 3, 4, 5, 6};
    constexpr uint8_t Filename[] = {'B', 'A', 'S', 'I', 'C', ',', 'L'};
    constexpr uint8_t Position[] = {'P', 0x62, 3, 0, 1};
    firmware.mount(relative_image(d80, payload, 2), d80 ? "rom-command.d80" : "rom-command.d64");
    KernalFixture rom(system, revision);
    auto test = rom.program();
    const auto received = test.data("positioned payload", NativePayload, 2, test_asm::Capture::Yes);
    const auto flags = test.data("positioned flags", NativeLastStatus, 2, test_asm::Capture::Yes);
    auto wire = system.trace_ieee();
    const auto result = test.within(TerminalRomBudget)
        .every(RomServicePeriod, ServiceTiming::AfterSlice, firmware.service_adapter())
        .run([&](auto& p) {
            rom.init_ieee(p);
            rom.device(p);
            rom.file(p, Filename, DataChannel);
            p.jsr(rom.entry(KernalRoutine::Openi));

            rom.file(p, Position, CommandChannel);
            p.jsr(rom.entry(KernalRoutine::Openi))
             .lda(test_asm::imm(DeviceAddress)).jsr(rom.entry(KernalRoutine::Talk))
             .lda(test_asm::imm(IEEE_CMD_SECONDARY(DataChannel))).jsr(rom.entry(KernalRoutine::Tksa));
            for (size_t i = 0; i < 2; ++i)
                p.jsr(rom.entry(KernalRoutine::Acptr)).sta(received.byte(i))
                 .lda(test_asm::zp(KernalWorkspace::Status)).sta(flags.byte(i));
            p.jsr(rom.entry(KernalRoutine::Untalk));
        });
    EXPECT_EQ(result[received], (std::vector<uint8_t>{5, 6}));
    EXPECT_EQ(result[flags], (std::vector<uint8_t>{0, 0x40}));
    std::vector<Record> expected;
    const auto append_open = [&](Channel channel, econopet::ByteView bytes) {
        expected.push_back(command(Listen(Device{DeviceAddress})));
        expected.push_back(command(Open(channel)));
        for (size_t i = 0; i < bytes.size(); ++i)
            expected.push_back(data(bytes[i], i + 1 == bytes.size() ? Eoi::Last : Eoi::More));
        expected.push_back(command(Unlisten));
    };
    append_open(Channel{DataChannel}, Filename);
    append_open(Channel{CommandChannel}, Position);
    expected.push_back(command(Talk(Device{DeviceAddress})));
    expected.push_back(command(Secondary(Channel{DataChannel})));
    expected.push_back(data(5));
    expected.push_back(data(6, Eoi::Last));
    expected.push_back(command(Untalk));
    require_sequence(wire.records(), expected, "PET ROM OPEN-15 binary positioning");
}

INSTANTIATE_TEST_SUITE_P(ModelsRevisions, DriveCommandRomTest,
    testing::Combine(testing::Bool(), testing::Values(test_rom::Set::Upgrade, test_rom::Set::Rom4)));

INSTANTIATE_TEST_SUITE_P(ModelsRevisions, DriveWildcardRomTest,
    testing::Combine(testing::Bool(),
        testing::Values(test_rom::Set::Upgrade, test_rom::Set::Rom4, test_rom::Set::Waterloo)));

INSTANTIATE_TEST_SUITE_P(ModelsRevisions, DriveRelativeRomTest,
    testing::Combine(testing::Bool(),
        testing::Values(test_rom::Set::Upgrade, test_rom::Set::Rom4, test_rom::Set::Waterloo)));

// Check exact native status text, with a drive suffix only for 8050 DOS 2.7.
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
    EXPECT_EQ(receive_status(system, firmware, controller, device),
        "62, FILE NOT FOUND,00,00" + suffix + "\r");
    EXPECT_EQ(receive_status(system, firmware, controller, peer),
        "62, FILE NOT FOUND,00,00" + peer_suffix + "\r");
    EXPECT_EQ(receive_status(system, firmware, controller, device),
        "00, OK,00,00" + suffix + "\r");
}

INSTANTIATE_TEST_SUITE_P(ModelsUnitsDrives, DriveStatusTest,
    testing::Combine(testing::Bool(), testing::Values(uint8_t{8}, uint8_t{9}, uint8_t{10}, uint8_t{11}),
        testing::Values(uint8_t{0}, uint8_t{1})));

// Checks CPU-written VIA state and the FPGA's injected external line inputs.
TEST_F(FirmwareTest, CpuViaWriteAndInjectedRead) {
    IeeeController controller(system);
    firmware.mount(disk_fixture::D64::empty());
    controller.write(pet::ViaPortB, ViaIdle);
    EXPECT_EQ(system.ieee().peek_via_orb(), ViaIdle);
    // Ignore unrelated pins, especially PB5's running vertical-retrace input.
    EXPECT_EQ(controller.read(pet::ViaPortB) & ViaIeeeIdle, ViaIeeeIdle);
}

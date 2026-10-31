// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <gmock/gmock.h>

#include "framework/firmware_test.h"
#include "ieee/controller.h"
#include "ieee/trace.h"

extern "C" {
#include "ieee/dos.h"
}

namespace {
using namespace ieee_test;
using disk_fixture::D64;
constexpr uint8_t DataChannel = 2;
constexpr size_t DirectoryStartTrack = 3;
constexpr size_t DirectoryStartSector = 4;
constexpr size_t D80SectorsPerTrack = 29;
constexpr size_t D80DirectoryTrack = 39;
constexpr uint8_t D80DataTrack = 38;
constexpr size_t SequentialFixtureLimit = 3 * DISKIMAGE_SECTOR_PAYLOAD_SIZE;
constexpr econopet::Cycles RomServicePeriod = 1000;
constexpr econopet::Cycles TerminalRomBudget = 12'000'000;
constexpr econopet::CpuAddress NativePayload{0x0800}, NativeLastStatus{0x0900};

// Resolve the directory independently of the production parser.
size_t sequential_directory(bool d80) {
    return d80 ? ((D80DirectoryTrack - 1) * D80SectorsPerTrack + 1) * DISKIMAGE_SECTOR_SIZE : D64::offset(18, 1);
}

// Locate the first data sector from the fixture's raw directory entry.
size_t sequential_first_sector(const std::vector<uint8_t>& image, bool d80) {
    const auto directory = sequential_directory(d80);
    const auto track = image.at(directory + DirectoryStartTrack);
    const auto sector = image.at(directory + DirectoryStartSector);
    return d80 ? ((track - 1) * D80SectorsPerTrack + sector) * DISKIMAGE_SECTOR_SIZE : D64::offset(track, sector);
}

// Build bounded SEQ fixtures without using the production geometry or parser.
std::vector<uint8_t> sequential_image(bool d80, const std::vector<uint8_t>& payload) {
    if (payload.size() < 2 || payload.size() > SequentialFixtureLimit)
        throw std::invalid_argument("SEQ fixture requires 2..762 payload bytes");
    if (!d80)
        return D64{}.seq("BASIC", payload).bytes();
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
    for (size_t used = 0, sector = 0; used < payload.size(); ++sector) {
        const auto offset = ((D80DataTrack - 1) * D80SectorsPerTrack + sector) * DISKIMAGE_SECTOR_SIZE;
        const auto count = std::min(payload.size() - used, size_t{DISKIMAGE_SECTOR_PAYLOAD_SIZE});
        const bool last = used + count == payload.size();
        image[offset] = last ? 0 : D80DataTrack;
        image[offset + 1] = static_cast<uint8_t>(last ? count + 1 : sector + 1);
        std::copy_n(payload.begin() + used, count, image.begin() + offset + DISKIMAGE_SECTOR_LINK_SIZE);
        used += count;
    }
    return image;
}

// Bound cooperative production service, including its physical SPI transactions.
void service_rx(System& system, FirmwareSession& firmware, const char* context) {
    system.execution()
        .within(ByteBudget)
        .service(firmware.service_adapter())
        .until([](const System& board) { return !board.snapshot().ieee_rx_ready_o; })
        .run(context);
}

// Send OPEN with an exact passive wire observation, including filename EOI.
void open_file(System& system, IeeeController& controller, Channel channel = Channel{LoadChannel},
               std::string_view filename = "BASIC", Device device = Device{DeviceAddress}) {
    auto wire = system.trace_ieee();
    controller.command(Listen(device));
    controller.command(Open(channel));
    std::vector<Record> expected{command(Listen(device)), command(Open(channel))};
    for (size_t i = 0; i < filename.size(); ++i) {
        const auto byte = static_cast<uint8_t>(filename[i]);
        const auto eoi = i + 1 == filename.size() ? Eoi::Last : Eoi::More;
        controller.data(byte, eoi);
        expected.push_back(data(byte, eoi));
    }
    controller.command(Unlisten);
    expected.push_back(command(Unlisten));
    require_sequence(wire.records(), expected, "OPEN wire sequence");
}

// Independently compare CPU-accepted bytes and the passive DAV-edge trace.
void receive_payload(System& system, FirmwareSession& firmware, IeeeController& controller,
                     const std::vector<Record>& expected) {
    auto wire = system.trace_ieee();
    std::vector<Record> received;
    for (size_t i = 0; i < expected.size(); ++i) {
        firmware.service();
        received.push_back(controller.receive());
        if (received.back().eoi == Eoi::Last)
            break;
    }
    require_sequence(received, expected, "CPU payload");
    require_sequence(wire.records(), expected, "wire payload");
}

// Observe ordinary PUT and EOI over real PIA/VIA lines, then persist with CLOSE.
void write_file(System& system, FirmwareSession& firmware, IeeeController& controller, std::string_view name,
                const std::vector<uint8_t>& bytes) {
    constexpr Device Drive{DeviceAddress};
    constexpr Channel Output{DataChannel};
    open_file(system, controller, Output, name);
    service_rx(system, firmware, "writable OPEN");
    auto wire = system.trace_ieee();
    controller.command(Listen(Drive));
    controller.command(Secondary(Output));
    std::vector<Record> expected{command(Listen(Drive)), command(Secondary(Output))};
    for (size_t index = 0; index < bytes.size(); ++index) {
        firmware.service();
        const Eoi eoi = index + 1 == bytes.size() ? Eoi::Last : Eoi::More;
        controller.data(bytes[index], eoi);
        expected.push_back(data(bytes[index], eoi));
    }
    controller.command(Unlisten);
    expected.push_back(command(Unlisten));
    service_rx(system, firmware, "writable UNLISTEN");
    controller.command(Listen(Drive));
    controller.command(Close(Output));
    controller.command(Unlisten);
    expected.insert(expected.end(), {command(Listen(Drive)), command(Close(Output)), command(Unlisten)});
    service_rx(system, firmware, "persist writable CLOSE");
    require_sequence(wire.records(), expected, "ordinary PUT and CLOSE wire sequence");
}

// Failed-board teardown must retire a dirty writer before freeing its backing.
TEST_F(SystemTest, DirtyWriterTeardownExceptionDetachesStorageWithoutSpi) {
    for (const bool deadline : {false, true}) {
        SCOPED_TRACE(deadline ? "deadline" : "observer");
        System board;
        IeeeController controller(board);
        FirmwareSession firmware(board);
        firmware.mount(D64::empty());
        open_file(board, controller, Channel{DataChannel}, "0:DIRTY,S,W");
        service_rx(board, firmware, "dirty teardown OPEN");
        controller.command(Listen(Device{DeviceAddress}));
        controller.command(Secondary(Channel{DataChannel}));
        controller.data(0x5a);
        controller.command(Unlisten);
        service_rx(board, firmware, "dirty teardown PUT");
        ASSERT_EQ(test_open_file_count(), 1u);

        if (deadline) {
            EXPECT_THROW(board.service_until([](const System&) { return false; },
                                             [&](System&, econopet::Cycles) { firmware.close(); }, 1,
                                             "dirty teardown deadline"),
                         std::runtime_error);
        } else {
            auto observation =
                board.observe([](const System&) { throw std::runtime_error("dirty teardown observer"); });
            try {
                firmware.close();
                FAIL() << "expected teardown observer failure";
            } catch (const std::runtime_error& error) {
                EXPECT_STREQ(error.what(), "dirty teardown observer");
            }
        }
        ASSERT_EQ(test_open_file_count(), 0u);
        EXPECT_FALSE(test_file_registered("/disks/test.d64"));
        System other;
        FirmwareSession replacement(other);
        replacement.mount(D64::empty());
        replacement.service();
        replacement.close();
        EXPECT_EQ(test_open_file_count(), 0u);
    }
}

// Send one EOI-terminated record through the controller and production firmware.
void put_record(System& system, FirmwareSession& firmware, IeeeController& controller, Channel channel,
                const std::vector<uint8_t>& bytes) {
    constexpr Device Drive{DeviceAddress};
    auto wire = system.trace_ieee();
    controller.command(Listen(Drive));
    controller.command(Secondary(channel));
    std::vector<Record> expected{command(Listen(Drive)), command(Secondary(channel))};
    for (size_t index = 0; index < bytes.size(); ++index) {
        firmware.service();
        const auto eoi = index + 1 == bytes.size() ? Eoi::Last : Eoi::More;
        controller.data(bytes[index], eoi);
        expected.push_back(data(bytes[index], eoi));
    }
    controller.command(Unlisten);
    expected.push_back(command(Unlisten));
    service_rx(system, firmware, "REL record PUT completion");
    require_sequence(wire.records(), expected, "REL PUT wire sequence");
}

// An exhausted or absent source cannot drive bytes for an armed listener.
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

// Check every status byte and the final CR/EOI independently of firmware state.
std::string receive_status(System& system, FirmwareSession& firmware, IeeeController& controller, Device device) {
    constexpr size_t StatusByteLimit = DOS_STATUS_TEXT_MAX + 1;
    controller.command(Talk(device));
    controller.command(Secondary(Channel{CommandChannel}));
    service_rx(system, firmware, "service status request");
    auto wire = system.trace_ieee();
    std::vector<Record> records;
    std::string text;
    for (size_t i = 0; i < StatusByteLimit; ++i) {
        firmware.service();
        records.push_back(controller.receive());
        require_sequence({records.back()}, {data(records.back().data, records.back().eoi)}, "status data flags");
        text.push_back(static_cast<char>(records.back().data));
        if (records.back().eoi == Eoi::Last)
            break;
    }
    require_sequence(wire.records(), records, "status CPU and wire payload");
    require_sequence({records.back()}, {data('\r', Eoi::Last)}, "status final CR/EOI");
    controller.command(Untalk);
    service_rx(system, firmware, "service status UNTALK");
    return text;
}

// Cover short files and both sides of the first sector boundary.
class DrivePrgTest : public FirmwareTest, public testing::WithParamInterface<size_t> {};
// Cross all three native models with both local slot positions.
class NativeDriveTest : public FirmwareTest, public testing::WithParamInterface<int> {};
// Cover the ordinary one-byte EOF quirk with zero, CR and high-bit bytes.
class DriveOneByteTest : public FirmwareTest, public testing::WithParamInterface<uint8_t> {};
// Keep terminal/read/status observations distinct for 4040 and 8050.
class DriveTerminalTest : public FirmwareTest, public testing::WithParamInterface<bool> {};
// Contrast the tape suite's old one-timer policy with native STOP-key retries.
class DriveFormatWaitTest : public FirmwareTest, public testing::WithParamInterface<bool> {};
// Each ROM service period starts with a fresh coordinated firmware/fabric state.
class DriveRepeatedStatusTest : public FirmwareTest, public testing::WithParamInterface<std::tuple<bool, bool>> {};
// Cross cold status transitions with every model and independent MCU service periods.
class DriveColdStatusTest : public FirmwareTest, public testing::WithParamInterface<std::tuple<int, bool>> {};
// Cross independent model, device and selected-drive status identities.
class DriveStatusTest : public FirmwareTest, public testing::WithParamInterface<std::tuple<bool, uint8_t, uint8_t>> {};
// Vary native model, source FIFO and whether NRFD has committed the presented byte.
class DriveInterruptionTest : public FirmwareTest, public testing::WithParamInterface<std::tuple<int, bool, bool>> {};
// Exercise host media lifecycle and shared-FIFO recovery on every native model.
class DriveLifecycleTest : public FirmwareTest, public testing::WithParamInterface<int> {};

// Select exact raw geometry without borrowing the production geometry helpers.
std::vector<uint8_t> lifecycle_image(int format, const std::vector<uint8_t>& payload) {
    auto image = sequential_image(format != 0, payload);
    if (format == 2)
        image.resize(DISKIMAGE_D82_SIZE);
    return image;
}

// Supply readable files and bounded native BAM ranges for INITIALIZE on each model.
std::vector<uint8_t> command_image(const int format) {
    auto image = lifecycle_image(format, {0x41, 0x42, 0x43, 0x44});
    if (format == 0) {
        return image;
    }
    constexpr size_t BamTrack = 38, DataTrack = 37, TracksPerPart = 50;
    constexpr size_t BamSectorStride = 3, BamLinkTrack = 0, BamLinkSector = 1;
    constexpr size_t BamFormat = 2, BamRangeStart = 4, BamRangeEnd = 5;
    const size_t data_offset = (DataTrack - 1) * D80SectorsPerTrack * DISKIMAGE_SECTOR_SIZE;
    const size_t bam_offset = (BamTrack - 1) * D80SectorsPerTrack * DISKIMAGE_SECTOR_SIZE;
    const size_t header_offset = (D80DirectoryTrack - 1) * D80SectorsPerTrack * DISKIMAGE_SECTOR_SIZE;
    std::copy_n(image.begin() + bam_offset, DISKIMAGE_SECTOR_SIZE, image.begin() + data_offset);
    image[sequential_directory(true) + DirectoryStartTrack] = DataTrack;
    image[header_offset + BamLinkTrack] = BamTrack;
    image[header_offset + BamLinkSector] = 0;
    image[header_offset + BamFormat] = 'C';
    const size_t parts = format == 1 ? 2 : 4;
    const size_t tracks = format == 1 ? DISKIMAGE_D80_TRACK_COUNT : DISKIMAGE_D82_TRACK_COUNT;
    // The remaining sectors are deliberately allocated, with consistent zero free counts.
    for (size_t part = 0; part < parts; ++part) {
        const size_t offset = bam_offset + part * BamSectorStride * DISKIMAGE_SECTOR_SIZE;
        std::fill_n(image.begin() + offset, DISKIMAGE_SECTOR_SIZE, 0);
        image[offset + BamLinkTrack] = part + 1 < parts ? BamTrack : D80DirectoryTrack;
        image[offset + BamLinkSector] = part + 1 < parts ? (part + 1) * BamSectorStride : 1;
        image[offset + BamFormat] = 'C';
        image[offset + BamRangeStart] = part * TracksPerPart + 1;
        image[offset + BamRangeEnd] = std::min((part + 1) * TracksPerPart + 1, tracks + 1);
    }
    return image;
}

// Give copy commands writable data space without changing the initialize-only fixture.
std::vector<uint8_t> copy_image(const int format) {
    auto image = command_image(format);
    if (format == 0) {
        return image;
    }
    constexpr size_t BamTrack = 38, DataTrack = 37, BamEntryStart = 6, BamEntrySize = 5;
    constexpr uint8_t FreeSectors = D80SectorsPerTrack - 1;
    const size_t entry =
        (BamTrack - 1) * D80SectorsPerTrack * DISKIMAGE_SECTOR_SIZE + BamEntryStart + (DataTrack - 1) * BamEntrySize;
    image[entry] = FreeSectors;
    for (size_t sector = 1; sector < D80SectorsPerTrack; ++sector) {
        image[entry + 1 + sector / 8] |= static_cast<uint8_t>(1u << (sector % 8));
    }
    return image;
}

// Every public media mutation must retain a busy source and ordered receive work.
void reject_media_mutations() {
    EXPECT_FALSE(ieee_drive_mount(0, "next.img"));
    EXPECT_FALSE(ieee_drive_unmount_all());
}
} // namespace

// ROM OPNWCH/APPEND/CLSWRT must persist independently through production SPI.
TEST_F(FirmwareTest, OrdinaryWritesPersistBothDisksAndRetainInterruptedReadback) {
    constexpr Device Drive{DeviceAddress};
    constexpr Channel Input{DataChannel};
    IeeeController controller(system);
    firmware.mount(D64{}.bytes(), "write0.d64", 0);
    firmware.mount(D64{}.bytes(), "write1.d64", 1);
    std::vector<uint8_t> first(255), second(508), suffix(22);
    for (size_t index = 0; index < first.size(); ++index)
        first[index] = static_cast<uint8_t>(index);
    for (size_t index = 0; index < second.size(); ++index)
        second[index] = static_cast<uint8_t>(0x20 + index);
    for (size_t index = 0; index < suffix.size(); ++index)
        suffix[index] = static_cast<uint8_t>(0x80 + index);
    write_file(system, firmware, controller, "0:FILE,S,W", first);
    write_file(system, firmware, controller, "1:FILE,S,W", second);
    write_file(system, firmware, controller, "0:FILE,A", suffix);
    first.insert(first.end(), suffix.begin(), suffix.end());
    ASSERT_TRUE(ieee_drive_unmount_all());
    ASSERT_TRUE(ieee_drive_mount(0, "write0.d64"));
    ASSERT_TRUE(ieee_drive_mount(1, "write1.d64"));
    for (const auto& [name, expected] :
         std::vector<std::pair<std::string, std::vector<uint8_t>>>{{"0:F?L*,S,R", first}, {"1:FILE,S,R", second}}) {
        open_file(system, controller, Input, name);
        service_rx(system, firmware, "persisted OPEN");
        controller.command(Talk(Drive));
        controller.command(Secondary(Input));
        service_rx(system, firmware, "persisted TALK");
        for (size_t index = 0; index < expected.size(); ++index) {
            firmware.service();
            EXPECT_EQ(controller.receive(),
                      data(expected[index], index + 1 == expected.size() ? Eoi::Last : Eoi::More));
            if (index == 1) {
                controller.command(Untalk);
                service_rx(system, firmware, "interrupt persisted stream");
                EXPECT_EQ(receive_status(system, firmware, controller, Drive), "00, OK,00,00\r");
                controller.command(Talk(Drive));
                controller.command(Secondary(Input));
                service_rx(system, firmware, "resume persisted stream");
            }
        }
        controller.command(Untalk);
        service_rx(system, firmware, "persisted UNTALK");
    }
    write_file(system, firmware, controller, "@0:FILE,S,W", suffix);
    open_file(system, controller, Input, "0:FILE,S,R");
    service_rx(system, firmware, "replacement OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(Input));
    service_rx(system, firmware, "replacement TALK");
    std::vector<Record> expected;
    for (size_t index = 0; index < suffix.size(); ++index)
        expected.push_back(data(suffix[index], index + 1 == suffix.size() ? Eoi::Last : Eoi::More));
    receive_payload(system, firmware, controller, expected);
    controller.command(Untalk);
    service_rx(system, firmware, "replacement UNTALK");
}

// TLK20 aborts without GET before NRFD assertion, but TLK30 GET survives ATN.
TEST_P(DriveInterruptionTest, BusyMutationAndAtnRetainExactNativeGetProgress) {
    const auto [format, status_source, fetched] = GetParam();
    constexpr Device Drive{DeviceAddress};
    constexpr uint8_t AcceptorBusy = BusReleased & ~pet::Nrfd;
    IeeeController controller(system);
    std::vector<uint8_t> payload(300);
    for (size_t index = 0; index < payload.size(); ++index)
        payload[index] = static_cast<uint8_t>(0x31 + index);
    firmware.mount(lifecycle_image(format, payload), "old.img");
    firmware.mount(lifecycle_image(format, {0x91, 0x92}), "next.img", 2);
    if (!status_source) {
        open_file(system, controller, Channel{DataChannel}, "BASIC,SEQ,R");
        service_rx(system, firmware, "interrupted OPEN");
    }
    const Channel channel{status_source ? CommandChannel : DataChannel};
    controller.command(Talk(Drive));
    controller.command(Secondary(channel));
    service_rx(system, firmware, "interrupted TALK");
    std::vector<Record> expected;
    if (status_source) {
        const std::string banner = format == 0 ? "73,CBM DOS V2,00,00\r" : "73,CBM DOS V2.7,00,00,0\r";
        for (size_t index = 0; index < banner.size(); ++index)
            expected.push_back(
                data(static_cast<uint8_t>(banner[index]), index + 1 == banner.size() ? Eoi::Last : Eoi::More));
    } else {
        for (size_t index = 0; index < payload.size(); ++index)
            expected.push_back(data(payload[index], index + 1 == payload.size() ? Eoi::Last : Eoi::More));
    }

    // Hold NDAC asserted through a real DAV presentation and each rejected API.
    auto presented = system.trace_ieee();
    controller.write(pet::Pia2ControlA, PiaC2Low.bits());
    controller.write(pet::ViaPortB, AcceptorBusy);
    controller.write(pet::ViaPortB, BusReleased);
    system.run_until([](const System& board) { return !board.snapshot().ieee_bus_dav_no; }, ByteBudget,
                     "active DAV before media mutation");
    reject_media_mutations();
    EXPECT_FALSE(system.snapshot().ieee_bus_dav_no);
    EXPECT_EQ(static_cast<uint8_t>(~controller.read(pet::Pia2PortA)), expected.front().data);
    if (fetched) {
        controller.write(pet::ViaPortB, AcceptorBusy);
        firmware.service();
        reject_media_mutations();
        EXPECT_FALSE(system.snapshot().ieee_bus_dav_no);
    }
    require_sequence(presented.records(), {expected.front()}, "unacknowledged presentation");

    // Abort before NDAC acceptance, then rebuild only the native unread suffix.
    controller.write(pet::ViaPortB, AcceptorBusy & ~pet::Atn);
    controller.command(Untalk);
    service_rx(system, firmware, "interrupted source UNTALK");
    controller.command(Talk(Drive));
    controller.command(Secondary(channel));
    service_rx(system, firmware, "interrupted source retalk");
    if (fetched)
        expected.erase(expected.begin());
    receive_payload(system, firmware, controller, expected);
    controller.command(Untalk);
    service_rx(system, firmware, "resumed source complete");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), format == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r");
}

INSTANTIATE_TEST_SUITE_P(ModelsSourcesAndGetPhases, DriveInterruptionTest,
                         testing::Combine(testing::Range(0, 3), testing::Bool(), testing::Bool()));

// A failed host mutation cannot erase a CLOSE waiting in the physical RX FIFO.
TEST_P(DriveLifecycleTest, PendingCloseRejectsMutationAndRemainsExecutable) {
    constexpr Device Drive{DeviceAddress};
    IeeeController controller(system);
    const int format = GetParam();
    const std::vector<uint8_t> payload{0x41, 0x42, 0x43, 0x44};
    firmware.mount(lifecycle_image(format, payload), "old.img");
    firmware.mount(lifecycle_image(format, {0x91, 0x92}), "next.img", 2);
    open_file(system, controller, Channel{DataChannel}, "BASIC,SEQ,R");
    service_rx(system, firmware, "pending CLOSE OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "pending CLOSE TALK");
    EXPECT_EQ(controller.receive(), data(payload.front()));
    controller.command(Untalk);
    service_rx(system, firmware, "pending CLOSE interrupted source");
    controller.command(Listen(Drive));
    controller.command(Close(Channel{DataChannel}));
    controller.command(Unlisten);
    ASSERT_TRUE(system.snapshot().ieee_rx_ready_o);
    reject_media_mutations();
    EXPECT_TRUE(system.snapshot().ieee_rx_ready_o);
    service_rx(system, firmware, "retained ordered CLOSE");
    controller.command(Talk(Drive));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "closed source TALK");
    expect_no_payload(system, firmware, controller);
    controller.command(Untalk);
    service_rx(system, firmware, "closed source UNTALK");
    open_file(system, controller, Channel{DataChannel}, "BASIC,SEQ,R");
    service_rx(system, firmware, "reopened old media");
    controller.command(Talk(Drive));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "reopened old source");
    receive_payload(system, firmware, controller, {data(0x41), data(0x42), data(0x43), data(0x44, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "reopened old source UNTALK");
}

// A peer OPEN globally flushes staged data, not the original source's GET cursor.
TEST_P(DriveLifecycleTest, PeerOpenRebuildsSharedFifoFromRetainedCursor) {
    constexpr Device Drive{DeviceAddress}, Peer{9};
    IeeeController controller(system);
    const int format = GetParam();
    firmware.mount(lifecycle_image(format, {0x41, 0x42, 0x43, 0x44}), "old.img");
    firmware.mount(lifecycle_image(format, {0x91, 0x92}), "next.img", 2);
    open_file(system, controller, Channel{DataChannel}, "BASIC,SEQ,R");
    service_rx(system, firmware, "peer-flush source OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "peer-flush source TALK");
    EXPECT_EQ(controller.receive(), data(0x41));
    controller.command(Untalk);
    service_rx(system, firmware, "peer-flush source paused");
    open_file(system, controller, Channel{DataChannel}, "BASIC,SEQ,R", Peer);
    service_rx(system, firmware, "peer OPEN flushes shared data");
    controller.command(Talk(Drive));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "source after peer OPEN");
    receive_payload(system, firmware, controller, {data(0x42), data(0x43), data(0x44, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "source after peer OPEN UNTALK");
    controller.command(Talk(Peer));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "peer source TALK");
    receive_payload(system, firmware, controller, {data(0x91), data(0x92, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "peer source UNTALK");
}

// A quiescent remount drops old data/status copies and old channel readiness.
TEST_P(DriveLifecycleTest, RemountDiscardsOldSourcesAndReopensReplacement) {
    constexpr Device Drive{DeviceAddress};
    IeeeController controller(system);
    const int format = GetParam();
    firmware.mount(lifecycle_image(format, {0x41, 0x42, 0x43, 0x44}), "old.img");
    firmware.mount(lifecycle_image(format, {0x91, 0x92}), "next.img", 2);
    open_file(system, controller, Channel{DataChannel}, "BASIC,SEQ,R");
    service_rx(system, firmware, "remount old OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "remount old TALK");
    EXPECT_EQ(controller.receive(), data(0x41));
    controller.command(Untalk);
    service_rx(system, firmware, "remount old data retained");
    controller.command(Talk(Drive));
    controller.command(Secondary(Channel{CommandChannel}));
    service_rx(system, firmware, "remount old status");
    EXPECT_EQ(controller.receive(), data('0'));
    controller.command(Untalk);
    service_rx(system, firmware, "remount old status retained");
    ASSERT_TRUE(ieee_drive_mount(0, "next.img"));
    EXPECT_EQ(receive_status(system, firmware, controller, Drive),
              format == 0 ? "73,CBM DOS V2,00,00\r" : "73,CBM DOS V2.7,00,00,0\r");
    controller.command(Talk(Drive));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "remount invalidated channel");
    expect_no_payload(system, firmware, controller);
    controller.command(Untalk);
    service_rx(system, firmware, "remount invalidated UNTALK");
    open_file(system, controller, Channel{DataChannel}, "BASIC,SEQ,R");
    service_rx(system, firmware, "remount replacement OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "remount replacement TALK");
    receive_payload(system, firmware, controller, {data(0x91), data(0x92, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "remount replacement UNTALK");
}

// The drive-reset API retires RX/FIFO/channel state but retains both file owners.
TEST_P(DriveLifecycleTest, DriveResetRetainsMediaAndCancelsOldSourcesAndRx) {
    constexpr Device Drive{DeviceAddress}, Peer{9};
    IeeeController controller(system);
    const int format = GetParam();
    firmware.mount(lifecycle_image(format, {0x41, 0x42, 0x43, 0x44}), "old.img");
    firmware.mount(lifecycle_image(format, {0x91, 0x92}), "next.img", 2);
    open_file(system, controller, Channel{DataChannel}, "BASIC,SEQ,R");
    service_rx(system, firmware, "drive reset old OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "drive reset old TALK");
    EXPECT_EQ(controller.receive(), data(0x41));
    controller.command(Untalk);
    service_rx(system, firmware, "drive reset old source retained");
    controller.command(Listen(Drive));
    controller.command(Close(Channel{DataChannel}));
    controller.command(Unlisten);
    ASSERT_TRUE(system.snapshot().ieee_rx_ready_o);
    ieee_drive_reset();
    EXPECT_FALSE(system.snapshot().ieee_rx_ready_o);
    const std::string banner = format == 0 ? "73,CBM DOS V2,00,00\r" : "73,CBM DOS V2.7,00,00,0\r";
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), banner);
    EXPECT_EQ(receive_status(system, firmware, controller, Peer), banner);
    controller.command(Talk(Drive));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "drive reset closed channel");
    expect_no_payload(system, firmware, controller);
    controller.command(Untalk);
    service_rx(system, firmware, "drive reset closed UNTALK");
    open_file(system, controller, Channel{DataChannel}, "BASIC,SEQ,R");
    service_rx(system, firmware, "drive reset reopened binding");
    controller.command(Talk(Drive));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "drive reset reopened TALK");
    receive_payload(system, firmware, controller, {data(0x41), data(0x42), data(0x43), data(0x44, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "drive reset reopened UNTALK");
}

INSTANTIATE_TEST_SUITE_P(Models, DriveLifecycleTest, testing::Range(0, 3));

// RECORD rejects non-REL channels without consuming staged local or peer bytes.
TEST_P(DriveLifecycleTest, NativeRecordRejectsBinaryRequestsAndPreservesCursors) {
    constexpr Device Drive{DeviceAddress}, Peer{9};
    constexpr Channel Reader{DataChannel}, Writer{DataChannel + 1}, Direct{DataChannel + 2};
    IeeeController controller(system);
    const int format = GetParam();
    firmware.mount(command_image(format), "record.img");
    firmware.mount(command_image(format), "record-peer.img", 2);
    open_file(system, controller, Reader, "0:BASIC,S,R");
    service_rx(system, firmware, "record reader OPEN");
    open_file(system, controller, Writer, "0:PWRITE,S,W");
    service_rx(system, firmware, "record writer OPEN");
    open_file(system, controller, Direct, "#");
    service_rx(system, firmware, "record direct OPEN");
    open_file(system, controller, Reader, "0:BASIC,S,R", Peer);
    service_rx(system, firmware, "record peer OPEN");
    for (const auto device : {Drive, Peer}) {
        controller.command(Talk(device));
        controller.command(Secondary(Reader));
        service_rx(system, firmware, "record paused source");
        EXPECT_EQ(controller.receive(), data(0x41));
        controller.command(Untalk);
        service_rx(system, firmware, "record paused UNTALK");
    }

    // Both OPEN 15 and the ordinary command secondary accept embedded zero/CR.
    for (const bool open : {false, true}) {
        for (const uint8_t raw : {uint8_t{0}, uint8_t{2}, uint8_t{0x62}, uint8_t{3}, uint8_t{4}, uint8_t{13},
                                  uint8_t{15}, uint8_t{16}, uint8_t{17}, uint8_t{18}, uint8_t{0xff}}) {
            const std::string request{'P', static_cast<char>(raw), '\0', '\0', '\1'};
            if (open) {
                open_file(system, controller, Channel{CommandChannel}, request);
            } else {
                controller.command(Listen(Drive));
                controller.command(Secondary(Channel{CommandChannel}));
                for (size_t byte = 0; byte < request.size(); ++byte) {
                    controller.data(static_cast<uint8_t>(request[byte]),
                                    byte + 1 == request.size() ? Eoi::Last : Eoi::More);
                }
                controller.command(Unlisten);
            }
            service_rx(system, firmware, "record binary P");
            const bool mismatch = raw == 2 || raw == 0x62 || raw == 4 || raw == 15 || raw == 16 || raw == 0xff;
            const std::string expected = mismatch ? "64, FILE TYPE MISMATCH,00,00" : "70,NO CHANNEL,00,00";
            EXPECT_EQ(receive_status(system, firmware, controller, Drive), expected + (format == 0 ? "\r" : ",0\r"));
        }
    }
    for (const auto device : {Drive, Peer}) {
        controller.command(Talk(device));
        controller.command(Secondary(Reader));
        service_rx(system, firmware, "record preserved source");
        EXPECT_EQ(controller.receive(), data(0x42));
        controller.command(Untalk);
        service_rx(system, firmware, "record preserved UNTALK");
    }
}

// Public REL creation, sparse growth and dependent commands survive actual media reload.
TEST_P(NativeDriveTest, NativeRelPublicCreatePositionCopyAndRemount) {
    constexpr Device Drive{DeviceAddress}, Peer{9};
    constexpr Channel Reader{DataChannel}, Command{CommandChannel};
    constexpr uint8_t RecordSize = 127, SparseRecord = 5, RecordPosition = 1;
    const int format = GetParam() / 2;
    const unsigned int slot = static_cast<unsigned int>(GetParam()) % 2;
    const std::string prefix = std::to_string(slot) + ":";
    const std::string suffix = format == 0 ? "\r" : "," + std::to_string(slot) + "\r";
    IeeeController controller(system);
    firmware.mount(copy_image(format), "public-rel.img", slot);
    firmware.mount(command_image(format), "public-rel-peer.img", 2);
    open_file(system, controller, Reader, "0:BASIC,S,R", Peer);
    service_rx(system, firmware, "REL peer OPEN");
    controller.command(Talk(Peer));
    controller.command(Secondary(Reader));
    service_rx(system, firmware, "REL paused peer");
    EXPECT_EQ(controller.receive(), data(0x41));
    controller.command(Untalk);
    service_rx(system, firmware, "REL paused peer UNTALK");

    open_file(system, controller, Reader, prefix + "REL,L," + static_cast<char>(RecordSize));
    service_rx(system, firmware, "REL binary OPEN");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), "00, OK,00,00" + suffix);
    put_record(system, firmware, controller, Reader, {0x41, 0, 0x42});
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), "00, OK,00,00" + suffix);
    const std::string sparse{'P', DataChannel, SparseRecord, 0, RecordPosition};
    open_file(system, controller, Command, sparse);
    service_rx(system, firmware, "REL missing record position");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), "50, RECORD NOT PRESENT,00,00" + suffix);
    put_record(system, firmware, controller, Reader, {0x54, 0x41, 0x49, 0x4c});
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), "00, OK,00,00" + suffix);
    const std::string first{'P', DataChannel, 1, 0, RecordPosition};
    open_file(system, controller, Command, first);
    service_rx(system, firmware, "REL rewind record");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), "00, OK,00,00" + suffix);
    controller.command(Talk(Drive));
    controller.command(Secondary(Reader));
    service_rx(system, firmware, "REL record TALK");
    receive_payload(system, firmware, controller, {data(0x41), data(0), data(0x42, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "REL record UNTALK");
    controller.command(Listen(Drive));
    controller.command(Close(Reader));
    controller.command(Unlisten);
    service_rx(system, firmware, "REL public CLOSE");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), "00, OK,00,00" + suffix);
    open_file(system, controller, Command, "C" + prefix + "COPY=" + prefix + "REL");
    service_rx(system, firmware, "REL native COPY");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), "00, OK,00,00" + suffix);
    controller.command(Talk(Peer));
    controller.command(Secondary(Reader));
    service_rx(system, firmware, "REL retained peer");
    EXPECT_EQ(controller.receive(), data(0x42));
    controller.command(Untalk);
    service_rx(system, firmware, "REL retained peer UNTALK");

    // Reload actual backing bytes rather than relying on any resident REL page.
    ASSERT_TRUE(ieee_drive_unmount_all());
    ASSERT_TRUE(ieee_drive_mount(slot, "public-rel.img"));
    open_file(system, controller, Reader, prefix + "COPY");
    service_rx(system, firmware, "REL copied remount OPEN");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), "00, OK,00,00" + suffix);
    open_file(system, controller, Command, sparse);
    service_rx(system, firmware, "REL copied remount position");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), "00, OK,00,00" + suffix);
    controller.command(Talk(Drive));
    controller.command(Secondary(Reader));
    service_rx(system, firmware, "REL copied remount TALK");
    receive_payload(system, firmware, controller, {data(0x54), data(0x41), data(0x49), data(0x4c, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "REL copied remount UNTALK");
    open_file(system, controller, Command, "S" + prefix + "REL=R");
    service_rx(system, firmware, "REL scratch source");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), "01, FILES SCRATCHED,01,00" + suffix);
    open_file(system, controller, Command, "V" + std::to_string(slot));
    service_rx(system, firmware, "REL validate copy");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), "00, OK,00,00" + suffix);
}

// WR30 reports delayed overflow and CLSALL retires every old FIFO copy.
TEST_P(NativeDriveTest, NativeRelOverflowCloseAllAndPersistedBytes) {
    constexpr Device Drive{DeviceAddress};
    constexpr Channel Reader{DataChannel}, Command{CommandChannel};
    constexpr uint8_t RecordSize = 3;
    const int format = GetParam() / 2;
    const unsigned int slot = static_cast<unsigned int>(GetParam()) % 2;
    const std::string prefix = std::to_string(slot) + ":";
    const std::string suffix = format == 0 ? "\r" : "," + std::to_string(slot) + "\r";
    IeeeController controller(system);
    firmware.mount(copy_image(format), "rel-overflow.img", slot);
    open_file(system, controller, Reader, prefix + "REL,L," + static_cast<char>(RecordSize));
    service_rx(system, firmware, "REL overflow OPEN");
    put_record(system, firmware, controller, Reader, {0x41, 0x42, 0x43, 0x44});
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), "51,OVERFLOW IN RECORD,00,00" + suffix);
    const std::string first{'P', DataChannel, 1, 0, 1};
    open_file(system, controller, Command, first);
    service_rx(system, firmware, "REL overflow rewind");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), "00, OK,00,00" + suffix);
    controller.command(Talk(Drive));
    controller.command(Secondary(Reader));
    service_rx(system, firmware, "REL overflow staged read");
    EXPECT_EQ(controller.receive(), data(0x41));
    controller.command(Untalk);
    service_rx(system, firmware, "REL overflow paused read");
    controller.command(Listen(Drive));
    controller.command(Close(Command));
    controller.command(Unlisten);
    service_rx(system, firmware, "REL CLOSE-all");
    controller.command(Talk(Drive));
    controller.command(Secondary(Reader));
    service_rx(system, firmware, "REL closed source TALK");
    expect_no_payload(system, firmware, controller);
    controller.command(Untalk);
    service_rx(system, firmware, "REL closed source UNTALK");
    ASSERT_TRUE(ieee_drive_unmount_all());
    ASSERT_TRUE(ieee_drive_mount(slot, "rel-overflow.img"));
    open_file(system, controller, Reader, prefix + "REL");
    service_rx(system, firmware, "REL overflow persisted OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(Reader));
    service_rx(system, firmware, "REL overflow persisted TALK");
    receive_payload(system, firmware, controller, {data(0x41), data(0x42), data(0x43, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "REL overflow persisted UNTALK");
}

// ROM USER/INITDR/DSKINT must retire old FIFO copies without consuming peer GETs.
TEST_P(DriveLifecycleTest, NativeCommandsRetireSelectedSourcesAndPreservePeers) {
    constexpr Device Drive{DeviceAddress}, Peer{9};
    constexpr Channel First{DataChannel}, Second{DataChannel + 1}, Command{CommandChannel};
    IeeeController controller(system);
    const int format = GetParam();
    firmware.mount(command_image(format), "commands0.img");
    firmware.mount(command_image(format), "commands1.img", 1);
    firmware.mount(command_image(format), "peer.img", 2);
    open_file(system, controller, First, "0:BASIC,S,R");
    service_rx(system, firmware, "command first OPEN");
    open_file(system, controller, Second, "1:BASIC,S,R");
    service_rx(system, firmware, "command second OPEN");
    open_file(system, controller, First, "0:BASIC,S,R", Peer);
    service_rx(system, firmware, "command peer OPEN");
    for (const auto [device, channel] : {std::pair{Drive, First}, {Drive, Second}, {Peer, First}}) {
        controller.command(Talk(device));
        controller.command(Secondary(channel));
        service_rx(system, firmware, "command paused source");
        EXPECT_EQ(controller.receive(), data(0x41));
        controller.command(Untalk);
        service_rx(system, firmware, "command paused UNTALK");
    }
    open_file(system, controller, Command, "?");
    service_rx(system, firmware, "command unknown");
    controller.command(Talk(Drive));
    controller.command(Secondary(Command));
    service_rx(system, firmware, "command partial error status");
    EXPECT_EQ(controller.receive(), data('3'));
    EXPECT_EQ(controller.receive(), data('1'));
    controller.command(Untalk);
    service_rx(system, firmware, "command partial status UNTALK");

    // Use the ordinary secondary path, not only OPEN 15, for U0.
    controller.command(Listen(Drive));
    controller.command(Secondary(Command));
    controller.data('U');
    controller.data('0', Eoi::Last);
    controller.command(Unlisten);
    service_rx(system, firmware, "command U0");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), format == 0 ? "00, OK,00,00\r" : "00, OK,00,00,1\r");
    controller.command(Talk(Drive));
    controller.command(Secondary(First));
    service_rx(system, firmware, "command U0 preserved source");
    EXPECT_EQ(controller.receive(), data(0x42));
    controller.command(Untalk);
    service_rx(system, firmware, "command U0 preserved UNTALK");

    open_file(system, controller, Command, "I0");
    service_rx(system, firmware, "command INITIALIZE");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), format == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r");
    controller.command(Talk(Drive));
    controller.command(Secondary(First));
    service_rx(system, firmware, "command initialized source");
    expect_no_payload(system, firmware, controller);
    controller.command(Untalk);
    service_rx(system, firmware, "command initialized UNTALK");
    controller.command(Talk(Drive));
    controller.command(Secondary(Second));
    service_rx(system, firmware, "command preserved second drive");
    EXPECT_EQ(controller.receive(), data(0x42));
    controller.command(Untalk);
    service_rx(system, firmware, "command preserved second UNTALK");

    open_file(system, controller, Command, "UJ");
    service_rx(system, firmware, "command cold reset");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive),
              format == 0 ? "73,CBM DOS V2,00,00\r" : "73,CBM DOS V2.7,00,00,0\r");
    controller.command(Talk(Drive));
    controller.command(Secondary(Second));
    service_rx(system, firmware, "command reset second source");
    expect_no_payload(system, firmware, controller);
    controller.command(Untalk);
    service_rx(system, firmware, "command reset second UNTALK");
    controller.command(Talk(Peer));
    controller.command(Secondary(First));
    service_rx(system, firmware, "command reset preserved peer");
    receive_payload(system, firmware, controller, {data(0x42), data(0x43), data(0x44, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "command reset preserved peer UNTALK");
    open_file(system, controller, First, "0:BASIC,S,R");
    service_rx(system, firmware, "command reset reopened binding");
    controller.command(Talk(Drive));
    controller.command(Secondary(First));
    service_rx(system, firmware, "command reset reopened source");
    receive_payload(system, firmware, controller, {data(0x41), data(0x42), data(0x43), data(0x44, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "command reset reopened UNTALK");
}

// RENAME preserves committed GET cursors while updating only the selected disk's name.
TEST_P(DriveLifecycleTest, RenamePreservesBothDisksAndReopensPersistedName) {
    constexpr Device Drive{DeviceAddress};
    constexpr Channel First{DataChannel}, Second{DataChannel + 1}, Command{CommandChannel};
    IeeeController controller(system);
    const int format = GetParam();
    firmware.mount(command_image(format), "rename0.img");
    firmware.mount(command_image(format), "rename1.img", 1);
    open_file(system, controller, First, "0:BASIC,S,R");
    service_rx(system, firmware, "rename first OPEN");
    open_file(system, controller, Second, "1:BASIC,S,R");
    service_rx(system, firmware, "rename second OPEN");
    for (const auto channel : {First, Second}) {
        controller.command(Talk(Drive));
        controller.command(Secondary(channel));
        service_rx(system, firmware, "rename paused source");
        EXPECT_EQ(controller.receive(), data(0x41));
        controller.command(Untalk);
        service_rx(system, firmware, "rename paused UNTALK");
    }
    open_file(system, controller, Command, "R0:NEW=BASIC");
    service_rx(system, firmware, "rename command");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), format == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r");
    for (const auto channel : {First, Second}) {
        controller.command(Talk(Drive));
        controller.command(Secondary(channel));
        service_rx(system, firmware, "rename retained cursor");
        receive_payload(system, firmware, controller, {data(0x42), data(0x43), data(0x44, Eoi::Last)});
        controller.command(Untalk);
        service_rx(system, firmware, "rename retained UNTALK");
    }
    for (const auto& [command, expected] :
         {std::pair{"R0:NEW=NEW", "63, FILE EXISTS,00,00"}, std::pair{"R0:NEW=ABSENT", "62, FILE NOT FOUND,00,00"}}) {
        open_file(system, controller, Command, command);
        service_rx(system, firmware, "rename error command");
        EXPECT_EQ(receive_status(system, firmware, controller, Drive),
                  std::string(expected) + (format == 0 ? "\r" : ",0\r"));
    }
    ASSERT_TRUE(ieee_drive_unmount_all());
    ASSERT_TRUE(ieee_drive_mount(0, "rename0.img"));
    ASSERT_TRUE(ieee_drive_mount(1, "rename1.img"));
    open_file(system, controller, First, "0:NEW,S,R");
    service_rx(system, firmware, "rename persisted OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(First));
    service_rx(system, firmware, "rename persisted TALK");
    receive_payload(system, firmware, controller, {data(0x41), data(0x42), data(0x43), data(0x44, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "rename persisted UNTALK");
}

// SCRATCH skips an active file, then persists deletion without disturbing another disk's cursor.
TEST_P(DriveLifecycleTest, ScratchProtectsReadersAndPersistsSelectedDeletion) {
    constexpr Device Drive{DeviceAddress};
    constexpr Channel First{DataChannel}, Second{DataChannel + 1}, Command{CommandChannel};
    IeeeController controller(system);
    const int format = GetParam();
    firmware.mount(command_image(format), "scratch0.img");
    firmware.mount(command_image(format), "scratch1.img", 1);
    open_file(system, controller, First, "0:BASIC,S,R");
    service_rx(system, firmware, "scratch first OPEN");
    open_file(system, controller, Second, "1:BASIC,S,R");
    service_rx(system, firmware, "scratch peer OPEN");
    for (const auto channel : {First, Second}) {
        controller.command(Talk(Drive));
        controller.command(Secondary(channel));
        service_rx(system, firmware, "scratch paused source");
        EXPECT_EQ(controller.receive(), data(0x41));
        controller.command(Untalk);
        service_rx(system, firmware, "scratch paused UNTALK");
    }
    open_file(system, controller, Command, "S0:BASIC");
    service_rx(system, firmware, "scratch active command");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive),
              format == 0 ? "01, FILES SCRATCHED,00,00\r" : "01, FILES SCRATCHED,00,00,0\r");
    controller.command(Listen(Drive));
    controller.command(Close(First));
    controller.command(Unlisten);
    service_rx(system, firmware, "scratch closed reader");

    // Send the destructive command on the ordinary command secondary path.
    controller.command(Listen(Drive));
    controller.command(Secondary(Command));
    for (const auto byte : std::string{"S0:BASIC"}) {
        controller.data(static_cast<uint8_t>(byte));
    }
    controller.data('\r', Eoi::Last);
    controller.command(Unlisten);
    service_rx(system, firmware, "scratch deletion");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive),
              format == 0 ? "01, FILES SCRATCHED,01,00\r" : "01, FILES SCRATCHED,01,00,0\r");
    controller.command(Talk(Drive));
    controller.command(Secondary(Second));
    service_rx(system, firmware, "scratch retained peer");
    receive_payload(system, firmware, controller, {data(0x42), data(0x43), data(0x44, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "scratch peer UNTALK");
    ASSERT_TRUE(ieee_drive_unmount_all());
    ASSERT_TRUE(ieee_drive_mount(0, "scratch0.img"));
    ASSERT_TRUE(ieee_drive_mount(1, "scratch1.img"));
    open_file(system, controller, First, "0:BASIC,S,R");
    service_rx(system, firmware, "scratch persisted missing OPEN");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive),
              format == 0 ? "62, FILE NOT FOUND,00,00\r" : "62, FILE NOT FOUND,00,00,0\r");
    open_file(system, controller, First, "1:BASIC,S,R");
    service_rx(system, firmware, "scratch persisted peer OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(First));
    service_rx(system, firmware, "scratch persisted peer TALK");
    receive_payload(system, firmware, controller, {data(0x41), data(0x42), data(0x43), data(0x44, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "scratch persisted peer UNTALK");
}

// Internal copy/append channels preserve public GET cursors and survive remount.
TEST_P(DriveLifecycleTest, CopyConcatenatesAcrossDisksAndPersistsAfterAppend) {
    constexpr Device Drive{DeviceAddress};
    constexpr Channel First{DataChannel}, Second{DataChannel + 1}, Command{CommandChannel};
    IeeeController controller(system);
    const int format = GetParam();
    firmware.mount(copy_image(format), "copy0.img");
    firmware.mount(copy_image(format), "copy1.img", 1);
    open_file(system, controller, First, "0:BASIC,S,R");
    service_rx(system, firmware, "copy first OPEN");
    open_file(system, controller, Second, "1:BASIC,S,R");
    service_rx(system, firmware, "copy peer OPEN");
    for (const auto channel : {First, Second}) {
        controller.command(Talk(Drive));
        controller.command(Secondary(channel));
        service_rx(system, firmware, "copy paused source");
        EXPECT_EQ(controller.receive(), data(0x41));
        controller.command(Untalk);
        service_rx(system, firmware, "copy paused UNTALK");
    }
    for (const auto* const request : {"C0:NEW=1:BASIC,0:BASIC", "C0:NEW=0:NEW,0:BASIC", "R0:RENAMED=NEW", "S0:RENAMED",
                                      "C0:PERSIST=1:BASIC,0:BASIC", "C0:PERSIST=0:PERSIST,0:BASIC"}) {
        SCOPED_TRACE(request);
        controller.command(Listen(Drive));
        controller.command(Open(Command));
        const std::string_view bytes{request};
        for (size_t index = 0; index < bytes.size(); ++index) {
            firmware.service();
            controller.data(static_cast<uint8_t>(bytes[index]), index + 1 == bytes.size() ? Eoi::Last : Eoi::More);
        }
        controller.command(Unlisten);
        service_rx(system, firmware, "copy command");
        const bool scratched = request[0] == 'S';
        EXPECT_EQ(receive_status(system, firmware, controller, Drive),
                  std::string(scratched ? "01, FILES SCRATCHED,01,00" : "00, OK,00,00") +
                      (format == 0 ? "\r" : ",0\r"));
    }
    for (const auto channel : {First, Second}) {
        SCOPED_TRACE("copy retained source");
        controller.command(Talk(Drive));
        controller.command(Secondary(channel));
        service_rx(system, firmware, "copy retained cursor");
        receive_payload(system, firmware, controller, {data(0x42), data(0x43), data(0x44, Eoi::Last)});
        controller.command(Untalk);
        service_rx(system, firmware, "copy retained UNTALK");
    }
    ASSERT_TRUE(ieee_drive_unmount_all());
    ASSERT_TRUE(ieee_drive_mount(0, "copy0.img"));
    ASSERT_TRUE(ieee_drive_mount(1, "copy1.img"));
    open_file(system, controller, First, "0:PERSIST,S,R");
    service_rx(system, firmware, "copy persisted OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(First));
    service_rx(system, firmware, "copy persisted TALK");
    receive_payload(system, firmware, controller,
                    {data(0x41), data(0x42), data(0x43), data(0x44), data(0x41), data(0x42), data(0x43), data(0x44),
                     data(0x41), data(0x42), data(0x43), data(0x44, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "copy persisted UNTALK");
}

// Colonless COPY iterates files across disks without replacing paused IEEE sources.
TEST_P(DriveLifecycleTest, CopyAllRetainsPeersAndPersistsEveryCompletedFile) {
    constexpr Device Drive{DeviceAddress}, Other{9};
    constexpr Channel Reader{DataChannel}, Peer{DataChannel + 1}, Command{CommandChannel};
    constexpr size_t EntrySize = 32, TypeOffset = 2, NameOffset = 5, NameLength = 16;
    constexpr uint8_t PayloadTrack = 1;
    const int format = GetParam();
    auto target = copy_image(format), source = copy_image(format);
    const size_t directory = sequential_directory(format != 0);
    target[directory + TypeOffset] = 0;
    ASSERT_NE(source[directory + DirectoryStartTrack], PayloadTrack);
    std::vector<uint8_t> payload(261);
    for (size_t index = 0; index < payload.size(); ++index) {
        payload[index] = index;
    }

    // Encode two additional source files independently of production OPEN/write paths.
    for (size_t file = 0; file < 2; ++file) {
        const size_t entry = directory + (file + 1) * EntrySize;
        source[entry + TypeOffset] = 0x80 | (file == 0 ? DISKIMAGE_FTYPE_PRG : DISKIMAGE_FTYPE_USR);
        source[entry + DirectoryStartTrack] = PayloadTrack;
        source[entry + DirectoryStartSector] = file == 0 ? 0 : 2;
        std::fill_n(source.begin() + entry + NameOffset, NameLength, 0xa0);
        const std::string_view name = file == 0 ? "PAGED" : "ONE";
        std::copy(name.begin(), name.end(), source.begin() + entry + NameOffset);
        source[entry + 30] = file == 0 ? 2 : 1;
    }
    source[0] = PayloadTrack;
    source[1] = 1;
    std::copy_n(payload.begin(), DISKIMAGE_SECTOR_PAYLOAD_SIZE, source.begin() + DISKIMAGE_SECTOR_LINK_SIZE);
    source[DISKIMAGE_SECTOR_SIZE] = 0;
    source[DISKIMAGE_SECTOR_SIZE + 1] = payload.size() - DISKIMAGE_SECTOR_PAYLOAD_SIZE + 1;
    std::copy(payload.begin() + DISKIMAGE_SECTOR_PAYLOAD_SIZE, payload.end(),
              source.begin() + DISKIMAGE_SECTOR_SIZE + DISKIMAGE_SECTOR_LINK_SIZE);
    source[2 * DISKIMAGE_SECTOR_SIZE] = 0;
    source[2 * DISKIMAGE_SECTOR_SIZE + 1] = DISKIMAGE_SECTOR_LINK_SIZE;
    source[2 * DISKIMAGE_SECTOR_SIZE + DISKIMAGE_SECTOR_LINK_SIZE] = 'A';
    if (format == 0) {
        const size_t bam = D64::offset(18, 0);
        constexpr size_t TrackOneCount = 4, TrackOneBitmap = 5;
        source[bam + TrackOneCount] -= 3;
        source[bam + TrackOneBitmap] &= ~7u;
    }
    firmware.mount(target, "copy-all-target.img");
    firmware.mount(source, "copy-all-source.img", 1);
    firmware.mount(source, "copy-all-other.img", 2);
    IeeeController controller(system);

    // Pause local drive-1 and independent-unit readers before issuing COPY on unit 8.
    for (const auto device : {Drive, Other}) {
        open_file(system, controller, Peer, device.value() == Drive.value() ? "1:BASIC,S,R" : "0:BASIC,S,R", device);
        service_rx(system, firmware, "copy-all peer OPEN");
        controller.command(Talk(device));
        controller.command(Secondary(Peer));
        service_rx(system, firmware, "copy-all peer TALK");
        receive_payload(system, firmware, controller, {data(0x41)});
        controller.command(Untalk);
        service_rx(system, firmware, "copy-all peer UNTALK");
    }
    for (const std::string_view request : {"C0=0", "C2=1", "CFOO=ONE", "C0=1", "COPY0=1TAIL"}) {
        SCOPED_TRACE(request);
        if (request == "COPY0=1TAIL") {
            controller.command(Listen(Drive));
            controller.command(Secondary(Command));
            for (size_t index = 0; index < request.size(); ++index) {
                controller.data(request[index], index + 1 == request.size() ? Eoi::Last : Eoi::More);
            }
            controller.command(Unlisten);
        } else {
            open_file(system, controller, Command, request);
        }
        service_rx(system, firmware, "copy-all command");
        const std::string status = request == "C0=1"          ? "00, OK,00,00"
                                   : request == "COPY0=1TAIL" ? "63, FILE EXISTS,00,00"
                                                              : "30,SYNTAX ERROR,00,00";
        EXPECT_EQ(receive_status(system, firmware, controller, Drive), status + (format == 0                ? "\r"
                                                                                 : request == "COPY0=1TAIL" ? ",0\r"
                                                                                                            : ",1\r"));
    }
    for (const auto device : {Drive, Other}) {
        controller.command(Talk(device));
        controller.command(Secondary(Peer));
        service_rx(system, firmware, "copy-all retained peer TALK");
        receive_payload(system, firmware, controller, {data(0x42), data(0x43), data(0x44, Eoi::Last)});
        controller.command(Untalk);
        service_rx(system, firmware, "copy-all retained peer UNTALK");
    }

    // Actual remount must expose all payloads and native singleton/EOI behavior.
    ASSERT_TRUE(ieee_drive_unmount_all());
    ASSERT_TRUE(ieee_drive_mount(0, "copy-all-target.img"));
    ASSERT_TRUE(ieee_drive_mount(1, "copy-all-source.img"));
    for (const auto name : {"0:PAGED,P,R", "1:PAGED,P,R"}) {
        open_file(system, controller, Reader, name);
        service_rx(system, firmware, "copy-all remounted paged OPEN");
        controller.command(Talk(Drive));
        controller.command(Secondary(Reader));
        service_rx(system, firmware, "copy-all remounted paged TALK");
        std::vector<Record> expected;
        for (size_t index = 0; index < payload.size(); ++index) {
            expected.push_back(data(payload[index], index + 1 == payload.size() ? Eoi::Last : Eoi::More));
        }
        receive_payload(system, firmware, controller, expected);
        controller.command(Untalk);
        service_rx(system, firmware, "copy-all remounted paged UNTALK");
        controller.command(Listen(Drive));
        controller.command(Close(Reader));
        controller.command(Unlisten);
        service_rx(system, firmware, "copy-all remounted paged CLOSE");
    }
    open_file(system, controller, Reader, "0:ONE,U,R");
    service_rx(system, firmware, "copy-all remounted USR OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(Reader));
    service_rx(system, firmware, "copy-all remounted USR TALK");
    receive_payload(system, firmware, controller, {data('A'), data(0), data(2), data('A', Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "copy-all remounted USR UNTALK");
}

// CPYDTD filters source names, preserves peers and keeps model-specific drive checks.
TEST_P(DriveLifecycleTest, PatternCopyRetainsPeersAndPersistsOnlySelectedFiles) {
    constexpr Device Drive{DeviceAddress}, Other{9};
    constexpr Channel Reader{DataChannel}, Peer{DataChannel + 1}, Command{CommandChannel};
    constexpr size_t EntrySize = 32, TypeOffset = 2, NameOffset = 5, NameLength = 16;
    constexpr uint8_t PayloadTrack = 1;
    const int format = GetParam();
    auto source = copy_image(format);
    const size_t directory = sequential_directory(format != 0), entry = directory + EntrySize;
    ASSERT_NE(source[directory + DirectoryStartTrack], PayloadTrack);

    // Encode the selectable USR file independently of the production writer.
    source[entry + TypeOffset] = 0x80 | DISKIMAGE_FTYPE_USR;
    source[entry + DirectoryStartTrack] = PayloadTrack;
    source[entry + DirectoryStartSector] = 0;
    std::fill_n(source.begin() + entry + NameOffset, NameLength, 0xa0);
    const std::string_view name = "ONE";
    std::copy(name.begin(), name.end(), source.begin() + entry + NameOffset);
    source[entry + 30] = 1;
    source[0] = 0;
    source[1] = 3;
    source[2] = 0x60;
    source[3] = 0x61;
    if (format == 0) {
        const size_t bam = D64::offset(18, 0);
        constexpr size_t TrackOneCount = 4, TrackOneBitmap = 5;
        --source[bam + TrackOneCount];
        source[bam + TrackOneBitmap] &= ~1u;
    }
    firmware.mount(copy_image(format), "pattern-target.img");
    firmware.mount(source, "pattern-source.img", 1);
    firmware.mount(copy_image(format), "pattern-other.img", 2);
    IeeeController controller(system);

    // An existing target BASIC must not prevent copying the selected ONE source.
    for (const auto device : {Drive, Other}) {
        open_file(system, controller, Peer, device.value() == Drive.value() ? "1:BASIC,S,R" : "0:BASIC,S,R", device);
        service_rx(system, firmware, "pattern peer OPEN");
        controller.command(Talk(device));
        controller.command(Secondary(Peer));
        service_rx(system, firmware, "pattern peer TALK");
        receive_payload(system, firmware, controller, {data(0x41)});
        controller.command(Untalk);
        service_rx(system, firmware, "pattern peer UNTALK");
    }
    const std::array<std::pair<std::string_view, std::string_view>, 6> commands{{
        {"C0:BAD*=1:BASIC", "30,SYNTAX ERROR,00,00"},
        {"C1:*=1:BASIC", format == 0 ? "63, FILE EXISTS,00,00" : "30,SYNTAX ERROR,00,00"},
        {"C0:*=1:MISS?", "00, OK,00,00"},
        {"C0:*TAIL=1:O?E", "00, OK,00,00"},
        {format == 0 ? "C0:*=1:ONE*IGNORED" : "C0:ONE=1:ONE", "63, FILE EXISTS,00,00"},
        {"C0:AFTER=1:ONE", "00, OK,00,00"},
    }};
    for (size_t index = 0; index < commands.size(); ++index) {
        const auto [request, expected] = commands[index];
        SCOPED_TRACE(request);
        controller.command(Listen(Drive));
        controller.command(index % 2 ? Secondary(Command) : Open(Command));
        for (size_t byte = 0; byte < request.size(); ++byte) {
            firmware.service();
            controller.data(request[byte], byte + 1 == request.size() ? Eoi::Last : Eoi::More);
        }
        controller.command(Unlisten);
        service_rx(system, firmware, "pattern command");
        EXPECT_EQ(receive_status(system, firmware, controller, Drive), std::string(expected) + (format == 0 ? "\r"
                                                                                                : index < 5 ? ",1\r"
                                                                                                            : ",0\r"));
    }
    for (const auto device : {Drive, Other}) {
        controller.command(Talk(device));
        controller.command(Secondary(Peer));
        service_rx(system, firmware, "pattern retained peer TALK");
        receive_payload(system, firmware, controller, {data(0x42), data(0x43), data(0x44, Eoi::Last)});
        controller.command(Untalk);
        service_rx(system, firmware, "pattern retained peer UNTALK");
    }

    // Remount both real backing files and read copied, recovered and preserved data.
    ASSERT_TRUE(ieee_drive_unmount_all());
    ASSERT_TRUE(ieee_drive_mount(0, "pattern-target.img"));
    ASSERT_TRUE(ieee_drive_mount(1, "pattern-source.img"));
    for (const auto filename : {"0:ONE,U,R", "0:AFTER,U,R", "1:ONE,U,R", "0:BASIC,S,R"}) {
        SCOPED_TRACE(filename);
        open_file(system, controller, Reader, filename);
        service_rx(system, firmware, "pattern remounted OPEN");
        controller.command(Talk(Drive));
        controller.command(Secondary(Reader));
        service_rx(system, firmware, "pattern remounted TALK");
        receive_payload(system, firmware, controller,
                        filename == std::string_view("0:BASIC,S,R")
                            ? std::vector<Record>{data(0x41), data(0x42), data(0x43), data(0x44, Eoi::Last)}
                            : std::vector<Record>{data(0x60), data(0x61, Eoi::Last)});
        controller.command(Untalk);
        service_rx(system, firmware, "pattern remounted UNTALK");
        controller.command(Listen(Drive));
        controller.command(Close(Reader));
        controller.command(Unlisten);
        service_rx(system, firmware, "pattern remounted CLOSE");
    }
}

// Validate abandons selected writers, preserves the peer and persists rebuilt maps.
TEST_P(DriveLifecycleTest, ValidateRebuildsMapsAndAbandonsOnlySelectedDisk) {
    constexpr Device Drive{DeviceAddress};
    constexpr Channel First{DataChannel}, Second{DataChannel + 1}, Writer{DataChannel + 2}, Command{CommandChannel};
    IeeeController controller(system);
    const int format = GetParam();
    firmware.mount(copy_image(format), "validate0.img");
    firmware.mount(copy_image(format), "validate1.img", 1);
    open_file(system, controller, First, "0:BASIC,S,R");
    service_rx(system, firmware, "validate selected OPEN");
    open_file(system, controller, Second, "1:BASIC,S,R");
    service_rx(system, firmware, "validate peer OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(Second));
    service_rx(system, firmware, "validate peer prefix");
    EXPECT_EQ(controller.receive(), data(0x41));
    controller.command(Untalk);
    service_rx(system, firmware, "validate peer UNTALK");
    open_file(system, controller, Writer, "0:UNCLOSED,S,W");
    service_rx(system, firmware, "validate writer OPEN");
    controller.command(Listen(Drive));
    controller.command(Secondary(Writer));
    controller.data(0x60, Eoi::Last);
    controller.command(Unlisten);
    service_rx(system, firmware, "validate unfinished writer");
    for (const auto* const request : {"V0", "VALIDATE0", "C0:NEW=0:BASIC", "V0:IGNORED"}) {
        SCOPED_TRACE(request);
        controller.command(Listen(Drive));
        controller.command(Open(Command));
        const std::string_view bytes{request};
        for (size_t index = 0; index < bytes.size(); ++index) {
            firmware.service();
            controller.data(static_cast<uint8_t>(bytes[index]), index + 1 == bytes.size() ? Eoi::Last : Eoi::More);
        }
        controller.command(Unlisten);
        service_rx(system, firmware, "validate command");
        EXPECT_EQ(receive_status(system, firmware, controller, Drive),
                  format == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r");
    }
    controller.command(Talk(Drive));
    controller.command(Secondary(Second));
    service_rx(system, firmware, "validate retained peer");
    receive_payload(system, firmware, controller, {data(0x42), data(0x43), data(0x44, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "validate retained UNTALK");
    ASSERT_TRUE(ieee_drive_unmount_all());
    ASSERT_TRUE(ieee_drive_mount(0, "validate0.img"));
    ASSERT_TRUE(ieee_drive_mount(1, "validate1.img"));
    open_file(system, controller, First, "0:UNCLOSED,S,R");
    service_rx(system, firmware, "validate removed splat OPEN");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive),
              format == 0 ? "62, FILE NOT FOUND,00,00\r" : "62, FILE NOT FOUND,00,00,0\r");
    open_file(system, controller, First, "0:NEW,S,R");
    service_rx(system, firmware, "validate persisted OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(First));
    service_rx(system, firmware, "validate persisted TALK");
    receive_payload(system, firmware, controller, {data(0x41), data(0x42), data(0x43), data(0x44, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "validate persisted UNTALK");
}

// NEW's quick/full paths must expose exact empty listings after a real remount.
TEST_P(DriveLifecycleTest, QuickAndFullFormatPersistNativeHeadersAndFreeCounts) {
    constexpr Device Drive{DeviceAddress};
    constexpr Channel Directory{LoadChannel}, Command{CommandChannel};
    constexpr size_t FormatRecoveryTurns = 128;
    constexpr size_t DirectoryRecordSize = 32;
    IeeeController controller(system);
    for (const bool full : {false, true}) {
        SCOPED_TRACE(full);
        const auto initial = copy_image(GetParam());
        const char* const name = full ? "format-full.img" : "format-quick.img";
        firmware.mount(initial, name);
        const std::string request = full ? "N0:M4 FORMATTED,AB" : "N0:M4 FORMATTED";

        // Collect the destructive command through the production fabric path.
        controller.command(Listen(Drive));
        controller.command(full ? Open(Command) : Secondary(Command));
        for (size_t index = 0; index < request.size(); ++index) {
            firmware.service();
            controller.data(request[index], index + 1 == request.size() ? Eoi::Last : Eoi::More);
        }
        controller.command(Unlisten);
        service_rx(system, firmware, "format dispatch");
        const size_t turns = initial.size() / DISKIMAGE_SECTOR_SIZE + FormatRecoveryTurns;
        for (size_t turn = 0; turn < turns; ++turn) {
            firmware.service();
        }
        EXPECT_EQ(receive_status(system, firmware, controller, Drive),
                  GetParam() == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r");
        ASSERT_TRUE(ieee_drive_unmount_all());
        ASSERT_TRUE(ieee_drive_mount(0, name));

        // Independently encode the entire empty BASIC directory, including EOI.
        std::vector<uint8_t> listing(64);
        listing[0] = 1;
        listing[1] = 4;
        listing[2] = listing[3] = 1;
        listing[6] = 0x12;
        listing[7] = '"';
        std::fill_n(listing.begin() + 8, 16, ' ');
        const std::string label = "M4 FORMATTED";
        std::copy(label.begin(), label.end(), listing.begin() + 8);
        listing[24] = '"';
        listing[25] = listing[28] = ' ';
        const size_t header =
            GetParam() == 0 ? D64::offset(18, 0) : (D80DirectoryTrack - 1) * D80SectorsPerTrack * DISKIMAGE_SECTOR_SIZE;
        const size_t label_offset = GetParam() == 0 ? 144 : 6;
        listing[26] = full ? 'A' : initial.at(header + label_offset + 18);
        listing[27] = full ? 'B' : initial.at(header + label_offset + 19);
        listing[29] = '2';
        listing[30] = GetParam() == 0 ? 'A' : 'C';
        listing[32] = listing[33] = 1;
        const unsigned int available = GetParam() == 0 ? 664 : GetParam() == 1 ? 2052 : 4133;
        listing[34] = available;
        listing[35] = available >> 8u;
        std::fill_n(listing.begin() + 36, 25, ' ');
        const std::string footer = "BLOCKS FREE.";
        std::copy(footer.begin(), footer.end(), listing.begin() + 36);
        std::vector<Record> expected;
        for (size_t index = 0; index < listing.size(); ++index) {
            expected.push_back(data(listing[index], index + 1 == listing.size() ? Eoi::Last : Eoi::More));
        }
        open_file(system, controller, Directory, "$0");
        service_rx(system, firmware, "formatted remounted directory");
        controller.command(Talk(Drive));
        controller.command(Secondary(Directory));
        service_rx(system, firmware, "formatted directory TALK");
        receive_payload(system, firmware, controller, {expected.begin(), expected.begin() + DirectoryRecordSize});
        // Empty-directory scanning yields once between its header and free-block row.
        firmware.service();
        receive_payload(system, firmware, controller, {expected.begin() + DirectoryRecordSize, expected.end()});
        controller.command(Untalk);
        service_rx(system, firmware, "formatted directory UNTALK");
        ASSERT_TRUE(ieee_drive_unmount_all());
    }
}

// DUPLICATE retains source data and an independently addressed reader across remount.
TEST_P(DriveLifecycleTest, DuplicateCopiesRawSectorsWithoutResettingOtherUnits) {
    constexpr Device Drive{DeviceAddress}, Other{9};
    constexpr Channel Reader{DataChannel}, Command{CommandChannel};
    constexpr size_t DuplicateRecoveryTurns = 128;
    const auto source = copy_image(GetParam());
    auto target = source;
    std::fill_n(target.begin(), DISKIMAGE_SECTOR_SIZE, 0xa5);
    firmware.mount(target, "duplicate-target.img");
    firmware.mount(source, "duplicate-source.img", 1);
    firmware.mount(source, "duplicate-other.img", 2);
    IeeeController controller(system);
    open_file(system, controller, Reader, "0:BASIC,S,R", Other);
    service_rx(system, firmware, "duplicate other OPEN");
    controller.command(Talk(Other));
    controller.command(Secondary(Reader));
    service_rx(system, firmware, "duplicate other prefix");
    receive_payload(system, firmware, controller, {data('A')});
    controller.command(Untalk);
    service_rx(system, firmware, "duplicate other UNTALK");

    // Let every cooperative read/write group finish before addressing status.
    const std::string_view request = "D0=1";
    controller.command(Listen(Drive));
    controller.command(Open(Command));
    for (size_t index = 0; index < request.size(); ++index) {
        firmware.service();
        controller.data(request[index], index + 1 == request.size() ? Eoi::Last : Eoi::More);
    }
    controller.command(Unlisten);
    service_rx(system, firmware, "duplicate dispatch");
    const size_t turns = 3u * (source.size() / DISKIMAGE_SECTOR_SIZE) + DuplicateRecoveryTurns;
    for (size_t turn = 0; turn < turns; ++turn) {
        firmware.service();
    }
    EXPECT_EQ(receive_status(system, firmware, controller, Drive),
              GetParam() == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r");
    controller.command(Talk(Other));
    controller.command(Secondary(Reader));
    service_rx(system, firmware, "duplicate retained other");
    receive_payload(system, firmware, controller, {data('B'), data('C'), data('D', Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "duplicate retained other UNTALK");

    // Both persisted local images remain readable after their real handle teardown.
    ASSERT_TRUE(ieee_drive_unmount_all());
    ASSERT_TRUE(ieee_drive_mount(0, "duplicate-target.img"));
    ASSERT_TRUE(ieee_drive_mount(1, "duplicate-source.img"));
    for (const auto* const name : {"0:BASIC,S,R", "1:BASIC,S,R"}) {
        open_file(system, controller, Reader, name);
        service_rx(system, firmware, "duplicate remounted OPEN");
        controller.command(Talk(Drive));
        controller.command(Secondary(Reader));
        service_rx(system, firmware, "duplicate remounted TALK");
        receive_payload(system, firmware, controller, {data('A'), data('B'), data('C'), data('D', Eoi::Last)});
        controller.command(Untalk);
        service_rx(system, firmware, "duplicate remounted UNTALK");
    }
}

// Direct LOAD uses sequential prepared-byte GET, while direct SAVE uses circular GET.
TEST_P(DriveLifecycleTest, DirectLoadAndSaveRetainTheirNativePreparedBytes) {
    constexpr Device Drive{DeviceAddress};
    constexpr uint8_t FirstBuffer = DOS_BUFFER_COUNT - 1;
    firmware.mount(copy_image(GetParam()), "direct-special.img");
    IeeeController controller(system);
    for (const uint8_t secondary : {IEEE_LOAD_CHANNEL, IEEE_SAVE_CHANNEL}) {
        const Channel direct{secondary};
        open_file(system, controller, direct, "#");
        service_rx(system, firmware, "direct special OPEN");
        controller.command(Talk(Drive));
        controller.command(Secondary(direct));
        service_rx(system, firmware, "direct special number");
        receive_payload(system, firmware, controller, {data(FirstBuffer)});
        controller.command(Untalk);
        service_rx(system, firmware, "direct special number UNTALK");

        // Cross the page boundary through PUT, then observe its sticky EOI.
        controller.command(Listen(Drive));
        controller.command(Secondary(direct));
        for (size_t index = 0; index < DISKIMAGE_SECTOR_SIZE; ++index) {
            firmware.service();
            controller.data(index, index + 1 == DISKIMAGE_SECTOR_SIZE ? Eoi::Last : Eoi::More);
        }
        controller.command(Unlisten);
        service_rx(system, firmware, "direct special PUT");
        controller.command(Talk(Drive));
        controller.command(Secondary(direct));
        service_rx(system, firmware, "direct special GET");
        receive_payload(system, firmware, controller, {data(0, Eoi::Last)});
        std::vector<Record> tail;
        if (secondary == LoadChannel) {
            tail.push_back(data(1));
        }
        for (unsigned int value = 1; value < 254; ++value) {
            tail.push_back(data(value, value == 253 ? Eoi::Last : Eoi::More));
        }
        receive_payload(system, firmware, controller, tail);
        controller.command(Untalk);
        service_rx(system, firmware, "direct special UNTALK");
        controller.command(Listen(Drive));
        controller.command(Close(direct));
        controller.command(Unlisten);
        service_rx(system, firmware, "direct special CLOSE");
    }
}

// Direct RAM channels wrap after EOI without losing peer cursors or writing a file.
TEST_P(DriveLifecycleTest, DirectBufferWrapRetainsPeerAndSurvivesRepeatedEoi) {
    constexpr Device Drive{DeviceAddress};
    constexpr Channel Direct{DataChannel}, Peer{DataChannel + 1}, Command{CommandChannel};
    constexpr uint8_t FirstBuffer = DOS_BUFFER_COUNT - 1;
    IeeeController controller(system);
    firmware.mount(copy_image(GetParam()), "direct0.img");
    firmware.mount(copy_image(GetParam()), "direct1.img", 1);

    // Consume the binary page number before PUT shares and advances its pointer.
    open_file(system, controller, Direct, "#");
    service_rx(system, firmware, "direct OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(Direct));
    service_rx(system, firmware, "direct page number");
    receive_payload(system, firmware, controller, {data(FirstBuffer)});
    controller.command(Untalk);
    service_rx(system, firmware, "direct number UNTALK");
    open_file(system, controller, Peer, "1:BASIC,S,R");
    service_rx(system, firmware, "direct peer OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(Peer));
    service_rx(system, firmware, "direct peer prefix");
    receive_payload(system, firmware, controller, {data(0x41)});
    controller.command(Untalk);
    service_rx(system, firmware, "direct peer UNTALK");

    // Fill all 256 raw offsets. PUT retains EOI when its pointer passes 255.
    controller.command(Listen(Drive));
    controller.command(Secondary(Direct));
    for (size_t index = 0; index < DISKIMAGE_SECTOR_SIZE; ++index) {
        firmware.service();
        controller.data(static_cast<uint8_t>(index), index + 1 == DISKIMAGE_SECTOR_SIZE ? Eoi::Last : Eoi::More);
    }
    controller.command(Unlisten);
    service_rx(system, firmware, "direct PUT wrap");
    controller.command(Talk(Drive));
    controller.command(Secondary(Direct));
    service_rx(system, firmware, "direct sticky EOI");
    receive_payload(system, firmware, controller, {data(0, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "direct first EOI UNTALK");

    // Readdressing after EOI resumes the same raw page, skipping offset zero.
    std::vector<Record> tail, repeated;
    for (size_t index = 1; index < DISKIMAGE_SECTOR_SIZE - 2; ++index) {
        tail.push_back(data(static_cast<uint8_t>(index), index == DISKIMAGE_SECTOR_SIZE - 3 ? Eoi::Last : Eoi::More));
    }
    for (size_t index = 0; index < DISKIMAGE_SECTOR_SIZE - 1; ++index) {
        repeated.push_back(
            data(static_cast<uint8_t>(index - 1), index == DISKIMAGE_SECTOR_SIZE - 2 ? Eoi::Last : Eoi::More));
    }
    for (const auto* const expected : {&tail, &repeated, &repeated}) {
        controller.command(Talk(Drive));
        controller.command(Secondary(Direct));
        service_rx(system, firmware, "direct circular TALK");
        receive_payload(system, firmware, controller, *expected);
        controller.command(Untalk);
        service_rx(system, firmware, "direct circular UNTALK");
    }
    controller.command(Talk(Drive));
    controller.command(Secondary(Peer));
    service_rx(system, firmware, "direct retained peer");
    receive_payload(system, firmware, controller, {data(0x42), data(0x43), data(0x44, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "direct retained UNTALK");

    // INITIALIZE retires only the page's selected drive before it is reopened.
    controller.command(Listen(Drive));
    controller.command(Open(Command));
    controller.data('I');
    firmware.service();
    controller.data('0', Eoi::Last);
    controller.command(Unlisten);
    service_rx(system, firmware, "direct initialize");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive),
              GetParam() == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r");
    open_file(system, controller, Direct, "#11");
    service_rx(system, firmware, "direct page reuse");
    controller.command(Talk(Drive));
    controller.command(Secondary(Direct));
    service_rx(system, firmware, "direct reused page number");
    receive_payload(system, firmware, controller, {data(FirstBuffer)});
    controller.command(Untalk);
    service_rx(system, firmware, "direct reuse UNTALK");
    ASSERT_TRUE(ieee_drive_unmount_all());
    ASSERT_TRUE(ieee_drive_mount(0, "direct0.img"));
    open_file(system, controller, Direct, "0:BASIC,S,R");
    service_rx(system, firmware, "direct persisted image OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(Direct));
    service_rx(system, firmware, "direct persisted image TALK");
    receive_payload(system, firmware, controller, {data(0x41), data(0x42), data(0x43), data(0x44, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "direct remount UNTALK");
}

// B-P retires stale FIFO previews, preserves sticky EOI and does not move a peer reader.
TEST_P(DriveLifecycleTest, BufferPointerReplacesStagedBytesAndRetainsPeer) {
    constexpr Device Drive{DeviceAddress};
    constexpr Channel Direct{DataChannel}, Peer{DataChannel + 1}, Command{CommandChannel};
    IeeeController controller(system);
    firmware.mount(copy_image(GetParam()), "pointer0.img");
    firmware.mount(copy_image(GetParam()), "pointer1.img", 1);
    open_file(system, controller, Direct, "#");
    service_rx(system, firmware, "pointer direct OPEN");
    open_file(system, controller, Peer, "1:BASIC,S,R");
    service_rx(system, firmware, "pointer peer OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(Peer));
    service_rx(system, firmware, "pointer peer prefix");
    receive_payload(system, firmware, controller, {data(0x41)});
    controller.command(Untalk);
    service_rx(system, firmware, "pointer peer UNTALK");

    // Seed the offset selected by ASCHEX's retained hundreds digit.
    constexpr uint8_t RetainedByte = 0xa5;
    open_file(system, controller, Command, "B-P:2,200");
    service_rx(system, firmware, "pointer retained digit seed");
    controller.command(Listen(Drive));
    controller.command(Secondary(Direct));
    controller.data(RetainedByte, Eoi::Last);
    controller.command(Unlisten);
    service_rx(system, firmware, "pointer retained digit PUT");

    // Write known raw offsets, including zero, without publishing a disk file.
    open_file(system, controller, Command, "B-P:2,0");
    service_rx(system, firmware, "pointer initial command");
    controller.command(Listen(Drive));
    controller.command(Secondary(Direct));
    constexpr uint8_t RawByteCount = 16;
    for (uint8_t value = 0; value < RawByteCount; ++value) {
        firmware.service();
        controller.data(value, value + 1 == RawByteCount ? Eoi::Last : Eoi::More);
    }
    controller.command(Unlisten);
    service_rx(system, firmware, "pointer raw PUT");

    // A partial GET leaves queued bytes which must be replaced by each B-P.
    struct PointerStep {
        std::string_view command;
        std::vector<Record> bytes;
    };
    const PointerStep steps[] = {
        {"B-P:258,A", {data(RetainedByte)}}, {"B-P:2,0", {data(0)}}, {"B-P:2,A", {data(0)}},
        {"B-P:2,13", {data(13), data(14)}},  {"B-P:2,255", {}},      {"B-P:2,0", {data(0, Eoi::Last)}},
        {"", {data(1), data(2), data(3)}},
    };
    for (const auto& step : steps) {
        SCOPED_TRACE(step.command);
        if (!step.command.empty()) {
            open_file(system, controller, Command, step.command);
            service_rx(system, firmware, "pointer command");
            EXPECT_EQ(receive_status(system, firmware, controller, Drive),
                      GetParam() == 0 ? "00, OK,00,00\r" : "00, OK,00,00,1\r");
        }
        if (!step.bytes.empty()) {
            controller.command(Talk(Drive));
            controller.command(Secondary(Direct));
            service_rx(system, firmware, "pointer TALK");
            receive_payload(system, firmware, controller, step.bytes);
            controller.command(Untalk);
            service_rx(system, firmware, "pointer UNTALK");
        }
    }

    // Syntax failure preserves the direct cursor and the independently paused peer.
    open_file(system, controller, Command, "B-P:2,17,0,0,0");
    service_rx(system, firmware, "pointer rejected command");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive),
              GetParam() == 0 ? "30,SYNTAX ERROR,00,00\r" : "30,SYNTAX ERROR,00,00,1\r");
    for (const auto selected : {Direct, Peer}) {
        controller.command(Talk(Drive));
        controller.command(Secondary(selected));
        service_rx(system, firmware, "pointer retained TALK");
        receive_payload(system, firmware, controller,
                        selected.value() == Direct.value()
                            ? std::vector<Record>{data(4)}
                            : std::vector<Record>{data(0x42), data(0x43), data(0x44, Eoi::Last)});
        controller.command(Untalk);
        service_rx(system, firmware, "pointer retained UNTALK");
    }
}

// Block jobs replace staged direct bytes, reset EOI and preserve a paused drive-1 reader.
TEST_P(DriveLifecycleTest, BlockReadsReplaceStagedBytesAndRetainPeer) {
    constexpr Device Drive{DeviceAddress};
    constexpr Channel Direct{DataChannel}, Peer{DataChannel + 1}, Command{CommandChannel};
    IeeeController controller(system);
    auto image = copy_image(GetParam());
    std::array<uint8_t, DISKIMAGE_SECTOR_SIZE> page{};
    for (size_t offset = 0; offset < page.size(); ++offset) {
        page[offset] = static_cast<uint8_t>(offset);
    }
    page[0] = 4;
    page[1] = 13;
    page[2] = 0;
    page[3] = 255;
    page[4] = 42;
    page.back() = 99;
    std::copy(page.begin(), page.end(), image.begin());
    firmware.mount(image, "block-read0.img");
    firmware.mount(image, "block-read1.img", 1);
    open_file(system, controller, Direct, "#");
    service_rx(system, firmware, "block direct OPEN");
    open_file(system, controller, Peer, "1:BASIC,S,R");
    service_rx(system, firmware, "block peer OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(Peer));
    service_rx(system, firmware, "block peer prefix");
    receive_payload(system, firmware, controller, {data(0x41)});
    controller.command(Untalk);
    service_rx(system, firmware, "block peer UNTALK");

    // Prime stale direct EOI before reads replace both the page and its prepared byte.
    open_file(system, controller, Command, "B-P:2,255");
    service_rx(system, firmware, "block stale EOI");
    std::vector<Record> raw;
    for (size_t offset = 0; offset < page.size(); ++offset) {
        raw.push_back(data(page[offset], offset + 1 == page.size() ? Eoi::Last : Eoi::More));
    }
    struct ReadStep {
        std::string_view command;
        std::vector<Record> bytes;
    };
    const ReadStep steps[] = {
        {"B-R:2,0,1,0", {data(13), data(0), data(255), data(42, Eoi::Last)}},
        {"", {data(13), data(0), data(255), data(42, Eoi::Last)}},
        {"U1:2,0,1,0", raw},
        {"", {data(13), data(0), data(255), data(42)}},
        {"UA:2,0,1,0", {data(4), data(13), data(0), data(255)}},
    };
    for (const auto& step : steps) {
        SCOPED_TRACE(step.command);
        if (!step.command.empty()) {
            open_file(system, controller, Command, step.command);
            service_rx(system, firmware, "block read command");
            EXPECT_EQ(receive_status(system, firmware, controller, Drive),
                      GetParam() == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r");
        }
        controller.command(Talk(Drive));
        controller.command(Secondary(Direct));
        service_rx(system, firmware, "block read TALK");
        receive_payload(system, firmware, controller, step.bytes);
        controller.command(Untalk);
        service_rx(system, firmware, "block read UNTALK");
    }

    // Rejected geometry and absent-channel jobs leave both source cursors untouched.
    open_file(system, controller, Command, "B-R:2,0,255,255");
    service_rx(system, firmware, "block invalid geometry");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive),
              GetParam() == 0 ? "66,ILLEGAL TRACK OR SECTOR,55,55\r" : "66,ILLEGAL TRACK OR SECTOR,255,255,0\r");
    open_file(system, controller, Command, "U!:9,0,1,0");
    service_rx(system, firmware, "block absent channel");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive),
              GetParam() == 0 ? "70,NO CHANNEL,00,00\r" : "70,NO CHANNEL,00,00,0\r");
    for (const auto selected : {Direct, Peer}) {
        controller.command(Talk(Drive));
        controller.command(Secondary(selected));
        service_rx(system, firmware, "block retained TALK");
        receive_payload(system, firmware, controller,
                        selected.value() == Direct.value()
                            ? std::vector<Record>{data(42)}
                            : std::vector<Record>{data(0x42), data(0x43), data(0x44, Eoi::Last)});
        controller.command(Untalk);
        service_rx(system, firmware, "block retained UNTALK");
    }
}

// Counted and raw block writes retain channel semantics and persist their entire raw page.
TEST_P(DriveLifecycleTest, BlockWritesPreserveStateAndPersistAfterRemount) {
    constexpr Device Drive{DeviceAddress};
    constexpr Channel Direct{DataChannel}, Peer{DataChannel + 1}, Command{CommandChannel};
    IeeeController controller(system);
    auto image = copy_image(GetParam());
    std::array<uint8_t, DISKIMAGE_SECTOR_SIZE> page{};
    for (size_t offset = 0; offset < page.size(); ++offset) {
        page[offset] = static_cast<uint8_t>(offset);
    }
    page[0] = 4;
    page[1] = 13;
    page[2] = 0;
    page[3] = 255;
    page[4] = 42;
    page.back() = 99;
    std::copy(page.begin(), page.end(), image.begin());
    std::fill_n(image.begin() + DISKIMAGE_SECTOR_SIZE, DISKIMAGE_SECTOR_SIZE, 0);
    firmware.mount(image, "block-write0.img");
    firmware.mount(image, "block-write1.img", 1);
    open_file(system, controller, Direct, "#");
    service_rx(system, firmware, "block write direct OPEN");
    open_file(system, controller, Peer, "1:BASIC,S,R");
    service_rx(system, firmware, "block write peer OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(Peer));
    service_rx(system, firmware, "block write peer prefix");
    receive_payload(system, firmware, controller, {data(0x41)});
    controller.command(Untalk);
    service_rx(system, firmware, "block write peer UNTALK");

    // Load the source, then copy to a distinct sector without changing LSTCHR.
    struct WriteStep {
        std::string_view command;
        std::vector<Record> bytes;
    };
    const WriteStep steps[] = {
        {"U1:2,0,1,0", {}},
        {"B-P:2,5", {}},
        {"B-W:2,0,1,1", {data(13), data(0), data(255), data(42)}},
        {"B-R:2,0,1,1", {data(13), data(0), data(255), data(42, Eoi::Last)}},
        {"B-P:2,4", {}},
        {"B-P:2,0", {}},
        {"U2:2,0,1,1", {data(4, Eoi::Last)}},
        {"UB:2,0,1,1", {}},
        {"U1:2,0,1,1", {data(4)}},
        {"B-P:2,0", {}},
        {"B-W:2,0,1,1", {data(13)}},
        {"B-R:2,0,1,1", {data(13, Eoi::Last)}},
        {"B-P:2,255", {}},
        {"B-W:2,0,1,1", {}},
        {"UB:2,0,1,1", {data(13, Eoi::Last)}},
    };
    for (const auto& step : steps) {
        SCOPED_TRACE(step.command);
        open_file(system, controller, Command, step.command);
        service_rx(system, firmware, "block write command");
        EXPECT_EQ(receive_status(system, firmware, controller, Drive),
                  GetParam() == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r");
        if (!step.bytes.empty()) {
            controller.command(Talk(Drive));
            controller.command(Secondary(Direct));
            service_rx(system, firmware, "block write TALK");
            receive_payload(system, firmware, controller, step.bytes);
            controller.command(Untalk);
            service_rx(system, firmware, "block write UNTALK");
        }
    }
    controller.command(Talk(Drive));
    controller.command(Secondary(Peer));
    service_rx(system, firmware, "block write retained peer");
    receive_payload(system, firmware, controller, {data(0x42), data(0x43), data(0x44, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "block write retained UNTALK");

    // Reopen the backing file and compare the entire destination, including unused tail bytes.
    ASSERT_TRUE(ieee_drive_unmount_all());
    ASSERT_TRUE(ieee_drive_mount(0, "block-write0.img"));
    open_file(system, controller, Direct, "#");
    service_rx(system, firmware, "block write remounted direct");
    open_file(system, controller, Command, "U1:2,0,1,1");
    service_rx(system, firmware, "block write remounted raw read");
    page[0] = 254;
    std::vector<Record> expected;
    for (size_t offset = 0; offset < page.size(); ++offset) {
        expected.push_back(data(page[offset], offset + 1 == page.size() ? Eoi::Last : Eoi::More));
    }
    controller.command(Talk(Drive));
    controller.command(Secondary(Direct));
    service_rx(system, firmware, "block write persisted TALK");
    receive_payload(system, firmware, controller, expected);
    controller.command(Untalk);
    service_rx(system, firmware, "block write persisted UNTALK");
}

// B-A/B-F leave peers and direct data intact, and CLOSE persists only the intended map bits.
TEST_P(DriveLifecycleTest, BlockMapSuggestionsAndFreesPersistAfterRemount) {
    constexpr Device Drive{DeviceAddress};
    constexpr Channel Direct{DataChannel}, Peer{DataChannel + 1}, Command{CommandChannel};
    IeeeController controller(system);
    auto image = command_image(GetParam());
    const size_t bam = GetParam() == 0 ? D64::offset(18, 0) : (38 - 1) * D80SectorsPerTrack * DISKIMAGE_SECTOR_SIZE;
    const size_t entry = GetParam() == 0 ? 4 : 6;
    std::fill_n(image.begin() + bam + entry, GetParam() == 0 ? 4 : 5, 0);
    image[bam + entry] = 2;
    image[bam + entry + 1] = 3;
    firmware.mount(image, "block-map0.img");
    firmware.mount(image, "block-map1.img", 1);
    open_file(system, controller, Direct, "#");
    service_rx(system, firmware, "block map direct OPEN");
    open_file(system, controller, Peer, "1:BASIC,S,R");
    service_rx(system, firmware, "block map peer OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(Peer));
    service_rx(system, firmware, "block map peer prefix");
    receive_payload(system, firmware, controller, {data(0x41)});
    controller.command(Untalk);
    service_rx(system, firmware, "block map peer UNTALK");

    // A suggested sector is still free, and repeated release must not inflate its count.
    const std::string_view requests[] = {
        "B-F:0,1,0", "B-A:0,1,0", "B-A:0,1,0", "B-A:0,1,1", "B-F:0,1,0", "B-F:0,1,0", "B-A:0,1,0",
    };
    for (size_t step = 0; step < std::size(requests); ++step) {
        SCOPED_TRACE(requests[step]);
        open_file(system, controller, Command, requests[step]);
        service_rx(system, firmware, "block map command");
        EXPECT_EQ(receive_status(system, firmware, controller, Drive),
                  step == 2 ? (GetParam() == 0 ? "65,NO BLOCK,01,01\r" : "65,NO BLOCK,01,01,0\r")
                            : (GetParam() == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r"));
    }
    for (const auto selected : {Direct, Peer}) {
        controller.command(Talk(Drive));
        controller.command(Secondary(selected));
        service_rx(system, firmware, "block map retained TALK");
        receive_payload(system, firmware, controller,
                        selected.value() == Direct.value()
                            ? std::vector<Record>{data(DOS_BUFFER_COUNT - 1)}
                            : std::vector<Record>{data(0x42), data(0x43), data(0x44, Eoi::Last)});
        controller.command(Untalk);
        service_rx(system, firmware, "block map retained UNTALK");
    }

    // MAPOUT occurs on the direct CLOSE, not by manufacturing a directory/file write.
    controller.command(Listen(Drive));
    controller.command(Close(Direct));
    controller.command(Unlisten);
    service_rx(system, firmware, "block map direct CLOSE");
    ASSERT_TRUE(ieee_drive_unmount_all());
    ASSERT_TRUE(ieee_drive_mount(0, "block-map0.img"));
    open_file(system, controller, Direct, "#");
    service_rx(system, firmware, "block map remounted direct");
    open_file(system, controller, Command, GetParam() == 0 ? "U1:2,0,18,0" : "U1:2,0,38,0");
    service_rx(system, firmware, "block map raw BAM read");
    open_file(system, controller, Command, GetParam() == 0 ? "B-P:2,4" : "B-P:2,6");
    service_rx(system, firmware, "block map raw count pointer");
    controller.command(Talk(Drive));
    controller.command(Secondary(Direct));
    service_rx(system, firmware, "block map persisted count TALK");
    receive_payload(system, firmware, controller, {data(0), data(0)});
    controller.command(Untalk);
    service_rx(system, firmware, "block map persisted UNTALK");

    // A long exhausted search must remain cooperative and must not wrap to a lower track.
    open_file(system, controller, Command, "B-A:0,1,0");
    service_rx(system, firmware, "block map remounted search");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive),
              GetParam() == 0 ? "65,NO BLOCK,02,00\r" : "65,NO BLOCK,00,00,0\r");
}

// Cached BAM versions gate writes, not reads, and rejected pages stay unchanged after remount.
TEST_P(DriveLifecycleTest, BlockWriteVersionsRejectForeignAndAcceptZero) {
    constexpr Device Drive{DeviceAddress};
    constexpr Channel Direct{DataChannel}, Peer{DataChannel + 1}, Command{CommandChannel};
    IeeeController controller(system);
    const uint8_t native = GetParam() == 0 ? 'A' : 'C';
    for (const uint8_t version : {uint8_t{0xff}, uint8_t{0}, native}) {
        SCOPED_TRACE(version);
        const bool rejected = version == 0xff;
        auto image = copy_image(GetParam());
        std::array<uint8_t, DISKIMAGE_SECTOR_SIZE> page{};
        for (size_t offset = 0; offset < page.size(); ++offset) {
            page[offset] = static_cast<uint8_t>(offset);
        }
        page[0] = 4;
        page[1] = 13;
        page[2] = 0;
        page[3] = 255;
        page[4] = 42;
        std::copy(page.begin(), page.end(), image.begin());
        std::fill_n(image.begin() + DISKIMAGE_SECTOR_SIZE, DISKIMAGE_SECTOR_SIZE, 0);
        const size_t bam = GetParam() == 0 ? D64::offset(18, 0) : (38 - 1) * D80SectorsPerTrack * DISKIMAGE_SECTOR_SIZE;
        const size_t parts = GetParam() == 0 ? 1 : GetParam() == 1 ? 2 : 4;
        for (size_t part = 0; part < parts; ++part) {
            image[bam + part * 3 * DISKIMAGE_SECTOR_SIZE + 2] = version;
        }
        if (GetParam() != 0) {
            image[(39 - 1) * D80SectorsPerTrack * DISKIMAGE_SECTOR_SIZE + 2] = 0xff;
        }
        const std::string path0 = "write-version-" + std::to_string(version) + "-0.img";
        const std::string path1 = "write-version-" + std::to_string(version) + "-1.img";
        firmware.mount(image, path0.c_str());
        firmware.mount(image, path1.c_str(), 1);
        open_file(system, controller, Command, "I0");
        service_rx(system, firmware, "version initialize");
        open_file(system, controller, Direct, "#");
        service_rx(system, firmware, "version direct OPEN");
        open_file(system, controller, Peer, "1:BASIC,S,R");
        service_rx(system, firmware, "version peer OPEN");
        controller.command(Talk(Drive));
        controller.command(Secondary(Peer));
        service_rx(system, firmware, "version peer prefix");
        receive_payload(system, firmware, controller, {data(0x41)});
        controller.command(Untalk);
        service_rx(system, firmware, "version peer UNTALK");

        // B-W mutates the count/pointer before rejection but must not prepare a new byte.
        open_file(system, controller, Command, "U1:2,0,1,0");
        service_rx(system, firmware, "version raw source");
        open_file(system, controller, Command, "B-P:2,5");
        service_rx(system, firmware, "version count pointer");
        open_file(system, controller, Command, "B-W:2,0,1,1");
        service_rx(system, firmware, "version counted write");
        const std::string expected_status =
            rejected ? (GetParam() == 0 ? "73,CBM DOS V2,01,01\r" : "73,CBM DOS V2.7,01,01,0\r")
                     : (GetParam() == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r");
        EXPECT_EQ(receive_status(system, firmware, controller, Drive), expected_status);
        controller.command(Talk(Drive));
        controller.command(Secondary(Direct));
        service_rx(system, firmware, "version retained direct TALK");
        receive_payload(system, firmware, controller, {data(rejected ? 5 : 13), data(0), data(255), data(42)});
        controller.command(Untalk);
        service_rx(system, firmware, "version retained direct UNTALK");
        open_file(system, controller, Command, "UB:2,0,1,1");
        service_rx(system, firmware, "version raw write");
        EXPECT_EQ(receive_status(system, firmware, controller, Drive), expected_status);
        controller.command(Talk(Drive));
        controller.command(Secondary(Peer));
        service_rx(system, firmware, "version retained peer TALK");
        receive_payload(system, firmware, controller, {data(0x42), data(0x43), data(0x44, Eoi::Last)});
        controller.command(Untalk);
        service_rx(system, firmware, "version retained peer UNTALK");

        // Reload the backing file so RAM-only rejection cannot masquerade as persisted data.
        ASSERT_TRUE(ieee_drive_unmount_all());
        ASSERT_TRUE(ieee_drive_mount(0, path0.c_str()));
        open_file(system, controller, Direct, "#");
        service_rx(system, firmware, "version remounted direct");
        open_file(system, controller, Command, "U1:2,0,1,1");
        service_rx(system, firmware, "version remounted raw read");
        std::vector<Record> expected;
        for (size_t offset = 0; offset < page.size(); ++offset) {
            expected.push_back(data(rejected ? 0 : page[offset], offset + 1 == page.size() ? Eoi::Last : Eoi::More));
        }
        controller.command(Talk(Drive));
        controller.command(Secondary(Direct));
        service_rx(system, firmware, "version persisted TALK");
        receive_payload(system, firmware, controller, expected);
        controller.command(Untalk);
        service_rx(system, firmware, "version persisted UNTALK");
        ASSERT_TRUE(ieee_drive_unmount_all());
    }
}

// Ordinary B-P edits the native prepared byte, not the next logical file offset.
TEST_P(DriveLifecycleTest, ReaderPointerRetainsPreparedByteEoiAndIndependentSources) {
    constexpr Device Drive{DeviceAddress}, Other{9};
    constexpr Channel Reader{DataChannel}, Peer{DataChannel + 1}, Command{CommandChannel};
    constexpr size_t EntrySize = 32, FileTypeOffset = 2, FilenameOffset = 5, FilenameLength = 16;
    constexpr uint8_t SingletonTrack = 1, SingletonSector = 0, Tail = 0xcc;
    std::vector<uint8_t> payload{0, 0x20};
    for (unsigned int index = 0; index < 256; ++index) {
        payload.push_back(index);
    }
    payload.insert(payload.end(), {'E', 'N', 'D'});
    auto image = lifecycle_image(GetParam(), payload);
    const size_t first = sequential_first_sector(image, GetParam() != 0);
    ASSERT_NE(first, 0u);
    ASSERT_NE(image[first], SingletonTrack);
    const uint8_t link = image[first];

    // Install a singleton independently, including a known unused raw tail.
    const size_t entry = sequential_directory(GetParam() != 0) + EntrySize;
    image[entry + FileTypeOffset] = 0x80 | DISKIMAGE_FTYPE_SEQ;
    image[entry + DirectoryStartTrack] = SingletonTrack;
    image[entry + DirectoryStartSector] = SingletonSector;
    std::fill_n(image.begin() + entry + FilenameOffset, FilenameLength, 0xa0);
    std::copy_n("ONE", 3, image.begin() + entry + FilenameOffset);
    image[entry + 30] = 1;
    std::fill_n(image.begin(), DISKIMAGE_SECTOR_SIZE, Tail);
    image[0] = 0;
    image[1] = DISKIMAGE_SECTOR_LINK_SIZE;
    image[2] = 'A';
    const size_t boundary_entry = entry + EntrySize;
    std::copy_n(image.begin() + entry, EntrySize, image.begin() + boundary_entry);
    image[boundary_entry + DirectoryStartSector] = 1;
    std::fill_n(image.begin() + boundary_entry + FilenameOffset, FilenameLength, 0xa0);
    std::copy_n("BOUND254", 8, image.begin() + boundary_entry + FilenameOffset);
    image[DISKIMAGE_SECTOR_SIZE] = 0;
    image[DISKIMAGE_SECTOR_SIZE + 1] = 255;
    for (unsigned int index = 0; index < DISKIMAGE_SECTOR_PAYLOAD_SIZE; ++index) {
        image[DISKIMAGE_SECTOR_SIZE + DISKIMAGE_SECTOR_LINK_SIZE + index] = index;
    }
    if (GetParam() == 0) {
        const size_t bam = D64::offset(18, 0);
        constexpr size_t TrackOneCount = 4, TrackOneBitmap = 5;
        image[bam + TrackOneCount] -= 2;
        image[bam + TrackOneBitmap] &= ~3u;
    }
    firmware.mount(image, "reader-pointer.img");
    firmware.mount(image, "reader-pointer-peer.img", 2);
    IeeeController controller(system);
    const std::string ok = GetParam() == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r";

    // Fence every source selection and observe both accepted and passive wire bytes.
    const auto read = [&](Channel channel, const std::vector<Record>& expected, Device device = Device{DeviceAddress}) {
        controller.command(Talk(device));
        controller.command(Secondary(channel));
        service_rx(system, firmware, "reader pointer TALK");
        if (expected.empty()) {
            expect_no_payload(system, firmware, controller);
        } else {
            receive_payload(system, firmware, controller, expected);
        }
        controller.command(Untalk);
        service_rx(system, firmware, "reader pointer UNTALK");
    };
    // Exercise both OPEN-15 and ordinary secondary-address command collection.
    const auto point = [&](unsigned int offset, bool secondary = false) {
        const std::string request = "B-P:2," + std::to_string(offset);
        if (secondary) {
            controller.command(Listen(Drive));
            controller.command(Secondary(Command));
            for (size_t index = 0; index < request.size(); ++index) {
                controller.data(request[index], index + 1 == request.size() ? Eoi::Last : Eoi::More);
            }
            controller.command(Unlisten);
        } else {
            open_file(system, controller, Command, request);
        }
        service_rx(system, firmware, "reader pointer command");
        EXPECT_EQ(receive_status(system, firmware, controller, Drive), ok);
    };
    // CLOSE must release reader ownership without writing or completing a writer.
    const auto reopen = [&](std::string_view name) {
        controller.command(Listen(Drive));
        controller.command(Close(Reader));
        controller.command(Unlisten);
        service_rx(system, firmware, "reader pointer CLOSE");
        open_file(system, controller, Reader, name);
        service_rx(system, firmware, "reader pointer OPEN");
    };

    // Keep paused sources on this unit and an independent unit throughout pointer edits.
    open_file(system, controller, Peer, "0:BOUND254,S,R");
    service_rx(system, firmware, "reader pointer local peer");
    open_file(system, controller, Peer, "0:BOUND254,S,R", Other);
    service_rx(system, firmware, "reader pointer other-unit peer");
    for (const auto device : {Drive, Other}) {
        read(Peer, {data(0), data(1)}, device);
    }
    open_file(system, controller, Reader, "0:BASIC,S,R");
    service_rx(system, firmware, "reader pointer linked OPEN");
    read(Reader, {data(0), data(0x20), data(0), data(1)});
    point(4);
    read(Reader, {data(0), data(0), data(1), data(2)});
    point(254, true);
    read(Reader, {data(250), data(250), data(251)});
    point(2);
    read(Reader, {data(252), data(252), data(253)});
    point(8);
    read(Reader, {data('D', Eoi::Last)});
    point(2);
    read(Reader, {});
    point(8);
    read(Reader, {data('D', Eoi::Last)});
    reopen("0:BASIC,S,R");
    read(Reader, {data(0)});
    point(0);
    read(Reader, {data(link, Eoi::Last)});
    point(0);
    read(Reader, {data(link, Eoi::Last)});
    for (const auto device : {Drive, Other}) {
        read(Peer, {data(2), data(3)}, device);
    }

    // Preserve singleton quirks, sticky EOI and raw-tail wrap without changing file data.
    reopen("0:ONE,S,R");
    point(2);
    read(Reader, {data('A', Eoi::Last)});
    point(0);
    read(Reader, {});
    point(2);
    read(Reader, {data('A', Eoi::Last)});
    reopen("0:ONE,S,R");
    read(Reader, {data('A')});
    point(0);
    read(Reader, {data(0), data(0), data(2), data('A', Eoi::Last)});
    reopen("0:BOUND254,S,R");
    read(Reader, {data(0), data(1)});
    point(254);
    read(Reader, {data(252), data(252)});
    point(2);
    read(Reader, {data(0, Eoi::Last)});
    point(255);
    read(Reader, {data(253, Eoi::Last)});
    reopen("0:ONE,S,R");
    point(255);
    read(Reader, {data(Tail), data(Tail, Eoi::Last)});
    point(2);
    read(Reader, {data('A', Eoi::Last)});
    reopen("0:BASIC,S,R");
    point(8);
    read(Reader, {data(4), data(4)});
    ASSERT_TRUE(ieee_drive_unmount_all());
}

// B-R/U1 replace an ordinary active page but retain sequential GET and independent readers.
TEST_P(DriveLifecycleTest, ReaderBlockReadsRetainSequentialEofAndIndependentSources) {
    constexpr Device Drive{DeviceAddress}, Other{9};
    constexpr Channel Reader{DataChannel}, Peer{DataChannel + 1}, Command{CommandChannel};
    constexpr unsigned int PrefixLength = 253;
    constexpr uint8_t Count = 4;
    std::vector<uint8_t> payload{0, 0x20};
    for (unsigned int index = 0; index < DISKIMAGE_SECTOR_SIZE; ++index) {
        payload.push_back(index);
    }
    payload.insert(payload.end(), {'E', 'N', 'D'});
    const auto source = lifecycle_image(GetParam(), payload);
    ASSERT_NE(sequential_first_sector(source, GetParam() != 0), 0u);
    auto target = copy_image(GetParam());
    std::array<uint8_t, DISKIMAGE_SECTOR_SIZE> page{};
    for (size_t offset = 0; offset < page.size(); ++offset) {
        page[offset] = offset;
    }
    page[0] = Count;
    page[1] = 13;
    page[2] = 0;
    page[3] = 255;
    page[4] = 42;
    page.back() = 99;
    ASSERT_NE(sequential_first_sector(target, GetParam() != 0), 0u);
    std::copy(page.begin(), page.end(), target.begin());
    firmware.mount(source, "reader-block-source.img");
    firmware.mount(target, "reader-block-target.img", 1);
    firmware.mount(copy_image(GetParam()), "reader-block-other.img", 2);
    IeeeController controller(system);
    const std::string ok = GetParam() == 0 ? "00, OK,00,00\r" : "00, OK,00,00,1\r";

    // Observe physical data/EOI and explicitly fence every TALK/UNTALK.
    const auto read = [&](Channel channel, const std::vector<Record>& expected, Device device = Device{DeviceAddress}) {
        controller.command(Talk(device));
        controller.command(Secondary(channel));
        service_rx(system, firmware, "reader block TALK");
        if (expected.empty()) {
            expect_no_payload(system, firmware, controller);
        } else {
            receive_payload(system, firmware, controller, expected);
        }
        controller.command(Untalk);
        service_rx(system, firmware, "reader block UNTALK");
    };
    // Alternate command routes and service between bytes before the retained I/O job.
    const auto command = [&](std::string_view request, bool secondary) {
        controller.command(Listen(Drive));
        controller.command(secondary ? Secondary(Command) : Open(Command));
        for (size_t index = 0; index < request.size(); ++index) {
            firmware.service();
            controller.data(request[index], index + 1 == request.size() ? Eoi::Last : Eoi::More);
        }
        controller.command(Unlisten);
        service_rx(system, firmware, "reader block command");
        EXPECT_EQ(receive_status(system, firmware, controller, Drive), ok);
    };

    // Pause local and independent-unit readers before crossing the edited reader's page.
    open_file(system, controller, Peer, "0:BASIC,S,R");
    service_rx(system, firmware, "reader block peer OPEN");
    read(Peer, {data(0), data(0x20)});
    open_file(system, controller, Peer, "0:BASIC,S,R", Other);
    service_rx(system, firmware, "reader block other OPEN");
    read(Peer, {data('A')}, Other);
    open_file(system, controller, Reader, "0:BASIC,S,R");
    service_rx(system, firmware, "reader block OPEN");
    std::vector<Record> prefix;
    for (unsigned int index = 0; index < PrefixLength; ++index) {
        prefix.push_back(data(payload[index]));
    }
    read(Reader, prefix);

    // Counted reads duplicate the prepared byte and stop, unlike circular direct GET.
    command("B-R:2,1,1,0", false);
    read(Reader, {data(13), data(13), data(0), data(255), data(42, Eoi::Last)});
    read(Reader, {});
    command("B-P:2,4", true);
    read(Reader, {data(42, Eoi::Last)});
    command("U1:2,1,1,0", true);
    read(Reader, {data(Count), data(Count), data(13), data(0)});
    std::vector<Record> tail;
    for (size_t offset = 3; offset < page.size(); ++offset) {
        tail.push_back(data(page[offset], offset + 1 == page.size() ? Eoi::Last : Eoi::More));
    }
    read(Reader, tail);
    read(Reader, {});
    command("UA:2,1,1,0", false);
    read(Reader, {data(Count), data(Count), data(13), data(0)});
    read(Peer, {data(0), data(1)});
    read(Peer, {data('B'), data('C'), data('D', Eoi::Last)}, Other);

    // CLOSE and actual remount must neither write the borrowed page nor alter the file.
    ASSERT_TRUE(ieee_drive_unmount_all());
    ASSERT_TRUE(ieee_drive_mount(0, "reader-block-source.img"));
    ASSERT_TRUE(ieee_drive_mount(1, "reader-block-target.img"));
    open_file(system, controller, Reader, "0:BASIC,S,R");
    service_rx(system, firmware, "reader block remounted OPEN");
    read(Reader, {data(0), data(0x20), data(0), data(1)});
    command("B-R:2,1,1,0", true);
    read(Reader, {data(13), data(13), data(0), data(255), data(42, Eoi::Last)});
}

// Ordinary B-W/U2 keep sequential GET, native prepared pointers and independent sources.
TEST_P(DriveLifecycleTest, ReaderBlockWritesPersistNativePagesWithoutGrantingListenAccess) {
    constexpr Device Drive{DeviceAddress}, Other{9};
    constexpr Channel Reader{DataChannel}, Peer{DataChannel + 1}, Command{CommandChannel};
    std::vector<uint8_t> payload{0, 0x20};
    for (unsigned int index = 0; index < DISKIMAGE_SECTOR_SIZE; ++index) {
        payload.push_back(index);
    }
    payload.insert(payload.end(), {'E', 'N', 'D'});
    const auto source = lifecycle_image(GetParam(), payload);
    const size_t first_offset = sequential_first_sector(source, GetParam() != 0);
    ASSERT_NE(first_offset, 0u);
    std::array<uint8_t, DISKIMAGE_SECTOR_SIZE> first{}, last{};
    std::copy_n(source.begin() + first_offset, first.size(), first.begin());
    // The independently encoded lifecycle fixture has exactly two linked data pages.
    const size_t last_offset = GetParam() == 0
                                   ? D64::offset(first[0], first[1])
                                   : ((first[0] - 1) * D80SectorsPerTrack + first[1]) * DISKIMAGE_SECTOR_SIZE;
    ASSERT_LT(last_offset + last.size(), source.size());
    std::copy_n(source.begin() + last_offset, last.size(), last.begin());
    ASSERT_EQ(last[0], 0);
    ASSERT_EQ(last[1], 8);
    first[0] = 6;
    firmware.mount(source, "reader-write-source.img");
    firmware.mount(copy_image(GetParam()), "reader-write-target.img", 1);
    firmware.mount(copy_image(GetParam()), "reader-write-other.img", 2);
    IeeeController controller(system);

    // Fence every counted transfer to expose retained FIFO source and EOI state.
    const auto read = [&](Channel channel, const std::vector<Record>& expected, Device device = Device{DeviceAddress}) {
        controller.command(Talk(device));
        controller.command(Secondary(channel));
        service_rx(system, firmware, "reader write TALK");
        if (expected.empty()) {
            expect_no_payload(system, firmware, controller);
        } else {
            receive_payload(system, firmware, controller, expected);
        }
        controller.command(Untalk);
        service_rx(system, firmware, "reader write UNTALK");
    };
    // Alternate OPEN-15 and ordinary command-secondary collection.
    const auto command = [&](std::string_view request, bool secondary) {
        controller.command(Listen(Drive));
        controller.command(secondary ? Secondary(Command) : Open(Command));
        for (size_t index = 0; index < request.size(); ++index) {
            firmware.service();
            controller.data(request[index], index + 1 == request.size() ? Eoi::Last : Eoi::More);
        }
        controller.command(Unlisten);
        service_rx(system, firmware, "reader write command");
        EXPECT_EQ(receive_status(system, firmware, controller, Drive),
                  GetParam() == 0 ? "00, OK,00,00\r" : "00, OK,00,00,1\r");
    };
    // Keep local and independent-unit readers partially consumed during every edit.
    open_file(system, controller, Peer, "0:BASIC,S,R");
    service_rx(system, firmware, "reader write peer OPEN");
    read(Peer, {data(0), data(0x20)});
    open_file(system, controller, Peer, "0:BASIC,S,R", Other);
    service_rx(system, firmware, "reader write other OPEN");
    read(Peer, {data('A')}, Other);
    open_file(system, controller, Reader, "BASIC,S,R");
    service_rx(system, firmware, "reader write default OPEN");
    read(Reader, {data(0), data(0x20), data(0), data(1)});
    command("B-W:2,3,1,0", false);
    // A block write must not turn this read channel into a direct or writable file.
    controller.command(Listen(Drive));
    controller.command(Secondary(Reader));
    controller.data(0x99, Eoi::Last);
    controller.command(Unlisten);
    service_rx(system, firmware, "reader write ignored ordinary PUT");
    read(Reader, {data(first[1]), data(first[1])});
    command("U2:2,1,1,1", true);
    read(Reader, {data(0), data(0x20)});

    // Cross the lazy/native active-page boundary, then write the already selected successor.
    open_file(system, controller, Reader, "0:BASIC,S,R");
    service_rx(system, firmware, "reader write explicit OPEN");
    std::vector<Record> prefix;
    for (size_t index = 0; index < 253; ++index) {
        prefix.push_back(data(payload[index]));
    }
    read(Reader, prefix);
    command("UB:2,1,1,2", true);
    read(Reader, {data(251), data(252)});
    command("B-P:2,8", false);
    read(Reader, {data('D', Eoi::Last)});
    command("U2:2,1,1,3", false);
    read(Reader, {});
    command("B-P:2,8", true);
    command("B-W:2,1,1,4", false);
    read(Reader, {data(8, Eoi::Last)});
    read(Reader, {});
    read(Peer, {data(0), data(1)});
    read(Peer, {data('B'), data('C'), data('D', Eoi::Last)}, Other);

    // Reopen actual backing files and compare all persisted page bytes, including tails.
    ASSERT_TRUE(ieee_drive_unmount_all());
    ASSERT_TRUE(ieee_drive_mount(0, "reader-write-source.img"));
    ASSERT_TRUE(ieee_drive_mount(1, "reader-write-target.img"));
    open_file(system, controller, Reader, "#");
    service_rx(system, firmware, "reader write remounted direct");
    for (unsigned int sector = 0; sector < 5; ++sector) {
        auto expected = sector < 2 ? first : last;
        if (sector == 4) {
            expected[0] = 7;
        }
        command("U1:2,1,1," + std::to_string(sector), false);
        std::vector<Record> page;
        for (size_t index = 0; index < expected.size(); ++index) {
            page.push_back(data(expected[index], index + 1 == expected.size() ? Eoi::Last : Eoi::More));
        }
        read(Reader, page);
    }
    open_file(system, controller, Reader, "0:BASIC,S,R");
    service_rx(system, firmware, "reader write remounted source");
    read(Reader, {data(0), data(0x20), data(0), data(1)});
}

// The default IRQ vector re-enters transport without resetting owners, prepared bytes or maps.
TEST_P(DriveLifecycleTest, UserInterruptRetainsSourcesWriterAndDirtyMap) {
    constexpr Device Drive{DeviceAddress}, Other{9};
    constexpr Channel Direct{DataChannel}, Reader{DataChannel + 1}, Writer{DataChannel + 2}, Command{CommandChannel};
    IeeeController controller(system);
    auto image = copy_image(GetParam());
    const size_t bam = GetParam() == 0 ? D64::offset(18, 0) : (38 - 1) * D80SectorsPerTrack * DISKIMAGE_SECTOR_SIZE;
    const size_t entry = GetParam() == 0 ? 4 : 6;
    std::fill_n(image.begin() + bam + entry, GetParam() == 0 ? 4 : 5, 0);
    image[bam + entry] = 2;
    image[bam + entry + 1] = 3;
    firmware.mount(image, "user-interrupt0.img");
    firmware.mount(image, "user-interrupt1.img", 1);
    firmware.mount(image, "user-interrupt-peer.img", 2);
    open_file(system, controller, Command, "I0");
    service_rx(system, firmware, "interrupt initial metadata");
    open_file(system, controller, Command, "I1");
    service_rx(system, firmware, "interrupt peer metadata");
    open_file(system, controller, Writer, "1:IRQFILE,S,W");
    service_rx(system, firmware, "interrupt unfinished writer");
    controller.command(Listen(Drive));
    controller.command(Secondary(Writer));
    for (const uint8_t byte : std::string_view{"WRIT"}) {
        firmware.service();
        controller.data(byte, byte == 'T' ? Eoi::Last : Eoi::More);
    }
    controller.command(Unlisten);
    service_rx(system, firmware, "interrupt writer prefix");
    open_file(system, controller, Reader, "0:BASIC,S,R");
    service_rx(system, firmware, "interrupt local reader");
    open_file(system, controller, Reader, "0:BASIC,S,R", Other);
    service_rx(system, firmware, "interrupt other-unit reader");
    for (const auto device : {Drive, Other}) {
        controller.command(Talk(device));
        controller.command(Secondary(Reader));
        service_rx(system, firmware, "interrupt reader prefix");
        receive_payload(system, firmware, controller, {data(0x41)});
        controller.command(Untalk);
        service_rx(system, firmware, "interrupt reader prefix UNTALK");
    }

    // Prepare page byte zero before IRQ entry, so a hidden cursor reset cannot pass.
    open_file(system, controller, Direct, "#11");
    service_rx(system, firmware, "interrupt explicit direct OPEN");
    open_file(system, controller, Command, "B-P:2,0");
    service_rx(system, firmware, "interrupt write pointer");
    controller.command(Listen(Drive));
    controller.command(Secondary(Direct));
    for (const uint8_t byte : std::string_view{"IRQ!"}) {
        firmware.service();
        controller.data(byte, byte == '!' ? Eoi::Last : Eoi::More);
    }
    controller.command(Unlisten);
    service_rx(system, firmware, "interrupt page write");
    open_file(system, controller, Command, "B-A:0,1,0");
    service_rx(system, firmware, "interrupt dirty claim");
    open_file(system, controller, Command, "B-P:2,0");
    service_rx(system, firmware, "interrupt prepared read pointer");
    open_file(system, controller, Command, "?");
    service_rx(system, firmware, "interrupt old error");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive),
              GetParam() == 0 ? "31,SYNTAX ERROR,00,00\r" : "31,SYNTAX ERROR,00,00,0\r");
    const std::string ok = GetParam() == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r";
    for (const std::string_view request : {"UK", "U", "U;", "U["}) {
        if (request == "U") {
            // Secondary-address collection must retain the previous selector without CR.
            controller.command(Listen(Drive));
            controller.command(Secondary(Command));
            controller.data('U', Eoi::Last);
            controller.command(Unlisten);
        } else {
            open_file(system, controller, Command, request);
        }
        service_rx(system, firmware, "interrupt vector or stale selector");
        EXPECT_EQ(receive_status(system, firmware, controller, Drive), ok);
    }
    controller.command(Talk(Drive));
    controller.command(Secondary(Direct));
    service_rx(system, firmware, "interrupt retained page TALK");
    receive_payload(system, firmware, controller, {data('I'), data('R'), data('Q'), data('!')});
    controller.command(Untalk);
    service_rx(system, firmware, "interrupt retained page UNTALK");
    for (const auto device : {Drive, Other}) {
        controller.command(Talk(device));
        controller.command(Secondary(Reader));
        service_rx(system, firmware, "interrupt retained reader TALK");
        receive_payload(system, firmware, controller, {data(0x42), data(0x43), data(0x44, Eoi::Last)});
        controller.command(Untalk);
        service_rx(system, firmware, "interrupt retained reader UNTALK");
    }
    open_file(system, controller, Command, "B-A:0,1,0");
    service_rx(system, firmware, "interrupt retained map");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive),
              GetParam() == 0 ? "65,NO BLOCK,01,01\r" : "65,NO BLOCK,01,01,0\r");

    // Continue the same writer after IRQ entry, without reopening or finalizing it early.
    controller.command(Listen(Drive));
    controller.command(Secondary(Writer));
    for (const uint8_t byte : std::string_view{"DONE"}) {
        firmware.service();
        controller.data(byte, byte == 'E' ? Eoi::Last : Eoi::More);
    }
    controller.command(Unlisten);
    service_rx(system, firmware, "interrupt writer suffix");
    for (const auto channel : {Writer, Direct}) {
        controller.command(Listen(Drive));
        controller.command(Close(channel));
        controller.command(Unlisten);
        service_rx(system, firmware, "interrupt retained channel CLOSE");
    }
    ASSERT_TRUE(ieee_drive_unmount_all());
    ASSERT_TRUE(ieee_drive_mount(0, "user-interrupt0.img"));
    ASSERT_TRUE(ieee_drive_mount(1, "user-interrupt1.img"));
    open_file(system, controller, Reader, "1:IRQFILE,S,R");
    service_rx(system, firmware, "interrupt persisted writer OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(Reader));
    service_rx(system, firmware, "interrupt persisted writer TALK");
    receive_payload(
        system, firmware, controller,
        {data('W'), data('R'), data('I'), data('T'), data('D'), data('O'), data('N'), data('E', Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "interrupt persisted writer UNTALK");
    open_file(system, controller, Direct, "#");
    service_rx(system, firmware, "interrupt remounted direct");
    open_file(system, controller, Command, GetParam() == 0 ? "U1:2,0,18,0" : "U1:2,0,38,0");
    service_rx(system, firmware, "interrupt remounted BAM");
    open_file(system, controller, Command, GetParam() == 0 ? "B-P:2,4" : "B-P:2,6");
    service_rx(system, firmware, "interrupt remounted BAM pointer");
    controller.command(Talk(Drive));
    controller.command(Secondary(Direct));
    service_rx(system, firmware, "interrupt persisted BAM TALK");
    receive_payload(system, firmware, controller, {data(1), data(2)});
    controller.command(Untalk);
    service_rx(system, firmware, "interrupt persisted BAM UNTALK");
}

// NMI releases sources without clearing page RAM, and only later AUTOI discards DOS 2.7 maps.
TEST_P(DriveLifecycleTest, WarmResetRetainsPagesAndDefersModelSpecificMapReload) {
    constexpr Device Drive{DeviceAddress}, Other{9};
    constexpr Channel Direct{DataChannel}, Reader{DataChannel + 1}, Command{CommandChannel};
    IeeeController controller(system);
    auto image = command_image(GetParam());
    const size_t bam = GetParam() == 0 ? D64::offset(18, 0) : (38 - 1) * D80SectorsPerTrack * DISKIMAGE_SECTOR_SIZE;
    const size_t entry = GetParam() == 0 ? 4 : 6;
    std::fill_n(image.begin() + bam + entry, GetParam() == 0 ? 4 : 5, 0);
    image[bam + entry] = 2;
    image[bam + entry + 1] = 3;
    firmware.mount(image, "warm-reset.img");
    firmware.mount(image, "warm-reset-peer.img", 2);
    open_file(system, controller, Command, "I0");
    service_rx(system, firmware, "warm initial metadata");
    open_file(system, controller, Reader, "0:BASIC,S,R");
    service_rx(system, firmware, "warm local reader");
    open_file(system, controller, Reader, "0:BASIC,S,R", Other);
    service_rx(system, firmware, "warm other-unit reader");
    for (const auto device : {Drive, Other}) {
        controller.command(Talk(device));
        controller.command(Secondary(Reader));
        service_rx(system, firmware, "warm reader prefix");
        receive_payload(system, firmware, controller, {data(0x41)});
        controller.command(Untalk);
        service_rx(system, firmware, "warm reader prefix UNTALK");
    }

    // Keep an explicit page and a dirty BAM in RAM, with no ordinary file to finalize.
    open_file(system, controller, Direct, "#11");
    service_rx(system, firmware, "warm explicit direct OPEN");
    open_file(system, controller, Command, "B-P:2,0");
    service_rx(system, firmware, "warm write pointer");
    controller.command(Listen(Drive));
    controller.command(Secondary(Direct));
    for (const uint8_t byte : std::string_view{"WARM"}) {
        firmware.service();
        controller.data(byte, byte == 'M' ? Eoi::Last : Eoi::More);
    }
    controller.command(Unlisten);
    service_rx(system, firmware, "warm page write");
    open_file(system, controller, Command, "B-A:0,1,0");
    service_rx(system, firmware, "warm dirty claim");
    const std::string power_on = GetParam() == 0 ? "73,CBM DOS V2,00,00\r" : "73,CBM DOS V2.7,00,00,0\r";
    const std::string occupied = GetParam() == 0 ? "65,NO BLOCK,01,01\r" : "65,NO BLOCK,01,01,0\r";
    const std::string ok = GetParam() == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r";
    for (const auto request : {"UI", "U"}) {
        open_file(system, controller, Command, request);
        service_rx(system, firmware, "warm reset or stale selector");
        EXPECT_EQ(receive_status(system, firmware, controller, Drive), power_on);
    }
    controller.command(Talk(Drive));
    controller.command(Secondary(Reader));
    service_rx(system, firmware, "warm released reader TALK");
    expect_no_payload(system, firmware, controller);
    controller.command(Untalk);
    service_rx(system, firmware, "warm released reader UNTALK");
    open_file(system, controller, Command, "B-A:0,1,0");
    service_rx(system, firmware, "warm retained pre-AUTOI claim");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), occupied);

    // Reopening the exact page proves byte retention and consumes DOS 2.7 AUTOI.
    for (const bool second_reset : {false, true}) {
        if (second_reset) {
            open_file(system, controller, Command, "U9");
            service_rx(system, firmware, "warm numeric alias");
            EXPECT_EQ(receive_status(system, firmware, controller, Drive), power_on);
        }
        open_file(system, controller, Direct, "#11");
        service_rx(system, firmware, "warm reused direct page");
        open_file(system, controller, Command, "B-P:2,0");
        service_rx(system, firmware, "warm retained read pointer");
        controller.command(Talk(Drive));
        controller.command(Secondary(Direct));
        service_rx(system, firmware, "warm retained page TALK");
        receive_payload(system, firmware, controller, {data('W'), data('A'), data('R'), data('M')});
        controller.command(Untalk);
        service_rx(system, firmware, "warm retained page UNTALK");
        if (!second_reset) {
            open_file(system, controller, Command, "B-A:0,1,0");
            service_rx(system, firmware, "warm post-AUTOI claim");
            EXPECT_EQ(receive_status(system, firmware, controller, Drive), GetParam() == 0 ? occupied : ok);
        }
    }
    controller.command(Listen(Drive));
    controller.command(Close(Direct));
    controller.command(Unlisten);
    service_rx(system, firmware, "warm surviving direct CLOSE");
    controller.command(Talk(Other));
    controller.command(Secondary(Reader));
    service_rx(system, firmware, "warm independent unit TALK");
    receive_payload(system, firmware, controller, {data(0x42), data(0x43), data(0x44, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "warm independent unit UNTALK");

    // Reload actual backing media to distinguish a retained claim from abandoned RAM.
    ASSERT_TRUE(ieee_drive_unmount_all());
    ASSERT_TRUE(ieee_drive_mount(0, "warm-reset.img"));
    open_file(system, controller, Direct, "#");
    service_rx(system, firmware, "warm remounted direct");
    open_file(system, controller, Command, GetParam() == 0 ? "U1:2,0,18,0" : "U1:2,0,38,0");
    service_rx(system, firmware, "warm remounted BAM");
    open_file(system, controller, Command, GetParam() == 0 ? "B-P:2,4" : "B-P:2,6");
    service_rx(system, firmware, "warm remounted BAM pointer");
    controller.command(Talk(Drive));
    controller.command(Secondary(Direct));
    service_rx(system, firmware, "warm persisted BAM TALK");
    receive_payload(system, firmware, controller,
                    GetParam() == 0 ? std::vector<Record>{data(1), data(2)} : std::vector<Record>{data(2), data(3)});
    controller.command(Untalk);
    service_rx(system, firmware, "warm persisted BAM UNTALK");
}

TEST_P(DrivePrgTest, StreamsCheckedPrgFixtureOverProductionTop) {
    std::vector<uint8_t> payload(GetParam());
    std::vector<Record> expected;
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(0x42 + i);
        expected.push_back(data(payload[i], i + 1 == payload.size() ? Eoi::Last : Eoi::More));
    }
    IeeeController controller(system);
    firmware.mount(D64{}.prg("BASIC", payload));
    open_file(system, controller);
    service_rx(system, firmware, "service PRG OPEN");
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Channel{LoadChannel}));
    service_rx(system, firmware, "service PRG TALK");
    receive_payload(system, firmware, controller, expected);
    EXPECT_TRUE(system.ieee().read_status().contains(fpga::IeeeStatusBit::TxEmpty));
    controller.command(Untalk);
    service_rx(system, firmware, "PRG UNTALK");
}

INSTANTIATE_TEST_SUITE_P(FileLengths, DrivePrgTest, testing::Values(size_t{2}, size_t{4}, size_t{254}, size_t{255}));

TEST_P(NativeDriveTest, ReplacementReadsAndConsumesStatusThroughProductionSpi) {
    const int format = GetParam() / 2;
    const unsigned int drive = GetParam() % 2;
    const Device device{static_cast<uint8_t>(8 + format)};
    const unsigned int slot = format * 2 + drive;
    IeeeController controller(system);
    std::vector<uint8_t> payload(255);
    for (size_t index = 0; index < payload.size(); ++index)
        payload[index] = static_cast<uint8_t>(index);
    auto image = sequential_image(format != 0, payload);
    if (format == 2) {
        image.resize(DISKIMAGE_D82_SIZE);
        const size_t first = sequential_first_sector(image, true);
        const size_t sectors = (payload.size() + DISKIMAGE_SECTOR_PAYLOAD_SIZE - 1) / DISKIMAGE_SECTOR_PAYLOAD_SIZE;
        for (size_t index = 0; index < sectors; ++index) {
            const size_t source = first + index * DISKIMAGE_SECTOR_SIZE;
            const size_t target = source + DISKIMAGE_D80_SIZE;
            std::copy_n(image.begin() + source, DISKIMAGE_SECTOR_SIZE, image.begin() + target);
            if (image[target] != 0)
                image[target] += DISKIMAGE_D80_TRACK_COUNT;
        }
        image[sequential_directory(true) + DirectoryStartTrack] += DISKIMAGE_D80_TRACK_COUNT;
    }
    firmware.mount(image, format == 0 ? "native.d64" : format == 1 ? "native.d80" : "native.d82", slot);
    EXPECT_EQ(receive_status(system, firmware, controller, device),
              format == 0 ? "73,CBM DOS V2,00,00\r" : "73,CBM DOS V2.7,00,00,0\r");
    EXPECT_EQ(receive_status(system, firmware, controller, device),
              format == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r");
    open_file(system, controller, Channel{DataChannel}, std::to_string(drive) + ":BASIC,SEQ,R", device);
    service_rx(system, firmware, "replacement OPEN");
    controller.command(Talk(device));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "replacement TALK");
    for (size_t index = 0; index < payload.size(); ++index) {
        firmware.service();
        EXPECT_EQ(controller.receive(), data(payload[index], index + 1 == payload.size() ? Eoi::Last : Eoi::More));
    }
    controller.command(Untalk);
    service_rx(system, firmware, "replacement UNTALK");
    EXPECT_EQ(receive_status(system, firmware, controller, device), format == 0  ? "00, OK,00,00\r"
                                                                    : drive == 0 ? "00, OK,00,00,0\r"
                                                                                 : "00, OK,00,00,1\r");
}

INSTANTIATE_TEST_SUITE_P(ModelsAndSlots, NativeDriveTest, testing::Range(0, 6));

TEST_F(FirmwareTest, NativeRejectedMountRetainsCursorBeforeSequentialReload) {
    constexpr Device Drive{DeviceAddress};
    IeeeController controller(system);
    const std::vector<uint8_t> payload{0x11, 0x22, 0x33, 0x44};
    firmware.mount(sequential_image(false, payload), "old.d64");
    firmware.mount(sequential_image(true, payload), "next.d80", 2);
    open_file(system, controller, Channel{DataChannel}, "0:BASIC,SEQ,R");
    service_rx(system, firmware, "retained OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "retained TALK");
    EXPECT_EQ(controller.receive(), data(payload[0]));
    controller.command(Untalk);
    service_rx(system, firmware, "retained UNTALK");
    EXPECT_FALSE(ieee_drive_mount(1, "next.d80"));
    EXPECT_FALSE(ieee_drive_mount(0, "does-not-exist.d82"));
    controller.command(Talk(Drive));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "retained suffix");
    for (size_t index = 1; index < payload.size(); ++index)
        EXPECT_EQ(controller.receive(), data(payload[index], index + 1 == payload.size() ? Eoi::Last : Eoi::More));
    controller.command(Untalk);
    service_rx(system, firmware, "retained suffix UNTALK");
    ASSERT_TRUE(ieee_drive_unmount_all());
    ASSERT_TRUE(ieee_drive_mount(0, "next.d80"));
    ASSERT_TRUE(ieee_drive_mount(1, "next.d80"));
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), "73,CBM DOS V2.7,00,00,0\r");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), "00, OK,00,00,0\r");
    open_file(system, controller, Channel{DataChannel}, "1:BASIC,SEQ,R");
    service_rx(system, firmware, "replacement OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "replacement TALK");
    receive_payload(system, firmware, controller, {data(0x11), data(0x22), data(0x33), data(0x44, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "replacement UNTALK");
}

TEST_F(FirmwareTest, NativeUnitRemovalPreservesPeerGetCursorAndStatus) {
    constexpr Device Drive{DeviceAddress}, Peer{9};
    IeeeController controller(system);
    const std::vector<uint8_t> payload{0x31, 0x32, 0x33, 0x34};
    firmware.mount(sequential_image(false, payload), "unit8.d64");
    firmware.mount(sequential_image(true, payload), "unit9.d80", 2);
    open_file(system, controller, Channel{DataChannel}, "0:BASIC,SEQ,R", Peer);
    service_rx(system, firmware, "peer OPEN");
    controller.command(Talk(Peer));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "peer TALK");
    EXPECT_EQ(controller.receive(), data(payload[0]));
    controller.command(Untalk);
    service_rx(system, firmware, "peer interrupted");
    ASSERT_TRUE(ieee_drive_mount(0, nullptr));
    controller.command(Talk(Peer));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "peer resumed");
    for (size_t index = 1; index < payload.size(); ++index)
        EXPECT_EQ(controller.receive(), data(payload[index], index + 1 == payload.size() ? Eoi::Last : Eoi::More));
    controller.command(Untalk);
    service_rx(system, firmware, "peer finished");
    EXPECT_EQ(receive_status(system, firmware, controller, Peer), "00, OK,00,00,0\r");
    controller.command(Talk(Drive));
    controller.command(Secondary(Channel{CommandChannel}));
    service_rx(system, firmware, "absent unit");
    expect_no_payload(system, firmware, controller);
    controller.command(Untalk);
    service_rx(system, firmware, "absent UNTALK");
}

// The selected ROM's OR30/RDBYT exposes X,0,2,X with EOI only on the fourth GET.
TEST_P(DriveOneByteTest, PreservesNativeEofBugOverProductionTop) {
    const uint8_t byte = GetParam();
    IeeeController controller(system);
    firmware.mount(D64{}.seq("BASIC", {byte}));
    open_file(system, controller, Channel{DataChannel});
    service_rx(system, firmware, "one-byte OPEN");
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "one-byte TALK");
    receive_payload(system, firmware, controller, {data(byte), data(0), data(2), data(byte, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "one-byte UNTALK");
}

INSTANTIATE_TEST_SUITE_P(StoredBytes, DriveOneByteTest,
                         testing::Values(uint8_t{0}, uint8_t{0x0d}, uint8_t{0x41}, uint8_t{0xff}));

TEST_P(DriveTerminalTest, SequentialRetalkAfterEofDoesNotInventData) {
    const bool d80 = GetParam();
    constexpr Device Drive{DeviceAddress};
    IeeeController controller(system);
    firmware.mount(sequential_image(d80, {0x42, 0x43}), d80 ? "terminal.d80" : "terminal.d64");
    open_file(system, controller, Channel{DataChannel});
    service_rx(system, firmware, "SEQ OPEN");
    controller.command(Talk(Drive));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "first SEQ TALK");
    receive_payload(system, firmware, controller, {data(0x42), data(0x43, Eoi::Last)});
    controller.command(Untalk);
    controller.command(Talk(Drive));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "exhausted SEQ TALK");
    expect_no_payload(system, firmware, controller);
    controller.command(Untalk);
    service_rx(system, firmware, "exhausted SEQ UNTALK");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), d80 ? "00, OK,00,00,0\r" : "00, OK,00,00\r");
    open_file(system, controller, Channel{DataChannel});
    service_rx(system, firmware, "SEQ reopen");
    controller.command(Talk(Drive));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "reopened SEQ TALK");
    receive_payload(system, firmware, controller, {data(0x42), data(0x43, Eoi::Last)});
    controller.command(Untalk);
    service_rx(system, firmware, "reopened SEQ UNTALK");
}

// Real ROM-4 per-byte status selection must retain CR/EOI across both service periods.
TEST_P(DriveRepeatedStatusTest, NativeRom4RepeatedOkStatusRetainsCrAndEoi) {
    const auto [d80, coarse] = GetParam();
    IeeeController controller(system);
    firmware.mount(sequential_image(d80, {0x42, 0x43}), d80 ? "repeat.d80" : "repeat.d64");
    open_file(system, controller, Channel{DataChannel});
    service_rx(system, firmware, "status-repeat OPEN");
    controller.command(Talk(Device{DeviceAddress}));
    controller.command(Secondary(Channel{DataChannel}));
    service_rx(system, firmware, "status-repeat TALK");
    receive_payload(system, firmware, controller, {data(0x42), data(0x43, Eoi::Last)});
    controller.command(Untalk);
    controller.command(Listen(Device{DeviceAddress}));
    controller.command(Close(Channel{DataChannel}));
    controller.command(Unlisten);
    service_rx(system, firmware, "status-repeat CLOSE");
    controller.write(pet::ViaPortB, BusReleased);
    const std::string status = d80 ? "00, OK,00,00,0\r" : "00, OK,00,00\r";
    const std::string repeated = status + status;
    const std::vector<uint8_t> expected(repeated.begin(), repeated.end());
    constexpr econopet::Cycles CoarseServicePeriod{64'000};
    {
        const auto period = coarse ? CoarseServicePeriod : RomServicePeriod;
        system.cpu().assert_reset();
        KernalFixture rom(system, test_rom::Set::Rom4);
        auto test = rom.program();
        const auto received = test.data("repeated status", NativePayload, expected.size(), test_asm::Capture::Yes);
        const auto flags = test.data("per-byte status", NativeLastStatus, expected.size(), test_asm::Capture::Yes);
        const auto result = test.within(TerminalRomBudget)
                                .every(period, ServiceTiming::AfterSlice, firmware.service_adapter())
                                .run([&](auto& p) {
                                    rom.init_ieee(p);
                                    rom.device(p);
                                    for (size_t i = 0; i < expected.size(); ++i) {
                                        p.lda(test_asm::imm(DeviceAddress))
                                            .jsr(rom.entry(KernalRoutine::Talk))
                                            .lda(test_asm::imm(IEEE_CMD_SECONDARY(CommandChannel)))
                                            .jsr(rom.entry(KernalRoutine::Tksa))
                                            .lda(test_asm::imm(0))
                                            .sta(test_asm::zp(KernalWorkspace::Status))
                                            .jsr(rom.entry(KernalRoutine::Acptr))
                                            .sta(received.byte(i))
                                            .lda(test_asm::zp(KernalWorkspace::Status))
                                            .sta(flags.byte(i))
                                            .jsr(rom.entry(KernalRoutine::Untalk));
                                    }
                                });
        EXPECT_EQ(result[received], expected);
        for (size_t i = 0; i < expected.size(); ++i)
            EXPECT_EQ(result[flags.byte(i)], (i + 1) % status.size() == 0 ? 0x40 : 0);
    }
    service_rx(system, firmware, "ROM status final UNTALK");
}

INSTANTIATE_TEST_SUITE_P(Models, DriveTerminalTest, testing::Bool());
INSTANTIATE_TEST_SUITE_P(ModelsAndServicePeriods, DriveRepeatedStatusTest,
                         testing::Combine(testing::Bool(), testing::Bool()));

// A healthy full NEW must not be mistaken for an absent status-channel talker.
TEST_P(DriveFormatWaitTest, Rom4FullFormatStatusWaitsBeyondOneTimerPeriod) {
    constexpr econopet::Cycles FormatServicePeriod{8000};
    constexpr econopet::Cycles FormatReadBudget{12'000'000};
    constexpr uint8_t StopKeyRetry = 0x80, OneTimerTimeout = 0;
    constexpr uint8_t InputTimeout = 0x02, TimeoutByte = 0x0d;
    constexpr uint8_t KeyNotPressed = 0xff;
    constexpr econopet::CpuAddress StopKeyState{0x009b};
    const std::string status = "00, OK,00,00\r";
    const std::string command = "N0:ACCEPT0,A0";
    const uint8_t policy = GetParam() ? StopKeyRetry : OneTimerTimeout;

    // Both policies see identical cooperative format work through real SPI/FPGA.
    firmware.mount(D64::empty(), "format-wait.d64");
    system.cpu().assert_reset();
    KernalFixture rom(system, test_rom::Set::Rom4);
    auto test = rom.program();
    const auto count = policy == OneTimerTimeout ? size_t{1} : status.size();
    const auto received = test.data("format status", NativePayload, count, test_asm::Capture::Yes);
    const auto flags = test.data("format status flags", NativeLastStatus, count, test_asm::Capture::Yes);
    const auto result = test.within(FormatReadBudget)
                            .every(FormatServicePeriod, ServiceTiming::AfterSlice, firmware.service_adapter())
                            .run([&](auto& p) {
                                rom.init_ieee(p);
                                rom.device(p);
                                p.lda(test_asm::imm(policy))
                                    .sta(KernalWorkspace::IeeeTimeoutControl)
                                    .lda(test_asm::imm(KeyNotPressed))
                                    .sta(StopKeyState)
                                    .jsr(rom.entry(KernalRoutine::Listen))
                                    .lda(test_asm::imm(IEEE_CMD_SECONDARY(CommandChannel)))
                                    .jsr(rom.entry(KernalRoutine::Secondary));
                                for (const uint8_t byte : command) {
                                    p.lda(test_asm::imm(byte)).jsr(rom.entry(KernalRoutine::Ciout));
                                }
                                p.jsr(rom.entry(KernalRoutine::Unlisten))
                                    .jsr(rom.entry(KernalRoutine::Talk))
                                    .lda(test_asm::imm(IEEE_CMD_SECONDARY(CommandChannel)))
                                    .jsr(rom.entry(KernalRoutine::Tksa));
                                for (size_t i = 0; i < count; ++i) {
                                    p.lda(test_asm::imm(0))
                                        .sta(test_asm::zp(KernalWorkspace::Status))
                                        .jsr(rom.entry(KernalRoutine::Acptr))
                                        .sta(received.byte(i))
                                        .lda(test_asm::zp(KernalWorkspace::Status))
                                        .sta(flags.byte(i));
                                }
                                p.jsr(rom.entry(KernalRoutine::Untalk));
                            });
    if (policy == OneTimerTimeout) {
        EXPECT_EQ(result[received.byte(0)], TimeoutByte);
        EXPECT_EQ(result[flags.byte(0)], InputTimeout);
    } else {
        EXPECT_EQ(result[received], std::vector<uint8_t>(status.begin(), status.end()));
        for (size_t i = 0; i < count; ++i) {
            EXPECT_EQ(result[flags.byte(i)], i + 1 == count ? 0x40 : 0);
        }
    }
    // A receive timeout does not cancel NEW, so retire its work before teardown.
    constexpr size_t FormatRecoveryTurns = 128;
    for (size_t turn = 0; turn < DISKIMAGE_D64_SIZE / DISKIMAGE_SECTOR_SIZE + FormatRecoveryTurns; ++turn) {
        firmware.service();
    }
    service_rx(system, firmware, "full NEW final UNTALK");
}

INSTANTIATE_TEST_SUITE_P(TimeoutPolicies, DriveFormatWaitTest, testing::Bool());

// Exercise GET#'s channel vectors across the power-on to OK transition, without file I/O.
TEST_P(DriveColdStatusTest, Rom4ColdStatusGetRetainsCrAndEoi) {
    const auto [format, coarse] = GetParam();
    const bool d80 = format != 0;
    firmware.mount(lifecycle_image(format, {0x42, 0x43}), "cold.img");
    const std::string startup = d80 ? "73,CBM DOS V2.7,00,00,0\r" : "73,CBM DOS V2,00,00\r";
    const std::string ok = d80 ? "00, OK,00,00,0\r" : "00, OK,00,00\r";
    const std::string expected = startup + ok + ok;
    constexpr econopet::Cycles CoarseServicePeriod{64'000};
    constexpr uint8_t OneOpenFile = 1, ScreenDevice = 3;
    system.cpu().assert_reset();
    KernalFixture rom(system, test_rom::Set::Rom4);
    auto test = rom.program();
    const auto received = test.data("cold and repeated status", NativePayload, expected.size(), test_asm::Capture::Yes);
    const auto flags = test.data("per-byte status", NativeLastStatus, expected.size(), test_asm::Capture::Yes);
    const auto result = test.within(TerminalRomBudget)
                            .every(coarse ? CoarseServicePeriod : RomServicePeriod, ServiceTiming::AfterSlice,
                                   firmware.service_adapter())
                            .run([&](auto& p) {
                                // OPEN channel 15 without a command, then install its logical-file entry.
                                rom.init_ieee(p);
                                rom.device(p);
                                p.lda(test_asm::imm(CommandChannel))
                                    .sta(test_asm::zp(KernalWorkspace::Secondary))
                                    .lda(test_asm::imm(0))
                                    .sta(test_asm::zp(KernalWorkspace::FilenameLength))
                                    .jsr(rom.entry(KernalRoutine::Openi))
                                    .lda(test_asm::imm(OneOpenFile))
                                    .sta(test_asm::zp(KernalWorkspace::FileCount))
                                    .lda(test_asm::imm(CommandChannel))
                                    .sta(KernalWorkspace::LogicalFiles)
                                    .lda(test_asm::imm(DeviceAddress))
                                    .sta(KernalWorkspace::Devices)
                                    .lda(test_asm::imm(IEEE_CMD_SECONDARY(CommandChannel)))
                                    .sta(KernalWorkspace::Secondaries)
                                    .lda(test_asm::imm(ScreenDevice))
                                    .sta(test_asm::zp(KernalWorkspace::DefaultOutput));
                                // GETIN clears ST itself, unlike a directly called ACPTR.
                                for (size_t i = 0; i < expected.size(); ++i) {
                                    p.ldx(test_asm::imm(CommandChannel))
                                        .jsr(rom.entry(KernalRoutine::Chkin))
                                        .jsr(rom.entry(KernalRoutine::Getin))
                                        .sta(received.byte(i))
                                        .lda(test_asm::zp(KernalWorkspace::Status))
                                        .sta(flags.byte(i))
                                        .jsr(rom.entry(KernalRoutine::Clrchn));
                                }
                            });
    EXPECT_EQ(result[received], std::vector<uint8_t>(expected.begin(), expected.end()));
    for (size_t i = 0; i < expected.size(); ++i)
        EXPECT_EQ(result[flags.byte(i)], expected[i] == '\r' ? 0x40 : 0) << "status byte " << i;
}

INSTANTIATE_TEST_SUITE_P(ModelsAndServicePeriods, DriveColdStatusTest,
                         testing::Combine(testing::Range(0, 3), testing::Bool()));

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
    firmware.mount(d80 ? std::vector<uint8_t>(DISKIMAGE_D80_SIZE) : D64::empty().bytes(), name);
    ASSERT_TRUE(ieee_drive_mount(slot, name));
    ASSERT_TRUE(ieee_drive_mount(peer_slot, name));
    open_file(system, controller, Channel{DataChannel}, std::to_string(drive) + ":MISSING", device);
    service_rx(system, firmware, "selected-unit missing file");
    open_file(system, controller, Channel{DataChannel}, std::to_string(peer_drive) + ":MISSING", peer);
    service_rx(system, firmware, "peer-unit missing file");
    const std::string suffix = d80 ? "," + std::to_string(drive) : "";
    const std::string peer_suffix = d80 ? "," + std::to_string(peer_drive) : "";
    EXPECT_EQ(receive_status(system, firmware, controller, device), "62, FILE NOT FOUND,00,00" + suffix + "\r");
    EXPECT_EQ(receive_status(system, firmware, controller, peer), "62, FILE NOT FOUND,00,00" + peer_suffix + "\r");
    EXPECT_EQ(receive_status(system, firmware, controller, device), "00, OK,00,00" + suffix + "\r");
}

INSTANTIATE_TEST_SUITE_P(ModelsUnitsDrives, DriveStatusTest,
                         testing::Combine(testing::Bool(),
                                          testing::Values(uint8_t{8}, uint8_t{9}, uint8_t{10}, uint8_t{11}),
                                          testing::Values(uint8_t{0}, uint8_t{1})));

// Excluded direct syntax fails while native REL type checks reject a PRG mismatch.
TEST_F(FirmwareTest, UnimplementedDriveFeaturesCannotFallBack) {
    constexpr Device Drive{DeviceAddress};
    IeeeController controller(system);
    firmware.mount(D64{}.prg("BASIC", {0x42, 0x43}));
    for (const auto name : {"#A", "BASIC,L"}) {
        open_file(system, controller, Channel{DataChannel}, name);
        service_rx(system, firmware, "unsupported OPEN");
        EXPECT_EQ(receive_status(system, firmware, controller, Drive),
                  name == std::string_view{"#A"} ? "30,SYNTAX ERROR,00,00\r" : "64, FILE TYPE MISMATCH,00,00\r");
        controller.command(Talk(Drive));
        controller.command(Secondary(Channel{DataChannel}));
        service_rx(system, firmware, "unsupported TALK");
        expect_no_payload(system, firmware, controller);
        controller.command(Untalk);
        service_rx(system, firmware, "unsupported UNTALK");
    }
    open_file(system, controller, Channel{DataChannel}, "B?SIC");
    service_rx(system, firmware, "wildcard OPEN");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), "00, OK,00,00\r");
    controller.command(Listen(Drive));
    controller.command(Close(Channel{DataChannel}));
    controller.command(Unlisten);
    service_rx(system, firmware, "wildcard CLOSE");
    open_file(system, controller, Channel{DataChannel}, "BASIC,PRG,W");
    service_rx(system, firmware, "existing writer OPEN");
    EXPECT_EQ(receive_status(system, firmware, controller, Drive), "63, FILE EXISTS,00,00\r");
}

// CPU-written VIA state still composes with the FPGA's external inputs.
TEST_F(FirmwareTest, CpuViaWriteAndInjectedRead) {
    constexpr uint8_t ViaIeeeIdle = pet::Ndac | pet::NrfdInput | pet::Dav | ViaIdle;
    IeeeController controller(system);
    firmware.mount(D64::empty());
    controller.write(pet::ViaPortB, ViaIdle);
    EXPECT_EQ(system.ieee().peek_via_orb(), ViaIdle);
    EXPECT_EQ(controller.read(pet::ViaPortB) & ViaIeeeIdle, ViaIeeeIdle);
}

// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <algorithm>
#include <array>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "controller.h"
#include "framework/firmware_test.h"
#include "trace.h"

namespace {
using namespace ieee_test;
constexpr Channel StatusChannel{CommandChannel};
constexpr Device Drive{DeviceAddress};
constexpr size_t StatusLimit = 80;
constexpr econopet::Cycles ServiceBudget{1000000};
// RAMVAR/EQUATE reserve jobs 12/13 for BAMs and $4300 for CMDBUF.
constexpr uint16_t SpareJob = 0x100e, SpareHeader = 0x1079, SpareBuffer = 0x4000;
constexpr uint8_t ReadJob = 0x80, WriteJob = 0x90, VerifyJob = 0xa0;

// Exercise the real soft-PET I/O, FPGA FIFO, physical SPI, ROM and FILE backend.
class Rom4040SystemTest : public FirmwareTest {
protected:
    IeeeController controller{system};

    // Drain queued wire events through normal production servicing.
    void drain() {
        system.execution().within(ServiceBudget)
            .service(firmware.service_adapter())
            .until([](const System& board) { return !board.snapshot().ieee_rx_ready_o; })
            .run("4040 RX drain");
    }

    // Submit native command-channel bytes, preserving binary operands and EOI.
    void command(const std::vector<uint8_t>& bytes) {
        controller.command(Listen(Drive));
        controller.command(Secondary(StatusChannel));
        for (size_t i = 0; i < bytes.size(); ++i)
            controller.data(bytes[i], i + 1 == bytes.size() ? Eoi::Last : Eoi::More);
        controller.command(Unlisten);
        drain();
    }

    // Write native RAM through the unmodified DOS M-W command.
    void memory_write(uint16_t address, const std::vector<uint8_t>& bytes) {
        ASSERT_LE(bytes.size(), 255u);
        std::vector<uint8_t> request{'M', '-', 'W', static_cast<uint8_t>(address),
            static_cast<uint8_t>(address >> 8), static_cast<uint8_t>(bytes.size())};
        request.insert(request.end(), bytes.begin(), bytes.end());
        command(request);
    }

    // Send a counted OPEN filename with the native final-byte EOI.
    void open(Channel channel, const std::string& name) {
        controller.command(Listen(Drive));
        controller.command(Open(channel));
        for (size_t i = 0; i < name.size(); ++i)
            controller.data(name[i], i + 1 == name.size() ? Eoi::Last : Eoi::More);
        controller.command(Unlisten);
        drain();
    }

    // Select a ROM source without directly touching FPGA TX registers.
    void talk(Channel channel = StatusChannel) {
        controller.command(Talk(Drive));
        controller.command(Secondary(channel));
        drain();
    }

    // Arm the PET acceptor, service until DAV, then let its CPU complete GET.
    Record receive() {
        controller.write(pet::Pia2ControlA, PiaC2Low.bits());
        controller.write(pet::ViaPortB, BusReleased);
        system.execution().within(ServiceBudget)
            .service(firmware.service_adapter())
            .until([](const System& board) { return !board.snapshot().ieee_bus_dav_no; })
            .run("4040 source DAV");
        return controller.receive();
    }

    // Terminate at the wire boundary and reconcile any last GET before commands.
    void untalk() {
        controller.command(Untalk);
        drain();
    }

    // Read a bounded native response, requiring CR and exactly final-byte EOI.
    std::string status() {
        talk();
        std::string text;
        for (size_t i = 0; i < StatusLimit; ++i) {
            const auto byte = receive();
            EXPECT_FALSE(byte.atn);
            text.push_back(static_cast<char>(byte.data));
            if (byte.eoi == Eoi::Last) {
                EXPECT_EQ(byte.data, 13);
                untalk();
                return text;
            }
        }
        ADD_FAILURE() << "native status did not terminate with CR/EOI";
        untalk();
        return text;
    }

    // M-R's count-one response intentionally lacks EOI in this selected ROM.
    uint8_t memory_read(uint16_t address) {
        command({'M', '-', 'R', static_cast<uint8_t>(address),
            static_cast<uint8_t>(address >> 8), 1});
        talk();
        const auto byte = receive();
        EXPECT_EQ(byte.eoi, Eoi::More);
        untalk();
        return byte.data;
    }

    // Submit a job by real 6502 stores made by DOS, not a C callback invocation.
    uint8_t job(uint8_t operation, uint8_t track, uint8_t sector) {
        memory_write(SpareHeader, {'0', '0', track, sector});
        memory_write(SpareJob, {operation});
        return memory_read(SpareJob);
    }

    // Read the independent raw backing fixture after controller writes flush.
    std::array<uint8_t, 256> backing_sector(unsigned track, unsigned sector) {
        std::array<uint8_t, 256> bytes{};
        FILE* file = std::fopen("/disks/test.d64", "rb");
        if (!file) throw std::runtime_error("cannot open mounted sector fixture");
        const bool ok = std::fseek(file, disk_fixture::D64::offset(track, sector), SEEK_SET) == 0
            && std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
        const bool closed = std::fclose(file) == 0;
        if (!ok || !closed) throw std::runtime_error("cannot read mounted sector fixture");
        return bytes;
    }
};

// Boot and ATN interrupts traverse PET PIA/VIA, IEEE RTL and the actual SPI pins.
TEST_F(Rom4040SystemTest, NativeStatusAndRiotMemoryAliasesCrossPhysicalSpi) {
    firmware.mount(disk_fixture::D64::empty());
    EXPECT_EQ(status(), "73,CBM DOS V2,00,00\r");
    EXPECT_EQ(status(), "00, OK,00,00\r");
    EXPECT_EQ(memory_read(0x1000), 0x0f);
    memory_write(SpareBuffer, {0xa5});
    EXPECT_EQ(memory_read(0xc400), 0xa5);
    // Device straps and resolved handshake inputs come from RIOT2 PB.
    EXPECT_EQ(memory_read(0x0282) & 7, 0);
    command({'I', '0'});
    EXPECT_EQ(status(), "00, OK,00,00\r");
}

// Check the complete CPU-store -> shared job -> controller -> disk -> ROM path.
TEST_F(Rom4040SystemTest, NativeJobsReadWriteVerifyAndErrorsCrossPhysicalSpi) {
    firmware.mount(disk_fixture::D64::empty());
    status();
    constexpr uint8_t Track = 1, Sector = 0;
    const auto original = backing_sector(Track, Sector);
    ASSERT_EQ(job(ReadJob, Track, Sector), 1);
    EXPECT_EQ(memory_read(SpareBuffer + 73), original[73]);
    memory_write(SpareBuffer + 73, {0x5a});
    EXPECT_EQ(job(VerifyJob, Track, Sector), 7);
    EXPECT_EQ(job(WriteJob, Track, Sector), 1);
    auto expected = original;
    expected[73] = 0x5a;
    EXPECT_EQ(backing_sector(Track, Sector), expected);
    EXPECT_EQ(job(VerifyJob, Track, Sector), 1);
    EXPECT_EQ(job(ReadJob, 36, 0), 3);
    EXPECT_EQ(job(ReadJob | 1, Track, Sector), 3);
    memory_write(SpareHeader, {'X', 'X', Track, Sector});
    memory_write(SpareJob, {ReadJob});
    EXPECT_EQ(memory_read(SpareJob), 11);
}

// Boot acknowledges controller parking, publishes ID and completes both native bump jobs.
TEST_F(Rom4040SystemTest, NativeStartupAndSeekBumpResultsUseSharedControllerProtocol) {
    firmware.mount(disk_fixture::D64::empty());
    EXPECT_EQ(status(), "73,CBM DOS V2,00,00\r");
    EXPECT_EQ(memory_read(0x1000), 0x0f);
    EXPECT_EQ(memory_read(0x1003), 1);
    EXPECT_EQ(memory_read(0x1004), 1);
    EXPECT_EQ(job(0xb0, 0, 0), 1);
    EXPECT_EQ(memory_read(SpareHeader), '0');
    EXPECT_EQ(memory_read(SpareHeader + 1), '0');
    EXPECT_EQ(memory_read(SpareHeader + 3), 1);
    EXPECT_EQ(job(0xc0, 0, 0), 1);
    EXPECT_EQ(memory_read(SpareHeader + 2), 0);
}

// Low-bit drive selection must survive native commands, shared mailboxes and FILE routing.
TEST_F(Rom4040SystemTest, BothMountedDrivesKeepJobsMediaAndControllerIdentityIndependent) {
    firmware.mount(disk_fixture::D64::empty(), "first.d64");
    firmware.mount(disk_fixture::D64::empty(), "second.d64", 1);
    EXPECT_EQ(status(), "73,CBM DOS V2,00,00\r");
    EXPECT_EQ(memory_read(0x1003), 1);
    EXPECT_EQ(memory_read(0x1004), 1);
    for (unsigned drive = 0; drive < 2; ++drive) {
        memory_write(SpareBuffer, {static_cast<uint8_t>(0x5a + drive)});
        EXPECT_EQ(job(WriteJob | drive, 1, 0), 1);
    }
    for (unsigned drive = 0; drive < 2; ++drive) {
        EXPECT_EQ(job(ReadJob | drive, 1, 0), 1);
        EXPECT_EQ(memory_read(SpareBuffer), 0x5a + drive);
        EXPECT_EQ(job(VerifyJob | drive, 1, 0), 1);
    }
}

// The original ROM downloads its formatter, then C performs only raw controller work.
TEST_F(Rom4040SystemTest, NativeFormatterAndSeekPublishExactMediaAndNewDiskIdentity) {
    firmware.mount(disk_fixture::D64::empty());
    status();
    const std::string format = "N0:PROTOCOL,NX";
    command(std::vector<uint8_t>(format.begin(), format.end()));
    EXPECT_EQ(status(), "00, OK,00,00\r");
    const auto cleared = backing_sector(1, 0);
    EXPECT_TRUE(std::all_of(cleared.begin(), cleared.end(), [](uint8_t byte) { return byte == 0; }));
    const auto bam = backing_sector(18, 0);
    EXPECT_EQ(bam[0], 18);
    EXPECT_EQ(bam[1], 1);
    EXPECT_EQ(bam[0xa2], 'N');
    EXPECT_EQ(bam[0xa3], 'X');
    EXPECT_EQ(job(0xb0, 18, 0), 1);
    EXPECT_EQ(memory_read(SpareHeader), 'N');
    EXPECT_EQ(memory_read(SpareHeader + 1), 'X');
    memory_write(SpareHeader, {'N', 'X', 1, 0});
    memory_write(SpareJob, {ReadJob});
    EXPECT_EQ(memory_read(SpareJob), 1);
}

// All byte values traverse the real listener, native PUT, controller WRITE and talker GET.
TEST_F(Rom4040SystemTest, EveryBinaryByteSurvivesListenerAndTalkerPhysicalSpiWithExactEoi) {
    firmware.mount(disk_fixture::D64::empty());
    status();
    constexpr Channel File{2};
    open(File, "0:WIRE,S,W");
    controller.command(Listen(Drive));
    controller.command(Secondary(File));
    // Keep batches below the physical 32-entry RX depth, including event tokens.
    constexpr unsigned ListenerBatch = 16;
    for (unsigned byte = 0; byte < 256; ++byte) {
        controller.data(byte, byte == 255 ? Eoi::Last : Eoi::More);
        if ((byte + 1) % ListenerBatch == 0) drain();
    }
    controller.command(Unlisten);
    drain();
    controller.command(Listen(Drive));
    controller.command(Close(File));
    controller.command(Unlisten);
    drain();
    EXPECT_EQ(status(), "00, OK,00,00\r");
    open(File, "0:WIRE,S,R");
    auto wire = system.trace_ieee();
    talk(File);
    for (unsigned byte = 0; byte < 256; ++byte)
        ASSERT_EQ(receive(), data(byte, byte == 255 ? Eoi::Last : Eoi::More));
    untalk();
    std::vector<Record> expected{ieee_test::command(Talk(Drive)), ieee_test::command(Secondary(File))};
    for (unsigned byte = 0; byte < 256; ++byte)
        expected.push_back(data(byte, byte == 255 ? Eoi::Last : Eoi::More));
    expected.push_back(ieee_test::command(Untalk));
    require_sequence(wire.records(), expected, "all binary byte values through native ROM");
}

// Compare the two native abort sides without varying DOS semantics.
class Rom4040AbortSystemTest : public Rom4040SystemTest,
                              public testing::WithParamInterface<bool> {};

// One outstanding ROM byte GETs on NRFD, not enqueue, presentation or private read.
TEST_P(Rom4040AbortSystemTest, NativeGetTracksNrfdRatherThanQueuedOrSampledData) {
    const std::vector<uint8_t> payload{'A', 'B', 'C'};
    firmware.mount(disk_fixture::D64{}.prg("PROBE", payload));
    status();
    controller.command(Listen(Drive));
    controller.command(Open(Channel{LoadChannel}));
    const std::string name = "0:PROBE";
    for (size_t i = 0; i < name.size(); ++i)
        controller.data(name[i], i + 1 == name.size() ? Eoi::Last : Eoi::More);
    controller.command(Unlisten);
    drain();
    talk(Channel{LoadChannel});
    controller.write(pet::Pia2ControlA, PiaC2Low.bits());
    controller.write(pet::ViaPortB, BusReleased);
    system.execution().within(ServiceBudget).service(firmware.service_adapter())
        .until([](const System& board) { return !board.snapshot().ieee_bus_dav_no; })
        .run("hold unread native byte");
    const auto before = ieee_drive_metrics();
    EXPECT_EQ(static_cast<uint8_t>(~controller.read(pet::Pia2PortA)), 'A');
    for (unsigned pass = 0; pass < 3; ++pass) firmware.service();
    const auto stalled = ieee_drive_metrics();
    EXPECT_EQ(stalled.consumed, before.consumed);
    EXPECT_EQ(stalled.presented, before.presented);
    if (GetParam()) {
        controller.write(pet::ViaPortB, static_cast<uint8_t>(BusReleased & ~pet::Nrfd));
        // ATN is next, with no intervening firmware service. Its RX processing
        // must reconcile the preceding hardware GET before discarding the offer.
    }
    untalk();
    talk(Channel{LoadChannel});
    for (size_t i = GetParam() ? 1 : 0; i < payload.size(); ++i)
        EXPECT_EQ(receive(), data(payload[i], i + 1 == payload.size() ? Eoi::Last : Eoi::More));
    untalk();
}

INSTANTIATE_TEST_SUITE_P(NativeGetBoundary, Rom4040AbortSystemTest, testing::Bool());

// Reset must clear both the native machine and outstanding physical FIFO transactions.
class Rom4040ResetSystemTest : public Rom4040SystemTest,
                              public testing::WithParamInterface<unsigned> {};

// Restart from idle, partial RX, unread DAV, consumed DAV and queued ATN without stale GET.
TEST_P(Rom4040ResetSystemTest, ColdResetDiscardsEveryPendingTransportPhaseAndRebootsOriginalRom) {
    firmware.mount(disk_fixture::D64::empty());
    status();
    if (GetParam() == 1) {
        controller.command(Listen(Drive));
        controller.command(Open(Channel{2}));
        controller.data('0');
    } else if (GetParam() >= 2) {
        talk();
        controller.write(pet::Pia2ControlA, PiaC2Low.bits());
        controller.write(pet::ViaPortB, BusReleased);
        system.execution().within(ServiceBudget).service(firmware.service_adapter())
            .until([](const System& board) { return !board.snapshot().ieee_bus_dav_no; })
            .run("reset with source DAV pending");
        if (GetParam() == 3)
            controller.write(pet::ViaPortB, static_cast<uint8_t>(BusReleased & ~pet::Nrfd));
        if (GetParam() == 4) controller.command(Untalk);
    }
    ieee_drive_reset();
    EXPECT_EQ(ieee_drive_metrics().consumed, 0u);
    EXPECT_EQ(ieee_drive_metrics().presented, 0u);
    controller.write(pet::Pia2ControlA, PiaC2High.bits());
    controller.write(pet::ViaPortB, BusReleased);
    EXPECT_EQ(status(), "73,CBM DOS V2,00,00\r");
    EXPECT_EQ(status(), "00, OK,00,00\r");
    EXPECT_EQ(memory_read(0x1000), 0x0f);
}

INSTANTIATE_TEST_SUITE_P(TransportPhase, Rom4040ResetSystemTest, testing::Range(0u, 5u));

// Keep schedule seeds distinct from the board's initialization seed and report both.
class Rom4040ScheduleTest : public Rom4040SystemTest,
                            public testing::WithParamInterface<unsigned> {};

// Random bounded RX bursts and service gaps must preserve every wire byte and its metadata.
TEST_P(Rom4040ScheduleTest, SeededListenerBurstsPreserveWireMetadataAndStoredPayload) {
    const unsigned seed = GetParam();
    SCOPED_TRACE("listener schedule seed=" + std::to_string(seed));
    std::mt19937 random(seed);
    constexpr Channel File{2};
    constexpr unsigned MaxBurst = 16, MaxServicePeriod = 4096;
    constexpr size_t PayloadSize = 64;
    std::vector<uint8_t> payload(PayloadSize);
    for (auto& byte : payload) byte = static_cast<uint8_t>(random());
    firmware.mount(disk_fixture::D64::empty());
    status();
    open(File, "0:SCHEDULE,S,W");
    auto wire = system.trace_ieee();
    controller.command(Listen(Drive));
    controller.command(Secondary(File));
    std::vector<Record> expected{ieee_test::command(Listen(Drive)),
        ieee_test::command(Secondary(File))};
    size_t sent = 0;
    while (sent < payload.size()) {
        const size_t end = std::min(payload.size(), sent + 1 + random() % MaxBurst);
        for (; sent < end; ++sent) {
            const auto eoi = sent + 1 == payload.size() ? Eoi::Last : Eoi::More;
            controller.data(payload[sent], eoi);
            expected.push_back(data(payload[sent], eoi));
        }
        system.execution().within(ServiceBudget)
            .every(1 + random() % MaxServicePeriod, ServiceTiming::AfterSlice,
                firmware.service_adapter())
            .until([](const System& board) { return !board.snapshot().ieee_rx_ready_o; })
            .run("seeded listener service gap");
    }
    controller.command(Unlisten);
    drain();
    controller.command(Listen(Drive));
    controller.command(Close(File));
    controller.command(Unlisten);
    drain();
    expected.push_back(ieee_test::command(Unlisten));
    expected.push_back(ieee_test::command(Listen(Drive)));
    expected.push_back(ieee_test::command(Close(File)));
    expected.push_back(ieee_test::command(Unlisten));
    require_sequence(wire.records(), expected, "seeded RX bursts and metadata");
    EXPECT_EQ(status(), "00, OK,00,00\r");
    open(File, "0:SCHEDULE,S,R");
    talk(File);
    for (size_t i = 0; i < payload.size(); ++i)
        ASSERT_EQ(receive(), data(payload[i], i + 1 == payload.size() ? Eoi::Last : Eoi::More));
    untalk();
}

// Service gaps, unread retries and GET-adjacent ATN must conserve native channel bytes.
TEST_P(Rom4040ScheduleTest, SeededServiceSchedulesPreserveOwnershipBytesAndGetConservation) {
    const unsigned seed = GetParam();
    SCOPED_TRACE("protocol schedule seed=" + std::to_string(seed));
    std::mt19937 random(seed);
    constexpr size_t PayloadSize = 24;
    constexpr unsigned MaxServicePeriod = 4096, MaxIdleGap = 2048;
    std::vector<uint8_t> payload(PayloadSize);
    for (auto& byte : payload) byte = static_cast<uint8_t>(random());
    firmware.mount(disk_fixture::D64{}.prg("SCHEDULE", payload));
    status();
    open(Channel{LoadChannel}, "0:SCHEDULE");
    const auto initial = ieee_drive_metrics();
    talk(Channel{LoadChannel});
    size_t index = 0;
    unsigned attempts = 0, unread_aborts = 0, consumed_aborts = 0;
    std::vector<uint8_t> consumed;
    constexpr unsigned AttemptLimit = PayloadSize * 4;
    while (index < payload.size() && attempts++ < AttemptLimit) {
        SCOPED_TRACE("attempt=" + std::to_string(attempts) + " offset=" + std::to_string(index));
        controller.write(pet::Pia2ControlA, PiaC2Low.bits());
        controller.write(pet::ViaPortB, BusReleased);
        const unsigned period = 1 + random() % MaxServicePeriod;
        // Firmware is intentionally scheduled independently of source and acceptor progress.
        system.execution().within(ServiceBudget)
            .every(period, ServiceTiming::AfterSlice, firmware.service_adapter())
            .until([](const System& board) { return !board.snapshot().ieee_bus_dav_no; })
            .run("seeded native source scheduling");
        const auto offered = ieee_drive_metrics();
        ASSERT_EQ(offered.consumed - initial.consumed, consumed.size());
        ASSERT_EQ(offered.presented - initial.presented, consumed.size() + unread_aborts + 1);
        ASSERT_EQ(static_cast<uint8_t>(~controller.read(pet::Pia2PortA)), payload[index]);
        const unsigned stall = random() % MaxIdleGap;
        bool gap_finished = false;
        system.execution().within(ServiceBudget)
            .after(stall, [&gap_finished](System&) { gap_finished = true; })
            .until([&gap_finished](const System&) { return gap_finished; })
            .run("seeded unread source gap");
        const unsigned passes = random() % 4;
        for (unsigned pass = 0; pass < passes; ++pass) firmware.service();
        ASSERT_EQ(ieee_drive_metrics().consumed, offered.consumed);
        ASSERT_EQ(ieee_drive_metrics().presented, offered.presented);

        // Bound retries so every seed exercises liveness as well as conservation.
        const unsigned action = attempts % 3 == 0 ? 2 : random() % 3;
        if (action == 0 && index + 1 < payload.size()) {
            untalk();
            ++unread_aborts;
            ASSERT_EQ(ieee_drive_metrics().consumed, offered.consumed);
            talk(Channel{LoadChannel});
        } else if (action == 1 && index + 1 < payload.size()) {
            // Actual NRFD acceptance followed immediately by ATN, without MCU service.
            controller.write(pet::ViaPortB, static_cast<uint8_t>(BusReleased & ~pet::Nrfd));
            consumed.push_back(payload[index++]);
            untalk();
            ++consumed_aborts;
            ASSERT_EQ(ieee_drive_metrics().consumed - initial.consumed, consumed.size());
            talk(Channel{LoadChannel});
        } else {
            ASSERT_EQ(controller.receive(),
                data(payload[index], index + 1 == payload.size() ? Eoi::Last : Eoi::More));
            consumed.push_back(payload[index++]);
            firmware.service();
            ASSERT_EQ(ieee_drive_metrics().consumed - initial.consumed, consumed.size());
        }
    }
    untalk();
    EXPECT_EQ(index, payload.size());
    EXPECT_EQ(consumed, payload);
    EXPECT_GT(unread_aborts, 0u);
    EXPECT_GT(consumed_aborts, 0u);
    EXPECT_EQ(ieee_drive_metrics().consumed - initial.consumed, payload.size());
    EXPECT_EQ(ieee_drive_metrics().presented - initial.presented, payload.size() + unread_aborts);
}

INSTANTIATE_TEST_SUITE_P(Adversarial, Rom4040ScheduleTest,
    testing::Values(1u, 0x4040u, 0xc0ffeeu));

// Keep native PET transport coverage small: one OPEN/read spans all real boundaries.
class Rom4040KernalTest : public FirmwareTest,
                          public testing::WithParamInterface<test_rom::Set> {};

// Installed KERNAL instructions, not a C handshake model, consume the ROM's bytes.
TEST_P(Rom4040KernalTest, NativeOpenAndReadUseOriginalPetKernal) {
    const std::vector<uint8_t> payload{0x01, 0x04, 0xa5, 0x5a};
    constexpr uint8_t filename[] = {'P', 'R', 'O', 'B', 'E'};
    constexpr econopet::CpuAddress Payload{0x0800}, Flags{0x0900};
    firmware.mount(disk_fixture::D64{}.prg("PROBE", payload));
    KernalFixture rom(system, GetParam());
    auto test = rom.program();
    const auto received = test.data("received payload", Payload, payload.size(), test_asm::Capture::Yes);
    const auto flags = test.data("status", Flags, payload.size(), test_asm::Capture::Yes);
    auto wire = system.trace_ieee();
    const auto result = test.within(KernalBudget)
        .every(1000, ServiceTiming::AfterSlice, firmware.service_adapter())
        .run([&](auto& p) {
            rom.init_ieee(p);
            rom.device(p);
            rom.file(p, filename, LoadChannel);
            p.jsr(rom.entry(KernalRoutine::Openi))
             .lda(test_asm::imm(DeviceAddress)).jsr(rom.entry(KernalRoutine::Talk))
             .lda(test_asm::imm(0x60 | LoadChannel)).jsr(rom.entry(KernalRoutine::Tksa));
            for (size_t i = 0; i < payload.size(); ++i)
                p.jsr(rom.entry(KernalRoutine::Acptr)).sta(received.byte(i))
                 .lda(test_asm::zp(KernalWorkspace::Status)).sta(flags.byte(i));
            p.jsr(rom.entry(KernalRoutine::Untalk));
        });
    EXPECT_EQ(result[received], payload);
    EXPECT_EQ(result[flags], (std::vector<uint8_t>{0, 0, 0, 0x40}));
    std::vector<Record> expected{command(Listen(Drive)), command(Open(Channel{LoadChannel}))};
    for (size_t i = 0; i < sizeof(filename); ++i)
        expected.push_back(data(filename[i], i + 1 == sizeof(filename) ? Eoi::Last : Eoi::More));
    expected.insert(expected.end(), {command(Unlisten), command(Talk(Drive)),
        command(Secondary(Channel{LoadChannel}))});
    for (size_t i = 0; i < payload.size(); ++i)
        expected.push_back(data(payload[i], i + 1 == payload.size() ? Eoi::Last : Eoi::More));
    expected.push_back(command(Untalk));
    require_sequence(wire.records(), expected, "native PET/4040 ROM OPEN/read");
}

INSTANTIATE_TEST_SUITE_P(Pet, Rom4040KernalTest,
    testing::Values(test_rom::Set::Upgrade, test_rom::Set::Rom4));
} // namespace

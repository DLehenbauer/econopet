// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>

#include "fifo.h"
#include "fixture.h"
#include "framework/asm.h"
#include "framework/firmware_test.h"
#include "trace.h"

extern "C" {
#include "ieee/ieee_protocol.h"
}

namespace {
using econopet::io::Via6522;
using test_asm::imm;
using test_asm::zp;
using namespace ieee_test;

constexpr auto KernalStatus = KernalWorkspace::Status;
constexpr auto BufferedByteState = KernalWorkspace::BufferedByte;
constexpr auto SourceByte = KernalWorkspace::SourceByte;
constexpr auto FileCount = KernalWorkspace::FileCount;
constexpr auto DefaultInputDevice = KernalWorkspace::DefaultInput;
constexpr auto DefaultOutputDevice = KernalWorkspace::DefaultOutput;
constexpr auto SecondaryAddress = KernalWorkspace::Secondary;
constexpr auto LogicalFileTable = KernalWorkspace::LogicalFiles;
constexpr auto DeviceFileTable = KernalWorkspace::Devices;
constexpr auto SecondaryFileTable = KernalWorkspace::Secondaries;
constexpr uint8_t InputTimeout = 0x02;
enum DefaultDevice : uint8_t { KeyboardDevice = 0, ScreenDevice = 3 };
constexpr uint8_t RecoveryAccumulator = 0x0d;
constexpr uint8_t LogicalFile = 1;
constexpr uint8_t OneOpenFile = 1;
constexpr uint8_t FileSecondary = IEEE_CMD_SECONDARY(FileChannel);
constexpr size_t CommandPayloadCapacity = 0xff;
constexpr econopet::Cycles BufferedByteBudget = 100000;
constexpr econopet::Cycles ResetFetchBudget = 20000;

// Share firmware ownership, mounted media, seed reporting and failure diagnostics.
class CommodoreRomTest : public FirmwareTest {
  protected:
    // Enable the peripheral without servicing drive logic during native ROM calls.
    void SetUp() override {
        FirmwareTest::SetUp();
        firmware.mount(disk_fixture::D64::empty(), "kernal.d64");
    }
};

// Install the selected revision only when a parameterized test actually runs.
class CommodoreRomIeee : public CommodoreRomTest, public testing::WithParamInterface<System::RomSet> {
  protected:
    // Mount media before installing the revision-bound native ROM fixture.
    void SetUp() override {
        CommodoreRomTest::SetUp();
        rom_.emplace(system, GetParam());
    }

    // Access the ROM installed by successful fixture setup.
    KernalFixture& selected_rom() { return rom_.value(); }

  private:
    std::optional<KernalFixture> rom_;
};

// Run fixed-revision transport cases with the same firmware lifecycle.
class CommodoreRomRevision : public CommodoreRomTest {};

// Give both parameterized suites stable names for their two selected revisions.
std::string rom_revision_name(const testing::TestParamInfo<System::RomSet>& info) {
    return info.param == System::RomSet::Upgrade ? "Rom2" : "Rom4";
}

// Executes a prepared KERNAL leaf entry and verifies that it returns to C++'s marker.
uint8_t run_kernal_leaf(KernalFixture& rom, KernalRoutine routine, uint8_t via_orb = ViaIdle,
                        uint8_t accumulator = 0x00,
                        std::initializer_list<std::pair<econopet::CpuAddress, uint8_t>> state = {}) {
    auto test = rom.program();
    const auto result = test.byte("leaf");
    const auto execution = test.within(KernalBudget).run([&](auto& call) {
        rom.init_ieee(call, via_orb);
        rom.device(call);
        for (const auto [address, value] : state)
            call.lda(imm(value)).sta(address);
        call.lda(imm(accumulator)).sta(zp(SourceByte)).jsr(rom.entry(routine)).sta(result);
    });
    return execution[result];
}

// Drain byte/ATN evidence, then independently check captured wire flags including EOI.
void require_transaction(System& system, const WireTrace& wire, const std::vector<Record>& expected,
                         const std::string& reason) {
    require_drained_sequence(system, expected, ByteBudget, reason);
    if (const auto mismatch = sequence_mismatch(wire.records(), expected))
        throw system.failure<std::runtime_error>("IEEE wire sequence", reason + ": " + *mismatch);
}

// Executes an addressing/termination primitive and checks its exact IEEE byte.
void run_command_case(System& system, KernalFixture& rom, KernalRoutine routine, uint8_t accumulator,
                      Command expected) {
    const auto diagnostic = rom.name(routine);
    const auto name = diagnostic.c_str();
    auto wire = system.trace_ieee();
    run_kernal_leaf(rom, routine, ViaIdle, accumulator);
    require_transaction(system, wire, {command(expected)}, name);
}

// Verifies a role-dependent sequence against the production FPGA RX FIFO.
void run_sequence_command_case(System& system, KernalFixture& rom,
                               std::initializer_list<std::pair<KernalRoutine, uint8_t>> calls,
                               std::initializer_list<Record> expected) {
    if (calls.size() == 0)
        throw system.failure<std::invalid_argument>("KERNAL sequence", "requires at least one routine");
    std::string diagnostic;
    for (const auto [routine, accumulator] : calls) {
        if (!diagnostic.empty())
            diagnostic += "/";
        diagnostic += rom.name(routine);
    }
    const auto name = diagnostic.c_str();
    auto wire = system.trace_ieee();
    // Keep dependent calls in one CPU session without resetting IEEE roles.
    auto test = rom.program();
    test.within(KernalBudget).run([&](auto& program) {
        rom.init_ieee(program);
        rom.device(program);
        for (const auto [routine, accumulator] : calls)
            program.lda(imm(accumulator)).jsr(rom.entry(routine));
    });
    require_transaction(system, wire, expected, name);
}

// Verifies a direct line-state entry without requiring a device handshake.
void run_line_case(System& system, KernalFixture& rom, KernalRoutine routine, uint8_t via_orb, uint8_t expected_orb,
                   uint8_t mask) {
    const auto diagnostic = rom.name(routine);
    const auto name = diagnostic.c_str();
    run_kernal_leaf(rom, routine, via_orb);
    if ((system.ieee().peek_via_orb() & mask) != expected_orb) {
        throw std::runtime_error(std::string("PET KERNAL ") + name + " set VIA ORB to " +
                                 std::to_string(system.ieee().peek_via_orb()) + " instead of " +
                                 std::to_string(expected_orb));
    }
    require_drained_sequence(system, {}, ByteBudget, name);
}

// Verifies ACPTR's timeout result under an explicit stuck external T1 flag fault.
TEST_P(CommodoreRomIeee, ACPTR_timeout) {
    auto& rom = selected_rom();
    const auto diagnostic = rom.name(KernalRoutine::Acptr);
    const auto name = diagnostic.c_str();
    system.io().via().set_timer1_fault(Via6522::Timer1Fault::StuckInterruptFlag);
    auto workspace = rom.lower_bank(rom.region("zero page"));
    const auto status = workspace.byte("SATUS", KernalStatus.value());
    const auto result = run_kernal_leaf(rom, KernalRoutine::Acptr);
    if (workspace.read(status) != InputTimeout || result != RecoveryAccumulator) {
        throw std::runtime_error(std::string("PET KERNAL ") + name + " did not report input timeout in SATUS");
    }
}

// Checks the receive-state setup entry against real PIA/VIA pins and ieee.sv snooping.
TEST_P(CommodoreRomIeee, ER001) {
    auto& rom = selected_rom();
    const auto diagnostic = rom.name(KernalRoutine::Er001);
    const auto name = diagnostic.c_str();
    run_kernal_leaf(rom, KernalRoutine::Er001, pet::Nrfd);
    if ((system.ieee().peek_via_orb() & pet::Atn) != 0 || system.ieee().peek_nrfd_released() ||
        system.ieee().peek_ndac_released()) {
        throw std::runtime_error(std::string("PET KERNAL ") + name + " did not establish IEEE receive state (ORB=" +
                                 std::to_string(system.ieee().peek_via_orb()) +
                                 ", NRFD_n=" + std::to_string(system.ieee().peek_nrfd_released()) +
                                 ", NDAC_n=" + std::to_string(system.ieee().peek_ndac_released()) + ")");
    }
}

// Runs LISTEN/secondary addressing, pauses, then sends one ISOUR byte in the
// same CPU session so the FPGA's listener state is preserved across the pause.
void run_isour_transmit_case(System& system, KernalFixture& scenario) {
    const auto diagnostic = scenario.name(KernalRoutine::Isour);
    const auto name = diagnostic.c_str();
    auto test = scenario.program(test_asm::ProgramMode::Phased);
    auto wire = system.trace_ieee();
    auto addressed = test.within(KernalBudget).pause([&](auto& p) {
        scenario.init_ieee(p);
        scenario.device(p);
        p.lda(imm(0x00))
            .jsr(scenario.entry(KernalRoutine::Listen))
            .lda(imm(IEEE_CMD_SECONDARY(FileChannel)))
            .jsr(scenario.entry(KernalRoutine::Secondary));
        p.pause("listener addressed");
        p.lda(imm(0x55)).sta(zp(SourceByte)).jsr(scenario.entry(KernalRoutine::Isour));
    });
    const std::vector<Record> addressing{command(Listen(Device{DeviceAddress})),
                                         command(Secondary(Channel{FileChannel}))};
    require_transaction(system, wire, addressing, name);

    const auto transmitted = test.resume(addressed, ByteBudget);
    if (!transmitted.complete()) {
        throw system.failure<std::runtime_error>(name, "did not complete the IEEE output handshake");
    }
    require_drained_sequence(system, {data(0x55)}, ByteBudget, name);
    auto expected = addressing;
    expected.push_back(data(0x55));
    require_sequence(wire.records(), expected, name);
}

// Verifies CIOUT's one-byte buffer and UNLSN's final-byte EOI flush.
void run_ciout_case(System& system, KernalFixture& rom) {
    const auto diagnostic = rom.name(KernalRoutine::Ciout);
    const auto name = diagnostic.c_str();
    SCOPED_TRACE(diagnostic);
    auto test = rom.program(test_asm::ProgramMode::Phased);
    auto wire = system.trace_ieee();
    auto addressed = test.within(KernalBudget).pause([&](auto& p) {
        rom.init_ieee(p);
        rom.device(p);
        p.lda(imm(0x00))
            .jsr(rom.entry(KernalRoutine::Listen))
            .lda(imm(IEEE_CMD_SECONDARY(FileChannel)))
            .jsr(rom.entry(KernalRoutine::Secondary));
        p.pause("listener addressed");
        p.lda(imm(0x00)).sta(zp(BufferedByteState)).lda(imm(0x41)).jsr(rom.entry(KernalRoutine::Ciout));
        p.pause("first byte buffered");
        p.lda(imm(0x42)).jsr(rom.entry(KernalRoutine::Ciout));
        p.pause("first byte sent");
        p.jsr(rom.entry(KernalRoutine::Unlisten));
    });
    ASSERT_EQ(addressed.name(), "listener addressed");
    std::vector<Record> expected{command(Listen(Device{DeviceAddress})), command(Secondary(Channel{FileChannel}))};
    require_transaction(system, wire, expected, name);
    auto buffered = test.resume(addressed, BufferedByteBudget);
    ASSERT_EQ(buffered.name(), "first byte buffered");
    // Observe the first call independently so an early sender cannot pass.
    require_drained_sequence(system, {}, ByteBudget, "first CIOUT must only buffer");
    ASSERT_THAT(wire.records(), testing::ElementsAreArray(expected));

    auto transmitted = test.resume(buffered, BufferedByteBudget);
    ASSERT_EQ(transmitted.name(), "first byte sent");
    require_drained_sequence(system, {data(0x41)}, ByteBudget, name);
    expected.push_back(data(0x41));
    EXPECT_THAT(wire.records(), testing::ElementsAreArray(expected));
    // UNLSN must flush the pending byte with EOI before sending UNLISTEN.
    const auto flushed = test.resume(transmitted, BufferedByteBudget);
    ASSERT_TRUE(flushed.complete());
    require_drained_sequence(system, {data(0x42, Eoi::Last), command(Unlisten)}, ByteBudget, name);
    expected.push_back(data(0x42, Eoi::Last));
    expected.push_back(command(Unlisten));
    EXPECT_THAT(wire.records(), testing::ElementsAreArray(expected));
}

// Executes ROM 4 TRANS1 with a prepared command-channel payload.
void run_rom4_trans1_case(System& system, const std::string& command, const char* name) {
    KernalFixture scenario(system, System::RomSet::Rom4);
    auto wire = system.trace_ieee();
    // Supply the command buffer normally constructed by the BASIC parser.
    if (command.empty() || command.size() > CommandPayloadCapacity)
        throw std::out_of_range("ROM command payload must contain 1..255 bytes");
    const std::vector<uint8_t> command_bytes(command.begin(), command.end());
    // Execute the prepared-state transport boundary, not a parser continuation.
    auto test = scenario.program();
    test.within(KernalBudget).run([&](auto& program) {
        scenario.init_ieee(program);
        scenario.device(program);
        scenario.file(program, command_bytes, CommandChannel);
        program.jsr(scenario.entry(KernalRoutine::Trans1));
    });

    // TRANS1 selects channel 15 with $6F, bypassing OPENI's $F0 conversion.
    std::vector<Record> expected{ieee_test::command(Listen(Device{DeviceAddress})),
                                 ieee_test::command(Secondary(Channel{CommandChannel}))};
    for (size_t index = 0; index < command.size(); ++index) {
        expected.push_back(
            data(static_cast<uint8_t>(command[index]), index + 1 == command.size() ? Eoi::Last : Eoi::More));
    }
    expected.push_back(ieee_test::command(Unlisten));
    require_transaction(system, wire, expected, name);
}

// Executes CLSE1 with an IEEE secondary address and verifies its close transaction.
TEST_P(CommodoreRomIeee, CLSE1) {
    auto& rom = selected_rom();
    const auto diagnostic = rom.name(KernalRoutine::Clse1);
    const auto name = diagnostic.c_str();
    auto wire = system.trace_ieee();
    // Select the file secondary before invoking the close sender.
    run_kernal_leaf(rom, KernalRoutine::Clse1, ViaIdle, 0x00, {{SecondaryAddress, FileChannel}});
    // Require the complete close framing and no trailing record.
    require_transaction(
        system, wire, {command(Listen(Device{DeviceAddress})), command(Close(Channel{FileChannel})), command(Unlisten)},
        name);
}

// Executes CLOS5 with one open IEEE file-table entry and verifies removal.
TEST_P(CommodoreRomIeee, CLOS5) {
    auto& rom = selected_rom();
    const auto diagnostic = rom.name(KernalRoutine::Clos5);
    const auto name = diagnostic.c_str();
    auto wire = system.trace_ieee();
    // Prepare one logical file and ask CLOS5 to remove it.
    run_kernal_leaf(rom, KernalRoutine::Clos5, ViaIdle, LogicalFile,
                    {{FileCount, OneOpenFile},
                     {LogicalFileTable, LogicalFile},
                     {DeviceFileTable, DeviceAddress},
                     {SecondaryFileTable, FileSecondary}});
    // Verify both the close transaction and file-table removal.
    require_transaction(
        system, wire, {command(Listen(Device{DeviceAddress})), command(Close(Channel{FileChannel})), command(Unlisten)},
        name);
    if (system.peek(econopet::SramAddress{FileCount.value()}) != 0x00) {
        throw std::runtime_error(std::string("PET KERNAL ") + name + " did not remove the closed file-table entry");
    }
}

// Executes CHKIN or CHKOUT through its public vector using one IEEE file-table entry.
void run_channel_select_case(System& system, KernalFixture& scenario, KernalRoutine routine, Command expected_primary) {
    const auto diagnostic = scenario.name(routine);
    const auto name = diagnostic.c_str();
    auto wire = system.trace_ieee();
    // Seed the file table after CINT and require a checked return from the vector.
    auto test = scenario.program();
    test.within(KernalBudget).run([&](auto& program) {
        scenario.init_ieee(program);
        program.lda(imm(OneOpenFile))
            .sta(zp(FileCount))
            .lda(imm(LogicalFile))
            .sta(LogicalFileTable)
            .lda(imm(DeviceAddress))
            .sta(DeviceFileTable)
            .lda(imm(FileSecondary))
            .sta(SecondaryFileTable)
            .ldx(imm(LogicalFile))
            .jsr(scenario.entry(routine));
    });
    require_transaction(system, wire, {command(expected_primary), command(Secondary(Channel{FileChannel}))}, name);
}

// Executes CLRCHN with device 8 selected as default output and checks UNLISTEN.
TEST_P(CommodoreRomIeee, CLRCHN_output) {
    auto& rom = selected_rom();
    const auto diagnostic = rom.name(KernalRoutine::Clrchn);
    const auto name = diagnostic.c_str();
    auto wire = system.trace_ieee();
    // Make the drive the default output, then clear the active channel.
    run_kernal_leaf(rom, KernalRoutine::Clrchn, ViaIdle, 0, {{DefaultOutputDevice, DeviceAddress}});
    // Expect exactly one ATN-tagged UNLISTEN.
    require_transaction(system, wire, {command(Unlisten)}, name);
}

// Executes CLALL with screen output and verifies that file bookkeeping is cleared without IEEE traffic.
TEST_P(CommodoreRomIeee, CLALL) {
    auto& rom = selected_rom();
    const auto diagnostic = rom.name(KernalRoutine::Clall);
    const auto name = diagnostic.c_str();
    auto wire = system.trace_ieee();
    // Use local keyboard/screen defaults so CLALL needs no IEEE termination.
    run_kernal_leaf(
        rom, KernalRoutine::Clall, ViaIdle, 0,
        {{FileCount, OneOpenFile}, {DefaultOutputDevice, ScreenDevice}, {DefaultInputDevice, KeyboardDevice}});
    // Check bookkeeping removal without any bus record.
    require_transaction(system, wire, {}, name);
    if (system.peek(econopet::SramAddress{FileCount.value()}) != 0x00) {
        throw std::runtime_error(std::string("PET KERNAL ") + name +
                                 " did not clear file bookkeeping without IEEE traffic");
    }
}

TEST_P(CommodoreRomIeee, SCATN) {
    run_line_case(system, selected_rom(), KernalRoutine::Scatn, pet::Nrfd, pet::Atn, pet::Atn);
}
TEST_P(CommodoreRomIeee, TKATN) {
    run_line_case(system, selected_rom(), KernalRoutine::Tkatn, pet::Nrfd, pet::Atn, ViaIdle);
}
TEST_P(CommodoreRomIeee, TALK) {
    run_command_case(system, selected_rom(), KernalRoutine::Talk, 0, Talk(Device{DeviceAddress}));
}
TEST_P(CommodoreRomIeee, LISTN) {
    run_command_case(system, selected_rom(), KernalRoutine::Listen, 0, Listen(Device{DeviceAddress}));
}
TEST_P(CommodoreRomIeee, LIST1) {
    run_command_case(system, selected_rom(), KernalRoutine::List1, IEEE_CMD_UNLISTEN, Unlisten);
}
TEST_P(CommodoreRomIeee, LISTN_SECND) {
    run_sequence_command_case(system, selected_rom(),
                              {{KernalRoutine::Listen, 0}, {KernalRoutine::Secondary, IEEE_CMD_SECONDARY(FileChannel)}},
                              {command(Listen(Device{DeviceAddress})), command(Secondary(Channel{FileChannel}))});
}
TEST_P(CommodoreRomIeee, TALK_UNTLK) {
    run_sequence_command_case(system, selected_rom(), {{KernalRoutine::Talk, 0}, {KernalRoutine::Untalk, 0}},
                              {command(Talk(Device{DeviceAddress})), command(Untalk)});
}
TEST_P(CommodoreRomIeee, TALK_TKSA) {
    run_sequence_command_case(system, selected_rom(),
                              {{KernalRoutine::Talk, 0}, {KernalRoutine::Tksa, IEEE_CMD_SECONDARY(FileChannel)}},
                              {command(Talk(Device{DeviceAddress})), command(Secondary(Channel{FileChannel}))});
}
TEST_P(CommodoreRomIeee, UNLSN) { run_command_case(system, selected_rom(), KernalRoutine::Unlisten, 0, Unlisten); }

// Verify ROM 2's addressed single-byte output independently of buffered output.
TEST_F(CommodoreRomRevision, Rom2_ISOUR_transmit) {
    KernalFixture rom(system, System::RomSet::Upgrade);
    run_isour_transmit_case(system, rom);
}

// Native file tables contain the encoded secondary ($60 OR channel).
// These prepared-vector cases do not execute the BASIC OPEN parser.
// Verify both revisions' input-channel TALK/secondary expectation.
TEST_P(CommodoreRomIeee, CHKIN) {
    run_channel_select_case(system, selected_rom(), KernalRoutine::Chkin, Talk(Device{DeviceAddress}));
}

// Verify both revisions' output-channel LISTEN/secondary expectation.
TEST_P(CommodoreRomIeee, CHKOUT) {
    run_channel_select_case(system, selected_rom(), KernalRoutine::Chkout, Listen(Device{DeviceAddress}));
}

INSTANTIATE_TEST_SUITE_P(Pet, CommodoreRomIeee, testing::Values(System::RomSet::Upgrade, System::RomSet::Rom4),
                         rom_revision_name);

// Verify ROM 2's first-byte buffering and last-byte EOI flush.
TEST_F(CommodoreRomRevision, Rom2_CIOUT_buffered_EOI) {
    KernalFixture rom(system, System::RomSet::Upgrade);
    run_ciout_case(system, rom);
}

// Verify the selected ROM 4 CIOUT entry with the same phase-by-phase observations.
TEST_F(CommodoreRomRevision, Rom4_CIOUT_buffered_EOI) {
    KernalFixture rom(system, System::RomSet::Rom4);
    run_ciout_case(system, rom);
}

// Verify ROM 4's addressed single-byte output independently of buffered output.
TEST_F(CommodoreRomRevision, Rom4_ISOUR_transmit) {
    KernalFixture rom(system, System::RomSet::Rom4);
    run_isour_transmit_case(system, rom);
}

// BOBREC feeds TRANS1 binary P data, including an embedded record-number zero.
// Unlike OPEN-15 integration, this independently requires secondary $6F.
// Text-only variants did not call their named BASIC parsers and duplicated
// this transport. OPENI framing is covered by native OPEN/read integration.
TEST_F(CommodoreRomRevision, Rom4_RECORD_command_transport) {
    constexpr uint16_t RecordNumber = 0x0034;
    constexpr uint8_t RecordPosition = 1;
    const std::string command{'P', FileSecondary, static_cast<char>(RecordNumber), static_cast<char>(RecordNumber >> 8),
                              RecordPosition};
    run_rom4_trans1_case(system, command, "RECORD");
}

// Retains both reset-vector fetch assertions from the retired board harness.
class KernalReset : public CommodoreRomIeee {};
// Requires the soft 6502 to fetch the selected ROM's actual reset target.
TEST_P(KernalReset, FetchesRomResetVector) {
    // Observe only fetches after the selected ROM's fixture setup.
    const econopet::CpuAddress entry{system.peek(econopet::SramAddress{pet::Reset6502.value()}) |
                                     (system.peek(econopet::SramAddress{(pet::Reset6502 + 1).value()}) << 8)};
    const uint64_t checkpoint = system.cpu().peek_fetch_checkpoint();
    // Start from the real reset vector and bound the first target fetch.
    system.cpu(CPU_SOFT_6502).start();
    system.run_until(
        [&](const System& observed) { return observed.cpu(CPU_SOFT_6502).peek_fetched_at(entry, checkpoint); },
        ResetFetchBudget, "KERNAL reset fetch");
}
INSTANTIATE_TEST_SUITE_P(Pet, KernalReset, testing::Values(System::RomSet::Upgrade, System::RomSet::Rom4),
                         rom_revision_name);

} // namespace

// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <array>
#include <type_traits>
#include <utility>
#include <vector>

#include "fixture.h"
#include "framework/system_test.h"
#include "ieee/fifo.h"
#include "ieee/trace.h"

namespace {
using namespace ieee_test;
using test_asm::imm;
constexpr econopet::Cycles FixtureBudget = 100000;

// Checks that the revision's exact callable boundary survives deliberate ROM patches.
class KernalFixtureTest : public SystemTest, public testing::WithParamInterface<test_rom::Set> {};

TEST_P(KernalFixtureTest, ResolvesEntriesAndRejectsUnsupportedRoutines) {
    KernalFixture rom(system, GetParam());
    const auto& entries = *test_rom::metadata(GetParam()).commodore;
    EXPECT_EQ(rom.entry(KernalRoutine::Cint), entries.Cint);
    EXPECT_EQ(rom.entry(KernalRoutine::Talk), entries.Talk);
    EXPECT_EQ(rom.entry(KernalRoutine::Openi), entries.Openi);
    EXPECT_NE(rom.name(KernalRoutine::Talk).find(test_rom::metadata(GetParam()).identity),
              std::string::npos);
    system.poke(econopet::SramAddress{(entries.Cint).value()}, 0);
    EXPECT_EQ(rom.entry(KernalRoutine::Cint), entries.Cint);
    EXPECT_THROW(rom.entry(static_cast<KernalRoutine>(999)), std::invalid_argument);
    EXPECT_EQ(rom.entry(KernalRoutine::Ciout), entries.Ciout);
    if (GetParam() == test_rom::Set::Upgrade) {
        EXPECT_THROW(rom.entry(KernalRoutine::Trans1), std::invalid_argument);
    } else {
        EXPECT_EQ(rom.entry(KernalRoutine::Trans1), *entries.Trans1);
    }
}

TEST_P(KernalFixtureTest, ProgramHasNoImplicitIeeeInitialization) {
    KernalFixture rom(system, GetParam());
    auto test = rom.program();
    const auto answer = test.byte("answer");
    const auto started = system.time();
    const auto result = test.within(FixtureBudget).run([&](auto& p) {
        p.lda(imm(0x42)).sta(answer);
    });
    EXPECT_EQ(result[answer], 0x42);
    EXPECT_EQ(result.started(), started);
    EXPECT_LE(result.completed() - started, FixtureBudget);
    EXPECT_EQ(system.snapshot().cpu_selection_o, std::to_underlying(CPU_SOFT_6502));
    EXPECT_EQ(system.io().pia1().writes().count(), 0);
    EXPECT_EQ(system.io().pia2().writes().count(), 0);
    EXPECT_EQ(system.io().via().writes().count(), 0);
}

// The revision facade forwards configured work without losing CPU completion.
TEST_P(KernalFixtureTest, ConfiguredRunSharesRelativeTimingVocabulary) {
    KernalFixture rom(system, GetParam());
    auto test = rom.program();
    const auto answer = test.byte("answer");
    unsigned calls = 0;
    const auto result = test.within(FixtureBudget)
        .after(0, [](System& board) { board.set_external_interrupts(false, true); }, RunAnchor::CpuRelease)
        .every(5, MissedService::Reject,
            [&](System&, econopet::Cycles) { ++calls; }, RunAnchor::CpuRelease)
        .until([](const System&) { return true; })
        .run([&](auto& p) { p.lda(imm(0x42)).sta(answer); });
    EXPECT_EQ(result[answer], 0x42);
    EXPECT_GT(calls, 0);
    EXPECT_FALSE(system.peek_stimulus().cpu_nmi_n_i);
}

TEST_P(KernalFixtureTest, ExplicitSetupEmitsRealFileAndDeviceInstructions) {
    KernalFixture rom(system, GetParam());
    auto test = rom.program();
    const uint8_t filename[] = {'T', 0, 'S', 'T'};
    auto zero = test.lower_bank("zero page");
    const auto parameters = zero.buffer("file parameters", KernalWorkspace::FilenameLength.value(), 11);
    const auto device = zero.byte("device", KernalWorkspace::Device.value());
    const auto secondary = zero.byte("secondary", KernalWorkspace::Secondary.value());
    const auto length = zero.byte("filename length", KernalWorkspace::FilenameLength.value());
    const auto pointer = zero.word("filename pointer", KernalWorkspace::Filename.value(),
        test_asm::ByteOrder::LittleEndian);
    const auto payload = test.lower_bank("payload").buffer("filename", 0, sizeof(filename));
    const auto status = zero.byte("status", KernalWorkspace::Status.value());
    test.capture(status);
    test.capture(parameters);
    test.capture(payload);
    const auto result = test.within(KernalBudget).run([&](auto& p) {
        rom.init_ieee(p);
        rom.device(p, 9);
        rom.file(p, filename, CommandChannel);
    });
    EXPECT_EQ(result[device], 9);
    EXPECT_EQ(result[secondary], CommandChannel);
    EXPECT_EQ(result[length], sizeof(filename));
    EXPECT_EQ(result[pointer], PayloadAddress.value());
    EXPECT_EQ(result[payload], (std::vector<uint8_t>{'T', 0, 'S', 'T'}));
    EXPECT_EQ(result[status], 0);
    EXPECT_EQ(system.ieee().peek_via_orb(), 0x06);
    EXPECT_EQ(system.ieee().peek_pia2_pb(), 0xff);
    EXPECT_GT(system.io().pia2().writes().count(), 0);
}

// Exercise real ROM initialization once, then preserve its I/O state across a private gate.
template<class Fixture>
void check_rom_phases(System& board, Fixture& rom) {
    auto test = rom.program(test_asm::ProgramMode::Phased);
    const auto answer = test.byte("answer");
    const auto other = test.byte("other");
    EXPECT_THROW(test.byte("overflow"), std::out_of_range);
    EXPECT_THROW(test.lower_bank("results").byte("private gate", 2), std::out_of_range);
    EXPECT_THROW(test.lower_bank("release"), std::invalid_argument);
    EXPECT_THROW(test.data("alias gate", ReleaseAddress, 1), std::invalid_argument);
    auto paused = test.within(KernalBudget).pause([&](auto& p) {
        rom.init_ieee(p);
        p.lda(imm(0x38)).sta(pet::ViaPortB).lda(imm(0x31)).sta(answer).pause("IEEE initialized");
        p.lda(imm(0x42)).sta(answer).lda(imm(0x43)).sta(other);
    });
    EXPECT_EQ(test.capture(paused)[answer], 0x31);
    const auto orb = board.ieee().peek_via_orb();
    bool reset_seen = false;
    auto reset_observation = board.observe([&](const System& observed) {
        reset_seen |= observed.cpu().peek_reset();
    });
    const auto writes = board.io().pia2().writes().count();
    board.tick(20);
    EXPECT_EQ(test.capture(paused)[answer], 0x31);
    auto configuration = board.execution();
    configuration.within(FixtureBudget).until([](const System&) { return true; });
    const auto complete = test.resume(paused, configuration);
    EXPECT_TRUE(complete.complete());
    EXPECT_EQ(test.capture(complete)[answer], 0x42);
    EXPECT_EQ(test.capture(complete)[other], 0x43);
    EXPECT_EQ(board.ieee().peek_via_orb(), orb);
    EXPECT_EQ(board.io().pia2().writes().count(), writes);
    EXPECT_FALSE(reset_seen);
    EXPECT_FALSE(board.cpu().peek_reset());
}

TEST_P(KernalFixtureTest, PhasesPreserveRealRomInitializedIeeeState) {
    KernalFixture rom(system, GetParam());
    check_rom_phases(system, rom);
}

TEST_F(SystemTest, WaterlooFixturePhasesPreserveRealRomInitializedIeeeState) {
    WaterlooFixture rom(system);
    check_rom_phases(system, rom);
}

// Replacing the revision rejects old phase capabilities without trying to release them.
template<class Fixture>
void check_replaced_phase(System& board, Fixture& rom, test_rom::Set replacement) {
    auto test = rom.program(test_asm::ProgramMode::Phased);
    auto paused = test.within(FixtureBudget).pause([](auto& p) { p.pause("first"); },
        [](System& board, econopet::Cycles) { board.tick(1); });
    board.cpu().assert_reset();
    board.load_rom_set(replacement, System::FixtureOverlap::Replace);
    const auto started = board.time();
    EXPECT_THROW(test.resume(paused, FixtureBudget), std::logic_error);
    EXPECT_THROW(test.capture(paused), std::logic_error);
    EXPECT_EQ(board.time(), started);
    EXPECT_EQ(board.peek(econopet::SramAddress{ReleaseAddress.value()}), 0);
}

TEST_P(KernalFixtureTest, PhasesRejectReplacedRevisionWithoutReleaseOrCapture) {
    KernalFixture rom(system, GetParam());
    check_replaced_phase(system, rom,
        GetParam() == test_rom::Set::Upgrade ? test_rom::Set::Rom4 : test_rom::Set::Upgrade);
}

TEST_F(SystemTest, WaterlooFixturePhasesRejectReplacedRevisionWithoutReleaseOrCapture) {
    WaterlooFixture rom(system);
    check_replaced_phase(system, rom, test_rom::Set::Rom4);
}

INSTANTIATE_TEST_SUITE_P(Pet, KernalFixtureTest,
    testing::Values(test_rom::Set::Upgrade, test_rom::Set::Rom4),
    [](const testing::TestParamInfo<test_rom::Set>& info) {
        return info.param == test_rom::Set::Upgrade ? "Rom2" : "Rom4";
    });

TEST_F(SystemTest, RomFixturesRejectMismatchedRevisionsWithoutMutation) {
    const auto started = system.time();
    EXPECT_THROW(KernalFixture(system, test_rom::Set::Waterloo), std::invalid_argument);
    EXPECT_FALSE(system.selected_rom());
    EXPECT_EQ(system.time(), started);
    KernalFixture rom(system, test_rom::Set::Upgrade);
    EXPECT_THROW(KernalFixture(system, test_rom::Set::Rom4), std::logic_error);
    EXPECT_THROW(WaterlooFixture{system}, std::logic_error);
    EXPECT_EQ(system.selected_rom()->set, test_rom::Set::Upgrade);
    EXPECT_EQ(system.time(), started);
}

TEST_F(SystemTest, RomProgramRejectsStaleRevisionBeforeAssembly) {
    KernalFixture rom(system, test_rom::Set::Upgrade);
    auto test = rom.program();
    system.load_rom_set(test_rom::Set::Rom4, System::FixtureOverlap::Replace);
    const auto started = system.time();
    bool assembled = false;
    EXPECT_THROW(test.within(FixtureBudget).run([&](auto&) { assembled = true; }), std::logic_error);
    EXPECT_FALSE(assembled);
    EXPECT_EQ(system.time(), started);
    EXPECT_THROW(rom.entry(KernalRoutine::Cint), std::logic_error);
}

TEST_F(SystemTest, RomProgramRechecksRevisionAfterAssembly) {
    KernalFixture rom(system, test_rom::Set::Upgrade);
    auto test = rom.program();
    const auto answer = test.byte("answer");
    const auto started = system.time();
    const auto original = system.peek(econopet::SramAddress{rom.code.start().value()});
    EXPECT_THROW(test.within(FixtureBudget).run([&](auto& p) {
        p.lda(imm(0x42)).sta(answer);
        system.load_rom_set(test_rom::Set::Rom4, System::FixtureOverlap::Replace);
    }), std::logic_error);
    EXPECT_EQ(system.time(), started);
    EXPECT_EQ(system.peek(econopet::SramAddress{rom.code.start().value()}), original);
}

TEST_F(SystemTest, RomProgramRechecksRevisionDuringFirmwareStyleService) {
    KernalFixture rom(system, test_rom::Set::Upgrade);
    auto test = rom.program();
    bool serviced = false;
    EXPECT_THROW(test.within(FixtureBudget).run([](auto& p) {
        const auto park = p.mark("park");
        p.jmp(park);
    }, [&](System& board, econopet::Cycles) {
        serviced = true;
        board.cpu().assert_reset();
        board.load_rom_set(test_rom::Set::Rom4, System::FixtureOverlap::Replace);
    }), std::logic_error);
    EXPECT_TRUE(serviced);
    EXPECT_TRUE(system.cpu().peek_reset());
    EXPECT_EQ(system.selected_rom()->set, test_rom::Set::Rom4);
}

TEST_F(SystemTest, RomProgramRemainsBoundAfterFixtureDestruction) {
    auto test = [&] {
        KernalFixture rom(system, test_rom::Set::Upgrade);
        return rom.program();
    }();
    const auto answer = test.byte("answer");
    const auto result = test.within(FixtureBudget).run([&](auto& p) {
        p.lda(imm(0x42)).sta(answer);
    });
    EXPECT_EQ(result[answer], 0x42);
}

TEST_F(SystemTest, WaterlooFixtureFocusedSetupHasOneExplicitInitializerCall) {
    WaterlooFixture rom(system);
    test_asm::mc6809::Program<System> p(rom, rom.code);
    const auto started = system.time();
    rom.init_ieee_lines(p);
    EXPECT_EQ(p.address(p.mark("after init")), rom.code.start() + 3);
    p.install();
    const uint8_t expected[] = {0xbd, 0xc1, 0x88};
    for (size_t i = 0; i < sizeof(expected); ++i)
        EXPECT_EQ(system.peek(econopet::SramAddress{(rom.code.start() + i).value()}), expected[i]);
    EXPECT_EQ(system.time(), started);
    EXPECT_EQ(rom.entry(WaterlooRoutine::Status), test_rom::WaterlooRevision12.Status);
    EXPECT_THROW(rom.entry(static_cast<WaterlooRoutine>(999)), std::invalid_argument);
}

TEST_F(SystemTest, WaterlooFixtureNativeSetupExplicitlyClearsHooksBeforeIeeeInit) {
    WaterlooFixture rom(system);
    test_asm::mc6809::Program<System> p(rom, rom.code);
    const auto started = system.time();
    rom.init_ieee(p);
    EXPECT_EQ(p.address(p.mark("after native init")), rom.code.start() + 6);
    p.install();
    const uint8_t expected[] = {0xbd, 0xe7, 0x7a, 0xbd, 0xc1, 0x88};
    for (size_t i = 0; i < sizeof(expected); ++i)
        EXPECT_EQ(system.peek(econopet::SramAddress{(rom.code.start() + i).value()}), expected[i]);
    EXPECT_EQ(system.time(), started);
}

TEST_F(SystemTest, WaterlooFixtureNativeHooksPermitRealUntalkTransport) {
    WaterlooFixture rom(system);
    system.ieee().write_control(fpga::IeeeControlBit::Enabled);
    auto wire = system.trace_ieee();
    const auto checkpoint = system.cpu(CPU_SOFT_6809).peek_fetch_checkpoint();
    auto test = rom.program();
    const auto answer = test.byte("returned from native UNTALK");
    const auto hooks = test.lower_bank("I/O hooks").buffer("all hooks", 0, 128);
    test.capture(hooks);
    const auto before = system.peek(econopet::SramAddress{0x057f});
    const auto after = system.peek(econopet::SramAddress{0x0600});
    EXPECT_EQ(system.peek(econopet::SramAddress{0x0580}), 0xea);
    EXPECT_EQ(system.peek(econopet::SramAddress{0x05ff}), 0xea);
    const auto result = test.within(WaterlooBudget).run([&](auto& p) {
        rom.init_ieee(p);
        rom.device(p);
        p.jsr(rom.entry(WaterlooRoutine::Untalk)).lda(imm(0x42)).sta(answer);
    });
    EXPECT_EQ(result[answer], 0x42);
    EXPECT_EQ(result[hooks], std::vector<uint8_t>(128, 0));
    EXPECT_EQ(system.peek(econopet::SramAddress{0x057f}), before);
    EXPECT_EQ(system.peek(econopet::SramAddress{0x0600}), after);
    EXPECT_TRUE(system.cpu(CPU_SOFT_6809).peek_opcode_fetched_at(econopet::CpuAddress{0xe77a}, checkpoint));
    EXPECT_TRUE(system.cpu(CPU_SOFT_6809).peek_opcode_fetched_at(econopet::CpuAddress{0xe76b}, checkpoint));
    EXPECT_FALSE(system.cpu(CPU_SOFT_6809).peek_opcode_fetched_at(econopet::CpuAddress{0xeaea}, checkpoint));
    require_sequence(wire.records(), {{0x5f, true, Eoi::More}}, "native Waterloo UNTALK");
    require_drained_sequence(system, {{0x5f, true, Eoi::More}}, ByteBudget, "native Waterloo UNTALK");
}

TEST_F(SystemTest, WaterlooFixtureFocusedInitializerPreservesHooks) {
    WaterlooFixture rom(system);
    auto test = rom.program();
    const auto hooks = test.lower_bank("I/O hooks").buffer("all hooks", 0, 128);
    test.capture(hooks);
    const auto result = test.within(FixtureBudget).run([&](auto& p) { rom.init_ieee_lines(p); });
    EXPECT_EQ(result[hooks], std::vector<uint8_t>(128, 0xea));
}

TEST_F(SystemTest, WaterlooFixtureExplicitInitializerEstablishesRomRegisterState) {
    WaterlooFixture rom(system);
    auto test = rom.program();
    EXPECT_TRUE(test.within(WaterlooBudget).run([&](auto& p) { rom.init_ieee(p); }).empty());
    EXPECT_EQ(system.ieee().peek_via_orb(), 0x06);
    EXPECT_EQ(system.ieee().peek_pia2_pb(), 0xff);
    EXPECT_TRUE(system.ieee().peek_nrfd_released());
    EXPECT_TRUE(system.ieee().peek_ndac_released());
    EXPECT_EQ(system.peek(econopet::SramAddress{0x77}), 0);
}

// Exercise the explicit scenario path independently of the convenience builder.
template<class Fixture>
void check_explicit_rom_run(System& board, Fixture& rom) {
    test_asm::Program<typename Fixture::Cpu, System> program(rom, rom.code);
    const auto marker = rom.completion(rom.completion_region);
    program.lda(imm(0x52)).sta(rom.results.start()).finish(marker);
    auto configuration = board.execution();
    configuration.within(FixtureBudget);
    const auto started = board.time();
    const auto result = rom.run(program, rom.results, configuration, "explicit revision-bound run");
    EXPECT_EQ(result.raw_bytes(), (std::vector<uint8_t>{0x52, 0}));
    EXPECT_EQ(result.started(), started);
    EXPECT_GT(result.completed(), started);
    EXPECT_LE(result.completed() - started, FixtureBudget);
}

TEST_P(KernalFixtureTest, ExplicitScenarioUsesCheckedRevisionExecution) {
    KernalFixture rom(system, GetParam());
    check_explicit_rom_run(system, rom);
}

TEST_F(SystemTest, WaterlooFixtureExplicitScenarioUsesCheckedRevisionExecution) {
    WaterlooFixture rom(system);
    check_explicit_rom_run(system, rom);
}

TEST_F(SystemTest, RomFixtureExplicitBudgetServiceRejectsRevisionReplacement) {
    KernalFixture rom(system, test_rom::Set::Upgrade);
    test_asm::mos6502::Program<System> p(rom, rom.code);
    const auto park = p.mark("park");
    p.jmp(park).finish(rom.completion(rom.completion_region));
    bool serviced = false;
    EXPECT_THROW(rom.run(p, rom.results, FixtureBudget, "explicit stale revision",
        [&](System& board, econopet::Cycles) {
            serviced = true;
            board.cpu().assert_reset();
            board.load_rom_set(test_rom::Set::Rom4, System::FixtureOverlap::Replace);
            board.tick(1);
        }), std::logic_error);
    EXPECT_TRUE(serviced);
    EXPECT_EQ(system.selected_rom()->set, test_rom::Set::Rom4);
}

TEST_F(SystemTest, WaterlooFixtureProgramOwnsCpuStackAndExplicitFileSetup) {
    WaterlooFixture rom(system);
    auto test = rom.program();
    const auto answer = test.byte("answer");
    const uint8_t filename[] = {'T', 'E', 'S', 'T'};
    const auto direct = test.lower_bank("direct page").buffer("pointers", 0, 0x100);
    const auto block = test.lower_bank("control block and filename").buffer("control block", 0, 0x80);
    test.capture(direct);
    test.capture(block);
    const auto result = test.within(FixtureBudget).run([&](auto& p) {
        rom.device(p, 9, FileChannel);
        rom.file(p, filename);
        p.lda(imm(0x42)).sta(answer);
    });
    EXPECT_EQ(result[answer], 0x42);
    EXPECT_EQ(system.snapshot().cpu_selection_o, std::to_underlying(CPU_SOFT_6809));
    EXPECT_EQ(result[direct.word(WaterlooWorkspace::ActiveBlock.value(), test_asm::ByteOrder::BigEndian)],
        WaterlooWorkspace::Block.value());
    EXPECT_EQ(result[block.byte(WaterlooWorkspace::Primary - WaterlooWorkspace::Block)], 9);
    EXPECT_EQ(result[block.byte(WaterlooWorkspace::Secondary - WaterlooWorkspace::Block)], FileChannel);
    EXPECT_EQ(result[block.byte(WaterlooWorkspace::DeviceCode - WaterlooWorkspace::Block)], 0x81);
    EXPECT_EQ(result[block.word(WaterlooWorkspace::RecordSizeHigh - WaterlooWorkspace::Block,
        test_asm::ByteOrder::BigEndian)], 1);
    EXPECT_EQ(result[block.word(WaterlooWorkspace::WaitParameterHigh - WaterlooWorkspace::Block,
        test_asm::ByteOrder::BigEndian)], 0);
    EXPECT_EQ(result[block.byte(WaterlooWorkspace::TransferState - WaterlooWorkspace::Block)], 0);
    EXPECT_EQ(result[block.byte(WaterlooWorkspace::DiskFlags - WaterlooWorkspace::Block)], 0x40);
    EXPECT_EQ(result[direct.word(WaterlooWorkspace::FilenameCursor.value(), test_asm::ByteOrder::BigEndian)],
        WaterlooWorkspace::Filename.value());
    EXPECT_EQ(result[block.slice(WaterlooWorkspace::Filename - WaterlooWorkspace::Block, sizeof(filename))],
        (std::vector<uint8_t>{'T', 'E', 'S', 'T'}));
    EXPECT_EQ(system.io().pia2().writes().count(), 0);
}

TEST_F(SystemTest, RomFixtureFileValidationDoesNotEmitPartialSetup) {
    KernalFixture kernal(system, test_rom::Set::Upgrade);
    test_asm::mos6502::Program<System> p(kernal, kernal.code);
    const std::array<uint8_t, 0> empty{};
    const std::vector<uint8_t> oversized(256);
    EXPECT_THROW(kernal.file(p, empty), std::out_of_range);
    EXPECT_THROW(kernal.file(p, oversized), std::out_of_range);
    EXPECT_EQ(p.address(p.mark("after validation")), kernal.code.start());
    system.load_rom_set(test_rom::Set::Waterloo, System::FixtureOverlap::Replace);
    WaterlooFixture waterloo(system);
    test_asm::mc6809::Program<System> q(waterloo, waterloo.code);
    EXPECT_THROW(waterloo.file(q, empty), std::out_of_range);
    EXPECT_THROW(waterloo.file(q, oversized), std::out_of_range);
    EXPECT_EQ(q.address(q.mark("after validation")), waterloo.code.start());
}

TEST_F(SystemTest, RomFixtureLayoutsRejectWorkspaceAliasesWithoutMutation) {
    KernalFixture rom(system, test_rom::Set::Upgrade);
    auto test = rom.program();
    const auto started = system.time();
    EXPECT_THROW(test.data("stack alias", econopet::CpuAddress{0x0100}, 1), std::invalid_argument);
    EXPECT_THROW(test.data("timeout alias", KernalWorkspace::IeeeTimeoutControl, 1), std::invalid_argument);
    EXPECT_THROW(test.data("file table alias", KernalWorkspace::LogicalFiles, 1), std::invalid_argument);
    EXPECT_EQ(test.buffer("complete capacity", 2).size(), 2u);
    EXPECT_THROW(test.byte("overflow"), std::out_of_range);
    EXPECT_THROW(test.within(FixtureBudget).pause([](auto&) {}), std::logic_error);
    EXPECT_EQ(system.time(), started);
    EXPECT_NO_THROW(test.run([](auto&) {}));
    system.cpu().assert_reset();
    system.load_rom_set(test_rom::Set::Waterloo, System::FixtureOverlap::Replace);
    WaterlooFixture waterloo(system);
    auto native = waterloo.program();
    const auto replaced = system.time();
    EXPECT_THROW(native.data("parameter block alias", WaterlooWorkspace::Block, 1), std::invalid_argument);
    EXPECT_THROW(native.data("filename alias", WaterlooWorkspace::Filename, 1), std::invalid_argument);
    EXPECT_EQ(native.buffer("complete capacity", 2).size(), 2u);
    EXPECT_THROW(native.byte("overflow"), std::out_of_range);
    EXPECT_EQ(system.time(), replaced);
}

TEST_F(SystemTest, RomFixtureFileValidationRejectsWaterlooBoundaryBeforeEmission) {
    WaterlooFixture rom(system);
    test_asm::mc6809::Program<System> p(rom, rom.code);
    const std::vector<uint8_t> oversized(WaterlooWorkspace::FilenameCapacity + 1);
    const auto started = system.time();
    EXPECT_THROW(rom.file(p, oversized), std::out_of_range);
    EXPECT_EQ(p.address(p.mark("after invalid filename")), rom.code.start());
    const std::vector<uint8_t> maximum(WaterlooWorkspace::FilenameCapacity, 'X');
    EXPECT_NO_THROW(rom.file(p, maximum));
    EXPECT_EQ(system.time(), started);
}

// Run maximum-sized literal emission, native setup and both phase handshakes together.
template<class Fixture>
void check_maximum_filename(System& board, Fixture& rom, size_t size, test_asm::ProgramMode mode) {
    constexpr bool kernal = std::is_same_v<typename Fixture::Cpu, test_asm::Mos6502>;
    auto test = rom.program(mode);
    const auto answer = test.byte("answer");
    auto data = test.lower_bank(kernal ? "payload" : "control block and filename");
    const auto filename = data.buffer("maximum filename",
        kernal ? 0 : WaterlooWorkspace::Filename - WaterlooWorkspace::Block, size);
    const auto pointer = [&] {
        if constexpr (kernal)
            return test.lower_bank("zero page").word("filename pointer", KernalWorkspace::Filename.value(),
                test_asm::ByteOrder::LittleEndian);
        else
            return test.lower_bank("direct page").word("filename cursor", WaterlooWorkspace::FilenameCursor.value(),
                test_asm::ByteOrder::BigEndian);
    }();
    test.capture(filename);
    test.capture(pointer);
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<uint8_t>(i ^ 0xa5);
    const std::vector<uint8_t> poison(size, 0xcc);
    data.initialize(filename, poison);
    const auto assemble = [&](auto& p) {
        rom.init_ieee(p);
        rom.device(p);
        rom.file(p, bytes);
        EXPECT_EQ(data.read(filename), poison);
        p.lda(imm(0x42)).sta(answer);
        if (mode == test_asm::ProgramMode::Phased) p.pause("maximum filename prepared");
        p.lda(imm(0x43)).sta(answer);
    };
    const auto result = [&] {
        if (mode == test_asm::ProgramMode::Complete)
            return test.within(KernalBudget).run(assemble);
        auto first = test.within(KernalBudget).pause(assemble);
        EXPECT_EQ(test.capture(first)[answer], 0x42);
        EXPECT_EQ(test.capture(first)[filename], bytes);
        auto last = test.resume(first, FixtureBudget);
        EXPECT_TRUE(last.complete());
        return test.capture(last);
    }();
    EXPECT_EQ(result[filename], bytes);
    EXPECT_EQ(result[pointer], filename.address().value());
    EXPECT_EQ(result[answer], 0x43);
    if constexpr (kernal)
        EXPECT_EQ(board.peek(econopet::SramAddress{0x00d1}), 255);
}

TEST_P(KernalFixtureTest, MaximumFilenameExecutesInCompleteAndPhasedPrograms) {
    KernalFixture rom(system, GetParam());
    check_maximum_filename(system, rom, 255, test_asm::ProgramMode::Complete);
    check_maximum_filename(system, rom, 255, test_asm::ProgramMode::Phased);
}

TEST_F(SystemTest, WaterlooFixtureMaximumFilenameExecutesInCompleteAndPhasedPrograms) {
    WaterlooFixture rom(system);
    check_maximum_filename(system, rom, 64, test_asm::ProgramMode::Complete);
    check_maximum_filename(system, rom, 64, test_asm::ProgramMode::Phased);
}

// Verify the explicit scenario path has the same usable maximum, without builder startup.
template<class Fixture>
void check_explicit_maximum_filename(System& board, Fixture& rom, size_t size) {
    constexpr bool kernal = std::is_same_v<typename Fixture::Cpu, test_asm::Mos6502>;
    test_asm::Program<typename Fixture::Cpu, System> p(rom, rom.code);
    constexpr uint8_t Native6809InterruptMasks = 0x50;
    if constexpr (!kernal) p.orcc(imm(Native6809InterruptMasks)).lds(test_asm::imm16(rom.stack_top()));
    const std::vector<uint8_t> bytes(size, 'M');
    rom.init_ieee(p);
    rom.device(p);
    rom.file(p, bytes);
    p.lda(imm(0x42)).sta(rom.results.start()).finish(rom.completion(rom.completion_region));
    const auto result = rom.run(p, rom.results, KernalBudget, "explicit maximum filename");
    EXPECT_EQ(result.raw_bytes(), (std::vector<uint8_t>{0x42, 0}));
    const auto data = rom.lower_bank(rom.region(kernal ? "payload" : "control block and filename"));
    const auto filename = data.buffer("maximum filename",
        kernal ? 0 : WaterlooWorkspace::Filename - WaterlooWorkspace::Block, size);
    EXPECT_EQ(data.read(filename), bytes);
    EXPECT_EQ(board.peek(econopet::SramAddress{rom.code.start().value()}), kernal ? 0x78 : 0x1a);
}

TEST_P(KernalFixtureTest, ExplicitScenarioExecutesMaximumFilename) {
    KernalFixture rom(system, GetParam());
    check_explicit_maximum_filename(system, rom, 255);
}

TEST_F(SystemTest, WaterlooFixtureExplicitScenarioExecutesMaximumFilename) {
    WaterlooFixture rom(system);
    check_explicit_maximum_filename(system, rom, 64);
}

TEST_F(SystemTest, WaterlooFixtureReservesBothHookTablesInBothExecutionPaths) {
    WaterlooFixture rom(system);
    const auto hooks = rom.region("I/O hooks");
    EXPECT_EQ(hooks.start(), econopet::CpuAddress{0x0580});
    EXPECT_EQ(hooks.size(), 128u);
    const auto started = system.time();
    for (const auto mode : {test_asm::ProgramMode::Complete, test_asm::ProgramMode::Phased}) {
        auto test = rom.program(mode);
        for (const auto address : {0x0580, 0x05bf, 0x05c0, 0x05ff}) {
            EXPECT_THROW(rom.reserve("hook alias", test_asm::RegionKind::Data,
                econopet::CpuAddress{address}, 1), std::invalid_argument);
            EXPECT_THROW(test.data("hook alias", econopet::CpuAddress{address}, 1), std::invalid_argument);
        }
        EXPECT_EQ(test.lower_bank("I/O hooks").buffer("all hooks", 0, 128).address(), hooks.start());
        EXPECT_NO_THROW(test.data("after hooks", econopet::CpuAddress{0x0600}, 1));
    }
    EXPECT_EQ(system.time(), started);
}

TEST_F(SystemTest, RomFixtureCustomCodeUsesSameLayoutInExplicitAndBuilderPaths) {
    const test_asm::MemoryRegion custom{econopet::CpuAddress{0x2000}, 0x800};
    KernalFixture rom(system, test_rom::Set::Upgrade, custom);
    EXPECT_EQ(rom.code.start(), econopet::CpuAddress{0x2000});
    EXPECT_EQ(rom.code.size(), 0x800u);
    rom.require(rom.payload, test_asm::RegionKind::Data);
    auto test = rom.program(test_asm::ProgramMode::Phased);
    EXPECT_THROW(test.data("code alias", econopet::CpuAddress{0x27ff}, 1), std::invalid_argument);
    EXPECT_THROW(test.data("payload alias", econopet::CpuAddress{0x0400}, 1), std::invalid_argument);
    const auto first = test.within(FixtureBudget).pause([](auto& p) { p.pause("ready"); });
    EXPECT_EQ(system.peek(econopet::SramAddress{0xfffc}), 0);
    EXPECT_EQ(system.peek(econopet::SramAddress{0xfffd}), 0x20);
    EXPECT_EQ(first.name(), "ready");
    EXPECT_THROW(rom.program(static_cast<test_asm::ProgramMode>(999)), std::invalid_argument);
}

TEST_F(SystemTest, WaterlooFixtureCustomCodeUsesSameLayoutInExplicitAndBuilderPaths) {
    const test_asm::MemoryRegion custom{econopet::CpuAddress{0x2000}, 0x800};
    WaterlooFixture rom(system, custom);
    EXPECT_EQ(rom.code.start(), econopet::CpuAddress{0x2000});
    EXPECT_EQ(rom.code.size(), 0x800u);
    EXPECT_EQ(rom.stack_top(), econopet::CpuAddress{0x0200});
    auto test = rom.program();
    EXPECT_THROW(test.data("code alias", econopet::CpuAddress{0x27ff}, 1), std::invalid_argument);
    EXPECT_THROW(test.data("vectors alias", econopet::CpuAddress{0xfff0}, 1), std::invalid_argument);
    EXPECT_TRUE(test.within(FixtureBudget).run([](auto&) {}).empty());
    EXPECT_EQ(system.peek(econopet::SramAddress{0xfffe}), 0x20);
    EXPECT_EQ(system.peek(econopet::SramAddress{0xffff}), 0);
}

TEST_F(SystemTest, RomFixtureCustomCodeCannotOverlapReservedWorkspace) {
    const auto started = system.time();
    EXPECT_THROW((KernalFixture(system, test_rom::Set::Upgrade,
        {econopet::CpuAddress{0x0400}, 0x100})), std::invalid_argument);
    EXPECT_THROW((WaterlooFixture(system,
        {econopet::CpuAddress{0x0580}, 0x100})), std::invalid_argument);
    EXPECT_FALSE(system.selected_rom());
    EXPECT_EQ(system.time(), started);
}

TEST_F(SystemTest, RomFixtureCpuBindingCannotBeForgedByDirectBuilderConstruction) {
    KernalFixture rom(system, test_rom::Set::Upgrade);
    const auto started = system.time();
    EXPECT_THROW((RomProgramBuilder<test_asm::Mc6809>(system, test_rom::Set::Upgrade, {},
        test_asm::ProgramMode::Complete)), std::invalid_argument);
    EXPECT_EQ(system.time(), started);
}
} // namespace

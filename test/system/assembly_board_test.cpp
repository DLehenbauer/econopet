// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <array>
#include <optional>
#include <stdexcept>
#include <string>

#include "framework/asm.h"
#include "framework/system_test.h"

using namespace test_asm;
using econopet::CpuAddress;
using econopet::SramAddress;

TEST_F(SystemTest, CpuAssemblyInstallationWritesOnlyOwnedLowerBankWithoutClocks) {
    const auto start = system.time();
    Scenario layout(system);
    const auto code = layout.reserve("code", RegionKind::Code, CpuAddress{0x300}, 8);
    const auto upper = SramAddress{econopet::CpuAddressCapacity + code.start().value()};
    system.poke(upper, 0x99);
    auto bus = system.trace_peripheral_writes();
    mos6502::Program program(layout, code);
    const auto target = program.label("target");
    program.lda(imm(0x42)).jmp(target);
    program.bind(target);
    program.rts();
    EXPECT_EQ(system.peek(SramAddress{0x300}), System::IdleRamByte);
    program.install();
    const std::array<uint8_t, 6> expected{0xa9, 0x42, 0x4c, 0x05, 0x03, 0x60};
    for (size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(system.peek(SramAddress{0x300 + i}), expected[i]);
    EXPECT_EQ(system.peek(SramAddress{0x306}), System::IdleRamByte);
    EXPECT_EQ(system.peek(upper), 0x99);
    EXPECT_EQ(system.time(), start);
    bus.capture().require_absent("assembly installation is not a physical CPU write");
}

TEST_F(SystemTest, CpuAssemblyFailuresRetainBoardEvidenceAndBacking) {
    Scenario layout(system);
    mos6502::Program program(layout, layout.reserve("small", RegionKind::Code, CpuAddress{0x300}, 2));
    const auto start = system.time();
    expect_failure<std::out_of_range>(system, "emit", [&] { program.jsr(CpuAddress{0x400}); },
        "program_cpu=soft6502");
    EXPECT_EQ(program.size(), 0u);
    mc6809::Program unresolved(layout, layout.reserve("unresolved", RegionKind::Code, CpuAddress{0x400}, 8));
    unresolved.jsr(unresolved.label("target"));
    expect_failure<std::logic_error>(system, "resolve", [&] { unresolved.install(); },
        "program_cpu=soft6809");
    EXPECT_EQ(system.peek(SramAddress{0x300}), System::IdleRamByte);
    EXPECT_EQ(system.peek(SramAddress{0x400}), System::IdleRamByte);
    EXPECT_EQ(system.time(), start);
}

TEST_F(SystemTest, CpuAssemblyInstallationObeysObservationMutationGuard) {
    Scenario layout(system);
    mc6809::Program program(layout, layout.reserve("code", RegionKind::Code, CpuAddress{0x300}, 8));
    program.lda(imm(0x42)).rts();
    system.run_until([&](const System&) {
        EXPECT_THROW(program.install(), std::logic_error);
        EXPECT_EQ(system.peek(SramAddress{0x300}), System::IdleRamByte);
        return true;
    }, 0, "observation-only installation");
    EXPECT_NO_THROW(program.install());
    EXPECT_EQ(system.peek(SramAddress{0x300}), 0x86);
}

// A checked pause protects label-derived replacement despite setup growth and host dwell.
TEST_F(SystemTest, CpuAssemblyContinuationGrowthKeepsParkedReplacementInsideItsRegion) {
    constexpr CpuAddress Entry{0x0300};
    constexpr size_t CodeCapacity = 0x100;
    constexpr econopet::Cycles ExecutionBudget{20000};
    constexpr std::array<econopet::Cycles, 4> HostDwells{0, 1, 2000, 2058};
    constexpr unsigned SetupPaddingInstructions = 40;
    for (const auto dwell : HostDwells) {
        SCOPED_TRACE("host dwell=" + std::to_string(dwell.value()));
        LayoutOverrides overrides;
        overrides.code = MemoryRegion{Entry, CodeCapacity};
        auto test = system.program6502(ProgramMode::Phased, Layout::Pet, overrides);
        const auto answer = test.byte("answer");
        std::optional<CpuAddress> continuation, replacement_start, replacement_end;
        const auto checkpoint = system.cpu().peek_fetch_checkpoint();
        auto phase = test.within(ExecutionBudget).pause([&](auto& setup) {
            const auto target = setup.label("continuation");
            setup.sei().jmp(target);
            for (unsigned index = 0; index < SetupPaddingInstructions; ++index) setup.sei();
            setup.bind(target);
            continuation = setup.address(target);
            setup.lda(imm(0x42)).sta(answer).pause("replace");
            replacement_start = setup.address(setup.mark("replacement"));
            setup.lda(imm(0x42)).sta(answer);
            replacement_end = setup.address(setup.mark("replacement end"));
        });
        ASSERT_FALSE(phase.complete());
        ASSERT_TRUE(continuation);
        ASSERT_TRUE(replacement_start);
        ASSERT_TRUE(replacement_end);
        ASSERT_GT(continuation->value(), (Entry + SetupPaddingInstructions).value());
        ASSERT_GT(replacement_end->value(), replacement_start->value());
        ASSERT_LE(replacement_end->value(), (Entry + CodeCapacity).value());
        EXPECT_TRUE(system.cpu(CPU_SOFT_6502).peek_opcode_fetched_at(*continuation, checkpoint));

        // The immutable closed gate keeps the future code unreachable throughout host dwell.
        system.tick(dwell);
        EXPECT_EQ(test.capture(phase)[answer], 0x42);
        EXPECT_FALSE(system.cpu(CPU_SOFT_6502).peek_opcode_fetched_at(*replacement_start, checkpoint));
        Scenario replacement_layout(system);
        const auto code = replacement_layout.reserve("replacement", RegionKind::Code, *replacement_start,
            static_cast<size_t>(*replacement_end - *replacement_start));
        mos6502::Program replacement(replacement_layout, code);
        replacement.lda(imm(0x5a)).sta(answer.address());
        ASSERT_EQ(replacement.size(), code.size());
        const auto before = system.time();
        replacement.install();
        EXPECT_EQ(system.time(), before);

        // Resume through the checked gate without reset, preserving the untouched terminal handshake.
        auto complete = test.resume(phase, ExecutionBudget);
        EXPECT_TRUE(complete.complete());
        EXPECT_EQ(test.capture(complete)[answer], 0x5a);
        EXPECT_FALSE(system.cpu().peek_reset());
        EXPECT_TRUE(system.cpu(CPU_SOFT_6502).peek_opcode_fetched_at(*replacement_start, checkpoint));
    }
}

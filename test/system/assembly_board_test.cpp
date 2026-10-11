// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <array>
#include <stdexcept>

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

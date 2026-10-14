// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <array>
#include <utility>

#include <gtest/gtest.h>

#include "framework/asm.h"
#include "host_memory.h"

using namespace test_asm;

// Mailboxes must be layout-owned, dedicated single bytes with distinct values.
TEST(ProgramCompletion, MarkerRejectsWrongPurposeExtentAndForeignOwnership) {
    HostMemory memory;
    Scenario layout(memory), other(memory);
    const auto mailbox = layout.reserve("mailbox", RegionKind::Mailbox, econopet::CpuAddress{0x200}, 2);
    const auto data = layout.reserve("data", RegionKind::Data, econopet::CpuAddress{0x400}, 1);
    EXPECT_THROW(layout.completion(mailbox), std::invalid_argument);
    EXPECT_THROW(layout.completion(data), std::invalid_argument);
    EXPECT_THROW(other.completion(mailbox.slice(0, 1)), std::invalid_argument);
    EXPECT_THROW(layout.completion(mailbox.slice(0, 1), 1, 1), std::invalid_argument);
    EXPECT_EQ(memory.writes, 0u);
}

// Terminal code must fit completely and permanently closes instruction emission.
TEST(ProgramCompletion, BothCpusEmitExactTerminalBytesAndRejectFurtherEmission) {
    HostMemory memory;
    Scenario layout(memory);
    const auto mailbox = layout.reserve("mailbox", RegionKind::Mailbox, econopet::CpuAddress{0x2ff}, 1);
    const auto marker = layout.completion(mailbox, 0x19, 0x42);
    const auto mos_code = layout.reserve("6502", RegionKind::Code, econopet::CpuAddress{0x300}, 8);
    const auto mc_code = layout.reserve("6809", RegionKind::Code, econopet::CpuAddress{0xfff9}, 7);
    mos6502::Program mos(layout, mos_code);
    mc6809::Program mc(layout, mc_code);
    mos.finish(marker);
    mc.finish(marker);
    EXPECT_EQ(memory.writes, 0u);
    EXPECT_THROW(mos.sei(), std::logic_error);
    EXPECT_THROW(mc.rts(), std::logic_error);
    EXPECT_THROW(mos.finish(marker), std::logic_error);
    EXPECT_THROW(mc.finish(marker), std::logic_error);
    mos.install();
    mc.install();
    const std::array<uint8_t, 8> mos_expected{0xa9, 0x42, 0x8d, 0xff, 0x02, 0x4c, 0x05, 0x03};
    const std::array<uint8_t, 7> mc_expected{0x86, 0x42, 0xb7, 0x02, 0xff, 0x20, 0xfe};
    for (size_t i = 0; i < mos_expected.size(); ++i) EXPECT_EQ(memory.bytes[0x300 + i], mos_expected[i]);
    for (size_t i = 0; i < mc_expected.size(); ++i) EXPECT_EQ(memory.bytes[0xfff9 + i], mc_expected[i]);
    auto moved = std::move(mc);
    EXPECT_THROW(mc.install(), std::logic_error);
    EXPECT_THROW(moved.rts(), std::logic_error);
    EXPECT_NO_THROW(moved.install());
}

// Capacity and marker identity rejection leave every buffered byte unchanged.
TEST(ProgramCompletion, TerminalPreflightIsAtomicForBothCpus) {
    HostMemory memory;
    Scenario layout(memory), other(memory);
    const auto marker = layout.completion(
        layout.reserve("mailbox", RegionKind::Mailbox, econopet::CpuAddress{0x2ff}, 1));
    const auto foreign = other.completion(
        other.reserve("mailbox", RegionKind::Mailbox, econopet::CpuAddress{0x2ff}, 1));
    mos6502::Program mos(layout, layout.reserve("6502", RegionKind::Code, econopet::CpuAddress{0x300}, 7));
    mc6809::Program mc(layout, layout.reserve("6809", RegionKind::Code, econopet::CpuAddress{0x400}, 6));
    EXPECT_THROW(mos.finish(foreign), std::invalid_argument);
    EXPECT_THROW(mc.finish(foreign), std::invalid_argument);
    EXPECT_THROW(mos.finish(marker), std::out_of_range);
    EXPECT_THROW(mc.finish(marker), std::out_of_range);
    EXPECT_EQ(mos.size(), 0u);
    EXPECT_EQ(mc.size(), 0u);
    EXPECT_EQ(memory.writes, 0u);
    EXPECT_NO_THROW(mos.sei());
    EXPECT_NO_THROW(mc.rts());
}

// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <array>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include "framework/phase.h"
#include "host_memory.h"

using namespace test_asm;

static_assert(!std::is_copy_constructible_v<PhaseHandle>);
static_assert(std::is_move_constructible_v<PhaseHandle>);
static_assert(!std::is_default_constructible_v<PhaseHandle>);
static_assert(!std::is_reference_v<decltype(std::declval<PhaseHandle&&>().window())>);

// Native-endian operands and -7 branches target the private gate load, preserving registers.
TEST(PhaseAssembly, EncodesRegisterPreservingHandshakesForBothCpus) {
    const econopet::CpuAddress acknowledgment{0x1234}, release{0xabcd};
    const auto mos = phase_detail::handshake<Mos6502>(acknowledgment, release, 0x42);
    const auto mc = phase_detail::handshake<Mc6809>(acknowledgment, release, 0xff);
    EXPECT_EQ(mos.bytes, (std::array<uint8_t, 16>{
        0x08, 0x48, 0xa9, 0x42, 0x8d, 0x34, 0x12, 0xad, 0xcd, 0xab, 0xc9, 0x42, 0xd0, 0xf9, 0x68, 0x28}));
    EXPECT_EQ(mc.bytes, (std::array<uint8_t, 16>{
        0x34, 0x03, 0x86, 0xff, 0xb7, 0x12, 0x34, 0xb6, 0xab, 0xcd, 0x81, 0xff, 0x26, 0xf9, 0x35, 0x03}));
}

// One whole handshake is a bounded instruction (no partial emission on insufficient capacity).
TEST(PhaseAssembly, HandshakeCapacityPreflightIsAtomicAndAllowsFinalCpuByte) {
    HostMemory memory;
    Scenario layout(memory);
    const auto small = layout.reserve("small", RegionKind::Code, econopet::CpuAddress{0x300}, 15);
    const auto exact = layout.reserve("exact", RegionKind::Code, econopet::CpuAddress{0xfff0}, 16);
    mos6502::Program mos(layout, small);
    mc6809::Program mc(layout, exact);
    EXPECT_THROW(mos.emit(phase_detail::handshake<Mos6502>({}, {}, 1)), std::out_of_range);
    EXPECT_EQ(mos.size(), 0u);
    EXPECT_EQ(memory.writes, 0u);
    mc.emit(phase_detail::handshake<Mc6809>({}, {}, 1));
    EXPECT_EQ(mc.size(), 16u);
    EXPECT_NO_THROW(mc.install());
    EXPECT_EQ(memory.bytes[0xffff], 0x03);
}

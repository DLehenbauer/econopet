// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "types.h"

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {
using namespace econopet;

// A distinct flag domain verifies that masks cannot cross API boundaries.
enum class OtherBit : uint8_t { Enabled = 1, All = Enabled };
using OtherFlags = Flags<OtherBit>;

// Exercise byte validation independently of an enum's declared mask width.
enum class WideBit : uint16_t { Byte = 0xff, High = 0x100, All = Byte | High };
using WideFlags = Flags<WideBit>;

// Check multiplier participation without triggering an implicit numeric conversion.
template<class Multiplier>
concept CycleMultiplier = requires(Cycles duration, Multiplier count) {
    duration * count;
};

static_assert(CycleMultiplier<int>);
static_assert(CycleMultiplier<uint64_t>);
static_assert(!CycleMultiplier<float>);
static_assert(!CycleMultiplier<double>);
static_assert(!CycleMultiplier<long double>);
static_assert(!std::is_convertible_v<CpuAddress, uint16_t>);
static_assert(!std::is_convertible_v<SramAddress, CpuAddress>);
static_assert(!std::is_convertible_v<WishboneAddress, SramAddress>);
static_assert(!std::is_constructible_v<SramAddress, CpuAddress>);
static_assert(!std::is_constructible_v<WishboneAddress, CpuAddress>);
static_assert(!std::is_constructible_v<WishboneAddress, SramAddress>);
static_assert(!std::is_constructible_v<CpuAddress, WishboneAddress>);
static_assert(!std::is_constructible_v<SramAddress, WishboneAddress>);
static_assert(!std::is_constructible_v<CpuAddress, SramAddress>);
static_assert(!std::is_convertible_v<uint32_t, CpuAddress>);
static_assert(!std::is_convertible_v<uint32_t, SramAddress>);
static_assert(!std::is_convertible_v<uint32_t, WishboneAddress>);
static_assert(!std::is_constructible_v<Cycles, CpuAddress>);
static_assert(!std::is_constructible_v<Cycles, double>);
static_assert(!std::is_constructible_v<CpuAddress, double>);
static_assert(!std::is_convertible_v<CycleTime, Cycles>);
static_assert(!std::is_convertible_v<CycleTime, uint64_t>);
static_assert(!std::is_convertible_v<uint64_t, CycleTime>);
static_assert(!std::is_constructible_v<CpuControl, OtherBit>);
static_assert(!std::is_constructible_v<CpuControl, OtherFlags>);
static_assert(!std::is_constructible_v<ByteView, const uint8_t*, size_t>);
static_assert(std::is_constructible_v<ByteView, uint8_t (&)[2]>);
static_assert(std::is_constructible_v<ByteView, const uint8_t (&)[2]>);
static_assert(!std::is_constructible_v<ByteView, uint8_t (&&)[2]>);
static_assert(!std::is_constructible_v<ByteView, const uint8_t (&&)[2]>);
static_assert(std::is_constructible_v<ByteView, std::vector<uint8_t>&>);
static_assert(std::is_constructible_v<ByteView, const std::vector<uint8_t>&>);
static_assert(!std::is_constructible_v<ByteView, std::vector<uint8_t>&&>);
static_assert(!std::is_constructible_v<ByteView, const std::vector<uint8_t>&&>);
static_assert(std::is_constructible_v<ByteView, std::array<uint8_t, 2>&>);
static_assert(std::is_constructible_v<ByteView, const std::array<uint8_t, 2>&>);
static_assert(!std::is_constructible_v<ByteView, std::array<uint8_t, 2>&&>);
static_assert(!std::is_constructible_v<ByteView, const std::array<uint8_t, 2>&&>);
static_assert(std::is_constructible_v<ByteView, std::array<uint8_t, 0>&>);
static_assert(std::is_constructible_v<ByteView, const std::array<uint8_t, 0>&>);
static_assert(!std::is_constructible_v<ByteView, std::array<uint8_t, 0>&&>);
static_assert(!std::is_constructible_v<ByteView, const std::array<uint8_t, 0>&&>);
static_assert(std::is_constructible_v<ByteView, std::span<const uint8_t>>);
static_assert(std::is_convertible_v<std::span<uint8_t, 2>, ByteView>);
static_assert(std::is_convertible_v<std::span<const uint8_t, 2>, ByteView>);
static_assert(!std::is_constructible_v<ByteView, std::span<const uint16_t>>);
static_assert((CpuAddress{0x1234} + 2).value() == 0x1236);
static_assert((Cycles{2} + Cycles{3}).half_ticks() == 10);
static_assert((Cycles{6} * 3).value() == 18);
static_assert((CycleTime{Cycles{2}} + Cycles{3}).value() == 5);
static_assert((CpuControlBit::Ready | CpuControlBit::Reset).bits() == 3);

// Independent hardware expectations prevent shared definitions hiding a bad change.
TEST(BoardTypes, AddressCapacitiesMatchHardware) {
    EXPECT_EQ(CpuAddressCapacity, 65536u);
    EXPECT_EQ(SramCapacity, 131072u);
    EXPECT_EQ(WishboneAddressCapacity, 1048576u);
}

// Diagnostic memory ranges exclude the SRAM mirror and unused BRAM window space.
TEST(BoardTypes, DiagnosticMemoryRangesMatchHardware) {
    EXPECT_EQ(ECONOPET_WB_RAM_BASE_ADDR, 0x00000u);
    EXPECT_EQ(ECONOPET_WB_RAM_BASE_ADDR + (1u << ECONOPET_RAM_ADDR_WIDTH) - 1u, 0x1ffffu);
    EXPECT_EQ(ECONOPET_WB_BRAM_BASE_ADDR, 0x68000u);
    EXPECT_EQ(ECONOPET_WB_BRAM_BASE_ADDR + (1u << ECONOPET_BRAM_ADDR_WIDTH) - 1u, 0x68fffu);
}

// Independent wire expectations distinguish indices, addresses and read/write bits.
TEST(BoardTypes, IeeeEncodingsMatchHardware) {
    EXPECT_EQ(ECONOPET_IEEE_REG_COUNT, 8u);
    EXPECT_EQ(ECONOPET_IEEE_REG_CTRL, 0u);
    EXPECT_EQ(ECONOPET_IEEE_REG_TXS_LAST, 7u);
    EXPECT_EQ(ECONOPET_WB_IEEE_CTRL_ADDR, 0x70000u);
    EXPECT_EQ(ECONOPET_WB_IEEE_TXS_LAST_ADDR, 0x70007u);
    EXPECT_EQ(ECONOPET_IEEE_CTRL_FLUSH_MASK, 0x02u);
    EXPECT_EQ(ECONOPET_IEEE_CTRL_RD_TX_ROOM_MASK, 0x02u);
    EXPECT_EQ(ECONOPET_IEEE_CTRL_DATA_FLUSH_MASK, 0x04u);
    EXPECT_EQ(ECONOPET_IEEE_ST_RX_ATN_MASK, 0x02u);
    EXPECT_EQ(ECONOPET_IEEE_ST_TALK_STARVED_MASK, 0x80u);
    EXPECT_EQ(ECONOPET_IEEE_TX_BURST_CHUNK, 64u);
}

// Check each domain's endpoints and signed displacement independently.
TEST(BoardTypes, AddressesCheckConstructionAndOffsetBoundaries) {
    const auto verify = [](auto zero, const uint64_t capacity) {
        using AddressType = decltype(zero);
        const AddressType last{capacity - 1};
        EXPECT_EQ(zero.value(), 0u);
        EXPECT_EQ((zero + (capacity - 1)), last);
        EXPECT_EQ(last + 0, last);
        EXPECT_EQ(last - zero, static_cast<int64_t>(capacity - 1));
        EXPECT_EQ(zero - last, -static_cast<int64_t>(capacity - 1));
        EXPECT_NE(last, zero);
        EXPECT_THROW(AddressType{capacity}, std::out_of_range);
        EXPECT_THROW(AddressType{-1}, std::out_of_range);
        EXPECT_THROW(zero + -1, std::out_of_range);
        EXPECT_THROW(last + 1, std::out_of_range);
    };
    verify(CpuAddress{}, CpuAddressCapacity);
    verify(SramAddress{}, SramCapacity);
    verify(WishboneAddress{}, WishboneAddressCapacity);
}

// Extreme signed offsets and large integers cannot wrap in any address domain.
TEST(BoardTypes, FailureContractInvalidAddressesNameTheirDomainAndOperands) {
    const auto verify = [](auto zero, auto last, const char* domain) {
        using AddressType = decltype(zero);
        const auto rejected = [&](auto operation) {
            try {
                operation();
                FAIL() << "invalid address accepted in " << domain;
            } catch (const std::out_of_range& error) {
                EXPECT_NE(std::string(error.what()).find(domain), std::string::npos) << error.what();
                EXPECT_NE(std::string(error.what()).find("address="), std::string::npos) << error.what();
            }
        };
        rejected([] { AddressType{std::numeric_limits<int64_t>::min()}; });
        rejected([] { AddressType{std::numeric_limits<uint64_t>::max()}; });
        rejected([&] { zero + std::numeric_limits<int64_t>::min(); });
        rejected([&] { last + std::numeric_limits<uint64_t>::max(); });
        rejected([&] { last + 1; });
        EXPECT_EQ(last + -static_cast<int64_t>(last.value()), zero);
    };
    verify(CpuAddress{0}, CpuAddress{CpuAddressCapacity - 1}, "CpuAddress");
    verify(SramAddress{0}, SramAddress{SramCapacity - 1}, "SramAddress");
    verify(WishboneAddress{0}, WishboneAddress{WishboneAddressCapacity - 1}, "WishboneAddress");
}

// Durations reject invalid construction and incomplete full-cycle conversion.
TEST(BoardTypes, DurationsCheckConstructionAndHalfTicks) {
    EXPECT_EQ(Cycles{0}.half_ticks(), 0u);
    EXPECT_EQ(Cycles{3}.half_ticks(), 6u);
    EXPECT_EQ(Cycles::from_half_ticks(6), Cycles{3});
    EXPECT_EQ(Cycles{Cycles::Maximum}.half_ticks(), Cycles::Maximum * HalfTicksPerCycle);
    EXPECT_EQ(Cycles::from_half_ticks(Cycles::Maximum * HalfTicksPerCycle),
        Cycles{Cycles::Maximum});
    EXPECT_THROW(Cycles{-1}, std::out_of_range);
    EXPECT_THROW(Cycles{std::numeric_limits<int64_t>::min()}, std::out_of_range);
    EXPECT_THROW(Cycles{Cycles::Maximum + 1}, std::out_of_range);
    EXPECT_THROW(Cycles{std::numeric_limits<uint64_t>::max()}, std::out_of_range);
    EXPECT_THROW(Cycles::from_half_ticks(1), std::logic_error);
    EXPECT_THROW(Cycles::from_half_ticks(std::numeric_limits<uint64_t>::max()),
        std::logic_error);
}

// Duration arithmetic never wraps at either representable endpoint.
TEST(BoardTypes, DurationArithmeticChecksOverflowAndUnderflow) {
    const Cycles maximum{Cycles::Maximum};
    EXPECT_EQ(Cycles{2} + Cycles{3}, Cycles{5});
    EXPECT_EQ(maximum + Cycles{0}, maximum);
    EXPECT_EQ(maximum - maximum, Cycles{0});
    EXPECT_EQ(Cycles{6} * 3, Cycles{18});
    EXPECT_EQ(maximum * 1, maximum);
    EXPECT_EQ(maximum * 0, Cycles{0});
    EXPECT_EQ(Cycles{0} * std::numeric_limits<uint64_t>::max(), Cycles{0});
    EXPECT_THROW(maximum + Cycles{1}, std::overflow_error);
    EXPECT_THROW(Cycles{0} - Cycles{1}, std::out_of_range);
    EXPECT_THROW(maximum * 2, std::overflow_error);
    EXPECT_THROW(Cycles{1} * std::numeric_limits<uint64_t>::max(), std::overflow_error);
    EXPECT_TRUE(Cycles{2} < Cycles{3});
    EXPECT_TRUE(Cycles{3} > Cycles{2});
    EXPECT_TRUE(Cycles{3} <= Cycles{3});
    EXPECT_TRUE(Cycles{3} >= Cycles{3});
    EXPECT_NE(Cycles{2}, Cycles{3});
}

// Reject signed negative multipliers before conversion, even for an empty duration.
TEST(BoardTypes, DurationMultipliersRejectNegativeIntegers) {
    for (const Cycles duration : {Cycles{0}, Cycles{6}}) {
        EXPECT_THROW(duration * -1, std::out_of_range);
        EXPECT_THROW(duration * std::numeric_limits<int64_t>::min(), std::out_of_range);
    }
    EXPECT_EQ(Cycles{6} * int64_t{3}, Cycles{18});
    EXPECT_EQ(Cycles{6} * uint64_t{3}, Cycles{18});
}

// Timestamps use duration arithmetic without becoming interchangeable with it.
TEST(BoardTypes, TimestampsCheckDeadlinesAndElapsedTime) {
    const CycleTime start{Cycles{3}};
    const auto end = start + Cycles{2};
    EXPECT_EQ(end.value(), 5u);
    EXPECT_EQ(end - start, Cycles{2});
    EXPECT_EQ(start - start, Cycles{0});
    EXPECT_EQ(start + Cycles{0}, start);
    EXPECT_NE(start, end);
    EXPECT_TRUE(start < end);
    EXPECT_TRUE(end > start);
    EXPECT_THROW(start - end, std::out_of_range);
    EXPECT_THROW(CycleTime{Cycles{Cycles::Maximum}} + Cycles{1}, std::overflow_error);
    std::ostringstream message;
    message << Cycles{2} << " at " << end;
    EXPECT_EQ(message.str(), "2 cycles at cycle 5");
}

// Flag construction, combination and queries reject unknown bits before narrowing.
TEST(BoardTypes, FlagSetsCheckMasksAndPreserveTheirDomain) {
    const CpuControl empty;
    const auto ready_reset = CpuControlBit::Ready | CpuControlBit::Reset;
    EXPECT_EQ(empty.bits(), 0);
    EXPECT_TRUE(empty.contains(CpuControl{}));
    EXPECT_FALSE(empty.contains(CpuControlBit::Ready));
    EXPECT_TRUE(ready_reset.contains(CpuControlBit::Ready));
    EXPECT_TRUE(ready_reset.contains(CpuControlBit::Reset));
    EXPECT_FALSE(ready_reset.contains(CpuControlBit::Nmi));
    EXPECT_TRUE(ready_reset.contains(ready_reset));
    EXPECT_FALSE(CpuControl{CpuControlBit::Ready}.contains(ready_reset));
    EXPECT_EQ(ready_reset | CpuControl{CpuControlBit::Nmi}, CpuControl{CpuControlBit::All});
    EXPECT_EQ(CpuControl::from_bits(3), ready_reset);
    EXPECT_EQ(CpuControl::from_bits(0), empty);
    EXPECT_NE(empty, ready_reset);
    EXPECT_THROW(CpuControl::from_bits(8), std::invalid_argument);
    EXPECT_THROW(CpuControl::from_bits(0x100), std::invalid_argument);
    EXPECT_THROW(CpuControl::from_bits(std::numeric_limits<uint64_t>::max()),
        std::invalid_argument);
    EXPECT_THROW(CpuControl{static_cast<CpuControlBit>(8)}, std::invalid_argument);
    EXPECT_THROW(ready_reset.contains(static_cast<CpuControlBit>(8)), std::invalid_argument);
}

// Reject high bits even when a wider enum mask declares them valid.
TEST(BoardTypes, FlagSetsRejectBitsBeyondByteCapacity) {
    const auto byte = WideFlags::from_bits(std::to_underlying(WideBit::Byte));
    EXPECT_EQ(byte.bits(), std::numeric_limits<uint8_t>::max());
    EXPECT_EQ(WideFlags::from_bits(0).bits(), 0);
    EXPECT_TRUE(byte.contains(WideBit::Byte));
    EXPECT_EQ(WideFlags{WideBit::Byte}, byte);
    EXPECT_THROW(WideFlags::from_bits(std::to_underlying(WideBit::High)),
        std::invalid_argument);
    EXPECT_THROW(WideFlags::from_bits(std::to_underlying(WideBit::All)),
        std::invalid_argument);
    EXPECT_THROW(WideFlags{WideBit::High}, std::invalid_argument);
    EXPECT_THROW(WideFlags{WideBit::All}, std::invalid_argument);
    EXPECT_THROW(byte.contains(WideBit::High), std::invalid_argument);
    EXPECT_THROW(WideBit::Byte | WideBit::High, std::invalid_argument);
}

// Borrowed views retain container extents and observe storage without copying.
TEST(BoardTypes, ByteViewsCheckContainerExtents) {
    const uint8_t c_array[]{0x12, 0x34};
    constexpr std::array<uint8_t, 2> fixed{{0x56, 0x78}};
    std::vector<uint8_t> dynamic{0x9a, 0xbc};
    const ByteView c_view{c_array};
    const ByteView fixed_view{fixed};
    const ByteView dynamic_view{dynamic};
    EXPECT_EQ(c_view.size(), 2u);
    EXPECT_EQ(fixed_view.size(), 2u);
    EXPECT_EQ(dynamic_view.size(), 2u);
    EXPECT_EQ(c_view[0], 0x12);
    EXPECT_EQ(fixed_view[1], 0x78);
    dynamic[1] = 0xde;
    EXPECT_EQ(dynamic_view[1], 0xde);
    EXPECT_THROW(c_view[2], std::out_of_range);
    EXPECT_THROW(fixed_view[2], std::out_of_range);
    EXPECT_THROW(dynamic_view[std::numeric_limits<size_t>::max()], std::out_of_range);
    const std::array<uint8_t, 0> empty_array{};
    const std::vector<uint8_t> empty_vector;
    const ByteView empty_fixed{empty_array};
    const ByteView empty_dynamic{empty_vector};
    EXPECT_EQ(empty_fixed.size(), 0u);
    EXPECT_EQ(empty_dynamic.size(), 0u);
    EXPECT_THROW(empty_fixed[0], std::out_of_range);
    EXPECT_THROW(empty_dynamic[0], std::out_of_range);
}

// Span inputs preserve subrange extents while wrapper indexing remains checked.
TEST(BoardTypes, ByteViewsCheckBorrowedSpanExtents) {
    std::array<uint8_t, 4> bytes{{0x12, 0x34, 0x56, 0x78}};
    const ByteView middle{std::span{bytes}.subspan(1, 2)};
    const ByteView fixed{std::span{bytes}.subspan<1, 2>()};
    const ByteView immutable{std::span<const uint8_t>{bytes}};
    const ByteView empty{std::span<const uint8_t>{}};
    EXPECT_EQ(middle.size(), 2u);
    EXPECT_EQ(fixed.size(), 2u);
    EXPECT_EQ(immutable.size(), bytes.size());
    EXPECT_EQ(empty.size(), 0u);
    EXPECT_EQ(middle[0], 0x34);
    EXPECT_EQ(fixed[1], 0x56);
    bytes[1] = 0x9a;
    EXPECT_EQ(middle[0], 0x9a);
    EXPECT_EQ(fixed[0], 0x9a);
    EXPECT_EQ(immutable[1], 0x9a);
    EXPECT_THROW(middle[2], std::out_of_range);
    EXPECT_THROW(fixed[2], std::out_of_range);
    EXPECT_THROW(immutable[bytes.size()], std::out_of_range);
    EXPECT_THROW(empty[0], std::out_of_range);
    constexpr uint8_t constant[]{0xab, 0xcd};
    static_assert(ByteView{std::span{constant}}[1] == 0xcd);
}
} // namespace

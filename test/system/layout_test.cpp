// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <array>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "framework/asm.h"
#include "host_memory.h"

namespace {
using namespace test_asm;
using econopet::CpuAddress;
using econopet::SramAddress;

static_assert(!std::is_copy_constructible_v<Scenario<HostMemory>>);
static_assert(!std::is_constructible_v<Scenario<HostMemory>, HostMemory&&>);
static_assert(!std::is_reference_v<decltype(std::declval<MemoryByte&&>().name())>);
static_assert(!std::is_reference_v<decltype(std::declval<const MemoryByte&&>().name())>);
static_assert(std::is_reference_v<decltype(std::declval<const MemoryCapture&>().raw_bytes())>);
static_assert(!std::is_reference_v<decltype(std::declval<MemoryCapture&&>().raw_bytes())>);
static_assert(!std::is_reference_v<decltype(std::declval<const MemoryCapture&&>().raw_bytes())>);
}

TEST(CheckedLayout, ValidatesReservationsBeforeWrites) {
    HostMemory memory;
    Scenario layout(memory);
    const auto region = layout.reserve("data", RegionKind::Data, CpuAddress{0x200}, 4);
    layout.reserve("adjacent", RegionKind::Data, CpuAddress{0x204}, 1);
    EXPECT_THROW(layout.reserve("", RegionKind::Data, CpuAddress{0}, 1), std::invalid_argument);
    EXPECT_THROW(layout.reserve("empty", RegionKind::Data, CpuAddress{0}, 0), std::out_of_range);
    EXPECT_THROW(layout.reserve("bad kind", static_cast<RegionKind>(99), CpuAddress{0}, 1), std::invalid_argument);
    EXPECT_THROW(layout.reserve("data", RegionKind::Data, CpuAddress{0}, 1), std::invalid_argument);
    EXPECT_THROW(layout.reserve("left", RegionKind::Data, CpuAddress{0x1ff}, 2), std::invalid_argument);
    EXPECT_THROW(layout.reserve("right", RegionKind::Data, CpuAddress{0x203}, 2), std::invalid_argument);
    EXPECT_THROW(layout.reserve("inside", RegionKind::Data, CpuAddress{0x201}, 1), std::invalid_argument);
    EXPECT_THROW(layout.reserve("enclosing", RegionKind::Data, CpuAddress{0x1ff}, 6), std::invalid_argument);
    EXPECT_THROW(layout.reserve("wrap", RegionKind::Data, CpuAddress{0xffff}, 2), std::out_of_range);
    EXPECT_THROW(layout.reserve("huge", RegionKind::Data, CpuAddress{0}, std::numeric_limits<size_t>::max()),
        std::out_of_range);
    EXPECT_EQ(layout.region("data").start(), region.start());
    EXPECT_THROW(layout.region("missing"), std::invalid_argument);
    EXPECT_EQ(memory.writes, 0u);
}

TEST(CheckedLayout, LastByteFullSpaceAndSlicesRemainBounded) {
    HostMemory memory;
    Scenario layout(memory);
    const auto full = layout.reserve("whole CPU space", RegionKind::Data, CpuAddress{0},
        econopet::CpuAddressCapacity);
    EXPECT_EQ(full.at(full.size() - 1), CpuAddress{0xffff});
    const auto last = full.slice(full.size() - 1, 1);
    EXPECT_EQ(last.start(), CpuAddress{0xffff});
    EXPECT_THROW(full.at(full.size()), std::out_of_range);
    EXPECT_THROW(full.slice(0, 0), std::out_of_range);
    EXPECT_THROW(full.slice(full.size(), 1), std::out_of_range);
    EXPECT_THROW(full.slice(1, full.size()), std::out_of_range);
    EXPECT_THROW(full.slice(0, std::numeric_limits<size_t>::max()), std::out_of_range);
    const std::array<uint8_t, 1> value{0x5a};
    layout.write(last, 0, value);
    const std::array<uint8_t, 0> empty{};
    EXPECT_NO_THROW(layout.write(last, 1, empty));
    EXPECT_THROW(layout.write(last, 1, value), std::out_of_range);
    EXPECT_THROW(layout.write(last, 2, empty), std::out_of_range);
    EXPECT_EQ(memory.peek(SramAddress{0xffff}), 0x5a);
    EXPECT_EQ(memory.writes, 1u);
}

TEST(CheckedLayout, EnforcesOwnerPurposeAndSubviewContainment) {
    HostMemory memory;
    Scenario layout(memory);
    Scenario foreign(memory);
    const auto data = layout.reserve("data", RegionKind::Data, CpuAddress{0x200}, 4);
    const auto other = foreign.reserve("data", RegionKind::Data, CpuAddress{0x200}, 4);
    EXPECT_THROW(layout.require(other, RegionKind::Data), std::invalid_argument);
    EXPECT_THROW(layout.require(data, RegionKind::Code), std::invalid_argument);
    EXPECT_THROW(layout.lower_bank(other), std::invalid_argument);
    const std::array<uint8_t, 1> byte{0x42};
    EXPECT_THROW(layout.write(other, 0, byte), std::invalid_argument);
    for (const auto kind : {RegionKind::Code, RegionKind::Stack, RegionKind::Mailbox, RegionKind::Vectors}) {
        Scenario reserved(memory);
        const auto special = reserved.reserve("special", kind, CpuAddress{0x300}, 2);
        EXPECT_THROW(reserved.lower_bank(special), std::invalid_argument);
    }
    const auto view = layout.lower_bank(data);
    auto narrow = layout.lower_bank(data.slice(1, 2));
    const auto buffer = view.buffer("all", 0, 4);
    EXPECT_THROW(narrow.read(buffer), std::invalid_argument);
    const std::array<uint8_t, 4> bytes{};
    EXPECT_THROW(narrow.initialize(buffer, bytes), std::invalid_argument);
    EXPECT_NO_THROW(layout.require(narrow.byte("same owner")));
    EXPECT_THROW(layout.require(foreign.lower_bank(other).byte("foreign")), std::invalid_argument);
    EXPECT_EQ(memory.writes, 0u);
}

TEST(OwnedMemory, HandlesValidateWidthsNamesAndBoundsBeforeMutation) {
    HostMemory memory;
    Scenario layout(memory);
    auto view = layout.lower_bank(layout.reserve("data", RegionKind::Data, CpuAddress{0xfffc}, 4));
    const auto bytes = view.buffer("bytes", 0, 4);
    const auto word = bytes.word(2, ByteOrder::BigEndian);
    EXPECT_THROW(bytes.byte(4), std::out_of_range);
    EXPECT_THROW(bytes.word(3, ByteOrder::LittleEndian), std::out_of_range);
    EXPECT_THROW(word.byte(2), std::out_of_range);
    EXPECT_THROW(bytes.slice(0, 0), std::out_of_range);
    EXPECT_THROW(bytes.slice(3, 2), std::out_of_range);
    EXPECT_THROW(view.byte(""), std::invalid_argument);
    EXPECT_THROW(view.buffer("empty", 0, 0), std::out_of_range);
    EXPECT_THROW(view.word("bad order", 0, static_cast<ByteOrder>(99)), std::invalid_argument);
    EXPECT_THROW(view.initialize(bytes.byte(0), -1), std::out_of_range);
    EXPECT_THROW(view.initialize(bytes.byte(0), 256), std::out_of_range);
    EXPECT_THROW(view.initialize(word, -1), std::out_of_range);
    EXPECT_THROW(view.initialize(word, 65536), std::out_of_range);
    const std::array<uint8_t, 1> short_data{0x42};
    EXPECT_THROW(view.initialize(bytes, short_data), std::out_of_range);
    EXPECT_EQ(memory.writes, 0u);
    view.initialize(word, CpuAddress{0x1234});
    EXPECT_EQ(view.read(word), 0x1234);
    EXPECT_EQ(view.read(word.byte(0)), 0x12);
    EXPECT_EQ(view.read(word.byte(1)), 0x34);
    EXPECT_EQ(bytes.slice(3, 1).address(), CpuAddress{0xffff});
}

TEST(OwnedMemory, RoundTripsBothByteOrdersAndCapturesDetachedBuffers) {
    for (const auto order : {ByteOrder::LittleEndian, ByteOrder::BigEndian}) {
        HostMemory memory;
        Scenario layout(memory);
        auto view = layout.lower_bank(layout.reserve("data", RegionKind::Results, CpuAddress{0x200}, 5));
        const auto word = view.word("word", 0, order);
        const auto buffer = view.buffer("payload", 2, 3);
        const std::array<uint8_t, 3> payload{0, 0x42, 0xff};
        view.initialize(word, 0xabcd);
        view.initialize(buffer, payload);
        const auto capture = view.capture();
        EXPECT_EQ(capture[word], 0xabcd);
        EXPECT_EQ(capture[word.byte(0)], order == ByteOrder::LittleEndian ? 0xcd : 0xab);
        EXPECT_EQ(capture[buffer], (std::vector<uint8_t>{0, 0x42, 0xff}));
        EXPECT_EQ(capture.raw_bytes().size(), 5u);
        view.initialize(word, 0);
        EXPECT_EQ(capture[word], 0xabcd);
        EXPECT_EQ(view.read(word), 0);
    }
}

TEST(OwnedMemory, CaptureRejectsForeignAndMissingBytes) {
    HostMemory memory;
    Scenario layout(memory);
    Scenario foreign(memory);
    const auto region = layout.reserve("data", RegionKind::Data, CpuAddress{0x200}, 4);
    auto all = layout.lower_bank(region);
    const auto partial = layout.lower_bank(region.slice(0, 2)).capture();
    EXPECT_THROW(partial[all.byte("missing", 2)], std::invalid_argument);
    EXPECT_THROW(partial[all.buffer("partial", 1, 2)], std::invalid_argument);
    EXPECT_THROW(partial[all.word("partial word", 1, ByteOrder::LittleEndian)], std::invalid_argument);
    const auto other = foreign.lower_bank(foreign.reserve("data", RegionKind::Data, CpuAddress{0x200}, 4));
    EXPECT_THROW(partial[other.byte("foreign")], std::invalid_argument);
}

TEST(OwnedMemory, ViewsRejectSiblingExtentsAndPartialWordBeforeWrites) {
    HostMemory memory;
    Scenario layout(memory);
    const auto left = layout.reserve("left", RegionKind::Data, CpuAddress{0x200}, 2);
    const auto right = layout.reserve("right", RegionKind::Data, CpuAddress{0x202}, 2);
    auto view = layout.lower_bank(left);
    auto sibling = layout.lower_bank(right);
    const auto other = sibling.word("sibling word", 0, ByteOrder::LittleEndian);
    EXPECT_THROW(view.read(other), std::invalid_argument);
    EXPECT_THROW(view.initialize(other, 0xffff), std::invalid_argument);
    const auto word = view.word("wide", 0, ByteOrder::LittleEndian);
    auto one_byte = layout.lower_bank(left.slice(0, 1));
    EXPECT_THROW(one_byte.read(word), std::invalid_argument);
    EXPECT_THROW(one_byte.initialize(word, 0xffff), std::invalid_argument);
    EXPECT_EQ(memory.writes, 0u);
    EXPECT_EQ(memory.reads, 0u);
    view.initialize(word, 0xffff);
    EXPECT_EQ(view.read(word), 0xffff);
    view.initialize(word.byte(0), 0xff);
    EXPECT_EQ(view.read(word.byte(0)), 0xff);
}

TEST(OwnedMemory, ViewMoveKeepsDestinationAndRejectsSourceAccess) {
    HostMemory memory;
    Scenario layout(memory);
    auto original = layout.lower_bank(layout.reserve("data", RegionKind::Data, CpuAddress{0}, 2));
    const auto byte = original.byte("byte");
    auto moved = std::move(original);
    EXPECT_THROW(original.byte("new"), std::logic_error);
    EXPECT_THROW(original.read(byte), std::logic_error);
    EXPECT_THROW(original.initialize(byte, 1), std::logic_error);
    EXPECT_THROW(original.capture(), std::logic_error);
    moved.initialize(byte, 0xff);
    EXPECT_EQ(moved.capture()[byte], 0xff);
    auto copied = moved;
    EXPECT_EQ(copied.read(byte), 0xff);
}

TEST(OwnedMemory, DetachedCaptureAndHandlesOutliveLayoutAndBacking) {
    std::optional<MemoryByte> byte;
    std::optional<MemoryCapture> capture;
    {
        HostMemory memory;
        Scenario layout(memory);
        auto view = layout.lower_bank(layout.reserve("data", RegionKind::Workspace, CpuAddress{0x200}, 1));
        byte = view.byte("persistent operand diagnostic name");
        view.initialize(*byte, 0x42);
        capture = view.capture();
    }
    ASSERT_TRUE(byte);
    ASSERT_TRUE(capture);
    EXPECT_EQ((*capture)[*byte], 0x42);
    EXPECT_EQ(byte->name(), "persistent operand diagnostic name");
}

TEST(CheckedLayout, MoveTransfersIdentityAndRejectsAllSourceOperations) {
    HostMemory memory;
    Scenario original(memory);
    const auto region = original.reserve("data", RegionKind::Data, CpuAddress{0x200}, 2);
    auto view = original.lower_bank(region);
    const auto byte = view.byte("byte");
    Scenario transferred(std::move(original));
    EXPECT_NO_THROW(transferred.require(region, RegionKind::Data));
    EXPECT_NO_THROW(transferred.require(byte));
    EXPECT_THROW(original.reserve("new", RegionKind::Data, CpuAddress{0}, 1), std::logic_error);
    EXPECT_THROW(original.region("data"), std::logic_error);
    EXPECT_THROW(original.lower_bank(region), std::logic_error);
    EXPECT_THROW(original.require(byte), std::logic_error);
    EXPECT_THROW(original.require(region, RegionKind::Data), std::logic_error);
    const std::array<uint8_t, 0> empty{};
    EXPECT_THROW(original.write(region, 0, empty), std::logic_error);
    view.initialize(byte, 0x42);
    EXPECT_EQ(transferred.lower_bank(region).read(byte), 0x42);
}

TEST(OwnedMemory, TemporaryAccessTransfersOrCopiesOwnedData) {
    HostMemory memory;
    Scenario layout(memory);
    auto view = layout.lower_bank(layout.reserve("data", RegionKind::Data, CpuAddress{0}, 1));
    const auto& name = view.byte("temporary operand diagnostic name").name();
    EXPECT_EQ(name, "temporary operand diagnostic name");
    const auto byte = view.byte("immutable operand diagnostic name");
    const auto& copied_name = std::move(byte).name();
    EXPECT_EQ(copied_name, byte.name());
    EXPECT_NE(copied_name.data(), byte.name().data());
    view.initialize(byte, 0x42);
    const auto& bytes = view.capture().raw_bytes();
    EXPECT_EQ(bytes, (std::vector<uint8_t>{0x42}));
    const auto immutable = view.capture();
    const auto& copied = std::move(immutable).raw_bytes();
    EXPECT_EQ(copied, immutable.raw_bytes());
    EXPECT_NE(copied.data(), immutable.raw_bytes().data());
    auto original = view.capture();
    const auto transferred = std::move(original);
    EXPECT_EQ(transferred[byte], 0x42);
    EXPECT_THROW(original[byte], std::logic_error);
    EXPECT_THROW(original.raw_bytes(), std::logic_error);
    auto drained = view.capture();
    EXPECT_EQ(std::move(drained).raw_bytes(), bytes);
    EXPECT_THROW(drained[byte], std::logic_error);
}

TEST(CheckedLayout, NamedLookupIncludesEveryReservationPurpose) {
    HostMemory memory;
    Scenario layout(memory);
    size_t offset = 0;
    for (const auto kind : {RegionKind::Code, RegionKind::Stack, RegionKind::Mailbox,
                           RegionKind::Results, RegionKind::Vectors, RegionKind::Data, RegionKind::Workspace}) {
        const auto name = "region " + std::to_string(offset);
        const auto reserved = layout.reserve(name, kind, CpuAddress{offset}, 1);
        const auto found = layout.region(name);
        EXPECT_EQ(found.start(), reserved.start());
        EXPECT_EQ(found.size(), reserved.size());
        EXPECT_NO_THROW(layout.require(found, kind));
        ++offset;
    }
}

TEST(OwnedMemory, BulkAccessVisitsBackingOncePerByteAndCopiesSelectedExtent) {
    HostMemory memory;
    Scenario layout(memory);
    auto view = layout.lower_bank(layout.reserve("full", RegionKind::Data, CpuAddress{0},
        econopet::CpuAddressCapacity));
    const auto buffer = view.buffer(std::string(256, 'x'), 0, econopet::CpuAddressCapacity);
    std::vector<uint8_t> bytes(buffer.size());
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(i);
    view.initialize(buffer, bytes);
    EXPECT_EQ(memory.writes, buffer.size());
    EXPECT_EQ(view.read(buffer), bytes);
    EXPECT_EQ(memory.reads, buffer.size());
    const auto captured = view.capture();
    EXPECT_EQ(memory.reads, buffer.size() * 2);
    EXPECT_EQ(captured[buffer], bytes);
    const auto tail = buffer.slice(buffer.size() - 2, 2);
    EXPECT_EQ(captured[tail], (std::vector<uint8_t>{0xfe, 0xff}));
    EXPECT_EQ(captured[tail.word(0, ByteOrder::BigEndian)], 0xfeff);
    EXPECT_EQ(memory.reads, buffer.size() * 2);
    const auto before = memory.writes;
    const std::array<uint8_t, 1> short_data{0x42};
    EXPECT_THROW(view.initialize(buffer, short_data), std::out_of_range);
    EXPECT_EQ(memory.writes, before);
}

TEST(CheckedLayout, SharedExtentRuleRejectsZeroWrappingAndOversizedSlices) {
    constexpr auto maximum = std::numeric_limits<size_t>::max();
    static_assert(memory_detail::contains_extent(maximum, maximum - 1, 1));
    static_assert(!memory_detail::contains_extent(maximum, maximum - 1, 2));
    static_assert(!memory_detail::contains_extent(maximum, maximum, 1));
    static_assert(!memory_detail::contains_extent(4, 0, 0));
    HostMemory memory;
    Scenario layout(memory);
    const auto region = layout.reserve("data", RegionKind::Data, CpuAddress{0}, 4);
    const auto buffer = layout.lower_bank(region).buffer("buffer", 0, 4);
    for (const auto offset : {size_t{0}, size_t{3}, size_t{4}, maximum}) {
        for (const auto size : {size_t{0}, size_t{1}, size_t{4}, maximum}) {
            if (memory_detail::contains_extent(4, offset, size)) {
                EXPECT_EQ(region.slice(offset, size).size(), size);
                EXPECT_EQ(buffer.slice(offset, size).size(), size);
            } else {
                EXPECT_THROW(region.slice(offset, size), std::out_of_range);
                EXPECT_THROW(buffer.slice(offset, size), std::out_of_range);
            }
        }
    }
}

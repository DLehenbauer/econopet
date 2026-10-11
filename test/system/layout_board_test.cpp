// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <array>
#include <stdexcept>
#include <utility>

#include "framework/asm.h"
#include "framework/system_test.h"

using namespace test_asm;
using econopet::CpuAddress;
using econopet::SramAddress;

TEST_F(SystemTest, CheckedLayoutUsesLowerBankWithoutCpuDecodingOrClocks) {
    const auto start = system.time();
    Scenario layout(system);
    auto view = layout.lower_bank(layout.reserve("workspace", RegionKind::Workspace,
        pet::Pia2PortA, 2));
    const auto word = view.word("pointer", 0, ByteOrder::BigEndian);
    const auto upper = SramAddress{econopet::CpuAddressCapacity + word.address().value()};
    system.poke(upper, 0x99);
    const auto writes = system.io().pia2().writes().count();
    auto model = system.trace_pia2_writes();
    auto bus = system.trace_peripheral_writes();
    view.initialize(word, CpuAddress{0x1234});
    EXPECT_EQ(view.read(word), 0x1234);
    EXPECT_EQ(system.peek(SramAddress{word.address().value()}), 0x12);
    EXPECT_EQ(system.peek(upper), 0x99);
    EXPECT_EQ(system.io().pia2().writes().count(), writes);
    EXPECT_EQ(system.time(), start);
    model.capture().require_absent("fixture initialization is not a model write");
    bus.capture().require_absent("fixture initialization is not a physical write");
}

TEST_F(SystemTest, CheckedLayoutValidationPreservesBackingAndBoardDiagnostics) {
    Scenario layout(system);
    const auto data = layout.reserve("data", RegionKind::Data, CpuAddress{0x200}, 2);
    auto view = layout.lower_bank(data);
    const auto buffer = view.buffer("buffer", 0, 2);
    const auto original = view.read(buffer);
    const auto start = system.time();
    expect_failure<std::invalid_argument>(system, "reserve", [&] {
        layout.reserve("overlap", RegionKind::Data, CpuAddress{0x201}, 1);
    }, "overlaps data");
    expect_failure<std::out_of_range>(system, "memory initialize", [&] {
        view.initialize(buffer.word(0, ByteOrder::LittleEndian), 65536);
    }, "storage width exceeded");
    const std::array<uint8_t, 3> oversized{1, 2, 3};
    expect_failure<std::out_of_range>(system, "scenario write", [&] {
        layout.write(data, 0, oversized);
    }, "fixture write exceeds data");
    EXPECT_EQ(view.read(buffer), original);
    EXPECT_EQ(system.time(), start);
}

TEST_F(SystemTest, CheckedLayoutMutationRejectsObservationWithoutPartialWrites) {
    Scenario layout(system);
    const auto data = layout.reserve("data", RegionKind::Results, CpuAddress{0x200}, 2);
    auto view = layout.lower_bank(data);
    const auto word = view.word("word", 0, ByteOrder::LittleEndian);
    const auto buffer = view.buffer("buffer", 0, 2);
    const std::array<uint8_t, 2> bytes{0x12, 0x34};
    system.run_until([&](const System&) {
        EXPECT_THROW(view.initialize(word, 0x1234), std::logic_error);
        EXPECT_THROW(view.initialize(buffer, bytes), std::logic_error);
        EXPECT_THROW(layout.write(data, 0, bytes), std::logic_error);
        EXPECT_EQ(view.read(word), 0xeaea);
        EXPECT_EQ(view.capture()[word], 0xeaea);
        return true;
    }, 0, "observation-only memory access");
    EXPECT_EQ(view.read(word), 0xeaea);
}

TEST_F(SystemTest, CheckedLayoutCaptureRemainsDetachedAcrossBoardMutation) {
    Scenario layout(system);
    const auto data = layout.reserve("data", RegionKind::Data, CpuAddress{0xffff}, 1);
    auto view = layout.lower_bank(data);
    const auto byte = view.byte("last byte");
    view.initialize(byte, 0x42);
    const auto captured = view.capture();
    view.initialize(byte, 0xff);
    system.tick(1);
    EXPECT_EQ(captured[byte], 0x42);
    EXPECT_EQ(view.read(byte), 0xff);
}

// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "framework/system_test.h"

namespace {
using namespace test_asm;
constexpr econopet::Cycles Budget{20000};

static_assert(std::is_reference_v<decltype(std::declval<const MemoryCapture&>().raw_bytes())>);
static_assert(!std::is_reference_v<decltype(std::declval<MemoryCapture&&>().raw_bytes())>);
static_assert(!std::is_reference_v<decltype(std::declval<const MemoryCapture&&>().raw_bytes())>);
static_assert(!std::is_reference_v<decltype(std::declval<MemoryByte&&>().name())>);
static_assert(!std::is_reference_v<decltype(std::declval<const MemoryByte&&>().name())>);
static_assert(!std::is_reference_v<decltype(std::declval<ExecutionResult&&>().window())>);
static_assert(!std::is_reference_v<decltype(std::declval<const ExecutionResult&&>().window())>);

// Exercises the same handles as instruction operands and detached result keys.
template<class Builder>
void exercise_data(Builder& test, System& system, ByteOrder order) {
    const auto byte = test.byte("answer");
    const auto word = test.word("pointer");
    const auto bytes = test.buffer("packet", 3);
    const auto scratch = test.buffer("scratch", 2, Capture::No);
    const auto payload = test.data("payload", econopet::CpuAddress{0x400}, 4, Capture::Yes);
    auto backing = test.lower_bank("payload");
    const std::array<uint8_t, 4> initial{1, 2, 3, 4};
    backing.initialize(payload, initial);
    const auto started = system.time();
    const std::array<uint8_t, 3> packet{0x42, 0, 0xff};
    const auto result = test.within(Budget).run([&](auto& p) {
        p.lda(imm(0x37)).sta(byte).store(word, payload.address()).store(bytes, packet)
         .lda(imm(0x99)).sta(payload.byte(2)).store(scratch.word(0, order), imm16(0x1234));
    });
    EXPECT_EQ(result[byte], 0x37);
    EXPECT_EQ(result[word], payload.address().value());
    EXPECT_EQ(word.order(), order);
    EXPECT_EQ(result[bytes], (std::vector<uint8_t>{0x42, 0, 0xff}));
    EXPECT_EQ(result[payload], (std::vector<uint8_t>{1, 2, 0x99, 4}));
    EXPECT_EQ(result[word.byte(0)], order == ByteOrder::LittleEndian ? 0 : 4);
    EXPECT_THROW(result[scratch], std::invalid_argument);
    EXPECT_EQ(result.started(), started);
    backing.initialize(payload, initial);
    EXPECT_EQ(result[payload.byte(2)], 0x99);
}
}

// Temporary result access returns independently owned data and interval identities.
TEST(MemoryOwnership, TemporaryExecutionAccessOwnsBytesAndWindow) {
    const auto source = std::make_shared<const int>(0);
    const econopet::CycleTime start{econopet::Cycles{0}};
    const econopet::CycleTime end{econopet::Cycles{1}};
    const auto result = [&] {
        return ExecutionResult(source, econopet::CpuAddress{0}, {0x42}, start, end,
            test_observation::TraceWindow(source, start, end));
    };
    const auto& bytes = result().raw_bytes();
    const auto& window = result().window();
    EXPECT_EQ(bytes, (std::vector<uint8_t>{0x42}));
    EXPECT_EQ(window.source(), source);
    EXPECT_EQ(window.completed(), end);

    const auto immutable = result();
    const auto& copied = std::move(immutable).raw_bytes();
    EXPECT_EQ(copied, immutable.raw_bytes());
    EXPECT_NE(copied.data(), immutable.raw_bytes().data());
    const auto& copied_window = std::move(immutable).window();
    EXPECT_EQ(copied_window.source(), source);
}

// Byte operands retain names independently of temporary handles and capture results.
TEST_F(SystemTest, TemporaryMemoryOperandAccessOwnsName) {
    auto program = system.program6502();
    const auto& name = program.byte("temporary operand diagnostic name").name();
    EXPECT_EQ(name, "temporary operand diagnostic name");
    const auto operand = program.byte("immutable operand diagnostic name");
    const auto& copied_name = std::move(operand).name();
    EXPECT_EQ(copied_name, operand.name());
    EXPECT_NE(copied_name.data(), operand.name().data());
}

TEST_F(SystemTest, OwnedMemory6502CapturesBytesWordsAndBuffers) {
    auto test = system.program6502();
    exercise_data(test, system, ByteOrder::LittleEndian);
}

TEST_F(SystemTest, OwnedMemory6809CapturesBytesWordsAndBuffers) {
    auto test = system.program6809();
    exercise_data(test, system, ByteOrder::BigEndian);
}

TEST_F(SystemTest, OwnedMemoryBoundsAndOwnerChecksPrecedeMutation) {
    auto test = system.program6502();
    const auto buffer = test.buffer("data", 3);
    auto view = test.lower_bank();
    auto foreign = system.program6502();
    const auto other = foreign.word("foreign");
    const auto before = system.time();
    const auto original = view.read(buffer);
    EXPECT_THROW(buffer.byte(3), std::out_of_range);
    EXPECT_THROW(buffer.word(2, ByteOrder::LittleEndian), std::out_of_range);
    EXPECT_THROW(buffer.slice(1, 3), std::out_of_range);
    EXPECT_THROW(buffer.slice(0, 0), std::out_of_range);
    EXPECT_THROW(view.word("end", 254, ByteOrder::LittleEndian), std::out_of_range);
    EXPECT_THROW(view.byte(""), std::invalid_argument);
    EXPECT_THROW(view.buffer("empty", 0, 0), std::out_of_range);
    EXPECT_THROW(view.word("bad order", 0, static_cast<ByteOrder>(99)), std::invalid_argument);
    EXPECT_THROW(view.initialize(other, 0x1234), std::invalid_argument);
    EXPECT_THROW(view.initialize(buffer.byte(0), 256), std::out_of_range);
    EXPECT_THROW(view.initialize(buffer.byte(0), -1), std::out_of_range);
    EXPECT_THROW(view.initialize(buffer.word(0, ByteOrder::LittleEndian), 65536), std::out_of_range);
    const uint8_t short_data[] = {0x42};
    EXPECT_THROW(view.initialize(buffer, short_data), std::out_of_range);
    EXPECT_EQ(view.read(buffer), original);
    EXPECT_EQ(system.time(), before);
    EXPECT_THROW(test.within(Budget).run([&](auto& p) { p.store(other, imm16(0x1234)); }),
        std::invalid_argument);
    EXPECT_EQ(view.read(buffer), original);
    EXPECT_EQ(system.time(), before);
}

TEST_F(SystemTest, OwnedMemoryAllocationValidatesNamesCapacityAndOverlap) {
    auto test = system.program6502();
    test.word("pointer");
    EXPECT_THROW(test.byte("pointer"), std::invalid_argument);
    EXPECT_THROW(test.buffer("", 3), std::invalid_argument);
    EXPECT_THROW(test.buffer("empty", 0), std::out_of_range);
    EXPECT_THROW(test.buffer("too large", 254), std::out_of_range);
    EXPECT_THROW(test.buffer("bad policy", 1, static_cast<Capture>(99)), std::invalid_argument);
    EXPECT_THROW(test.data("code alias", econopet::CpuAddress{0x300}, 1), std::invalid_argument);
    EXPECT_THROW(test.data("wrap", econopet::CpuAddress{0xffff}, 2), std::out_of_range);
    EXPECT_THROW(test.lower_bank("code"), std::invalid_argument);
    EXPECT_THROW(test.lower_bank("completion"), std::invalid_argument);
    EXPECT_THROW(test.lower_bank("unknown"), std::invalid_argument);
    EXPECT_EQ(test.buffer("remaining", 253).size(), 253);
}

TEST_F(SystemTest, OwnedMemoryHostInitializationDoesNotEmitCpuWrites) {
    auto test = system.program6809();
    const auto data = test.data("workspace", econopet::CpuAddress{0x400}, 4, Capture::Yes);
    auto view = test.lower_bank("workspace");
    const auto word = data.word(0, ByteOrder::BigEndian);
    const auto before = system.time();
    view.initialize(word, econopet::CpuAddress{0x1234});
    EXPECT_EQ(view.read(word), 0x1234);
    EXPECT_EQ(view.read(data.byte(0)), 0x12);
    EXPECT_EQ(system.time(), before);
    size_t writes = 0;
    auto subscription = system.observe([&](const System& board) {
        const auto pins = board.snapshot();
        if (pins.cpu_clock_o && !pins.cpu_we_n_o && pins.cpu_addr_o == data.address()
            && pins.soft6809_addr_o == data.address()) ++writes;
    });
    const auto result = test.within(Budget).run([&](auto& p) {
        p.store(data.word(2, ByteOrder::BigEndian), imm16(0x5678));
    });
    EXPECT_EQ(writes, 0);
    EXPECT_EQ(result[word], 0x1234);
    EXPECT_EQ(result[data.word(2, ByteOrder::BigEndian)], 0x5678);
}

TEST_F(SystemTest, OwnedMemoryUncapturedDataAndForeignCapturesRejectExplicitly) {
    auto test = system.program6502();
    const auto input = test.data("input", econopet::CpuAddress{0x400}, 1);
    auto other = system.program6502();
    const auto foreign = other.byte("answer");
    const auto result = test.within(Budget).run([&](auto& p) { p.lda(input.byte(0)); });
    EXPECT_THROW(result[input], std::invalid_argument);
    EXPECT_THROW(result[foreign], std::invalid_argument);
}

TEST_F(SystemTest, OwnedMemoryExistingDataCanOptIntoCaptureWithTheSameHandle) {
    auto test = system.program6502();
    const auto data = test.data("data", econopet::CpuAddress{0x400}, 4);
    const auto word = data.word(0, ByteOrder::LittleEndian);
    const auto byte = data.byte(3);
    test.capture(word);
    test.capture(byte);
    const auto result = test.within(Budget).run([&](auto& p) {
        p.store(word, imm16(0x1234)).lda(imm(0x42)).sta(byte);
    });
    EXPECT_EQ(result[word], 0x1234);
    EXPECT_EQ(result[byte], 0x42);
    EXPECT_THROW(result[data.byte(2)], std::invalid_argument);
    EXPECT_THROW(result[data], std::invalid_argument);
}

TEST_F(SystemTest, OwnedMemoryCaptureRegistrationRejectsForeignAndLateHandles) {
    auto test = system.program6502();
    auto other = system.program6502();
    const auto foreign = other.buffer("foreign", 2);
    EXPECT_THROW(test.capture(foreign), std::invalid_argument);
    const auto data = test.data("data", econopet::CpuAddress{0x400}, 2);
    test.capture(data);
    test.within(Budget).run([](auto&) {});
    EXPECT_THROW(test.capture(data), std::logic_error);
}

TEST_F(SystemTest, OwnedMemoryDataOnlyScenarioPreservesFixtureInitialization) {
    Scenario scenario(system);
    const auto data = scenario.reserve("data", RegionKind::Data, econopet::CpuAddress{0x400}, 2);
    auto view = scenario.lower_bank(data);
    const auto word = view.word("answer", 0, ByteOrder::LittleEndian);
    view.initialize(word, 0x1234);
    const auto code = scenario.reserve("code", RegionKind::Code, econopet::CpuAddress{0x300}, 0x100);
    scenario.reserve("vectors", RegionKind::Vectors, econopet::CpuAddress{0xfffc}, 2);
    const auto marker = scenario.completion(
        scenario.reserve("completion", RegionKind::Mailbox, econopet::CpuAddress{0x2ff}, 1));
    mos6502::Program program(scenario, code);
    program.lda(imm(0x56)).sta(word.byte(1));
    program.finish(marker);
    const auto result = scenario.run(program, data, Budget, "ordinary data capture");
    EXPECT_EQ(result[word], 0x5634);
}

TEST_F(SystemTest, OwnedMemoryScenarioCanRunWithoutCapturedResults) {
    Scenario scenario(system);
    const auto code = scenario.reserve("code", RegionKind::Code, econopet::CpuAddress{0x300}, 0x100);
    scenario.reserve("vectors", RegionKind::Vectors, econopet::CpuAddress{0xfffc}, 2);
    const auto marker = scenario.completion(
        scenario.reserve("completion", RegionKind::Mailbox, econopet::CpuAddress{0x2ff}, 1));
    mos6502::Program program(scenario, code);
    program.finish(marker);
    const auto result = scenario.run(program, Budget, "no captures");
    static_assert(std::is_same_v<std::remove_cv_t<decltype(result)>, ExecutionResult>);
    EXPECT_TRUE(result.raw_bytes().empty());
    EXPECT_TRUE(result.empty());
    EXPECT_EQ(result.completed(), system.time());
}

TEST_F(SystemTest, OwnedMemoryCompleteBuildersCanRunWithoutCapturedResults) {
    auto first = system.program6502();
    auto view = first.lower_bank();
    const auto unselected = view.byte("unselected");
    const auto result = first.within(Budget).run([](auto&) {});
    EXPECT_TRUE(result.empty());
    EXPECT_THROW(result[unselected], std::invalid_argument);
    auto second = system.program6809();
    const auto scratch = second.buffer("scratch", 2, Capture::No);
    const auto other = second.within(Budget).run([&](auto& p) {
        p.store(scratch.word(0, ByteOrder::BigEndian), imm16(0x1234));
    });
    EXPECT_TRUE(other.empty());
    EXPECT_THROW(other[scratch], std::invalid_argument);
}

TEST_F(SystemTest, OwnedMemoryViewsRespectObservationOnlyGuards) {
    auto test = system.program6502();
    const auto data = test.data("data", econopet::CpuAddress{0x400}, 2);
    auto view = test.lower_bank("data");
    const auto word = data.word(0, ByteOrder::BigEndian);
    view.initialize(word, 0x1234);
    const auto before = system.time();
    EXPECT_NO_THROW(system.run_until([&](const System&) { return view.read(word) == 0x1234; },
        0, "host view observation"));
    EXPECT_THROW(system.run_until([&](const System&) {
        view.initialize(word, 0x5678);
        return true;
    }, 0, "host view mutation"), std::logic_error);
    EXPECT_EQ(view.read(word), 0x1234);
    EXPECT_EQ(system.time(), before);
}

TEST_F(SystemTest, OwnedMemoryLastCpuAddressSupportsAByteButNotAWrappingWord) {
    Scenario scenario(system);
    const auto region = scenario.reserve("last byte", RegionKind::Data, econopet::CpuAddress{0xffff}, 1);
    auto view = scenario.lower_bank(region);
    const auto byte = view.byte("last");
    view.initialize(byte, 0x42);
    EXPECT_EQ(view.read(byte), 0x42);
    EXPECT_EQ(byte.address(), econopet::CpuAddress{0xffff});
    EXPECT_THROW(view.word("wrap", 0, ByteOrder::LittleEndian), std::out_of_range);
}

TEST_F(SystemTest, OwnedMemoryBufferStoreRejectsWrongLengthBeforeInstallation) {
    auto test = system.program6502();
    const auto data = test.buffer("data", 2);
    auto view = test.lower_bank();
    const auto original = view.read(data);
    const auto before = system.time();
    const uint8_t short_data[] = {1};
    EXPECT_THROW(test.within(Budget).run([&](auto& p) { p.store(data, short_data); }), std::out_of_range);
    EXPECT_EQ(view.read(data), original);
    EXPECT_EQ(system.time(), before);
}

TEST_F(SystemTest, OwnedMemoryCaptureSurvivesBuilderAndBoardDestruction) {
    auto detached = [] {
        System board;
        auto test = board.program6502();
        const auto word = test.word("answer");
        const auto result = test.within(Budget).run([&](auto& p) { p.store(word, imm16(0x1234)); });
        return std::make_pair(word, result);
    }();
    EXPECT_EQ(detached.second[detached.first], 0x1234);
}

// Union capture selections without exposing holes or depending on registration order.
TEST_F(SystemTest, OwnedMemoryCaptureUnionsAdjacentOverlappingAndDuplicateSelections) {
    auto test = system.program6502();
    const auto data = test.data("data", econopet::CpuAddress{0x400}, 8);
    test.capture(data.slice(2, 2));
    test.capture(data.slice(0, 2));
    test.capture(data.slice(1, 2));
    test.capture(data.slice(0, 2));
    test.capture(data.slice(6, 2));
    const std::array<uint8_t, 8> initial{1, 2, 3, 4, 5, 6, 7, 8};
    test.lower_bank("data").initialize(data, initial);
    auto result = test.within(Budget).run([](auto&) {});
    EXPECT_EQ(result[data.slice(0, 4)], (std::vector<uint8_t>{1, 2, 3, 4}));
    EXPECT_EQ(result[data.word(1, ByteOrder::LittleEndian)], 0x0302);
    EXPECT_EQ(result[data.slice(6, 2)], (std::vector<uint8_t>{7, 8}));
    EXPECT_THROW(result[data.word(3, ByteOrder::LittleEndian)], std::invalid_argument);
    EXPECT_THROW(result[data], std::invalid_argument);
    EXPECT_THROW(result.raw_bytes(), std::logic_error);
    EXPECT_THROW(std::move(std::as_const(result)).raw_bytes(), std::logic_error);
    EXPECT_THROW(std::move(result).raw_bytes(), std::logic_error);
    EXPECT_FALSE(result.empty());
    auto moved = std::move(result);
    EXPECT_THROW(result[data.byte(0)], std::logic_error);
    EXPECT_THROW(result.empty(), std::logic_error);
    EXPECT_EQ(moved[data.word(1, ByteOrder::BigEndian)], 0x0203);
}

// Contiguous selected results preserve the ordinary capture's borrow, copy and transfer rules.
TEST_F(SystemTest, OwnedMemoryUnifiedResultPreservesContiguousRawBytes) {
    auto test = system.program6502();
    const auto answer = test.word("answer");
    auto result = test.within(Budget).run([&](auto& p) { p.store(answer, imm16(0x1234)); });
    static_assert(std::is_same_v<decltype(result), ExecutionResult>);
    const auto& borrowed = result.raw_bytes();
    EXPECT_EQ(borrowed, (std::vector<uint8_t>{0x34, 0x12}));
    const auto copied = std::move(std::as_const(result)).raw_bytes();
    EXPECT_EQ(copied, borrowed);
    EXPECT_NE(copied.data(), borrowed.data());
    const auto transferred = std::move(result).raw_bytes();
    EXPECT_EQ(transferred, copied);
    EXPECT_THROW(result[answer], std::logic_error);
    EXPECT_THROW(result.empty(), std::logic_error);

    auto empty = system.program6809().within(Budget).run([](auto&) {});
    EXPECT_TRUE(empty.empty());
    EXPECT_TRUE(empty.raw_bytes().empty());
    EXPECT_TRUE(std::move(empty).raw_bytes().empty());
    EXPECT_THROW(empty.raw_bytes(), std::logic_error);
}

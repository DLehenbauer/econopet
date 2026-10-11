// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <array>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "framework/observations.h"
#include "framework/system_test.h"

namespace {
using namespace test_observation;
using namespace econopet;
using namespace econopet::io;
struct TraceSentinel {};

// Measure copies made by the public recorder capture path, not merely its selection helper.
struct CountedEvent {
    std::shared_ptr<size_t> copies;
    explicit CountedEvent(std::shared_ptr<size_t> counter) : copies(std::move(counter)) {}
    CountedEvent(const CountedEvent& other) : copies(other.copies) { ++*copies; }
    CountedEvent(CountedEvent&&) noexcept = default;
};

// Advance an explicit board-only interval without depending on configured execution.
TraceWindow run_window(System& board, Cycles duration) {
    const auto start = board.time();
    board.tick(duration);
    return board.trace_window(start, board.time());
}

static_assert(!std::is_reference_v<decltype(std::declval<ScopedRecorder<uint8_t>&&>().records())>);
static_assert(!std::is_reference_v<decltype(std::declval<const ScopedRecorder<uint8_t>&&>().times())>);
static_assert(!std::is_reference_v<decltype(std::declval<EventTrace<uint8_t>&&>().records())>);

template<class Board>
concept TemporaryTraceFactory = requires(Board board) { std::move(board).trace_av(); };
static_assert(!TemporaryTraceFactory<System>);
}

TEST_F(SystemTest, TraceAvSamplesInitialLevelsAndEachCompletedCycleWithStereoAudio) {
    auto trace = system.trace_av(AvSampling::EveryCycle);
    const auto initial = system.snapshot();
    ASSERT_EQ(trace.records().size(), 1u);
    EXPECT_EQ(trace.records()[0].at, system.time());
    EXPECT_EQ(trace.records()[0].levels.video, initial.video_o);
    EXPECT_EQ(trace.records()[0].levels.horizontal, initial.horiz_drive_o);
    EXPECT_EQ(trace.records()[0].levels.vertical, initial.vert_drive_o);
    EXPECT_EQ(trace.records()[0].levels.jiffy, initial.jiffy_clock_o);
    EXPECT_EQ(trace.records()[0].levels.audio_left, initial.audio_l_o);
    EXPECT_EQ(trace.records()[0].levels.audio_right, initial.audio_r_o);
    const auto window = run_window(system, Cycles{3});
    trace.capture(window).require_count(3, "completed cycle samples exclude initial state");
    EXPECT_EQ(trace.records().size(), 4u);
    EXPECT_EQ(trace.times().back(), window.completed());
    const auto final = system.snapshot();
    EXPECT_EQ(trace.records().back().levels.audio_left, final.audio_l_o);
    EXPECT_EQ(trace.records().back().levels.audio_right, final.audio_r_o);
    EXPECT_THROW(system.trace_av(static_cast<AvSampling>(99)), std::invalid_argument);
}

TEST_F(SystemTest, TraceRecorderSmallWindowCopiesOnlyRequestedValues) {
    const auto copies = std::make_shared<size_t>(0);
    auto trace = system.trace_events<CountedEvent>([copies](const System&) {
        return std::optional<CountedEvent>{CountedEvent{copies}};
    });
    system.tick(998);
    const auto start = system.time();
    system.tick(2);
    const auto window = system.trace_window(start, system.time());
    *copies = 0;
    const auto selected = trace.capture(window);
    EXPECT_EQ(*copies, 2u);
    selected.require_count(2, "last two records from long history");
    *copies = 0;
    System other;
    EXPECT_THROW(trace.capture(other.trace_window(other.time(), other.time())), std::invalid_argument);
    EXPECT_EQ(*copies, 0u);
}

TEST_F(SystemTest, TraceAvChangesOnlyMatchesEveryCycleDigitalChanges) {
    auto all = system.trace_av(AvSampling::EveryCycle);
    auto changes = system.trace_av();
    system.tick(2000);
    ASSERT_GT(all.records().size(), 1u);
    std::vector<AvRecord> expected{all.records().front()};
    for (const auto& record : all.records())
        if (!(record.levels == expected.back().levels)) expected.push_back(record);
    ASSERT_GT(expected.size(), 1u);
    ASSERT_EQ(changes.records().size(), expected.size());
    for (size_t index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(changes.records()[index].at, expected[index].at);
        EXPECT_EQ(changes.records()[index].levels, expected[index].levels);
    }
}

TEST(TraceBoard, AvBundleComparisonIncludesBothAudioChannels) {
    const AvLevels initial{false, false, false, false, false, false};
    for (const auto member : {&AvLevels::video, &AvLevels::horizontal, &AvLevels::vertical,
            &AvLevels::jiffy, &AvLevels::audio_left, &AvLevels::audio_right}) {
        auto changed = initial;
        changed.*member = true;
        EXPECT_FALSE(changed == initial);
    }
    EXPECT_TRUE(initial == initial);
}

TEST_F(SystemTest, TraceRvalueAccessCopiesActiveStorageAndMoveStopRemainIndependent) {
    auto first = system.trace_av(AvSampling::EveryCycle);
    auto other = system.trace_av(AvSampling::EveryCycle);
    system.tick(2);
    const auto& records = std::move(first).records();
    const auto& times = std::move(std::as_const(first)).times();
    EXPECT_EQ(records.size(), 3u);
    EXPECT_EQ(times.size(), 3u);
    auto moved = std::move(first);
    EXPECT_THROW(first.records(), std::logic_error);
    system.tick(1);
    EXPECT_EQ(moved.records().size(), 4u);
    EXPECT_EQ(records.size(), 3u);
    moved.stop();
    moved.stop();
    system.tick(2);
    EXPECT_EQ(moved.records().size(), 4u);
    EXPECT_EQ(other.records().size(), 6u);
    other = std::move(moved);
    EXPECT_THROW(moved.capture(), std::logic_error);
    system.tick(1);
    EXPECT_EQ(other.records().size(), 4u);
}

TEST_F(SystemTest, TraceSignalsRecordTransitionsWithoutInventingInitialEdges) {
    auto irq = system.trace_signal(Signal::Pia1Irq);
    auto reset = system.trace_signal(Signal::CpuReset);
    const auto start = system.time();
    system.cpu().release_reset();
    reset.capture(system.trace_window(start, system.time())).require_count(1, "reset release");
    EXPECT_TRUE(reset.records().front().before);
    EXPECT_FALSE(reset.records().front().after);
    system.set_external_reset(true);
    system.tick(1);
    EXPECT_EQ(reset.records().size(), 2u);
    EXPECT_FALSE(reset.records().back().before);
    EXPECT_TRUE(reset.records().back().after);
    irq.capture().require_absent("no fitted PIA IRQ");
    EXPECT_THROW(system.trace_signal(static_cast<Signal>(999)), std::invalid_argument);
    for (const auto signal : {Signal::CpuIrq, Signal::CpuNmi, Signal::CpuReady, Signal::Video,
            Signal::Horizontal, Signal::Vertical, Signal::Jiffy, Signal::AudioLeft, Signal::AudioRight,
            Signal::Pia2Irq, Signal::ViaIrq})
        EXPECT_NO_THROW(system.trace_signal(signal));
}

TEST_F(SystemTest, TraceModelWritesSurviveClearingAndAreNotPhysicalBusWrites) {
    auto pia = system.trace_pia1_writes();
    auto another = system.trace_pia1_writes();
    auto pia2 = system.trace_pia2_writes();
    auto via = system.trace_via_writes();
    auto bus = system.trace_peripheral_writes();
    const auto start = system.time();
    system.tick(1);
    system.io().pia1().set_control(PiaRegister::ControlA, PiaC2Low, system.time());
    system.io().clear_observations();
    system.io().pia1().set_control(PiaRegister::ControlA, PiaC2High, system.time());
    system.io().pia2().set_control(PiaRegister::ControlB, PiaPortAccess, system.time());
    system.io().via().set_acr({}, system.time());
    system.io().clear_observations();
    system.tick(1);
    const auto window = system.trace_window(start, system.time());
    pia.capture(window).require_count(2, "same-cycle direct writes");
    another.capture(window).require_count(2, "independent source tap");
    pia2.capture(window).require_count(1, "PIA2 isolation");
    via.capture(window).require_count(1, "VIA isolation");
    EXPECT_EQ(pia.times()[0], pia.times()[1]);
    EXPECT_EQ(system.io().pia1().writes().count(), 0u);
    bus.capture(window).require_absent("host setup is not a physical bus access");
}

TEST_F(SystemTest, TracePhysicalBusWritesKeepRepeatedValuesAndDispatchOrder) {
    auto bus = system.trace_peripheral_writes();
    auto pia = system.trace_pia2_writes();
    auto via = system.trace_via_writes();
    // Execute raw opcodes through the existing CPU lifecycle, without an assembler layer.
    constexpr CpuAddress entry{0x0200};
    constexpr std::array<uint8_t, 14> program{
        0xa9, 0x42, 0x8d, 0x20, 0xe8, 0x8d, 0x20, 0xe8, 0x8d, 0x40, 0xe8, 0x4c, 0x0b, 0x02};
    for (size_t index = 0; index < program.size(); ++index)
        system.poke(SramAddress{entry.value() + index}, program[index]);
    const auto start = system.time();
    system.cpu(CPU_SOFT_6502).start(entry);
    system.run_until([&](const System&) { return bus.records().size() == 3; }, 20000, "three physical writes");
    const auto capture = bus.capture(system.trace_window(start, system.time()));
    capture.require_count(3, "physical writes");
    pia.capture(capture.window()).require_count(2, "repeated PIA writes");
    via.capture(capture.window()).require_count(1, "VIA write");
    EXPECT_EQ(capture.records()[0].address, pet::Pia2PortA);
    EXPECT_EQ(capture.records()[1].address, pet::Pia2PortA);
    EXPECT_EQ(capture.records()[2].address, pet::ViaPortB);
    for (const auto& record : capture.records()) EXPECT_EQ(record.data, 0x42);
    EXPECT_LT(capture.times()[0], capture.times()[1]);
    EXPECT_LT(capture.times()[1], capture.times()[2]);
}

TEST_F(SystemTest, TracePhysicalWritePublicationFailureCannotProveAbsenceAfterRecovery) {
    constexpr CpuAddress entry{0x0200};
    constexpr std::array<uint8_t, 8> program{0xa9, 0x42, 0x8d, 0x20, 0xe8, 0x4c, 0x05, 0x02};
    for (size_t index = 0; index < program.size(); ++index)
        system.poke(SramAddress{entry.value() + index}, program[index]);
    system.cpu(CPU_SOFT_6502).start(entry);
    auto bus = system.trace_peripheral_writes();
    const auto start = system.time();
    // Force model publication to fail after its physical chip write has committed.
    const auto future = system.time() + Cycles{20001};
    auto injected = std::make_shared<TraceState<WriteRecord<PiaRegister>>>(
        TraceState<WriteRecord<PiaRegister>>{system.observation_source(), future, future, {}, {}});
    system.io().pia2().writes().trace(injected);
    EXPECT_THROW(system.run_until([](const System&) { return false; }, 20000, "injected model publication"),
        std::logic_error);
    ASSERT_EQ(system.io().pia2().writes().count(), 1u);
    EXPECT_TRUE(system.clock_faulted());
    EXPECT_FALSE(injected->complete);
    injected->active = false;
    system.drive_physical_cpu({system.snapshot().cpu_addr_o, std::nullopt});
    system.set_external_reset(true);
    EXPECT_NO_THROW(system.tick(1));
    EXPECT_FALSE(system.clock_faulted());
    EXPECT_EQ(system.io().pia2().writes().count(), 1u);
    EXPECT_TRUE(bus.records().empty());
    EXPECT_FALSE(bus.capture().complete());
    EXPECT_THROW(bus.capture().require_absent("committed write before failed publication"), std::logic_error);
    const auto recovered = bus.capture(system.trace_window(start, system.time()));
    EXPECT_FALSE(recovered.complete());
    EXPECT_THROW(recovered.require_absent("recovery window includes missed publication"), std::logic_error);
}

TEST(TraceBoard, DetachedCaptureAndRecorderMayOutliveBoard) {
    std::optional<ScopedRecorder<SignalTransition>> recorder;
    std::optional<TraceCapture<SignalTransition>> detached;
    {
        System board;
        recorder.emplace(board.trace_signal(Signal::CpuReset));
        detached.emplace(recorder->capture(run_window(board, Cycles{3})));
    }
    detached->require_absent("destroyed source");
    recorder->capture().require_absent("retained recorder");
    recorder.reset();
    detached->require_absent("destroyed recorder");
}

TEST_F(SystemTest, TraceWindowsRejectForeignLateFutureReversedAndStoppedIntervals) {
    auto trace = system.trace_signal(Signal::AudioLeft);
    System other;
    EXPECT_THROW(trace.capture(run_window(other, Cycles{2})), std::invalid_argument);
    const auto window = run_window(system, Cycles{2});
    auto late = system.trace_signal(Signal::AudioLeft);
    EXPECT_THROW(late.capture(window), std::out_of_range);
    EXPECT_THROW(system.trace_window(system.time(), system.time() + Cycles{1}), std::out_of_range);
    EXPECT_THROW(system.trace_window(system.time(), window.started()), std::invalid_argument);
    trace.stop();
    EXPECT_THROW(trace.capture(run_window(system, Cycles{2})), std::out_of_range);
    trace.capture(window).require_absent("covered past interval");
    late.capture(system.trace_window(system.time(), system.time())).require_absent("empty covered interval");
}

TEST_F(SystemTest, TraceDetectorFailuresInvalidateAbsenceWithoutReplacingPayload) {
    auto trace = system.trace_events<unsigned>([](const System&) -> std::optional<unsigned> {
        throw TraceSentinel{};
    });
    EXPECT_THROW(system.tick(1), TraceSentinel);
    EXPECT_FALSE(system.clock_faulted());
    EXPECT_FALSE(trace.capture().complete());
    EXPECT_THROW(trace.capture().require_absent("failed detector"), std::logic_error);
}

TEST_F(SystemTest, TraceSkippedObserverBoundaryCannotClaimCompleteCoverage) {
    auto failing = system.observe([](const System&) { throw TraceSentinel{}; });
    auto trace = system.trace_signal(Signal::AudioLeft);
    const auto start = system.time();
    EXPECT_THROW(system.tick(1), TraceSentinel);
    EXPECT_THROW(trace.capture(system.trace_window(start, system.time())), std::out_of_range);
    failing.reset();
    EXPECT_THROW(system.tick(1), std::logic_error);
    EXPECT_FALSE(trace.capture().complete());
    EXPECT_THROW(trace.capture().require_count(0, "missed observer"), std::logic_error);
}

TEST_F(SystemTest, TraceCustomDetectorsAreObservationOnlyAndIncludeEndpoint) {
    const auto end = system.time() + Cycles{3};
    auto trace = system.trace_events<unsigned>([end](const System& board) -> std::optional<unsigned> {
        if (board.time() == end) return 42;
        return std::nullopt;
    });
    trace.capture(run_window(system, Cycles{3})).require_count(1, "endpoint event");
    EXPECT_EQ(trace.records().front(), 42u);
    EXPECT_EQ(trace.times().front(), end);
    auto invalid = system.trace_events<unsigned>([&](const System&) -> std::optional<unsigned> {
        system.tick(1);
        return std::nullopt;
    });
    EXPECT_THROW(system.tick(1), std::logic_error);
    EXPECT_FALSE(invalid.capture().complete());
}

TEST_F(SystemTest, TraceFactoriesRespectObservationAndInputEditGuards) {
    const auto check = [&] {
        EXPECT_THROW(system.trace_av(), std::logic_error);
        EXPECT_THROW(system.trace_signal(Signal::CpuReset), std::logic_error);
        EXPECT_THROW(system.trace_pia1_writes(), std::logic_error);
        EXPECT_THROW(system.trace_peripheral_writes(), std::logic_error);
        EXPECT_THROW(system.trace_events<unsigned>([](const System&) { return std::optional<unsigned>{}; }),
            std::logic_error);
    };
    auto subscription = system.observe([&](const System&) { check(); });
    system.tick(1);
    subscription.reset();
    system.raw_stimulus([&](auto&) { check(); });
    system.io().via().inputs([&](auto&) { check(); });
}

TEST(TraceBoard, ManualEventsOwnRecordsAndRejectBackwardsTimestamps) {
    EventTrace<uint8_t> trace;
    const CycleTime start{Cycles{0}}, at{Cycles{1}}, end{Cycles{2}};
    trace.record(at, 0x42);
    trace.record(at, 0x42);
    trace.record(end, 0x73);
    EXPECT_THROW(trace.record(start, 0x10), std::invalid_argument);
    const auto immutable = trace;
    const auto& copied = std::move(immutable).records();
    EXPECT_EQ(copied.size(), 3u);
    EXPECT_NE(copied.data(), immutable.records().data());
    const auto& records = std::move(trace).records();
    EXPECT_EQ(records.size(), 3u);
    EXPECT_EQ(records[0].at, at);
    EXPECT_EQ(records[1].at, at);
    EXPECT_EQ(records[2].at, end);
    EXPECT_EQ(records[0].event, 0x42);
    EXPECT_EQ(records[1].event, 0x42);
    EXPECT_EQ(records[2].event, 0x73);
}

TEST_F(SystemTest, TraceStabilityChecksIncludeInitialStateAndEndpoint) {
    const auto start = system.time();
    require_stable(system, [](const System& board) { return board.cpu().peek_reset(); }, Cycles{3}, "reset held");
    EXPECT_EQ(system.time(), start + Cycles{3});
    require_absent(system, [](const System& board) { return !board.cpu().peek_reset(); }, Cycles{0}, "empty interval");
    EXPECT_THROW(require_stable(system, [](const System&) { return false; }, Cycles{1}, "initial failure"),
        std::runtime_error);
    const auto end = system.time() + Cycles{2};
    EXPECT_THROW(require_stable(system, [end](const System& board) { return board.time() < end; },
        Cycles{2}, "endpoint failure"), std::runtime_error);
    EXPECT_EQ(system.time(), end);
}

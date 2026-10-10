// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "framework/trace.h"
#include "io.h"

namespace {
using namespace test_observation;
using namespace econopet;

// Construct standalone storage with a distinct identity and zero-length initial coverage.
template<class Record = uint8_t>
std::shared_ptr<TraceState<Record>> storage() {
    const CycleTime start{Cycles{0}};
    return std::make_shared<TraceState<Record>>(TraceState<Record>{
        std::make_shared<const int>(0), start, start, {}, {}});
}

static_assert(std::is_reference_v<decltype(std::declval<const TraceCapture<uint8_t>&>().records())>);
static_assert(!std::is_reference_v<decltype(std::declval<TraceCapture<uint8_t>&&>().records())>);
static_assert(!std::is_reference_v<decltype(std::declval<const TraceCapture<uint8_t>&&>().records())>);
static_assert(!std::is_reference_v<decltype(std::declval<TraceCapture<uint8_t>&&>().times())>);
static_assert(!std::is_reference_v<decltype(std::declval<const TraceCapture<uint8_t>&&>().times())>);
static_assert(!std::is_reference_v<decltype(std::declval<TraceCapture<uint8_t>&&>().window())>);
static_assert(!std::is_reference_v<decltype(std::declval<const TraceCapture<uint8_t>&&>().window())>);
static_assert(!std::is_reference_v<decltype(std::declval<TraceWindow&&>().source())>);

// Count value copies so window selection cost is measured independently of elapsed time.
struct CountedRecord {
    std::shared_ptr<size_t> copies;
    unsigned value;
    CountedRecord(std::shared_ptr<size_t> counter, unsigned data) : copies(std::move(counter)), value(data) {}
    CountedRecord(const CountedRecord& other) : copies(other.copies), value(other.value) { ++*copies; }
    CountedRecord(CountedRecord&&) noexcept = default;
};
}

TEST(TraceCore, WindowSelectionCopiesOnlySelectedRecordsAndValidatesBeforeCopying) {
    auto state = storage<CountedRecord>();
    const auto copies = std::make_shared<size_t>(0);
    constexpr unsigned HistoryLength = 1000;
    for (unsigned index = 1; index <= HistoryLength; ++index) {
        const CycleTime at{Cycles{index}};
        state->append(at, CountedRecord{copies, index});
        if (index == HistoryLength) state->append(at, CountedRecord{copies, index});
        state->cover(at);
    }
    const auto full = state->capture();
    const TraceWindow tail{state->source, CycleTime{Cycles{HistoryLength - 2}}, state->completed};
    *copies = 0;
    const auto live = state->capture(tail);
    EXPECT_EQ(*copies, 3u);
    *copies = 0;
    const auto detached = full.capture(tail);
    EXPECT_EQ(*copies, 3u);
    ASSERT_EQ(live.records().size(), 3u);
    EXPECT_EQ(live.times(), detached.times());
    for (size_t index = 0; index < live.records().size(); ++index)
        EXPECT_EQ(live.records()[index].value, detached.records()[index].value);
    *copies = 0;
    const TraceWindow empty{state->source, state->completed, state->completed};
    state->capture(empty).require_absent("empty live selection");
    full.capture(empty).require_absent("empty detached selection");
    const TraceWindow foreign{std::make_shared<const int>(0), state->started, state->completed};
    EXPECT_THROW(state->capture(foreign), std::invalid_argument);
    EXPECT_THROW(full.capture(foreign), std::invalid_argument);
    const TraceWindow future{state->source, state->started, state->completed + Cycles{1}};
    EXPECT_THROW(state->capture(future), std::out_of_range);
    EXPECT_THROW(full.capture(future), std::out_of_range);
    EXPECT_EQ(*copies, 0u);
    state->complete = false;
    EXPECT_FALSE(state->capture(tail).complete());
    EXPECT_THROW(state->capture(tail).require_absent("incomplete direct selection"), std::logic_error);
}

TEST(TraceCore, WindowsRequireIdentityAndOrderedBoundaries) {
    const auto state = storage();
    EXPECT_THROW(TraceWindow({}, state->started, state->completed), std::invalid_argument);
    EXPECT_THROW(TraceWindow(state->source, CycleTime{Cycles{1}}, state->started), std::invalid_argument);
    EXPECT_NO_THROW(TraceWindow(state->source, state->started, state->started));
}

TEST(TraceCore, CapturesExcludeStartIncludeEndAndRetainSameCycleRepeats) {
    auto state = storage();
    state->append(state->started, 0x10);
    const auto middle = state->started + Cycles{1};
    const auto end = middle + Cycles{1};
    state->append(middle, 0x42);
    state->append(middle, 0x42);
    state->cover(middle);
    state->append(end, 0x73);
    state->cover(end);
    const auto capture = state->capture().capture(TraceWindow(state->source, state->started, end));
    EXPECT_EQ(capture.records(), (std::vector<uint8_t>{0x42, 0x42, 0x73}));
    EXPECT_EQ(capture.times(), (std::vector<CycleTime>{middle, middle, end}));
    capture.require_count(3, "all repeated events");
    capture.require_count([](uint8_t value) { return value == 0x42; }, 2, "repeated marker");
    capture.require_order(std::vector<uint8_t>{0x42, 0x42, 0x73}, [](uint8_t value) { return value; }, "wire order");
    EXPECT_THROW(capture.require_count(2, "wrong count"), std::runtime_error);
    EXPECT_THROW(capture.require_count([](uint8_t) { return true; }, 4, "wrong matching count"), std::runtime_error);
    EXPECT_THROW(capture.require_order(std::vector<uint8_t>{0x73}, [](uint8_t value) { return value; },
        "wrong order"), std::runtime_error);
    state->capture().capture(TraceWindow(state->source, end, end)).require_absent("empty interval");
    const auto endpoint = capture.capture(TraceWindow(state->source, middle, end));
    EXPECT_EQ(endpoint.records(), (std::vector<uint8_t>{0x73}));
}

TEST(TraceCore, QuietCoverageRejectsForeignEarlyFutureAndIncompleteWindows) {
    auto state = storage();
    state->cover(CycleTime{Cycles{1}});
    const auto capture = state->capture();
    capture.require_absent("covered quiet interval");
    EXPECT_THROW(capture.capture(TraceWindow(std::make_shared<const int>(0), state->started, state->completed)),
        std::invalid_argument);
    EXPECT_THROW(capture.capture(TraceWindow(state->source, state->started, state->completed + Cycles{1})),
        std::out_of_range);
    const TraceState<uint8_t> late{state->source, state->completed, state->completed, {}, {}};
    EXPECT_THROW(late.capture().capture(capture.window()), std::out_of_range);
    EXPECT_THROW(state->cover(state->completed + Cycles{2}), std::logic_error);
    EXPECT_FALSE(state->capture().complete());
    EXPECT_THROW(state->capture().require_absent("missed boundary"), std::logic_error);
    EXPECT_THROW(state->capture().require_order(std::vector<uint8_t>{}, [](uint8_t value) { return value; },
        "incomplete order"), std::logic_error);
}

TEST(TraceCore, TimestampFailuresInvalidateEvidenceAndUncoveredEventsCannotProveAbsence) {
    auto state = storage();
    state->cover(CycleTime{Cycles{1}});
    EXPECT_THROW(state->append(state->started, 0x42), std::invalid_argument);
    EXPECT_TRUE(state->records.empty());
    EXPECT_FALSE(state->capture().complete());
    auto pending = storage();
    pending->append(CycleTime{Cycles{1}}, 0x73);
    EXPECT_FALSE(pending->capture().complete());
    pending->cover(CycleTime{Cycles{1}});
    EXPECT_TRUE(pending->capture().complete());
}

TEST(TraceCore, TemporaryCaptureAccessOwnsRecordsTimesWindowAndIdentity) {
    auto state = storage();
    state->cover(CycleTime{Cycles{1}});
    state->append(state->completed, 0x42);
    const auto& records = state->capture().records();
    const auto& times = state->capture().times();
    const auto& window = state->capture().window();
    const auto& identity = TraceWindow(state->source, state->started, state->completed).source();
    const auto capture = state->capture();
    const auto& copied = std::move(capture).records();
    const auto& copied_times = std::move(capture).times();
    const auto& copied_window = std::move(capture).window();
    EXPECT_NE(copied.data(), capture.records().data());
    EXPECT_EQ(copied_times, capture.times());
    state.reset();
    EXPECT_EQ(records, (std::vector<uint8_t>{0x42}));
    EXPECT_EQ(times, (std::vector<CycleTime>{CycleTime{Cycles{1}}}));
    EXPECT_EQ(window.source(), identity);
    EXPECT_EQ(copied_window.source(), identity);
    EXPECT_NO_THROW(capture.require_count(1, "destroyed source storage"));
}

TEST(TraceCore, SourcesSupportIndependentSubscribersStopAndExpiredStorage) {
    TraceSource<uint8_t> source;
    auto first = storage();
    auto second = storage();
    source.subscribe(first);
    source.subscribe(second);
    auto expired = storage();
    source.subscribe(expired);
    expired.reset();
    source.emit(CycleTime{Cycles{1}}, 0x42);
    first->cover(CycleTime{Cycles{1}});
    second->cover(CycleTime{Cycles{1}});
    first->active = false;
    source.emit(CycleTime{Cycles{2}}, 0x73);
    second->cover(CycleTime{Cycles{2}});
    EXPECT_EQ(first->records, (std::vector<uint8_t>{0x42}));
    EXPECT_EQ(second->records, (std::vector<uint8_t>{0x42, 0x73}));
    source.stop();
    source.stop();
    source.emit(CycleTime{Cycles{3}}, 0x10);
    EXPECT_EQ(second->records.size(), 2u);
    EXPECT_FALSE(second->active);
}

TEST(TraceCore, TransferredCapturesCannotClaimCompleteQuietCoverage) {
    auto state = storage();
    state->cover(CycleTime{Cycles{1}});
    state->append(state->completed, 0x42);
    auto original = state->capture();
    auto moved = std::move(original);
    moved.require_count(1, "moved evidence");
    EXPECT_FALSE(original.complete());
    EXPECT_THROW(original.require_absent("moved-from"), std::logic_error);
    EXPECT_THROW(original.capture(moved.window()), std::logic_error);
    original = std::move(moved);
    original.require_count(1, "move-assigned evidence");
    EXPECT_FALSE(moved.complete());
    const auto records = std::move(original).records();
    EXPECT_EQ(records, (std::vector<uint8_t>{0x42}));
    EXPECT_FALSE(original.complete());
    EXPECT_THROW(original.require_absent("transferred records"), std::logic_error);
    EXPECT_THROW(original.capture(state->capture().window()), std::logic_error);
    auto timestamps = state->capture();
    const auto times = std::move(timestamps).times();
    EXPECT_EQ(times.size(), 1u);
    EXPECT_THROW(timestamps.require_count(1, "transferred timestamps"), std::logic_error);
    auto interval = state->capture();
    const auto window = std::move(interval).window();
    EXPECT_EQ(window.source(), state->source);
    EXPECT_THROW(interval.require_count(1, "transferred interval"), std::logic_error);
}

TEST(TraceCore, SourceFailureInvalidatesEverySubscriberWithoutReplacingException) {
    TraceSource<uint8_t> source;
    auto first = storage();
    auto second = storage();
    first->cover(CycleTime{Cycles{1}});
    source.subscribe(first);
    source.subscribe(second);
    EXPECT_THROW(source.emit(CycleTime{Cycles{0}}, 0x42), std::invalid_argument);
    EXPECT_FALSE(first->complete);
    EXPECT_FALSE(second->complete);
    EXPECT_THROW(second->capture().require_absent("delivery interrupted"), std::logic_error);
}

TEST(TraceCore, ModelWritesRetainSameCycleEventsAcrossClearAndReset) {
    using namespace econopet::io;
    Pia6520 pia;
    Via6522 via;
    auto writes = storage<WriteRecord<PiaRegister>>();
    auto via_writes = storage<WriteRecord<ViaRegister>>();
    pia.writes().trace(writes);
    via.writes().trace(via_writes);
    const CycleTime at{Cycles{1}};
    pia.set_control(PiaRegister::ControlA, PiaC2Low, at);
    pia.clear_observations();
    pia.reset();
    pia.set_control(PiaRegister::ControlA, PiaC2High, at);
    via.set_acr({}, at);
    via.clear_observations();
    via.reset();
    writes->cover(at);
    via_writes->cover(at);
    writes->capture().require_count(2, "model writes survive clearing/reset");
    via_writes->capture().require_count(1, "VIA source");
    EXPECT_EQ(writes->times[0], writes->times[1]);
    EXPECT_EQ(writes->records[0].data, PiaC2Low.bits());
    EXPECT_EQ(writes->records[1].data, PiaC2High.bits());
}

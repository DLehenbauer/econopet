// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "types.h"

namespace test_observation {

// A source-bound (start, end] interval, independent of the source's lifetime.
class TraceWindow {
public:
    // Retain identity without retaining the board or any mutable simulator state.
    TraceWindow(std::shared_ptr<const int> source, econopet::CycleTime start, econopet::CycleTime end)
        : source_(std::move(source)), start_(start), end_(end) {
        if (!source_) throw std::invalid_argument("trace window: missing source identity");
        if (end < start) throw std::invalid_argument("trace window: reversed interval");
    }
    // Return interval boundaries without accessing a live source.
    econopet::CycleTime started() const { return start_; }
    econopet::CycleTime completed() const { return end_; }
    // Compare identities, not timestamps from unrelated simulation contexts.
    const std::shared_ptr<const int>& source() const & { return source_; }
    // Retain identity independently of a temporary window.
    std::shared_ptr<const int> source() const && { return source_; }
private:
    std::shared_ptr<const int> source_;
    econopet::CycleTime start_, end_;
};

template<class Record> struct TraceState;

// Detached immutable values with explicit capture coverage.
template<class Record>
class TraceCapture {
public:
    TraceCapture(const TraceCapture&) = default;
    TraceCapture& operator=(const TraceCapture&) = default;
    // Transfer detached evidence without leaving a complete, empty moved-from capture.
    TraceCapture(TraceCapture&& other) noexcept
        : window_(std::move(other.window_)), records_(std::move(other.records_)),
          times_(std::move(other.times_)), complete_(std::exchange(other.complete_, false)) {}
    // Invalidate the previous source's coverage when replacing detached evidence.
    TraceCapture& operator=(TraceCapture&& other) noexcept {
        if (this != &other) {
            window_ = std::move(other.window_);
            records_ = std::move(other.records_);
            times_ = std::move(other.times_);
            complete_ = std::exchange(other.complete_, false);
        }
        return *this;
    }
    // Inspect records and corresponding timestamps after source destruction.
    const std::vector<Record>& records() const & { return records_; }
    // Transfer records out of a temporary capture.
    std::vector<Record> records() && {
        complete_ = false;
        return std::move(records_);
    }
    // Copy records out of an immutable temporary capture.
    std::vector<Record> records() const && { return records_; }
    // Borrow captured timestamps while the capture remains alive.
    const std::vector<econopet::CycleTime>& times() const & { return times_; }
    // Transfer timestamps out of a temporary capture.
    std::vector<econopet::CycleTime> times() && {
        complete_ = false;
        return std::move(times_);
    }
    // Copy timestamps out of an immutable temporary capture.
    std::vector<econopet::CycleTime> times() const && { return times_; }
    // Borrow the capture's interval while its owner remains alive.
    const TraceWindow& window() const & { return window_; }
    // Transfer a temporary capture's independently owned interval.
    TraceWindow window() && {
        complete_ = false;
        return std::move(window_);
    }
    // Copy an immutable temporary capture's interval.
    TraceWindow window() const && { return window_; }
    // Report whether every source observation boundary in the window was captured.
    bool complete() const { return complete_; }
    // Require absence only when the entire requested interval was observed.
    void require_absent(const std::string& reason) const { require_count(0, reason); }
    // Require an exact total count, including repeated records.
    void require_count(size_t expected, const std::string& reason) const {
        require_complete(reason);
        if (records_.size() != expected)
            throw std::runtime_error(diagnostic(reason + ": expected " + std::to_string(expected)
                + " records, observed " + std::to_string(records_.size())));
    }
    // Count matching source events without discarding coverage of their interval.
    template<class Predicate>
    void require_count(Predicate matches, size_t expected, const std::string& reason) const {
        require_complete(reason);
        const auto count = static_cast<size_t>(std::count_if(records_.begin(), records_.end(), matches));
        if (count != expected)
            throw std::runtime_error(diagnostic(reason + ": expected " + std::to_string(expected)
                + " matching records, observed " + std::to_string(count)));
    }
    // Compare source values in order while leaving timestamp selection to the window.
    template<class Event, class Projection>
    void require_order(const std::vector<Event>& expected, Projection value, const std::string& reason) const {
        require_complete(reason);
        for (size_t i = 0; i < std::max(expected.size(), records_.size()); ++i) {
            if (i >= expected.size() || i >= records_.size() || !(value(records_[i]) == expected[i]))
                throw std::runtime_error(diagnostic(reason + ": sequence mismatch at index " + std::to_string(i)));
        }
    }
    // Select a covered subwindow, excluding its start and including its endpoint.
    TraceCapture capture(const TraceWindow& requested) const {
        return select(window_, records_, times_, complete_, requested);
    }
    // Use a run result or acknowledged phase's interval directly.
    template<class Interval>
    TraceCapture capture(const Interval& interval) const { return capture(interval.window()); }
private:
    friend struct TraceState<Record>;
    TraceCapture(TraceWindow window, std::vector<Record> records,
                 std::vector<econopet::CycleTime> times, bool complete)
        : window_(std::move(window)), records_(std::move(records)), times_(std::move(times)), complete_(complete) {}
    // Validate before copying and binary-search sorted timestamps to copy only the requested range.
    static TraceCapture select(const TraceWindow& coverage, const std::vector<Record>& records,
                               const std::vector<econopet::CycleTime>& times, bool complete,
                               const TraceWindow& requested) {
        if (!coverage.source() || records.size() != times.size())
            throw std::logic_error(diagnostic(coverage, "trace capture: evidence was transferred out"));
        if (requested.source() != coverage.source())
            throw std::invalid_argument(diagnostic(coverage, "trace capture: foreign source"));
        if (requested.started() < coverage.started() || requested.completed() > coverage.completed())
            throw std::out_of_range(diagnostic(coverage, "trace capture: interval is not fully covered"));
        const auto first = std::upper_bound(times.begin(), times.end(), requested.started());
        const auto last = std::upper_bound(first, times.end(), requested.completed());
        const auto offset = first - times.begin();
        return TraceCapture(requested,
            std::vector<Record>(records.begin() + offset, records.begin() + offset + (last - first)),
            std::vector<econopet::CycleTime>(first, last), complete);
    }
    // Include the exact interval even when the source no longer exists.
    std::string diagnostic(const std::string& detail) const {
        return diagnostic(window_, detail);
    }
    // Format shared selection errors without allocating or copying capture storage.
    static std::string diagnostic(const TraceWindow& window, const std::string& detail) {
        return detail + " [interval=(" + std::to_string(window.started().value()) + ","
            + std::to_string(window.completed().value()) + "]]";
    }
    // Reject assertions that could mistake missing observations for quiet behavior.
    void require_complete(const std::string& reason) const {
        if (!complete_) throw std::logic_error(diagnostic(reason + ": incomplete observation coverage"));
    }
    TraceWindow window_;
    std::vector<Record> records_;
    std::vector<econopet::CycleTime> times_;
    bool complete_;
};

// Shared callback storage allows recorder moves without capturing a recorder's address.
template<class Record>
struct TraceState {
    std::shared_ptr<const int> source;
    econopet::CycleTime started, completed;
    std::vector<Record> records;
    std::vector<econopet::CycleTime> times;
    bool active = true, complete = true;

    // Append every source event, including multiple events at one boundary.
    void append(econopet::CycleTime at, Record record) {
        if (!active) return;
        try {
            if (at < completed || (!times.empty() && at < times.back()))
                throw std::invalid_argument("trace record: timestamp precedes capture");
            if (times.size() == times.capacity())
                times.reserve(std::max(size_t{8}, times.size() * 2));
            records.push_back(std::move(record));
            times.push_back(at);
        } catch (...) {
            complete = false;
            throw;
        }
    }
    // Reject gaps caused by skipped callbacks rather than claiming quiet coverage.
    void cover(econopet::CycleTime at) {
        if (!active) return;
        if (at != completed + econopet::Cycles{1}) {
            complete = false;
            throw std::logic_error("trace coverage: missed completed-cycle boundary");
        }
        completed = at;
    }
    // Copy all retained values, preserving whether capture completed successfully.
    TraceCapture<Record> capture() const {
        return TraceCapture<Record>(TraceWindow(source, started, completed), records, times,
            complete && (times.empty() || !(times.back() > completed)));
    }
    // Select directly from live storage using the same checks and boundaries as detached evidence.
    TraceCapture<Record> capture(const TraceWindow& requested) const {
        return TraceCapture<Record>::select(TraceWindow(source, started, completed), records, times,
            complete && (times.empty() || !(times.back() > completed)), requested);
    }
};

// Weak event taps preserve all writes/reads without retaining recorders or their source.
template<class Record>
class TraceSource {
public:
    // Attach storage owned by a scoped recorder.
    void subscribe(const std::shared_ptr<TraceState<Record>>& state) { subscribers_.push_back(state); }
    // Deliver a source event to each live recorder in registration order.
    void emit(econopet::CycleTime at, Record record) {
        subscribers_.erase(std::remove_if(subscribers_.begin(), subscribers_.end(),
            [](const auto& weak) { const auto state = weak.lock(); return !state || !state->active; }),
            subscribers_.end());
        try {
            for (const auto& weak : subscribers_)
                if (const auto state = weak.lock()) state->append(at, record);
        } catch (...) {
            invalidate();
            throw;
        }
    }
    // Retain missed-publication evidence for every active subscriber without clocks or allocation.
    void invalidate() noexcept {
        for (const auto& weak : subscribers_)
            if (const auto state = weak.lock(); state && state->active) state->complete = false;
    }
    // Freeze all recorder windows before a session releases its binding.
    void stop() {
        for (const auto& weak : subscribers_)
            if (const auto state = weak.lock()) state->active = false;
        subscribers_.clear();
    }
private:
    std::vector<std::weak_ptr<TraceState<Record>>> subscribers_;
};

enum class AvSampling { EveryCycle, ChangesOnly };
enum class Signal { CpuReset, CpuIrq, CpuNmi, CpuReady, Video, Horizontal, Vertical, Jiffy,
                    AudioLeft, AudioRight, Pia1Irq, Pia2Irq, ViaIrq };
struct SignalTransition {
    econopet::CycleTime at;
    bool before, after;
};
struct PeripheralWrite {
    econopet::CycleTime at;
    econopet::CpuAddress address;
    uint8_t data;
};

template<class Record> class ScopedRecorder;
class AvTrace;

} // namespace test_observation

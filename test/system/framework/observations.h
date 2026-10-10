// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "system.h"

namespace test_observation {

// One owner for storage and completed-cycle registration, safe to move or outlive its board.
template<class Record>
class ScopedRecorder {
public:
    // Register source-specific sampling without capturing this recorder's address.
    explicit ScopedRecorder(System& board,
        std::function<void(const System&, TraceState<Record>&)> sample = {})
        : state_(std::make_shared<TraceState<Record>>(TraceState<Record>{
            board.observation_source(), board.time(), board.time(), {}, {}})) {
        subscription_ = board.observe([state = state_, sample = std::move(sample)](const System& observed) {
            if (!state->active) return;
            try {
                state->cover(observed.time());
                if (sample) sample(observed, *state);
            } catch (...) {
                state->complete = false;
                throw;
            }
        });
    }
    // Disconnect without evaluating a board that may already have been destroyed.
    ~ScopedRecorder() { stop(); }
    ScopedRecorder(const ScopedRecorder&) = delete;
    ScopedRecorder& operator=(const ScopedRecorder&) = delete;
    ScopedRecorder(ScopedRecorder&&) noexcept = default;
    // Freeze the previous destination before transferring its registration.
    ScopedRecorder& operator=(ScopedRecorder&& other) noexcept {
        if (this != &other) {
            stop();
            state_ = std::move(other.state_);
            subscription_ = std::move(other.subscription_);
        }
        return *this;
    }
    // Stop idempotently without clocking, retaining already captured evidence.
    void stop() noexcept {
        subscription_.reset();
        if (state_) state_->active = false;
    }
    // Inspect retained values or copy them into detached coverage-bearing storage.
    const std::vector<Record>& records() const & { return state().records; }
    // Copy temporary recorder data without altering shared active storage.
    std::vector<Record> records() const && { return state().records; }
    // Borrow timestamps while the recorder remains alive.
    const std::vector<econopet::CycleTime>& times() const & { return state().times; }
    // Copy temporary recorder timestamps without altering shared active storage.
    std::vector<econopet::CycleTime> times() const && { return state().times; }
    // Detach values and the coverage accumulated up to the last observed boundary.
    TraceCapture<Record> capture() const { return state().capture(); }
    // Restrict detached evidence to a source-bound covered interval.
    TraceCapture<Record> capture(const TraceWindow& window) const { return state().capture(window); }
    // Bind to a checked run result or acknowledged phase without manual timestamps.
    template<class Interval>
    TraceCapture<Record> capture(const Interval& interval) const { return capture(interval.window()); }

protected:
    // Let a real event source tap the same scoped storage as the coverage observer.
    const std::shared_ptr<TraceState<Record>>& storage() const & { return state_; }
    // Retain shared storage independently of a temporary recorder.
    std::shared_ptr<TraceState<Record>> storage() const && { return state_; }
private:
    friend class ::System;
    // Attach either an event source or fitted-chip history without exposing recorder storage.
    template<class Attach>
    static ScopedRecorder attached(System& board, Attach attach) {
        ScopedRecorder recorder(board);
        attach(recorder.state_);
        return recorder;
    }
    // Moved-from recorders cannot masquerade as an empty successful trace.
    const TraceState<Record>& state() const {
        if (!state_) throw std::logic_error("trace access: moved-from recorder");
        return *state_;
    }
    std::shared_ptr<TraceState<Record>> state_;
    System::ObserverSubscription subscription_;
};

struct AvLevels {
    bool video, horizontal, vertical, jiffy, audio_left, audio_right;
    // Compare the complete sampled digital bundle.
    friend bool operator==(const AvLevels& lhs, const AvLevels& rhs) {
        return lhs.video == rhs.video && lhs.horizontal == rhs.horizontal
            && lhs.vertical == rhs.vertical && lhs.jiffy == rhs.jiffy
            && lhs.audio_left == rhs.audio_left && lhs.audio_right == rhs.audio_right;
    }
};
struct AvRecord { econopet::CycleTime at; AvLevels levels; };

// Digital A/V boundary samples, not rendered frames or analog audio.
class AvTrace : public ScopedRecorder<AvRecord> {
public:
    using Sampling = AvSampling;
    using Levels = AvLevels;
    using Record = AvRecord;

    // Record the initial evaluated state and subscribe to completed-cycle samples.
    explicit AvTrace(System& system, Sampling sampling) : ScopedRecorder(system, sampler(system, sampling)) {
        storage()->append(system.time(), {system.time(), levels(system)});
    }
    AvTrace(const AvTrace&) = delete;
    AvTrace& operator=(const AvTrace&) = delete;
    AvTrace(AvTrace&&) noexcept = default;
    AvTrace& operator=(AvTrace&&) noexcept = default;
private:
    // Resolve the same digital bundle initially and at completed-cycle boundaries.
    static Levels levels(const System& board) {
        const auto pins = board.snapshot();
        return {pins.video_o, pins.horiz_drive_o, pins.vert_drive_o,
            pins.jiffy_clock_o, pins.audio_l_o, pins.audio_r_o};
    }
    // Validate before registration, including forged policies.
    static std::function<void(const System&, TraceState<Record>&)> sampler(System& board, Sampling sampling) {
        if (sampling != Sampling::EveryCycle && sampling != Sampling::ChangesOnly)
            throw board.failure<std::invalid_argument>("AvTrace", "unsupported A/V sampling policy");
        return [sampling](const System& observed, TraceState<Record>& state) {
            const auto current = levels(observed);
            if (sampling == Sampling::EveryCycle || !(state.records.back().levels == current))
                state.append(observed.time(), {observed.time(), current});
        };
    }
};

// Explicit manual log for already captured evidence, not a coverage-bearing source recorder.
template<class Event>
class EventTrace {
public:
    struct Record {
        econopet::CycleTime at;
        Event event;
    };

    // Appends a timestamped event, rejecting backwards time without altering the trace.
    void record(econopet::CycleTime at, Event event) {
        if (!records_.empty() && at < records_.back().at)
            throw std::invalid_argument("event record: timestamp=" + std::to_string(at.value())
                + " precedes last timestamp=" + std::to_string(records_.back().at.value()));
        records_.push_back({at, std::move(event)});
    }
    // Exposes immutable records for detailed test diagnostics.
    const std::vector<Record>& records() const & { return records_; }
    // Transfer a temporary manual event log's records.
    std::vector<Record> records() && { return std::move(records_); }
    // Copy an immutable temporary manual event log's records.
    std::vector<Record> records() const && { return records_; }

private:
    std::vector<Record> records_;
};

// Requires a condition initially and at every completed-cycle boundary through the endpoint.
template<class Predicate>
void require_stable(System& system, Predicate condition, econopet::Cycles duration,
                    const std::string& reason) {
    const auto end = system.time() + duration;
    system.run_until([&](const System& observed) {
        if (!condition(observed))
            throw observed.failure<std::runtime_error>("require_stable", reason + ": condition was not stable");
        return observed.time() == end;
    }, duration, reason);
}

// Requires an event detector to remain false at every boundary, including both endpoints.
// The detector must be observation-only. Pulses between cycle boundaries are not sampled.
template<class Predicate>
void require_absent(System& system, Predicate event, econopet::Cycles duration,
                    const std::string& reason) {
    require_stable(system, [&](const System& observed) { return !event(observed); }, duration, reason);
}

} // namespace test_observation

// Obtain the scoped A/V recorder from its clock-owning source.
inline test_observation::AvTrace System::trace_av(test_observation::AvSampling sampling) & {
    return test_observation::AvTrace(*this, sampling);
}

// Keep custom event detectors under the same coverage and mutation guard as built-in sources.
template<class Event, class Detector>
test_observation::ScopedRecorder<Event> System::trace_events(Detector detector) & {
    static_assert(std::is_invocable_r_v<std::optional<Event>, Detector&, const System&>,
        "trace detector must return optional<Event> from const System&");
    return test_observation::ScopedRecorder<Event>(*this,
        [detector = std::move(detector)](const System& board, auto& state) mutable {
            if (auto event = detector(board)) state.append(board.time(), std::move(*event));
        });
}

// Subscribe directly to PIA1 writes, independently of resets or observation clearing.
inline test_observation::ScopedRecorder<econopet::io::WriteRecord<econopet::io::PiaRegister>>
System::trace_pia1_writes() & {
    using Recorder = test_observation::ScopedRecorder<econopet::io::WriteRecord<econopet::io::PiaRegister>>;
    return Recorder::attached(*this, [this](const auto& storage) { io().pia1().writes().trace(storage); });
}

// Subscribe directly to PIA2 writes without an instance-selection argument.
inline test_observation::ScopedRecorder<econopet::io::WriteRecord<econopet::io::PiaRegister>>
System::trace_pia2_writes() & {
    using Recorder = test_observation::ScopedRecorder<econopet::io::WriteRecord<econopet::io::PiaRegister>>;
    return Recorder::attached(*this, [this](const auto& storage) { io().pia2().writes().trace(storage); });
}

// Subscribe directly to the VIA write source, not its clearable counters.
inline test_observation::ScopedRecorder<econopet::io::WriteRecord<econopet::io::ViaRegister>>
System::trace_via_writes() & {
    using Recorder = test_observation::ScopedRecorder<econopet::io::WriteRecord<econopet::io::ViaRegister>>;
    return Recorder::attached(*this, [this](const auto& storage) { io().via().writes().trace(storage); });
}

// Observe the selected signal's evaluated level (active-low nets retain electrical polarity).
inline test_observation::ScopedRecorder<test_observation::SignalTransition>
System::trace_signal(test_observation::Signal signal) & {
    using test_observation::Signal;
    const auto level = [signal](const System& board) {
        const auto p = board.snapshot();
        switch (signal) {
        case Signal::CpuReset: return p.cpu_reset_active_o;
        case Signal::CpuIrq: return p.cpu_irq_n_o;
        case Signal::CpuNmi: return p.cpu_nmi_n_o;
        case Signal::CpuReady: return p.cpu_ready_o;
        case Signal::Video: return p.video_o;
        case Signal::Horizontal: return p.horiz_drive_o;
        case Signal::Vertical: return p.vert_drive_o;
        case Signal::Jiffy: return p.jiffy_clock_o;
        case Signal::AudioLeft: return p.audio_l_o;
        case Signal::AudioRight: return p.audio_r_o;
        case Signal::Pia1Irq: return board.io().pia1().irq();
        case Signal::Pia2Irq: return board.io().pia2().irq();
        case Signal::ViaIrq: return board.io().via().irq();
        }
        throw board.failure<std::invalid_argument>("trace_signal", "unsupported signal");
    };
    return test_observation::ScopedRecorder<test_observation::SignalTransition>(*this,
        [level, previous = level(*this)](const System& board, auto& state) mutable {
            const bool current = level(board);
            if (current != previous) state.append(board.time(), {board.time(), previous, current});
            previous = current;
        });
}

// Capture completed physical PIA/VIA bus writes in board dispatch order.
inline test_observation::ScopedRecorder<test_observation::PeripheralWrite> System::trace_peripheral_writes() & {
    using Recorder = test_observation::ScopedRecorder<test_observation::PeripheralWrite>;
    return Recorder::attached(*this, [this](const auto& storage) { peripheral_writes_.subscribe(storage); });
}

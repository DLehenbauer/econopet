// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "framework/scheduler.h"
#include "framework/trace.h"
#include "system.h"

// Relative schedules choose their origin instead of inferring a CPU lifecycle.
enum class RunAnchor { RunEntry, CpuRelease };
enum class ServiceTiming { AfterSlice };

// One single-use configuration for board waits and checked CPU execution.
// The board and all callback captures must outlive the terminal operation.
class RunConfiguration {
public:
    using Schedule = DeterministicScheduler;
    using MissedService = Schedule::MissedService;
    using Stimulus = Schedule::Stimulus;
    using Service = Schedule::Service;
    using Predicate = std::function<bool(const System&)>;

    // Configure without attaching a scheduler, clocking or changing board state.
    explicit RunConfiguration(System& board) : board_(board) {}
    RunConfiguration(const RunConfiguration&) = delete;
    RunConfiguration& operator=(const RunConfiguration&) = delete;
    RunConfiguration(RunConfiguration&&) = delete;
    RunConfiguration& operator=(RunConfiguration&&) = delete;

    // Set the elapsed budget, including reset and installation for CPU programs.
    RunConfiguration& within(econopet::Cycles budget) {
        require_open();
        budget_ = budget;
        return *this;
    }
    // Schedule non-clocking input stimulus at an exact relative cycle boundary.
    RunConfiguration& after(econopet::Cycles delay, Stimulus work,
                            RunAnchor anchor = RunAnchor::RunEntry) {
        require_open();
        require_anchor(anchor);
        if (!work) throw board_.failure<std::invalid_argument>("run after", "stimulus is empty");
        events_.push_back({delay, anchor, std::move(work)});
        return *this;
    }
    // First service is one period after the anchor, with phase retained thereafter.
    RunConfiguration& every(econopet::Cycles period, MissedService policy, Service work,
                            RunAnchor anchor = RunAnchor::RunEntry) {
        require_open();
        require_scheduled_service();
        require_anchor(anchor);
        require_period(period, work);
        if (slice_) throw board_.failure<std::logic_error>("run every", "cannot mix phase-anchored and after-slice work");
        if (policy != MissedService::Coalesce && policy != MissedService::Reject)
            throw board_.failure<std::invalid_argument>("run every", "unsupported missed-service policy");
        periodic_.push_back({period, policy, anchor, std::move(work)});
        return *this;
    }
    // Advance a full slice, then service. Service duration shifts the next slice.
    // For programs, slices begin only after CPU release. A partial final slice
    // reaches the deadline without calling work.
    RunConfiguration& every(econopet::Cycles slice, ServiceTiming timing, Service work) {
        require_open();
        require_scheduled_service();
        require_period(slice, work);
        if (timing != ServiceTiming::AfterSlice)
            throw board_.failure<std::invalid_argument>("run every", "unsupported service timing");
        if (slice_ || !periodic_.empty())
            throw board_.failure<std::logic_error>("run every", "after-slice work requires one exclusive service");
        slice_.emplace(Slice{slice, std::move(work)});
        return *this;
    }
    // Invoke active work on each polling pass, preserving exact stimulus and predicate checks.
    RunConfiguration& service(Service work) {
        require_open();
        if (!work) throw board_.failure<std::invalid_argument>("run service", "service is empty");
        if (service_ || slice_ || !periodic_.empty())
            throw board_.failure<std::logic_error>("run service", "polling service requires exclusive active work");
        service_ = std::move(work);
        return *this;
    }
    // Observation only. CPU runs require both program completion and this condition.
    RunConfiguration& until(Predicate done) {
        require_open();
        if (!done) throw board_.failure<std::invalid_argument>("run until", "predicate is empty");
        if (done_) throw board_.failure<std::logic_error>("run until", "predicate already configured");
        done_ = std::move(done);
        return *this;
    }
    // Wait for the predicate, or advance exactly the budget when none is supplied.
    test_observation::TraceWindow run(const std::string& reason = "configured board run");

    // Check the execution context before assembly consumes a CPU builder.
    void validate(System& board, bool has_release) const {
        require_open();
        if (&board != &board_)
            throw board_.failure<std::invalid_argument>("configured run", "configuration belongs to another board");
        if (!budget_) throw board_.failure<std::logic_error>("configured run", "requires within(budget)");
        if (!has_release) {
            for (const auto& event : events_) require_board_anchor(event.anchor);
            for (const auto& work : periodic_) require_board_anchor(work.anchor);
        }
    }

    // Shared plumbing for checked startup and phase runners. The release hook must
    // be called exactly once, after reset release (or after opening a phase gate).
    template<class Execute>
    decltype(auto) execute(System& board, const std::string& reason, bool has_release, Execute operation) {
        validate(board, has_release);
        consumed_ = true;
        Schedule schedule(board_);
        System::DeadlineScope deadline(board_, board_.time() + *budget_, reason);
        const auto install = [&](RunAnchor anchor) {
            const auto origin = board_.time();
            for (size_t i = 0; i < events_.size(); ++i) {
                const auto& event = events_[i];
                if (event.anchor == anchor) schedule.at_ordered(origin + event.delay, event.work, i);
            }
            for (size_t i = 0; i < periodic_.size(); ++i) {
                const auto& work = periodic_[i];
                if (work.anchor == anchor)
                    schedule.every_ordered(origin + work.period, work.period, work.policy, work.work, i);
            }
        };

        install(RunAnchor::RunEntry);
        bool released = false;
        const auto release = [&] {
            if (!has_release)
                throw board_.failure<std::logic_error>("configured run", "board-only run has no CPU release hook");
            if (released) throw board_.failure<std::logic_error>("configured run", "CPU release hook called twice");
            install(RunAnchor::CpuRelease);
            released = true;
        };
        const auto service = [&](System& observed, econopet::Cycles remaining) {
            try {
                schedule.require_health();
                if (&observed != &board_)
                    throw board_.failure<std::invalid_argument>("configured service", "configuration belongs to another board");
                if (service_) {
                    schedule.require_registration();
                    if (board_.spi_active_)
                        throw board_.failure<std::logic_error>("configured service", "polling service requires an unowned SPI bus");
                    if (has_release && !released)
                        throw board_.failure<std::logic_error>("configured service", "polling service requires CPU release");
                    schedule.apply_stimulus();
                    service_(observed, remaining);
                } else if (slice_) {
                    schedule.require_registration();
                    if (board_.spi_active_)
                        throw board_.failure<std::logic_error>("configured service", "after-slice service requires an unowned SPI bus");
                    if (has_release && !released)
                        throw board_.failure<std::logic_error>("configured service", "after-slice service requires CPU release");
                    const auto elapsed = std::min(slice_->period, remaining);
                    observed.tick(elapsed);
                    if (elapsed == slice_->period) {
                        schedule.apply_stimulus();
                        slice_->work(observed, remaining - elapsed);
                    }
                } else {
                    schedule.step(observed, remaining);
                }
                schedule.require_health();
            } catch (...) {
                schedule.failed_ = true;
                throw;
            }
        };
        const auto done = [&](const System& observed) {
            schedule.require_health();
            const bool complete = !done_ || done_(observed);
            schedule.require_health();
            return complete;
        };
        const auto finish = [&] {
            schedule.require_health();
            if (has_release && !released)
                throw board_.failure<std::logic_error>("configured run", "CPU release hook was not called");
        };
        if constexpr (std::is_void_v<decltype(operation(*budget_, service, release, done))>) {
            operation(*budget_, service, release, done);
            finish();
        } else {
            decltype(auto) result = operation(*budget_, service, release, done);
            finish();
            return result;
        }
    }

private:
    template<class> friend class test_asm::ProgramBuilder;
    // An independent configuration may replace a budget, but never discard authored work.
    void require_replaceable() const {
        require_open();
        if (!events_.empty() || !periodic_.empty() || slice_ || service_ || done_)
            throw board_.failure<std::logic_error>("program configuration", "cannot replace authored work");
    }
    // Polling and periodic/sliced callbacks have distinct cadence and cannot share ownership.
    void require_scheduled_service() const {
        if (service_)
            throw board_.failure<std::logic_error>("run every", "polling service requires exclusive active work");
    }
    struct Event { econopet::Cycles delay; RunAnchor anchor; Stimulus work; };
    struct Periodic {
        econopet::Cycles period;
        MissedService policy;
        RunAnchor anchor;
        Service work;
    };
    struct Slice { econopet::Cycles period; Service work; };

    // Configuration changes are active operations, including inside predicates.
    void require_open() const {
        board_.require_mutable_access();
        if (consumed_) throw board_.failure<std::logic_error>("configured run", "configuration already consumed");
    }
    // Forged enum values never become silent defaults.
    void require_anchor(RunAnchor anchor) const {
        if (anchor != RunAnchor::RunEntry && anchor != RunAnchor::CpuRelease)
            throw board_.failure<std::invalid_argument>("configured run", "unsupported schedule anchor");
    }
    // Board-only execution has no implicit CPU release event.
    void require_board_anchor(RunAnchor anchor) const {
        if (anchor == RunAnchor::CpuRelease)
            throw board_.failure<std::invalid_argument>("configured run", "CPU-release anchor requires checked CPU execution");
    }
    // Reject invalid work before registering any callbacks or changing board state.
    void require_period(econopet::Cycles period, const Service& work) const {
        if (period == econopet::Cycles{0} || !work)
            throw board_.failure<std::invalid_argument>("run every", "requires a positive period and nonempty service");
    }
    System& board_;
    std::optional<econopet::Cycles> budget_;
    std::vector<Event> events_;
    std::vector<Periodic> periodic_;
    std::optional<Slice> slice_;
    Predicate done_;
    Service service_;
    bool consumed_ = false;
};

inline test_observation::TraceWindow RunConfiguration::run(const std::string& reason) {
    return execute(board_, reason, false, [&](econopet::Cycles budget, auto service, auto, auto done) {
        const auto start = board_.time();
        const auto end = board_.time() + budget;
        board_.service_until([&](const System& observed) {
            const bool complete = done(observed);
            return done_ ? complete : !(observed.time() < end);
        }, std::move(service), budget, reason);
        return board_.trace_window(start, board_.time());
    });
}

// Create a single-use configuration without retaining a temporary board.
inline RunConfiguration System::execution() & {
    require_mutable_access();
    return RunConfiguration(*this);
}

#include "framework/program.h"

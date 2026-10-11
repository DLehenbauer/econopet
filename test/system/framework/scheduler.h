// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "types.h"

class System;
class RunConfiguration;

enum class MissedService { Coalesce, Reject };

// Scoped deterministic orchestration. System remains the only clock owner.
// The board must outlive this nonmovable owner and all callbacks it retains.
class DeterministicScheduler {
    friend class RunConfiguration;
public:
    using MissedService = ::MissedService;
    using Stimulus = std::function<void(System&)>;
    using Service = std::function<void(System&, econopet::Cycles)>;

    // Attach one scheduler to this board, including clocks inside SPI operations.
    explicit DeterministicScheduler(System& system);
    // Detach without clocking, discarding all remaining callbacks.
    ~DeterministicScheduler();
    DeterministicScheduler(const DeterministicScheduler&) = delete;
    DeterministicScheduler& operator=(const DeterministicScheduler&) = delete;
    DeterministicScheduler(DeterministicScheduler&&) = delete;
    DeterministicScheduler& operator=(DeterministicScheduler&&) = delete;

    // Apply input changes at this boundary, before evaluation of its next cycle.
    // Same-time actions execute in registration order. Past times are errors.
    void at(econopet::CycleTime time, Stimulus stimulus);
    // Register phase-anchored periodic active work, first due at the given time.
    // Work receives the shared deadline's remaining budget, not its period.
    void every(econopet::CycleTime first, econopet::Cycles period,
               MissedService policy, Service service);
    // Apply boundary inputs before external after-slice work without adding clocks.
    void apply_stimulus();
    // One active polling pass. Inputs precede service, with equal-due work in order.
    void step(System& board, econopet::Cycles remaining);

    // Wait for an observation-only condition under the board's inclusive deadline.
    template<class Predicate>
    void run_until(Predicate done, econopet::Cycles budget, const std::string& reason);

private:
    struct Event { econopet::CycleTime at; size_t order; Stimulus callback; };
    struct Periodic {
        std::optional<econopet::CycleTime> next;
        econopet::Cycles period;
        MissedService policy;
        size_t order;
        Service callback;
    };

    // Preserve author order when relative anchors are installed at different times.
    void at_ordered(econopet::CycleTime time, Stimulus stimulus, size_t order);
    // Equal-due services retain configuration registration order across anchors.
    void every_ordered(econopet::CycleTime first, econopet::Cycles period,
                       MissedService policy, Service service, size_t order);
    // Reject failed evidence before callbacks or successful completion are accepted.
    void require_health() const;
    // Reject observer mutation, callback registration and reuse after partial failure.
    void require_registration() const;
    // Consume each event once, with clock/SPI entry forbidden until it returns.
    void dispatch();

    System& system_;
    std::vector<Event> events_;
    std::vector<Periodic> services_;
    size_t event_order_ = 0, service_order_ = 0;
    bool servicing_ = false;
    bool failed_ = false;
};

// System's configured-run API needs the complete scheduler declaration.
#include "system.h"

template<class Predicate>
void DeterministicScheduler::run_until(Predicate done, econopet::Cycles budget, const std::string& reason) {
    require_registration();
    system_.service_until([&](const System& board) {
        require_health();
        const bool complete = std::invoke(done, board);
        require_health();
        return complete;
    }, [this](System& board, econopet::Cycles remaining) { step(board, remaining); },
        budget, reason);
}

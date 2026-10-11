// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "framework/scheduler.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "system.h"

DeterministicScheduler::DeterministicScheduler(System& system) : system_(system) {
    require_registration();
    if (system_.scheduled_boundary_)
        throw system_.failure<std::logic_error>("Scheduler", "board already has a scheduler");
    system_.scheduled_boundary_ = [this] { dispatch(); };
}

DeterministicScheduler::~DeterministicScheduler() { system_.scheduled_boundary_ = {}; }

void DeterministicScheduler::at(econopet::CycleTime time, Stimulus stimulus) {
    at_ordered(time, std::move(stimulus), event_order_++);
}

void DeterministicScheduler::at_ordered(econopet::CycleTime time, Stimulus stimulus, size_t order) {
    require_registration();
    if (time < system_.time()) throw system_.failure<std::invalid_argument>("schedule at", "stimulus is in the past");
    if (!stimulus) throw system_.failure<std::invalid_argument>("schedule at", "stimulus is empty");
    Event event{time, order, std::move(stimulus)};
    const auto position = std::upper_bound(events_.begin(), events_.end(), event,
        [](const Event& next, const Event& existing) {
            return next.at < existing.at || (next.at == existing.at && next.order < existing.order);
        });
    events_.insert(position, std::move(event));
}

void DeterministicScheduler::every(econopet::CycleTime first, econopet::Cycles period,
                                   MissedService policy, Service service) {
    every_ordered(first, period, policy, std::move(service), service_order_++);
}

void DeterministicScheduler::every_ordered(econopet::CycleTime first, econopet::Cycles period,
                                           MissedService policy, Service service, size_t order) {
    require_registration();
    if (first < system_.time()) throw system_.failure<std::invalid_argument>("schedule every", "first service time is in the past");
    if (period == econopet::Cycles{0}) throw system_.failure<std::invalid_argument>("schedule every", "service period must be positive");
    if (policy != MissedService::Coalesce && policy != MissedService::Reject)
        throw system_.failure<std::invalid_argument>("schedule every", "unsupported missed-service policy");
    if (!service) throw system_.failure<std::invalid_argument>("schedule every", "service is empty");
    services_.push_back({first, period, policy, order, std::move(service)});
}

void DeterministicScheduler::apply_stimulus() {
    require_registration();
    dispatch();
}

void DeterministicScheduler::step(System& board, econopet::Cycles remaining) {
    require_registration();
    if (&board != &system_) throw system_.failure<std::invalid_argument>("scheduler step", "scheduler belongs to another board");
    if (remaining == econopet::Cycles{0})
        throw system_.failure<std::invalid_argument>("scheduler step", "needs a positive budget");
    if (system_.spi_active_)
        throw system_.failure<std::logic_error>("scheduler step", "scheduled service requires an unowned SPI bus");
    // Bound callback clocks even when step is called outside a configured run.
    System::DeadlineScope deadline(system_, system_.time() + remaining, "scheduled service");
    dispatch();
    bool called = false;
    for (;;) {
        size_t selected = services_.size();
        for (size_t i = 0; i < services_.size(); ++i) {
            if (!services_[i].next || *services_[i].next > system_.time()) continue;
            if (selected == services_.size() || services_[i].next < services_[selected].next
                || (services_[i].next == services_[selected].next
                    && services_[i].order < services_[selected].order))
                selected = i;
        }
        if (selected == services_.size()) {
            if (!called) system_.tick(1);
            return;
        }
        auto& service = services_[selected];
        if (service.policy == MissedService::Reject && *service.next < system_.time()) {
            failed_ = true;
            throw system_.failure<std::runtime_error>("scheduler step", "scheduled service missed cycle " + std::to_string(service.next->value()));
        }
        servicing_ = true;
        const auto before = system_.time();
        try {
            service.callback(system_, system_.deadline_->at - system_.time());
            require_health();
            const auto slots = service.policy == MissedService::Coalesce
                ? (system_.time() - *service.next).value() / service.period.value() + 1 : 1;
            // Retire unreachable recurrences without overflowing duration or timestamp arithmetic.
            const auto available = econopet::Cycles::Maximum - service.next->value();
            if (slots > available / service.period.value()) service.next.reset();
            else service.next = *service.next + service.period * slots;
            servicing_ = false;
        } catch (...) {
            servicing_ = false;
            failed_ = true;
            throw;
        }
        called = true;
        if (system_.time() != before) return;
    }
}

void DeterministicScheduler::require_health() const {
    if (system_.clock_faulted_)
        throw system_.failure<std::logic_error>("schedule health", "recover the suspended clock before scheduler work");
    if (system_.spi_failed_)
        throw system_.failure<std::logic_error>("schedule health", "scheduler requires a usable SPI board");
    if (failed_) throw system_.failure<std::logic_error>("schedule health", "scheduler is unusable after a callback failure");
}

void DeterministicScheduler::require_registration() const {
    system_.require_mutable_access();
    require_health();
    if (system_.dispatching_stimulus_ || servicing_)
        throw system_.failure<std::logic_error>("schedule registration", "callbacks cannot change or reenter the schedule");
}

void DeterministicScheduler::dispatch() {
    if (failed_) throw system_.failure<std::logic_error>("scheduled stimulus", "scheduler is unusable after a callback failure");
    if (system_.dispatching_stimulus_)
        throw system_.failure<std::logic_error>("scheduled stimulus", "cannot recursively dispatch");
    while (!events_.empty() && !(system_.time() < events_.front().at)) {
        auto callback = std::move(events_.front().callback);
        events_.erase(events_.begin());
        system_.dispatching_stimulus_ = true;
        try {
            callback(system_);
            system_.dispatching_stimulus_ = false;
        } catch (...) {
            system_.dispatching_stimulus_ = false;
            failed_ = true;
            throw;
        }
    }
}

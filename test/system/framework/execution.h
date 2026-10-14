// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include <array>
#include <utility>
#include <vector>

#include "framework/asm.h"
#include "framework/run.h"
#include "registers.h"
#include "system.h"

namespace test_asm {

// Validate the complete program before entering a deadline-bound startup lifecycle.
template<class Memory>
template<class CpuProgram, class Service, class Released, class Predicate>
ExecutionResult Scenario<Memory>::execute_program(CpuProgram& program, const Region* results,
    econopet::Cycles budget, const std::string& reason, Service service, Released released, Predicate extra,
    bool capture_results) {
    program.require_live();
    if (!program.completion_)
        throw failure<std::logic_error>("scenario run", "scenario execution requires program.finish");
    const auto marker = *program.completion_;
    return start_program(program, results, marker, budget, reason, std::move(service), std::move(released),
        [marker, extra = std::move(extra)](const Memory& observed) {
            return observed.peek(econopet::SramAddress{marker.address().value()}) == marker.completed()
                && extra(observed);
        }, [] {}, capture_results);
}

// Resolve all code before clocks, then establish reset, install and release exactly once.
template<class Memory>
template<class CpuProgram, class Service, class Released, class Predicate, class Prepared>
ExecutionResult Scenario<Memory>::start_program(CpuProgram& program, const Region* results,
    const CompletionMarker& marker, econopet::Cycles budget, const std::string& reason,
    Service service, Released released, Predicate done, Prepared prepared, bool capture_results) {
    using Cpu = typename CpuProgram::CpuType;
    program.require_live();
    if (&program.scenario_ != this)
        throw failure<std::invalid_argument>("scenario run", "program belongs to another scenario");
    require(marker.region_, RegionKind::Mailbox);
    if (results) {
        require_owned(*results);
        if (results->kind_ != RegionKind::Results && results->kind_ != RegionKind::Data)
            throw failure<std::invalid_argument>("scenario capture", "requires ordinary data");
    }
    constexpr auto vector = std::is_same_v<Cpu, Mos6502> ? pet::Reset6502 : pet::Reset6809;
    bool owns_vector = false;
    for (const auto& region : regions_)
        if (region.kind_ == RegionKind::Vectors && region.start_.value() <= vector.value()
            && region.start_.value() + region.size_ >= vector.value() + 2) owns_vector = true;
    if (!owns_vector) throw failure<std::invalid_argument>("scenario run", "must reserve the CPU reset vector");
    const auto code = program.resolved_bytes();
    std::vector<uint8_t> captured(results ? results->size() : 0, 0);
    const auto started = memory_.time();
    bool initialized = false;
    // Keep startup and polling inside the earliest inherited deadline.
    memory_.service_until([&](const Memory& observed) {
        return initialized && done(observed);
    }, [&](Memory& board, econopet::Cycles remaining) {
        if (initialized) {
            service(board, remaining);
            return;
        }
        // Quiesce the selected core before touching code, results or native vectors.
        auto cpu = board.cpu(std::is_same_v<Cpu, Mos6502> ? CPU_SOFT_6502 : CPU_SOFT_6809);
        cpu.prepare();
        if (results && results->kind_ == RegionKind::Results) write(*results, 0, captured);
        const std::array<uint8_t, 1> pending{marker.pending()};
        write(marker.region_, 0, pending);
        write(program.region_, 0, code);
        cpu.write_vector(program.entry());
        prepared();
        cpu.release_reset();
        released();
        initialized = true;
    }, budget, reason);
    // Sample exactly at completion, with no cleanup or capture clocks.
    if (capture_results) {
        for (size_t i = 0; i < captured.size(); ++i)
            captured[i] = memory_.peek(econopet::SramAddress{results->start().value() + i});
    } else {
        captured.clear();
    }
    return {owner_, results ? results->start() : econopet::CpuAddress{0}, std::move(captured),
        started, memory_.time(), memory_.trace_window(started, memory_.time())};
}

// Run and capture one ordinary owned reservation with caller-provided active work.
template<class Memory>
template<class CpuProgram, class Service>
ExecutionResult Scenario<Memory>::run(CpuProgram& program, const Region& results, econopet::Cycles budget,
                                      const std::string& reason, Service service) {
    return execute_program(program, &results, budget, reason, std::move(service), [] {},
        [](const Memory&) { return true; }, true);
}
// Run without selected result data using single-cycle polling.
template<class Memory>
template<class CpuProgram>
ExecutionResult Scenario<Memory>::run(CpuProgram& program, econopet::Cycles budget, const std::string& reason) {
    return run(program, budget, reason, [](Memory& board, econopet::Cycles) { board.tick(1); });
}
// Run without selected result data using caller-provided active work.
template<class Memory>
template<class CpuProgram, class Service>
ExecutionResult Scenario<Memory>::run(CpuProgram& program, econopet::Cycles budget,
                                      const std::string& reason, Service service) {
    return execute_program(program, nullptr, budget, reason, std::move(service), [] {},
        [](const Memory&) { return true; }, true);
}
// Capture an owned reservation under a configured schedule and release anchor.
template<class Memory>
template<class CpuProgram>
ExecutionResult Scenario<Memory>::run(CpuProgram& program, const Region& results,
                                      RunConfiguration& configuration, const std::string& reason) {
    return configuration.execute(memory_, reason, true, [&](auto budget, auto service, auto released, auto done) {
        return execute_program(program, &results, budget, reason, service, released, done, true);
    });
}
// Execute a result-free program under the same configured lifecycle.
template<class Memory>
template<class CpuProgram>
ExecutionResult Scenario<Memory>::run(CpuProgram& program, RunConfiguration& configuration, const std::string& reason) {
    return configuration.execute(memory_, reason, true, [&](auto budget, auto service, auto released, auto done) {
        return execute_program(program, nullptr, budget, reason, service, released, done, true);
    });
}
} // namespace test_asm

// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <optional>

#include "framework/observations.h"
#include "framework/system_test.h"

using econopet::Cycles;
using test_asm::imm;

namespace {
constexpr Cycles Budget{20000};
}

// Both CPUs retain mandatory completion while anchoring work at checked release.
TEST_F(SystemTest, ConfiguredProgramsAnchorWorkAtCheckedCpuRelease) {
    const auto execute = [&](auto& test) {
        const auto answer = test.byte("answer");
        const auto entered = system.time();
        std::optional<econopet::CycleTime> released, first;
        bool entry_called = false;
        const auto result = test.within(Budget)
            .after(0, [&](System& board) { entry_called = true; EXPECT_EQ(board.time(), entered); })
            .after(0, [&](System& board) {
                released = board.time();
                EXPECT_FALSE(board.cpu().peek_reset());
            }, RunAnchor::CpuRelease)
            .every(7, MissedService::Reject, [&](System& board, Cycles) {
                if (!first) first = board.time();
            }, RunAnchor::CpuRelease)
            .run([&](auto& p) { p.lda(imm(0x42)).sta(answer); });
        ASSERT_TRUE(released);
        ASSERT_TRUE(first);
        EXPECT_TRUE(entry_called);
        EXPECT_EQ(*first, *released + Cycles{7});
        EXPECT_GT(*released, entered);
        EXPECT_EQ(result.started(), entered);
        EXPECT_EQ(result[answer], 0x42);
    };
    auto mos = system.program6502();
    execute(mos);
    auto mc = system.program6809();
    execute(mc);
}

// Startup lateness must not be silently re-anchored to CPU release.
TEST_F(SystemTest, ConfiguredProgramsEntryPolicyDetectsStartupMisses) {
    auto strict = system.program6502();
    unsigned calls = 0;
    EXPECT_THROW(strict.within(Budget).every(1, MissedService::Reject,
        [&](System&, Cycles) { ++calls; }).run([](auto&) {}), std::runtime_error);
    EXPECT_EQ(calls, 0);
    auto coalesced = system.program6502();
    EXPECT_NO_THROW(coalesced.within(Budget).every(1, MissedService::Coalesce,
        [&](System&, Cycles) { ++calls; }).run([](auto&) {}));
    EXPECT_GT(calls, 0);
}

// Extra conditions cannot substitute for freshly initialized CPU completion.
TEST_F(SystemTest, ConfiguredProgramsUntilRequiresCompletionAndSharedDeadline) {
    auto test = system.program6502();
    const auto answer = test.byte("answer");
    const auto start = system.time();
    const auto result = test.within(Budget).until([](const System&) { return true; })
        .run([&](auto& p) { p.lda(imm(0x37)).sta(answer); });
    EXPECT_EQ(result[answer], 0x37);
    EXPECT_GT(result.completed(), start);
    auto incomplete = system.program6502();
    const auto before = system.time();
    EXPECT_THROW(incomplete.within(Budget).until([](const System&) { return false; })
        .run([](auto&) {}), std::runtime_error);
    EXPECT_EQ(system.time(), before + Budget);
}

// Independent configurations remain board-owned when passed to a builder.
TEST_F(SystemTest, ConfiguredProgramsIndependentConfigurationAndForeignBoardChecks) {
    auto test = system.program6809();
    const auto answer = test.word("answer");
    auto run = system.execution();
    run.within(Budget);
    const auto result = test.run([&](auto& p) { p.store(answer, test_asm::imm16(0x1234)); }, run);
    EXPECT_EQ(result[answer], 0x1234);
    System other(system.seed());
    auto foreign = other.execution();
    foreign.within(Budget);
    auto rejected = system.program6502();
    const auto start = system.time();
    bool assembled = false;
    EXPECT_THROW(rejected.run([&](auto&) { assembled = true; }, foreign), std::invalid_argument);
    EXPECT_FALSE(assembled);
    EXPECT_EQ(system.time(), start);
}

// Independent configurations cannot discard authored work.
TEST_F(SystemTest, ConfiguredProgramsRejectReplacingAuthoredWork) {
    auto test = system.program6502();
    test.within(Budget).after(1, [](System&) {});
    const auto start = system.time();
    auto configuration = system.execution();
    configuration.within(Budget);
    EXPECT_THROW(test.run([](auto&) {}, configuration), std::logic_error);
    EXPECT_EQ(system.time(), start);
}

// Explicit result-free layouts share the builder's configured release contract.
TEST_F(SystemTest, ConfiguredProgramsExplicitScenarioNeedsNoCapturedData) {
    test_asm::Scenario scenario(system);
    const auto code = scenario.reserve("code", test_asm::RegionKind::Code, econopet::CpuAddress{0x300}, 0x100);
    scenario.reserve("vectors", test_asm::RegionKind::Vectors, pet::Reset6502, 2);
    const auto marker = scenario.completion(
        scenario.reserve("completion", test_asm::RegionKind::Mailbox, econopet::CpuAddress{0x2ff}, 1));
    test_asm::mos6502::Program program(scenario, code);
    program.finish(marker);
    auto run = system.execution();
    unsigned calls = 0;
    run.within(Budget).every(10, MissedService::Reject,
        [&](System&, Cycles) { ++calls; }, RunAnchor::CpuRelease);
    const auto result = scenario.run(program, run, "configured result-free scenario");
    EXPECT_TRUE(result.empty());
    EXPECT_EQ(result.completed(), system.time());
    EXPECT_GT(calls, 0);
}

// After-slice work starts after release and captures the same sampled completion boundary.
TEST_F(SystemTest, ConfiguredProgramsAfterSliceStartsOnlyAfterRelease) {
    auto test = system.program6809();
    const auto answer = test.byte("answer");
    std::optional<econopet::CycleTime> released;
    size_t calls = 0;
    const auto result = test.within(Budget)
        .after(0, [&](System& board) { released = board.time(); }, RunAnchor::CpuRelease)
        .every(7, ServiceTiming::AfterSlice, [&](System& board, Cycles remaining) {
            ASSERT_TRUE(released);
            EXPECT_FALSE(board.time() < *released + Cycles{7});
            EXPECT_FALSE(board.cpu().peek_reset());
            EXPECT_LE(remaining, Budget);
            ++calls;
            board.tick(1);
        }).run([&](auto& p) { p.lda(imm(0x64)).sta(answer); });
    EXPECT_GT(calls, 0u);
    EXPECT_EQ(result[answer], 0x64);
    EXPECT_EQ(result.completed(), system.time());
    EXPECT_EQ(result.window().completed(), system.time());
}

// A program's larger budget cannot exceed the active caller-owned deadline.
TEST_F(SystemTest, ConfiguredProgramsInheritEarliestDeadline) {
    auto test = system.program6502();
    const auto start = system.time();
    EXPECT_THROW(system.service_until([](const System&) { return false; },
        [&](System&, Cycles) { test.within(Budget).run([](auto&) {}); },
        2, "outer program deadline"), std::runtime_error);
    EXPECT_EQ(system.time(), start + Cycles{2});
}

// Owned completion windows contain real repeated I/O writes, not fixture initialization.
TEST_F(SystemTest, ConfiguredProgramsCaptureRepeatedPeripheralWritesAndThenPark) {
    auto model = system.trace_pia2_writes();
    auto bus = system.trace_peripheral_writes();
    const auto execute = [&](auto& test) {
        const auto result = test.within(Budget).run([](auto& p) {
            p.lda(imm(0x42)).sta(pet::Pia2PortA).sta(pet::Pia2PortA);
        });
        model.capture(result.window()).require_count(2, "CPU repeated PIA2 writes");
        const auto captured = bus.capture(result.window());
        captured.require_count(2, "physical repeated PIA2 writes");
        EXPECT_EQ(captured.records()[0].address, pet::Pia2PortA);
        EXPECT_EQ(captured.records()[1].address, pet::Pia2PortA);
        const auto parked = system.time();
        system.tick(2000);
        bus.capture(system.trace_window(parked, system.time())).require_absent("terminal park has no writes");
    };
    auto mos = system.program6502();
    execute(mos);
    auto mc = system.program6809();
    execute(mc);
}

// Peripheral model rejection must stop checked execution, not produce a result.
TEST_F(SystemTest, ConfiguredProgramsSurfaceUnsupportedPeripheralAccess) {
    System writer(system.seed());
    auto write = writer.program6502();
    expect_failure<std::logic_error>(writer, "I/O sample", [&] {
        write.within(Budget).run([](auto& p) {
            const auto unsupported = std::to_underlying(econopet::io::ViaAcrBit::ShiftBit0);
            p.lda(imm(unsupported)).sta(pet::ViaAcr);
        });
    }, "unsupported 6522");
    EXPECT_EQ(writer.io().via().writes().count(), 0u);
    EXPECT_THROW(write.run([](auto&) {}), std::logic_error);
    System reader(system.seed());
    auto read = reader.program6809();
    expect_failure<std::logic_error>(reader, "I/O", [&] {
        read.within(Budget).run([](auto& p) { p.lda(pet::ViaShift); });
    }, "shift-register read");
    EXPECT_EQ(reader.io().via().writes().count(), 0u);
}

// Forwarded polling work uses the same configuration budget, anchors and mandatory completion.
TEST_F(SystemTest, ConfiguredProgramsForwardServiceThroughOneExecutionPath) {
    const auto check = [&](auto& test) {
        const auto answer = test.byte("answer");
        const auto start = system.time();
        bool entry = false, released = false;
        size_t services = 0, predicates = 0;
        auto result = test.within(Budget)
            .after(0, [&](System& board) { entry = true; EXPECT_EQ(board.time(), start); })
            .after(0, [&](System& board) {
                released = true;
                EXPECT_FALSE(board.cpu().peek_reset());
            }, RunAnchor::CpuRelease)
            .until([&](const System&) { ++predicates; return true; })
            .run([&](auto& p) { p.lda(imm(0x42)).sta(answer); },
                [&](System& board, Cycles remaining) {
                    EXPECT_TRUE(entry);
                    EXPECT_TRUE(released);
                    EXPECT_FALSE(board.cpu().peek_reset());
                    EXPECT_EQ(remaining, Budget - (board.time() - start));
                    ++services;
                    board.tick(1);
                });
        static_assert(std::is_same_v<decltype(result), test_asm::ExecutionResult>);
        EXPECT_EQ(result[answer], 0x42);
        EXPECT_EQ(result.started(), start);
        EXPECT_EQ(result.completed(), system.time());
        EXPECT_GT(services, 0u);
        EXPECT_GT(predicates, 0u);
        EXPECT_THROW(test.run([](auto&) {}), std::logic_error);
    };
    auto mos = system.program6502();
    check(mos);
    auto mc = system.program6809();
    check(mc);
}

// Invalid service registration rejects before consuming assembly or changing the board.
TEST_F(SystemTest, ConfiguredProgramsForwardServiceValidationAndFailureRemainChecked) {
    auto test = system.program6502();
    test.within(Budget);
    bool assembled = false;
    const auto start = system.time();
    EXPECT_THROW(test.run([&](auto&) { assembled = true; }, RunConfiguration::Service{}),
        std::invalid_argument);
    EXPECT_FALSE(assembled);
    EXPECT_EQ(system.time(), start);
    auto scheduled = system.program6502();
    scheduled.within(Budget).every(1, MissedService::Coalesce, [](System&, Cycles) {}, RunAnchor::CpuRelease);
    EXPECT_THROW(scheduled.run([&](auto&) { assembled = true; }, [](System&, Cycles) {}), std::logic_error);
    EXPECT_FALSE(assembled);
    EXPECT_EQ(system.time(), start);
    struct Sentinel {};
    EXPECT_THROW(test.run([](auto&) {}, [](System&, Cycles) { throw Sentinel{}; }), Sentinel);
    EXPECT_THROW(test.run([](auto&) {}), std::logic_error);

    auto failed = system.program6502();
    bool fired = false;
    failed.within(Budget).after(1, [&](System&) { fired = true; throw Sentinel{}; }, RunAnchor::CpuRelease);
    EXPECT_THROW(failed.run([](auto&) {}, [](System& board, Cycles) {
        try { board.tick(2); } catch (const Sentinel&) {}
    }), std::logic_error);
    EXPECT_TRUE(fired);
}

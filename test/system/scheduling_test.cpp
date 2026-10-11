// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <optional>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "framework/system_test.h"
#include "scheduler.h"

using econopet::Cycles;
using econopet::CycleTime;

TEST_F(SystemTest, SchedulerOrdersStimulusBeforeCompletedCycleObservers) {
    Scheduler scheduler(system);
    const auto start = system.time();
    std::vector<int> order;
    scheduler.at(start + Cycles{2}, [&](System& board) {
        order.push_back(2);
        board.set_external_interrupts(true, false);
    });
    scheduler.at(start, [&](System&) { order.push_back(0); });
    scheduler.at(start + Cycles{2}, [&](System&) { order.push_back(3); });
    auto observer = system.observe([&](const System& board) {
        EXPECT_EQ(board.snapshot().stimulus.cpu_irq_n_i, !(board.time() > start + Cycles{2}));
    });
    system.tick(4);
    EXPECT_EQ(order, (std::vector<int>{0, 2, 3}));
    EXPECT_EQ(system.time(), start + Cycles{4});
}

TEST_F(SystemTest, SchedulerUsesHalfOpenStimulusIntervals) {
    Scheduler scheduler(system);
    const auto endpoint = system.time() + Cycles{3};
    unsigned count = 0;
    scheduler.at(endpoint, [&](System&) { ++count; });
    system.tick(3);
    system.tick(0);
    EXPECT_EQ(count, 0u);
    system.tick(1);
    EXPECT_EQ(count, 1u);
    system.tick(1);
    EXPECT_EQ(count, 1u);
}

TEST_F(SystemTest, SchedulerStimulusRunsDuringSpiWithoutAnotherClockOwner) {
    Scheduler scheduler(system);
    const auto at = system.time() + Cycles{7};
    std::optional<CycleTime> applied;
    scheduler.at(at, [&](System& board) {
        applied = board.time();
        board.io().pia1().inputs([](auto& inputs) { inputs.port_a = 0xc2; });
    });
    system.spi().write(fpga::address(econopet::SramAddress{0x200}), 0x5a);
    ASSERT_TRUE(applied);
    EXPECT_EQ(*applied, at);
    EXPECT_EQ(system.io().pia1().peek_inputs().port_a, 0xc2);
    EXPECT_EQ(system.peek(econopet::SramAddress{0x200}), 0x5a);
}

TEST_F(SystemTest, SchedulerValidatesOwnershipTimesAndPolicies) {
    Scheduler scheduler(system);
    System other;
    const auto start = system.time();
    EXPECT_THROW(Scheduler duplicate(system), std::logic_error);
    EXPECT_THROW(scheduler.at(start, {}), std::invalid_argument);
    EXPECT_THROW(scheduler.at(CycleTime{Cycles{0}}, [](System&) {}), std::invalid_argument);
    EXPECT_THROW(scheduler.every(start, 0, MissedService::Coalesce, [](System&, Cycles) {}), std::invalid_argument);
    EXPECT_THROW(scheduler.every(start, 1, static_cast<MissedService>(99), [](System&, Cycles) {}),
        std::invalid_argument);
    EXPECT_THROW(scheduler.every(start, 1, MissedService::Reject, {}), std::invalid_argument);
    EXPECT_THROW(scheduler.step(other, 1), std::invalid_argument);
    EXPECT_THROW(scheduler.step(system, 0), std::invalid_argument);
    EXPECT_EQ(system.time(), start);
}

TEST_F(SystemTest, SchedulerRejectsRecursiveActiveWorkAndRegistration) {
    Scheduler scheduler(system);
    const auto start = system.time();
    scheduler.at(start, [&](System& board) {
        EXPECT_THROW(board.tick(1), std::logic_error);
        EXPECT_THROW(board.run_until([](const System&) { return true; }, 0, "nested wait"), std::logic_error);
        EXPECT_THROW(board.spi().read(fpga::Register::CpuSelect), std::logic_error);
        EXPECT_THROW(board.drive_spi(false, true, true), std::logic_error);
        EXPECT_THROW(scheduler.at(start, [](System&) {}), std::logic_error);
        EXPECT_THROW(scheduler.step(board, 1), std::logic_error);
        board.set_external_interrupts(false, true);
    });
    system.tick(1);
    EXPECT_EQ(system.time(), start + Cycles{1});
    EXPECT_FALSE(system.snapshot().stimulus.cpu_nmi_n_i);
    EXPECT_FALSE(system.spi().peek_failed());
}

TEST_F(SystemTest, SchedulerFailsExplicitlyAndDetachesOnUnwind) {
    const auto start = system.time();
    {
        Scheduler scheduler(system);
        scheduler.at(start, [](System&) { throw std::runtime_error("stimulus failed"); });
        EXPECT_THROW(system.tick(1), std::runtime_error);
        EXPECT_FALSE(system.clock_faulted());
        EXPECT_THROW(system.tick(1), std::logic_error);
        EXPECT_THROW(scheduler.at(start, [](System&) {}), std::logic_error);
        EXPECT_EQ(system.time(), start);
    }
    Scheduler replacement(system);
    EXPECT_NO_THROW(system.tick(1));
}

TEST_F(SystemTest, SchedulerRejectsCaughtStimulusFailureBeforePollingCompletion) {
    Scheduler scheduler(system);
    const auto start = system.time();
    bool caught = false;
    bool accepted = false;
    scheduler.at(start + Cycles{3}, [](System&) { throw std::runtime_error("stimulus failed"); });
    scheduler.every(start + Cycles{2}, 2, MissedService::Reject, [&](System& board, Cycles) {
        try {
            board.tick(2);
        } catch (const std::runtime_error&) {
            caught = true;
        }
    });
    EXPECT_THROW(scheduler.run_until([&](const System&) {
        if (caught) accepted = true;
        return caught;
    }, 6, "caught scheduler failure"), std::logic_error);
    EXPECT_TRUE(caught);
    EXPECT_FALSE(accepted);
    EXPECT_EQ(system.time(), start + Cycles{3});
    EXPECT_THROW(scheduler.step(system, 1), std::logic_error);
}

TEST_F(SystemTest, SchedulerExhaustsUnrepresentableRecurrencesWithoutFailure) {
    for (const auto policy : {MissedService::Reject, MissedService::Coalesce}) {
        Scheduler scheduler(system);
        unsigned calls = 0;
        const auto start = system.time();
        scheduler.every(start, Cycles{Cycles::Maximum}, policy,
            [&](System&, Cycles) { ++calls; });
        EXPECT_NO_THROW(scheduler.step(system, 1));
        EXPECT_EQ(calls, 1u);
        EXPECT_EQ(system.time(), start);
        EXPECT_NO_THROW(scheduler.step(system, 1));
        EXPECT_EQ(calls, 1u);
        EXPECT_EQ(system.time(), start + Cycles{1});
    }
}

TEST_F(SystemTest, SchedulerKeepsTheLastRepresentableRecurrence) {
    for (const auto policy : {MissedService::Reject, MissedService::Coalesce}) {
        Scheduler scheduler(system);
        const auto start = system.time();
        unsigned calls = 0;
        scheduler.every(start, Cycles{Cycles::Maximum - start.value()}, policy,
            [&](System&, Cycles) { ++calls; });
        EXPECT_NO_THROW(scheduler.step(system, 1));
        EXPECT_NO_THROW(scheduler.step(system, 1));
        EXPECT_EQ(calls, 1u);
        EXPECT_EQ(system.time(), start + Cycles{1});
    }
}

TEST_F(SystemTest, SchedulerStrictMissRemainsFailedAfterCatch) {
    Scheduler scheduler(system);
    unsigned calls = 0;
    scheduler.every(system.time(), 2, MissedService::Reject, [&](System&, Cycles) { ++calls; });
    system.tick(1);
    EXPECT_THROW(scheduler.step(system, 2), std::runtime_error);
    EXPECT_THROW(scheduler.step(system, 2), std::logic_error);
    EXPECT_THROW(scheduler.run_until([](const System&) { return true; }, 0, "failed strict service"),
        std::logic_error);
    EXPECT_EQ(calls, 0u);
}

TEST_F(SystemTest, SchedulerCoalescesMissedServiceSlotsAndOrdersPeers) {
    Scheduler scheduler(system);
    const auto start = system.time();
    std::vector<int> calls;
    std::vector<CycleTime> times;
    scheduler.every(start, 3, MissedService::Coalesce, [&](System& board, Cycles) {
        calls.push_back(1);
        times.push_back(board.time());
        board.tick(7);
    });
    scheduler.every(start, 100, MissedService::Coalesce, [&](System&, Cycles) { calls.push_back(2); });
    scheduler.step(system, 20);
    scheduler.step(system, 20);
    EXPECT_EQ(calls, (std::vector<int>{1, 2}));
    system.tick(2);
    scheduler.step(system, 20);
    EXPECT_EQ(times, (std::vector<CycleTime>{start, start + Cycles{9}}));
}

TEST_F(SystemTest, SchedulerRejectsMissedServiceAndDoesNotRunIt) {
    Scheduler scheduler(system);
    unsigned calls = 0;
    scheduler.every(system.time() + Cycles{2}, 3, MissedService::Reject, [&](System&, Cycles) { ++calls; });
    system.tick(3);
    const auto before = system.time();
    EXPECT_THROW(scheduler.step(system, 10), std::runtime_error);
    EXPECT_EQ(calls, 0u);
    EXPECT_EQ(system.time(), before);
}

TEST_F(SystemTest, SchedulerStrictServiceDoesNotCoalesceItsOwnOverrun) {
    Scheduler scheduler(system);
    scheduler.every(system.time(), 2, MissedService::Reject, [](System& board, Cycles) { board.tick(3); });
    scheduler.step(system, 10);
    const auto before = system.time();
    EXPECT_THROW(scheduler.step(system, 10), std::runtime_error);
    EXPECT_EQ(system.time(), before);
}

TEST_F(SystemTest, SchedulerRunsZeroClockPeersAtTheirSharedDueTime) {
    Scheduler scheduler(system);
    const auto start = system.time();
    std::vector<int> calls;
    for (int index = 0; index < 3; ++index)
        scheduler.every(start, 2, MissedService::Reject, [&, index](System& board, Cycles) {
            EXPECT_EQ(board.time(), start);
            calls.push_back(index);
        });
    scheduler.run_until([&](const System&) { return calls.size() == 3; }, 1, "zero-clock peers");
    EXPECT_EQ(calls, (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(system.time(), start);
}

TEST_F(SystemTest, SchedulerDetachesPendingWorkAndRejectsObserverMutation) {
    const auto at = system.time() + Cycles{1};
    unsigned calls = 0;
    {
        Scheduler scheduler(system);
        scheduler.at(at, [&](System&) { ++calls; });
        auto observer = system.observe([&](const System&) {
            EXPECT_THROW(scheduler.at(at, [](System&) {}), std::logic_error);
        });
        system.tick(1);
    }
    system.tick(2);
    EXPECT_EQ(calls, 0u);
}

TEST_F(SystemTest, SchedulerCannotMutateCapturedActiveSpiTransaction) {
    Scheduler scheduler(system);
    auto transaction = system.spi().raw_transaction();
    scheduler.at(system.time(), [&](System&) {
        EXPECT_THROW(transaction.byte(0x42), std::logic_error);
        EXPECT_THROW(transaction.abort(), std::logic_error);
    });
    EXPECT_THROW(scheduler.step(system, 1), std::logic_error);
    system.tick(1);
    EXPECT_FALSE(system.snapshot().spi_cs_ni);
    EXPECT_FALSE(system.snapshot().spi_sck_i);
    transaction.abort();
    EXPECT_FALSE(system.spi().peek_failed());
}

TEST_F(SystemTest, SchedulerSharesDeadlinesAndRejectsServiceReentry) {
    Scheduler scheduler(system);
    const auto start = system.time();
    unsigned calls = 0;
    scheduler.every(start, 2, MissedService::Reject, [&](System& board, Cycles remaining) {
        ++calls;
        EXPECT_EQ(remaining, Cycles{5} - (board.time() - start));
        EXPECT_THROW(scheduler.step(board, remaining), std::logic_error);
    });
    scheduler.run_until([&](const System& board) { return board.time() == start + Cycles{5}; },
        5, "scheduled endpoint");
    EXPECT_EQ(calls, 3u);
    EXPECT_EQ(system.time(), start + Cycles{5});
}

TEST_F(SystemTest, SchedulerBoundsActiveServiceAndStopsAfterFailure) {
    Scheduler scheduler(system);
    const auto start = system.time();
    scheduler.every(start, 1, MissedService::Coalesce, [](System& board, Cycles) { board.tick(4); });
    EXPECT_THROW(scheduler.step(system, 3), std::runtime_error);
    EXPECT_EQ(system.time(), start);
    EXPECT_THROW(scheduler.step(system, 10), std::logic_error);
}

TEST_F(SystemTest, SchedulerPreservesSuspendedHardwarePhaseAndRequiresResetRecovery) {
    system.spi().write(fpga::Register::CpuControl, econopet::CpuControl{}.bits());
    Scheduler scheduler(system);
    unsigned applied = 0;
    scheduler.at(system.time(), [&](System& board) {
        ++applied;
        const econopet::CpuAddress shift{pet::ViaPortB.value()
            + std::to_underlying(econopet::io::ViaRegister::Shift)};
        board.drive_physical_cpu({shift, uint8_t{0x42}, false});
    });
    EXPECT_THROW(system.tick(1000), std::logic_error);
    ASSERT_TRUE(system.clock_faulted());
    const auto stopped = system.half_ticks();
    EXPECT_EQ(stopped % econopet::HalfTicksPerCycle, 1u);
    EXPECT_EQ(applied, 1u);
    EXPECT_THROW(scheduler.step(system, 1), std::logic_error);
    EXPECT_THROW(scheduler.at(system.time(), [](System&) {}), std::logic_error);
    EXPECT_EQ(system.half_ticks(), stopped);
    system.set_external_reset(true);
    system.tick(1);
    EXPECT_FALSE(system.clock_faulted());
    EXPECT_EQ(system.half_ticks(), stopped + 1);
    EXPECT_EQ(applied, 1u);
    scheduler.at(system.time(), [&](System&) { ++applied; });
    system.tick(1);
    EXPECT_EQ(applied, 2u);
}

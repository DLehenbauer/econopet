// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "framework/system_test.h"
#include "scheduler.h"

using econopet::Cycles;
using econopet::CycleTime;

namespace {
template<class Board>
concept TemporaryExecution = requires(Board board) { std::move(board).execution(); };
template<class Schedule>
concept PublicOrderedStimulus = requires(Schedule schedule, CycleTime time) {
    schedule.at_ordered(time, typename Schedule::Stimulus{}, 0);
};
template<class Schedule>
concept PublicOrderedService = requires(Schedule schedule, CycleTime time) {
    schedule.every_ordered(time, Cycles{1}, MissedService::Reject, typename Schedule::Service{}, 0);
};
static_assert(!TemporaryExecution<System>);
static_assert(!PublicOrderedStimulus<Scheduler>);
static_assert(!PublicOrderedService<Scheduler>);
static_assert(!std::is_copy_constructible_v<RunConfiguration>);
static_assert(!std::is_move_constructible_v<RunConfiguration>);
static_assert(!std::is_move_constructible_v<Scheduler>);
}

TEST_F(SystemTest, ConfiguredRunOrdersStimulusAndPhaseAnchoredServices) {
    const auto start = system.time();
    std::vector<int> order;
    std::vector<CycleTime> serviced;
    auto run = system.execution();
    run.within(7)
        .after(2, [&](System& board) { order.push_back(1); board.set_external_interrupts(true, false); })
        .after(2, [&](System&) { order.push_back(2); })
        .every(2, MissedService::Reject, [&](System& board, Cycles remaining) {
            EXPECT_FALSE(board.snapshot().stimulus.cpu_irq_n_i);
            EXPECT_EQ(remaining, Cycles{7} - (board.time() - start));
            order.push_back(3);
            serviced.push_back(board.time());
        })
        .until([&](const System&) { return serviced.size() == 3; }).run();
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3, 3, 3}));
    EXPECT_EQ(serviced, (std::vector<CycleTime>{start + Cycles{2}, start + Cycles{4}, start + Cycles{6}}));
    EXPECT_EQ(system.time(), start + Cycles{6});
}

TEST_F(SystemTest, ConfiguredRunFixedDurationAndInclusiveObservationDeadline) {
    const auto start = system.time();
    const auto window = system.execution().within(3).run();
    EXPECT_EQ(window.started(), start);
    EXPECT_EQ(window.completed(), start + Cycles{3});
    system.execution().within(2).until([&](const System& board) {
        return board.time() == start + Cycles{5};
    }).run();
    EXPECT_THROW(system.execution().within(2).until([](const System&) { return false; }).run(), std::runtime_error);
    EXPECT_EQ(system.time(), start + Cycles{7});
}

TEST_F(SystemTest, ConfiguredRunZeroBudgetAndPendingEndpointDoNotMutate) {
    const auto start = system.time();
    unsigned calls = 0;
    system.execution().within(0).after(0, [&](System&) { ++calls; }).run();
    system.execution().within(2).after(2, [&](System&) { ++calls; }).run();
    system.tick(1);
    EXPECT_EQ(calls, 0u);
    EXPECT_EQ(system.time(), start + Cycles{3});
    EXPECT_THROW(system.execution().within(0).until([](const System&) { return false; }).run(), std::runtime_error);
}

TEST_F(SystemTest, ConfiguredRunDistinguishesAfterSliceFromAnchoredWork) {
    const auto anchored = system.time();
    std::vector<Cycles> phase_times;
    system.execution().within(13).every(3, MissedService::Reject, [&](System& board, Cycles) {
        phase_times.push_back(board.time() - anchored);
        board.tick(1);
    }).run();
    const auto sliced = system.time();
    std::vector<Cycles> slice_times;
    system.execution().within(13).every(3, ServiceTiming::AfterSlice, [&](System& board, Cycles) {
        slice_times.push_back(board.time() - sliced);
        board.tick(1);
    }).run();
    EXPECT_EQ(phase_times, (std::vector<Cycles>{3, 6, 9, 12}));
    EXPECT_EQ(slice_times, (std::vector<Cycles>{3, 7, 11}));
    EXPECT_EQ(system.time(), sliced + Cycles{13});
}

TEST_F(SystemTest, ConfiguredRunPreservesSliceSamplingAndBoundaryStimulusOrder) {
    const auto start = system.time();
    bool changed = false;
    std::vector<int> order;
    system.execution().within(10)
        .after(3, [&](System&) { changed = true; order.push_back(1); })
        .every(3, ServiceTiming::AfterSlice, [&](System&, Cycles) {
            EXPECT_TRUE(changed);
            order.push_back(2);
        })
        .until([&](const System& board) { return !(board.time() < start + Cycles{5}); }).run();
    EXPECT_EQ(system.time(), start + Cycles{6});
    EXPECT_EQ(order, (std::vector<int>{1, 2, 2}));
}

TEST_F(SystemTest, ConfiguredRunBoundsPartialSlicesAndServiceOverrun) {
    unsigned calls = 0;
    const auto start = system.time();
    system.execution().within(2).every(3, ServiceTiming::AfterSlice, [&](System&, Cycles) { ++calls; }).run();
    EXPECT_EQ(calls, 0u);
    EXPECT_EQ(system.time(), start + Cycles{2});
    EXPECT_THROW(system.execution().within(3).every(2, ServiceTiming::AfterSlice,
        [](System& board, Cycles) { board.tick(2); }).run(), std::runtime_error);
    EXPECT_EQ(system.time(), start + Cycles{4});
}

TEST_F(SystemTest, ConfiguredRunCompleteFinalSliceServicesWithZeroBudget) {
    const auto start = system.time();
    std::vector<Cycles> budgets;
    system.execution().within(6).every(3, ServiceTiming::AfterSlice,
        [&](System& board, Cycles remaining) {
            budgets.push_back(remaining);
            if (remaining == Cycles{0}) {
                EXPECT_EQ(board.time(), start + Cycles{6});
                EXPECT_THROW(board.tick(1), std::runtime_error);
            }
        }).run();
    EXPECT_EQ(budgets, (std::vector<Cycles>{3, 0}));
    EXPECT_EQ(system.time(), start + Cycles{6});
}

TEST_F(SystemTest, ConfiguredRunRejectsOrCoalescesMissedSlots) {
    unsigned strict_calls = 0;
    EXPECT_THROW(system.execution().within(20).every(2, MissedService::Reject,
        [&](System& board, Cycles) { ++strict_calls; board.tick(5); }).run(), std::runtime_error);
    EXPECT_EQ(strict_calls, 1u);
    const auto start = system.time();
    std::vector<Cycles> calls;
    system.execution().within(20).every(2, MissedService::Coalesce,
        [&](System& board, Cycles) { calls.push_back(board.time() - start); board.tick(5); })
        .until([&](const System&) { return calls.size() == 2; }).run();
    EXPECT_EQ(calls, (std::vector<Cycles>{2, 8}));
}

TEST_F(SystemTest, ConfiguredRunValidatesPoliciesOwnershipAndSingleUse) {
    auto run = system.execution();
    const auto start = system.time();
    EXPECT_THROW(run.run(), std::logic_error);
    EXPECT_THROW(run.after(1, {}), std::invalid_argument);
    EXPECT_THROW(run.after(1, [](System&) {}, static_cast<RunAnchor>(99)), std::invalid_argument);
    EXPECT_THROW(run.every(0, MissedService::Reject, [](System&, Cycles) {}), std::invalid_argument);
    EXPECT_THROW(run.every(1, static_cast<MissedService>(99), [](System&, Cycles) {}), std::invalid_argument);
    EXPECT_THROW(run.every(1, ServiceTiming::AfterSlice, {}), std::invalid_argument);
    EXPECT_THROW(run.every(1, static_cast<ServiceTiming>(99), [](System&, Cycles) {}), std::invalid_argument);
    EXPECT_THROW(run.until({}), std::invalid_argument);
    EXPECT_EQ(system.time(), start);
    run.within(1).run();
    EXPECT_THROW(run.run(), std::logic_error);
    EXPECT_THROW(run.within(1), std::logic_error);
    EXPECT_THROW(run.after(1, [](System&) {}), std::logic_error);
    auto slice = system.execution();
    slice.every(1, ServiceTiming::AfterSlice, [](System&, Cycles) {});
    EXPECT_THROW(slice.every(1, ServiceTiming::AfterSlice, [](System&, Cycles) {}), std::logic_error);
    EXPECT_THROW(slice.every(1, MissedService::Reject, [](System&, Cycles) {}), std::logic_error);
    auto phase = system.execution();
    phase.every(1, MissedService::Reject, [](System&, Cycles) {});
    EXPECT_THROW(phase.every(1, ServiceTiming::AfterSlice, [](System&, Cycles) {}), std::logic_error);
    auto predicate = system.execution();
    predicate.until([](const System&) { return true; });
    EXPECT_THROW(predicate.until([](const System&) { return true; }), std::logic_error);
    System other;
    auto foreign = other.execution();
    foreign.within(1);
    EXPECT_THROW(foreign.validate(system, false), std::invalid_argument);
}

TEST_F(SystemTest, ConfiguredRunRequiresAnExplicitCpuReleaseLifecycle) {
    const auto start = system.time();
    EXPECT_THROW(system.execution().within(1).after(0, [](System&) {}, RunAnchor::CpuRelease).run(),
        std::invalid_argument);
    EXPECT_THROW(system.execution().within(1).every(1, MissedService::Reject,
        [](System&, Cycles) {}, RunAnchor::CpuRelease).run(), std::invalid_argument);
    EXPECT_EQ(system.time(), start);
}

TEST_F(SystemTest, ConfiguredRunRejectsPredicateMutationAndRecursiveRuns) {
    auto run = system.execution();
    run.within(1).until([&](const System&) {
        EXPECT_THROW(system.tick(1), std::logic_error);
        EXPECT_THROW(run.after(0, [](System&) {}), std::logic_error);
        EXPECT_THROW(system.execution().within(1).run(), std::logic_error);
        return true;
    }).run();
    system.execution().within(3).every(1, MissedService::Reject, [&](System&, Cycles) {
        EXPECT_THROW(system.execution().within(1).run(), std::logic_error);
    }).run();
}

TEST_F(SystemTest, ConfiguredRunDetachesOnCallbackFailureAndRejectsCompetingSchedulers) {
    struct Sentinel {};
    unsigned calls = 0;
    auto run = system.execution();
    EXPECT_THROW(run.within(3).after(1, [&](System&) { ++calls; throw Sentinel{}; }).run(), Sentinel);
    EXPECT_EQ(calls, 1u);
    EXPECT_THROW(run.run(), std::logic_error);
    EXPECT_NO_THROW(system.execution().within(1).run());
    Scheduler owner(system);
    const auto start = system.time();
    EXPECT_THROW(system.execution().within(1).run(), std::logic_error);
    EXPECT_EQ(system.time(), start);
}

TEST_F(SystemTest, ConfiguredRunInheritsTheEarliestDeadline) {
    const auto start = system.time();
    EXPECT_THROW(system.service_until([](const System&) { return false; },
        [](System& board, Cycles) { board.execution().within(100).run(); }, 2, "outer deadline"),
        std::runtime_error);
    EXPECT_EQ(system.time(), start + Cycles{2});
}

TEST_F(SystemTest, ConfiguredRunProducesCoveredTraceWindow) {
    auto trace = system.trace_av(test_observation::AvSampling::EveryCycle);
    const auto window = system.execution().within(5).run();
    trace.capture(window).require_count(5, "configured interval");
    EXPECT_EQ(window.completed() - window.started(), Cycles{5});
}

TEST_F(SystemTest, ConfiguredRunReleaseHookInstallsExplicitLifecycleAnchors) {
    auto run = system.execution();
    const auto entry = system.time();
    std::optional<CycleTime> release_time;
    std::vector<int> events;
    std::vector<CycleTime> services;
    run.within(10000)
        .after(0, [&](System& board) { EXPECT_EQ(board.time(), entry); events.push_back(0); })
        .after(0, [&](System& board) { EXPECT_EQ(board.time(), *release_time); events.push_back(1); },
            RunAnchor::CpuRelease)
        .after(0, [&](System&) { events.push_back(2); }, RunAnchor::CpuRelease)
        .every(3, MissedService::Reject, [&](System& board, Cycles) { services.push_back(board.time()); },
            RunAnchor::CpuRelease);
    const auto window = run.execute(system, "explicit CPU release lifecycle", true,
        [&](Cycles budget, auto service, auto release, auto done) {
            // Establish real CPU release using the existing lifecycle, without a program builder.
            system.cpu(CPU_SOFT_6502).prepare();
            system.cpu().release_reset();
            release_time = system.time();
            release();
            EXPECT_THROW(release(), std::logic_error);
            const auto end = system.time() + Cycles{7};
            system.service_until([&](const System& board) { return board.time() == end && done(board); },
                service, budget, "anchored services");
            return system.trace_window(entry, system.time());
        });
    EXPECT_EQ(events, (std::vector<int>{0, 1, 2}));
    ASSERT_TRUE(release_time);
    EXPECT_EQ(services, (std::vector<CycleTime>{*release_time + Cycles{3}, *release_time + Cycles{6}}));
    EXPECT_EQ(window.started(), entry);
    EXPECT_EQ(window.completed(), *release_time + Cycles{7});
}

TEST_F(SystemTest, ConfiguredRunFailedSetupConsumesWithoutLeakingItsScheduler) {
    auto run = system.execution();
    run.within(1).after(Cycles{Cycles::Maximum}, [](System&) {});
    const auto start = system.time();
    EXPECT_THROW(run.run(), std::overflow_error);
    EXPECT_EQ(system.time(), start);
    EXPECT_THROW(run.within(1), std::logic_error);
    EXPECT_NO_THROW(system.execution().within(1).run());
}

TEST_F(SystemTest, ConfiguredRunRejectsCaughtStimulusFailuresInBothServiceModes) {
    for (const bool after_slice : {false, true}) {
        const auto start = system.time();
        bool caught = false;
        auto run = system.execution();
        run.within(6).after(3, [](System&) { throw std::runtime_error("stimulus failed"); });
        const auto work = [&](System& board, Cycles) {
            try {
                board.tick(2);
            } catch (const std::runtime_error&) {
                caught = true;
            }
        };
        if (after_slice) run.every(2, ServiceTiming::AfterSlice, work);
        else run.every(2, MissedService::Reject, work);
        run.until([&](const System&) { return caught; });
        EXPECT_THROW(run.run(), std::logic_error);
        EXPECT_TRUE(caught);
        EXPECT_EQ(system.time(), start + Cycles{3});
        EXPECT_FALSE(system.clock_faulted());
        EXPECT_THROW(run.run(), std::logic_error);
        EXPECT_NO_THROW(system.execution().within(1).run());
    }
}

TEST_F(SystemTest, ConfiguredRunRequiresReleaseBeforeSuccessfulLifecycleReturn) {
    unsigned calls = 0;
    auto value_run = system.execution();
    value_run.within(1).after(0, [&](System&) { ++calls; }, RunAnchor::CpuRelease);
    EXPECT_THROW(value_run.execute(system, "missing release value", true,
        [](Cycles, auto, auto, auto) { return 42; }), std::logic_error);
    auto void_run = system.execution();
    void_run.within(1).every(1, MissedService::Reject, [&](System&, Cycles) { ++calls; },
        RunAnchor::CpuRelease);
    EXPECT_THROW(void_run.execute(system, "missing release void", true,
        [](Cycles, auto, auto, auto) {}), std::logic_error);
    auto empty_run = system.execution();
    empty_run.within(0);
    EXPECT_THROW(empty_run.execute(system, "missing release without anchored work", true,
        [](Cycles, auto, auto, auto) {}), std::logic_error);
    EXPECT_EQ(calls, 0u);
    EXPECT_THROW(value_run.run(), std::logic_error);
    EXPECT_THROW(void_run.run(), std::logic_error);
    EXPECT_THROW(empty_run.run(), std::logic_error);
    EXPECT_NO_THROW(system.execution().within(1).run());
}

TEST_F(SystemTest, ConfiguredRunChecksHealthEvenWhenLifecycleCatchesServiceFailure) {
    bool caught = false;
    auto run = system.execution();
    run.within(6).after(1, [](System&) { throw std::runtime_error("stimulus failed"); });
    EXPECT_THROW(run.execute(system, "caught lifecycle failure", true,
        [&](Cycles budget, auto service, auto release, auto) {
            release();
            service(system, budget);
            try {
                service(system, budget - Cycles{1});
            } catch (const std::runtime_error&) {
                caught = true;
            }
            return true;
        }), std::logic_error);
    EXPECT_TRUE(caught);
    EXPECT_NO_THROW(system.execution().within(1).run());
}

TEST_F(SystemTest, ConfiguredRunPreservesSuccessfulLifecycleResultTypes) {
    auto value_run = system.execution();
    value_run.within(0);
    const auto value = value_run.execute(system, "move-only result", true,
        [](Cycles, auto, auto release, auto) {
            release();
            return std::make_unique<int>(42);
        });
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 42);
    auto reference_run = system.execution();
    reference_run.within(0);
    int expected = 7;
    int& result = reference_run.execute(system, "reference result", true,
        [&](Cycles, auto, auto release, auto) -> int& {
            release();
            return expected;
        });
    EXPECT_EQ(&result, &expected);
    auto void_run = system.execution();
    void_run.within(0);
    EXPECT_NO_THROW(void_run.execute(system, "void result", true,
        [](Cycles, auto, auto release, auto) { release(); }));
    struct Sentinel {};
    auto failed_run = system.execution();
    failed_run.within(0);
    EXPECT_THROW(failed_run.execute(system, "exception before release", true,
        [](Cycles, auto, auto, auto) { throw Sentinel{}; }), Sentinel);
}

TEST_F(SystemTest, ConfiguredRunRetainsAuthorOrderAcrossDifferentAnchors) {
    const auto start = system.time();
    std::vector<int> events;
    std::vector<int> services;
    auto run = system.execution();
    run.within(6)
        .after(0, [&](System&) { events.push_back(1); }, RunAnchor::CpuRelease)
        .after(2, [&](System&) { events.push_back(2); })
        .every(2, MissedService::Reject, [&](System&, Cycles) { services.push_back(1); },
            RunAnchor::CpuRelease)
        .every(4, MissedService::Reject, [&](System&, Cycles) { services.push_back(2); });
    run.execute(system, "cross-anchor author ordering", true,
        [&](Cycles budget, auto service, auto release, auto done) {
            // Hold the release anchor until entry-relative work has reached its endpoint.
            system.tick(2);
            release();
            system.service_until([&](const System& board) {
                return board.time() == start + Cycles{5} && done(board);
            }, service, budget, "cross-anchor services");
        });
    EXPECT_EQ(events, (std::vector<int>{1, 2}));
    EXPECT_EQ(services, (std::vector<int>{1, 2}));
}

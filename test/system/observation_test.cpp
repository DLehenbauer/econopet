// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "system.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "framework/system_test.h"

namespace {
using namespace econopet;
using namespace econopet::io;

constexpr Cycles DeadlineCycles{7};
constexpr Cycles IndependentCycles{3};
constexpr Cycles ObserverCycles{5};
constexpr SramAddress ProbeAddress{0x0400};
constexpr uint8_t Marker = 0x42;
constexpr size_t RecentEventLimit = 16;
constexpr unsigned SeededIterations = 12;
constexpr uint32_t SeededBudgetRange = 17;

struct UnwindSentinel {};

static_assert(!std::is_copy_constructible_v<System::ObserverSubscription>);
static_assert(!std::is_copy_assignable_v<System::ObserverSubscription>);
static_assert(std::is_nothrow_move_constructible_v<System::ObserverSubscription>);
static_assert(std::is_nothrow_move_assignable_v<System::ObserverSubscription>);
static_assert(std::is_nothrow_destructible_v<System::ObserverSubscription>);

// Check the exception category, operation and live evidence without clocking.
template<class Exception, class Operation>
void expect_failure(System& system, const std::string& operation, Operation action,
                    const std::string& detail = "") {
    try {
        action();
        FAIL() << "expected " << operation << " to fail";
    } catch (const Exception& error) {
        const std::string message = error.what();
        for (const auto& field : {operation, detail, std::string("cycle="),
             std::string("half_tick="), std::string("cpu="),
             "seed=" + std::to_string(system.seed()), std::string("recent={")})
            EXPECT_NE(message.find(field), std::string::npos) << message;
    }
}
}

TEST_F(SystemTest, BoundedWaitReportsTimeoutAndDoesNotOvershoot) {
    const auto start = system.time();
    expect_failure<std::runtime_error>(system, "deadline", [&] {
        system.run_until([](const System&) { return false; }, DeadlineCycles, "test deadline");
    }, "test deadline timed out");
    EXPECT_EQ(system.time() - start, DeadlineCycles);
    system.run_until([](const System&) { return true; }, 0, "already complete");
    EXPECT_EQ(system.time() - start, DeadlineCycles);
}

TEST_F(SystemTest, CompletionPredicatesRejectActiveWorkBeforeMutation) {
    const auto start = system.time();
    const auto marker = system.peek(ProbeAddress);
    for (auto budget : {Cycles{0}, DeadlineCycles}) {
        EXPECT_THROW(system.run_until([&](const System&) {
            system.tick(1);
            return true;
        }, budget, "captured tick"), std::logic_error);
        EXPECT_THROW(system.run_until([&](const System&) {
            system.poke(ProbeAddress, Marker);
            return true;
        }, budget, "captured mutation"), std::logic_error);
    }
    EXPECT_EQ(system.time(), start);
    EXPECT_EQ(system.peek(ProbeAddress), marker);
    EXPECT_NO_THROW(system.tick(1));
}

TEST_F(SystemTest, ObservationWaitIncludesFinalCycleAndRestoresOnException) {
    const auto start = system.time();
    unsigned polls = 0;
    system.run_until([&](const System& observed) {
        ++polls;
        return observed.time() == start + DeadlineCycles;
    }, DeadlineCycles, "final cycle");
    EXPECT_EQ(system.time() - start, DeadlineCycles);
    EXPECT_EQ(polls, DeadlineCycles.value() + 1);
    EXPECT_THROW(system.run_until([](const System&) -> bool {
        throw UnwindSentinel{};
    }, DeadlineCycles, "throwing predicate"), UnwindSentinel);
    EXPECT_NO_THROW(system.tick(IndependentCycles));
    EXPECT_EQ(system.time() - start, DeadlineCycles + IndependentCycles);
}

TEST_F(SystemTest, ClockObserversRejectReentrantAdvancement) {
    const auto start = system.time();
    auto subscription = system.observe([&](const System&) { system.tick(1); });
    EXPECT_THROW(system.tick(ObserverCycles), std::logic_error);
    EXPECT_EQ(system.time() - start, Cycles{1});
    EXPECT_FALSE(system.clock_faulted());
    subscription.reset();
    EXPECT_NO_THROW(system.tick(1));
}

TEST_F(SystemTest, ActiveServiceUsesRemainingCyclesAndHandlesZeroProgress) {
    const auto start = system.time();
    unsigned passes = 0;
    system.service_until([&](const System& observed) {
        return observed.time() == start + DeadlineCycles;
    }, [&](System& active, Cycles remaining) {
        EXPECT_EQ(remaining, DeadlineCycles - (active.time() - start));
        ++passes;
        active.tick(1);
    }, DeadlineCycles, "active final cycle");
    EXPECT_EQ(passes, DeadlineCycles.value());
    const auto idle_start = system.time();
    passes = 0;
    system.service_until([&](const System& observed) {
        return observed.time() == idle_start + DeadlineCycles;
    }, [&](System&, Cycles) { ++passes; }, DeadlineCycles, "idle service");
    EXPECT_EQ(passes, DeadlineCycles.value());
    EXPECT_EQ(system.time() - idle_start, DeadlineCycles);
    const auto synchronous_start = system.time();
    bool complete = false;
    system.service_until([&](const System&) { return complete; },
        [&](System&, Cycles) { complete = true; }, DeadlineCycles, "synchronous completion");
    EXPECT_EQ(system.time(), synchronous_start);
    system.service_until([](const System&) { return true; },
        [](System&, Cycles) { FAIL() << "already complete service ran"; }, 0, "already complete");
    EXPECT_THROW(system.service_until([](const System&) { return false; },
        [](System&, Cycles) { FAIL() << "zero-budget service ran"; }, 0, "zero budget"), std::runtime_error);
}

TEST_F(SystemTest, ActiveServiceCannotOvershootAndRestoresAfterExceptions) {
    const auto start = system.time();
    EXPECT_THROW(system.service_until([](const System&) { return false; },
        [](System& active, Cycles remaining) { active.tick(remaining + Cycles{1}); },
        DeadlineCycles, "oversized work"), std::runtime_error);
    EXPECT_EQ(system.time(), start);
    EXPECT_FALSE(system.clock_faulted());
    EXPECT_THROW(system.service_until([](const System&) { return false; },
        [](System& active, Cycles) {
            active.tick(1);
            throw UnwindSentinel{};
        }, DeadlineCycles, "throwing work"), UnwindSentinel);
    EXPECT_EQ(system.time() - start, Cycles{1});
    EXPECT_NO_THROW(system.tick(IndependentCycles));
    unsigned predicates = 0;
    EXPECT_THROW(system.run_until([&](const System&) { ++predicates; return true; },
        Cycles{Cycles::Maximum}, "overflow"), std::overflow_error);
    EXPECT_EQ(predicates, 0u);
}

TEST_F(SystemTest, NestedWaitsCannotExtendActiveDeadline) {
    const auto start = system.time();
    expect_failure<std::runtime_error>(system, "deadline", [&] {
        system.service_until([](const System&) { return false; },
            [](System& active, Cycles) {
                active.tick(1);
                active.run_until([](const System&) { return false; },
                    Cycles{Cycles::Maximum}, "inner wait");
            }, DeadlineCycles, "outer wait");
    }, "outer wait timed out");
    EXPECT_EQ(system.time() - start, DeadlineCycles);
    EXPECT_NO_THROW(system.tick(IndependentCycles));
}

TEST_F(SystemTest, NestedDeadlineRestoresEnclosingBudget) {
    const auto start = system.time();
    bool complete = false;
    system.service_until([&](const System&) { return complete; },
        [&](System& active, Cycles) {
            EXPECT_THROW(active.run_until([](const System&) { return false; },
                2, "short inner wait"), std::runtime_error);
            active.tick(DeadlineCycles - Cycles{2});
            complete = true;
        }, DeadlineCycles, "outer work");
    EXPECT_EQ(system.time() - start, DeadlineCycles);
}

TEST_F(SystemTest, ObserverSamplesEveryCycleWithoutAdvancingTime) {
    unsigned samples = 0;
    const auto start = system.time();
    auto subscription = system.observe([&](const System& observed) {
        ++samples;
        EXPECT_EQ(observed.time(), start + Cycles{samples});
    });
    system.tick(ObserverCycles);
    EXPECT_EQ(samples, ObserverCycles.value());
    subscription.reset();
    system.tick(1);
    EXPECT_EQ(samples, ObserverCycles.value());
}

TEST_F(SystemTest, ObserverSubscriptionsComposeInRegistrationOrder) {
    std::vector<unsigned> calls;
    auto first = system.observe([&](const System&) { calls.push_back(1); });
    auto second = system.observe([&](const System&) { calls.push_back(2); });
    system.tick(0);
    EXPECT_TRUE(calls.empty());
    system.tick(2);
    EXPECT_EQ(calls, (std::vector<unsigned>{1, 2, 1, 2}));
    calls.clear();
    {
        auto nested = system.observe([&](const System&) { calls.push_back(3); });
        system.tick(1);
        EXPECT_EQ(calls, (std::vector<unsigned>{1, 2, 3}));
    }
    calls.clear();
    system.tick(1);
    EXPECT_EQ(calls, (std::vector<unsigned>{1, 2}));
    first.reset();
    first.reset();
    calls.clear();
    auto last = system.observe([&](const System&) { calls.push_back(4); });
    system.tick(1);
    EXPECT_EQ(calls, (std::vector<unsigned>{2, 4}));
    EXPECT_FALSE(first.connected());
    EXPECT_TRUE(second.connected());
    EXPECT_TRUE(last.connected());
}

TEST_F(SystemTest, ObserverSubscriptionDisconnectsAndReleasesCapturesOnUnwind) {
    unsigned calls = 0;
    std::weak_ptr<int> captured;
    bool setup_complete = false;
    EXPECT_THROW({
        auto state = std::make_shared<int>(Marker);
        captured = state;
        auto subscription = system.observe([state, &calls](const System&) {
            EXPECT_EQ(*state, Marker);
            ++calls;
        });
        state.reset();
        EXPECT_FALSE(captured.expired());
        system.tick(1);
        setup_complete = true;
        throw UnwindSentinel{};
    }, UnwindSentinel);
    ASSERT_TRUE(setup_complete);
    EXPECT_TRUE(captured.expired());
    system.tick(1);
    EXPECT_EQ(calls, 1u);
}

TEST_F(SystemTest, ObserverSubscriptionMovesWithoutDuplicatingCallbacks) {
    unsigned first_calls = 0;
    unsigned second_calls = 0;
    auto first = system.observe([&](const System&) { ++first_calls; });
    auto moved = std::move(first);
    EXPECT_FALSE(first.connected());
    EXPECT_TRUE(moved.connected());
    system.tick(1);
    EXPECT_EQ(first_calls, 1u);
    auto second = system.observe([&](const System&) { ++second_calls; });
    moved = std::move(second);
    EXPECT_FALSE(second.connected());
    EXPECT_TRUE(moved.connected());
    system.tick(1);
    EXPECT_EQ(first_calls, 1u);
    EXPECT_EQ(second_calls, 1u);
    moved.reset();
    system.tick(1);
    EXPECT_EQ(first_calls, 1u);
    EXPECT_EQ(second_calls, 1u);
}

TEST_F(SystemTest, ObserverSubscriptionSafelyOutlivesItsBoard) {
    System::ObserverSubscription subscription;
    unsigned calls = 0;
    {
        System other;
        subscription = other.observe([&](const System&) { ++calls; });
        EXPECT_TRUE(subscription.connected());
        other.tick(1);
    }
    EXPECT_FALSE(subscription.connected());
    subscription.reset();
    system.tick(1);
    EXPECT_EQ(calls, 1u);
}

TEST_F(SystemTest, ObserverDisconnectsTakeEffectDuringDispatch) {
    System::ObserverSubscription first;
    System::ObserverSubscription second;
    unsigned first_calls = 0;
    unsigned second_calls = 0;
    unsigned last_calls = 0;
    auto state = std::make_shared<int>(Marker);
    std::weak_ptr<int> captured = state;
    first = system.observe([state, &captured, &first, &second, &first_calls](const System&) {
        first.reset();
        second.reset();
        EXPECT_FALSE(captured.expired());
        EXPECT_EQ(*state, Marker);
        ++first_calls;
    });
    state.reset();
    second = system.observe([&](const System&) { ++second_calls; });
    auto last = system.observe([&](const System&) { ++last_calls; });
    system.tick(1);
    EXPECT_TRUE(captured.expired());
    EXPECT_EQ(first_calls, 1u);
    EXPECT_EQ(second_calls, 0u);
    EXPECT_EQ(last_calls, 1u);
    system.tick(1);
    EXPECT_EQ(first_calls, 1u);
    EXPECT_EQ(second_calls, 0u);
    EXPECT_EQ(last_calls, 2u);
}

TEST_F(SystemTest, ObserverExceptionsStopDispatchAndRestoreNonReentrantGuard) {
    std::vector<unsigned> calls;
    auto first = system.observe([&](const System&) { calls.push_back(1); });
    auto throwing = system.observe([&](const System&) {
        calls.push_back(2);
        throw UnwindSentinel{};
    });
    auto last = system.observe([&](const System&) { calls.push_back(3); });
    const auto start = system.time();
    EXPECT_THROW(system.tick(ObserverCycles), UnwindSentinel);
    EXPECT_EQ(system.time() - start, Cycles{1});
    EXPECT_FALSE(system.clock_faulted());
    EXPECT_EQ(calls, (std::vector<unsigned>{1, 2}));
    EXPECT_TRUE(first.connected());
    EXPECT_TRUE(throwing.connected());
    EXPECT_TRUE(last.connected());
    throwing.reset();
    calls.clear();
    EXPECT_NO_THROW(system.tick(2));
    EXPECT_EQ(calls, (std::vector<unsigned>{1, 3, 1, 3}));
}

TEST_F(SystemTest, ObserverDispatchRejectsActiveWorkAndRegistration) {
    auto& pia = system.io().pia1();
    auto& via = system.io().via();
    unsigned calls = 0;
    auto subscription = system.observe([&](const System& observed) {
        const auto start = observed.time();
        expect_failure<std::logic_error>(system, "observation", [&] { system.tick(0); });
        EXPECT_THROW(system.tick(1), std::logic_error);
        EXPECT_THROW(system.drive_spi(false, true, true), std::logic_error);
        EXPECT_THROW(system.set_external_reset(true), std::logic_error);
        EXPECT_THROW(system.set_external_interrupts(true, true), std::logic_error);
        EXPECT_THROW(system.drive_physical_cpu({CpuAddress{1}, Marker}), std::logic_error);
        EXPECT_THROW(system.set_display(pet_video_type_fixed), std::logic_error);
        EXPECT_THROW(system.set_keyboard(pet_keyboard_model_graphics), std::logic_error);
        EXPECT_THROW(system.poke(ProbeAddress, Marker), std::logic_error);
        EXPECT_THROW(system.io(), std::logic_error);
        EXPECT_THROW(system.raw_stimulus([](auto&) {}), std::logic_error);
        EXPECT_THROW(system.run_until([](const System&) { return true; }, 0, "nested"), std::logic_error);
        EXPECT_THROW({ auto nested = system.observe([](const System&) {}); }, std::logic_error);
        EXPECT_THROW(pia.inputs([](auto& inputs) { inputs.port_a = 0; }), std::logic_error);
        EXPECT_THROW(via.inputs([](auto& inputs) { inputs.ca1 = false; }), std::logic_error);
        EXPECT_THROW(pia.clock(), std::logic_error);
        EXPECT_THROW(pia.reset(), std::logic_error);
        EXPECT_THROW(pia.clear_observations(), std::logic_error);
        EXPECT_THROW(via.set_timer1_fault(Via6522::Timer1Fault::StuckInterruptFlag), std::logic_error);
        EXPECT_EQ(observed.io().pia1().peek_inputs().port_a, PortAllHigh);
        EXPECT_EQ(observed.time(), start);
        EXPECT_TRUE(observed.snapshot().spi_cs_ni);
        ++calls;
    });
    EXPECT_THROW({ auto invalid = system.observe({}); }, std::invalid_argument);
    system.tick(ObserverCycles);
    EXPECT_EQ(calls, ObserverCycles.value());
}

TEST_F(SystemTest, FailureContractObserverSentinelRecursionAndLifetime) {
    unsigned calls = 0;
    std::weak_ptr<unsigned> retained;
    EXPECT_THROW({
        auto capture = std::make_shared<unsigned>(0);
        retained = capture;
        auto subscription = system.observe([&, capture](const System&) {
            ++calls;
            ++*capture;
            expect_failure<std::logic_error>(system, "observation", [&] { system.tick(1); });
            throw UnwindSentinel{};
        });
        capture.reset();
        system.tick(2);
    }, UnwindSentinel);
    EXPECT_TRUE(retained.expired());
    EXPECT_EQ(calls, 1u);
    EXPECT_NO_THROW(system.tick(2));
    EXPECT_EQ(calls, 1u);
}

TEST_F(SystemTest, FailureContractDeadlineDiagnosticsAndSeededBoundaries) {
    std::mt19937 random(system.seed());
    for (unsigned iteration = 0; iteration < SeededIterations; ++iteration) {
        const Cycles budget{iteration == 0 ? 0 : random() % SeededBudgetRange};
        SCOPED_TRACE(budget.value());
        auto start = system.time();
        system.run_until([&](const System& observed) { return observed.time() == start + budget; },
            budget, "inclusive boundary");
        EXPECT_EQ(system.time(), start + budget);
        start = system.time();
        expect_failure<std::runtime_error>(system, "deadline", [&] {
            system.run_until([&](const System& observed) {
                return observed.time() == start + budget + Cycles{1};
            }, budget, "one cycle too late");
        }, "one cycle too late timed out");
        EXPECT_EQ(system.time(), start + budget);
    }
}

TEST_F(SystemTest, FailureContractDiagnosticsAreReadOnlyAndBoundRecentEvents) {
    const auto start = system.time();
    for (unsigned index = 0; index < 40; ++index) {
        system.set_external_interrupts(index % 2, index % 3);
        system.tick(1);
    }
    const auto before = system.half_ticks();
    const auto pins = system.snapshot();
    const auto message = system.diagnostic("test operation", "requested address=0xfffff");
    const auto recent = message.substr(message.find("recent={"));
    EXPECT_EQ(std::count(recent.begin(), recent.end(), ':'), RecentEventLimit);
    EXPECT_EQ(recent.find("board initialized"), std::string::npos);
    EXPECT_NE(recent.find(std::to_string(start.value() + 24) + ":external interrupts"), std::string::npos);
    EXPECT_NE(recent.find(std::to_string(start.value() + 39) + ":external interrupts"), std::string::npos);
    EXPECT_EQ(system.half_ticks(), before);
    EXPECT_EQ(system.snapshot().cpu_addr_o, pins.cpu_addr_o);
    unsigned callbacks = 0;
    auto subscription = system.observe([&](const System& observed) {
        ++callbacks;
        EXPECT_NE(observed.diagnostic("observer", "read-only").find("recent={"), std::string::npos);
    });
    system.tick(1);
    EXPECT_EQ(callbacks, 1u);
    EXPECT_EQ(system.half_ticks(), before + HalfTicksPerCycle);
}

TEST(SimulationSeed, RejectsInvalidSeedsAndPoliciesBeforeModelConstruction) {
    for (const auto text : {"", "0", "-1", "+1", " 1", "1 ", "1x", "2147483648", "4294967296"})
        EXPECT_THROW(System::parse_seed(text), std::invalid_argument) << text;
    EXPECT_THROW(System::parse_seed(std::string_view{}), std::invalid_argument);
    EXPECT_EQ(System::parse_seed("1"), 1u);
    EXPECT_EQ(System::parse_seed("2147483647"), 2147483647u);
    EXPECT_THROW(System(0), std::invalid_argument);
    EXPECT_THROW(System(std::numeric_limits<uint32_t>::max()), std::invalid_argument);
    EXPECT_THROW(System(1, static_cast<System::InitialState>(99)), std::invalid_argument);
}

// Raw edits retain the changed pin and value, not an unrelated unchanged CPU.
TEST_F(SystemTest, RawStimulusHistoryRecordsChangedInputsAndIgnoresDiscardedEdits) {
    const auto start = system.half_ticks();
    for (size_t index = 0; index < RecentEventLimit + 1; ++index) {
        system.raw_stimulus([](auto& inputs) { inputs.diag_i = !inputs.diag_i; });
    }
    const auto message = system.diagnostic("raw edit evidence", "");
    const auto recent = message.substr(message.find("recent={"));
    EXPECT_EQ(std::count(recent.begin(), recent.end(), ':'), RecentEventLimit);
    EXPECT_NE(recent.find(":diag_i(0x0,0x0)"), std::string::npos);
    EXPECT_NE(recent.find(":diag_i(0x0,0x1)"), std::string::npos);
    EXPECT_EQ(recent.find("physical CPU"), std::string::npos);
    system.raw_stimulus([](auto&) {});
    EXPECT_EQ(system.diagnostic("raw edit evidence", ""), message);
    EXPECT_THROW(system.raw_stimulus([](auto& inputs) {
        inputs.audio_det_i = true;
        throw UnwindSentinel{};
    }), UnwindSentinel);
    EXPECT_EQ(system.diagnostic("raw edit evidence", ""), message);
    system.raw_stimulus([](auto& inputs) {
        inputs.audio_det_i = true;
        inputs.pmod1_i = Marker;
        inputs.spare[2] = true;
        inputs.cpu.address = CpuAddress{ProbeAddress.value()};
        inputs.cpu.write_data = Marker;
        inputs.cpu.sync = true;
    });
    const auto changed = system.diagnostic("raw edit evidence", "");
    for (const auto field : {"audio_det_i(0x0,0x1)", "pmod1_i(0x0,0x42)",
         "spare(0x2,0x1)", "CPU address(0x400,0x0)", "CPU write data(0x0,0x42)",
         "CPU write enable(0x0,0x1)", "CPU sync(0x0,0x1)"})
        EXPECT_NE(changed.find(field), std::string::npos) << changed;
    system.raw_stimulus([](auto& inputs) { inputs.cpu.write_data.reset(); });
    const auto released = system.diagnostic("raw edit evidence", "");
    EXPECT_NE(released.find("CPU write enable(0x0,0x0)"), std::string::npos);
    EXPECT_EQ(system.peek_stimulus().cpu.write_data, std::nullopt);
    EXPECT_EQ(system.snapshot().stimulus.cpu.address, CpuAddress{ProbeAddress.value()});
    EXPECT_EQ(system.half_ticks(), start);
}

TEST(SimulationSeed, EnvironmentDefaultMatchesEachBoardAndExplicitOverride) {
    const auto expected = System::environment_seed();
    const char* environment = std::getenv("ECONOPET_SIM_SEED");
    EXPECT_EQ(expected, environment ? std::stoul(environment) : 1u);
    System first;
    System second;
    EXPECT_EQ(first.seed(), expected);
    EXPECT_EQ(second.seed(), expected);
    EXPECT_EQ(first.initial_state(), System::InitialState::Zero);
    EXPECT_EQ(second.initial_state(), System::InitialState::Zero);
    const auto override_seed = expected == 17 ? 19u : 17u;
    System overridden(override_seed);
    EXPECT_EQ(overridden.seed(), override_seed);
    EXPECT_EQ(first.seed(), expected);
    EXPECT_EQ(second.seed(), expected);
    EXPECT_EQ(System::environment_seed(), expected);
}

TEST_F(SystemTest, FailureContractIndependentSeededContextsReproduceStartupAndStimulus) {
    struct Evidence {
        std::vector<uint32_t> values;
        CycleTime at{Cycles{0}};
    };
    const auto capture = [&](uint32_t seed) {
        System subject(seed, System::InitialState::Random);
        EXPECT_EQ(subject.seed(), seed);
        EXPECT_EQ(subject.initial_state(), System::InitialState::Random);
        std::mt19937 random(subject.seed());
        Evidence evidence;
        const auto startup = subject.snapshot();
        evidence.values.insert(evidence.values.end(), {
            startup.cpu_addr_o.value(), startup.cpu_data_o, startup.cpu_selection_o,
            startup.video_o, startup.audio_l_o, startup.audio_r_o});
        for (unsigned index = 0; index < 16; ++index) {
            subject.io().pia1().inputs([&](auto& inputs) { inputs.port_a = static_cast<uint8_t>(random()); });
            subject.tick(Cycles{random() % 11});
            const auto pins = subject.snapshot();
            evidence.values.insert(evidence.values.end(), {
                pins.cpu_addr_o.value(), pins.cpu_selection_o, pins.cpu_data_o,
                pins.video_o, pins.audio_l_o, subject.io().pia1().peek_inputs().port_a});
        }
        evidence.at = subject.time();
        EXPECT_NE(subject.diagnostic("seed evidence", "").find("seed=" + std::to_string(seed)),
            std::string::npos);
        return evidence;
    };
    const auto first = capture(system.seed());
    const auto peer_seed = system.seed() == 17 ? 19u : 17u;
    const auto peer = capture(peer_seed);
    const auto repeated = capture(system.seed());
    EXPECT_EQ(first.values, repeated.values);
    EXPECT_EQ(first.at, repeated.at);
    EXPECT_NE(first.values, peer.values);
}

TEST_F(SystemTest, CallbackScopesEditingRejectsObservationAndPolling) {
    auto& pia = system.io().pia1();
    const auto start = system.time();
    const auto verify_guard = [&] {
        EXPECT_THROW({ auto subscription = system.observe([](const System&) {}); }, std::logic_error);
        expect_failure<std::logic_error>(system, "stimulus edit", [&] {
            system.run_until([](const System&) { return true; }, 0, "edit poll");
        });
        EXPECT_THROW(system.service_until([](const System&) { return true; },
            [](System&, Cycles) {}, 0, "edit service"), std::logic_error);
    };
    system.raw_stimulus([&](auto&) { verify_guard(); });
    pia.inputs([&](auto&) { verify_guard(); });
    EXPECT_EQ(system.time(), start);
    EXPECT_NO_THROW(system.run_until([](const System&) { return true; }, 0, "recovered"));
}

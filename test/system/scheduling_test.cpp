// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <array>
#include <optional>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "framework/asm.h"
#include "framework/observations.h"
#include "framework/system_test.h"
#include "ieee/controller.h"
#include "scheduler.h"

using econopet::Cycles;
using econopet::CycleTime;

namespace {
constexpr Cycles ProgramBudget{20000};
constexpr Cycles ProgramStimulusDelay{8000};
constexpr Cycles PeripheralBudget{1000};
constexpr Cycles AvDuration{10000};
constexpr econopet::CpuAddress CodeStart{0x300};
constexpr econopet::CpuAddress ResultStart{0x200};
constexpr econopet::CpuAddress CompletionAddress{0x201};
constexpr econopet::CpuAddress StackStart{0x100};
constexpr econopet::CpuAddress VectorsStart{0xfffa};
constexpr size_t CodeCapacity = 0x100;
constexpr size_t StackCapacity = 0x100;
constexpr size_t VectorCapacity = 6;
constexpr uint8_t Answer = 0x42;
}

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

// A checked 6502 program must consume scheduled GPIO before signaling completion.
TEST_F(SystemTest, SchedulerInterleavesCpuExecutionAndPeripheralStimulus) {
    // Reserve and assemble a real peripheral polling loop with a CPU-written terminal.
    test_asm::Scenario scenario(system);
    const auto code = scenario.reserve("code", test_asm::RegionKind::Code, CodeStart, CodeCapacity);
    const auto result = scenario.reserve("result", test_asm::RegionKind::Results, ResultStart, 1);
    const auto marker = scenario.completion(
        scenario.reserve("marker", test_asm::RegionKind::Mailbox, CompletionAddress, 1));
    scenario.reserve("vectors", test_asm::RegionKind::Vectors, VectorsStart, VectorCapacity);
    test_asm::mos6502::Program program(scenario, code);
    program.emit(test_asm::mos6502::lda_imm(econopet::io::PiaPortAccess.bits()));
    program.emit(test_asm::mos6502::sta_abs(pet::Pia1ControlA));
    const auto poll = program.mark("poll");
    program.emit(test_asm::mos6502::lda_abs(pet::Pia1PortA));
    program.emit(test_asm::mos6502::beq(poll));
    program.emit(test_asm::mos6502::sta_abs(result.start()));
    program.finish(marker);

    // Diagnostic sense owns PA7, so hold it low while waiting for a zero input byte.
    system.raw_stimulus([](auto& inputs) { inputs.diag_i = false; });
    system.io().pia1().inputs([](auto& inputs) { inputs.port_a = 0; });
    Scheduler scheduler(system);
    const auto at = system.time() + ProgramStimulusDelay;
    std::optional<CycleTime> applied;
    scheduler.at(at, [&](System& board) {
        applied = board.time();
        board.io().pia1().inputs([](auto& inputs) { inputs.port_a = Answer; });
    });

    // Execute through the existing clock owner and capture only checked completion.
    const auto execution = scenario.run(program, result, ProgramBudget, "scheduled GPIO",
        [&](System& board, Cycles remaining) { scheduler.step(board, remaining); });
    ASSERT_TRUE(applied);
    EXPECT_EQ(*applied, at);
    EXPECT_EQ(execution[scenario.lower_bank(result).byte("answer")], Answer);
    EXPECT_GT(execution.completed(), at);
}

// Scheduled CA1 edges must clock through the fitted PIA into the shared IRQ net.
TEST_F(SystemTest, SchedulerPeripheralInterruptUsesModeledEdgeDetection) {
    // Configure the PIA through actual CPU writes and prime its initial high input.
    IeeeController controller(system);
    controller.write(pet::Pia2ControlA,
        (econopet::io::PiaPortAccess | econopet::io::PiaControlBit::C1IrqEnable).bits());
    const auto clocks = system.io().clock_count();
    system.run_until([&](const System& board) { return board.io().clock_count() > clocks; },
        PeripheralBudget, "prime peripheral inputs");
    ASSERT_FALSE(system.io().pia2().irq());

    // Drive the external edge without injecting interrupt flags or host completion.
    Scheduler scheduler(system);
    const auto at = system.time() + Cycles{3};
    std::optional<CycleTime> applied;
    scheduler.at(at, [&](System& board) {
        applied = board.time();
        board.io().pia2().inputs([](auto& inputs) { inputs.ca1 = false; });
    });
    scheduler.run_until([](const System& board) { return board.io().pia2().irq(); },
        PeripheralBudget, "scheduled PIA IRQ");
    ASSERT_TRUE(applied);
    EXPECT_EQ(*applied, at);
    EXPECT_GT(system.time(), at);
    EXPECT_TRUE(system.io().pia2().irq());
    EXPECT_FALSE(system.snapshot().io_irq_ni);
}

// A timed external IRQ must execute the production 6502's reserved RAM handler.
TEST_F(SystemTest, SchedulerExternalInterruptExecutesSoft6502Handler) {
    // Keep the CPU quiescent while installing the handler's native interrupt vector.
    system.cpu().assert_reset();
    test_asm::Scenario scenario(system);
    const auto code = scenario.reserve("code", test_asm::RegionKind::Code, CodeStart, CodeCapacity);
    const auto result = scenario.reserve("result", test_asm::RegionKind::Results, ResultStart, 1);
    const auto marker = scenario.completion(
        scenario.reserve("marker", test_asm::RegionKind::Mailbox, CompletionAddress, 1));
    scenario.reserve("stack", test_asm::RegionKind::Stack, StackStart, StackCapacity);
    const auto vectors = scenario.reserve("vectors", test_asm::RegionKind::Vectors,
        VectorsStart, VectorCapacity);
    test_asm::mos6502::Program program(scenario, code);
    program.emit(test_asm::mos6502::cli());
    const auto park = program.mark("wait for IRQ");
    program.emit(test_asm::mos6502::jmp_abs(park));
    const auto handler = program.mark("IRQ handler");
    program.emit(test_asm::mos6502::lda_imm(Answer));
    program.emit(test_asm::mos6502::sta_abs(result.start()));
    program.finish(marker);
    const auto entry = program.address(handler).value();
    const std::array<uint8_t, 2> irq_vector{
        static_cast<uint8_t>(entry), static_cast<uint8_t>(entry >> 8)};
    scenario.write(vectors, pet::Irq6502.value() - vectors.start().value(), irq_vector);

    // Assert only the external IRQ pin and let the CPU produce the captured result.
    Scheduler scheduler(system);
    const auto at = system.time() + ProgramStimulusDelay;
    std::optional<CycleTime> applied;
    scheduler.at(at, [&](System& board) {
        applied = board.time();
        board.set_external_interrupts(true, false);
    });
    const auto execution = scenario.run(program, result, ProgramBudget, "scheduled 6502 IRQ",
        [&](System& board, Cycles remaining) { scheduler.step(board, remaining); });
    ASSERT_TRUE(applied);
    EXPECT_EQ(*applied, at);
    EXPECT_EQ(execution[scenario.lower_bank(result).byte("answer")], Answer);
    EXPECT_GT(execution.completed(), at);
}

// Scheduled input and composed recorders must share completed-cycle A/V observations.
TEST_F(SystemTest, SchedulerAvCaptureUsesOneObservationPhase) {
    // Register recorders before the observer that independently checks their latest sample.
    Scheduler scheduler(system);
    system.set_display(pet_video_type_fixed);
    auto all = system.trace_av(test_observation::AvSampling::EveryCycle);
    auto changes = system.trace_av(test_observation::AvSampling::ChangesOnly);
    const auto start = system.time();
    const auto at = start + Cycles{2};
    std::optional<CycleTime> applied;
    scheduler.at(at, [&](System& board) {
        applied = board.time();
        board.raw_stimulus([](auto& inputs) { inputs.audio_det_i = true; });
    });
    size_t observed = 0;
    auto subscription = system.observe([&](const System& board) {
        ++observed;
        const auto pins = board.snapshot();
        EXPECT_EQ(all.records().back().at, board.time());
        EXPECT_EQ(all.records().back().levels.audio_left, pins.audio_l_o);
        EXPECT_EQ(all.records().back().levels.audio_right, pins.audio_r_o);
        EXPECT_EQ(all.records().back().levels.video, pins.video_o);
    });

    // Compare both sampling policies over the same complete interval and actual changes.
    system.tick(AvDuration);
    ASSERT_TRUE(applied);
    EXPECT_EQ(*applied, at);
    EXPECT_EQ(observed, AvDuration.value());
    ASSERT_EQ(all.records().size(), AvDuration.value() + 1);
    EXPECT_EQ(all.records().front().at, start);
    EXPECT_EQ(all.records().back().at, start + AvDuration);
    std::vector<test_observation::AvTrace::Record> expected{all.records().front()};
    for (size_t index = 1; index < all.records().size(); ++index)
        if (!(all.records()[index].levels == expected.back().levels))
            expected.push_back(all.records()[index]);
    ASSERT_EQ(changes.records().size(), expected.size());
    EXPECT_GT(changes.records().size(), 1u);
    for (size_t index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(changes.records()[index].at, expected[index].at);
        EXPECT_EQ(changes.records()[index].levels, expected[index].levels);
    }
}

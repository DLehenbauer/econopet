// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "framework/observations.h"
#include "framework/system_test.h"
#include "ieee/controller.h"

using econopet::Cycles;
using test_asm::imm;

namespace {
constexpr Cycles Budget{20000};
constexpr unsigned CompletionDelayInstructions = 40;

// Own explicit completion storage independently of the convenience builder's private marker.
struct CompletionScenario : test_asm::Scenario<System> {
    // Reserve disjoint code, results, stack, mailbox and native reset vectors.
    explicit CompletionScenario(System& board) : Scenario(board),
        code(reserve("code", test_asm::RegionKind::Code, econopet::CpuAddress{0x0300}, 0x100)),
        results(reserve("results", test_asm::RegionKind::Results, econopet::CpuAddress{0x0200}, 2)),
        marker(completion(reserve("completion", test_asm::RegionKind::Mailbox,
            results.start() + results.size(), 1))) {
        reserve("stack", test_asm::RegionKind::Stack, econopet::CpuAddress{0x0100}, 0x100);
        reserve("vectors", test_asm::RegionKind::Vectors, econopet::CpuAddress{0xfff0}, 16);
    }
    test_asm::Region code, results;
    test_asm::CompletionMarker marker;
};
}

// A correct result is not completion until the CPU executes its later terminal store.
TEST_F(SystemTest, ExplicitCompletionWaitsPastResultsAndAcceptsTheFinalCycle) {
    const auto assemble = [](auto& program, auto& scenario) {
        program.lda(imm(0x42)).sta(scenario.results.start());
        for (unsigned index = 0; index < CompletionDelayInstructions; ++index) program.lda(imm(0));
        program.finish(scenario.marker);
    };
    CompletionScenario scenario(system);
    test_asm::mos6502::Program program(scenario, scenario.code);
    assemble(program, scenario);
    std::optional<econopet::CycleTime> result_at, marker_at;
    auto subscription = system.observe([&](const System& board) {
        if (!result_at && board.peek(econopet::SramAddress{scenario.results.start().value()}) == 0x42)
            result_at = board.time();
        if (!marker_at && board.peek(econopet::SramAddress{scenario.marker.address().value()})
            == scenario.marker.completed())
            marker_at = board.time();
    });
    const auto result = scenario.run(program, scenario.results, Budget, "delayed completion");
    ASSERT_TRUE(result_at);
    ASSERT_TRUE(marker_at);
    EXPECT_GT(*marker_at, *result_at);
    EXPECT_EQ(result.completed(), *marker_at);
    subscription.reset();

    // Repeat the same CPU instructions on identically seeded hardware at the exact endpoint.
    System other(system.seed());
    CompletionScenario exact_scenario(other);
    test_asm::mos6502::Program exact_program(exact_scenario, exact_scenario.code);
    assemble(exact_program, exact_scenario);
    const auto duration = result.completed() - result.started();
    const auto exact = exact_scenario.run(exact_program, exact_scenario.results, duration,
        "final completion cycle");
    EXPECT_EQ(exact.completed() - exact.started(), duration);
    EXPECT_EQ(exact.raw_bytes(), result.raw_bytes());
}

// Explicit nonzero pending values are installed under reset and survive failed service.
TEST_F(SystemTest, ExplicitCompletionInitializesNonzeroPendingAndRetainsServiceFailure) {
    constexpr uint8_t Pending = 0xc3, Completed = 0x7e;
    CompletionScenario scenario(system);
    const auto marker = scenario.completion(scenario.reserve("nonzero completion",
        test_asm::RegionKind::Mailbox, scenario.marker.address() + 1, 1), Pending, Completed);
    test_asm::mos6502::Program program(scenario, scenario.code);
    for (unsigned index = 0; index < CompletionDelayInstructions; ++index) program.lda(imm(0));
    program.finish(marker);
    bool initialized_under_reset = false;
    auto subscription = system.observe([&](const System& board) {
        if (board.cpu().peek_reset()
            && board.peek(econopet::SramAddress{marker.address().value()}) == marker.pending())
            initialized_under_reset = true;
    });
    unsigned passes = 0;
    const auto start = system.time();
    const auto result = scenario.run(program, scenario.results, Budget, "nonzero completion",
        [&](System& board, Cycles remaining) {
            EXPECT_EQ(remaining, Budget - (board.time() - start));
            ++passes;
        });
    EXPECT_TRUE(initialized_under_reset);
    EXPECT_GT(passes, 0u);
    EXPECT_EQ(result.raw_bytes(), (std::vector<uint8_t>{0, 0}));
    EXPECT_EQ(system.peek(econopet::SramAddress{marker.address().value()}), marker.completed());

    // A later run resets the marker before invoking work that fails without progress.
    const auto failed_at = system.time();
    bool serviced = false;
    EXPECT_THROW(scenario.run(program, scenario.results, Budget, "completion service failure",
        [&](System&, Cycles) {
            serviced = true;
            throw std::runtime_error("injected completion service failure");
        }), std::runtime_error);
    EXPECT_TRUE(serviced);
    EXPECT_LE(system.time() - failed_at, Budget);
    EXPECT_EQ(system.peek(econopet::SramAddress{marker.address().value()}), marker.pending());
    EXPECT_FALSE(system.spi().peek_failed());
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
    using namespace econopet::io;
    constexpr uint8_t Outputs = 0xf0, PiaValue = 0xa5, ViaValue = 0x55, ResetRegister = 0;
    constexpr Cycles ParkedInterval{2000};
    auto model = system.trace_pia2_writes();
    auto bus = system.trace_peripheral_writes();
    const auto execute = [&](auto& test) {
        const auto result = test.within(Budget).run([&](auto& p) {
            p.lda(imm(PiaDdrAccess.bits())).sta(pet::Pia2ControlA)
                .lda(imm(Outputs)).sta(pet::Pia2PortA)
                .lda(imm(PiaPortAccess.bits())).sta(pet::Pia2ControlA)
                .lda(imm(PiaValue)).sta(pet::Pia2PortA).sta(pet::Pia2PortA)
                .lda(imm(Outputs)).sta(pet::ViaDdrB)
                .lda(imm(ViaValue)).sta(pet::ViaPortB).sta(pet::ViaPortB);
        });
        model.capture(result.window()).require_count(5, "CPU PIA2 direction/control/data writes");
        const auto captured = bus.capture(result.window());
        using Write = std::pair<econopet::CpuAddress, uint8_t>;
        captured.require_order(std::vector<Write>{
            {pet::Pia2ControlA, PiaDdrAccess.bits()}, {pet::Pia2PortA, Outputs},
            {pet::Pia2ControlA, PiaPortAccess.bits()}, {pet::Pia2PortA, PiaValue},
            {pet::Pia2PortA, PiaValue}, {pet::ViaDdrB, Outputs},
            {pet::ViaPortB, ViaValue}, {pet::ViaPortB, ViaValue}},
            [](const auto& write) { return Write{write.address, write.data}; }, "physical write order");
        EXPECT_EQ(system.io().pia2().writes().count(), 5u);
        EXPECT_EQ(system.io().via().writes().count(), 3u);
        EXPECT_EQ(system.io().pia2().state().pa.ddr, Outputs);
        EXPECT_EQ(system.io().pia2().state().pa.outr, PiaValue);
        EXPECT_EQ(system.io().pia2().peek(PiaRegister::PortA), 0xaf);
        EXPECT_EQ(system.io().via().state().pb.ddr, Outputs);
        EXPECT_EQ(system.io().via().state().pb.outr, ViaValue);
        EXPECT_EQ(system.io().via().peek(ViaRegister::PortB), 0x5f);
        const auto parked = system.time();
        system.tick(ParkedInterval);
        bus.capture(system.trace_window(parked, system.time())).require_absent("terminal park has no writes");
        system.cpu().assert_reset();
        EXPECT_EQ(system.io().pia2().state().pa.ddr, ResetRegister);
        EXPECT_EQ(system.io().via().state().pb.ddr, ResetRegister);
        EXPECT_EQ(system.io().via().state().pb.outr, ResetRegister);
        EXPECT_EQ(system.io().pia2().writes().count(), 5u);
        EXPECT_EQ(system.io().via().writes().count(), 3u);
        system.io().clear_observations();
    };
    auto mos = system.program6502();
    execute(mos);
    auto mc = system.program6809();
    execute(mc);
}

// Both native cores publish zero-valued writes at the same completed boundary as observers.
TEST_F(SystemTest, NativePeripheralZeroWritesOwnOptionalTimestampedEvidence) {
    const auto execute = [&](auto& test) {
        EXPECT_FALSE(system.io().pia1().writes().last());
        EXPECT_FALSE(system.io().pia2().writes().last());
        EXPECT_FALSE(system.io().via().writes().last());
        unsigned observed_writes = 0;
        auto observer = system.observe([&](const System& board) {
            for (const auto& last : {board.io().pia1().writes().last(), board.io().pia2().writes().last()})
                if (last && last->at == board.time()) ++observed_writes;
            const auto& last = board.io().via().writes().last();
            if (last && last->at == board.time()) ++observed_writes;
        });
        test.within(Budget).run([](auto& p) {
            p.lda(imm(0)).sta(pet::Pia1PortA).sta(pet::Pia2PortA).sta(pet::ViaPortB);
        });
        EXPECT_EQ(observed_writes, 3u);
        const auto pia1 = system.io().pia1().writes().last();
        const auto pia2 = system.io().pia2().writes().last();
        const auto via = system.io().via().writes().last();
        ASSERT_TRUE(pia1);
        ASSERT_TRUE(pia2);
        ASSERT_TRUE(via);
        EXPECT_EQ(pia1->reg, econopet::io::PiaRegister::PortA);
        EXPECT_EQ(pia2->reg, econopet::io::PiaRegister::PortA);
        EXPECT_EQ(via->reg, econopet::io::ViaRegister::PortB);
        EXPECT_EQ(pia1->data, 0);
        EXPECT_EQ(pia2->data, 0);
        EXPECT_EQ(via->data, 0);
        EXPECT_LT(pia1->at, pia2->at);
        EXPECT_LT(pia2->at, via->at);
        system.cpu().assert_reset();
        EXPECT_EQ(system.io().pia1().writes().last()->at, pia1->at);
        EXPECT_EQ(system.io().pia2().writes().last()->at, pia2->at);
        EXPECT_EQ(system.io().via().writes().last()->at, via->at);
        system.io().clear_observations();
    };
    auto mos = system.program6502();
    execute(mos);
    auto mc = system.program6809();
    execute(mc);
}

// A fitted VIA counts actual PHI2 clocks and a native CPU read acknowledges its timer IRQ.
TEST_F(SystemTest, NativeViaTimerCountsPhysicalPhi2AndAcknowledgesThroughCpuRead) {
    using namespace econopet::io;
    constexpr unsigned TimerLoad = 256, PipelineClocks = 2, ByteBits = 8;
    const auto source = std::to_underlying(ViaInterruptBit::Timer1);
    const auto irq = (ViaInterruptBit::Summary | ViaInterruptBit::Timer1).bits();
    IeeeController controller(system);
    controller.write(pet::ViaIer, irq);
    controller.write(pet::ViaTimer1Low, TimerLoad & PortAllHigh);
    std::optional<uint64_t> loaded_at, expired_at;
    auto observer = system.observe([&](const System& board) {
        const auto& devices = board.io();
        const auto& last = devices.via().writes().last();
        if (!loaded_at && last && last->reg == ViaRegister::Timer1High) {
            loaded_at = devices.clock_count();
            EXPECT_EQ(devices.via().state().t1.counter, TimerLoad);
        }
        if (!loaded_at || expired_at) return;
        const auto elapsed = devices.clock_count() - *loaded_at;
        if (devices.via().peek(ViaRegister::Ifr) & source) {
            expired_at = devices.clock_count();
            EXPECT_EQ(elapsed, TimerLoad + PipelineClocks);
        } else {
            EXPECT_LT(elapsed, TimerLoad + PipelineClocks);
        }
    });
    controller.write(pet::ViaTimer1High, TimerLoad >> ByteBits);
    system.run_until([&](const System&) { return expired_at.has_value(); }, Budget, "physical VIA timer");
    observer.reset();
    ASSERT_TRUE(loaded_at);
    EXPECT_EQ(system.io().via().timer1_fault(), Via6522::Timer1Fault::None);
    EXPECT_EQ(controller.read(pet::ViaIfr) & irq, irq);
    EXPECT_FALSE(system.snapshot().io_irq_ni);
    controller.read(pet::ViaTimer1Low);
    EXPECT_EQ(controller.read(pet::ViaIfr) & source, 0);
    EXPECT_TRUE(system.snapshot().io_irq_ni);
}

// A real fitted timer, rather than an injected IRQ, vectors the native 6502 into its handler.
TEST_F(SystemTest, NativeViaTimerInterruptExecutesSoft6502Handler) {
    using namespace econopet::io;
    constexpr uint8_t TimerLoad = 32;
    constexpr econopet::CpuAddress Handler{0x0400};
    constexpr unsigned ByteBits = 8;
    const auto source = std::to_underlying(ViaInterruptBit::Timer1);
    CompletionScenario scenario(system);
    test_asm::mos6502::Program main(scenario, scenario.code);
    main.sei().lda(imm((ViaInterruptBit::Summary | ViaInterruptBit::Timer1).bits())).sta(pet::ViaIer)
        .lda(imm(TimerLoad)).sta(pet::ViaTimer1Low).lda(imm(0)).sta(pet::ViaTimer1High).cli();
    main.jmp(main.mark("wait for IRQ"));
    main.finish(scenario.marker);
    test_asm::mos6502::Program handler(scenario,
        scenario.reserve("IRQ handler", test_asm::RegionKind::Code, Handler, 0x100));
    handler.lda(imm(source)).sta(pet::ViaIfr).lda(imm(0x42)).sta(scenario.results.start())
        .lda(imm(scenario.marker.completed())).sta(scenario.marker.address()).rti();
    handler.install();
    system.poke(econopet::SramAddress{pet::Irq6502.value()}, Handler.value() & PortAllHigh);
    system.poke(econopet::SramAddress{(pet::Irq6502 + 1).value()}, Handler.value() >> ByteBits);
    const auto checkpoint = system.cpu().peek_fetch_checkpoint();
    bool saw_timer_irq = false;
    auto observer = system.observe([&](const System& board) {
        saw_timer_irq |= (board.io().via().peek(ViaRegister::Ifr) & source) != 0
            && !board.snapshot().io_irq_ni;
    });
    const auto result = scenario.run(main, scenario.results, Budget, "native VIA IRQ handler");
    EXPECT_TRUE(saw_timer_irq);
    EXPECT_EQ(result.raw_bytes(), (std::vector<uint8_t>{0x42, 0}));
    EXPECT_TRUE(system.cpu(CPU_SOFT_6502).peek_fetched_at(Handler, checkpoint));
    EXPECT_EQ(system.io().via().peek(ViaRegister::Ifr) & source, 0);
    EXPECT_EQ(system.io().via().timer1_fault(), Via6522::Timer1Fault::None);
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

// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <type_traits>
#include <utility>

#include "framework/system_test.h"

namespace {
using test_asm::imm;
constexpr econopet::Cycles PhaseBudget{20000};
struct PhaseSentinel {};

static_assert(std::is_reference_v<decltype(std::declval<const test_asm::PhaseHandle&>().name())>);
static_assert(!std::is_reference_v<decltype(std::declval<test_asm::PhaseHandle&&>().name())>);
static_assert(!std::is_reference_v<decltype(std::declval<const test_asm::PhaseHandle&&>().name())>);
static_assert(!std::is_reference_v<decltype(std::declval<test_asm::PhaseHandle&&>().window())>);
static_assert(!std::is_reference_v<decltype(std::declval<const test_asm::PhaseHandle&&>().window())>);

// Exercises the same ownership, ordering and register contract on both real soft CPUs.
template<class Builder>
void check_phases(System& system, Builder& test) {
    const auto answer = test.byte("answer");
    const auto started = system.time();
    auto first = test.within(PhaseBudget).pause([&](auto& p) {
        p.lda(imm(0x37)).sta(answer).pause("first");
        p.sta(answer).pause("second");
        p.lda(imm(0x42)).sta(answer);
    });
    EXPECT_EQ(first.name(), "first");
    const auto& copied_name = std::move(std::as_const(first)).name();
    const auto& copied_window = std::move(std::as_const(first)).window();
    EXPECT_EQ(copied_name, first.name());
    EXPECT_NE(copied_name.data(), first.name().data());
    EXPECT_EQ(copied_window.source(), first.window().source());
    EXPECT_FALSE(first.complete());
    const auto captured = test.capture(first);
    EXPECT_EQ(captured[answer], 0x37);
    EXPECT_EQ(captured.started(), started);
    EXPECT_EQ(captured.completed(), system.time());
    system.tick(2000);
    EXPECT_EQ(system.peek(econopet::SramAddress{answer.address().value()}), 0x37);
    std::array<uint8_t, 256> code{};
    std::array<uint8_t, 16> vectors{};
    for (size_t i = 0; i < code.size(); ++i)
        code[i] = system.peek(econopet::SramAddress{0x0300 + i});
    for (size_t i = 0; i < vectors.size(); ++i)
        vectors[i] = system.peek(econopet::SramAddress{0xfff0 + i});
    auto second = test.resume(first, PhaseBudget);
    EXPECT_EQ(second.name(), "second");
    EXPECT_EQ(test.capture(second)[answer], 0x37);
    EXPECT_THROW(test.capture(first), std::logic_error);
    EXPECT_THROW(test.resume(first, PhaseBudget), std::logic_error);
    auto last = test.resume(second, PhaseBudget);
    EXPECT_TRUE(last.complete());
    EXPECT_EQ(last.name(), "complete");
    EXPECT_EQ(test.capture(last)[answer], 0x42);
    EXPECT_EQ(captured[answer], 0x37);
    for (size_t i = 0; i < code.size(); ++i)
        EXPECT_EQ(system.peek(econopet::SramAddress{0x0300 + i}), code[i]);
    for (size_t i = 0; i < vectors.size(); ++i)
        EXPECT_EQ(system.peek(econopet::SramAddress{0xfff0 + i}), vectors[i]);
    EXPECT_FALSE(system.cpu().peek_reset());
    EXPECT_THROW(test.resume(last, PhaseBudget), std::logic_error);
    system.tick(1000);
    EXPECT_EQ(test.capture(last)[answer], 0x42);
}
}

// A temporary acknowledged phase transfers its name instead of lending a string.
TEST_F(SystemTest, TemporaryPhaseAccessOwnsName) {
    auto program = system.program6502(test_asm::ProgramMode::Phased);
    const auto& name = program.within(PhaseBudget).pause([](auto& p) { p.lda(imm(0x42)); }).name();
    EXPECT_EQ(name, "complete");
}

// A temporary phase's interval retains the board identity after the handle expires.
TEST_F(SystemTest, TemporaryPhaseAccessOwnsWindow) {
    auto program = system.program6502(test_asm::ProgramMode::Phased);
    const auto& window = program.within(PhaseBudget).pause([](auto& p) { p.lda(imm(0x42)); }).window();
    EXPECT_EQ(window.source(), system.observation_source());
    EXPECT_EQ(window.completed(), system.time());
}

TEST_F(SystemTest, ProgramPhases6502PreserveAccumulatorAndOrder) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    check_phases(system, test);
}

TEST_F(SystemTest, ProgramPhases6809PreserveAccumulatorAndOrder) {
    auto test = system.program6809(test_asm::ProgramMode::Phased);
    check_phases(system, test);
}

TEST_F(SystemTest, ProgramPhases6502PreserveFlagsAndStack) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    const auto answer = test.byte("answer");
    const auto stack = test.byte("stack");
    auto paused = test.within(PhaseBudget).pause([&](auto& p) {
        p.lda(imm(0));
        p.pause("zero set");
        const auto correct = p.label("correct");
        p.beq(correct).lda(imm(0xff)).sta(answer);
        p.bind(correct);
        const std::array<uint8_t, 2> read_flags{0x08, 0x68};
        const std::array<uint8_t, 2> read_stack{0xba, 0x8a};
        p.raw_bytes(read_flags).sta(answer);
        p.raw_bytes(read_stack).sta(stack);
    });
    auto last = test.resume(paused, PhaseBudget);
    EXPECT_NE(test.capture(last)[answer] & 0x02, 0);
    EXPECT_NE(test.capture(last)[answer] & 0x04, 0);
    EXPECT_EQ(test.capture(last)[stack], 0xff);
}

TEST_F(SystemTest, ProgramPhases6809PreserveFlagsAndStack) {
    auto test = system.program6809(test_asm::ProgramMode::Phased);
    const auto answer = test.byte("answer");
    const auto stack_high = test.byte("stack high");
    const auto stack_low = test.byte("stack low");
    auto paused = test.within(PhaseBudget).pause([&](auto& p) {
        p.lda(imm(0));
        p.pause("zero set");
        const std::array<uint8_t, 2> read_flags{0x1f, 0xa8}; // TFR CC,A
        const std::array<uint8_t, 2> read_stack{0x1f, 0x40}; // TFR S,D
        p.raw_bytes(read_flags).sta(answer);
        p.raw_bytes(read_stack).sta(stack_high);
        p.stb(stack_low.address());
    });
    auto last = test.resume(paused, PhaseBudget);
    EXPECT_NE(test.capture(last)[answer] & 0x04, 0);
    EXPECT_EQ(test.capture(last)[answer] & 0x50, 0x50);
    EXPECT_EQ(test.capture(last)[stack_high], 2);
    EXPECT_EQ(test.capture(last)[stack_low], 0);
}

TEST_F(SystemTest, ProgramPhasesNoInlinePausesStillVerifyTerminalGate) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    auto terminal = test.within(PhaseBudget).pause([](auto&) {});
    EXPECT_TRUE(terminal.complete());
    EXPECT_EQ(test.capture(terminal).completed(), system.time());
}

TEST_F(SystemTest, ProgramPhasesRequireExplicitModeAndBudget) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    const auto before = system.time();
    EXPECT_THROW(test.pause([](auto&) {}), std::logic_error);
    EXPECT_EQ(system.time(), before);
    EXPECT_THROW(test.within(PhaseBudget).run([](auto& p) { p.pause("not phased"); }), std::logic_error);
    EXPECT_EQ(system.time(), before);
}

TEST_F(SystemTest, ProgramPhasesRejectInvalidNamesAndOverflowBeforeMutation) {
    const auto before = system.time();
    auto empty = system.program6502(test_asm::ProgramMode::Phased);
    EXPECT_THROW(empty.within(PhaseBudget).pause([](auto& p) { p.pause(""); }), std::invalid_argument);
    auto reserved = system.program6502(test_asm::ProgramMode::Phased);
    EXPECT_THROW(reserved.within(PhaseBudget).pause([](auto& p) { p.pause("complete"); }), std::invalid_argument);
    auto duplicate = system.program6502(test_asm::ProgramMode::Phased);
    EXPECT_THROW(duplicate.within(PhaseBudget).pause([](auto& p) {
        p.pause("same");
        p.pause("same");
    }), std::invalid_argument);
    test_asm::LayoutOverrides layout;
    layout.code = test_asm::MemoryRegion{econopet::CpuAddress{0x0300}, 19};
    auto overflow = system.program6502(test_asm::ProgramMode::Phased, test_asm::Layout::Pet, layout);
    EXPECT_THROW(overflow.within(PhaseBudget).pause([](auto&) {}), std::out_of_range);
    EXPECT_EQ(system.time(), before);
    EXPECT_EQ(system.peek(econopet::SramAddress{0x0300}), System::IdleRamByte);
}

TEST_F(SystemTest, ProgramPhasesReservePrivateReleaseWithoutStealingResults) {
    test_asm::LayoutOverrides layout;
    layout.results = test_asm::MemoryRegion{econopet::CpuAddress{0x0200}, 1};
    auto test = system.program6502(test_asm::ProgramMode::Phased, test_asm::Layout::Pet, layout);
    const auto answer = test.byte("answer");
    auto terminal = test.within(PhaseBudget).pause([&](auto& p) { p.lda(imm(0x42)).sta(answer); });
    EXPECT_TRUE(terminal.complete());
    EXPECT_EQ(test.capture(terminal)[answer], 0x42);
}

TEST_F(SystemTest, ProgramPhasesRejectForeignAndMovedHandles) {
    auto first = system.program6502(test_asm::ProgramMode::Phased);
    auto handle = first.within(PhaseBudget).pause([](auto& p) { p.pause("first"); });
    auto peer = system.program6502(test_asm::ProgramMode::Phased);
    EXPECT_THROW(peer.resume(handle, PhaseBudget), std::logic_error);
    EXPECT_THROW(peer.capture(handle), std::logic_error);
    auto moved = std::move(handle);
    EXPECT_THROW(first.resume(handle, PhaseBudget), std::logic_error);
    auto last = first.resume(moved, PhaseBudget);
    EXPECT_TRUE(last.complete());
    EXPECT_THROW(first.within(PhaseBudget), std::logic_error);
}

TEST_F(SystemTest, ProgramPhasesRejectResetAndRestoredCpuSelection) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    auto handle = test.within(PhaseBudget).pause([](auto& p) { p.pause("first"); });
    system.cpu().assert_reset();
    system.cpu(CPU_SOFT_6809).select();
    system.cpu(CPU_SOFT_6502).select();
    system.cpu().release_reset();
    EXPECT_THROW(test.resume(handle, PhaseBudget), std::logic_error);
    EXPECT_THROW(test.capture(handle), std::logic_error);
}

TEST_F(SystemTest, ProgramPhasesMarkerWriteAloneNeverAcknowledgesPark) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    const auto before = system.time();
    EXPECT_THROW(test.within(5000).pause([](auto& p) {
        p.lda(imm(1)).sta(econopet::CpuAddress{0x02ff});
        const auto stuck = p.mark("never reach gate");
        p.jmp(stuck);
        p.pause("unreachable");
    }), std::runtime_error);
    EXPECT_EQ(system.time() - before, econopet::Cycles{5000});
    EXPECT_EQ(system.peek(econopet::SramAddress{0x02ff}), 1);
}

TEST_F(SystemTest, ProgramPhasesFreshAcknowledgmentRejectsHostSpoofAndSkippedPhase) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    system.poke(econopet::SramAddress{0x02ff}, 1);
    const auto before = system.time();
    EXPECT_THROW(test.within(5000).pause([](auto& p) {
        const auto later = p.label("later");
        p.jmp(later);
        p.pause("skipped");
        p.bind(later);
    }, [](System& board, econopet::Cycles) {
        board.poke(econopet::SramAddress{0x02ff}, 1);
        board.tick(1);
    }), std::runtime_error);
    EXPECT_EQ(system.time() - before, econopet::Cycles{5000});
}

TEST_F(SystemTest, ProgramPhasesSpiMailboxReadsCannotMasqueradeAsCpuPark) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    bool acknowledgment_seen = false;
    size_t host_reads = 0;
    EXPECT_THROW(test.within(10000).pause([](auto& p) {
        p.lda(imm(1)).sta(econopet::CpuAddress{0x02ff});
        const auto stuck = p.mark("not parked in handshake");
        p.jmp(stuck);
        p.pause("unreachable");
    }, [&](System& board, econopet::Cycles remaining) {
        acknowledgment_seen |= board.peek(econopet::SramAddress{0x02ff}) == 1;
        if (remaining < econopet::Cycles{256}) {
            board.tick(remaining);
            return;
        }
        board.spi().read(econopet::WishboneAddress{0x02fe});
        ++host_reads;
    }), std::runtime_error);
    EXPECT_TRUE(acknowledgment_seen);
    EXPECT_GT(host_reads, 0);
}

TEST_F(SystemTest, ProgramPhasesZeroStartupBudgetDoesNotMutateOrClock) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    const auto before = system.time();
    const auto original = system.peek(econopet::SramAddress{0x02ff});
    EXPECT_THROW(test.within(0).pause([](auto& p) { p.pause("first"); }), std::runtime_error);
    EXPECT_EQ(system.time(), before);
    EXPECT_EQ(system.peek(econopet::SramAddress{0x02ff}), original);
}

TEST_F(SystemTest, ProgramPhasesServiceFailurePreservesPayloadAndInvalidatesRun) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    auto handle = test.within(PhaseBudget).pause([](auto& p) { p.pause("first"); });
    bool serviced = false;
    EXPECT_THROW(test.resume(handle, PhaseBudget, [&](System&, econopet::Cycles) {
        serviced = true;
        throw PhaseSentinel{};
    }), PhaseSentinel);
    EXPECT_TRUE(serviced);
    EXPECT_THROW(test.capture(handle), std::logic_error);
    EXPECT_THROW(test.resume(handle, PhaseBudget), std::logic_error);
    system.tick(1);
}

TEST_F(SystemTest, ProgramPhasesResumeDeadlineNeverOvershootsNestedWork) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    auto handle = test.within(PhaseBudget).pause([](auto& p) { p.pause("first"); });
    const auto before = system.time();
    EXPECT_THROW(test.resume(handle, 3, [](System& board, econopet::Cycles) {
        board.tick(4);
    }), std::runtime_error);
    EXPECT_EQ(system.time() - before, econopet::Cycles{0});
    EXPECT_THROW(test.capture(handle), std::logic_error);
}

TEST_F(SystemTest, ProgramPhasesExactStartupAndResumeDeadlineBoundaries) {
    System baseline(system.seed());
    auto reference = baseline.program6502(test_asm::ProgramMode::Phased);
    auto boundary = reference.within(PhaseBudget).pause([](auto& p) { p.pause("first"); });
    const auto start_budget = reference.capture(boundary).completed() - reference.capture(boundary).started();
    const auto resume_start = baseline.time();
    auto terminal = reference.resume(boundary, PhaseBudget);
    const auto resume_budget = reference.capture(terminal).completed() - resume_start;
    auto exact = system.program6502(test_asm::ProgramMode::Phased);
    auto first = exact.within(start_budget).pause([](auto& p) { p.pause("first"); });
    auto last = exact.resume(first, resume_budget);
    EXPECT_TRUE(last.complete());
    System short_board(system.seed());
    auto short_test = short_board.program6502(test_asm::ProgramMode::Phased);
    EXPECT_THROW(short_test.within(start_budget - econopet::Cycles{1}).pause(
        [](auto& p) { p.pause("first"); }), std::runtime_error);
    System short_resume_board(system.seed());
    auto short_resume = short_resume_board.program6502(test_asm::ProgramMode::Phased);
    auto ready = short_resume.within(start_budget).pause([](auto& p) { p.pause("first"); });
    const auto before = short_resume_board.time();
    EXPECT_THROW(short_resume.resume(ready, resume_budget - econopet::Cycles{1}), std::runtime_error);
    EXPECT_EQ(short_resume_board.time() - before, resume_budget - econopet::Cycles{1});
}

TEST_F(SystemTest, ProgramPhasesZeroResumeBudgetLeavesGateClosed) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    auto first = test.within(PhaseBudget).pause([](auto& p) { p.pause("first"); });
    const auto before = system.time();
    EXPECT_THROW(test.resume(first, 0), std::runtime_error);
    EXPECT_EQ(system.time(), before);
    EXPECT_EQ(system.peek(econopet::SramAddress{0x02ff}), 1);
    EXPECT_EQ(system.peek(econopet::SramAddress{0x02fe}), 0);
}

TEST_F(SystemTest, ProgramPhasesRejectTokenExhaustionBeforeMutation) {
    test_asm::LayoutOverrides layout;
    layout.code = test_asm::MemoryRegion{econopet::CpuAddress{0x0400}, 0x2000};
    auto test = system.program6502(test_asm::ProgramMode::Phased, test_asm::Layout::Pet, layout);
    const auto before = system.time();
    EXPECT_THROW(test.within(PhaseBudget).pause([](auto& p) {
        for (size_t i = 0; i < 255; ++i)
            p.pause("phase " + std::to_string(i));
    }), std::out_of_range);
    EXPECT_EQ(system.time(), before);
    EXPECT_EQ(system.peek(econopet::SramAddress{0x0400}), System::IdleRamByte);
}

TEST_F(SystemTest, ProgramPhasesRejectExternallyReleasedGate) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    auto first = test.within(PhaseBudget).pause([](auto& p) { p.pause("first"); });
    system.poke(econopet::SramAddress{0x02fe}, 1);
    EXPECT_THROW(test.capture(first), std::logic_error);
    EXPECT_THROW(test.resume(first, PhaseBudget), std::logic_error);
}

TEST_F(SystemTest, ProgramPhasesBuilderDestructionLeavesClosedGateAndScopedObserver) {
    const auto before = system.time();
    std::optional<test_asm::PhaseHandle> handle;
    {
        auto test = system.program6502(test_asm::ProgramMode::Phased);
        const auto answer = test.byte("answer");
        handle.emplace(test.within(PhaseBudget).pause([&](auto& p) {
            p.pause("first");
            p.lda(imm(0x42)).sta(answer);
        }));
    }
    system.tick(2000);
    EXPECT_GT(system.time(), before);
    EXPECT_EQ(system.peek(econopet::SramAddress{0x0200}), 0);
    EXPECT_EQ(handle->name(), "first");
}

TEST_F(SystemTest, ProgramPhasesResetDuringServiceCannotIssueHandle) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    auto paused = test.within(PhaseBudget).pause([](auto& p) { p.pause("first"); });
    bool serviced = false;
    EXPECT_THROW(test.resume(paused, PhaseBudget, [&](System& board, econopet::Cycles) {
        serviced = true;
        board.cpu().assert_reset();
    }), std::logic_error);
    EXPECT_TRUE(serviced);
    EXPECT_THROW(test.capture(paused), std::logic_error);
}

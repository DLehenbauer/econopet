// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <array>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "framework/observations.h"
#include "framework/system_test.h"

namespace {
using namespace test_asm;
using econopet::Cycles;
constexpr Cycles Budget{20000};

// Verify real CPU bus records in phase-only windows, excluding the host's closed-gate dwell.
template<class Builder>
void check_phase_windows(System& board, Builder& program) {
    auto writes = board.trace_peripheral_writes();
    auto first = program.within(Budget).pause([](auto& p) {
        p.lda(imm(0x31)).sta(pet::Pia2PortA).pause("first write");
        p.lda(imm(0x42)).sta(pet::ViaPortB);
    });
    const auto initial = writes.capture(first);
    initial.require_count(1, "first phase");
    EXPECT_EQ(initial.records()[0].address, pet::Pia2PortA);
    const auto quiet = board.execution().within(500).run("closed phase gate");
    writes.capture(quiet).require_absent("closed phase gate");
    auto last = program.resume(first, Budget);
    const auto resumed = writes.capture(last);
    resumed.require_count(1, "resumed phase");
    EXPECT_EQ(resumed.records()[0].address, pet::ViaPortB);
    EXPECT_EQ(resumed.records()[0].data, 0x42);
    EXPECT_EQ(last.window().started(), quiet.completed());
    EXPECT_EQ(first.window().completed(), quiet.started());
    EXPECT_EQ(writes.capture(first).records().size(), 1u);
    const auto result = program.capture(last);
    EXPECT_EQ(writes.capture(result).records().size(), 2u);
    EXPECT_EQ(result.started(), first.window().started());
    EXPECT_EQ(result.completed(), last.window().completed());
}

// Use fresh release-relative configured work across startup and a later gate opening.
template<class Builder>
void check_configured_phases(System& board, Builder& test) {
    const auto answer = test.byte("answer");
    size_t startup_services = 0;
    auto ready = test.within(Budget)
        .every(5, MissedService::Reject, [&](System&, Cycles) { ++startup_services; }, RunAnchor::CpuRelease)
        .pause([&](auto& p) {
            p.lda(imm(0x37)).sta(answer).pause("ready");
            p.lda(imm(0x42)).sta(answer);
        });
    EXPECT_GT(startup_services, 0u);
    EXPECT_EQ(test.capture(ready)[answer], 0x37);
    const auto start = board.time();
    std::optional<econopet::CycleTime> first;
    std::vector<int> order;
    auto run = board.execution();
    run.within(Budget)
        .after(1, [&](System&) { order.push_back(1); }, RunAnchor::CpuRelease)
        .after(1, [&](System&) { order.push_back(2); })
        .every(5, MissedService::Reject, [&](System& observed, Cycles) {
            order.push_back(3);
            if (!first) first = observed.time();
        }, RunAnchor::CpuRelease)
        .every(5, MissedService::Reject, [&](System&, Cycles) { order.push_back(4); });
    auto complete = test.resume(ready, run);
    ASSERT_TRUE(first);
    EXPECT_EQ(*first, start + Cycles{5});
    ASSERT_GE(order.size(), 4u);
    EXPECT_EQ(std::vector<int>(order.begin(), order.begin() + 4), (std::vector<int>{1, 2, 3, 4}));
    EXPECT_TRUE(complete.complete());
    EXPECT_EQ(test.capture(complete)[answer], 0x42);
}
}

TEST_F(SystemTest, PhaseCaptureCannotExposePrivateReleaseGate) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    auto data = test.lower_bank();
    const auto before = system.time();
    EXPECT_EQ(data.buffer("all ordinary data", 0, 254).size(), 254u);
    EXPECT_THROW(data.byte("reserved release", 254), std::out_of_range);
    EXPECT_THROW(test.lower_bank("release"), std::invalid_argument);
    EXPECT_THROW(test.data("alias release", econopet::CpuAddress{0x2fe}, 1), std::invalid_argument);
    EXPECT_EQ(system.time(), before);
    EXPECT_THROW(test.within(Budget).pause([](auto& p) { p.lower_bank("release"); }), std::invalid_argument);
    EXPECT_EQ(system.time(), before);
}

TEST_F(SystemTest, PhaseCapturesPreserveWordsAndSelectedDataAtEachBoundary) {
    auto test = system.program6809(test_asm::ProgramMode::Phased);
    const auto word = test.word("word");
    const auto data = test.data("data", econopet::CpuAddress{0x400}, 2, Capture::Yes);
    auto first = test.within(Budget).pause([&](auto& p) {
        p.store(word, imm16(0x1234)).store(data.word(0, ByteOrder::LittleEndian), imm16(0x5678));
        p.pause("first");
        p.store(word, imm16(0xabcd)).store(data.word(0, ByteOrder::LittleEndian), imm16(0xef01));
    });
    const auto captured = test.capture(first);
    EXPECT_EQ(captured[word], 0x1234);
    EXPECT_EQ(captured[data], (std::vector<uint8_t>{0x78, 0x56}));
    auto done = test.resume(first, Budget);
    EXPECT_EQ(test.capture(done)[word], 0xabcd);
    EXPECT_EQ(test.capture(done)[data], (std::vector<uint8_t>{1, 0xef}));
    EXPECT_EQ(captured[word], 0x1234);
}

TEST_F(SystemTest, PhaseStorageCountsBytesRatherThanAllocationNames) {
    LayoutOverrides layout;
    layout.results = MemoryRegion{econopet::CpuAddress{0x200}, 3};
    auto test = system.program6502(test_asm::ProgramMode::Phased, Layout::Pet, layout);
    test.buffer("all data", 3, Capture::No);
    EXPECT_THROW(test.byte("overflow"), std::out_of_range);
    EXPECT_TRUE(test.within(Budget).pause([](auto&) {}).complete());
}

TEST_F(SystemTest, Phase6502TraceWindowsExcludeParkedHostInterval) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    check_phase_windows(system, test);
}

TEST_F(SystemTest, Phase6809TraceWindowsExcludeParkedHostInterval) {
    auto test = system.program6809(test_asm::ProgramMode::Phased);
    check_phase_windows(system, test);
}

TEST_F(SystemTest, Phase6502ConfiguredStartupAndResumeUseSharedVocabulary) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    check_configured_phases(system, test);
}

TEST_F(SystemTest, Phase6809ConfiguredStartupAndResumeUseSharedVocabulary) {
    auto test = system.program6809(test_asm::ProgramMode::Phased);
    check_configured_phases(system, test);
}

TEST_F(SystemTest, PhaseAfterSliceAndForwardedServicesBeginAfterRelease) {
    auto test = system.program6809(test_asm::ProgramMode::Phased);
    size_t startup = 0;
    auto first = test.within(Budget).pause([](auto& p) { p.pause("first"); },
        [&](System& board, Cycles remaining) {
            EXPECT_FALSE(board.cpu().peek_reset());
            EXPECT_LE(remaining, Budget);
            ++startup;
            board.tick(1);
        });
    EXPECT_GT(startup, 0u);
    auto configuration = system.execution();
    size_t slices = 0;
    configuration.within(Budget).every(7, ServiceTiming::AfterSlice,
        [&](System& board, Cycles) {
            EXPECT_FALSE(board.cpu().peek_reset());
            ++slices;
        });
    auto last = test.resume(first, configuration);
    EXPECT_GT(slices, 0u);
    EXPECT_TRUE(last.complete());
}

TEST_F(SystemTest, PhaseExtraPredicateCannotSubstituteForVerifiedPark) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    const auto before = system.time();
    EXPECT_THROW(test.within(5000).until([](const System&) { return true; }).pause([](auto& p) {
        const auto loop = p.mark("never acknowledge");
        p.jmp(loop);
    }), std::runtime_error);
    EXPECT_EQ(system.time(), before + Cycles{5000});
}

TEST_F(SystemTest, PhaseInvalidResumeConfigurationDoesNotConsumeCapability) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    auto first = test.within(Budget).pause([](auto& p) { p.pause("first"); });
    const auto start = system.time();
    auto missing_budget = system.execution();
    EXPECT_THROW(test.resume(first, missing_budget), std::logic_error);
    System other(system.seed());
    auto foreign = other.execution();
    foreign.within(Budget);
    EXPECT_THROW(test.resume(first, foreign), std::invalid_argument);
    EXPECT_EQ(system.time(), start);
    EXPECT_NO_THROW(test.capture(first));
    auto last = test.resume(first, Budget);
    EXPECT_TRUE(last.complete());
}

TEST_F(SystemTest, PhaseCapturesRemainFrozenWhileHostBackingChanges) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    const auto answer = test.word("answer");
    auto backing = test.lower_bank();
    auto first = test.within(Budget).pause([&](auto& p) {
        p.store(answer, imm16(0x1234)).pause("first");
    });
    const auto captured = test.capture(first);
    backing.initialize(answer, 0x5678);
    system.tick(100);
    EXPECT_EQ(test.capture(first)[answer], 0x1234);
    auto last = test.resume(first, Budget);
    EXPECT_EQ(test.capture(last)[answer], 0x5678);
    EXPECT_EQ(captured[answer], 0x1234);
}

TEST_F(SystemTest, PhaseResetPinAndRestoredMailboxInvalidationRemainSticky) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    auto first = test.within(Budget).pause([](auto& p) { p.pause("first"); });
    system.poke(econopet::SramAddress{0x2fe}, 1);
    EXPECT_THROW(test.capture(first), std::logic_error);
    system.poke(econopet::SramAddress{0x2fe}, 0);
    EXPECT_THROW(test.capture(first), std::logic_error);
    EXPECT_THROW(test.resume(first, Budget), std::logic_error);
    auto second = system.program6502(test_asm::ProgramMode::Phased);
    auto ready = second.within(Budget).pause([](auto& p) { p.pause("ready"); });
    system.set_external_reset(true);
    EXPECT_THROW(second.capture(ready), std::logic_error);
    system.set_external_reset(false);
    EXPECT_THROW(second.resume(ready, Budget), std::logic_error);
}

TEST_F(SystemTest, PhaseCaptureAllowsObservationButResumeAndConstructionReject) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    const auto answer = test.byte("answer");
    auto first = test.within(Budget).pause([&](auto& p) { p.lda(imm(0x42)).sta(answer).pause("first"); });
    const auto before = system.time();
    system.run_until([&](const System&) {
        EXPECT_EQ(test.capture(first)[answer], 0x42);
        EXPECT_THROW(test.resume(first, Budget), std::logic_error);
        EXPECT_THROW(test.pause([](auto&) {}), std::logic_error);
        return true;
    }, 0, "phase observation guard");
    EXPECT_EQ(system.time(), before);
    EXPECT_NO_THROW(test.resume(first, Budget));
}

TEST_F(SystemTest, PhaseStackMustFitSavedRegistersBeforeConsumingBuilder) {
    LayoutOverrides layout;
    layout.stack = MemoryRegion{econopet::CpuAddress{0x400}, 1};
    auto test = system.program6809(test_asm::ProgramMode::Phased, Layout::Pet, layout);
    const auto before = system.time();
    EXPECT_THROW(test.within(Budget).pause([](auto&) {}), std::out_of_range);
    EXPECT_EQ(system.time(), before);
    EXPECT_NO_THROW(test.run([](auto&) {}));
}

TEST_F(SystemTest, PhaseIndependentStartupConfigurationPreservesReleaseAndCapture) {
    auto test = system.program6809(test_asm::ProgramMode::Phased);
    const auto answer = test.byte("answer");
    auto run = system.execution();
    bool released = false;
    run.within(Budget).after(0, [&](System& board) {
        released = true;
        EXPECT_FALSE(board.cpu().peek_reset());
    }, RunAnchor::CpuRelease);
    auto terminal = test.pause([&](auto& p) { p.lda(imm(0x42)).sta(answer); }, run);
    EXPECT_TRUE(released);
    EXPECT_TRUE(terminal.complete());
    EXPECT_EQ(test.capture(terminal)[answer], 0x42);
    EXPECT_THROW(run.run(), std::logic_error);
}

TEST_F(SystemTest, PhaseTerminalCapacityAndGateAddressesFollowOverrides) {
    LayoutOverrides layout;
    layout.code = MemoryRegion{econopet::CpuAddress{0x400}, 41};
    layout.results = MemoryRegion{econopet::CpuAddress{0x280}, 1};
    layout.release = econopet::CpuAddress{0x281};
    layout.completion = econopet::CpuAddress{0x282};
    auto test = system.program6502(test_asm::ProgramMode::Phased, Layout::Pet, layout);
    const auto answer = test.byte("answer");
    auto first = test.within(Budget).pause([&](auto& p) { p.lda(imm(0x42)).sta(answer).pause("first"); });
    EXPECT_EQ(test.capture(first)[answer], 0x42);
    EXPECT_EQ(system.peek(econopet::SramAddress{0x281}), 0);
    EXPECT_EQ(system.peek(econopet::SramAddress{0x282}), 1);
    auto last = test.resume(first, Budget);
    EXPECT_TRUE(last.complete());
    EXPECT_EQ(system.peek(econopet::SramAddress{0x281}), 1);
    EXPECT_EQ(system.peek(econopet::SramAddress{0x282}), 2);
    EXPECT_EQ(system.peek(econopet::SramAddress{0x400 + 41}), System::IdleRamByte);
}

TEST_F(SystemTest, PhaseResumeRejectsOwnedSpiBeforeOpeningGate) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    auto first = test.within(Budget).pause([](auto& p) { p.pause("first"); });
    auto owner = system.spi().raw_transaction();
    const auto start = system.time();
    EXPECT_THROW(test.resume(first, Budget), std::logic_error);
    EXPECT_EQ(system.time(), start);
    EXPECT_EQ(system.peek(econopet::SramAddress{0x2ff}), 1);
    EXPECT_EQ(system.peek(econopet::SramAddress{0x2fe}), 0);
    EXPECT_NO_THROW(test.capture(first));
    owner.finish();
    EXPECT_NO_THROW(test.resume(first, Budget));
}

TEST_F(SystemTest, PhasePredicateWaitCannotReuseGateReadAfterExternalRelease) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    bool pending_predicate = false;
    bool altered = false;
    test.within(Budget).until([&](const System&) {
        pending_predicate = true;
        return false;
    });
    EXPECT_THROW(test.pause([](auto& p) { p.pause("first"); }, [&](System& board, Cycles) {
        if (pending_predicate && !altered) {
            altered = true;
            board.poke(econopet::SramAddress{0x2fe}, 1);
        }
        board.tick(1);
    }), std::logic_error);
    EXPECT_TRUE(pending_predicate);
    EXPECT_TRUE(altered);
}

TEST_F(SystemTest, PhaseResumeSharesEarliestInheritedDeadline) {
    auto test = system.program6809(test_asm::ProgramMode::Phased);
    auto first = test.within(Budget).pause([](auto& p) { p.pause("first"); });
    const auto start = system.time();
    EXPECT_THROW(system.service_until([](const System&) { return false; },
        [&](System&, Cycles) { test.resume(first, Budget); }, 2, "outer phase deadline"), std::runtime_error);
    EXPECT_EQ(system.time(), start + Cycles{2});
    EXPECT_THROW(test.capture(first), std::logic_error);
}

TEST_F(SystemTest, PhaseCaughtStimulusFailureCannotReturnCapability) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    auto first = test.within(Budget).pause([](auto& p) { p.pause("first"); });
    struct Sentinel {};
    bool fired = false;
    auto run = system.execution();
    run.within(Budget).after(1, [&](System&) { fired = true; throw Sentinel{}; }, RunAnchor::CpuRelease)
        .service([](System& board, Cycles) {
            try { board.tick(2); } catch (const Sentinel&) {}
        });
    EXPECT_THROW(test.resume(first, run), std::logic_error);
    EXPECT_TRUE(fired);
    EXPECT_THROW(test.capture(first), std::logic_error);
    EXPECT_THROW(test.resume(first, Budget), std::logic_error);
}

TEST_F(SystemTest, PhaseCaptureMayBeEmptyAndDetachedBeyondBoardLifetime) {
    auto empty = system.program6502(test_asm::ProgramMode::Phased);
    auto done = empty.within(Budget).pause([](auto&) {});
    EXPECT_TRUE(empty.capture(done).empty());
    const auto detached = [&] {
        System board(system.seed());
        auto test = board.program6809(test_asm::ProgramMode::Phased);
        const auto word = test.word("answer");
        auto first = test.within(Budget).pause([&](auto& p) { p.store(word, imm16(0x1234)).pause("first"); });
        return std::make_pair(word, test.capture(first));
    }();
    EXPECT_EQ(detached.second[detached.first], 0x1234);
}

TEST_F(SystemTest, PhaseUnclockedResetDuringPredicateWaitCannotIssueCapability) {
    auto test = system.program6809(test_asm::ProgramMode::Phased);
    bool reached_gate = false;
    bool reset = false;
    test.within(Budget).until([&](const System&) { reached_gate = true; return false; });
    EXPECT_THROW(test.pause([](auto& p) { p.pause("first"); }, [&](System& board, Cycles) {
        if (reached_gate) {
            reset = true;
            board.set_external_reset(true);
        } else {
            board.tick(1);
        }
    }), std::logic_error);
    EXPECT_TRUE(reset);
}

TEST_F(SystemTest, Phase6502HandshakePreservesIndexRegisters) {
    auto test = system.program6502(test_asm::ProgramMode::Phased);
    const auto accumulator = test.byte("A"), x = test.byte("X"), y = test.byte("Y");
    auto first = test.within(Budget).pause([&](auto& p) {
        const std::array<uint8_t, 2> load_y{0xa0, 0x34}; // LDY #$34
        p.ldx(imm(0x12)).raw_bytes(load_y).lda(imm(0x42)).pause("registers");
        p.sta(accumulator);
        const std::array<uint8_t, 1> transfer_x{0x8a}, transfer_y{0x98}; // TXA, TYA
        p.raw_bytes(transfer_x).sta(x).raw_bytes(transfer_y).sta(y);
    });
    auto last = test.resume(first, Budget);
    const auto result = test.capture(last);
    EXPECT_EQ(result[accumulator], 0x42);
    EXPECT_EQ(result[x], 0x12);
    EXPECT_EQ(result[y], 0x34);
}

TEST_F(SystemTest, Phase6809HandshakePreservesOtherDataAndIndexRegisters) {
    auto test = system.program6809(test_asm::ProgramMode::Phased);
    const auto accumulator = test.byte("A"), b = test.byte("B");
    const auto x = test.word("X"), y = test.word("Y");
    auto first = test.within(Budget).pause([&](auto& p) {
        const std::array<uint8_t, 4> load_y{0x10, 0x8e, 0x56, 0x78}; // LDY #$5678
        p.ldb(imm(0x34)).ldx(imm16(0x1234)).raw_bytes(load_y).lda(imm(0x42)).pause("registers");
        p.sta(accumulator).stb(b.address());
        const std::array<uint8_t, 2> x_to_d{0x1f, 0x10}, y_to_d{0x1f, 0x20}; // TFR X,D and Y,D
        p.raw_bytes(x_to_d).sta(x.byte(0)).stb(x.byte(1).address());
        p.raw_bytes(y_to_d).sta(y.byte(0)).stb(y.byte(1).address());
    });
    auto last = test.resume(first, Budget);
    const auto result = test.capture(last);
    EXPECT_EQ(result[accumulator], 0x42);
    EXPECT_EQ(result[b], 0x34);
    EXPECT_EQ(result[x], 0x1234);
    EXPECT_EQ(result[y], 0x5678);
}

TEST_F(SystemTest, PhaseLastRepresentableTokenAcknowledgesTerminalBoundary) {
    LayoutOverrides layout;
    layout.code = MemoryRegion{econopet::CpuAddress{0x400}, 0x2000};
    auto test = system.program6502(test_asm::ProgramMode::Phased, Layout::Pet, layout);
    constexpr size_t InlinePhases = std::numeric_limits<uint8_t>::max() - 1;
    auto current = test.within(Budget).pause([](auto& p) {
        for (size_t i = 0; i < InlinePhases; ++i) p.pause("phase " + std::to_string(i));
    });
    for (size_t i = 0; i < InlinePhases; ++i) {
        EXPECT_FALSE(current.complete());
        EXPECT_EQ(current.name(), "phase " + std::to_string(i));
        auto next = test.resume(current, Budget);
        current = std::move(next);
    }
    EXPECT_TRUE(current.complete());
    EXPECT_EQ(system.peek(econopet::SramAddress{0x2ff}), 0xff);
    EXPECT_THROW(test.resume(current, Budget), std::logic_error);
}

TEST_F(SystemTest, PhaseArbitraryCpuMailboxWriteAndReadCannotMasqueradeAsHandshake) {
    const auto check = [&](auto& test) {
        const auto start = system.time();
        EXPECT_THROW(test.within(5000).pause([](auto& p) {
            p.lda(imm(1)).sta(econopet::CpuAddress{0x2ff}).lda(econopet::CpuAddress{0x2fe});
            const auto stuck = p.mark("outside the preassembled handshake");
            if constexpr (std::is_same_v<typename std::remove_reference_t<decltype(p)>::CpuType, Mos6502>)
                p.jmp(stuck);
            else
                p.bra(stuck);
            p.pause("unreachable");
        }), std::runtime_error);
        EXPECT_EQ(system.time(), start + Cycles{5000});
    };
    auto mos = system.program6502(test_asm::ProgramMode::Phased);
    check(mos);
    auto mc = system.program6809(test_asm::ProgramMode::Phased);
    check(mc);
}

TEST_F(SystemTest, PhaseReadingHandshakeOpcodeAsDataCannotAcknowledge) {
    const auto check = [&](auto& test) {
        const auto start = system.time();
        EXPECT_THROW(test.within(5000).pause([](auto& p) {
            const auto outside = p.mark("outside handshake");
            // The final loop uses a three-byte JMP or a two-byte BRA.
            constexpr bool Is6502 = std::is_same_v<typename std::remove_reference_t<decltype(p)>::CpuType, Mos6502>;
            constexpr size_t OutsideSize = Is6502 ? 14 : 13;
            const auto gate_load = p.address(outside) + OutsideSize + phase_detail::GateLoadOffset;
            p.lda(imm(1)).sta(econopet::CpuAddress{0x2ff})
                .lda(gate_load).lda(econopet::CpuAddress{0x2fe});
            if constexpr (Is6502)
                p.jmp(outside);
            else
                p.bra(outside);
            p.pause("unreachable");
        }), std::runtime_error);
        EXPECT_EQ(system.time(), start + Cycles{5000});
    };
    auto mos = system.program6502(test_asm::ProgramMode::Phased);
    check(mos);
    auto mc = system.program6809(test_asm::ProgramMode::Phased);
    check(mc);
}

TEST_F(SystemTest, PhaseTerminalHandshakeExactlyFillsCodeForBothCpus) {
    LayoutOverrides layout;
    constexpr size_t Prologue6502 = 4, Prologue6809 = 6;
    layout.code = MemoryRegion{econopet::CpuAddress{0x400}, Prologue6502 + phase_detail::HandshakeSize};
    auto mos = system.program6502(ProgramMode::Phased, Layout::Pet, layout);
    auto first = mos.within(Budget).pause([](auto&) {});
    EXPECT_TRUE(first.complete());
    EXPECT_EQ(system.peek(econopet::SramAddress{0x400 + layout.code->size}), System::IdleRamByte);
    EXPECT_THROW(mos.resume(first, Budget), std::logic_error);
    system.tick(500);
    EXPECT_NO_THROW(mos.capture(first));
    layout.code->size = Prologue6809 + phase_detail::HandshakeSize;
    auto mc = system.program6809(ProgramMode::Phased, Layout::Pet, layout);
    auto second = mc.within(Budget).pause([](auto&) {});
    EXPECT_TRUE(second.complete());
    EXPECT_EQ(system.peek(econopet::SramAddress{0x400 + layout.code->size}), System::IdleRamByte);
    EXPECT_THROW(mc.resume(second, Budget), std::logic_error);
    system.tick(500);
    EXPECT_NO_THROW(mc.capture(second));
}

TEST_F(SystemTest, PhaseModeFixesCapacityBeforeAllocatingAndPreservesCompleteBuilders) {
    auto complete = system.program6502();
    EXPECT_NO_THROW(complete.lower_bank().byte("final data byte", 254));
    const auto all = complete.buffer("all", 255);
    const auto before = system.time();
    EXPECT_THROW(complete.within(Budget).pause([](auto&) {}), std::logic_error);
    EXPECT_EQ(system.time(), before);
    EXPECT_EQ(complete.run([&](auto& p) { p.lda(imm(0x42)).sta(all.byte(254)); })[all.byte(254)], 0x42);
    auto phased = system.program6809(ProgramMode::Phased);
    const auto data = phased.buffer("all", 254);
    EXPECT_THROW(phased.byte("extra"), std::out_of_range);
    auto last = phased.within(Budget).pause([&](auto& p) {
        p.lda(imm(0x37)).sta(data.byte(253));
        EXPECT_THROW(p.lower_bank("release"), std::invalid_argument);
        EXPECT_THROW(p.lower_bank("results").byte("release alias", 254), std::out_of_range);
    });
    EXPECT_EQ(phased.capture(last)[data.byte(253)], 0x37);
    EXPECT_EQ(system.peek(econopet::SramAddress{0x2fe}), 0);
}

TEST_F(SystemTest, PhaseMailboxOverridesRejectOverlapsAndCompleteMode) {
    LayoutOverrides layout;
    layout.release = econopet::CpuAddress{0x200};
    const auto before = system.time();
    EXPECT_THROW(system.program6502(ProgramMode::Phased, Layout::Pet, layout), std::invalid_argument);
    layout.release = econopet::CpuAddress{0x2ff};
    EXPECT_THROW(system.program6809(ProgramMode::Phased, Layout::Pet, layout), std::invalid_argument);
    EXPECT_THROW(system.program6502(Layout::Pet, layout), std::invalid_argument);
    EXPECT_THROW(system.program6502(static_cast<ProgramMode>(99)), std::invalid_argument);
    EXPECT_EQ(system.time(), before);
}

TEST_F(SystemTest, PhaseInvariantFailuresRetainTheirCauseAfterRestoration) {
    const auto check = [&](econopet::SramAddress altered, uint8_t changed, uint8_t restored,
                           const std::string& cause, bool clock) {
        auto test = system.program6502(ProgramMode::Phased);
        auto first = test.within(Budget).pause([](auto& p) { p.pause("first"); });
        system.poke(altered, changed);
        if (clock) {
            system.tick(1);
            system.poke(altered, restored);
        }
        // Observe the same retained cause in capture and resume, even after repairing the mailbox.
        const auto expect_cause = [&](auto operation) {
            try {
                operation();
                FAIL() << "altered phase was accepted";
            } catch (const std::logic_error& error) {
                EXPECT_NE(std::string(error.what()).find(cause), std::string::npos) << error.what();
            }
        };
        expect_cause([&] { test.capture(first); });
        system.poke(altered, restored);
        expect_cause([&] { test.capture(first); });
        expect_cause([&] { test.resume(first, Budget); });
    };
    for (const bool clock : {false, true}) {
        check(econopet::SramAddress{0x2fe}, 1, 0, "private release gate externally opened", clock);
        check(econopet::SramAddress{0x2ff}, 0, 1, "acknowledgment mailbox externally altered", clock);
    }
}

TEST_F(SystemTest, PhaseOpcodeHistoryIsFreshAndDistinctFromNative6809Sync) {
    auto test = system.program6809(ProgramMode::Phased);
    const auto checkpoint = system.cpu(CPU_SOFT_6809).peek_fetch_checkpoint();
    auto first = test.within(Budget).pause([](auto& p) { p.pause("first"); });
    auto cpu = system.cpu(CPU_SOFT_6809);
    const econopet::CpuAddress GateOpcode{0x300 + 6 + phase_detail::GateLoadOffset};
    EXPECT_TRUE(cpu.peek_opcode_fetched_at(GateOpcode, checkpoint));
    EXPECT_FALSE(cpu.peek_opcode_fetched_at(GateOpcode + 1, checkpoint));
    EXPECT_FALSE(cpu.peek_fetched_at(GateOpcode, checkpoint));
    EXPECT_FALSE(system.cpu(CPU_SOFT_6502).peek_opcode_fetched_at(GateOpcode, checkpoint));
    auto last = test.resume(first, Budget);
    EXPECT_TRUE(last.complete());
    cpu.assert_reset();
    const auto reset_checkpoint = cpu.peek_fetch_checkpoint();
    system.tick(100);
    EXPECT_EQ(cpu.peek_fetch_checkpoint(), reset_checkpoint);
    EXPECT_TRUE(cpu.peek_opcode_fetched_at(GateOpcode, checkpoint));
    EXPECT_FALSE(cpu.peek_opcode_fetched_at(GateOpcode, reset_checkpoint));
}

TEST_F(SystemTest, PhaseEscapingFaultsRetainSpecificCausesBeforeObserverDispatch) {
    enum class Fault { Clock, Spi, Callback };
    struct Sentinel {};
    const auto check = [&](System& board, auto& test, Fault fault) {
        auto first = test.within(Budget).pause([&](auto& p) {
            p.pause("ready");
            if (fault == Fault::Clock) p.lda(pet::ViaShift);
        });
        const auto service = [&](System& active, Cycles remaining) {
            if (fault == Fault::Clock) {
                active.tick(remaining);
            } else if (fault == Fault::Spi) {
                active.spi().transaction([](auto&) { throw Sentinel{}; });
            } else {
                throw Sentinel{};
            }
        };
        if (fault == Fault::Clock) {
            EXPECT_THROW(test.resume(first, Budget, service), std::logic_error);
            ASSERT_TRUE(board.clock_faulted());
            ASSERT_FALSE(board.spi().peek_failed());
        } else {
            EXPECT_THROW(test.resume(first, Budget, service), Sentinel);
            ASSERT_EQ(board.spi().peek_failed(), fault == Fault::Spi);
            ASSERT_FALSE(board.clock_faulted());
        }
        const auto stopped = board.half_ticks();
        const std::string cause = fault == Fault::Clock ? "hardware clock failed during phases"
            : fault == Fault::Spi ? "SPI transport failed during phases" : "phase execution previously failed";
        // Even the consumed resume capability must report the retained cause, without more clocks.
        const auto expect_cause = [&](auto operation) {
            try {
                operation();
                FAIL() << "failed phase was accepted";
            } catch (const std::logic_error& error) {
                EXPECT_NE(std::string(error.what()).find(cause), std::string::npos) << error.what();
            }
            EXPECT_EQ(board.half_ticks(), stopped);
        };
        expect_cause([&] { test.capture(first); });
        expect_cause([&] { test.resume(first, Budget); });
        expect_cause([&] { test.capture(first); });
    };
    for (const auto fault : {Fault::Clock, Fault::Spi, Fault::Callback}) {
        SCOPED_TRACE(std::to_underlying(fault));
        System mos_board(system.seed());
        auto mos = mos_board.program6502(ProgramMode::Phased);
        check(mos_board, mos, fault);
        System mc_board(system.seed());
        auto mc = mc_board.program6809(ProgramMode::Phased);
        check(mc_board, mc, fault);
    }
}

TEST_F(SystemTest, PhaseSnapshotsFreezeBeforeServiceOvershootAndExtraPredicateWait) {
    const auto check = [&](auto make_builder, bool wait_for_predicate) {
        System reference(system.seed());
        auto baseline = make_builder(reference);
        const auto original = baseline.byte("answer");
        const auto assemble = [](auto& p, const MemoryByte& answer) {
            p.lda(imm(0x31)).sta(answer).pause("ready");
            p.lda(imm(0x42)).sta(answer);
        };
        auto first_reference = baseline.within(Budget).pause([&](auto& p) { assemble(p, original); });
        const auto startup_elapsed = first_reference.window().completed() - first_reference.window().started();

        System subject(system.seed());
        auto test = make_builder(subject);
        const auto answer = test.byte("answer");
        constexpr Cycles Slice{512};
        const Cycles predicate_wait{wait_for_predicate ? 1000u : 0u};
        const auto start = subject.time();
        const auto expected_first = start + startup_elapsed;
        bool startup_mutated = false;
        auto startup = subject.execution();
        startup.within(Budget)
            .after(startup_elapsed + Cycles{1}, [&](System& board) {
                startup_mutated = true;
                board.poke(econopet::SramAddress{answer.address().value()}, 0x99);
            })
            .until([&](const System& board) { return !(board.time() < expected_first + predicate_wait); })
            .service([&](System& board, Cycles remaining) { board.tick(std::min(Slice, remaining)); });
        auto first = test.pause([&](auto& p) { assemble(p, answer); }, startup);
        ASSERT_TRUE(startup_mutated);
        EXPECT_GT(subject.time().value(), expected_first.value());
        EXPECT_EQ(subject.peek(econopet::SramAddress{answer.address().value()}), 0x99);
        const auto initial = test.capture(first);
        EXPECT_EQ(initial[answer], 0x31);
        EXPECT_EQ(initial.started(), start);
        EXPECT_EQ(initial.completed(), expected_first);
        EXPECT_EQ(initial.window().completed(), expected_first);
        EXPECT_EQ(first.window().started(), start);
        EXPECT_EQ(first.window().completed(), expected_first);

        const auto resumed = subject.time();
        std::optional<econopet::CycleTime> expected_last;
        // Host dwell changes where the CPU is in its polling loop when resume opens the gate.
        auto observer = subject.observe([&](const System& board) {
            const auto pins = board.snapshot();
            if (!expected_last && pins.cpu_clock_o && pins.cpu_we_n_o && !pins.ram_oe_n_o
                && pins.cpu_addr_o == econopet::CpuAddress{0x2fe}
                && board.peek(econopet::SramAddress{0x2ff}) == 2
                && board.peek(econopet::SramAddress{0x2fe}) != 2)
                expected_last = board.time();
        });
        bool resume_mutated = false;
        auto continuation = subject.execution();
        continuation.within(Budget)
            .until([&](const System& board) {
                return expected_last && !(board.time() < *expected_last + predicate_wait);
            })
            .service([&](System& board, Cycles remaining) {
                board.tick(std::min(Slice, remaining));
                if (expected_last && *expected_last < board.time()) {
                    resume_mutated = true;
                    board.poke(econopet::SramAddress{answer.address().value()}, 0xaa);
                }
            });
        auto last = test.resume(first, continuation);
        ASSERT_TRUE(expected_last);
        ASSERT_TRUE(resume_mutated);
        EXPECT_TRUE(last.complete());
        EXPECT_GT(subject.time().value(), expected_last->value());
        EXPECT_EQ(subject.peek(econopet::SramAddress{answer.address().value()}), 0xaa);
        const auto final = test.capture(last);
        EXPECT_EQ(final[answer], 0x42);
        EXPECT_EQ(final.started(), start);
        EXPECT_EQ(final.completed(), *expected_last);
        EXPECT_EQ(final.window().completed(), *expected_last);
        EXPECT_EQ(last.window().started(), resumed);
        EXPECT_EQ(last.window().completed(), *expected_last);
        EXPECT_EQ(initial[answer], 0x31);
    };
    for (const bool wait : {false, true}) {
        SCOPED_TRACE(wait);
        check([](System& board) { return board.program6502(ProgramMode::Phased); }, wait);
        check([](System& board) { return board.program6809(ProgramMode::Phased); }, wait);
    }
}

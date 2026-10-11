// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <memory>
#include <type_traits>
#include <utility>

#include "framework/system_test.h"
#include "host_memory.h"

namespace {
constexpr econopet::Cycles ExecutionBudget = 20000;
using test_asm::imm;
using test_asm::Layout;
using test_asm::LayoutOverrides;
using test_asm::MemoryRegion;

// Snapshots all SRAM to prove that rejected assembly never partially installs.
std::vector<uint8_t> memory_image(const System& system) {
    std::vector<uint8_t> image(System::RamSize);
    for (size_t i = 0; i < image.size(); ++i)
        image[i] = system.peek(econopet::SramAddress{i});
    return image;
}

struct AssemblySentinel {};

template<class Board>
concept HasProgramFactories = requires(Board&& board) {
    std::forward<Board>(board).program6502();
    std::forward<Board>(board).program6809();
};
static_assert(HasProgramFactories<System&>);
static_assert(!HasProgramFactories<const System&>);
static_assert(!HasProgramFactories<System>);
static_assert(std::is_same_v<decltype(std::declval<System&>().program6502()),
    test_asm::ProgramBuilder<test_asm::Mos6502>>);
static_assert(!std::is_constructible_v<test_asm::ProgramBuilder<test_asm::Mos6502>,
    HostMemory&, Layout>);

template<class Receiver, class = void> struct HasSei : std::false_type {};
template<class Receiver>
struct HasSei<Receiver, std::void_t<decltype(std::declval<Receiver&>().sei())>> : std::true_type {};
template<class Receiver, class = void> struct HasOrcc : std::false_type {};
template<class Receiver>
struct HasOrcc<Receiver, std::void_t<decltype(std::declval<Receiver&>().orcc(imm(0)))>> : std::true_type {};
template<class Receiver, class Operand, class = void> struct CanJsr : std::false_type {};
template<class Receiver, class Operand>
struct CanJsr<Receiver, Operand,
    std::void_t<decltype(std::declval<Receiver&>().jsr(std::declval<Operand>()))>> : std::true_type {};
template<class Receiver, class Operand, class = void> struct CanLda : std::false_type {};
template<class Receiver, class Operand>
struct CanLda<Receiver, Operand,
    std::void_t<decltype(std::declval<Receiver&>().lda(std::declval<Operand>()))>> : std::true_type {};
}

TEST_F(SystemTest, ProgramBuilder6502OwnsExecutionAndDetachedResults) {
    const auto started = system.time();
    auto test = system.program6502(Layout::Pet);
    const auto answer = test.byte("answer");
    const auto untouched = test.byte("untouched");
    system.poke(econopet::SramAddress{untouched.address().value()}, 0xff);
    const auto result = test.within(ExecutionBudget).run([&](auto& p) {
        p.lda(imm(0x42)).sta(answer);
    });
    EXPECT_EQ(result[answer], 0x42);
    EXPECT_EQ(result[untouched], 0);
    EXPECT_EQ(result.started(), started);
    EXPECT_EQ(result.completed(), system.time());
    EXPECT_LE(result.completed() - result.started(), ExecutionBudget);
    const auto& copied_window = std::move(result).window();
    EXPECT_EQ(copied_window.source(), result.window().source());
    EXPECT_EQ(copied_window.completed(), result.completed());
    EXPECT_EQ(system.snapshot().cpu_selection_o, std::to_underlying(CPU_SOFT_6502));
    EXPECT_FALSE(system.cpu().peek_reset());
    system.poke(econopet::SramAddress{answer.address().value()}, 0x99);
    EXPECT_EQ(result[answer], 0x42);
    EXPECT_THROW(test.run([](auto&) {}), std::logic_error);
    EXPECT_THROW(test.byte("late"), std::logic_error);
    EXPECT_THROW(test.within(1), std::logic_error);
}

// Accessing a temporary program result transfers the interval before destruction.
TEST_F(SystemTest, TemporaryExecutionResultAccessOwnsWindow) {
    auto program = system.program6502();
    const auto& window = program.within(ExecutionBudget).run([](auto& p) { p.lda(imm(0x42)); }).window();
    EXPECT_EQ(window.source(), system.observation_source());
    EXPECT_EQ(window.completed(), system.time());
}

TEST_F(SystemTest, ProgramBuilder6809UsesItsEncodingAndStack) {
    auto test = system.program6809();
    const auto answer = test.byte("answer");
    const auto result = test.within(ExecutionBudget).run([&](auto& p) {
        const auto main = p.label("main");
        p.bra(main);
        const auto subroutine = p.mark("subroutine");
        p.lda(imm(0x42)).sta(answer);
        p.rts();
        p.bind(main);
        p.jsr(subroutine);
    });
    EXPECT_EQ(result[answer], 0x42);
    EXPECT_EQ(system.snapshot().cpu_selection_o, std::to_underlying(CPU_SOFT_6809));
    // The builder's ORCC/LDS prologue masks interrupts and initializes S to $0200.
    const std::array<uint8_t, 6> prologue{0x1a, 0x50, 0x10, 0xce, 0x02, 0x00};
    for (size_t i = 0; i < prologue.size(); ++i)
        EXPECT_EQ(system.peek(econopet::SramAddress{0x0300 + i}), prologue[i]);
    EXPECT_EQ(system.peek(econopet::SramAddress{0xfffe}), 0x03);
    EXPECT_EQ(system.peek(econopet::SramAddress{0xffff}), 0x00);
}

TEST_F(SystemTest, ProgramBuilder6502InitializesStackForSubroutines) {
    auto test = system.program6502();
    const auto answer = test.byte("answer");
    const auto result = test.within(ExecutionBudget).run([&](auto& p) {
        const auto main = p.label("main");
        p.jmp(main);
        const auto subroutine = p.mark("subroutine");
        p.lda(imm(0x37)).sta(answer);
        p.rts();
        p.bind(main);
        p.jsr(subroutine);
    });
    EXPECT_EQ(result[answer], 0x37);
    const std::array<uint8_t, 4> prologue{0x78, 0xa2, 0xff, 0x9a};
    for (size_t i = 0; i < prologue.size(); ++i)
        EXPECT_EQ(system.peek(econopet::SramAddress{0x0300 + i}), prologue[i]);
    EXPECT_EQ(system.peek(econopet::SramAddress{0xfffc}), 0x00);
    EXPECT_EQ(system.peek(econopet::SramAddress{0xfffd}), 0x03);
}

TEST_F(SystemTest, ProgramBuilderRejectsLayoutBeforeMutation) {
    const auto before = memory_image(system);
    const auto started = system.time();
    LayoutOverrides overrides;
    overrides.code = MemoryRegion{econopet::CpuAddress{0x0200}, 0x100};
    EXPECT_THROW(system.program6502(Layout::Pet, overrides), std::invalid_argument);
    overrides = {};
    overrides.vectors = MemoryRegion{econopet::CpuAddress{0xfffa}, 2};
    EXPECT_THROW(system.program6502(Layout::Pet, overrides), std::invalid_argument);
    overrides = {};
    overrides.code = MemoryRegion{econopet::CpuAddress{0xffff}, 2};
    EXPECT_THROW(system.program6809(Layout::Pet, overrides), std::out_of_range);
    overrides = {};
    overrides.stack = MemoryRegion{econopet::CpuAddress{0x0400}, 0x100};
    EXPECT_THROW(system.program6502(Layout::Pet, overrides), std::invalid_argument);
    EXPECT_THROW(system.program6502(static_cast<Layout>(99)), std::invalid_argument);
    EXPECT_EQ(system.time(), started);
    EXPECT_EQ(memory_image(system), before);
}

TEST_F(SystemTest, ProgramBuilderValidatesLabelsAndCompletionSpaceBeforeMutation) {
    const auto before = memory_image(system);
    const auto started = system.time();
    auto unresolved = system.program6502();
    EXPECT_THROW(unresolved.within(ExecutionBudget).run([](auto& p) {
        p.jmp(p.label("missing"));
    }), std::logic_error);
    LayoutOverrides overrides;
    // 6502 prologue (4) and terminal sequence (8) need 12 bytes even for an empty body.
    overrides.code = MemoryRegion{econopet::CpuAddress{0x0300}, 11};
    auto overflow = system.program6502(Layout::Pet, overrides);
    EXPECT_THROW(overflow.within(ExecutionBudget).run([](auto&) {}), std::out_of_range);
    auto branch = system.program6809();
    EXPECT_THROW(branch.within(ExecutionBudget).run([](auto& p) {
        const auto target = p.label("too_far");
        p.bra(target);
        const std::vector<uint8_t> padding(128, 0x12);
        p.raw_bytes(padding);
        p.bind(target);
    }), std::out_of_range);
    EXPECT_EQ(system.time(), started);
    EXPECT_EQ(memory_image(system), before);
}

TEST_F(SystemTest, ProgramBuilderSupportsExactCodeBoundaryOverrides) {
    LayoutOverrides overrides;
    overrides.code = MemoryRegion{econopet::CpuAddress{0x0400}, 17};
    overrides.results = MemoryRegion{econopet::CpuAddress{0x0280}, 1};
    overrides.completion = econopet::CpuAddress{0x0281};
    auto test = system.program6502(Layout::Pet, overrides);
    const auto answer = test.byte("answer");
    const auto result = test.within(ExecutionBudget).run([&](auto& p) {
        p.lda(imm(0x81)).sta(answer);
    });
    EXPECT_EQ(result[answer], 0x81);
    EXPECT_EQ(system.peek(econopet::SramAddress{0xfffc}), 0x00);
    EXPECT_EQ(system.peek(econopet::SramAddress{0xfffd}), 0x04);
    EXPECT_EQ(system.peek(econopet::SramAddress{0x0411}), System::IdleRamByte);
}

TEST_F(SystemTest, ProgramBuilderChecksNamedByteOwnershipAndCapacity) {
    LayoutOverrides overrides;
    overrides.results = MemoryRegion{econopet::CpuAddress{0x0200}, 1};
    auto test = system.program6502(Layout::Pet, overrides);
    EXPECT_THROW(test.byte(""), std::invalid_argument);
    const auto answer = test.byte("answer");
    EXPECT_THROW(test.byte("answer"), std::invalid_argument);
    EXPECT_THROW(test.byte("overflow"), std::out_of_range);
    auto peer = system.program6502();
    const auto foreign = peer.byte("answer");
    const auto started = system.time();
    const auto before = memory_image(system);
    EXPECT_THROW(peer.within(ExecutionBudget).run([&](auto& p) { p.sta(answer); }), std::invalid_argument);
    EXPECT_EQ(system.time(), started);
    EXPECT_EQ(memory_image(system), before);
    const auto result = test.within(ExecutionBudget).run([&](auto& p) {
        p.lda(imm(0x42)).sta(answer);
    });
    EXPECT_EQ(result[answer], 0x42);
    EXPECT_THROW(result[foreign], std::invalid_argument);
}

TEST_F(SystemTest, ProgramBuilderCallbackUnwindNeverInstallsOrStarts) {
    auto test = system.program6502();
    const auto before = memory_image(system);
    const auto started = system.time();
    bool assembled = false;
    try {
        test.within(ExecutionBudget).run([&](auto& p) {
            p.lda(imm(0x42));
            assembled = true;
            throw AssemblySentinel{};
        });
        FAIL() << "assembly sentinel was not propagated";
    } catch (const AssemblySentinel&) {
        EXPECT_TRUE(assembled);
    }
    EXPECT_EQ(system.time(), started);
    EXPECT_EQ(memory_image(system), before);
    EXPECT_THROW(test.run([](auto&) {}), std::logic_error);
}

TEST_F(SystemTest, ProgramBuilderRejectsRecursiveExecutionAndLateAllocation) {
    auto test = system.program6502();
    test.within(ExecutionBudget).run([&](auto&) {
        EXPECT_THROW(test.run([](auto&) {}), std::logic_error);
        EXPECT_THROW(test.byte("late"), std::logic_error);
        EXPECT_THROW(test.within(1), std::logic_error);
    });
}

TEST_F(SystemTest, ProgramBuilderCannotAcceptStaleCompletionOnRestart) {
    auto first = system.program6502();
    const auto first_answer = first.byte("answer");
    const auto first_result = first.within(ExecutionBudget).run([&](auto& p) {
        p.lda(imm(0x42)).sta(first_answer);
    });
    EXPECT_EQ(first_result[first_answer], 0x42);
    auto second = system.program6809();
    const auto second_answer = second.byte("answer");
    const auto second_result = second.within(ExecutionBudget).run([&](auto& p) {
        p.lda(imm(0x24)).sta(second_answer);
    });
    EXPECT_EQ(second_result[second_answer], 0x24);
    EXPECT_EQ(first_result[first_answer], 0x42);
    auto looping = system.program6502();
    const auto unused = looping.byte("unused");
    const auto started = system.time();
    EXPECT_THROW(looping.within(ExecutionBudget).run([](auto& p) {
        const auto park = p.mark("never_complete");
        p.emit(test_asm::mos6502::jmp_abs(park));
    }), std::runtime_error);
    EXPECT_EQ(system.time() - started, ExecutionBudget);
    EXPECT_EQ(system.peek(econopet::SramAddress{unused.address().value()}), 0);
}

TEST_F(SystemTest, ProgramBuilderRequiresBudgetAndHonorsZeroStartupDeadline) {
    auto test = system.program6502();
    EXPECT_THROW(test.run([](auto&) {}), std::logic_error);
    bool assembled = false;
    const auto before = memory_image(system);
    const auto started = system.time();
    EXPECT_THROW(test.within(0).run([&](auto&) { assembled = true; }), std::runtime_error);
    EXPECT_TRUE(assembled);
    EXPECT_EQ(system.time(), started);
    EXPECT_EQ(memory_image(system), before);
}

TEST_F(SystemTest, ProgramBuilderSharesStartupDeadlineAndServiceCapture) {
    auto test = system.program6809();
    const auto answer = test.byte("answer");
    size_t services = 0;
    const auto result = test.within(ExecutionBudget).run([&](auto& p) {
        p.lda(imm(0x56)).sta(answer);
    }, [&](System& board, econopet::Cycles remaining) {
        EXPECT_LE(remaining, ExecutionBudget);
        EXPECT_FALSE(board.cpu().peek_reset());
        ++services;
        board.tick(1);
    });
    EXPECT_GT(services, 0);
    EXPECT_EQ(result[answer], 0x56);
    const auto elapsed = result.completed() - result.started();
    System exact_board(system.seed());
    auto exact = exact_board.program6809();
    exact.byte("answer");
    EXPECT_NO_THROW(exact.within(elapsed).run([](auto& p) {
        p.lda(imm(0x56)).sta(econopet::CpuAddress{0x0200});
    }));
    System short_board(system.seed());
    auto short_run = short_board.program6809();
    short_run.byte("answer");
    const auto started = short_board.time();
    EXPECT_THROW(short_run.within(elapsed - econopet::Cycles{1}).run([](auto& p) {
        p.lda(imm(0x56)).sta(econopet::CpuAddress{0x0200});
    }), std::runtime_error);
    EXPECT_EQ(short_board.time() - started, elapsed - econopet::Cycles{1});
}

TEST_F(SystemTest, ProgramBuilderCaptureOutlivesItsBoardAndBuilder) {
    const auto captured = [&] {
        System local(system.seed());
        auto test = local.program6502();
        const auto answer = test.byte("answer");
        auto result = test.within(ExecutionBudget).run([&](auto& p) {
            p.lda(imm(0x64)).sta(answer);
        });
        return std::make_pair(answer, std::move(result));
    }();
    EXPECT_EQ(captured.second[captured.first], 0x64);
}

// Explicit execution validates all layout, completion and relocation contracts before clocks.
TEST_F(SystemTest, ScenarioExecutionRejectsInvalidProgramsBeforeMutation) {
    using namespace test_asm;
    const auto before = memory_image(system);
    const auto started = system.time();
    Scenario layout(system), other(system);
    const auto code = layout.reserve("code", RegionKind::Code, econopet::CpuAddress{0x300}, 0x100);
    const auto marker = layout.completion(
        layout.reserve("completion", RegionKind::Mailbox, econopet::CpuAddress{0x2ff}, 1));
    const auto data = layout.reserve("data", RegionKind::Data, econopet::CpuAddress{0x400}, 2);
    mos6502::Program unfinished(layout, code);
    EXPECT_THROW(layout.run(unfinished, ExecutionBudget, "missing finish"), std::logic_error);
    unfinished.finish(marker);
    EXPECT_THROW(layout.run(unfinished, ExecutionBudget, "missing vector"), std::invalid_argument);
    layout.reserve("vectors", RegionKind::Vectors, pet::Reset6502, 2);
    EXPECT_THROW(other.run(unfinished, ExecutionBudget, "foreign program"), std::invalid_argument);
    EXPECT_THROW(layout.run(unfinished, code, ExecutionBudget, "wrong capture purpose"), std::invalid_argument);
    const auto foreign_data = other.reserve("data", RegionKind::Data, econopet::CpuAddress{0x400}, 2);
    EXPECT_THROW(layout.run(unfinished, foreign_data, ExecutionBudget, "foreign capture"), std::invalid_argument);
    mos6502::Program unresolved(layout, code);
    unresolved.jmp(unresolved.label("unbound"));
    unresolved.finish(marker);
    EXPECT_THROW(layout.run(unresolved, data, ExecutionBudget, "unbound label"), std::logic_error);
    auto moved = std::move(unfinished);
    EXPECT_THROW(layout.run(unfinished, ExecutionBudget, "moved program"), std::logic_error);
    EXPECT_EQ(system.time(), started);
    EXPECT_EQ(memory_image(system), before);
    EXPECT_NO_THROW(layout.run(moved, data, ExecutionBudget, "valid moved program"));
}

// Factories and direct builders must not escape observation-only execution contexts.
TEST_F(SystemTest, ProgramBuilderRespectsObservationOnlyGuards) {
    auto test = system.program6502();
    test.within(ExecutionBudget);
    const auto before = memory_image(system);
    const auto started = system.time();
    system.run_until([&](const System&) {
        EXPECT_THROW(system.program6502(), std::logic_error);
        EXPECT_THROW(system.program6809(), std::logic_error);
        EXPECT_THROW(test.byte("forbidden"), std::logic_error);
        EXPECT_THROW(test.run([](auto&) {}), std::logic_error);
        return true;
    }, 0, "program guards");
    EXPECT_EQ(system.time(), started);
    EXPECT_EQ(memory_image(system), before);
    EXPECT_NO_THROW(test.run([](auto&) {}));
}

// Direct heap construction must not let an active builder escape an observation callback.
TEST_F(SystemTest, ProgramBuilderDirectConstructionRejectsObservationAndAllowsActiveUse) {
    using MosBuilder = test_asm::ProgramBuilder<test_asm::Mos6502>;
    using McBuilder = test_asm::ProgramBuilder<test_asm::Mc6809>;
    std::unique_ptr<MosBuilder> mos;
    std::unique_ptr<McBuilder> mc;
    const auto before = memory_image(system);
    const auto started = system.time();
    const auto reject_construction = [&] {
        EXPECT_THROW(mos = std::make_unique<MosBuilder>(system, Layout::Pet), std::logic_error);
        EXPECT_THROW(mc = std::make_unique<McBuilder>(system, Layout::Pet), std::logic_error);
        EXPECT_FALSE(mos);
        EXPECT_FALSE(mc);
    };
    system.run_until([&](const System&) {
        reject_construction();
        return true;
    }, 0, "direct builder predicate guard");
    EXPECT_EQ(system.time(), started);
    EXPECT_EQ(memory_image(system), before);
    size_t observations = 0;
    auto subscription = system.observe([&](const System&) {
        ++observations;
        reject_construction();
    });
    system.tick(1);
    EXPECT_EQ(observations, 1u);
    EXPECT_EQ(memory_image(system), before);
    subscription.reset();
    mos = std::make_unique<MosBuilder>(system, Layout::Pet);
    mc = std::make_unique<McBuilder>(system, Layout::Pet);
    const auto answer = mos->byte("answer");
    const auto result = mos->within(ExecutionBudget).run([&](auto& p) {
        p.lda(imm(0x42)).sta(answer);
    });
    EXPECT_EQ(result[answer], 0x42);
    EXPECT_NO_THROW(mc->within(ExecutionBudget).run([](auto&) {}));
}

// Restricted assembly preserves the Program's CPU, reservation and cursor diagnostics.
TEST_F(SystemTest, ProgramAssemblyDelegatesDataFailureDiagnostics) {
    const auto check = [&](auto& test, const std::string& cpu) {
        const auto buffer = test.buffer("packet", 2);
        const auto start = system.time();
        const std::array<uint8_t, 1> short_data{0x42};
        try {
            test.within(ExecutionBudget).run([&](auto& p) { p.store(buffer, short_data); });
            FAIL() << "short store unexpectedly succeeded";
        } catch (const std::out_of_range& error) {
            const std::string message = error.what();
            EXPECT_NE(message.find("program_cpu=" + cpu), std::string::npos);
            EXPECT_NE(message.find("code_address=768"), std::string::npos);
            EXPECT_NE(message.find("capacity=256"), std::string::npos);
            EXPECT_NE(message.find("cursor="), std::string::npos);
        }
        EXPECT_EQ(system.time(), start);
    };
    auto mos = system.program6502();
    check(mos, "soft6502");
    auto mc = system.program6809();
    check(mc, "soft6809");
}

TEST(ProgramOperands, ImmediateBytesAndCpuInstructionsAreChecked) {
    EXPECT_EQ(imm(0).value, 0);
    EXPECT_EQ(imm(255).value, 255);
    EXPECT_THROW(imm(-1), std::out_of_range);
    EXPECT_THROW(imm(256), std::out_of_range);
    static_assert(!std::is_constructible_v<test_asm::Instruction<test_asm::Mos6502, 2>,
        test_asm::Instruction<test_asm::Mc6809, 2>>);
    static_assert(!std::is_move_constructible_v<test_asm::ProgramBuilder<test_asm::Mos6502>>);
}

TEST(ProgramOperands, FluentSurfacesPreserveCpuAndOperandTypes) {
    using P6502 = test_asm::mos6502::Program<System>;
    using P6809 = test_asm::mc6809::Program<System>;
    using A6502 = test_asm::Assembly<test_asm::Mos6502, System>;
    using A6809 = test_asm::Assembly<test_asm::Mc6809, System>;
    static_assert(HasSei<P6502>::value && HasSei<A6502>::value);
    static_assert(!HasSei<P6809>::value && !HasSei<A6809>::value);
    static_assert(HasOrcc<P6809>::value && HasOrcc<A6809>::value);
    static_assert(!HasOrcc<P6502>::value && !HasOrcc<A6502>::value);
    static_assert(CanJsr<P6502, test_asm::mos6502::Label>::value);
    static_assert(!CanJsr<P6502, test_asm::mc6809::Label>::value);
    static_assert(!CanJsr<A6809, test_asm::mos6502::Label>::value);
    static_assert(CanLda<A6502, test_asm::ZeroPage>::value);
    static_assert(!CanLda<A6809, test_asm::ZeroPage>::value);
    static_assert(!CanLda<A6502, int>::value && !CanLda<A6809, int>::value);
    static_assert(!CanLda<A6502, test_asm::ImmediateWord>::value);
    static_assert(std::is_same_v<decltype(std::declval<P6502&>().sei().lda(imm(0))), P6502&>);
    static_assert(std::is_same_v<decltype(std::declval<A6809&>().orcc(imm(0)).rts()), A6809&>);
    EXPECT_EQ(test_asm::imm16(65535).value, 65535);
    EXPECT_EQ(test_asm::imm16(econopet::CpuAddress{0x1234}).value, 0x1234);
    EXPECT_THROW(test_asm::imm16(-1), std::out_of_range);
    EXPECT_THROW(test_asm::imm16(65536), std::out_of_range);
    EXPECT_THROW(test_asm::zp(econopet::CpuAddress{0x100}), std::out_of_range);
    EXPECT_THROW(test_asm::dp(econopet::CpuAddress{0x100}), std::out_of_range);
    EXPECT_EQ(test_asm::mc6809::clr_indexed_offset(-1).bytes[1], 0x1f);
    EXPECT_EQ(test_asm::mc6809::clr_indexed_offset(0).bytes[1], 0x00);
    EXPECT_THROW(test_asm::mc6809::clr_indexed_offset(0x84), std::out_of_range);
    EXPECT_THROW(test_asm::mc6809::clr_indexed_offset(std::numeric_limits<uint64_t>::max()),
        std::out_of_range);
    EXPECT_THROW(test_asm::mc6809::clr_indexed_offset(std::numeric_limits<int64_t>::min()),
        std::out_of_range);
    EXPECT_EQ(test_asm::mc6809::raw_clr_indexed_postbyte(0x84).bytes[1], 0x84);
}

// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <array>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "framework/asm.h"
#include "host_memory.h"

namespace {
using namespace test_asm;
using econopet::CpuAddress;
using P6502 = mos6502::Program<HostMemory>;
using P6809 = mc6809::Program<HostMemory>;

template<class P, class I>
concept CanEmit = requires(P& p, const I& instruction) { p.emit(instruction); };
template<class P, class Operand>
concept CanLoad = requires(P& p, Operand operand) { p.lda(operand); };
template<class P, class Operand>
concept CanCall = requires(P& p, Operand operand) { p.jsr(operand); };
template<class P>
concept PublicDataGuard = requires(P& p, const MemoryByte& byte) { p.require_data(byte); };
template<class P>
concept PublicDataFailure = requires(P& p) {
    p.template data_failure<std::logic_error>("operation", "detail");
};
static_assert(CanEmit<P6502, mos6502::Instruction<1>>);
static_assert(CanEmit<P6809, mc6809::Instruction<1>>);
static_assert(!CanEmit<P6502, mc6809::Instruction<1>>);
static_assert(!CanEmit<P6809, mos6502::Instruction<1>>);
static_assert(!CanEmit<P6502, LabelInstruction<Mc6809>>);
static_assert(!CanEmit<P6809, LabelInstruction<Mos6502>>);
static_assert(!CanLoad<P6502, int>);
static_assert(!CanLoad<P6502, ImmediateWord>);
static_assert(!CanLoad<P6809, ZeroPage>);
static_assert(!CanLoad<P6502, econopet::SramAddress>);
static_assert(!CanLoad<P6809, econopet::WishboneAddress>);
static_assert(!CanCall<P6502, Label<Mc6809>>);
static_assert(!CanCall<P6809, Label<Mos6502>>);
static_assert(!PublicDataGuard<P6502>);
static_assert(!PublicDataGuard<P6809>);
static_assert(!PublicDataFailure<P6502>);
static_assert(!PublicDataFailure<P6809>);
static_assert(std::is_same_v<P6502, Program<Mos6502, HostMemory>>);
static_assert(std::is_same_v<P6809, Program<Mc6809, HostMemory>>);
static_assert(!std::is_copy_constructible_v<P6502>);
static_assert(std::is_move_constructible_v<P6502>);
static_assert(!std::is_constructible_v<P6502, Scenario<HostMemory>&&, const Region&>);
static_assert(imm(255).value == 255);
static_assert(imm16(65535).value == 65535);
static_assert(mc6809::bra(-128).bytes[1] == 0x80);
static_assert(mc6809::clr_indexed_offset(-16).bytes[1] == 0x10);

// Read only the emitted part of a code reservation after installation.
template<class P>
std::vector<uint8_t> installed(HostMemory& memory, P& program) {
    program.install();
    const auto start = program.entry().value();
    return {memory.bytes.begin() + start, memory.bytes.begin() + start + program.size()};
}
}

TEST(CpuAssembly, TypedOperandsRejectNarrowingAndImplicitAddressModes) {
    EXPECT_THROW(imm(-1), std::out_of_range);
    EXPECT_THROW(imm(256), std::out_of_range);
    EXPECT_THROW(imm(std::numeric_limits<uint64_t>::max()), std::out_of_range);
    EXPECT_THROW(imm16(-1), std::out_of_range);
    EXPECT_THROW(imm16(65536), std::out_of_range);
    EXPECT_THROW(imm16(std::numeric_limits<uint64_t>::max()), std::out_of_range);
    EXPECT_EQ(imm16(CpuAddress{0xffff}).value, 0xffff);
    EXPECT_EQ(zp(CpuAddress{0xff}).address(), CpuAddress{0xff});
    EXPECT_EQ(dp(CpuAddress{0xff}).offset(), CpuAddress{0xff});
    EXPECT_THROW(zp(CpuAddress{0x100}), std::out_of_range);
    EXPECT_THROW(dp(CpuAddress{0x100}), std::out_of_range);
    EXPECT_THROW(mos6502::lda_zp(CpuAddress{0x100}), std::out_of_range);
    EXPECT_THROW(mos6502::sta_zp(CpuAddress{0x100}), std::out_of_range);
    EXPECT_THROW(mc6809::stx_dp(CpuAddress{0x100}), std::out_of_range);
}

TEST(CpuAssembly, Encodes6502FluentSurfaceExactly) {
    HostMemory memory;
    Scenario layout(memory);
    P6502 program(layout, layout.reserve("code", RegionKind::Code, CpuAddress{0x300}, 128));
    program.sei().cli().txs().rti().rts()
        .lda(imm(0x42)).ldx(imm(0xff))
        .lda(zp(CpuAddress{0x80})).sta(zp(CpuAddress{0x81}))
        .lda(CpuAddress{0x12}).sta(CpuAddress{0x1234})
        .jsr(CpuAddress{0xabcd}).jmp(CpuAddress{0xffff});
    EXPECT_EQ(memory.writes, 0u);
    EXPECT_EQ(installed(memory, program), (std::vector<uint8_t>{
        0x78, 0x58, 0x9a, 0x40, 0x60, 0xa9, 0x42, 0xa2, 0xff,
        0xa5, 0x80, 0x85, 0x81, 0xad, 0x12, 0x00, 0x8d, 0x34, 0x12,
        0x20, 0xcd, 0xab, 0x4c, 0xff, 0xff}));
}

TEST(CpuAssembly, Encodes6809FluentSurfaceExactly) {
    HostMemory memory;
    Scenario layout(memory);
    P6809 program(layout, layout.reserve("code", RegionKind::Code, CpuAddress{0x300}, 128));
    program.lda(imm(0x42)).ldb(imm(0xff)).ldd(imm16(0x1234)).ldx(imm16(0xabcd))
        .lds(imm16(0x200)).orcc(imm(0x50)).lda(CpuAddress{0x12})
        .sta(CpuAddress{0x1234}).stb(CpuAddress{0xabcd}).stx(dp(CpuAddress{0xff}))
        .jsr(CpuAddress{0xcdef}).bra_offset(-2).rts().clr_x()
        .clr_x_offset(-16).clr_x_offset(15).raw_clr_indexed_postbyte(0x89);
    EXPECT_EQ(installed(memory, program), (std::vector<uint8_t>{
        0x86, 0x42, 0xc6, 0xff, 0xcc, 0x12, 0x34, 0x8e, 0xab, 0xcd,
        0x10, 0xce, 0x02, 0x00, 0x1a, 0x50, 0xb6, 0x00, 0x12,
        0xb7, 0x12, 0x34, 0xf7, 0xab, 0xcd, 0x9f, 0xff,
        0xbd, 0xcd, 0xef, 0x20, 0xfe, 0x39, 0x6f, 0x84,
        0x6f, 0x10, 0x6f, 0x0f, 0x6f, 0x89}));
    EXPECT_EQ(mc6809::lds_imm(CpuAddress{0xabcd}).bytes,
        (std::array<uint8_t, 4>{0x10, 0xce, 0xab, 0xcd}));
    EXPECT_EQ(mc6809::ldx_imm(CpuAddress{0xabcd}).bytes,
        (std::array<uint8_t, 3>{0x8e, 0xab, 0xcd}));
}

TEST(CpuAssembly, IndexedAndLiteralBranchesEnforceEveryBoundary) {
    EXPECT_EQ(mc6809::bra(-128).bytes[1], 0x80);
    EXPECT_EQ(mc6809::bra(127u).bytes[1], 0x7f);
    EXPECT_THROW(mc6809::bra(-129), std::out_of_range);
    EXPECT_THROW(mc6809::bra(128u), std::out_of_range);
    EXPECT_THROW(mc6809::bra(std::numeric_limits<uint64_t>::max()), std::out_of_range);
    EXPECT_EQ(mc6809::clr_indexed_offset(-16).bytes[1], 0x10);
    EXPECT_EQ(mc6809::clr_indexed_offset(15u).bytes[1], 0x0f);
    EXPECT_THROW(mc6809::clr_indexed_offset(-17), std::out_of_range);
    EXPECT_THROW(mc6809::clr_indexed_offset(16u), std::out_of_range);
    EXPECT_THROW(mc6809::clr_indexed_offset(std::numeric_limits<uint64_t>::max()), std::out_of_range);
}

TEST(CpuAssembly, LabelsResolveCallsJumpsAndBranchesForBothCpus) {
    HostMemory memory;
    Scenario layout(memory);
    P6502 p(layout, layout.reserve("6502", RegionKind::Code, CpuAddress{0x1234}, 16));
    const auto target = p.label("target");
    p.jsr(target).jmp(target).beq(target).lda(imm(0));
    p.bind(target);
    p.rts();
    EXPECT_EQ(p.address(target), CpuAddress{0x123e});
    EXPECT_EQ(installed(memory, p), (std::vector<uint8_t>{
        0x20, 0x3e, 0x12, 0x4c, 0x3e, 0x12, 0xf0, 0x02, 0xa9, 0, 0x60}));
    P6809 q(layout, layout.reserve("6809", RegionKind::Code, CpuAddress{0x2345}, 16));
    const auto begin = q.mark("begin");
    const auto end = q.label("end");
    q.jsr(end).bra(end).bra(begin);
    q.bind(end);
    q.rts();
    EXPECT_EQ(installed(memory, q), (std::vector<uint8_t>{
        0xbd, 0x23, 0x4c, 0x20, 0x02, 0x20, 0xf9, 0x39}));
}

TEST(CpuAssembly, LabelValidationRejectsForeignDuplicateAndUnresolvedTargets) {
    HostMemory memory;
    Scenario layout(memory);
    const auto code = layout.reserve("code", RegionKind::Code, CpuAddress{0x300}, 32);
    P6502 p(layout, code);
    P6502 other(layout, code);
    const auto foreign = other.mark("same");
    const auto label = p.label("same");
    EXPECT_THROW(p.label(""), std::invalid_argument);
    EXPECT_THROW(p.label("same"), std::invalid_argument);
    EXPECT_THROW(p.bind(foreign), std::invalid_argument);
    EXPECT_THROW(p.address(foreign), std::invalid_argument);
    EXPECT_THROW(p.jmp(foreign), std::invalid_argument);
    EXPECT_THROW(p.address(label), std::logic_error);
    p.jmp(label);
    EXPECT_THROW(p.install(), std::logic_error);
    EXPECT_EQ(memory.writes, 0u);
    p.bind(label);
    EXPECT_THROW(p.bind(label), std::logic_error);
    const auto before = p.size();
    EXPECT_THROW(p.emit(LabelInstruction<Mos6502>{0x4c, label, static_cast<Relocation>(99)}),
        std::invalid_argument);
    EXPECT_THROW(p.emit(LabelInstruction<Mos6502>{0x4c, label, Relocation::BigEndian16}),
        std::invalid_argument);
    EXPECT_EQ(p.size(), before);
    EXPECT_NO_THROW(p.install());
    const auto moved_label = p.label("moved");
    auto source = moved_label;
    const auto destination = std::move(source);
    EXPECT_THROW(p.bind(source), std::invalid_argument);
    EXPECT_NO_THROW(p.bind(destination));
}

TEST(CpuAssembly, RelativeFixupsEnforceSignedLimitsBeforeAnyInstallation) {
    for (const bool is6502 : {true, false}) {
        for (const int displacement : {-129, -128, 127, 128}) {
            HostMemory memory;
            Scenario layout(memory);
            const auto code = layout.reserve("code", RegionKind::Code, CpuAddress{0x300}, 300);
            const auto exercise = [&](auto& p) {
                if (displacement < 0) {
                    const auto target = p.mark("target");
                    std::vector<uint8_t> padding(static_cast<size_t>(-displacement - 2), 0);
                    p.raw_bytes(padding);
                    if constexpr (std::is_same_v<typename std::remove_reference_t<decltype(p)>::CpuType, Mos6502>)
                        p.beq(target);
                    else p.bra(target);
                } else {
                    const auto target = p.label("target");
                    if constexpr (std::is_same_v<typename std::remove_reference_t<decltype(p)>::CpuType, Mos6502>)
                        p.beq(target);
                    else p.bra(target);
                    std::vector<uint8_t> padding(static_cast<size_t>(displacement), 0);
                    p.raw_bytes(padding);
                    p.bind(target);
                }
                if (displacement < -128 || displacement > 127) {
                    EXPECT_THROW(p.install(), std::out_of_range);
                    EXPECT_EQ(memory.writes, 0u);
                } else {
                    EXPECT_NO_THROW(p.install());
                    const auto operand = displacement < 0 ? static_cast<size_t>(-displacement - 1) : 1;
                    EXPECT_EQ(memory.bytes[code.start().value() + operand], static_cast<uint8_t>(displacement));
                }
            };
            if (is6502) { P6502 p(layout, code); exercise(p); }
            else { P6809 p(layout, code); exercise(p); }
        }
    }
}

TEST(CpuAssembly, CodeBoundsIncludeLastCpuByteAndRejectOnePastCursor) {
    HostMemory memory;
    Scenario layout(memory);
    const auto code = layout.reserve("last byte", RegionKind::Code, CpuAddress{0xffff}, 1);
    P6502 p(layout, code);
    EXPECT_THROW(p.lda(imm(0)), std::out_of_range);
    EXPECT_EQ(p.size(), 0u);
    p.rts();
    EXPECT_EQ(p.size(), 1u);
    EXPECT_THROW(p.address(), std::out_of_range);
    EXPECT_THROW(p.mark("past"), std::out_of_range);
    EXPECT_THROW(p.rts(), std::out_of_range);
    EXPECT_EQ(installed(memory, p), (std::vector<uint8_t>{0x60}));
    const std::array<uint8_t, 0> empty{};
    EXPECT_NO_THROW(p.raw_bytes(empty));
    const auto data = layout.reserve("data", RegionKind::Data, CpuAddress{0x200}, 1);
    EXPECT_THROW(P6502 bad(layout, data), std::invalid_argument);
    Scenario foreign(memory);
    EXPECT_THROW(P6502 bad(foreign, code), std::invalid_argument);
}

TEST(CpuAssembly, MoveTransfersLabelsAndRejectsAllSourceOperations) {
    HostMemory memory;
    Scenario layout(memory);
    const auto code = layout.reserve("code", RegionKind::Code, CpuAddress{0x300}, 16);
    P6502 source(layout, code);
    const auto label = source.label("target");
    source.jmp(label);
    P6502 destination(std::move(source));
    EXPECT_THROW(source.rts(), std::logic_error);
    EXPECT_THROW(source.install(), std::logic_error);
    EXPECT_THROW(source.label("new"), std::logic_error);
    EXPECT_THROW(source.bind(label), std::logic_error);
    EXPECT_THROW(source.address(), std::logic_error);
    EXPECT_THROW(source.entry(), std::logic_error);
    EXPECT_THROW(source.size(), std::logic_error);
    destination.bind(label);
    destination.rts();
    EXPECT_NO_THROW(destination.install());
    Scenario transferred(std::move(layout));
    EXPECT_THROW(destination.install(), std::logic_error);
}

TEST(CpuAssembly, OwnedStoresValidateEntireOperationBeforeAppending) {
    HostMemory memory;
    Scenario layout(memory);
    const auto data = layout.reserve("data", RegionKind::Data, CpuAddress{0xff}, 3);
    auto view = layout.lower_bank(data);
    const auto word = view.word("word", 0, ByteOrder::LittleEndian);
    const auto buffer = view.buffer("buffer", 0, 3);
    P6502 p(layout, layout.reserve("code", RegionKind::Code, CpuAddress{0x300}, 9));
    EXPECT_THROW(p.store(word, imm16(0x1234)), std::out_of_range);
    EXPECT_THROW(p.store_zp(word, CpuAddress{0x1234}), std::out_of_range);
    const std::array<uint8_t, 3> bytes{1, 2, 3};
    EXPECT_THROW(p.store(buffer, bytes), std::out_of_range);
    const std::array<uint8_t, 1> short_data{1};
    EXPECT_THROW(p.store(buffer, short_data), std::out_of_range);
    EXPECT_EQ(p.size(), 0u);
    Scenario other(memory);
    auto foreign = other.lower_bank(other.reserve("foreign", RegionKind::Data, CpuAddress{0}, 2));
    EXPECT_THROW(p.lda(foreign.byte("foreign")), std::invalid_argument);
    EXPECT_THROW(p.sta(foreign.byte("foreign")), std::invalid_argument);
    EXPECT_THROW(p.store(foreign.word("foreign", 0, ByteOrder::BigEndian), imm16(0)), std::invalid_argument);
    EXPECT_EQ(p.size(), 0u);
    EXPECT_EQ(memory.writes, 0u);
}

TEST(CpuAssembly, OwnedWordAndBufferStoresRespectDeclaredOrder) {
    for (const bool is6502 : {true, false}) {
        for (const auto order : {ByteOrder::LittleEndian, ByteOrder::BigEndian}) {
            HostMemory memory;
            Scenario layout(memory);
            const auto code = layout.reserve("code", RegionKind::Code, CpuAddress{0x300}, 32);
            auto view = layout.lower_bank(layout.reserve("data", RegionKind::Data, CpuAddress{0x200}, 4));
            const auto word = view.word("word", 0, order);
            const auto buffer = view.buffer("buffer", 2, 2);
            const std::array<uint8_t, 2> bytes{0x42, 0xff};
            const auto exercise = [&](auto& p) {
                p.store(word, CpuAddress{0x1234}).store(buffer, bytes);
                const auto encoded = installed(memory, p);
                const uint8_t low = order == ByteOrder::LittleEndian ? 0x34 : 0x12;
                const uint8_t high = order == ByteOrder::LittleEndian ? 0x12 : 0x34;
                if (is6502) EXPECT_EQ(encoded, (std::vector<uint8_t>{
                    0xa9, low, 0x8d, 0, 2, 0xa9, high, 0x8d, 1, 2,
                    0xa9, 0x42, 0x8d, 2, 2, 0xa9, 0xff, 0x8d, 3, 2}));
                else EXPECT_EQ(encoded, (std::vector<uint8_t>{
                    0x86, low, 0xb7, 2, 0, 0x86, high, 0xb7, 2, 1,
                    0x86, 0x42, 0xb7, 2, 2, 0x86, 0xff, 0xb7, 2, 3}));
                EXPECT_EQ(memory.bytes[0x200], 0u);
            };
            if (is6502) { P6502 p(layout, code); exercise(p); }
            else { P6809 p(layout, code); exercise(p); }
        }
    }
}

TEST(CpuAssembly, RawBytesStayExplicitAndInstallRepeatedlyWithoutExtraWrites) {
    HostMemory memory;
    Scenario layout(memory);
    P6809 p(layout, layout.reserve("code", RegionKind::Code, CpuAddress{0x300}, 4));
    const std::array<uint8_t, 4> raw{0x10, 0xff, 0x00, 0x01};
    p.raw_bytes(raw);
    EXPECT_EQ(memory.writes, 0u);
    EXPECT_EQ(installed(memory, p), (std::vector<uint8_t>{0x10, 0xff, 0, 1}));
    EXPECT_EQ(memory.writes, 4u);
    EXPECT_NO_THROW(p.install());
    EXPECT_EQ(memory.writes, 8u);
}

TEST(CpuAssembly, ExplicitZeroPageWordStoresRespectBothOrders) {
    for (const auto order : {ByteOrder::LittleEndian, ByteOrder::BigEndian}) {
        HostMemory memory;
        Scenario layout(memory);
        const auto code = layout.reserve("code", RegionKind::Code, CpuAddress{0x300}, 8);
        auto view = layout.lower_bank(layout.reserve("data", RegionKind::Data, CpuAddress{0xfe}, 2));
        P6502 program(layout, code);
        program.store_zp(view.word("word", 0, order), CpuAddress{0x1234});
        const uint8_t first = order == ByteOrder::LittleEndian ? 0x34 : 0x12;
        const uint8_t second = order == ByteOrder::LittleEndian ? 0x12 : 0x34;
        EXPECT_EQ(installed(memory, program), (std::vector<uint8_t>{
            0xa9, first, 0x85, 0xfe, 0xa9, second, 0x85, 0xff}));
        EXPECT_EQ(program.size(), code.size());
    }
}

TEST(CpuAssembly, FullRegionFixupCanEndAtLastCpuByteWithoutWrapping) {
    HostMemory memory;
    Scenario layout(memory);
    P6809 program(layout, layout.reserve("last bytes", RegionKind::Code, CpuAddress{0xfffe}, 2));
    const auto self = program.mark("self");
    program.bra(self);
    EXPECT_EQ(installed(memory, program), (std::vector<uint8_t>{0x20, 0xfe}));
    EXPECT_THROW(program.address(), std::out_of_range);
    EXPECT_EQ(program.address(self), CpuAddress{0xfffe});
}

TEST(CpuAssembly, LateFixupFailureLeavesEntireBackingUnmodified) {
    HostMemory memory;
    Scenario layout(memory);
    P6502 program(layout, layout.reserve("code", RegionKind::Code, CpuAddress{0x300}, 300));
    const auto near = program.mark("near");
    const auto far = program.label("far");
    program.jsr(near).beq(far);
    std::vector<uint8_t> padding(128, 0);
    program.raw_bytes(padding);
    program.bind(far);
    const auto before = memory.bytes;
    EXPECT_THROW(program.install(), std::out_of_range);
    EXPECT_EQ(memory.bytes, before);
    EXPECT_EQ(memory.writes, 0u);
}

TEST(CpuAssembly, RelativeBranchesWrapAtBothEndsOfCpuAddressSpace) {
    for (const bool is6502 : {true, false}) {
        for (const bool forward : {true, false}) {
            for (const int displacement : {forward ? 0 : -3, forward ? 127 : -128,
                                            forward ? 128 : -129}) {
                HostMemory memory;
                Scenario layout(memory);
                const auto code = layout.reserve("whole CPU space", RegionKind::Code, CpuAddress{0},
                    econopet::CpuAddressCapacity);
                const auto exercise = [&](auto& program) {
                    const auto target = program.label("target");
                    const size_t branch = forward ? 0xfffe : 0;
                    const size_t destination = forward ? static_cast<size_t>(displacement)
                        : static_cast<size_t>(econopet::CpuAddressCapacity + 2 + displacement);
                    if (forward) {
                        std::vector<uint8_t> before_target(destination);
                        program.raw_bytes(before_target);
                        program.bind(target);
                        std::vector<uint8_t> before_branch(branch - destination);
                        program.raw_bytes(before_branch);
                    }

                    if constexpr (std::is_same_v<typename std::remove_reference_t<decltype(program)>::CpuType, Mos6502>)
                        program.beq(target);
                    else program.bra(target);
                    if (!forward) {
                        std::vector<uint8_t> before_target(destination - program.size());
                        program.raw_bytes(before_target);
                        program.bind(target);
                    }
                    if (displacement < -128 || displacement > 127) {
                        EXPECT_THROW(program.install(), std::out_of_range);
                        EXPECT_EQ(memory.writes, 0u);
                    } else {
                        EXPECT_NO_THROW(program.install());
                        EXPECT_EQ(memory.bytes[branch + 1], static_cast<uint8_t>(displacement));
                    }
                };
                if (is6502) { P6502 program(layout, code); exercise(program); }
                else { P6809 program(layout, code); exercise(program); }
            }
        }
    }
}

TEST(CpuAssembly, DocumentationExampleReturnsToJumpAndStoppingLoop) {
    HostMemory memory;
    Scenario layout(memory);
    const auto code = layout.reserve("code", RegionKind::Code, CpuAddress{0x300}, 32);
    mos6502::Program program(layout, code);
    const auto subroutine = program.label("subroutine");
    const auto stopped = program.label("stopped");
    program.sei().ldx(imm(0xff)).txs().jsr(subroutine).jmp(stopped);
    program.bind(subroutine);
    program.lda(imm(0x42)).sta(CpuAddress{0x200}).rts();
    program.bind(stopped);
    program.jmp(stopped);
    EXPECT_EQ(installed(memory, program), (std::vector<uint8_t>{
        0x78, 0xa2, 0xff, 0x9a, 0x20, 0x0a, 0x03, 0x4c, 0x10, 0x03,
        0xa9, 0x42, 0x8d, 0x00, 0x02, 0x60, 0x4c, 0x10, 0x03}));
}

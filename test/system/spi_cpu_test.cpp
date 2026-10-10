// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "system.h"

#include <array>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "framework/system_test.h"

namespace {
using namespace econopet;
constexpr SramAddress RamProbe{0x1234};
constexpr SramAddress BankedProbe{0x12345};
constexpr CpuAddress Result{0x0200};
constexpr CpuAddress Entry6502{0x0300};
constexpr CpuAddress Entry6809{0x0400};
constexpr WishboneAddress LastWishboneAddress{WishboneAddressCapacity - 1};
constexpr Cycles ExecutionBudget{10000};
constexpr Cycles ExecutionSlice{2000};
constexpr uint8_t Marker = 0x42;
constexpr uint8_t OtherMarker = 0x5a;
constexpr unsigned SpiBitsPerByte = 8;
constexpr uint8_t SpiHighBit = 0x80;
constexpr Cycles SpiHalfPeriod{2};
struct UnwindSentinel {};
struct SpiUnwindSentinel : std::runtime_error {
    // Identify an injected failure independently of transport exception categories.
    SpiUnwindSentinel() : std::runtime_error("injected SPI observer failure") {}
};

// Install a tiny native instruction loop without the later assembler or ROM layer.
void install_loop(System& board, cpu_type_t cpu, CpuAddress entry, uint8_t marker) {
    const std::array<uint8_t, 8> mos6502{
        0xa9, marker, 0x8d, static_cast<uint8_t>(Result.value()),
        static_cast<uint8_t>(Result.value() >> 8), 0x4c,
        static_cast<uint8_t>(entry.value()), static_cast<uint8_t>(entry.value() >> 8)};
    const std::array<uint8_t, 7> mc6809{
        0x86, marker, 0xb7, static_cast<uint8_t>(Result.value() >> 8),
        static_cast<uint8_t>(Result.value()), 0x20, 0xf9};
    if (cpu == CPU_SOFT_6502) {
        for (size_t index = 0; index < mos6502.size(); ++index) board.poke(SramAddress{entry.value() + index}, mos6502[index]);
    } else {
        for (size_t index = 0; index < mc6809.size(); ++index) board.poke(SramAddress{entry.value() + index}, mc6809[index]);
    }
}

// Each view independently rejects temporary owners and active const-board work.
template<class Owner>
concept HasSpiView = requires(Owner&& owner) { std::forward<Owner>(owner).spi(); };
template<class Owner>
concept HasCpuView = requires(Owner&& owner) { std::forward<Owner>(owner).cpu(); };
template<class View>
concept HasRead = requires(View& view) { view.read(fpga::Register::Status); };
template<class View>
concept HasControlRead = requires(View& view) { view.read_control(); };
template<class View>
concept HasStart = requires(View& view) { view.start(); };
template<class View>
concept HasByte = requires(View& view) { view.byte(uint8_t{}); };
template<class View>
concept HasFinish = requires(View& view) { view.finish(); };
template<class Result>
struct CallbackResult {
    Result operator()(System::SpiCommands&) const;
};
template<class Result>
concept CanReturnSpiResult = requires(System::SpiView<System>& view) { view.transaction(CallbackResult<Result>{}); };
static_assert(HasSpiView<System&> && HasSpiView<const System&>);
static_assert(!HasSpiView<System> && !HasSpiView<const System>);
static_assert(HasCpuView<System&> && HasCpuView<const System&>);
static_assert(!HasCpuView<System> && !HasCpuView<const System>);
static_assert(HasRead<System::SpiView<System>> && !HasRead<System::SpiView<const System>>);
static_assert(HasControlRead<System::CpuView<System>> && !HasControlRead<System::CpuView<const System>>);
static_assert(HasStart<System::CpuView<System>> && !HasStart<System::CpuView<const System>>);
static_assert(!HasByte<System::SpiCommands> && !HasFinish<System::SpiCommands>);
static_assert(!HasByte<System::SpiView<System>>);
static_assert(!std::is_copy_constructible_v<System::SpiTransaction> && !std::is_move_constructible_v<System::SpiTransaction>);
static_assert(std::is_nothrow_destructible_v<System::SpiTransaction>);
static_assert(CanReturnSpiResult<void> && CanReturnSpiResult<std::unique_ptr<int>>);
static_assert(!CanReturnSpiResult<int&> && !CanReturnSpiResult<const int&>);
static_assert(!CanReturnSpiResult<int*> && !CanReturnSpiResult<std::reference_wrapper<int>>);
static_assert(!CanReturnSpiResult<const std::reference_wrapper<int>>);
static_assert(!std::is_constructible_v<System::SpiView<const System>, System&&>);
static_assert(!std::is_constructible_v<System::SpiView<const System>, const System&&>);
static_assert(!std::is_constructible_v<System::CpuView<const System>, System&&>);
static_assert(!std::is_constructible_v<System::CpuView<const System>, const System&&>);
}

TEST_F(SystemTest, SpiCommandsValidateBeforeMutation) {
    const std::vector<std::vector<uint8_t>> invalid{
        {}, {0x40}, {0x40, 0x12}, {0x40, 0x12, 0x34, 0},
        {0xc0, 0x12, 0x34}, {0xc0, 0x12, 0x34, 0x5a, 0},
        {0x20, 0}, {0xa0}, {0x10}, {0x21}, {0xd0, 0x12, 0x34, 0x5a}};
    const auto start = system.time();
    for (const auto& bytes : invalid) {
        expect_failure<std::invalid_argument>(system, "SPI command", [&] { system.spi().command(bytes); });
    }
    EXPECT_THROW(system.spi().raw_transaction(0), std::invalid_argument);
    EXPECT_THROW(system.spi().read(static_cast<fpga::Register>(0)), std::invalid_argument);
    EXPECT_THROW(system.spi().read(WishboneAddress{WishboneAddressCapacity}), std::out_of_range);
    EXPECT_EQ(system.time(), start);
    EXPECT_TRUE(system.snapshot().spi_cs_ni);
    EXPECT_FALSE(system.spi().peek_failed());
    const std::array<uint8_t, 1> same{fpga::command_byte(fpga::SpiCommand::ReadSame)};
    EXPECT_NO_THROW(system.spi().command(same));
}

TEST_F(SystemTest, SpiReachesProductionRegistersAndBothSramBanksUnderReset) {
    system.cpu().assert_reset();
    system.cpu(CPU_SOFT_6502).select();
    EXPECT_EQ(system.cpu().read_selection(), CPU_SOFT_6502);
    for (const auto [address, value] : {std::pair{RamProbe, Marker}, std::pair{BankedProbe, OtherMarker}}) {
        const auto start = system.time();
        system.spi().write(fpga::address(address), value);
        EXPECT_EQ(system.peek(address), value);
        EXPECT_EQ(system.spi().read(fpga::address(address)), value);
        EXPECT_GT(system.time(), start);
        EXPECT_TRUE(system.cpu().peek_reset());
    }
    EXPECT_EQ(system.peek(RamProbe), Marker);
}

TEST_F(SystemTest, UnmappedWishboneAccessFailsClosedWithBoundedEvidence) {
    const auto start = system.time();
    const auto marker = system.peek(RamProbe);
    expect_failure<std::runtime_error>(system, "deadline", [&] {
        system.spi().read(LastWishboneAddress);
    }, "controller readiness");
    EXPECT_GT(system.time(), start);
    EXPECT_EQ(system.peek(RamProbe), marker);
    EXPECT_TRUE(system.spi().peek_failed());
    EXPECT_TRUE(system.snapshot().spi_cs_ni);
    EXPECT_FALSE(system.snapshot().spi_sck_i);
    const auto failed_time = system.time();
    const auto message = system.diagnostic("failed transport", "");
    EXPECT_NE(message.find("SPI abandoned"), std::string::npos);
    EXPECT_NE(message.find("last_spi_absolute_address=0xfffff"), std::string::npos);
    EXPECT_THROW(system.spi().read(fpga::Register::Status), std::logic_error);
    EXPECT_THROW(system.tick(0), std::logic_error);
    EXPECT_THROW(system.tick(1), std::logic_error);
    EXPECT_THROW(system.run_until([](const System&) { return true; }, 0, "failed wait"), std::logic_error);
    EXPECT_EQ(system.time(), failed_time);
    const System& board = system;
    EXPECT_EQ(board.cpu().peek_reset(), board.snapshot().cpu_reset_active_o);
    EXPECT_EQ(std::to_underlying(board.cpu().peek_selection()), board.snapshot().cpu_selection_o);
}

TEST_F(SystemTest, RawSpiAbortsPartialInputAndRejectsCompetingOwners) {
    const auto marker = system.peek(RamProbe);
    {
        auto owner = system.spi().raw_transaction();
        const auto start = system.time();
        EXPECT_THROW(system.spi().read(fpga::Register::Status), std::logic_error);
        EXPECT_THROW(system.spi().raw_transaction(), std::logic_error);
        EXPECT_THROW(system.drive_spi(true, false, false), std::logic_error);
        EXPECT_EQ(system.time(), start);
        owner.bit(true);
        owner.bit(false);
        owner.bit(true);
        owner.abort();
        EXPECT_THROW(owner.byte(0), std::logic_error);
        EXPECT_THROW(owner.abort(), std::logic_error);
    }
    {
        auto owner = system.spi().raw_transaction();
        owner.byte(fpga::command_byte(fpga::SpiCommand::WriteAt));
        owner.byte(static_cast<uint8_t>(RamProbe.value() >> 8));
        owner.abort();
    }
    EXPECT_EQ(system.peek(RamProbe), marker);
    EXPECT_FALSE(system.spi().peek_failed());
    system.spi().write(fpga::address(RamProbe), OtherMarker);
    EXPECT_EQ(system.spi().read(fpga::address(RamProbe)), OtherMarker);
    {
        auto owner = system.spi().raw_transaction();
        owner.byte(0x10);
        owner.abort();
    }
    EXPECT_FALSE(system.spi().peek_failed());
}

TEST_F(SystemTest, RawSpiTimingVariationKeepsEveryCompleteClockPhase) {
    for (const auto half_period : {Cycles{2}, Cycles{5}}) {
        auto owner = system.spi().raw_transaction(half_period);
        const std::array<uint8_t, 4> bytes{
            fpga::command_byte(fpga::SpiCommand::WriteAt, fpga::address(RamProbe)),
            static_cast<uint8_t>(RamProbe.value() >> 8), static_cast<uint8_t>(RamProbe.value()), Marker};
        const auto start = system.time();
        for (const auto value : bytes) owner.byte(value);
        constexpr uint64_t HalfPeriodsPerByte = 17;
        EXPECT_EQ(system.time() - start, half_period * HalfPeriodsPerByte * bytes.size());
        owner.finish();
        EXPECT_EQ(system.peek(RamProbe), Marker);
        EXPECT_TRUE(system.snapshot().spi_cs_ni);
        EXPECT_FALSE(system.snapshot().spi_sck_i);
    }
}

TEST_F(SystemTest, UnfinishedRawSpiScopeFailsClosedWithoutUnwindClocks) {
    auto stopped = system.time();
    EXPECT_THROW({
        auto owner = system.spi().raw_transaction();
        owner.bit(true);
        stopped = system.time();
        throw UnwindSentinel{};
    }, UnwindSentinel);
    EXPECT_EQ(system.time(), stopped);
    EXPECT_TRUE(system.spi().peek_failed());
    EXPECT_TRUE(system.snapshot().spi_cs_ni);
    EXPECT_FALSE(system.snapshot().spi_sck_i);
    EXPECT_THROW(system.tick(1), std::logic_error);
    EXPECT_THROW(system.drive_spi(true, false, false), std::logic_error);
    System other;
    {
        auto owner = other.spi().raw_transaction();
    }
    EXPECT_TRUE(other.spi().peek_failed());
}

TEST_F(SystemTest, CheckedSpiCallbackPreservesCommandPipelineAndReturnsOwnedResult) {
    const auto address = fpga::address(RamProbe);
    const std::array<uint8_t, 4> first{
        fpga::command_byte(fpga::SpiCommand::WriteAt, address),
        static_cast<uint8_t>(address.value() >> 8), static_cast<uint8_t>(address.value()), Marker};
    const std::array<uint8_t, 2> next{fpga::command_byte(fpga::SpiCommand::WriteNext), OtherMarker};
    const std::array<uint8_t, 3> seek{
        fpga::command_byte(fpga::SpiCommand::ReadAt, address),
        static_cast<uint8_t>(address.value() >> 8), static_cast<uint8_t>(address.value())};
    const std::array<uint8_t, 1> read_next{fpga::command_byte(fpga::SpiCommand::ReadNext)};
    const auto result = system.spi().transaction([&](auto& commands) {
        EXPECT_FALSE(system.snapshot().spi_cs_ni);
        EXPECT_THROW(system.spi().transaction([](auto&) {}), std::logic_error);
        commands.command(first);
        commands.command(next);
        commands.command(seek);
        auto result = std::make_unique<std::array<uint8_t, 2>>();
        (*result)[0] = commands.command(read_next);
        (*result)[1] = commands.command(read_next);
        return result;
    });
    EXPECT_EQ((*result)[0], Marker);
    EXPECT_EQ((*result)[1], OtherMarker);
    EXPECT_EQ(system.peek(RamProbe), Marker);
    EXPECT_EQ(system.peek(RamProbe + 1), OtherMarker);
    EXPECT_TRUE(system.snapshot().spi_cs_ni);
    EXPECT_FALSE(system.spi().peek_failed());
    EXPECT_NO_THROW(system.spi().transaction([](auto&) {}));
}

TEST_F(SystemTest, CheckedSpiCallbackFailuresRetainOriginalExceptionAndTimestamp) {
    bool entered = false;
    auto stopped = system.time();
    const std::array<uint8_t, 1> next{fpga::command_byte(fpga::SpiCommand::ReadNext)};
    EXPECT_THROW(system.spi().transaction([&](auto& commands) {
        entered = true;
        commands.command(next);
        stopped = system.time();
        throw UnwindSentinel{};
    }), UnwindSentinel);
    EXPECT_TRUE(entered);
    EXPECT_EQ(system.time(), stopped);
    EXPECT_TRUE(system.spi().peek_failed());
    EXPECT_TRUE(system.snapshot().spi_cs_ni);
    EXPECT_FALSE(system.snapshot().spi_sck_i);
    EXPECT_THROW(system.spi().transaction([](auto&) {}), std::logic_error);
}

TEST_F(SystemTest, CheckedSpiCompletionFailureCannotReturnSuccess) {
    bool work_completed = false;
    auto stopped = system.time();
    auto subscription = system.observe([&](const System&) {
        if (work_completed) {
            stopped = system.time();
            throw SpiUnwindSentinel{};
        }
    });
    EXPECT_THROW(system.spi().transaction([&](auto&) {
        work_completed = true;
        return Marker;
    }), SpiUnwindSentinel);
    EXPECT_TRUE(work_completed);
    EXPECT_EQ(system.time(), stopped);
    EXPECT_TRUE(system.spi().peek_failed());
    EXPECT_TRUE(system.snapshot().spi_cs_ni);
    EXPECT_FALSE(system.snapshot().spi_sck_i);
}

TEST_F(SystemTest, CheckedSpiAcquisitionFailureNeverEntersCallback) {
    bool entered = false;
    unsigned samples = 0;
    auto stopped = system.time();
    auto subscription = system.observe([&](const System&) {
        ++samples;
        stopped = system.time();
        throw SpiUnwindSentinel{};
    });
    EXPECT_THROW(system.spi().transaction([&](auto&) { entered = true; }), SpiUnwindSentinel);
    EXPECT_FALSE(entered);
    EXPECT_EQ(samples, 1u);
    EXPECT_EQ(system.time(), stopped);
    EXPECT_TRUE(system.spi().peek_failed());
    EXPECT_TRUE(system.snapshot().spi_cs_ni);
}

TEST_F(SystemTest, SpiExceptionsAtEveryTransportPhaseRetainOriginalFailure) {
    enum class Phase { Setup, Shift, Drain, Release };
    constexpr unsigned WriteTransferCycles = 2 + 4 * 34;
    for (const auto phase : {Phase::Setup, Phase::Shift, Phase::Drain, Phase::Release}) {
        System subject;
        bool injected = false;
        unsigned callbacks = 0;
        auto stopped = subject.time();
        auto subscription = subject.observe([&](const System& observed) {
            ++callbacks;
            const auto pins = observed.snapshot();
            if ((phase == Phase::Setup && !pins.spi_cs_ni && !pins.spi_sck_i)
                || (phase == Phase::Shift && !pins.spi_cs_ni && pins.spi_sck_i)
                || (phase == Phase::Drain && !pins.spi_cs_ni && pins.spi_stall_o
                    && callbacks > WriteTransferCycles)
                || (phase == Phase::Release && pins.spi_cs_ni)) {
                injected = true;
                stopped = observed.time();
                throw SpiUnwindSentinel{};
            }
        });
        EXPECT_THROW(subject.spi().write(fpga::address(RamProbe), Marker), SpiUnwindSentinel);
        EXPECT_TRUE(injected) << static_cast<int>(phase);
        EXPECT_EQ(subject.time(), stopped);
        EXPECT_FALSE(subject.clock_faulted());
        EXPECT_TRUE(subject.spi().peek_failed());
        EXPECT_TRUE(subject.snapshot().spi_cs_ni);
        EXPECT_FALSE(subject.snapshot().spi_sck_i);
        const auto failed_callbacks = callbacks;
        EXPECT_THROW(subject.spi().raw_transaction(), std::logic_error);
        EXPECT_THROW(subject.spi().wait_ready(), std::logic_error);
        EXPECT_THROW(subject.tick(1), std::logic_error);
        EXPECT_EQ(subject.time(), stopped);
        EXPECT_EQ(callbacks, failed_callbacks);
    }
}

TEST_F(SystemTest, CheckedSpiValidationCanBeCaughtInsideOwnerButUnwindingFailsClosed) {
    const std::array<uint8_t, 0> empty{};
    system.spi().transaction([&](auto& commands) {
        const auto start = system.time();
        expect_failure<std::invalid_argument>(system, "SPI command", [&] { commands.command(empty); });
        EXPECT_EQ(system.time(), start);
        EXPECT_FALSE(system.spi().peek_failed());
    });
    EXPECT_FALSE(system.spi().peek_failed());
    EXPECT_NO_THROW(system.spi().read(fpga::Register::Status));
    EXPECT_THROW(system.spi().transaction([&](auto& commands) { commands.command(empty); }),
        std::invalid_argument);
    EXPECT_TRUE(system.spi().peek_failed());
    EXPECT_TRUE(system.snapshot().spi_cs_ni);
}

TEST_F(SystemTest, SpiReadyWaitAcceptsFinalCycleAndRestoresShortDeadline) {
    System reference;
    System short_board;
    auto reference_owner = reference.spi().raw_transaction();
    auto owner = system.spi().raw_transaction();
    auto short_owner = short_board.spi().raw_transaction();
    const std::array<uint8_t, 4> bytes{
        fpga::command_byte(fpga::SpiCommand::WriteAt, fpga::address(RamProbe)),
        static_cast<uint8_t>(RamProbe.value() >> 8), static_cast<uint8_t>(RamProbe.value()), Marker};
    for (const auto value : bytes) {
        reference_owner.byte(value);
        owner.byte(value);
        short_owner.byte(value);
    }
    ASSERT_TRUE(reference.snapshot().spi_stall_o);
    const auto start = reference.time();
    reference.spi().wait_ready();
    const auto cost = reference.time() - start;
    ASSERT_GT(cost, Cycles{0});
    const auto exact_start = system.time();
    EXPECT_NO_THROW(system.spi().wait_ready(cost));
    EXPECT_EQ(system.time() - exact_start, cost);
    const auto short_start = short_board.time();
    EXPECT_THROW(short_board.spi().wait_ready(cost - Cycles{1}), std::runtime_error);
    EXPECT_EQ(short_board.time() - short_start, cost - Cycles{1});
    EXPECT_NO_THROW(short_board.spi().wait_ready(1));
    const auto ready = system.time();
    EXPECT_NO_THROW(system.spi().wait_ready(0));
    EXPECT_EQ(system.time(), ready);
    reference_owner.finish();
    owner.finish();
    short_owner.finish();
}

TEST_F(SystemTest, ActiveSpiPollingCountsEveryTransportCycle) {
    System reference;
    const auto start = reference.time();
    reference.spi().read(fpga::address(RamProbe));
    const auto cost = reference.time() - start;
    const auto active_start = system.time();
    bool complete = false;
    system.service_until([&](const System&) { return complete; },
        [&](System& board, Cycles remaining) {
            EXPECT_EQ(remaining, cost);
            complete = board.spi().read(fpga::address(RamProbe)) == System::IdleRamByte;
        }, cost, "SPI poll");
    EXPECT_EQ(system.time() - active_start, cost);
    EXPECT_FALSE(system.spi().peek_failed());
}

TEST_F(SystemTest, ActiveSpiStallAndReleaseCannotExtendDeadline) {
    constexpr Cycles StallDeadline{200};
    const auto start = system.time();
    EXPECT_THROW(system.service_until([](const System&) { return false; },
        [](System& board, Cycles) { board.spi().read(LastWishboneAddress); },
        StallDeadline, "outer SPI deadline"), std::runtime_error);
    EXPECT_EQ(system.time() - start, StallDeadline);
    EXPECT_TRUE(system.spi().peek_failed());
    System short_board;
    constexpr Cycles ShortBudget{3};
    const auto short_start = short_board.time();
    bool completed_work = false;
    EXPECT_THROW(short_board.service_until([](const System&) { return false; },
        [&](System& board, Cycles) { board.spi().transaction([&](auto&) { completed_work = true; }); },
        ShortBudget, "release deadline"), std::runtime_error);
    EXPECT_TRUE(completed_work);
    EXPECT_EQ(short_board.time() - short_start, Cycles{2});
    EXPECT_TRUE(short_board.spi().peek_failed());
}

TEST_F(SystemTest, CaughtSpiFailureCannotBeReportedAsSuccessfulCompletion) {
    unsigned predicates = 0;
    bool caught = false;
    EXPECT_THROW(system.service_until([&](const System&) { ++predicates; return caught; },
        [&](System& board, Cycles) {
            try {
                board.spi().transaction([](auto&) { throw UnwindSentinel{}; });
            } catch (const UnwindSentinel&) { caught = true; }
        }, Cycles{10}, "caught SPI failure"), std::logic_error);
    EXPECT_TRUE(caught);
    EXPECT_EQ(predicates, 1u);
    EXPECT_TRUE(system.spi().peek_failed());
}

TEST_F(SystemTest, SpiAndCpuViewsRejectCapturedMutationFromObservationAndEdits) {
    auto spi = system.spi();
    auto cpu = system.cpu(CPU_SOFT_6502);
    const auto verify = [&] {
        const auto start = system.time();
        EXPECT_THROW(spi.read(fpga::Register::Status), std::logic_error);
        EXPECT_THROW(spi.write(fpga::Register::CpuControl, 0), std::logic_error);
        EXPECT_THROW(spi.transaction([](auto&) {}), std::logic_error);
        EXPECT_THROW(cpu.read_selection(), std::logic_error);
        EXPECT_THROW(cpu.read_control(), std::logic_error);
        EXPECT_THROW(cpu.assert_reset(), std::logic_error);
        EXPECT_THROW(cpu.prepare(), std::logic_error);
        EXPECT_THROW(cpu.select(), std::logic_error);
        EXPECT_THROW(cpu.start(Entry6502), std::logic_error);
        EXPECT_THROW(cpu.release_reset(), std::logic_error);
        EXPECT_THROW(cpu.write_vector(Entry6502), std::logic_error);
        EXPECT_THROW(cpu.write_control_raw(0), std::logic_error);
        EXPECT_EQ(system.time(), start);
        EXPECT_FALSE(spi.peek_failed());
    };
    auto subscription = system.observe([&](const System& board) {
        verify();
        EXPECT_EQ(board.cpu().peek_reset(), board.snapshot().cpu_reset_active_o);
        EXPECT_EQ(board.cpu(CPU_SOFT_6502).peek_address(), board.snapshot().soft6502_addr_o);
    });
    system.tick(2);
    subscription.reset();
    system.raw_stimulus([&](auto&) { verify(); });
    system.io().pia1().inputs([&](auto&) { verify(); });
}

TEST_F(SystemTest, CapturedRawOwnerCannotShiftOrReleaseFromObserverOrInputEdit) {
    auto owner = system.spi().raw_transaction();
    const std::array<uint8_t, 1> same{fpga::command_byte(fpga::SpiCommand::ReadSame)};
    const auto verify = [&] {
        const auto start = system.time();
        const auto pins = system.snapshot();
        EXPECT_THROW(owner.byte(0), std::logic_error);
        EXPECT_THROW(owner.bit(true), std::logic_error);
        EXPECT_THROW(owner.command(same), std::logic_error);
        EXPECT_THROW(owner.finish(), std::logic_error);
        EXPECT_THROW(owner.abort(), std::logic_error);
        EXPECT_EQ(system.time(), start);
        EXPECT_EQ(system.snapshot().spi_cs_ni, pins.spi_cs_ni);
        EXPECT_EQ(system.snapshot().spi_sck_i, pins.spi_sck_i);
        EXPECT_FALSE(system.spi().peek_failed());
    };
    auto subscription = system.observe([&](const System&) { verify(); });
    system.tick(1);
    subscription.reset();
    system.raw_stimulus([&](auto&) { verify(); });
    system.io().via().inputs([&](auto&) { verify(); });
    owner.abort();
    EXPECT_NO_THROW(system.spi().read(fpga::Register::Status));
}

TEST_F(SystemTest, CpuBoundIdentityAndInvalidTargetsRejectBeforeMutation) {
    auto cpu = system.cpu(CPU_SOFT_6502);
    cpu.prepare();
    const auto start = system.time();
    const auto low = system.peek(SramAddress{pet::Reset6502.value()});
    const auto high = system.peek(SramAddress{pet::Reset6809.value()});
    EXPECT_THROW(system.cpu(CPU_AUTO), std::invalid_argument);
    EXPECT_THROW(system.cpu().prepare(), std::logic_error);
    EXPECT_THROW(system.cpu().start(), std::logic_error);
    EXPECT_THROW(system.cpu(CPU_PHYS_6502).start(Entry6502), std::logic_error);
    EXPECT_THROW(system.cpu(CPU_PHYS_6502).peek_fetched_at(Entry6502, 0), std::logic_error);
    EXPECT_THROW(system.cpu(CPU_SOFT_6809).write_vector(Entry6809), std::logic_error);
    EXPECT_THROW(system.cpu(CPU_SOFT_6809).release_reset(), std::logic_error);
    EXPECT_EQ(system.time(), start);
    EXPECT_EQ(system.peek(SramAddress{pet::Reset6502.value()}), low);
    EXPECT_EQ(system.peek(SramAddress{pet::Reset6809.value()}), high);
    cpu.release_reset();
    const auto running = system.time();
    EXPECT_THROW(cpu.write_vector(Entry6502), std::logic_error);
    EXPECT_EQ(system.time(), running);
}

TEST_F(SystemTest, CpuLifecycleSwitchesNativeCoresAndPreservesHistoryAcrossReset) {
    install_loop(system, CPU_SOFT_6502, Entry6502, Marker);
    install_loop(system, CPU_SOFT_6809, Entry6809, OtherMarker);
    const auto run = [&](cpu_type_t selection, CpuAddress entry, uint8_t value) {
        system.cpu(selection).start(entry);
        EXPECT_FALSE(system.cpu().peek_reset());
        system.run_until([&](const System& board) {
            return board.peek(SramAddress{Result.value()}) == value;
        }, ExecutionBudget, "native CPU loop");
    };
    run(CPU_SOFT_6502, Entry6502, Marker);
    EXPECT_TRUE(system.cpu(CPU_SOFT_6502).peek_fetched_at(Entry6502, 0));
    EXPECT_FALSE(system.cpu(CPU_SOFT_6502).peek_fetched_at(Entry6502 + 1, 0));
    EXPECT_FALSE(system.cpu(CPU_SOFT_6809).peek_fetched_at(Entry6809, 0));
    EXPECT_THROW(system.cpu(CPU_SOFT_6809).select(), std::logic_error);
    system.cpu().assert_reset();
    const auto checkpoint = system.cpu().peek_fetch_checkpoint();
    ASSERT_GT(checkpoint, 0u);
    system.poke(SramAddress{Result.value()}, 0);
    system.spi().read(fpga::Register::CpuSelect);
    system.tick(ExecutionSlice);
    EXPECT_EQ(system.cpu().peek_fetch_checkpoint(), checkpoint);
    EXPECT_EQ(system.peek(SramAddress{Result.value()}), 0);
    run(CPU_SOFT_6809, Entry6809, OtherMarker);
    EXPECT_EQ(system.cpu().read_control(), CpuControl{});
    EXPECT_EQ(system.peek(SramAddress{pet::Reset6809.value()}), Entry6809.value() >> 8);
    EXPECT_EQ(system.peek(SramAddress{(pet::Reset6809 + 1).value()}), Entry6809.value() & 0xff);
    EXPECT_FALSE(system.cpu(CPU_SOFT_6502).peek_fetched_at(Entry6502, checkpoint));
    system.cpu().assert_reset();
    system.poke(SramAddress{Result.value()}, 0);
    const auto second_checkpoint = system.cpu().peek_fetch_checkpoint();
    system.tick(ExecutionSlice);
    EXPECT_EQ(system.peek(SramAddress{Result.value()}), 0);
    EXPECT_EQ(system.cpu().peek_fetch_checkpoint(), second_checkpoint);
    run(CPU_SOFT_6502, Entry6502, Marker);
    EXPECT_EQ(system.cpu().read_control(), CpuControl{CpuControlBit::Ready});
    EXPECT_EQ(system.peek(SramAddress{pet::Reset6502.value()}), Entry6502.value() & 0xff);
    EXPECT_EQ(system.peek(SramAddress{(pet::Reset6502 + 1).value()}), Entry6502.value() >> 8);
    EXPECT_TRUE(system.cpu(CPU_SOFT_6502).peek_fetched_at(Entry6502, second_checkpoint));
}

TEST_F(SystemTest, Halted6502SyncDoesNotAdvanceFetchCheckpoint) {
    constexpr uint8_t StopOpcode = 0xdb;
    constexpr uint8_t NopOpcode = 0xea;
    install_loop(system, CPU_SOFT_6502, Entry6502, Marker);
    system.poke(SramAddress{Entry6502.value()}, StopOpcode);
    auto cpu = system.cpu(CPU_SOFT_6502);
    cpu.start(Entry6502);
    system.run_until([](const System& board) { return !board.snapshot().cpu_ready_o; },
        ExecutionBudget, "6502 stopped on SYNC");
    ASSERT_EQ(cpu.peek_address(), Entry6502);
    const auto checkpoint = cpu.peek_fetch_checkpoint();
    system.tick(ExecutionSlice);
    EXPECT_EQ(cpu.peek_address(), Entry6502);
    EXPECT_EQ(cpu.peek_fetch_checkpoint(), checkpoint);
    EXPECT_FALSE(cpu.peek_fetched_at(Entry6502, checkpoint));
    system.spi().read(fpga::Register::Status);
    EXPECT_EQ(cpu.peek_fetch_checkpoint(), checkpoint);
    system.poke(SramAddress{Entry6502.value()}, NopOpcode);
    system.spi().write(fpga::Register::Breakpoint, ECONOPET_REG_BP_CTL_CLEAR_MASK);
    system.run_until([&](const System& board) {
        return board.cpu(CPU_SOFT_6502).peek_fetched_at(Entry6502, checkpoint);
    }, ExecutionBudget, "6502 resumed fetch");
}

TEST_F(SystemTest, CpuSelectionReadsAndResetReleaseAcceptSuperpetIoMode) {
    for (const auto selection : {CPU_PHYS_6502, CPU_SOFT_6502, CPU_SOFT_6809}) {
        system.cpu(selection).prepare();
        const auto configuration = static_cast<uint8_t>(
            static_cast<uint8_t>(selection) | ECONOPET_CPU_SEL_SUPERPET_IO_MASK);
        system.spi().write(fpga::Register::CpuSelect, configuration);
        EXPECT_EQ(system.cpu().peek_selection(), selection);
        EXPECT_EQ(system.cpu().read_selection(), selection);
        EXPECT_NO_THROW(system.cpu(selection).release_reset());
        EXPECT_FALSE(system.cpu().peek_reset());
        const bool ready = selection != CPU_SOFT_6809;
        EXPECT_EQ(system.snapshot().cpu_ready_o, ready);
        EXPECT_EQ(system.cpu().read_control(),
            ready ? CpuControl{CpuControlBit::Ready} : CpuControl{});
        EXPECT_EQ(system.spi().read(fpga::Register::CpuSelect), configuration);
    }
}

TEST_F(SystemTest, CpuControlReadsIgnoreUnspecifiedRegisterBits) {
    constexpr uint8_t FirstReservedBit = 0x08;
    const CpuControl known{CpuControlBit::Reset | CpuControlBit::Ready | CpuControlBit::Nmi};
    for (unsigned reserved = 0; reserved <= UINT8_MAX; reserved += FirstReservedBit) {
        system.cpu().write_control_raw(static_cast<uint8_t>(reserved | known.bits()));
        EXPECT_EQ(system.cpu().read_control(), known);
        EXPECT_NO_THROW(system.cpu().assert_reset());
        EXPECT_EQ(system.cpu().read_control(), known);
    }
    System randomized(system.seed(), System::InitialState::Random);
    randomized.cpu().write_control_raw(UINT8_MAX);
    EXPECT_EQ(randomized.cpu().read_control(), known);
    EXPECT_NO_THROW(randomized.cpu(CPU_SOFT_6502).prepare());
}

TEST_F(SystemTest, CpuLifecyclePreservesSuperpetModeDuringCoreSelection) {
    system.cpu().assert_reset();
    system.spi().write(fpga::Register::CpuSelect,
        ECONOPET_CPU_SEL_SOFT_6502 | ECONOPET_CPU_SEL_SUPERPET_IO_MASK);
    for (const auto selected : {CPU_SOFT_6502, CPU_PHYS_6502, CPU_SOFT_6809}) {
        auto cpu = system.cpu(selected);
        cpu.select();
        const auto expected = static_cast<uint8_t>(selected) | ECONOPET_CPU_SEL_SUPERPET_IO_MASK;
        EXPECT_EQ(system.spi().read(fpga::Register::CpuSelect), expected);
        cpu.prepare();
        EXPECT_EQ(system.spi().read(fpga::Register::CpuSelect), expected);
        cpu.start();
        EXPECT_EQ(system.spi().read(fpga::Register::CpuSelect), expected);
        system.cpu().assert_reset();
    }
}

TEST_F(SystemTest, AssertedVideoStatusBitDenotesTextMode) {
    constexpr uint8_t Ca2FixedLow = 0x0c;
    constexpr uint8_t Ca2FixedHigh = 0x0e;
    for (const bool text : {false, true}) {
        io::write(system.io().via(), io::ViaRegister::Pcr,
            text ? Ca2FixedHigh : Ca2FixedLow, system.time());
        system.tick(1);
        const auto raw = system.spi().read(fpga::Register::Status);
        EXPECT_EQ((raw & ECONOPET_REG_STATUS_GRAPHICS_MASK) != 0, text);
        EXPECT_EQ(fpga::Status::from_bits(raw).contains(fpga::StatusBit::Text), text);
    }
}

TEST_F(SystemTest, ResetVectorRequiresIdleAndDrainedSpiTransport) {
    auto cpu = system.cpu(CPU_SOFT_6502);
    cpu.prepare();
    const auto vector = SramAddress{pet::Reset6502.value()};
    const auto address = fpga::address(vector);
    const std::array<uint8_t, 4> write{
        fpga::command_byte(fpga::SpiCommand::WriteAt, address),
        static_cast<uint8_t>(address.value() >> 8), static_cast<uint8_t>(address.value()), Marker};
    system.spi().transaction([&](auto& commands) {
        commands.command(write);
        const auto start = system.time();
        const auto low = system.peek(vector);
        const auto high = system.peek(vector + 1);
        EXPECT_THROW(cpu.write_vector(Entry6502), std::logic_error);
        EXPECT_EQ(system.time(), start);
        EXPECT_EQ(system.peek(vector), low);
        EXPECT_EQ(system.peek(vector + 1), high);
    });
    EXPECT_EQ(system.peek(vector), Marker);
    // Release unscoped raw CS before settling it, while the request is still in flight.
    system.drive_spi(false, false, false);
    for (const auto byte : write) {
        for (unsigned bit = 0; bit < SpiBitsPerByte; ++bit) {
            const bool value = (byte & (SpiHighBit >> bit)) != 0;
            system.drive_spi(false, false, value);
            system.tick(SpiHalfPeriod);
            system.drive_spi(false, true, value);
            system.tick(SpiHalfPeriod);
        }
        system.drive_spi(false, false, false);
        system.tick(SpiHalfPeriod);
    }
    ASSERT_TRUE(system.snapshot().spi_stall_o);
    system.drive_spi(true, false, false);
    cpu.write_vector(Entry6502);
    system.spi().wait_ready();
    system.tick(ExecutionSlice);
    EXPECT_EQ(system.peek(vector), Entry6502.value() & 0xff);
    EXPECT_EQ(system.peek(vector + 1), Entry6502.value() >> 8);
}

TEST_F(SystemTest, ExternalResetCannotBeOverriddenByCpuControl) {
    system.set_external_reset(true);
    system.tick(1);
    EXPECT_TRUE(system.cpu().peek_reset());
    EXPECT_THROW(system.cpu().release_reset(), std::runtime_error);
    system.set_external_reset(false);
    system.tick(1);
    EXPECT_FALSE(system.cpu().peek_reset());
    EXPECT_FALSE(system.spi().peek_failed());
}

TEST_F(SystemTest, Native6809SyncHistoryIsInspectableAndSurvivesReset) {
    constexpr uint8_t SyncInstruction = 0x13;
    system.poke(SramAddress{Entry6809.value()}, SyncInstruction);
    auto cpu = system.cpu(CPU_SOFT_6809);
    cpu.start(Entry6809);
    const auto checkpoint = cpu.peek_fetch_checkpoint();
    system.run_until([](const System& board) { return board.snapshot().soft6809_fetch_o; },
        ExecutionBudget, "6809 native SYNC");
    const auto observed = cpu.peek_address();
    EXPECT_TRUE(cpu.peek_fetched_at(observed, checkpoint));
    EXPECT_FALSE(system.cpu(CPU_SOFT_6502).peek_fetched_at(observed, checkpoint));
    cpu.assert_reset();
    EXPECT_TRUE(cpu.peek_fetched_at(observed, checkpoint));
    const auto reset_checkpoint = cpu.peek_fetch_checkpoint();
    system.tick(ExecutionSlice);
    EXPECT_EQ(cpu.peek_fetch_checkpoint(), reset_checkpoint);
}

TEST_F(SystemTest, RawCpuControlAllowsUnusualReadyAndResetSequences) {
    system.cpu(CPU_SOFT_6502).prepare();
    system.cpu().write_control_raw(CpuControl{CpuControlBit::Reset | CpuControlBit::Ready}.bits());
    EXPECT_EQ(system.cpu().read_control(), CpuControl(CpuControlBit::Reset | CpuControlBit::Ready));
    EXPECT_TRUE(system.cpu().peek_reset());
    system.cpu().write_control_raw(0);
    EXPECT_EQ(system.cpu().read_control(), CpuControl{});
    EXPECT_FALSE(system.cpu().peek_reset());
}

TEST_F(SystemTest, TypedConfigurationReachesProductionStatusBits) {
    for (auto display : {pet_video_type_crtc, pet_video_type_fixed}) {
        for (auto keyboard : {pet_keyboard_model_business, pet_keyboard_model_graphics}) {
            system.set_display(display);
            system.set_keyboard(keyboard);
            const auto status = fpga::Status::from_bits(system.spi().read(fpga::Register::Status));
            EXPECT_EQ(status.contains(fpga::StatusBit::FixedDisplay), display == pet_video_type_fixed);
            EXPECT_EQ(status.contains(fpga::StatusBit::GraphicsKeyboard), keyboard == pet_keyboard_model_graphics);
        }
    }
}

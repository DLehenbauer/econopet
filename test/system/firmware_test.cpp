// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#include "framework/firmware_test.h"

#include <cstdio>
#include <type_traits>

#include <gtest/gtest-spi.h>

#include "ieee/controller.h"
#include "ieee/trace.h"
#include "scheduler.h"

extern "C" {
#include "driver.h"
#include "global.h"
#include "sd/sd.h"
}

namespace {
struct UnwindSentinel { int value = 0; };
static_assert(!std::is_copy_constructible_v<FirmwareSession>);
static_assert(!std::is_move_constructible_v<FirmwareSession>);
static_assert(!std::is_constructible_v<FirmwareSession, System&&>);
constexpr econopet::SramAddress kSpiRamProbe{0x1234};
constexpr econopet::CpuAddress kResult{0x0200};

// Capture completed-CS transactions and MSB-first MOSI bytes at clock boundaries.
class SpiCapture {
public:
    // Subscribe before the operation under test and retain the scoped owner.
    explicit SpiCapture(System& system) : subscription_(system.observe([this](const System& board) {
        const auto pins = board.snapshot();
        if (!pins.spi_cs_ni && !selected_) {
            transactions.emplace_back();
            bits_ = 0;
            byte_ = 0;
        }
        if (!pins.spi_cs_ni && pins.spi_sck_i && !clock_) {
            byte_ = static_cast<uint8_t>((byte_ << 1) | pins.spi_sdo_i);
            if (++bits_ == 8) {
                transactions.back().push_back(byte_);
                bits_ = 0;
                byte_ = 0;
            }
        }
        selected_ = !pins.spi_cs_ni;
        clock_ = pins.spi_sck_i;
    })) {}
    std::vector<std::vector<uint8_t>> transactions;

private:
    bool selected_ = false, clock_ = false;
    unsigned bits_ = 0;
    uint8_t byte_ = 0;
    System::ObserverSubscription subscription_;
};
} // namespace

// Multiple scoped destinations can survive close without affecting a later session.
TEST_F(FirmwareTest, ScopedTracesFirmwareRxOwnsStorageAndSessionRegistration) {
    firmware.mount(disk_fixture::D64::empty());
    auto first = firmware.trace_rx();
    auto other = firmware.trace_rx();
    firmware.service();
    EXPECT_EQ(first.records(), other.records());
    auto moved = std::move(first);
    EXPECT_THROW(first.records(), std::logic_error);
    firmware.close();
    const auto closed_end = moved.capture().window().completed();
    EXPECT_THROW(firmware.trace_rx(), std::logic_error);
    FirmwareTestSession replacement(system);
    replacement.mount(disk_fixture::D64::empty());
    auto current = replacement.trace_rx();
    moved.stop();
    replacement.service();
    EXPECT_GT(current.capture().window().completed(), closed_end);
    EXPECT_EQ(other.capture().window().completed(), closed_end);
}

// Reject complete invalid transfers before changing SRAM, time or destinations.
TEST_F(FirmwareTest, FirmwareBulkAddressRangesAreCheckedBeforeTransfers) {
    const uint8_t source[] = {0x42, 0x5a};
    uint8_t destination[] = {0xa5, 0x5a};
    const auto start = system.time();
    const auto marker = system.peek(econopet::SramAddress{kResult.value()});
    EXPECT_THROW(spi_write(econopet::WishboneAddressCapacity - 1, source, sizeof(source)), std::out_of_range);
    EXPECT_THROW(spi_read(econopet::WishboneAddressCapacity - 1, sizeof(destination), destination), std::out_of_range);
    EXPECT_THROW(spi_fill(econopet::WishboneAddressCapacity - 1, 0, sizeof(source)), std::out_of_range);
    EXPECT_THROW(spi_write_same_block(econopet::WishboneAddressCapacity, source, sizeof(source)), std::out_of_range);
    EXPECT_THROW(spi_write(kSpiRamProbe.value(), nullptr, 1), std::invalid_argument);
    EXPECT_THROW(spi_read(kSpiRamProbe.value(), 1, nullptr), std::invalid_argument);
    EXPECT_THROW(spi_write_same_block(kSpiRamProbe.value(), nullptr, 1), std::invalid_argument);
    EXPECT_EQ(destination[0], 0xa5);
    EXPECT_EQ(destination[1], 0x5a);
    EXPECT_EQ(system.peek(econopet::SramAddress{kResult.value()}), marker);
    EXPECT_EQ(system.time(), start);
}

// Real firmware servicing shares the same deadline as explicit board operations.
TEST_F(FirmwareTest, FirmwareServiceConsumesActiveDeadline) {
    firmware.mount(disk_fixture::D64::empty());
    const auto measurement_start = system.time();
    firmware.service();
    const auto cost = system.time() - measurement_start;
    ASSERT_GT(cost, econopet::Cycles{0});
    const auto start = system.time();
    bool complete = false;
    system.service_until([&](const System&) { return complete; },
        [&](System&, econopet::Cycles) {
            firmware.service();
            complete = true;
        }, cost, "firmware service");
    EXPECT_EQ(system.time() - start, cost);
    EXPECT_FALSE(system.spi().peek_failed());
}

// Direct run work borrows the session without forwarding unused callback arguments.
TEST_F(FirmwareTest, ConfiguredRunFirmwareServiceAdapterUsesRealSpiAndDeadline) {
    firmware.mount(disk_fixture::D64::empty());
    SpiCapture capture(system);
    const auto start = system.time();
    system.execution().within(10000)
        .after(2, [](System& board) { board.set_external_interrupts(false, true); })
        .every(1, Scheduler::MissedService::Coalesce, firmware.service_adapter())
        .until([&](const System&) { return !capture.transactions.empty(); })
        .run("direct firmware adapter");
    EXPECT_GT(system.time() - start, econopet::Cycles{2});
    EXPECT_LE(system.time() - start, econopet::Cycles{10000});
    EXPECT_FALSE(system.peek_stimulus().cpu_nmi_n_i);
    EXPECT_FALSE(system.spi().peek_failed());
}

// An adapter cannot service the process-global binding through another board.
TEST_F(FirmwareTest, ConfiguredRunFirmwareAdapterRejectsForeignAndClosedSessions) {
    auto service = firmware.service_adapter();
    System other;
    EXPECT_THROW(other.execution().within(10)
        .every(1, Scheduler::MissedService::Reject, service).run(), std::invalid_argument);
    EXPECT_FALSE(other.spi().peek_failed());
    firmware.close();
    EXPECT_THROW(firmware.service_adapter(), std::logic_error);
    EXPECT_THROW(system.execution().within(10)
        .every(1, ServiceTiming::AfterSlice, service).run(), std::logic_error);
    EXPECT_FALSE(system.spi().peek_failed());
}

// Firmware SPI and timed stimulus share one scheduler and elapsed-cycle deadline.
TEST_F(FirmwareTest, SchedulerInterleavesFirmwareSpiAndExactStimulus) {
    firmware.mount(disk_fixture::D64::empty());
    Scheduler scheduler(system);
    const auto at = system.time() + econopet::Cycles{7};
    std::optional<econopet::CycleTime> applied;
    bool complete = false;
    scheduler.at(at, [&](System& board) {
        applied = board.time();
        EXPECT_FALSE(board.snapshot().spi_cs_ni);
        board.set_external_interrupts(false, true);
    });
    scheduler.every(system.time(), 1000, Scheduler::MissedService::Coalesce,
        [&](System&, econopet::Cycles) {
            firmware.service();
            complete = true;
        });
    scheduler.run_until([&](const System&) { return complete; }, 10000, "scheduled firmware");
    ASSERT_TRUE(applied);
    EXPECT_EQ(*applied, at);
    EXPECT_FALSE(system.peek_stimulus().cpu_nmi_n_i);
    EXPECT_FALSE(system.spi().peek_failed());
}

// Failed scenarios release global ownership before a new session is created.
TEST_F(SystemTest, FirmwareBindingIsReleasedDuringExceptionUnwind) {
    bool setup_complete = false;
    bool caught_sentinel = false;
    try {
        FirmwareSession session(system);
        session.mount(disk_fixture::D64::empty());
        setup_complete = true;
        throw UnwindSentinel{};
    } catch (const UnwindSentinel&) {
        caught_sentinel = true;
    }
    ASSERT_TRUE(setup_complete);
    ASSERT_TRUE(caught_sentinel);
    EXPECT_NO_THROW({
        FirmwareSession next(system);
        next.mount(disk_fixture::D64::empty());
    });
}

// C firmware accesses reach physical SPI, SRAM and production register decoding.
TEST_F(FirmwareTest, FirmwareSpiAdapterReachesProductionRamAndRegisters) {
    spi_write_at(fpga::address(kSpiRamProbe).value(), 0x5a);
    EXPECT_EQ(system.peek(kSpiRamProbe), 0x5a);
    system.poke(kSpiRamProbe, 0x42);
    EXPECT_EQ(spi_read_at(fpga::address(kSpiRamProbe).value()), 0x42);
    const auto enable = fpga::IeeeControl{fpga::IeeeControlBit::Enabled}.bits();
    spi_write_at(fpga::address(fpga::Register::IeeeControl).value(), enable);
    EXPECT_TRUE(system.ieee().read_control().contains(fpga::IeeeControlStatusBit::Enabled));
    spi_write_at(fpga::address(fpga::Register::IeeeControl).value(), 0);
    EXPECT_FALSE(system.ieee().read_control().contains(fpga::IeeeControlStatusBit::Enabled));
}

// Verify exact command bytes and CS boundaries, not just final memory contents.
TEST_F(FirmwareTest, FirmwareBulkTransfersUseProductionSequentialTransactions) {
    SpiCapture capture(system);
    const uint8_t source[] = {0x42, 0x5a, 0xa5};
    spi_write(kSpiRamProbe.value(), source, sizeof(source));
    uint8_t destination[sizeof(source)]{};
    spi_read(kSpiRamProbe.value(), sizeof(destination), destination);
    EXPECT_EQ(std::vector<uint8_t>(std::begin(destination), std::end(destination)),
        std::vector<uint8_t>(std::begin(source), std::end(source)));
    const std::vector<std::vector<uint8_t>> expected{
        {0xc0, 0x12, 0x34, 0x42}, {0xa0, 0x5a}, {0xa0, 0xa5},
        {0x40, 0x12, 0x34}, {0x20}, {0x20}, {0x20}};
    EXPECT_EQ(capture.transactions, expected);
}

// Verify pipeline return values and next/previous/same FPGA pointer semantics.
TEST_F(FirmwareTest, FirmwareRelativeTransfersPreserveReadPipeline) {
    system.poke(kSpiRamProbe + -1, 0x11);
    system.poke(kSpiRamProbe, 0x22);
    system.poke(kSpiRamProbe + 1, 0x33);
    spi_read_seek(kSpiRamProbe.value());
    EXPECT_EQ(spi_read_same(), 0x22);
    EXPECT_EQ(spi_read_next(), 0x22);
    EXPECT_EQ(spi_read_prev(), 0x33);
    EXPECT_EQ(spi_read_prev(), 0x22);
    EXPECT_EQ(spi_write_at(kSpiRamProbe.value(), 0xa1), 0x11);
    spi_read_seek(kSpiRamProbe.value());
    EXPECT_EQ(spi_write_next(0xa2), 0xa1);
    spi_read_seek((kSpiRamProbe + 1).value());
    EXPECT_EQ(spi_write_prev(0xa3), 0xa2);
    spi_read_seek(kSpiRamProbe.value());
    EXPECT_EQ(spi_write_same(0xa4), 0xa3);
    EXPECT_EQ(system.peek(kSpiRamProbe), 0xa4);
    EXPECT_EQ(system.peek(kSpiRamProbe + 1), 0xa2);
}

// FIFO writes must retain CS across every WRITE_SAME in the production burst.
TEST_F(FirmwareTest, FirmwareSameBlockKeepsOneChipSelectTransaction) {
    SpiCapture capture(system);
    const uint8_t source[] = {0x41, 0x52, 0x63};
    spi_write_same_block(kSpiRamProbe.value(), source, sizeof(source));
    const std::vector<std::vector<uint8_t>> expected{
        {0xc0, 0x12, 0x34, 0x41, 0x80, 0x52, 0x80, 0x63}};
    EXPECT_EQ(capture.transactions, expected);
    EXPECT_EQ(system.peek(kSpiRamProbe), 0x63);
    EXPECT_EQ(system.peek(kSpiRamProbe + 1), System::IdleRamByte);
    EXPECT_TRUE(system.snapshot().spi_cs_ni);
    EXPECT_FALSE(system.spi().peek_failed());
}

// Empty operations retain production's asymmetric read-seek behavior.
TEST_F(FirmwareTest, FirmwareEmptyTransfersAndFillUseDocumentedTransactions) {
    SpiCapture capture(system);
    spi_write(kSpiRamProbe.value(), nullptr, 0);
    spi_write_same_block(kSpiRamProbe.value(), nullptr, 0);
    spi_fill(kSpiRamProbe.value(), 0, 0);
    EXPECT_TRUE(capture.transactions.empty());
    spi_read(kSpiRamProbe.value(), 0, nullptr);
    EXPECT_EQ(capture.transactions, (std::vector<std::vector<uint8_t>>{{0x40, 0x12, 0x34}}));
    capture.transactions.clear();
    spi_fill(kSpiRamProbe.value(), 0x5a, 3);
    const std::vector<std::vector<uint8_t>> expected{
        {0xc0, 0x12, 0x34, 0x5a}, {0xa0, 0x5a}, {0xa0, 0x5a}};
    EXPECT_EQ(capture.transactions, expected);
    for (int offset = 0; offset < 3; ++offset) EXPECT_EQ(system.peek(kSpiRamProbe + offset), 0x5a);
}

// The optional adapter never falls back to an unbound or already closed board.
TEST_F(SystemTest, FirmwareTransportRequiresAnActiveSession) {
    EXPECT_THROW(spi_read_seek(kSpiRamProbe.value()), std::logic_error);
    EXPECT_THROW(spi_read_next(), std::logic_error);
    EXPECT_THROW(spi_write_next(0), std::logic_error);
    FirmwareSession session(system);
    EXPECT_THROW(FirmwareSession duplicate(system), std::logic_error);
    session.close();
    EXPECT_THROW(spi_write_at(kSpiRamProbe.value(), 0), std::logic_error);
    EXPECT_THROW(session.service(), std::logic_error);
    EXPECT_THROW(session.mount(disk_fixture::D64::empty()), std::logic_error);
    EXPECT_NO_THROW(session.close());
}

// Fill must readdress at the production buffer boundary, not for every byte.
TEST_F(FirmwareTest, FirmwareFillPreservesProductionChunkBoundaries) {
    SpiCapture capture(system);
    constexpr size_t length = TEMP_BUFFER_SIZE + 2;
    spi_fill(kSpiRamProbe.value(), 0x5a, length);
    std::vector<std::vector<uint8_t>> expected{{0xc0, 0x12, 0x34, 0x5a}};
    for (size_t index = 1; index < TEMP_BUFFER_SIZE; ++index) expected.push_back({0xa0, 0x5a});
    expected.push_back({0xc0, 0x1a, 0x34, 0x5a});
    expected.push_back({0xa0, 0x5a});
    EXPECT_EQ(capture.transactions, expected);
    for (size_t index = 0; index < length; ++index)
        EXPECT_EQ(system.peek(kSpiRamProbe + index), 0x5a);
}

// Relative commands retain the SRAM bank bit rather than truncating at 16 bits.
TEST_F(FirmwareTest, FirmwareRelativePointerCrossesTheSramBankBoundary) {
    SpiCapture capture(system);
    spi_write_at(0xffff, 0x11);
    spi_write_next(0x42);
    EXPECT_EQ(system.peek(econopet::SramAddress{0x10000}), 0x42);
    spi_write_prev(0x22);
    spi_write_next(0x5a);
    EXPECT_EQ(system.peek(econopet::SramAddress{0x10000}), 0x5a);
    EXPECT_EQ(system.peek(econopet::SramAddress{0xffff}), 0x22);
    const std::vector<std::vector<uint8_t>> expected{
        {0xc0, 0xff, 0xff, 0x11}, {0xa0, 0x42}, {0xe0, 0x22}, {0xa0, 0x5a}};
    EXPECT_EQ(capture.transactions, expected);
}

// An interrupted multi-command burst consumes only its deadline and fails closed.
TEST_F(SystemTest, FirmwareBurstDeadlineFailureReleasesSessionOwnership) {
    FirmwareSession session(system);
    const uint8_t source[] = {0x41, 0x52, 0x63, 0x74};
    constexpr econopet::Cycles budget = 200;
    const auto start = system.time();
    EXPECT_THROW(system.service_until([](const System&) { return false; },
        [&](System&, econopet::Cycles) {
            spi_write_same_block(kSpiRamProbe.value(), source, sizeof(source));
        }, budget, "interrupted firmware burst"), std::runtime_error);
    EXPECT_EQ(system.time() - start, budget);
    EXPECT_TRUE(system.spi().peek_failed());
    EXPECT_TRUE(system.snapshot().spi_cs_ni);
    EXPECT_FALSE(system.snapshot().spi_sck_i);
    EXPECT_THROW(session.close(), std::logic_error);
    System other;
    EXPECT_NO_THROW(FirmwareSession next(other));
}

// Failure during session initialization must not retain the process-global board.
TEST_F(SystemTest, FirmwareInitializationFailureReleasesBinding) {
    auto observation = system.observe([](const System&) {
        throw std::runtime_error("firmware initialization observer failure");
    });
    EXPECT_THROW(FirmwareSession session(system), std::runtime_error);
    EXPECT_TRUE(system.spi().peek_failed());
    System other;
    EXPECT_NO_THROW(FirmwareSession next(other));
}

// Local ROM-style owners report failed-board teardown without aborting the runner.
TEST_F(SystemTest, FirmwareTestSessionReportsTeardownFailureAndReleasesBinding) {
    EXPECT_NONFATAL_FAILURE({
        FirmwareTestSession session(system);
        auto observation = system.observe([](const System&) {
            throw std::runtime_error("firmware transport observer failure");
        });
        EXPECT_THROW(spi_read_at(kSpiRamProbe.value()), std::runtime_error);
    }, "firmware teardown failed");
    EXPECT_TRUE(system.spi().peek_failed());
    System other;
    EXPECT_NO_THROW(FirmwareTestSession next(other));
}

// A live non-standard observer exception must reach the reporter, not terminate the runner.
TEST_F(SystemTest, FirmwareTestSessionReportsNonStandardDestructorFailure) {
    System::ObserverSubscription observation;
    EXPECT_NONFATAL_FAILURE({
        FirmwareTestSession session(system);
        session.mount(disk_fixture::D64::empty());
        observation = system.observe([](const System&) { throw UnwindSentinel{}; });
    }, "firmware teardown failed: non-standard exception");
    observation.reset();
    EXPECT_TRUE(system.spi().peek_failed());
    EXPECT_FALSE(test_file_registered("/disks/test.d64"));
    System other;
    EXPECT_NO_THROW({
        FirmwareTestSession next(other);
        next.mount(disk_fixture::D64::empty());
    });
}

// Fixture teardown must report the same exception domain as local session destruction.
TEST_F(FirmwareTest, FirmwareFixtureReportsNonStandardExplicitTeardownFailure) {
    firmware.mount(disk_fixture::D64::empty());
    auto observation = system.observe([](const System&) { throw UnwindSentinel{}; });
    EXPECT_NONFATAL_FAILURE(TearDown(), "firmware teardown failed: non-standard exception");
    observation.reset();
    EXPECT_TRUE(system.spi().peek_failed());
    EXPECT_FALSE(test_file_registered("/disks/test.d64"));
    EXPECT_NO_THROW(firmware.close());
    System other;
    EXPECT_NO_THROW(FirmwareTestSession next(other));
}

// Standard fixture teardown keeps the original diagnostic through the shared reporter.
TEST_F(FirmwareTest, FirmwareFixtureReportsOriginalStandardTeardownDiagnostic) {
    firmware.mount(disk_fixture::D64::empty());
    auto observation = system.observe([](const System&) {
        throw std::runtime_error("explicit teardown observer failure");
    });
    EXPECT_NONFATAL_FAILURE(TearDown(), "firmware teardown failed: explicit teardown observer failure");
    observation.reset();
    EXPECT_TRUE(system.spi().peek_failed());
    EXPECT_FALSE(test_file_registered("/disks/test.d64"));
    EXPECT_NO_THROW(firmware.close());
    System other;
    EXPECT_NO_THROW(FirmwareTestSession next(other));
}

// A custom reporter retains the exact dynamic type and payload after destruction.
TEST_F(SystemTest, FirmwareTeardownReporterRetainsOriginalExceptionAndFrozenTrace) {
    static std::exception_ptr reported;
    for (const bool standard : {false, true}) {
        System board;
        System::ObserverSubscription observation;
        std::optional<FirmwareRxTrace> rx;
        reported = {};
        {
            FirmwareSession session(board, [](std::exception_ptr error) { reported = error; });
            session.mount(disk_fixture::D64::empty());
            rx.emplace(session.trace_rx());
            observation = board.observe([standard](const System&) {
                if (standard) throw std::runtime_error("original teardown diagnostic");
                throw UnwindSentinel{42};
            });
        }
        observation.reset();
        ASSERT_TRUE(reported);
        try {
            std::rethrow_exception(reported);
            FAIL() << "reporter lost teardown failure";
        } catch (const std::runtime_error& error) {
            EXPECT_TRUE(standard);
            EXPECT_STREQ(error.what(), "original teardown diagnostic");
        } catch (const UnwindSentinel& error) {
            EXPECT_FALSE(standard);
            EXPECT_EQ(error.value, 42);
        }
        EXPECT_FALSE(test_file_registered("/disks/test.d64"));
        const auto frozen_end = rx->capture().window().completed();
        FirmwareSession replacement(system);
        replacement.mount(disk_fixture::D64::empty());
        replacement.service();
        EXPECT_EQ(rx->capture().window().completed(), frozen_end);
        EXPECT_NO_THROW(replacement.close());
        reported = {};
    }
}

// Failed explicit close preserves a non-standard cause and still releases all session owners.
TEST_F(SystemTest, FirmwareExplicitCloseRethrowsNonStandardFailureAfterCleanup) {
    FirmwareSession session(system);
    session.mount(disk_fixture::D64::empty());
    auto borrowed = session.service_adapter();
    auto rx = session.trace_rx();
    auto observation = system.observe([](const System&) { throw UnwindSentinel{17}; });
    try {
        session.close();
        FAIL() << "expected non-standard teardown failure";
    } catch (const UnwindSentinel& error) {
        EXPECT_EQ(error.value, 17);
    }
    observation.reset();
    EXPECT_TRUE(system.spi().peek_failed());
    EXPECT_FALSE(test_file_registered("/disks/test.d64"));
    const auto at = system.time();
    EXPECT_NO_THROW(session.close());
    EXPECT_THROW(borrowed(system, 1), std::logic_error);
    EXPECT_EQ(system.time(), at);
    const auto frozen_end = rx.capture().window().completed();
    System other;
    FirmwareSession replacement(other);
    replacement.mount(disk_fixture::D64::empty());
    replacement.service();
    EXPECT_EQ(rx.capture().window().completed(), frozen_end);
}

// Teardown reporting during unwind must not replace the original active exception.
TEST_F(SystemTest, FirmwareNonStandardTeardownDuringUnwindPreservesOuterFailure) {
    System::ObserverSubscription observation;
    bool caught_outer = false;
    EXPECT_NONFATAL_FAILURE({
        try {
            FirmwareTestSession session(system);
            session.mount(disk_fixture::D64::empty());
            observation = system.observe([](const System&) { throw UnwindSentinel{}; });
            throw std::runtime_error("outer scenario failure");
        } catch (const std::runtime_error& error) {
            EXPECT_STREQ(error.what(), "outer scenario failure");
            caught_outer = true;
        }
    }, "firmware teardown failed: non-standard exception");
    observation.reset();
    EXPECT_TRUE(caught_outer);
    EXPECT_FALSE(test_file_registered("/disks/test.d64"));
    System other;
    EXPECT_NO_THROW(FirmwareTestSession next(other));
}

// Without a reporter, both exception domains retain the documented fail-fast policy.
TEST_F(SystemTest, FirmwareUnhandledDestructorFailuresEmitDiagnosticsAndTerminate) {
    for (const bool standard : {false, true}) {
        EXPECT_DEATH({
            System::ObserverSubscription observation;
            FirmwareSession session(system);
            session.mount(disk_fixture::D64::empty());
            observation = system.observe([standard](const System&) {
                if (standard) throw std::runtime_error("unhandled standard failure");
                throw UnwindSentinel{};
            });
        }, standard ? "firmware teardown failed: unhandled standard failure"
                    : "firmware teardown failed: non-standard exception");
    }
    // Each death-test child has independent bindings, leaving the parent usable.
    EXPECT_NO_THROW(FirmwareSession next(system));
}

// Reporting cannot let a rejected destructor return with dangling global ownership.
TEST_F(SystemTest, FirmwareRejectedDestructorReportsThenTerminatesInEveryGuardedContext) {
    enum class Context { Observation, PinEdit, PeripheralEdit, ScheduledStimulus, ScopedSpi, RawSpi };
    for (const auto context : {Context::Observation, Context::PinEdit, Context::PeripheralEdit,
                              Context::ScheduledStimulus, Context::ScopedSpi, Context::RawSpi}) {
        EXPECT_DEATH({
            auto session = std::make_unique<FirmwareSession>(system, [](std::exception_ptr error) {
                try {
                    std::rethrow_exception(error);
                } catch (const std::logic_error&) {
                    std::fputs("reported rejected teardown: ", stderr);
                }
            });
            session->mount(disk_fixture::D64::empty());
            switch (context) {
            case Context::Observation: {
                auto observation = system.observe([&](const System&) { session.reset(); });
                system.tick(1);
                break;
            }
            case Context::PinEdit:
                system.raw_stimulus([&](auto&) { session.reset(); });
                break;
            case Context::PeripheralEdit:
                system.io().pia1().inputs([&](auto&) { session.reset(); });
                break;
            case Context::ScheduledStimulus:
                system.execution().within(10)
                    .after(1, [&](System&) { session.reset(); })
                    .run("rejected session destruction");
                break;
            case Context::ScopedSpi:
                system.spi().transaction([&](auto&) { session.reset(); });
                break;
            case Context::RawSpi:
                system.drive_spi(false, false, false);
                session.reset();
                break;
            }
        }, "reported rejected teardown: firmware teardown cannot abandon an active session");
    }
    // The rejecting child processes must never corrupt the parent registry.
    EXPECT_NO_THROW(FirmwareSession next(system));
}

// A rejected fixture close retains a live owner, unlike a rejected destructor.
TEST_F(FirmwareTest, FirmwareRejectedFixtureTeardownRetainsOwnershipUntilRetry) {
    firmware.mount(disk_fixture::D64::empty());
    auto rx = firmware.trace_rx();
    auto observation = system.observe([&](const System&) { TearDown(); });
    EXPECT_NONFATAL_FAILURE(system.tick(1), "callback cannot mutate or advance the board");
    observation.reset();
    EXPECT_TRUE(test_file_registered("/disks/test.d64"));
    EXPECT_FALSE(system.spi().peek_failed());
    EXPECT_NO_THROW(firmware.service());
    EXPECT_NO_THROW(firmware.close());
    EXPECT_FALSE(test_file_registered("/disks/test.d64"));
    const auto frozen_end = rx.capture().window().completed();
    System other;
    EXPECT_NO_THROW(FirmwareTestSession next(other));
    EXPECT_EQ(rx.capture().window().completed(), frozen_end);
}

// Explicit selected-bus rejection must keep files and bindings available for a later close.
TEST_F(FirmwareTest, FirmwareSelectedBusCloseRetainsOwnershipUntilRetry) {
    firmware.mount(disk_fixture::D64::empty());
    system.spi().transaction([&](auto&) {
        EXPECT_THROW(firmware.close(), std::logic_error);
        EXPECT_TRUE(test_file_registered("/disks/test.d64"));
    });
    system.drive_spi(false, false, false);
    EXPECT_THROW(firmware.close(), std::logic_error);
    EXPECT_TRUE(test_file_registered("/disks/test.d64"));
    system.drive_spi(true, false, false);
    system.tick(1);
    EXPECT_NO_THROW(firmware.service());
    EXPECT_NO_THROW(firmware.close());
    EXPECT_FALSE(test_file_registered("/disks/test.d64"));
    System other;
    EXPECT_NO_THROW(FirmwareTestSession next(other));
}

// Expired callbacks cannot service a replacement owner's process-global drive.
TEST_F(SystemTest, FirmwareServiceAdapterRejectsDestroyedSessionWithoutClocks) {
    RunConfiguration::Service borrowed;
    {
        FirmwareSession session(system);
        borrowed = session.service_adapter();
    }
    FirmwareSession replacement(system);
    replacement.mount(disk_fixture::D64::empty());
    const auto start = system.time();
    EXPECT_THROW(borrowed(system, 1), std::logic_error);
    EXPECT_EQ(system.time(), start);
    EXPECT_NO_THROW(replacement.service());
}

// Host validation rejects malformed mounts without clocking or poisoning later names.
TEST_F(FirmwareTest, FirmwareMountValidationAndFailureRetainUsableSession) {
    const auto image = disk_fixture::D64::empty();
    const auto start = system.time();
    EXPECT_THROW(firmware.mount(image, nullptr), std::invalid_argument);
    EXPECT_THROW(firmware.mount(image, ""), std::invalid_argument);
    const std::string long_name(SD_PATH_MAX, 'x');
    EXPECT_THROW(firmware.mount(image, long_name.c_str()), std::out_of_range);
    EXPECT_THROW(firmware.mount(std::vector<uint8_t>{}), std::invalid_argument);
    EXPECT_THROW(firmware.mount(std::vector<uint8_t>{0}, "retry.d64"), std::runtime_error);
    EXPECT_EQ(system.time(), start);
    EXPECT_NO_THROW(firmware.mount(image, "retry.d64"));
    const auto mounted = system.time();
    EXPECT_THROW(firmware.mount(image, "retry.d64"), std::invalid_argument);
    EXPECT_EQ(system.time(), mounted);
    EXPECT_TRUE(system.ieee().read_control().contains(fpga::IeeeControlStatusBit::Enabled));
    EXPECT_NO_THROW(firmware.service());
}

// A shared-registry collision must not abort, advance the board or transfer file ownership.
TEST_F(SystemTest, FirmwareMountRejectsSharedRegistryCollisionWithoutClaimingIt) {
    constexpr const char* path = "/disks/test.d64";
    const uint8_t original[] = {0x5a, 0xa5};
    test_register_binary_file(path, original, sizeof(original), false);
    {
        FirmwareSession session(system);
        const auto start = system.time();
        expect_failure<std::invalid_argument>(system, "firmware mount",
            [&] { session.mount(disk_fixture::D64::empty()); }, "already registered");
        EXPECT_EQ(system.time(), start);
        session.close();
    }
    // Closing the rejecting session must preserve the original backing bytes.
    FILE* file = std::fopen(path, "rb");
    ASSERT_NE(file, nullptr);
    for (const auto byte : original) EXPECT_EQ(std::fgetc(file), byte);
    EXPECT_EQ(std::fgetc(file), EOF);
    EXPECT_EQ(std::fclose(file), 0);
    test_unregister_file(path);
    // Removing the external fixture makes the same default name available again.
    EXPECT_NO_THROW({
        FirmwareSession next(system);
        next.mount(disk_fixture::D64::empty());
    });
}

// Teardown releases only this session's backing stores after closing drive handles.
TEST_F(SystemTest, FirmwareClosePreservesUnownedRegisteredFixtures) {
    constexpr const char* path = "/econopet-pr18-unowned.bin";
    const uint8_t value = 0x5a;
    test_register_binary_file(path, &value, sizeof(value), false);
    {
        FirmwareSession session(system);
        session.mount(disk_fixture::D64::empty());
    }
    FILE* file = std::fopen(path, "rb");
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(std::fgetc(file), value);
    EXPECT_EQ(std::fclose(file), 0);
    test_unregister_file(path);
    EXPECT_NO_THROW({
        FirmwareSession next(system);
        next.mount(disk_fixture::D64::empty());
    });
}

// Observation and pin-edit callbacks cannot mutate C drive state before SPI rejects them.
TEST_F(FirmwareTest, FirmwareOperationsRejectObservationAndInputEditReentry) {
    const auto image = disk_fixture::D64::empty();
    const auto require_rejection = [&] {
        const auto at = system.time();
        EXPECT_THROW(firmware.mount(image), std::logic_error);
        EXPECT_THROW(firmware.service(), std::logic_error);
        EXPECT_THROW(firmware.close(), std::logic_error);
        EXPECT_EQ(system.time(), at);
    };
    auto observation = system.observe([&](const System&) { require_rejection(); });
    system.tick(1);
    observation.reset();
    system.raw_stimulus([&](auto&) { require_rejection(); });
    EXPECT_NO_THROW(firmware.mount(image));
    EXPECT_NO_THROW(firmware.service());
}

// Exact stimulus delivered inside C firmware SPI cannot unmount its live drive.
TEST_F(FirmwareTest, FirmwareOperationsRejectScheduledStimulusReentry) {
    firmware.mount(disk_fixture::D64::empty());
    bool checked = false;
    system.execution().within(10000)
        .after(7, [&](System& board) {
            EXPECT_FALSE(board.snapshot().spi_cs_ni);
            const auto at = board.time();
            EXPECT_THROW(firmware.close(), std::logic_error);
            EXPECT_THROW(firmware.mount(disk_fixture::D64::empty(), "nested.d64"), std::logic_error);
            EXPECT_THROW(firmware.service(), std::logic_error);
            EXPECT_EQ(board.time(), at);
            checked = true;
        })
        .service(firmware.service_adapter())
        .until([&](const System&) { return checked; })
        .run("firmware stimulus guard");
    EXPECT_TRUE(checked);
    EXPECT_FALSE(system.spi().peek_failed());
    EXPECT_NO_THROW(firmware.service());
}

// Real C servicing observes every FIFO byte and ATN, without inventing received EOI.
TEST_F(FirmwareTest, FirmwareRxTraceRecordsConsumedProductionFifoHeads) {
    using namespace ieee_test;
    IeeeController controller(system);
    firmware.mount(disk_fixture::D64::empty());
    auto rx = firmware.trace_rx();
    auto wire = system.trace_ieee();
    const auto listen = Listen(Device{8});
    const auto secondary = Secondary(Channel{2});
    controller.command(listen);
    controller.command(secondary);
    controller.data(0x41, Eoi::Last);
    const std::vector<Record> sent{command(listen), command(secondary), data(0x41, Eoi::Last)};
    EXPECT_EQ(wire.records(), sent);
    firmware.service();
    EXPECT_EQ(rx.records(), fifo_records(sent));
    rx.capture().require_count(3, "firmware consumed RX heads");
    EXPECT_FALSE(system.ieee().read_status().contains(fpga::IeeeStatusBit::RxReady));
}

// Semantic evidence requires fresh adjacent absolute reads, never arbitrary pipeline traffic.
TEST_F(FirmwareTest, FirmwareRxTraceRejectsStaleStatusAndGenericReads) {
    using namespace ieee_test;
    IeeeController controller(system);
    firmware.mount(disk_fixture::D64::empty());
    const auto listen = Listen(Device{8});
    const auto secondary = Secondary(Channel{2});
    controller.command(listen);
    controller.command(secondary);
    auto rx = firmware.trace_rx();
    const auto status = fpga::address(fpga::Register::IeeeStatus).value();
    const auto head = fpga::address(fpga::Register::IeeeRx).value();
    spi_read_at(status);
    const auto sampled = system.time();
    EXPECT_THROW(spi_read_at(econopet::WishboneAddressCapacity), std::out_of_range);
    EXPECT_EQ(system.time(), sampled);
    EXPECT_EQ(spi_read_at(head), 0x28);
    EXPECT_EQ(spi_read_at(head), 0x28);
    spi_read_at(status);
    spi_write_at(kSpiRamProbe.value(), 0);
    spi_read_at(head);
    spi_read_at(status);
    system.tick(1);
    spi_read_at(head);
    spi_read_at(status);
    system.spi().write(fpga::Register::IeeeRx, 0);
    EXPECT_EQ(spi_read_at(head), 0x62);
    spi_read_at(status);
    uint8_t byte{};
    spi_read(head, 1, &byte);
    EXPECT_EQ(byte, 0x62);
    ASSERT_EQ(rx.records().size(), 1);
    spi_read_at(status);
    spi_read_at(head);
    EXPECT_EQ(rx.records(), fifo_records({command(listen), command(secondary)}));
    rx.capture().require_count(2, "only adjacent absolute RX observations");
}

// A fault after opening a C-backed image still closes its handle before unregistering bytes.
TEST_F(SystemTest, FirmwareInterruptedMountReleasesFilesAndBindingOnClose) {
    FirmwareSession session(system);
    auto observation = system.observe([](const System&) { throw UnwindSentinel{}; });
    EXPECT_THROW(session.mount(disk_fixture::D64::empty()), UnwindSentinel);
    observation.reset();
    EXPECT_TRUE(system.spi().peek_failed());
    EXPECT_THROW(session.close(), std::logic_error);
    EXPECT_NO_THROW(session.close());
    System other;
    EXPECT_NO_THROW({
        FirmwareSession next(other);
        next.mount(disk_fixture::D64::empty());
    });
}

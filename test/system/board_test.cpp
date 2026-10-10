// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#include "system.h"

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>

#include <gtest/gtest.h>

#include "framework/system_test.h"

namespace {
using namespace econopet;
using namespace econopet::io;

constexpr Cycles SpiHalfPeriod{2};
constexpr Cycles SpiReleaseCycles{4};
constexpr Cycles ExecutionSlice{256};
constexpr unsigned BusWaitSlices = 80;
constexpr unsigned SpiReadyClocks = 10000;
constexpr unsigned SpiByteBits = 8;
constexpr uint8_t SpiTopBit = 0x80;
constexpr CpuAddress ResultAddress{0x0400};
constexpr SramAddress ResultBacking{ResultAddress.value()};
constexpr SramAddress BankedAddress{0x10400};
constexpr CpuAddress ViaDdrAAddress{0xe840 + std::to_underlying(ViaRegister::DdrA)};
constexpr uint8_t Marker = 0x42;
constexpr uint8_t OtherMarker = 0x5a;
constexpr uint8_t DiagnosticPortMask = 1 << 7;
constexpr uint8_t ViaPcrReset = 0;
constexpr CpuAddress ViaShiftAddress{0xe840 + std::to_underlying(ViaRegister::Shift)};
constexpr CpuAddress ProgramAddress{0x0200};
constexpr CpuAddress PhysicalProbeAddress{0x0500};
constexpr CpuAddress ResetVector6502{0xfffc};
constexpr uint8_t LoadImmediate6502 = 0xa9;
constexpr uint8_t StoreAbsolute6502 = 0x8d;
constexpr uint8_t JumpAbsolute6502 = 0x4c;

static_assert(!std::is_copy_constructible_v<System> && !std::is_move_constructible_v<System>);
static_assert(!std::is_copy_assignable_v<System> && !std::is_move_assignable_v<System>);
static_assert(std::is_copy_constructible_v<System::Snapshot>);
static_assert(std::is_copy_assignable_v<System::Snapshot>);

// Shift one raw mode-0 byte solely to exercise production boundary wiring.
uint8_t shift_byte(System& board, uint8_t data) {
    uint8_t received = 0;
    for (unsigned bit = 0; bit < SpiByteBits; ++bit) {
        const bool value = (data & SpiTopBit) != 0;
        board.drive_spi(false, false, value);
        board.tick(SpiHalfPeriod);
        board.drive_spi(false, true, value);
        board.tick(SpiHalfPeriod);
        received = static_cast<uint8_t>((received << 1) | board.snapshot().spi_sdi_o);
        data <<= 1;
    }
    board.drive_spi(false, false, false);
    board.tick(SpiHalfPeriod);
    return received;
}

// Bound the wait for production SPI command acceptance or completion.
void wait_spi_ready(System& board) {
    for (unsigned clock = 0; clock < SpiReadyClocks && board.snapshot().spi_stall_o; ++clock)
        board.tick(Cycles{1});
    ASSERT_FALSE(board.snapshot().spi_stall_o);
}

// Issue one raw command and capture its first pipelined response byte.
void spi_command(System& board, ByteView bytes, uint8_t& received) {
    // Wait for production SPI to accept the command before selecting it.
    ASSERT_NO_FATAL_FAILURE(wait_spi_ready(board));
    // Select, transmit the absolute address and data, then release the interface.
    board.drive_spi(false, false, false);
    board.tick(SpiHalfPeriod);
    received = shift_byte(board, bytes[0]);
    for (size_t index = 1; index < bytes.size(); ++index) shift_byte(board, bytes[index]);
    // Keep CS asserted until the production Wishbone request has completed.
    ASSERT_NO_FATAL_FAILURE(wait_spi_ready(board));
    board.drive_spi(true, false, false);
    board.tick(SpiReleaseCycles);
}

// Bootstrap production registers with a raw command, not a checked transport API.
void write_fpga(System& board, WishboneAddress address, uint8_t data) {
    const std::array<uint8_t, 4> command{
        static_cast<uint8_t>(ECONOPET_SPI_CMD_WRITE_AT | (address.value() >> 16)),
        static_cast<uint8_t>(address.value() >> 8), static_cast<uint8_t>(address.value()), data};
    uint8_t received = 0;
    ASSERT_NO_FATAL_FAILURE(spi_command(board, command, received));
}

// Seek through production SPI/Wishbone, then clock out the pipelined read response.
void read_fpga(System& board, WishboneAddress address, uint8_t& received) {
    const std::array<uint8_t, 3> seek{
        static_cast<uint8_t>(ECONOPET_SPI_CMD_READ_AT | (address.value() >> 16)),
        static_cast<uint8_t>(address.value() >> 8), static_cast<uint8_t>(address.value())};
    const std::array<uint8_t, 1> fetch{ECONOPET_SPI_CMD_READ_NEXT};
    uint8_t seek_response = 0;
    ASSERT_NO_FATAL_FAILURE(spi_command(board, seek, seek_response));
    ASSERT_NO_FATAL_FAILURE(spi_command(board, fetch, received));
}
} // namespace

// Every board owns its SRAM, fitted devices and full-cycle clock independently.
TEST_F(SystemTest, IndependentBoardsOwnClocksRamAndDevices) {
    System other;
    const auto start = system.time();
    const auto other_start = other.time();
    system.poke(BankedAddress, Marker);
    system.io().pia1().inputs([](auto& inputs) { inputs.port_a = Marker; });
    system.tick(ExecutionSlice);
    EXPECT_EQ(system.time() - start, ExecutionSlice);
    EXPECT_EQ(other.time(), other_start);
    EXPECT_EQ(system.peek(BankedAddress), Marker);
    EXPECT_EQ(other.peek(BankedAddress), System::IdleRamByte);
    EXPECT_EQ(other.io().pia1().peek_inputs().port_a, PortAllHigh);
    EXPECT_THROW(system.poke(SramAddress{System::RamSize}, 0), std::out_of_range);
    const auto unchanged = system.time();
    system.tick(Cycles{0});
    EXPECT_EQ(system.time(), unchanged);
}

// Boundary inputs and snapshots are detached, with combinational changes settling on tick.
TEST_F(SystemTest, PinSnapshotsAreDetachedAndStimulusSettlesOnlyOnTick) {
    const auto before = system.snapshot();
    const auto start = system.time();
    auto inputs = system.peek_stimulus();
    inputs.cpu_reset_n_i = false;
    inputs.diag_i = false;
    system.raw_stimulus([&](auto& copy) { copy = inputs; });
    EXPECT_EQ(system.time(), start);
    EXPECT_TRUE(before.stimulus.cpu_reset_n_i);
    EXPECT_FALSE(system.snapshot().stimulus.cpu_reset_n_i);
    inputs.cpu_reset_n_i = true;
    EXPECT_FALSE(system.peek_stimulus().cpu_reset_n_i);
    system.tick(Cycles{1});
    const auto after = system.snapshot();
    EXPECT_TRUE(after.cpu_reset_active_o);
    EXPECT_FALSE(after.stimulus.diag_i);
}

// Each fixture input reaches its own model pin without changing harness-owned levels.
TEST_F(SystemTest, RawStimulusMapsEveryFixtureInputToItsActualPin) {
    constexpr std::array boolean_inputs{
        &System::RawStimulus::cpu_reset_n_i, &System::RawStimulus::cpu_irq_n_i,
        &System::RawStimulus::cpu_nmi_n_i, &System::RawStimulus::diag_i,
        &System::RawStimulus::audio_det_i, &System::RawStimulus::config_hz_i,
        &System::RawStimulus::spi1_cs_ni, &System::RawStimulus::spi1_sck_i,
        &System::RawStimulus::spi1_sd_i, &System::RawStimulus::spi1_sdo_i,
        &System::RawStimulus::i2c0_scl_i, &System::RawStimulus::i2c0_sda_i,
        &System::RawStimulus::i2c1_scl_i, &System::RawStimulus::i2c1_sda_i,
        &System::RawStimulus::mcu_cec_i,
    };
    const System::RawStimulus baseline{};
    // Toggle each scalar independently so missing or swapped assignments fail.
    for (size_t index = 0; index < boolean_inputs.size(); ++index) {
        SCOPED_TRACE(index);
        auto requested = baseline;
        requested.*boolean_inputs[index] = !(requested.*boolean_inputs[index]);
        system.raw_stimulus([&](auto& inputs) { inputs = requested; });
        expect_stimulus_pins(requested);
    }
    // Drive CPU control and distinguish the two PMOD buses at their model inputs.
    for (const bool writing : {false, true}) {
        auto requested = baseline;
        requested.cpu = {ResultAddress, writing ? std::optional<uint8_t>{Marker} : std::nullopt, writing};
        requested.pmod1_i = Marker;
        requested.pmod2_i = OtherMarker;
        system.raw_stimulus([&](auto& inputs) { inputs = requested; });
        expect_stimulus_pins(requested);
    }
    // Exercise every physical spare independently, including the gap in pin numbering.
    for (size_t index = 0; index < baseline.spare.size(); ++index) {
        SCOPED_TRACE(index);
        auto requested = baseline;
        requested.spare[index] = true;
        system.raw_stimulus([&](auto& inputs) { inputs = requested; });
        expect_stimulus_pins(requested);
        EXPECT_TRUE(system.snapshot().spi_cs_ni);
        EXPECT_FALSE(system.snapshot().spi_sck_i);
        EXPECT_TRUE(system.snapshot().sys_clock_i);
    }
}

// The diagnostic switch controls PIA1 PA7 without replacing unrelated fixture inputs.
TEST_F(SystemTest, DiagnosticSwitchReachesFittedPia1WithoutChangingOtherInputs) {
    ASSERT_NO_FATAL_FAILURE(write_fpga(system, WishboneAddress{ECONOPET_WB_CPU_ADDR}, CpuControl{}.bits()));
    auto& pia = system.io().pia1();
    pia.set_control(PiaRegister::ControlA, PiaPortAccess, system.time());
    for (const uint8_t other_inputs : {Marker, OtherMarker}) {
        pia.inputs([&](auto& inputs) { inputs.port_a = other_inputs; });
        for (const bool normal : {false, true, false, true}) {
            SCOPED_TRACE(normal);
            system.raw_stimulus([&](auto& inputs) { inputs.diag_i = normal; });
            system.tick(Cycles{1});
            const uint8_t expected = other_inputs | (normal ? DiagnosticPortMask : 0);
            EXPECT_EQ(read(pia, PiaRegister::PortA, system.time()), expected);
            EXPECT_EQ(pia.peek_inputs().port_a, expected);
        }
    }
}

// SRAM and register reads remain valid while either reset driver holds the CPU.
TEST_F(SystemTest, SpiReadsMemoryAndRegistersWhileCpuIsHeldInReset) {
    system.poke(ResultBacking, Marker);
    system.poke(BankedAddress, OtherMarker);
    uint8_t received = 0;
    // Verify initial FPGA-controlled reset without first releasing the CPU.
    ASSERT_TRUE(system.snapshot().cpu_reset_active_o);
    ASSERT_NO_FATAL_FAILURE(read_fpga(system, WishboneAddress{ResultBacking.value()}, received));
    EXPECT_EQ(received, Marker);
    ASSERT_NO_FATAL_FAILURE(read_fpga(system, WishboneAddress{BankedAddress.value()}, received));
    EXPECT_EQ(received, OtherMarker);
    ASSERT_NO_FATAL_FAILURE(read_fpga(system, WishboneAddress{ECONOPET_WB_CPU_ADDR}, received));
    EXPECT_EQ(received, CpuControl{CpuControlBit::Reset}.bits());
    // The same SRAM read must work with reset released and then externally asserted.
    ASSERT_NO_FATAL_FAILURE(write_fpga(system, WishboneAddress{ECONOPET_WB_CPU_ADDR}, CpuControl{}.bits()));
    ASSERT_FALSE(system.snapshot().cpu_reset_active_o);
    ASSERT_NO_FATAL_FAILURE(read_fpga(system, WishboneAddress{ResultBacking.value()}, received));
    EXPECT_EQ(received, Marker);
    system.set_external_reset(true);
    system.tick(Cycles{1});
    ASSERT_TRUE(system.snapshot().cpu_reset_active_o);
    ASSERT_NO_FATAL_FAILURE(read_fpga(system, WishboneAddress{ResultBacking.value()}, received));
    EXPECT_EQ(received, Marker);
    ASSERT_NO_FATAL_FAILURE(read_fpga(system, WishboneAddress{BankedAddress.value()}, received));
    EXPECT_EQ(received, OtherMarker);
    EXPECT_EQ(system.peek(ResultBacking), Marker);
    EXPECT_EQ(system.peek(BankedAddress), OtherMarker);
}

// Read-only snapshots retain values after their original model and context are destroyed.
TEST_F(SystemTest, PinSnapshotOutlivesItsBoard) {
    const auto saved = [] {
        System temporary;
        temporary.set_external_interrupts(true, false);
        temporary.drive_physical_cpu({ResultAddress, Marker, true});
        return temporary.snapshot();
    }();
    EXPECT_FALSE(saved.stimulus.cpu_irq_n_i);
    EXPECT_EQ(saved.stimulus.cpu.address, ResultAddress);
    EXPECT_EQ(saved.stimulus.cpu.write_data, std::optional<uint8_t>{Marker});
    EXPECT_TRUE(saved.stimulus.cpu.sync);
}

// Snapshot mutation and reassignment are ordinary value operations, not board edits.
TEST_F(SystemTest, SnapshotsCanBeEditedAndReassignedWhilePolling) {
    auto pins = system.snapshot();
    pins.stimulus.diag_i = false;
    pins.cpu_data_i = Marker;
    EXPECT_TRUE(system.peek_stimulus().diag_i);
    EXPECT_NE(system.snapshot().cpu_data_i, Marker);
    system.set_external_interrupts(true, false);
    system.tick(Cycles{1});
    pins = system.snapshot();
    EXPECT_FALSE(pins.stimulus.cpu_irq_n_i);
    EXPECT_TRUE(pins.stimulus.diag_i);
}

// Unsupported reads and writes retain their failure boundary and permit reset recovery.
TEST_F(SystemTest, RejectedPeripheralAccessSuspendsClockUntilExternalReset) {
    for (const bool writing : {true, false}) {
        SCOPED_TRACE(writing);
        System board;
        ASSERT_NO_FATAL_FAILURE(write_fpga(board, WishboneAddress{ECONOPET_WB_CPU_ADDR}, CpuControl{}.bits()));
        unsigned observed = 0;
        const auto start = board.time();
        auto subscription = board.observe([&](const System& completed) {
            ++observed;
            EXPECT_EQ(completed.time(), start + Cycles{observed});
            EXPECT_EQ(completed.half_ticks() % HalfTicksPerCycle, 0);
            EXPECT_FALSE(completed.clock_faulted());
        });
        board.drive_physical_cpu({ViaShiftAddress,
            writing ? std::optional<uint8_t>{Marker} : std::nullopt, false});
        try {
            board.tick(ExecutionSlice);
            FAIL() << "unsupported VIA access completed";
        } catch (const std::logic_error& error) {
            const std::string message = error.what();
            EXPECT_NE(message.find("I/O"), std::string::npos) << message;
            EXPECT_NE(message.find("seed=" + std::to_string(board.seed())), std::string::npos) << message;
            EXPECT_NE(message.find("cycle=" + std::to_string(board.time().value())), std::string::npos) << message;
            EXPECT_NE(message.find("half_tick=" + std::to_string(board.half_ticks() % HalfTicksPerCycle)),
                std::string::npos) << message;
        }
        ASSERT_TRUE(board.clock_faulted());
        const auto stopped = board.half_ticks();
        const auto completed = board.time();
        EXPECT_EQ(completed, CycleTime{Cycles{stopped / HalfTicksPerCycle}});
        if (writing) EXPECT_EQ(stopped % HalfTicksPerCycle, 1);
        const auto pins = board.snapshot();
        const auto writes = board.io().via().writes().count();
        const auto io_clocks = board.io().clock_count();
        const auto callbacks = observed;
        EXPECT_EQ(observed, (completed - start).value());
        EXPECT_EQ(board.peek(ResultBacking), System::IdleRamByte);
        // A rejected retry must not silently advance or re-clock any device.
        EXPECT_THROW(board.tick(Cycles{1}), std::logic_error);
        EXPECT_EQ(board.half_ticks(), stopped);
        EXPECT_EQ(board.time(), completed);
        EXPECT_EQ(board.snapshot().cpu_addr_o, pins.cpu_addr_o);
        EXPECT_EQ(board.io().via().writes().count(), writes);
        EXPECT_EQ(board.io().clock_count(), io_clocks);
        EXPECT_EQ(board.peek(ResultBacking), System::IdleRamByte);
        EXPECT_EQ(observed, callbacks);
        // Active polling cannot claim success on an interrupted hardware clock.
        EXPECT_THROW(board.run_until([](const System&) { return true; }, 0, "faulted poll"), std::logic_error);
        EXPECT_NE(board.diagnostic("fault evidence", "").find("clock_faulted=1"), std::string::npos);
        // Reset cancels the access at its retained phase before normal clocks resume.
        board.set_external_reset(true);
        ASSERT_NO_THROW(board.tick(Cycles{0}));
        EXPECT_TRUE(board.clock_faulted());
        EXPECT_EQ(board.half_ticks(), stopped);
        EXPECT_EQ(observed, callbacks);
        ASSERT_NO_THROW(board.tick(Cycles{1}));
        EXPECT_FALSE(board.clock_faulted());
        EXPECT_EQ(board.time(), completed + Cycles{1});
        EXPECT_EQ(board.half_ticks(), (stopped / HalfTicksPerCycle + 1) * HalfTicksPerCycle);
        EXPECT_TRUE(board.snapshot().cpu_reset_active_o);
        EXPECT_EQ(board.io().via().writes().count(), writes);
        EXPECT_EQ(board.io().clock_count(), io_clocks);
        EXPECT_EQ(observed, callbacks + 1);
        // Remove the invalid stimulus and demonstrate a fresh physical SRAM access.
        board.drive_physical_cpu({ResultAddress, OtherMarker, false});
        board.set_external_reset(false);
        for (unsigned slice = 0; slice < BusWaitSlices && board.peek(ResultBacking) != OtherMarker; ++slice)
            board.tick(ExecutionSlice);
        EXPECT_EQ(board.peek(ResultBacking), OtherMarker);
    }
}

// Catching a hardware rejection cannot bypass health checks before completion.
TEST_F(SystemTest, ServiceCompletionRequiresClockRecoveryAfterCaughtPeripheralFailure) {
    enum class Recovery { None, ResetAsserted, Completed };
    for (const auto recovery : {Recovery::None, Recovery::ResetAsserted, Recovery::Completed}) {
        SCOPED_TRACE(std::to_underlying(recovery));
        System board;
        ASSERT_NO_FATAL_FAILURE(write_fpga(board, WishboneAddress{ECONOPET_WB_CPU_ADDR}, CpuControl{}.bits()));
        board.drive_physical_cpu({ViaShiftAddress, Marker, false});
        unsigned predicates = 0;
        bool caught = false;
        uint64_t failure_time = 0;
        const auto wait = [&] {
            board.service_until([&](const System& observed) {
                ++predicates;
                EXPECT_FALSE(observed.clock_faulted());
                return caught;
            }, [&](System& active, Cycles) {
                try {
                    active.tick(ExecutionSlice);
                    FAIL() << "unsupported VIA access completed";
                } catch (const std::logic_error&) {
                    caught = true;
                    EXPECT_TRUE(active.clock_faulted());
                    failure_time = active.half_ticks();
                    EXPECT_EQ(failure_time % HalfTicksPerCycle, 1);
                    if (recovery != Recovery::None) {
                        active.set_external_reset(true);
                        active.tick(recovery == Recovery::Completed ? Cycles{1} : Cycles{0});
                    }
                }
            }, ExecutionSlice + Cycles{1}, "caught peripheral failure");
        };
        if (recovery == Recovery::Completed) {
            EXPECT_NO_THROW(wait());
            EXPECT_EQ(predicates, 2u);
            EXPECT_FALSE(board.clock_faulted());
            EXPECT_EQ(board.half_ticks(), failure_time + 1);
        } else {
            EXPECT_THROW(wait(), std::logic_error);
            EXPECT_EQ(predicates, 1u);
            EXPECT_TRUE(board.clock_faulted());
            EXPECT_EQ(board.half_ticks(), failure_time);
            board.set_external_reset(true);
            EXPECT_NO_THROW(board.tick(1));
        }
        EXPECT_TRUE(caught);
        EXPECT_NO_THROW(board.run_until([](const System&) { return true; }, 0, "recovered wait"));
    }
}

// Fixture-installed instructions exercise soft-6502 read capture and SRAM write timing.
TEST_F(SystemTest, Soft6502ExecutesFixtureProgramAndIgnoresPhysicalWriteData) {
    constexpr std::array<uint8_t, 8> program{
        LoadImmediate6502, Marker,
        StoreAbsolute6502, static_cast<uint8_t>(ResultAddress.value()),
        static_cast<uint8_t>(ResultAddress.value() >> 8),
        JumpAbsolute6502, static_cast<uint8_t>(ProgramAddress.value()),
        static_cast<uint8_t>(ProgramAddress.value() >> 8),
    };
    // Install a minimal loop and reset vector directly into physical SRAM.
    for (size_t offset = 0; offset < program.size(); ++offset)
        system.poke(SramAddress{ProgramAddress.value() + offset}, program[offset]);
    system.poke(SramAddress{ResetVector6502.value()}, static_cast<uint8_t>(ProgramAddress.value()));
    system.poke(SramAddress{ResetVector6502.value() + 1}, static_cast<uint8_t>(ProgramAddress.value() >> 8));
    system.poke(ResultBacking, 0);
    system.poke(BankedAddress, 0);
    // Select under reset, then release READY for the production soft core.
    ASSERT_NO_FATAL_FAILURE(write_fpga(system, WishboneAddress{ECONOPET_WB_CPU_SEL_ADDR}, ECONOPET_CPU_SEL_SOFT_6502));
    system.drive_physical_cpu({PhysicalProbeAddress, OtherMarker, true});
    ASSERT_NO_FATAL_FAILURE(write_fpga(system, WishboneAddress{ECONOPET_WB_CPU_ADDR},
        CpuControl{CpuControlBit::Ready}.bits()));
    for (unsigned slice = 0; slice < BusWaitSlices && system.peek(ResultBacking) != Marker; ++slice)
        system.tick(ExecutionSlice);
    EXPECT_EQ(system.peek(ResultBacking), Marker);
    EXPECT_EQ(system.peek(SramAddress{PhysicalProbeAddress.value()}), System::IdleRamByte);
    EXPECT_EQ(system.peek(BankedAddress), 0);
    EXPECT_FALSE(system.snapshot().cpu_be_o);
    EXPECT_EQ(system.peek_stimulus().cpu.write_data, std::optional<uint8_t>{OtherMarker});
}

// Detached input callbacks reject cross-device mutation and recover after exceptions.
TEST_F(SystemTest, PinEditsRollbackAndRejectBoardMutation) {
    const auto before = system.peek_stimulus();
    const auto start = system.time();
    auto& via = system.io().via();
    EXPECT_THROW(system.raw_stimulus([&](auto& inputs) {
        inputs.cpu_reset_n_i = false;
        EXPECT_THROW(system.tick(Cycles{1}), std::logic_error);
        EXPECT_THROW(system.poke(ResultBacking, Marker), std::logic_error);
        EXPECT_THROW(via.reset(), std::logic_error);
        throw std::runtime_error("discard pin edit");
    }), std::runtime_error);
    EXPECT_EQ(system.peek_stimulus().cpu_reset_n_i, before.cpu_reset_n_i);
    EXPECT_EQ(system.time(), start);
    EXPECT_EQ(system.peek(ResultBacking), System::IdleRamByte);
    via.inputs([&](auto&) {
        EXPECT_THROW(system.drive_spi(false, true, false), std::logic_error);
        EXPECT_THROW(system.set_external_reset(true), std::logic_error);
    });
    system.raw_stimulus([](auto& inputs) { inputs.diag_i = false; });
    EXPECT_FALSE(system.snapshot().stimulus.diag_i);
    EXPECT_NO_THROW(system.tick(Cycles{1}));
}

// Split production SRAM address pins must not alias the lower and upper banks.
TEST_F(SystemTest, SpiBoundaryReachesProductionRamAndCpuRegisters) {
    write_fpga(system, WishboneAddress{ResultBacking.value()}, Marker);
    write_fpga(system, WishboneAddress{BankedAddress.value()}, OtherMarker);
    EXPECT_EQ(system.peek(ResultBacking), Marker);
    EXPECT_EQ(system.peek(BankedAddress), OtherMarker);
    write_fpga(system, WishboneAddress{ECONOPET_WB_CPU_SEL_ADDR}, ECONOPET_CPU_SEL_SOFT_6502);
    EXPECT_EQ(system.snapshot().cpu_selection_o, ECONOPET_CPU_SEL_SOFT_6502);
    EXPECT_TRUE(system.snapshot().cpu_reset_active_o);
    system.set_external_reset(true);
    write_fpga(system, WishboneAddress{ECONOPET_WB_CPU_ADDR}, CpuControl{}.bits());
    EXPECT_TRUE(system.snapshot().cpu_reset_active_o);
    system.set_external_reset(false);
    system.tick(Cycles{1});
    EXPECT_FALSE(system.snapshot().cpu_reset_active_o);
}

// Physical CPU write/read stimulus traverses production SRAM and VIA address decode.
TEST_F(SystemTest, PhysicalCpuStimulusWritesSramAndIoAndReleasesReadData) {
    write_fpga(system, WishboneAddress{ECONOPET_WB_CPU_ADDR}, CpuControl{}.bits());
    ASSERT_FALSE(system.snapshot().cpu_reset_active_o);
    system.poke(ResultBacking, 0);
    bool observed_commit = false;
    auto subscription = system.observe([&](const System& observed) {
        if (observed.peek(ResultBacking) == Marker) observed_commit = true;
    });
    system.drive_physical_cpu({ResultAddress, Marker, false});
    for (unsigned slice = 0; slice < BusWaitSlices && system.peek(ResultBacking) != Marker; ++slice)
        system.tick(ExecutionSlice);
    ASSERT_EQ(system.peek(ResultBacking), Marker);
    EXPECT_EQ(system.peek_stimulus().cpu.write_data, std::optional<uint8_t>{Marker});
    // A physical CPU must release data during the FPGA's SPI arbitration slots.
    write_fpga(system, WishboneAddress{BankedAddress.value()}, OtherMarker);
    EXPECT_EQ(system.peek(BankedAddress), OtherMarker);
    EXPECT_EQ(system.peek(ResultBacking), Marker);
    EXPECT_TRUE(observed_commit);
    // Drive an external peripheral and verify the completed physical write.
    system.drive_physical_cpu({ViaDdrAAddress, OtherMarker, false});
    for (unsigned slice = 0; slice < BusWaitSlices
        && system.io().via().peek(ViaRegister::DdrA) != OtherMarker; ++slice)
        system.tick(ExecutionSlice);
    ASSERT_EQ(system.io().via().peek(ViaRegister::DdrA), OtherMarker);
    ASSERT_TRUE(system.io().via().writes().last());
    EXPECT_EQ(system.io().via().writes().last()->reg, ViaRegister::DdrA);
    EXPECT_FALSE(system.time() < system.io().via().writes().last()->at);
    // Release CPU write data and wait for the read value on the resolved data bus.
    system.drive_physical_cpu({ResultAddress, std::nullopt, true});
    bool presented = false;
    for (unsigned slice = 0; slice < BusWaitSlices && !presented; ++slice) {
        system.tick(ExecutionSlice);
        const auto pins = system.snapshot();
        presented = pins.cpu_be_o && pins.cpu_we_n_o && pins.cpu_data_i == Marker;
    }
    EXPECT_TRUE(presented);
    EXPECT_TRUE(system.snapshot().stimulus.cpu.sync);
    EXPECT_FALSE(system.peek_stimulus().cpu.write_data.has_value());
    EXPECT_EQ(system.peek(ResultBacking), Marker);
}

// Board feedback must reflect VIA graphics/audio outputs, IRQ and external reset.
TEST_F(SystemTest, FittedDevicesDriveFeedbackAndResetPreservesInputsAndHistory) {
    write_fpga(system, WishboneAddress{ECONOPET_WB_CPU_ADDR}, CpuControl{}.bits());
    auto& via = system.io().via();
    via.inputs([](auto& inputs) { inputs.port_a = Marker; });
    constexpr uint8_t ViaFixedHighOutputs = 0xee;
    write(via, ViaRegister::Pcr, ViaFixedHighOutputs, system.time());
    via.set_timer1_fault(Via6522::Timer1Fault::StuckInterruptFlag);
    via.set_interrupts(ViaInterruptBit::Timer1, true, system.time());
    system.tick(Cycles{1});
    EXPECT_TRUE(system.snapshot().text_mode_i);
    EXPECT_TRUE(system.snapshot().via_cb2_i);
    EXPECT_FALSE(system.snapshot().io_irq_ni);
    EXPECT_EQ(system.io().pia1().peek_inputs().cb1, system.snapshot().jiffy_clock_o);
    const auto writes = via.writes().count();
    system.set_external_reset(true);
    system.tick(Cycles{1});
    EXPECT_EQ(via.peek(ViaRegister::Pcr), ViaPcrReset);
    EXPECT_EQ(via.peek_inputs().port_a, Marker);
    EXPECT_EQ(via.timer1_fault(), Via6522::Timer1Fault::StuckInterruptFlag);
    EXPECT_EQ(via.writes().count(), writes);
    EXPECT_TRUE(system.snapshot().io_irq_ni);
}

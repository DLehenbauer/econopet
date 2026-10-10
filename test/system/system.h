// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "Vsystem.h"
#include "verilated.h"

#include "io.h"
#include "system_state.h"

// Own one production FPGA and the physical devices fitted outside its boundary.
class System {
    friend class SystemTest;
public:
    static constexpr size_t RamSize = econopet::SramCapacity;
    static constexpr uint8_t IdleRamByte = 0xea; // 6502 NOP in uninitialized SRAM.

    // Hold physical CPU address/control, releasing write data for a read.
    struct CpuStimulus {
        econopet::CpuAddress address;
        std::optional<uint8_t> write_data;
        bool sync = false;
    };

    // Fixture-owned pins, distinct from harness-owned clock and device feedback.
    struct RawStimulus {
        CpuStimulus cpu;
        bool cpu_reset_n_i = true;
        bool cpu_irq_n_i = true;
        bool cpu_nmi_n_i = true;
        bool diag_i = true;
        bool audio_det_i = false;
        bool config_hz_i = false;
        bool spi1_cs_ni = true;
        bool spi1_sck_i = false;
        bool spi1_sd_i = false;
        bool spi1_sdo_i = true;
        bool i2c0_scl_i = true;
        bool i2c0_sda_i = true;
        bool i2c1_scl_i = true;
        bool i2c1_sda_i = true;
        bool mcu_cec_i = true;
        uint8_t pmod1_i = 0;
        uint8_t pmod2_i = 0;
        std::array<bool, 6> spare{};
    };

    // Detached boundary values never expose writable generated-model storage.
    struct Snapshot {
        RawStimulus stimulus;
        bool graphic_i, via_cb2_i;
        bool sys_clock_i, spi_cs_ni, spi_sck_i, spi_sdo_i;
        bool config_crt_i, config_keyboard_i, io_irq_ni;
        uint8_t cpu_data_i;
        bool status_no;
        bool cpu_reset_n_o, cpu_reset_n_oe, cpu_be_o, cpu_clock_o, cpu_ready_o;
        econopet::CpuAddress cpu_addr_o;
        uint16_t cpu_addr_oe;
        uint8_t cpu_data_o, cpu_data_oe;
        bool cpu_we_n_o, cpu_we_n_oe;
        bool cpu_irq_n_o, cpu_irq_n_oe, cpu_nmi_n_o, cpu_nmi_n_oe;
        bool ram_addr_a10_o, ram_addr_a11_o, ram_addr_a15_o, ram_addr_a16_o;
        bool ram_oe_n_o, ram_we_n_o, io_oe_n_o;
        bool pia1_cs_n_o, pia2_cs_n_o, via_cs_n_o, pia1_clock_o;
        bool spi_sdi_o, spi_stall_o, spi1_sdo_o, spi1_sdo_oe;
        bool i2c0_scl_o, i2c0_scl_oe, i2c0_sda_o, i2c0_sda_oe;
        bool i2c1_scl_o, i2c1_scl_oe, i2c1_sda_o, i2c1_sda_oe;
        bool mcu_cec_o, mcu_cec_oe;
        bool horiz_drive_o, vert_drive_o, jiffy_clock_o, video_o, audio_l_o, audio_r_o;
        uint8_t pmod1_o, pmod1_oe, pmod2_o, pmod2_oe;
        std::array<bool, 6> spare_o, spare_oe;
        bool cpu_reset_active_o;
        uint8_t cpu_selection_o;

    private:
        friend class System;
        // Copy the last evaluated boundary without retaining its board.
        explicit Snapshot(const Vsystem& p, const RawStimulus& inputs)
            : stimulus(inputs), graphic_i(p.graphic_i), via_cb2_i(p.via_cb2_i),
              sys_clock_i(p.sys_clock_i), spi_cs_ni(p.spi_cs_ni), spi_sck_i(p.spi_sck_i),
              spi_sdo_i(p.spi_sdo_i), config_crt_i(p.config_crt_i),
              config_keyboard_i(p.config_keyboard_i), io_irq_ni(p.io_irq_ni),
              cpu_data_i(p.cpu_data_i), status_no(p.status_no),
              cpu_reset_n_o(p.cpu_reset_n_o), cpu_reset_n_oe(p.cpu_reset_n_oe),
              cpu_be_o(p.cpu_be_o), cpu_clock_o(p.cpu_clock_o), cpu_ready_o(p.cpu_ready_o),
              cpu_addr_o(p.cpu_addr_o), cpu_addr_oe(p.cpu_addr_oe),
              cpu_data_o(p.cpu_data_o), cpu_data_oe(p.cpu_data_oe),
              cpu_we_n_o(p.cpu_we_n_o), cpu_we_n_oe(p.cpu_we_n_oe),
              cpu_irq_n_o(p.cpu_irq_n_o), cpu_irq_n_oe(p.cpu_irq_n_oe),
              cpu_nmi_n_o(p.cpu_nmi_n_o), cpu_nmi_n_oe(p.cpu_nmi_n_oe),
              ram_addr_a10_o(p.ram_addr_a10_o), ram_addr_a11_o(p.ram_addr_a11_o),
              ram_addr_a15_o(p.ram_addr_a15_o), ram_addr_a16_o(p.ram_addr_a16_o),
              ram_oe_n_o(p.ram_oe_n_o), ram_we_n_o(p.ram_we_n_o), io_oe_n_o(p.io_oe_n_o),
              pia1_cs_n_o(p.pia1_cs_n_o), pia2_cs_n_o(p.pia2_cs_n_o), via_cs_n_o(p.via_cs_n_o),
              pia1_clock_o(p.pia1_clock_o),
              spi_sdi_o(p.spi_sdi_o), spi_stall_o(p.spi_stall_o),
              spi1_sdo_o(p.spi1_sdo_o), spi1_sdo_oe(p.spi1_sdo_oe),
              i2c0_scl_o(p.i2c0_scl_o), i2c0_scl_oe(p.i2c0_scl_oe),
              i2c0_sda_o(p.i2c0_sda_o), i2c0_sda_oe(p.i2c0_sda_oe),
              i2c1_scl_o(p.i2c1_scl_o), i2c1_scl_oe(p.i2c1_scl_oe),
              i2c1_sda_o(p.i2c1_sda_o), i2c1_sda_oe(p.i2c1_sda_oe),
              mcu_cec_o(p.mcu_cec_o), mcu_cec_oe(p.mcu_cec_oe),
              horiz_drive_o(p.horiz_drive_o), vert_drive_o(p.vert_drive_o),
              jiffy_clock_o(p.jiffy_clock_o), video_o(p.video_o), audio_l_o(p.audio_l_o), audio_r_o(p.audio_r_o),
              pmod1_o(p.pmod1_o), pmod1_oe(p.pmod1_oe),
              pmod2_o(p.pmod2_o), pmod2_oe(p.pmod2_oe),
              spare_o{bool(p.sp1_o), bool(p.sp2_o), bool(p.sp3_o),
                      bool(p.sp6_o), bool(p.sp7_o), bool(p.sp8_o)},
              spare_oe{bool(p.sp1_oe), bool(p.sp2_oe), bool(p.sp3_oe),
                       bool(p.sp6_oe), bool(p.sp7_oe), bool(p.sp8_oe)},
              cpu_reset_active_o(p.cpu_reset_active_o), cpu_selection_o(p.cpu_selection_o) {}
    };

    // Initialize inactive pins and SRAM before settling the production top.
    System()
        : context_(make_context()), dut_(std::make_unique<Vsystem>(context_.get())),
          io_([this] { require_mutable_access(); }) {
        ram_.fill(IdleRamByte);
        dut_->sys_clock_i = 0;
        apply_raw_stimulus(stimulus_);
        dut_->cpu_data_i = econopet::io::PortAllHigh;
        dut_->io_irq_ni = 1;
        set_display(pet_video_type_crtc);
        set_keyboard(pet_keyboard_model_business);
        dut_->graphic_i = 0;
        dut_->via_cb2_i = 1;
        drive_spi(true, false, false);
        tick(InitialSettleCycles);
    }

    // Finalize the model while its private context is still alive.
    ~System() { dut_->final(); }
    System(const System&) = delete;
    System& operator=(const System&) = delete;
    System(System&&) = delete;
    System& operator=(System&&) = delete;

    // Inspect a detached copy without evaluating or clocking the FPGA.
    Snapshot snapshot() const { return Snapshot(*dut_, stimulus_); }
    // Return completed whole cycles, including when an interrupted cycle is pending.
    econopet::CycleTime time() const {
        return econopet::CycleTime{econopet::Cycles{context_->time() / econopet::HalfTicksPerCycle}};
    }
    // Inspect the exact failure boundary without coercing it into a whole cycle.
    uint64_t half_ticks() const { return context_->time(); }
    // Report a suspended tick that requires external reset before continuation.
    bool clock_faulted() const { return clock_faulted_; }
    // Read physical SRAM without going through the CPU or SPI bus.
    uint8_t peek(econopet::SramAddress address) const { return ram_[address.value()]; }
    // Patch physical SRAM for fixture setup without advancing the board.
    void poke(econopet::SramAddress address, uint8_t value) {
        require_mutable_access();
        ram_[address.value()] = value;
    }
    // Borrow fitted devices from a persistent board for external wiring and setup.
    econopet::io::Io& io() & { require_mutable_access(); return io_; }
    // Inspect fitted devices and their write histories without mutation.
    const econopet::io::Io& io() const & { return io_; }
    // Device borrows must not escape temporary boards.
    econopet::io::Io& io() && = delete;
    const econopet::io::Io& io() const && = delete;

    // Drive the external reset source, not the production CPU-control register.
    void set_external_reset(bool asserted) {
        require_mutable_access();
        stimulus_.cpu_reset_n_i = dut_->cpu_reset_n_i = !asserted;
    }
    // Drive independent interrupt sources that combine with fitted-device IRQs.
    void set_external_interrupts(bool irq_asserted, bool nmi_asserted) {
        require_mutable_access();
        stimulus_.cpu_irq_n_i = dut_->cpu_irq_n_i = !irq_asserted;
        stimulus_.cpu_nmi_n_i = dut_->cpu_nmi_n_i = !nmi_asserted;
    }
    // Hold address/control and optional physical CPU write data between ticks.
    void drive_physical_cpu(const CpuStimulus& cpu) {
        require_mutable_access();
        stimulus_.cpu = cpu;
        dut_->cpu_addr_i = cpu.address.value();
        dut_->cpu_we_n_i = !cpu.write_data.has_value();
        dut_->cpu_sync_i = cpu.sync;
    }
    // Drive SPI0 boundary pins only (no transaction ownership or command policy).
    void drive_spi(bool cs_n, bool clock, bool data) {
        require_mutable_access();
        dut_->spi_cs_ni = cs_n;
        dut_->spi_sck_i = clock;
        dut_->spi_sdo_i = data;
    }
    // Set the display DIP input using the production configuration enum.
    void set_display(pet_video_type_t display) {
        require_mutable_access();
        switch (display) {
        case pet_video_type_crtc:
        case pet_video_type_fixed:
            dut_->config_crt_i = std::to_underlying(display);
            return;
        }
        throw std::invalid_argument("unsupported display configuration");
    }
    // Set the keyboard DIP input using the production configuration enum.
    void set_keyboard(pet_keyboard_model_t keyboard) {
        require_mutable_access();
        switch (keyboard) {
        case pet_keyboard_model_business:
        case pet_keyboard_model_graphics:
            dut_->config_keyboard_i = std::to_underlying(keyboard);
            return;
        }
        throw std::invalid_argument("unsupported keyboard configuration");
    }
    // Copy fixture-owned levels without retaining their board.
    RawStimulus peek_stimulus() const { return stimulus_; }
    // Commit a detached pin edit only after successful callback completion.
    template<class Edit>
    void raw_stimulus(Edit&& edit) {
        require_mutable_access();
        auto copy = stimulus_;
        {
            struct Scope {
                bool& active;
                // Reject board mutation during the detached edit callback.
                explicit Scope(bool& value) : active(value) { active = true; }
                // Restore access after success or an exception.
                ~Scope() { active = false; }
            } scope(editing_stimulus_);
            std::invoke(std::forward<Edit>(edit), copy);
        }
        apply_raw_stimulus(copy);
    }

    // Advance complete system clocks, resolving external devices at both phases.
    void tick(econopet::Cycles cycles) {
        require_mutable_access();
        if (clock_faulted_ && stimulus_.cpu_reset_n_i)
            throw std::logic_error("board clock is suspended after failure: assert external reset before ticking");
        const auto remaining = cycles.half_ticks() - (cycles.value() != 0 && phase_ == ClockPhase::HighPending ? 1 : 0);
        if (remaining > std::numeric_limits<uint64_t>::max() - context_->time())
            throw std::overflow_error("board clock would overflow simulation time");
        struct TickScope {
            bool& faulted;
            bool completed = false;
            // Preserve the fault on exceptional exits without altering clock or device state.
            explicit TickScope(bool& state) : faulted(state) {}
            // Any interrupted tick requires explicit reset recovery.
            ~TickScope() { if (!completed) faulted = true; }
        } scope(clock_faulted_);
        for (uint64_t index = 0; index < cycles.value(); ++index) {
            const auto completed = time() + econopet::Cycles{1};
            if (phase_ == ClockPhase::LowPending) {
                // Present data before the rising edge, or resume here after a low-phase failure.
                dut_->sys_clock_i = 0;
                dut_->eval();
                sample_io(completed);
                dut_->cpu_data_i = bus_read_data();
                dut_->eval();
                context_->timeInc(1);
                phase_ = ClockPhase::HighPending;
            }
            // Advance the production top and complete external PHI2 accesses.
            // Recovery reevaluates asserted reset at the held high level, not a new clock edge.
            dut_->sys_clock_i = 1;
            dut_->eval();
            sample_io(completed);
            // Latch the last driven SRAM data only when write enable releases.
            if (!dut_->ram_we_n_o && ((dut_->cpu_data_oe & DataBusEnabled) || physical_cpu_driving())) {
                ram_write_pending_ = true;
                ram_write_address_ = ram_address();
                ram_write_data_ = write_bus_data();
            }
            if (ram_we_was_low_ && dut_->ram_we_n_o && ram_write_pending_) {
                ram_[ram_write_address_.value()] = ram_write_data_;
                ram_write_pending_ = false;
            }
            ram_we_was_low_ = !dut_->ram_we_n_o;
            context_->timeInc(1);
            phase_ = ClockPhase::LowPending;
            clock_faulted_ = false;
        }
        scope.completed = true;
    }

private:
    enum class ClockPhase { LowPending, HighPending };
    static constexpr econopet::Cycles InitialSettleCycles{8};
    static constexpr uint16_t SharedRamAddressMask = 0x73ff;
    static constexpr uint8_t DataBusEnabled = 1;
    static constexpr uint8_t JiffyPortMask = 1 << 5;
    static constexpr uint8_t DiagnosticPortMask = 1 << 7;
    static constexpr int DefaultSeed = 1;
    static constexpr int ZeroInitialState = 0;

    // Initialize each model independently without modifying global Verilator RNGs.
    static std::unique_ptr<VerilatedContext> make_context() {
        auto context = std::make_unique<VerilatedContext>();
        context->randSeed(DefaultSeed);
        context->randReset(ZeroInitialState);
        return context;
    }
    // Prevent pin-edit callbacks from mutating or advancing their fitted board.
    void require_mutable_access() const {
        if (editing_stimulus_ || io_.editing_inputs())
            throw std::logic_error("pin edit callback cannot mutate or advance the board");
    }
    // Apply fixture pins without evaluating the model or changing time.
    void apply_raw_stimulus(const RawStimulus& stimulus) {
        require_mutable_access();
        stimulus_ = stimulus;
        drive_physical_cpu(stimulus.cpu);
        dut_->cpu_reset_n_i = stimulus.cpu_reset_n_i;
        dut_->cpu_irq_n_i = stimulus.cpu_irq_n_i;
        dut_->cpu_nmi_n_i = stimulus.cpu_nmi_n_i;
        dut_->diag_i = stimulus.diag_i;
        dut_->audio_det_i = stimulus.audio_det_i;
        dut_->config_hz_i = stimulus.config_hz_i;
        dut_->spi1_cs_ni = stimulus.spi1_cs_ni;
        dut_->spi1_sck_i = stimulus.spi1_sck_i;
        dut_->spi1_sd_i = stimulus.spi1_sd_i;
        dut_->spi1_sdo_i = stimulus.spi1_sdo_i;
        dut_->i2c0_scl_i = stimulus.i2c0_scl_i;
        dut_->i2c0_sda_i = stimulus.i2c0_sda_i;
        dut_->i2c1_scl_i = stimulus.i2c1_scl_i;
        dut_->i2c1_sda_i = stimulus.i2c1_sda_i;
        dut_->mcu_cec_i = stimulus.mcu_cec_i;
        dut_->pmod1_i = stimulus.pmod1_i;
        dut_->pmod2_i = stimulus.pmod2_i;
        dut_->sp1_i = stimulus.spare[0];
        dut_->sp2_i = stimulus.spare[1];
        dut_->sp3_i = stimulus.spare[2];
        dut_->sp6_i = stimulus.spare[3];
        dut_->sp7_i = stimulus.spare[4];
        dut_->sp8_i = stimulus.spare[5];
    }
    // Reconstruct the physical SRAM address from split production address pins.
    econopet::SramAddress ram_address() const {
        return econopet::SramAddress{(static_cast<uint32_t>(dut_->ram_addr_a16_o) << 16)
             | (static_cast<uint32_t>(dut_->ram_addr_a15_o) << 15)
             | (static_cast<uint32_t>(dut_->ram_addr_a11_o) << 11)
             | (static_cast<uint32_t>(dut_->ram_addr_a10_o) << 10)
             | (dut_->cpu_addr_o & SharedRamAddressMask)};
    }
    // A physical CPU releases its data whenever the FPGA deasserts BE.
    bool physical_cpu_driving() const {
        return dut_->cpu_be_o && !dut_->cpu_we_n_i && stimulus_.cpu.write_data.has_value();
    }
    // Sample the actual write source, not an undriven FPGA output.
    uint8_t write_bus_data() const {
        return physical_cpu_driving() ? *stimulus_.cpu.write_data : dut_->cpu_data_o;
    }
    // Resolve CPU, FPGA, fitted I/O, SRAM and undriven data ownership.
    uint8_t bus_read_data() const {
        if (physical_cpu_driving()) {
            if (dut_->cpu_data_oe)
                throw std::runtime_error("physical CPU and FPGA both drive the data bus");
            return *stimulus_.cpu.write_data;
        }
        if (dut_->cpu_data_oe & DataBusEnabled) return dut_->cpu_data_o;
        if (!dut_->io_oe_n_o && dut_->cpu_we_n_o) return io_read_data();
        return dut_->ram_oe_n_o ? econopet::io::PortAllHigh : ram_[ram_address().value()];
    }
    // Preview the physically selected peripheral without acknowledging its read.
    uint8_t io_read_data() const {
        const uint8_t reg = dut_->cpu_addr_o & econopet::io::ViaRegisterMask;
        if (!dut_->pia1_cs_n_o) return io_.pia1().peek(
            static_cast<econopet::io::PiaRegister>(reg & econopet::io::PiaRegisterMask));
        if (!dut_->pia2_cs_n_o) return io_.pia2().peek(
            static_cast<econopet::io::PiaRegister>(reg & econopet::io::PiaRegisterMask));
        if (!dut_->via_cs_n_o) {
            // Reset cancels the unsupported shift read, not SRAM or supported device reads.
            if (dut_->cpu_reset_active_o && reg == std::to_underlying(econopet::io::ViaRegister::Shift))
                return econopet::io::PortAllHigh;
            return io_.via().peek(static_cast<econopet::io::ViaRegister>(reg));
        }
        throw std::runtime_error("I/O output enable asserted without a peripheral select");
    }
    // Connect PHI2, reset, selects, diagnostic sense, jiffy, IRQ and VIA outputs.
    void sample_io(econopet::CycleTime completed) {
        // Feed shared physical levels while preserving unrelated fixture inputs.
        io_.pia1().inputs([&](auto& inputs) {
            inputs.cb1 = dut_->jiffy_clock_o;
            inputs.port_a = static_cast<uint8_t>((inputs.port_a & ~DiagnosticPortMask)
                | (stimulus_.diag_i ? DiagnosticPortMask : 0));
        });
        io_.via().inputs([&](auto& inputs) {
            inputs.port_b = static_cast<uint8_t>((inputs.port_b & ~JiffyPortMask)
                | (dut_->jiffy_clock_o ? JiffyPortMask : 0));
        });
        // Complete the physical access and return resolved device feedback.
        using econopet::io::ChipSelect;
        const econopet::io::ChipSelects selects = (!dut_->pia1_cs_n_o ? ChipSelect::Pia1 : ChipSelect::None)
            | (!dut_->pia2_cs_n_o ? ChipSelect::Pia2 : ChipSelect::None)
            | (!dut_->via_cs_n_o ? ChipSelect::Via : ChipSelect::None);
        io_.sample({dut_->cpu_clock_o != 0, dut_->cpu_reset_active_o != 0, selects,
            static_cast<uint8_t>(dut_->cpu_addr_o & econopet::io::ViaRegisterMask),
            write_bus_data(), dut_->cpu_we_n_o == 0, dut_->pia1_clock_o != 0}, completed);
        dut_->io_irq_ni = !io_.irq();
        dut_->graphic_i = io_.via().ca2();
        dut_->via_cb2_i = io_.via().cb2();
    }

    bool editing_stimulus_ = false;
    bool clock_faulted_ = false;
    ClockPhase phase_ = ClockPhase::LowPending;
    std::unique_ptr<VerilatedContext> context_;
    std::unique_ptr<Vsystem> dut_;
    RawStimulus stimulus_;
    std::array<uint8_t, RamSize> ram_;
    econopet::io::Io io_;
    bool ram_we_was_low_ = false;
    bool ram_write_pending_ = false;
    econopet::SramAddress ram_write_address_;
    uint8_t ram_write_data_ = 0;
};

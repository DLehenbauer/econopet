// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "Vsystem.h"
#include "verilated.h"

#include "driver.h"
#include "io.h"
#include "registers.h"
#include "system_state.h"

namespace system_detail {
template<class> struct IsReferenceWrapper : std::false_type {};
template<class T> struct IsReferenceWrapper<std::reference_wrapper<T>> : std::true_type {};
template<class T>
concept AllowedSpiResult = !std::is_reference_v<T> && !std::is_pointer_v<T>
    && !IsReferenceWrapper<std::remove_cv_t<T>>::value;
}

// Own one production FPGA and the physical devices fitted outside its boundary.
class System {
    friend class SystemTest;
    struct ObserverEntry {
        std::function<void(const System&)> callback;
    };
    struct ObserverRegistry {
        std::vector<std::weak_ptr<ObserverEntry>> entries;
    };
public:
    static constexpr size_t RamSize = econopet::SramCapacity;
    static constexpr uint8_t IdleRamByte = 0xea; // 6502 NOP in uninitialized SRAM.

    enum class InitialState { Zero, Random };

    // Parse a reproducible simulation seed without process-global RNG state.
    static uint32_t parse_seed(std::string_view text) {
        if (text.empty())
            throw std::invalid_argument("simulation seed: empty seed (expected decimal integer in [1, INT_MAX])");
        uint32_t value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
            || value == 0 || value > static_cast<uint32_t>(std::numeric_limits<int>::max()))
            throw std::invalid_argument("simulation seed: expected decimal integer in [1, INT_MAX]");
        return value;
    }
    // Use a fixed default or an explicit environment seed independently on each board.
    static uint32_t environment_seed() {
        const char* text = std::getenv("ECONOPET_SIM_SEED");
        return text ? parse_seed(text) : DefaultSeed;
    }

    // Own one observer without retaining or dereferencing its board.
    class ObserverSubscription {
    public:
        // An empty subscription owns no callback.
        ObserverSubscription() = default;
        // Disconnect before captures in the enclosing scope are destroyed.
        ~ObserverSubscription() { reset(); }
        ObserverSubscription(const ObserverSubscription&) = delete;
        ObserverSubscription& operator=(const ObserverSubscription&) = delete;
        // Transfer ownership without changing registration order.
        ObserverSubscription(ObserverSubscription&&) noexcept = default;
        // Disconnect the previous callback before taking another registration.
        ObserverSubscription& operator=(ObserverSubscription&& other) noexcept {
            if (this != &other) {
                reset();
                registry_ = std::move(other.registry_);
                entry_ = std::move(other.entry_);
            }
            return *this;
        }
        // Disconnect idempotently, including from inside an executing callback.
        void reset() noexcept {
            entry_.reset();
            registry_.reset();
        }
        // Report whether the callback still belongs to a live board.
        bool connected() const noexcept {
            return entry_ && !registry_.expired();
        }
    private:
        friend class System;
        // Retain the callback while keeping board lifetime independent.
        ObserverSubscription(const std::shared_ptr<ObserverRegistry>& registry,
                             std::shared_ptr<ObserverEntry> entry)
            : registry_(registry), entry_(std::move(entry)) {}
        std::weak_ptr<ObserverRegistry> registry_;
        std::shared_ptr<ObserverEntry> entry_;
    };

    // Own selected SPI until completion or deliberate raw abort.
    class SpiTransaction {
    public:
        // Fail closed without clocking or invoking observers during unwinding.
        ~SpiTransaction() { if (!closed_) system_.fail_spi("SPI abandoned"); }
        SpiTransaction(const SpiTransaction&) = delete;
        SpiTransaction& operator=(const SpiTransaction&) = delete;
        SpiTransaction(SpiTransaction&&) = delete;
        SpiTransaction& operator=(SpiTransaction&&) = delete;

        // Exchange a complete byte while this scope owns the selected bus.
        uint8_t byte(uint8_t value) {
            require_open();
            if (raw_) system_.remember("raw SPI byte", 0, value);
            return system_.exchange_spi_byte(value, half_period_);
        }
        // Validate a command and return its first byte from the production RX pipeline.
        uint8_t command(econopet::ByteView bytes) {
            require_open();
            system_.validate_spi_command(bytes);
            system_.remember_spi_command(bytes);
            system_.wait_ready();
            const auto received = byte(bytes[0]);
            for (size_t index = 1; index < bytes.size(); ++index) byte(bytes[index]);
            return received;
        }
        // Clock a raw bit with fixture-selected mode-0 timing.
        bool bit(bool value) {
            require_open();
            if (!raw_) throw system_.failure<std::logic_error>("SPI bit", "requires a raw transaction");
            return system_.exchange_spi_bit(value, half_period_);
        }
        // Drain the production controller, then release CS and settle the bus.
        void finish() {
            require_open();
            system_.wait_ready();
            close();
        }
        // Release a raw CS scope without rolling back an already issued request.
        void abort() {
            require_open();
            if (!raw_) throw system_.failure<std::logic_error>("SPI abort", "requires a raw transaction");
            close();
        }
    private:
        friend class System;
        // Reserve the bus before any clock or callback can fail.
        SpiTransaction(System& system, bool raw, econopet::Cycles half_period)
            : system_(system), raw_(raw), half_period_(half_period) {
            system_.require_spi_idle();
            if (half_period.value() == 0)
                throw system_.failure<std::invalid_argument>("SPI transaction", "half period must be positive");
            system_.spi_active_ = true;
            system_.remember(raw ? "raw SPI begin" : "SPI begin");
            try {
                if (!raw_) system_.wait_ready();
                system_.dut_->spi_cs_ni = 0;
                if (!raw_) system_.tick(half_period_);
            } catch (...) {
                system_.fail_spi("SPI setup failed");
                throw;
            }
        }
        // Reject closed scopes, failed boards and captured-reference mutation.
        void require_open() const {
            system_.require_mutable_access();
            if (closed_ || system_.spi_failed_ || !system_.spi_active_ || system_.dut_->spi_cs_ni)
                throw system_.failure<std::logic_error>("SPI transfer/release", "requires an active selected transaction");
        }
        // Mark completion only after all release clocks succeed.
        void close() {
            system_.dut_->spi_cs_ni = 1;
            system_.dut_->spi_sck_i = 0;
            system_.tick(SpiReleaseCycles);
            system_.spi_active_ = false;
            closed_ = true;
            system_.remember("SPI closed");
        }
        System& system_;
        bool raw_;
        econopet::Cycles half_period_;
        bool closed_ = false;
    };

    template<class Board> class SpiView;
    // Borrow complete-command access only for a checked callback's lifetime.
    class SpiCommands {
    public:
        SpiCommands(const SpiCommands&) = delete;
        SpiCommands& operator=(const SpiCommands&) = delete;
        // Validate and issue one command while preserving the production pipeline.
        uint8_t command(econopet::ByteView bytes) { return transaction_.command(bytes); }
    private:
        template<class> friend class SpiView;
        // Bind commands to the callback's transaction owner.
        explicit SpiCommands(SpiTransaction& transaction) : transaction_(transaction) {}
        SpiTransaction& transaction_;
    };

    // Borrow active SPI transport and passive failure state from a persistent board.
    template<class Board>
    class SpiView {
    public:
        // Bind the view without clocking or retaining the board.
        explicit SpiView(Board& board) : board_(board) {}
        SpiView(Board&&) = delete;
        // Read a checked address through the real pipelined SPI/Wishbone bus.
        uint8_t read(econopet::WishboneAddress address) requires (!std::is_const_v<Board>) {
            return board_.spi_read_at(address);
        }
        // Read a named register through the same transport.
        uint8_t read(fpga::Register reg) requires (!std::is_const_v<Board>) {
            return read(fpga::address(reg));
        }
        // Write a checked address using production address decoding.
        void write(econopet::WishboneAddress address, uint8_t value) requires (!std::is_const_v<Board>) {
            board_.spi_write_at(address, value);
        }
        // Write a named register, preserving byte-level register semantics.
        void write(fpga::Register reg, uint8_t value) requires (!std::is_const_v<Board>) {
            write(fpga::address(reg), value);
        }
        // Issue exactly one validated command under scoped CS ownership.
        uint8_t command(econopet::ByteView bytes) requires (!std::is_const_v<Board>) {
            return board_.spi_command(bytes);
        }
        // Finish before returning a value (hidden borrows remain the caller's responsibility).
        template<class Work>
            requires (!std::is_const_v<Board>) && std::invocable<Work, SpiCommands&>
                && system_detail::AllowedSpiResult<std::invoke_result_t<Work, SpiCommands&>>
        auto transaction(Work&& work) {
            using Result = std::invoke_result_t<Work, SpiCommands&>;
            auto owner = board_.spi_transaction();
            SpiCommands commands(owner);
            if constexpr (std::is_void_v<Result>) {
                std::invoke(std::forward<Work>(work), commands);
                owner.finish();
            } else {
                auto result = std::invoke(std::forward<Work>(work), commands);
                owner.finish();
                return result;
            }
        }
        // Opt into partial commands, custom bit timing and deliberate aborts.
        SpiTransaction raw_transaction(econopet::Cycles half_period = SpiHalfPeriodCycles)
            requires (!std::is_const_v<Board>) {
            return SpiTransaction(board_, true, half_period);
        }
        // Inspect fail-closed transport state without clocks.
        bool peek_failed() const { return board_.spi_failed_; }
        // Wait for production STALL to clear under the inclusive inherited deadline.
        void wait_ready(econopet::Cycles budget = SpiStallBudget) requires (!std::is_const_v<Board>) {
            board_.wait_ready(budget);
        }
    private:
        Board& board_;
    };

    // Borrow shared CPU lifecycle controls, optionally bound to a selected core.
    template<class Board>
    class CpuView {
    public:
        // Bind and validate a target without a bus transaction.
        explicit CpuView(Board& board, std::optional<cpu_type_t> target = {})
            : board_(board), target_(target) {
            if (target) board_.validate_cpu(*target);
        }
        CpuView(Board&&, std::optional<cpu_type_t> = {}) = delete;
        // Read selection through SPI, validating the returned encoding.
        cpu_type_t read_selection() requires (!std::is_const_v<Board>) { return board_.read_cpu_selection(); }
        // Inspect selected core directly without clocks.
        cpu_type_t peek_selection() const { return board_.decode_cpu(board_.dut_->cpu_selection_o); }
        // Read CPU control flags through production SPI.
        econopet::CpuControl read_control() requires (!std::is_const_v<Board>) { return board_.read_cpu_control(); }
        // Inspect the resolved physical reset net.
        bool peek_reset() const { return board_.dut_->cpu_reset_active_o; }
        // Inspect the shared monotonic fetch/SYNC checkpoint.
        uint64_t peek_fetch_checkpoint() const { return board_.fetch_sequence_; }
        // Assert shared reset and drain admitted bus activity without clearing history.
        void assert_reset() requires (!std::is_const_v<Board>) { board_.assert_reset(); }
        // Select this core only while CPU control has established reset quiescence.
        void select() requires (!std::is_const_v<Board>) { board_.select_cpu(target()); }
        // Establish reset and selection before installing fixture code or vectors.
        void prepare() requires (!std::is_const_v<Board>) {
            const auto selected = target();
            board_.assert_reset();
            board_.select_cpu(selected);
        }
        // Release reset with the selected core's READY policy.
        void release_reset() requires (!std::is_const_v<Board>) {
            board_.require_mutable_access();
            if (target_ && peek_selection() != *target_)
                throw board_.template failure<std::logic_error>("release_reset", "bound CPU is not selected");
            board_.release_reset();
        }
        // Install a native-endian reset vector only while this core is quiescent.
        void write_vector(econopet::CpuAddress entry) requires (!std::is_const_v<Board>) {
            board_.require_mutable_access();
            const auto selected = target();
            if (selected == CPU_PHYS_6502)
                throw board_.template failure<std::logic_error>("CPU vector", "physical CPU vectors require explicit memory setup");
            if (!peek_reset() || peek_selection() != selected || board_.ram_write_pending_)
                throw board_.template failure<std::logic_error>("CPU vector", "requires the bound CPU quiescent in reset");
            board_.require_spi_idle();
            // Propagate unscoped raw CS release before inspecting admitted Wishbone work.
            board_.tick(SpiReleaseCycles);
            board_.run_until([](const System& board) {
                return board.dut_->spi_quiescent_o && !board.ram_write_pending_;
            }, SpiStallBudget, "CPU vector transport quiescence");
            if (!peek_reset() || peek_selection() != selected || board_.ram_write_pending_)
                throw board_.template failure<std::logic_error>("CPU vector", "CPU lost quiescence while draining SPI");
            const bool little = selected == CPU_SOFT_6502;
            const auto vector = little ? pet::Reset6502 : pet::Reset6809;
            const auto low = static_cast<uint8_t>(entry.value());
            const auto high = static_cast<uint8_t>(entry.value() >> 8);
            board_.poke(econopet::SramAddress{vector.value()}, little ? low : high);
            board_.poke(econopet::SramAddress{(vector + 1).value()}, little ? high : low);
        }
        // Start using the already installed vector without claiming execution completion.
        void start() requires (!std::is_const_v<Board>) { start_impl({}); }
        // Quiesce, install this core's reset vector and start execution.
        void start(econopet::CpuAddress entry) requires (!std::is_const_v<Board>) { start_impl(entry); }
        // Inspect this core's current bus address, not necessarily an opcode fetch.
        econopet::CpuAddress peek_address() const {
            switch (target()) {
            case CPU_PHYS_6502: return board_.stimulus_.cpu.address;
            case CPU_SOFT_6502: return econopet::CpuAddress{board_.dut_->soft6502_addr_o};
            case CPU_SOFT_6809: return econopet::CpuAddress{board_.dut_->soft6809_addr_o};
            case CPU_AUTO: break;
            }
            throw board_.template failure<std::logic_error>("CPU address", "unsupported CPU");
        }
        // Inspect opcode history for 6502, or native SYNC observations for 6809.
        bool peek_fetched_at(econopet::CpuAddress address, uint64_t checkpoint) const {
            switch (target()) {
            case CPU_SOFT_6502: return board_.soft_6502_fetches_[address.value()] > checkpoint;
            case CPU_SOFT_6809: return board_.soft_6809_fetches_[address.value()] > checkpoint;
            case CPU_PHYS_6502:
                throw board_.template failure<std::logic_error>("CPU fetch", "physical CPU fetch history is unsupported");
            case CPU_AUTO: break;
            }
            throw board_.template failure<std::logic_error>("CPU fetch", "unsupported CPU");
        }
        // Bypass lifecycle ordering for unusual raw control-sequence tests.
        void write_control_raw(uint8_t value) requires (!std::is_const_v<Board>) {
            board_.spi_write_at(fpga::address(fpga::Register::CpuControl), value);
        }
    private:
        // Share ordered reset/select/vector/hold/release steps between overloads.
        void start_impl(std::optional<econopet::CpuAddress> entry) {
            board_.require_mutable_access();
            const auto selected = target();
            if (entry && selected == CPU_PHYS_6502)
                throw board_.template failure<std::logic_error>("CPU start", "physical CPU vectors require explicit memory setup");
            prepare();
            if (entry) write_vector(*entry);
            board_.tick(ResetHoldCycles);
            release_reset();
        }
        // Require an explicit core for core-specific operations.
        cpu_type_t target() const {
            if (!target_) throw board_.template failure<std::logic_error>("CPU target", "operation requires cpu(selection)");
            return *target_;
        }
        Board& board_;
        std::optional<cpu_type_t> target_;
    };

    // Borrow active SPI and passive failure inspection from a persistent owner.
    SpiView<System> spi() & { return SpiView<System>(*this); }
    // Borrow only passive SPI inspection from a const owner.
    SpiView<const System> spi() const & { return SpiView<const System>(*this); }
    SpiView<System> spi() && = delete;
    SpiView<const System> spi() const && = delete;
    // Borrow shared lifecycle controls or bind them to a validated CPU.
    CpuView<System> cpu(std::optional<cpu_type_t> target = {}) & { return CpuView<System>(*this, target); }
    // Borrow passive CPU inspection from a const owner.
    CpuView<const System> cpu(std::optional<cpu_type_t> target = {}) const & {
        return CpuView<const System>(*this, target);
    }
    CpuView<System> cpu(std::optional<cpu_type_t> = {}) && = delete;
    CpuView<const System> cpu(std::optional<cpu_type_t> = {}) const && = delete;

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
        econopet::CpuAddress soft6502_addr_o, soft6809_addr_o;
        bool soft6502_fetch_o, soft6809_fetch_o;

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
              cpu_reset_active_o(p.cpu_reset_active_o), cpu_selection_o(p.cpu_selection_o),
              soft6502_addr_o(p.soft6502_addr_o), soft6809_addr_o(p.soft6809_addr_o),
              soft6502_fetch_o(p.soft6502_fetch_o), soft6809_fetch_o(p.soft6809_fetch_o) {}
    };

    // Initialize inactive pins and SRAM before settling the production top.
    System() : System(environment_seed()) {}

    // Seed the private context before model construction consumes randomness.
    explicit System(uint32_t seed, InitialState initial = InitialState::Zero)
        : context_(make_context(seed, initial)), dut_(std::make_unique<Vsystem>(context_.get())),
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
        remember("board initialized");
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
    // Report the actual context seed independently of test-runner shuffle seeds.
    uint32_t seed() const { return static_cast<uint32_t>(context_->randSeed()); }
    // Report the startup policy independently of board reset.
    InitialState initial_state() const {
        return context_->randReset() == RandomInitialState ? InitialState::Random : InitialState::Zero;
    }
    // Format bounded, read-only evidence even at an interrupted half-cycle.
    std::string diagnostic(const std::string& operation, const std::string& detail) const {
        std::ostringstream out;
        out << operation << ": " << detail << " [cycle=" << time().value()
            << ", half_tick=" << half_ticks() % econopet::HalfTicksPerCycle << ", cpu=";
        switch (dut_->cpu_selection_o) {
        case ECONOPET_CPU_SEL_PHYS_6502: out << "physical6502"; break;
        case ECONOPET_CPU_SEL_SOFT_6502: out << "soft6502"; break;
        case ECONOPET_CPU_SEL_SOFT_6809: out << "soft6809"; break;
        default: out << "invalid(" << unsigned(dut_->cpu_selection_o) << ')'; break;
        }
        out << ", seed=" << seed() << ", initial_state="
            << (initial_state() == InitialState::Random ? "random" : "zero")
            << ", reset=" << unsigned(dut_->cpu_reset_active_o)
            << ", clock_faulted=" << clock_faulted()
            << ", cpu_address=0x" << std::hex << dut_->cpu_addr_o
            << ", 6502_address=0x" << dut_->soft6502_addr_o
            << ", 6809_address=0x" << dut_->soft6809_addr_o
            << ", sram_address=0x" << ram_address().value()
            << ", spi_cs=" << unsigned(dut_->spi_cs_ni)
            << ", spi_stall=" << unsigned(dut_->spi_stall_o) << std::dec
            << ", spi_failed=" << spi_failed_ << ", last_spi_absolute_address=";
        if (last_spi_absolute_address_) out << "0x" << std::hex << *last_spi_absolute_address_ << std::dec;
        else out << "none";
        out << ", recent={";
        for (size_t index = 0; index < event_count_; ++index) {
            const auto& event = recent_[(event_next_ + recent_.size() - event_count_ + index) % recent_.size()];
            if (index) out << ", ";
            out << event.half_ticks / econopet::HalfTicksPerCycle;
            if (event.half_ticks % econopet::HalfTicksPerCycle) out << "+1/2";
            out << ':' << event.operation << "(0x" << std::hex << event.address
                << ",0x" << unsigned(event.data) << std::dec << ')';
        }
        return out.str() + "}]";
    }
    // Preserve exception categories while adding live board evidence.
    template<class Exception>
    Exception failure(const std::string& operation, const std::string& detail) const {
        return Exception(diagnostic(operation, detail));
    }
    // Sample each completed cycle with a scoped, observation-only callback.
    [[nodiscard]] ObserverSubscription observe(std::function<void(const System&)> observer) {
        require_mutable_access();
        if (!observer) throw failure<std::invalid_argument>("observe", "observer callback must not be empty");
        prune_observers();
        auto entry = std::make_shared<ObserverEntry>(ObserverEntry{std::move(observer)});
        observers_->entries.push_back(entry);
        return ObserverSubscription(observers_, std::move(entry));
    }
    // Poll a const board at the initial boundary and every cycle through the deadline.
    template<class Predicate>
    void run_until(Predicate done, econopet::Cycles budget, const std::string& reason) {
        service_until(std::move(done), [](System& board, econopet::Cycles) { board.tick(1); }, budget, reason);
    }
    // Alternate passive completion checks with active work under an inherited deadline.
    template<class Predicate, class Operation>
    void service_until(Predicate done, Operation service, econopet::Cycles budget, const std::string& reason) {
        static_assert(std::is_invocable_r_v<bool, Predicate&, const System&>,
                      "completion predicates must observe const System&");
        static_assert(std::is_invocable_v<Operation&, System&, econopet::Cycles>,
                      "active operations must accept System& and remaining Cycles");
        require_mutable_access();
        if (spi_failed_) throw failure<std::logic_error>("service_until", "deadline operation on an unusable SPI board");
        const auto at = deadline_ && budget > deadline_->at - time()
            ? deadline_->at : time() + budget;
        DeadlineScope deadline(*this, at, reason);
        remember("deadline begin");
        const auto observe_done = [&] {
            if (spi_failed_) throw failure<std::logic_error>("service_until", "deadline operation on an unusable SPI board");
            if (clock_faulted_)
                throw failure<std::logic_error>("service_until", "recover the suspended clock before polling");
            ObservationScope observation(*this);
            const bool complete = std::invoke(done, std::as_const(*this));
            observation.check_time();
            return complete;
        };
        for (;;) {
            const auto before = time();
            if (observe_done()) return;
            const auto remaining = deadline_->at - time();
            if (remaining == econopet::Cycles{0}) timeout(deadline_->reason);
            std::invoke(service, *this, remaining);
            if (time() == before) {
                if (observe_done()) return;
                tick(1);
            }
        }
    }
    // Read physical SRAM without going through the CPU or SPI bus.
    uint8_t peek(econopet::SramAddress address) const { return ram_[address.value()]; }
    // Patch physical SRAM for fixture setup without advancing the board.
    void poke(econopet::SramAddress address, uint8_t value) {
        require_mutable_access();
        ram_[address.value()] = value;
        remember("SRAM patch", address.value(), value);
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
        remember("external reset", 0, asserted);
    }
    // Drive independent interrupt sources that combine with fitted-device IRQs.
    void set_external_interrupts(bool irq_asserted, bool nmi_asserted) {
        require_mutable_access();
        stimulus_.cpu_irq_n_i = dut_->cpu_irq_n_i = !irq_asserted;
        stimulus_.cpu_nmi_n_i = dut_->cpu_nmi_n_i = !nmi_asserted;
        remember("external interrupts", 0, irq_asserted | (nmi_asserted << 1));
    }
    // Hold address/control and optional physical CPU write data between ticks.
    void drive_physical_cpu(const CpuStimulus& cpu) {
        require_mutable_access();
        stimulus_.cpu = cpu;
        apply_physical_cpu(cpu);
        remember("physical CPU", cpu.address.value(), cpu.write_data.value_or(0));
    }
    // Drive SPI0 boundary pins only (no transaction ownership or command policy).
    void drive_spi(bool cs_n, bool clock, bool data) {
        require_mutable_access();
        if (spi_active_ || spi_failed_)
            throw failure<std::logic_error>("drive_spi", "raw pins require an unowned, usable SPI bus");
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
        throw failure<std::invalid_argument>("display", "unsupported display configuration");
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
        throw failure<std::invalid_argument>("keyboard", "unsupported keyboard configuration");
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
        remember_stimulus_changes(copy);
        apply_raw_stimulus(copy);
    }

    // Advance complete system clocks, resolving external devices at both phases.
    void tick(econopet::Cycles cycles) {
        require_mutable_access();
        if (spi_failed_) throw failure<std::logic_error>("tick", "board is unusable after an unfinished SPI transaction");
        if (clock_faulted_ && stimulus_.cpu_reset_n_i)
            throw failure<std::logic_error>("tick", "board clock is suspended after failure: assert external reset before ticking");
        const auto remaining = cycles.half_ticks() - (cycles.value() != 0 && phase_ == ClockPhase::HighPending ? 1 : 0);
        if (remaining > std::numeric_limits<uint64_t>::max() - context_->time())
            throw failure<std::overflow_error>("tick", "board clock would overflow simulation time");
        if (deadline_ && cycles > deadline_->at - time()) timeout(deadline_->reason);
        struct TickScope {
            bool& faulted;
            bool completed = false;
            // Preserve the fault on exceptional exits without altering clock or device state.
            explicit TickScope(bool& state) : faulted(state) {}
            // Any interrupted tick requires explicit reset recovery.
            ~TickScope() { if (!completed) faulted = true; }
        };
        for (uint64_t index = 0; index < cycles.value(); ++index) {
            TickScope scope(clock_faulted_);
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
                remember("SRAM write", ram_write_address_.value(), ram_write_data_);
                ram_write_pending_ = false;
            }
            ram_we_was_low_ = !dut_->ram_we_n_o;
            context_->timeInc(1);
            phase_ = ClockPhase::LowPending;
            clock_faulted_ = false;
            scope.completed = true;
            record_cpu_cycle();
            notify_observers();
        }
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
    static constexpr int RandomInitialState = 2;
    static constexpr econopet::Cycles SpiHalfPeriodCycles{2};
    static constexpr econopet::Cycles SpiReleaseCycles{4};
    static constexpr econopet::Cycles SpiStallBudget{10000};
    static constexpr econopet::Cycles ResetHoldCycles{1280};

    // Preserve command validation categories and add read-only board context.
    void validate_spi_command(econopet::ByteView bytes) const {
        try {
            fpga::validate_command(bytes);
        } catch (const std::invalid_argument& error) {
            throw failure<std::invalid_argument>("SPI command", error.what());
        }
    }
    // Record validated absolute command intent without guessing the RTL pointer.
    void remember_spi_command(econopet::ByteView bytes) {
        const auto command = bytes[0] & ECONOPET_SPI_CMD_OPCODE_MASK;
        if (command == std::to_underlying(fpga::SpiCommand::ReadAt)
            || command == std::to_underlying(fpga::SpiCommand::WriteAt))
            last_spi_absolute_address_ = (uint32_t(bytes[0] & ECONOPET_SPI_CMD_ADDRESS_HIGH_MASK) << 16)
                | (uint32_t(bytes[1]) << 8) | bytes[2];
        remember("SPI command", last_spi_absolute_address_.value_or(0), bytes[0]);
    }
    // Reject failed boards, interrupted clocks and competing external or scoped owners.
    void require_spi_idle() const {
        require_mutable_access();
        if (spi_failed_ || clock_faulted_)
            throw failure<std::logic_error>("SPI begin", "SPI transaction requires a usable board");
        if (spi_active_ || !dut_->spi_cs_ni || dut_->spi_sck_i)
            throw failure<std::logic_error>("SPI begin", "SPI transaction requires an idle, unowned bus");
    }
    // Release ownership and retain the failure without clocks or callback execution.
    void fail_spi(const char* operation) noexcept {
        dut_->spi_cs_ni = 1;
        dut_->spi_sck_i = 0;
        spi_active_ = false;
        spi_failed_ = true;
        remember(operation);
    }
    // Await real controller readiness using the inclusive inherited deadline.
    void wait_ready(econopet::Cycles budget = SpiStallBudget) {
        if (spi_failed_) throw failure<std::logic_error>("wait_ready", "SPI wait on an unusable board");
        run_until([](const System& board) { return !board.snapshot().spi_stall_o; },
            budget, "FPGA SPI controller readiness");
    }
    // Shift one mode-0 bit using complete clock slices, never shortening a deadline phase.
    bool exchange_spi_bit(bool value, econopet::Cycles half_period) {
        require_mutable_access();
        dut_->spi_sdo_i = value;
        dut_->spi_sck_i = 0;
        tick(half_period);
        dut_->spi_sck_i = 1;
        tick(half_period);
        return dut_->spi_sdi_o;
    }
    // Clock a complete byte, including its final falling SCK edge.
    uint8_t exchange_spi_byte(uint8_t value, econopet::Cycles half_period) {
        uint8_t received = 0;
        for (unsigned bit = 0; bit < SpiByteBits; ++bit) {
            received = static_cast<uint8_t>((received << 1) | exchange_spi_bit((value & SpiTopBit) != 0, half_period));
            value <<= 1;
        }
        dut_->spi_sck_i = 0;
        tick(half_period);
        return received;
    }
    // Acquire a normal checked multi-command scope.
    SpiTransaction spi_transaction() { return SpiTransaction(*this, false, SpiHalfPeriodCycles); }
    // Validate before acquiring ownership and return the first received command byte.
    uint8_t spi_command(econopet::ByteView bytes) {
        require_spi_idle();
        validate_spi_command(bytes);
        auto owner = spi_transaction();
        const auto received = owner.command(bytes);
        owner.finish();
        return received;
    }
    // Seek then clock out the pipelined response using the production protocol.
    uint8_t spi_read_at(econopet::WishboneAddress address) {
        require_spi_idle();
        remember("SPI read FPGA", address.value());
        const std::array<uint8_t, 3> seek{
            fpga::command_byte(fpga::SpiCommand::ReadAt, address),
            static_cast<uint8_t>(address.value() >> 8), static_cast<uint8_t>(address.value())};
        spi_command(seek);
        const std::array<uint8_t, 1> next{fpga::command_byte(fpga::SpiCommand::ReadNext)};
        return spi_command(next);
    }
    // Write a complete absolute command with full address-width preservation.
    void spi_write_at(econopet::WishboneAddress address, uint8_t value) {
        require_spi_idle();
        remember("SPI write FPGA", address.value(), value);
        const std::array<uint8_t, 4> bytes{
            fpga::command_byte(fpga::SpiCommand::WriteAt, address),
            static_cast<uint8_t>(address.value() >> 8), static_cast<uint8_t>(address.value()), value};
        spi_command(bytes);
    }
    // Validate production CPU choices before a bus operation.
    void validate_cpu(cpu_type_t cpu) const {
        switch (cpu) {
        case CPU_PHYS_6502: case CPU_SOFT_6502: case CPU_SOFT_6809: return;
        case CPU_AUTO: break;
        }
        throw failure<std::invalid_argument>("select_cpu", "unsupported CPU selection");
    }
    // Decode hardware bits without constructing an invalid production enum.
    cpu_type_t decode_cpu(uint8_t value) const {
        constexpr uint8_t SelectionMask = ECONOPET_CPU_SEL_SOFT_6502 | ECONOPET_CPU_SEL_SOFT_6809;
        switch (value & SelectionMask) {
        case ECONOPET_CPU_SEL_PHYS_6502: return CPU_PHYS_6502;
        case ECONOPET_CPU_SEL_SOFT_6502: return CPU_SOFT_6502;
        case ECONOPET_CPU_SEL_SOFT_6809: return CPU_SOFT_6809;
        }
        throw failure<std::runtime_error>("selected_cpu", "CPU selection register contains an unsupported value");
    }
    // Read defined lifecycle bits through the real bus (upper register bits are unspecified).
    econopet::CpuControl read_cpu_control() {
        return econopet::CpuControl::from_bits(
            spi_read_at(fpga::address(fpga::Register::CpuControl)) & ECONOPET_REG_CPU_MASK);
    }
    // Read and validate the production selection register.
    cpu_type_t read_cpu_selection() {
        return decode_cpu(spi_read_at(fpga::address(fpga::Register::CpuSelect)));
    }
    // Assert reset and drain admitted activity while preserving unrelated control bits.
    void assert_reset() {
        require_mutable_access();
        remember("assert_reset");
        const auto control = read_cpu_control() | econopet::CpuControlBit::Reset;
        spi_write_at(fpga::address(fpga::Register::CpuControl), control.bits());
        tick(ResetHoldCycles);
        if (!read_cpu_control().contains(econopet::CpuControlBit::Reset) || !dut_->cpu_reset_active_o
            || ram_write_pending_)
            throw failure<std::runtime_error>("assert_reset", "CPU reset did not establish quiescence");
    }
    // Select a core only after reset has stopped admitted bus activity.
    void select_cpu(cpu_type_t cpu) {
        require_mutable_access();
        validate_cpu(cpu);
        if (!read_cpu_control().contains(econopet::CpuControlBit::Reset) || !dut_->cpu_reset_active_o
            || ram_write_pending_)
            throw failure<std::logic_error>("select_cpu", "CPU selection requires quiescent asserted reset");
        const auto selection = static_cast<uint8_t>(static_cast<uint8_t>(cpu)
            | (spi_read_at(fpga::address(fpga::Register::CpuSelect)) & ECONOPET_CPU_SEL_SUPERPET_IO_MASK));
        spi_write_at(fpga::address(fpga::Register::CpuSelect), selection);
        if (read_cpu_selection() != cpu)
            throw failure<std::runtime_error>("select_cpu", "CPU selection did not take effect");
    }
    // Release shared reset with the selected core's READY policy and verify it.
    void release_reset() {
        require_mutable_access();
        remember("release_reset");
        const auto selection = read_cpu_selection();
        const econopet::CpuControl control = selection != CPU_SOFT_6809
            ? econopet::CpuControl{econopet::CpuControlBit::Ready} : econopet::CpuControl{};
        spi_write_at(fpga::address(fpga::Register::CpuControl), control.bits());
        if (read_cpu_control() != control || dut_->cpu_reset_active_o)
            throw failure<std::runtime_error>("release_reset", "CPU reset did not release with the required READY state");
    }
    // Edge-detect native CPU observations without changing RTL or clearing history.
    void record_cpu_cycle() {
        if (dut_->soft6502_fetch_o && !soft_6502_fetch_active_) {
            soft_6502_fetches_[dut_->soft6502_fetch_addr_o] = ++fetch_sequence_;
            remember("6502 fetch", dut_->soft6502_fetch_addr_o);
        }
        if (dut_->soft6809_fetch_o && !soft_6809_fetch_active_) {
            soft_6809_fetches_[dut_->soft6809_addr_o] = ++fetch_sequence_;
            remember("6809 SYNC", dut_->soft6809_addr_o);
        }
        soft_6502_fetch_active_ = dut_->soft6502_fetch_o;
        soft_6809_fetch_active_ = dut_->soft6809_fetch_o;
    }
    static constexpr unsigned SpiByteBits = 8;
    static constexpr uint8_t SpiTopBit = 0x80;

    // Initialize each model independently without modifying global Verilator RNGs.
    static std::unique_ptr<VerilatedContext> make_context(uint32_t seed, InitialState initial) {
        if (seed == 0 || seed > static_cast<uint32_t>(std::numeric_limits<int>::max()))
            throw std::invalid_argument("System initialization: seed must be in [1, INT_MAX]");
        if (initial != InitialState::Zero && initial != InitialState::Random)
            throw std::invalid_argument("System initialization: unsupported initial-state policy");
        auto context = std::make_unique<VerilatedContext>();
        context->randSeed(static_cast<int>(seed));
        context->randReset(initial == InitialState::Random ? RandomInitialState : ZeroInitialState);
        return context;
    }
    struct Deadline {
        econopet::CycleTime at;
        std::string reason;
    };
    // Install the tighter deadline and restore its enclosing scope on every exit.
    class DeadlineScope {
    public:
        // Nested waits cannot extend the active operation's endpoint.
        DeadlineScope(System& board, econopet::CycleTime at, const std::string& reason)
            : board_(board), saved_(board.deadline_) {
            if (!board_.deadline_ || at < board_.deadline_->at)
                board_.deadline_ = Deadline{at, reason};
        }
        // Preserve the outer deadline after success, timeout or callback failure.
        ~DeadlineScope() { board_.deadline_ = std::move(saved_); }
        DeadlineScope(const DeadlineScope&) = delete;
        DeadlineScope& operator=(const DeadlineScope&) = delete;
    private:
        System& board_;
        std::optional<Deadline> saved_;
    };
    // Protect predicates and per-cycle observers from captured mutable references.
    class ObservationScope {
    public:
        // Remember exact simulator time, including sub-cycle advancement.
        explicit ObservationScope(System& board)
            : board_(board), start_(board.half_ticks()), saved_(board.observing_) {
            board_.observing_ = true;
        }
        // Restore access even when a callback throws.
        ~ObservationScope() { board_.observing_ = saved_; }
        ObservationScope(const ObservationScope&) = delete;
        ObservationScope& operator=(const ObservationScope&) = delete;
        // Diagnose advancement that bypassed ordinary clock-entry checks.
        void check_time() const {
            if (board_.half_ticks() != start_)
                throw board_.failure<std::logic_error>("observation", "callback advanced simulation time");
        }
    private:
        System& board_;
        uint64_t start_;
        bool saved_;
    };
    // Prevent pin-edit callbacks from mutating or advancing their fitted board.
    void require_mutable_access() const {
        if (observing_)
            throw failure<std::logic_error>("observation", "callback cannot mutate or advance the board");
        if (editing_stimulus_ || io_.editing_inputs())
            throw failure<std::logic_error>("stimulus edit", "callback cannot mutate or advance the board");
    }
    // Remove expired registrations without disturbing callback order.
    void prune_observers() {
        std::erase_if(observers_->entries, [](const auto& entry) { return entry.expired(); });
    }
    // Dispatch after completed I/O, SRAM and time updates, stopping on exceptions.
    void notify_observers() {
        if (observers_->entries.empty()) return;
        prune_observers();
        ObservationScope observation(*this);
        for (const auto& registration : observers_->entries) {
            const auto entry = registration.lock();
            if (!entry) continue;
            entry->callback(std::as_const(*this));
            observation.check_time();
        }
    }
    // Report the inclusive deadline without advancing the board.
    [[noreturn]] void timeout(const std::string& reason) const {
        throw failure<std::runtime_error>("deadline", reason + " timed out");
    }
    struct RecentEvent {
        uint64_t half_ticks = 0;
        const char* operation = "";
        uint32_t address = 0;
        uint8_t data = 0;
    };
    // Retain a bounded chronological ring without clocks or callback dispatch.
    void remember(const char* operation, uint32_t address = 0, uint8_t data = 0) {
        recent_[event_next_] = {half_ticks(), operation, address, data};
        event_next_ = (event_next_ + 1) % recent_.size();
        event_count_ = std::min(event_count_ + 1, recent_.size());
    }
    // Record only successfully committed changes, preserving the requested pin values.
    void remember_stimulus_changes(const RawStimulus& next) {
        if (next.cpu.address != stimulus_.cpu.address)
            remember("CPU address", next.cpu.address.value());
        if (next.cpu.write_data.has_value() != stimulus_.cpu.write_data.has_value())
            remember("CPU write enable", 0, next.cpu.write_data.has_value());
        if (next.cpu.write_data != stimulus_.cpu.write_data && next.cpu.write_data)
            remember("CPU write data", 0, *next.cpu.write_data);
        if (next.cpu.sync != stimulus_.cpu.sync)
            remember("CPU sync", 0, next.cpu.sync);
        static constexpr std::pair<const char*, bool RawStimulus::*> boolean_fields[]{
            {"cpu_reset_n_i", &RawStimulus::cpu_reset_n_i},
            {"cpu_irq_n_i", &RawStimulus::cpu_irq_n_i},
            {"cpu_nmi_n_i", &RawStimulus::cpu_nmi_n_i},
            {"diag_i", &RawStimulus::diag_i},
            {"audio_det_i", &RawStimulus::audio_det_i},
            {"config_hz_i", &RawStimulus::config_hz_i},
            {"spi1_cs_ni", &RawStimulus::spi1_cs_ni},
            {"spi1_sck_i", &RawStimulus::spi1_sck_i},
            {"spi1_sd_i", &RawStimulus::spi1_sd_i},
            {"spi1_sdo_i", &RawStimulus::spi1_sdo_i},
            {"i2c0_scl_i", &RawStimulus::i2c0_scl_i},
            {"i2c0_sda_i", &RawStimulus::i2c0_sda_i},
            {"i2c1_scl_i", &RawStimulus::i2c1_scl_i},
            {"i2c1_sda_i", &RawStimulus::i2c1_sda_i},
            {"mcu_cec_i", &RawStimulus::mcu_cec_i},
        };
        for (const auto& [name, member] : boolean_fields) {
            if (next.*member != stimulus_.*member)
                remember(name, 0, next.*member);
        }
        if (next.pmod1_i != stimulus_.pmod1_i) remember("pmod1_i", 0, next.pmod1_i);
        if (next.pmod2_i != stimulus_.pmod2_i) remember("pmod2_i", 0, next.pmod2_i);
        for (size_t index = 0; index < next.spare.size(); ++index) {
            if (next.spare[index] != stimulus_.spare[index])
                remember("spare", static_cast<uint32_t>(index), next.spare[index]);
        }
    }
    // Apply CPU pin levels without recording a public fixture operation.
    void apply_physical_cpu(const CpuStimulus& cpu) {
        dut_->cpu_addr_i = cpu.address.value();
        dut_->cpu_we_n_i = !cpu.write_data.has_value();
        dut_->cpu_sync_i = cpu.sync;
    }
    // Apply fixture pins without evaluating the model or changing time.
    void apply_raw_stimulus(const RawStimulus& stimulus) {
        require_mutable_access();
        stimulus_ = stimulus;
        apply_physical_cpu(stimulus.cpu);
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
                throw failure<std::runtime_error>("bus read", "physical CPU and FPGA both drive the data bus");
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
            try {
                return io_.via().peek(static_cast<econopet::io::ViaRegister>(reg));
            } catch (const std::logic_error& error) {
                throw failure<std::logic_error>("I/O read", error.what());
            }
        }
        throw failure<std::runtime_error>("I/O read", "I/O output enable asserted without a peripheral select");
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
        try {
            io_.sample({dut_->cpu_clock_o != 0, dut_->cpu_reset_active_o != 0, selects,
                static_cast<uint8_t>(dut_->cpu_addr_o & econopet::io::ViaRegisterMask),
                write_bus_data(), dut_->cpu_we_n_o == 0, dut_->pia1_clock_o != 0}, completed);
        } catch (const std::logic_error& error) {
            throw failure<std::logic_error>("I/O sample", error.what());
        }
        dut_->io_irq_ni = !io_.irq();
        dut_->graphic_i = io_.via().ca2();
        dut_->via_cb2_i = io_.via().cb2();
    }

    std::array<RecentEvent, 16> recent_{};
    size_t event_next_ = 0;
    size_t event_count_ = 0;
    bool observing_ = false;
    std::optional<Deadline> deadline_;
    std::shared_ptr<ObserverRegistry> observers_ = std::make_shared<ObserverRegistry>();
    bool spi_active_ = false;
    bool spi_failed_ = false;
    std::optional<uint32_t> last_spi_absolute_address_;
    std::array<uint64_t, econopet::CpuAddressCapacity> soft_6502_fetches_{}, soft_6809_fetches_{};
    uint64_t fetch_sequence_ = 0;
    bool soft_6502_fetch_active_ = false;
    bool soft_6809_fetch_active_ = false;
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

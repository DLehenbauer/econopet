// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>

#include "m6520.h"
#include "m6522.h"

#include "types.h"

namespace econopet::io {

// Board select encoding shared with the system bus adapter.
enum class ChipSelect : uint8_t {
    None = 0,
    Pia1 = 1,
    Pia2 = 2,
    Via = 4,
    All = Pia1 | Pia2 | Via,
};
using ChipSelects = econopet::Flags<ChipSelect>;
using econopet::operator|;

enum class PiaRegister : uint8_t {
    PortA = M6520_REG_RA,
    ControlA = M6520_REG_CRA,
    PortB = M6520_REG_RB,
    ControlB = M6520_REG_CRB,
};
enum class ViaRegister : uint8_t {
    PortB = M6522_REG_RB,
    PortA = M6522_REG_RA,
    DdrB = M6522_REG_DDRB,
    DdrA = M6522_REG_DDRA,
    Timer1Low = M6522_REG_T1CL,
    Timer1High = M6522_REG_T1CH,
    Timer1LatchLow = M6522_REG_T1LL,
    Timer1LatchHigh = M6522_REG_T1LH,
    Timer2Low = M6522_REG_T2CL,
    Timer2High = M6522_REG_T2CH,
    Shift = M6522_REG_SR,
    Acr = M6522_REG_ACR,
    Pcr = M6522_REG_PCR,
    Ifr = M6522_REG_IFR,
    Ier = M6522_REG_IER,
    PortANoHandshake = M6522_REG_RA_NOH,
};

enum class PiaControlBit : uint8_t {
    C1IrqEnable = M6520_CR_C1_ENABLE_IRQ,
    C1PositiveEdge = M6520_CR_C1_POS_EDGE,
    PortAccess = M6520_CR_PORT_SELECT,
    C2Mode = M6520_CR_C2_MODE,
    C2PositiveEdge = M6520_CR_C2_EDGE,
    C2Output = M6520_CR_C2_OUTPUT,
    Irq2 = M6520_CR_IRQ2,
    Irq1 = M6520_CR_IRQ1,
    All = C1IrqEnable | C1PositiveEdge | PortAccess | C2Mode
        | C2PositiveEdge | C2Output | Irq2 | Irq1,
};
enum class ViaInterruptBit : uint8_t {
    Ca2 = M6522_IRQ_CA2,
    Ca1 = M6522_IRQ_CA1,
    Shift = M6522_IRQ_SR,
    Cb2 = M6522_IRQ_CB2,
    Cb1 = M6522_IRQ_CB1,
    Timer2 = M6522_IRQ_T2,
    Timer1 = M6522_IRQ_T1,
    Summary = M6522_IRQ_ANY,
    All = Ca2 | Ca1 | Shift | Cb2 | Cb1 | Timer2 | Timer1 | Summary,
};
enum class ViaAcrBit : uint8_t {
    LatchA = 0x01,
    LatchB = 0x02,
    ShiftBit0 = 0x04,
    ShiftBit1 = 0x08,
    ShiftBit2 = 0x10,
    Timer2CountPb6 = 0x20,
    Timer1Continuous = 0x40,
    Timer1Pb7 = 0x80,
    All = LatchA | LatchB | ShiftBit0 | ShiftBit1 | ShiftBit2
        | Timer2CountPb6 | Timer1Continuous | Timer1Pb7,
};
using PiaControl = econopet::Flags<PiaControlBit>;
using ViaInterrupts = econopet::Flags<ViaInterruptBit>;
using ViaAcr = econopet::Flags<ViaAcrBit>;

// Register decoding masks shared by device adapters and the system bus.
inline constexpr uint8_t PiaRegisterMask = M6520_REG_CRB;
inline constexpr uint8_t ViaRegisterMask = M6522_REG_RA_NOH;

// Digital pull-up levels and the VIA timer-two external counting input.
inline constexpr uint8_t PortAllHigh = 0xff;
inline constexpr uint8_t ViaPb6 = 0x40;

// PIA control values select DDR/port access and the modeled C2 output modes.
inline constexpr PiaControl PiaDdrAccess{};
inline constexpr PiaControl PiaPortAccess{PiaControlBit::PortAccess};
inline constexpr PiaControl PiaC2Handshake = PiaControlBit::C2Output | PiaControlBit::PortAccess;
inline constexpr PiaControl PiaC2Pulse = PiaC2Handshake | PiaControlBit::C2Mode;
inline constexpr PiaControl PiaC2Low = PiaC2Handshake | PiaControlBit::C2PositiveEdge;
inline constexpr PiaControl PiaC2High = PiaC2Low | PiaControlBit::C2Mode;

// Digital input levels only (no analog loading or propagation-delay model).
struct Inputs {
    uint8_t port_a = PortAllHigh;
    uint8_t port_b = PortAllHigh;
    bool ca1 = true;
    bool ca2 = true;
    bool cb1 = true;
    bool cb2 = true;
};

class Pia6520;
class Via6522;

namespace detail {
// Owned mutation guard and edit state, borrowed by fitted devices.
struct InputEditState {
    std::function<void()> guard;
    bool active = false;
};

// Edit detached digital levels and commit only after successful callback return.
class InputLevels {
public:
    // Own an inline edit context for a standalone device.
    InputLevels() : owned_edits_(std::in_place), edits_(*owned_edits_) {}
    // Preserve the original context binding and callback edit ownership.
    InputLevels(const InputLevels&) = delete;
    InputLevels& operator=(const InputLevels&) = delete;
    InputLevels(InputLevels&&) = delete;
    InputLevels& operator=(InputLevels&&) = delete;
    // Return a detached observation, never a writable reference into the device.
    Inputs peek_inputs() const { return inputs_; }
    // All representable levels are valid. Exceptions discard the complete edit.
    template<class Edit>
    void inputs(Edit&& edit) {
        require_mutable_device();
        auto copy = inputs_;
        {
            struct Scope {
                bool& active;
                explicit Scope(bool& value) : active(value) { active = true; }
                ~Scope() { active = false; }
            } scope(edits_.active);
            std::invoke(std::forward<Edit>(edit), copy);
        }
        if (edits_.guard) edits_.guard();
        inputs_ = copy;
    }
private:
    friend class econopet::io::Pia6520;
    friend class econopet::io::Via6522;
    // Borrow the fitted device's final context without constructing a local context.
    explicit InputLevels(InputEditState& edits) : edits_(edits) {}
    // Retained model references obey the board guard and shared input edit owner.
    void require_mutable_device() const {
        if (edits_.guard) edits_.guard();
        if (edits_.active) throw std::logic_error("device inputs: mutation during editing is not allowed");
    }
    Inputs inputs_;
    std::optional<InputEditState> owned_edits_;
    InputEditState& edits_;
};
} // namespace detail

// One completed PHI2 access, timestamped at its containing full-cycle boundary.
template<class Register>
struct Access {
    Register reg;
    uint8_t data;
    bool write;
    CycleTime at;
};

// Complete one raw register write and peripheral clock at the caller's timestamp.
template<class Device>
void write(Device& device, typename Device::Register reg, uint8_t data, CycleTime at) {
    device.clock(typename Device::BusAccess{reg, data, true, at});
}

// Return pre-acknowledgment data and complete one read/peripheral clock at the timestamp.
template<class Device>
uint8_t read(Device& device, typename Device::Register reg, CycleTime at) {
    // Capture presented data before the completed access acknowledges flags or handshakes.
    const uint8_t data = device.peek(reg);
    // Clock exactly once, retaining the caller's full-cycle timestamp.
    device.clock(typename Device::BusAccess{reg, 0, false, at});
    return data;
}

// One completed write at a full-cycle observation boundary.
template<class Register>
struct WriteRecord {
    Register reg;
    uint8_t data;
    CycleTime at;
};

// Observations survive chip reset and can be cleared independently.
template<class Register>
class Writes {
public:
    // Return completed writes, including repeated values, for bus assertions.
    uint64_t count() const { return count_; }
    // Return the latest timestamped write, or none since construction/clearing.
    const std::optional<WriteRecord<Register>>& last() const { return last_; }
    // Discard history without changing peripheral state.
    void clear() { count_ = 0; last_.reset(); }
    // Observe a completed access, ignoring reads.
    void record(const Access<Register>& access) {
        if (access.write) {
            ++count_;
            last_ = WriteRecord<Register>{access.reg, access.data, access.at};
        }
    }

private:
    uint64_t count_ = 0;
    std::optional<WriteRecord<Register>> last_;
};

// MOS 6520: DDR/latch isolation, mixed GPIO, edge IRQs and all C2 modes.
// Each clock() is one PHI2 falling edge. peek() never clears IRQs or handshakes.
class Pia6520 {
public:
    using Register = PiaRegister;
    using BusAccess = Access<Register>;
    // Initialize the chip with independent external inputs and write history.
    Pia6520();
    // Device identity and its guard binding cannot be copied or replaced.
    Pia6520(const Pia6520&) = delete;
    Pia6520& operator=(const Pia6520&) = delete;
    Pia6520(Pia6520&&) = delete;
    Pia6520& operator=(Pia6520&&) = delete;
    // Observe detached digital levels without exposing the edit context.
    Inputs peek_inputs() const { return inputs_.peek_inputs(); }
    // Commit detached level changes only after the callback succeeds.
    template<class Edit>
    void inputs(Edit&& edit) { inputs_.inputs(std::forward<Edit>(edit)); }
    // Expose modeled registers for device and bus assertions.
    const m6520_t& state() const { return chip_; }
    // Inspect completed writes independently of chip reset.
    const Writes<Register>& writes() const { return writes_; }
    // Clear write history while preserving inputs and chip state.
    void clear_observations() { inputs_.require_mutable_device(); writes_.clear(); }
    // Reset registers without changing inputs or write history.
    void reset();
    // Tick one falling PHI2 edge, optionally completing a selected access.
    void clock(std::optional<BusAccess> access = std::nullopt);
    // Read a register without acknowledging IRQs or triggering handshakes.
    uint8_t peek(Register reg) const;
    // Complete a checked control-register write at the caller's cycle time.
    void set_control(Register reg, PiaControl flags, CycleTime at);
    // Report the chip's active interrupt output to board wiring.
    bool irq() const;
    // Resolve CA2 from output mode or the externally driven input.
    bool ca2() const;
    // Resolve CB2 from output mode or the externally driven input.
    bool cb2() const;

private:
    // Construct a fitted PIA with its owner's final mutation context.
    explicit Pia6520(detail::InputEditState& edits);
    // Encode driven inputs for both ticking and side-effect-free peeking.
    uint64_t input_pins() const;
    // Reject invalid register indices before touching state or history.
    static void validate(Register reg);
    friend class Io;
    detail::InputLevels inputs_;
    m6520_t chip_;
    Writes<Register> writes_;
};

// MOS 6522: unlatched mixed GPIO, timed T1 one-shot/free-run, timed/PB6-counted
// T2, IFR/IER, control-input IRQs and fixed/handshake C2 outputs.
// Shift registers, port-input latching, PB7 timer output and C2 pulse outputs
// are rejected explicitly. Reset changes registers, not inputs/faults/history.
class Via6522 {
public:
    using Register = ViaRegister;
    using BusAccess = Access<Register>;
    enum class Timer1Fault : uint8_t {
        None,
        StuckInterruptFlag,
    };

    // Initialize the chip with independent inputs, fault state and history.
    Via6522();
    // Device identity and its guard binding cannot be copied or replaced.
    Via6522(const Via6522&) = delete;
    Via6522& operator=(const Via6522&) = delete;
    Via6522(Via6522&&) = delete;
    Via6522& operator=(Via6522&&) = delete;
    // Observe detached digital levels without exposing the edit context.
    Inputs peek_inputs() const { return inputs_.peek_inputs(); }
    // Commit detached level changes only after the callback succeeds.
    template<class Edit>
    void inputs(Edit&& edit) { inputs_.inputs(std::forward<Edit>(edit)); }
    // Expose modeled registers, excluding the synthetic fault overlay.
    const m6522_t& state() const { return chip_; }
    // Inspect completed writes independently of chip reset.
    const Writes<Register>& writes() const { return writes_; }
    // Clear write history while preserving inputs, faults and chip state.
    void clear_observations() { inputs_.require_mutable_device(); writes_.clear(); }
    // Reset registers without changing inputs, faults or write history.
    void reset();
    // Tick one falling PHI2 edge, optionally completing a selected access.
    void clock(std::optional<BusAccess> access = std::nullopt);
    // Read a register with fault overlay but no acknowledgment or handshake.
    uint8_t peek(Register reg) const;
    // Inspect normal and injected IFR sources as VIA-domain flags.
    ViaInterrupts interrupts() const { return ViaInterrupts::from_bits(interrupt_flags()); }
    // Configure ACR flags at the caller's cycle time, rejecting unsupported modes.
    void set_acr(ViaAcr flags, CycleTime at) { write(*this, Register::Acr, flags.bits(), at); }
    // Set named IER sources at the caller's cycle time.
    void set_interrupts(ViaInterrupts sources, bool enabled, CycleTime at);
    // Report enabled normal or injected interrupts to board wiring.
    bool irq() const;
    // Resolve CA2 from output mode or the externally driven input.
    bool ca2() const;
    // Resolve CB2 from output mode or the externally driven input.
    bool cb2() const;

    // Explicit adversarial stuck-flag injection, independent of normal timers.
    void set_timer1_fault(Timer1Fault fault);
    // Inspect the injected fault, which survives register reset.
    Timer1Fault timer1_fault() const { return timer1_fault_; }

private:
    // Construct a fitted VIA with its owner's final mutation context.
    explicit Via6522(detail::InputEditState& edits);
    // Encode driven inputs for ticking and side-effect-free peeking.
    uint64_t input_pins() const;
    // Apply fault injection and recompute the enabled-interrupt summary.
    uint8_t interrupt_flags() const;
    // Reject unsupported accesses before changing registers or observations.
    static void validate(const BusAccess& access);
    friend class Io;
    detail::InputLevels inputs_;
    m6522_t chip_;
    Writes<Register> writes_;
    Timer1Fault timer1_fault_ = Timer1Fault::None;
};

// Physical chip selects at the FPGA boundary (bit 0: PIA1, 1: PIA2, 2: VIA).
struct BusSample {
    bool phi2;
    bool reset;
    ChipSelects selects;
    uint8_t reg;
    uint8_t data;
    bool write;
    // Default to CPU PHI2, or supply the board's isolated PIA1 clock.
    std::optional<bool> pia1_phi2 = std::nullopt;
};

// Latches the stable bus just before falling PHI2, exactly once per cycle.
// Deselected peripherals still clock their timers and control-input detectors.
class Io {
public:
    // Own the common context inline and construct fitted devices with it directly.
    explicit Io(std::function<void()> guard = {})
        : edits_{std::move(guard)}, pia1_(edits_), pia2_(edits_), via_(edits_) {}
    // Fitted devices retain stable identities and borrow this owner's context.
    Io(const Io&) = delete;
    Io& operator=(const Io&) = delete;
    Io(Io&&) = delete;
    Io& operator=(Io&&) = delete;
    // Let the board reject mutation/clocking while a detached edit is in progress.
    bool editing_inputs() const { return edits_.active; }
    // Access PIA1 for board wiring and fault-free device setup.
    Pia6520& pia1() { return pia1_; }
    // Inspect PIA1 from read-only board observations.
    const Pia6520& pia1() const { return pia1_; }
    // Access PIA2 for board wiring and device setup.
    Pia6520& pia2() { return pia2_; }
    // Inspect PIA2 from read-only board observations.
    const Pia6520& pia2() const { return pia2_; }
    // Access the VIA for board wiring, setup and fault injection.
    Via6522& via() { return via_; }
    // Inspect the VIA from read-only board observations.
    const Via6522& via() const { return via_; }
    // Timestamp completed writes at the containing full-cycle boundary.
    void sample(const BusSample& bus, CycleTime at);
    // Combine physical device IRQs for the FPGA input.
    bool irq() const { return pia1_.irq() || pia2_.irq() || via_.irq(); }
    // Return completed physical PHI2 cycles for timer timing assertions.
    uint64_t clock_count() const { return clocks_; }
    // Clear write/cycle observations without changing devices or bus history.
    void clear_observations();

private:
    detail::InputEditState edits_;
    Pia6520 pia1_;
    Pia6520 pia2_;
    Via6522 via_;
    BusSample previous_ = {};
    uint64_t clocks_ = 0;
};

} // namespace econopet::io

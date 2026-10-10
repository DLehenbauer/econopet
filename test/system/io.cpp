// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#define CHIPS_IMPL
#include "io.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace econopet::io {
namespace {

constexpr uint8_t ViaAcrUnsupportedMask = (ViaAcrBit::LatchA | ViaAcrBit::LatchB
    | ViaAcrBit::ShiftBit0 | ViaAcrBit::ShiftBit1 | ViaAcrBit::ShiftBit2
    | ViaAcrBit::Timer1Pb7).bits();

// IFR/IER bit 7 is summary/enable selection, not a source interrupt.
constexpr uint8_t ViaIerSet = std::to_underlying(ViaInterruptBit::Summary);
constexpr uint8_t ViaIrqSources = std::to_underlying(ViaInterruptBit::All) & ~ViaIerSet;

static_assert(M6520_CA1 == M6522_CA1, "pins_for requires matching PIA/VIA CA1 encodings");
static_assert(M6520_CA2 == M6522_CA2, "pins_for requires matching PIA/VIA CA2 encodings");
static_assert(M6520_CB1 == M6522_CB1, "pins_for requires matching PIA/VIA CB1 encodings");
static_assert(M6520_CB2 == M6522_CB2, "pins_for requires matching PIA/VIA CB2 encodings");
static_assert(M6520_PA0 == M6522_PA0 && M6520_PA1 == M6522_PA1
    && M6520_PA2 == M6522_PA2 && M6520_PA3 == M6522_PA3
    && M6520_PA4 == M6522_PA4 && M6520_PA5 == M6522_PA5
    && M6520_PA6 == M6522_PA6 && M6520_PA7 == M6522_PA7,
    "pins_for requires matching PIA/VIA port A encodings");
static_assert(M6520_PB0 == M6522_PB0 && M6520_PB1 == M6522_PB1
    && M6520_PB2 == M6522_PB2 && M6520_PB3 == M6522_PB3
    && M6520_PB4 == M6522_PB4 && M6520_PB5 == M6522_PB5
    && M6520_PB6 == M6522_PB6 && M6520_PB7 == M6522_PB7,
    "pins_for requires matching PIA/VIA port B encodings");

// Encode external levels for both adapters (their port/control pins coincide).
uint64_t pins_for(const Inputs& inputs) {
    // Present the independently driven control levels.
    uint64_t pins = (inputs.ca1 ? M6520_CA1 : 0)
        | (inputs.ca2 ? M6520_CA2 : 0)
        | (inputs.cb1 ? M6520_CB1 : 0)
        | (inputs.cb2 ? M6520_CB2 : 0);
    // Merge the two byte-wide GPIO input ports.
    M6520_SET_PAB(pins, inputs.port_a, inputs.port_b);
    return pins;
}

} // namespace

// Initialize the underlying PIA model.
Pia6520::Pia6520() { m6520_init(&chip_); }
// Initialize a fitted PIA with the owner's final context, without rebinding.
Pia6520::Pia6520(detail::InputEditState& edits) : inputs_(edits) { m6520_init(&chip_); }
// Reset chip registers while retaining adapter inputs and observations.
void Pia6520::reset() { inputs_.require_mutable_device(); m6520_reset(&chip_); }
// Present current driven levels to the chip model.
uint64_t Pia6520::input_pins() const { return pins_for(inputs_.peek_inputs()); }

// Guard the chip model against out-of-range register decoding.
void Pia6520::validate(Register reg) {
    if (std::to_underlying(reg) > PiaRegisterMask) throw std::out_of_range("6520 register must be in [0, 3]");
}

// Complete an optional access at one PHI2 falling edge.
void Pia6520::clock(std::optional<BusAccess> access) {
    inputs_.require_mutable_device();
    // Validate and present the selected bus access with external inputs.
    uint64_t pins = input_pins();
    if (access) {
        validate(access->reg);
        pins |= M6520_CS | std::to_underlying(access->reg) | (access->write ? 0 : M6520_RW);
        M6520_SET_DATA(pins, access->data);
    }
    // Advance the model before recording a successfully completed write.
    m6520_tick(&chip_, pins);
    if (access) writes_.record(*access);
}

// Inspect a register without read acknowledgments or handshake side effects.
uint8_t Pia6520::peek(Register reg) const {
    validate(reg);
    return m6520_peek(&chip_, input_pins() | std::to_underlying(reg));
}

// Restrict typed control writes to the two control registers.
void Pia6520::set_control(Register reg, PiaControl flags, CycleTime at) {
    if (reg != Register::ControlA && reg != Register::ControlB)
        throw std::invalid_argument("PIA control flags require a control register");
    write(*this, reg, flags.bits(), at);
}

// Resolve the active interrupt output for board wiring.
bool Pia6520::irq() const { return (chip_.pins & M6520_IRQ) != 0; }
// Resolve CA2 according to its configured direction.
bool Pia6520::ca2() const {
    return (chip_.pa.cr & M6520_CR_C2_OUTPUT) ? chip_.pa.c2_out : inputs_.peek_inputs().ca2;
}
// Resolve CB2 according to its configured direction.
bool Pia6520::cb2() const {
    return (chip_.pb.cr & M6520_CR_C2_OUTPUT) ? chip_.pb.c2_out : inputs_.peek_inputs().cb2;
}

// Initialize the underlying VIA model.
Via6522::Via6522() { m6522_init(&chip_); }
// Initialize a fitted VIA with the owner's final context, without rebinding.
Via6522::Via6522(detail::InputEditState& edits) : inputs_(edits) { m6522_init(&chip_); }
// Reset registers while retaining adapter inputs, faults and observations.
void Via6522::reset() { inputs_.require_mutable_device(); m6522_reset(&chip_); }
// Present current driven levels to the chip model.
uint64_t Via6522::input_pins() const { return pins_for(inputs_.peek_inputs()); }

// Reject unsupported modes before the chip can mutate state or history.
void Via6522::validate(const BusAccess& access) {
    // Reject invalid indices and all shift-register accesses.
    if (std::to_underlying(access.reg) > ViaRegisterMask) throw std::out_of_range("6522 register must be in [0, 15]");
    const std::string operation = access.write ? "write" : "read";
    if (access.reg == Register::Shift) {
        throw std::logic_error("unsupported 6522 shift-register " + operation);
    }
    if (!access.write) return;
    // Reads are safe elsewhere, but mode writes must stay within model support.
    if (access.reg == Register::Acr && (access.data & ViaAcrUnsupportedMask)) {
        throw std::logic_error("unsupported 6522 ACR mode " + std::to_string(access.data)
            + " (port latching, shift register and PB7 timer output are not modeled)");
    }
    if (access.reg == Register::Pcr) {
        m6522_t control{};
        control.pcr = access.data;
        if (M6522_PCR_CA2_PULSE_OUTPUT((&control)) || M6522_PCR_CB2_PULSE_OUTPUT((&control))) {
            throw std::logic_error("unsupported 6522 PCR pulse-output mode "
                + std::to_string(access.data));
        }
    }
}

// Complete an optional access while clocking timers and control detectors.
void Via6522::clock(std::optional<BusAccess> access) {
    inputs_.require_mutable_device();
    // Validate and present the selected bus access with external inputs.
    uint64_t pins = input_pins();
    if (access) {
        validate(*access);
        pins |= M6522_CS1 | std::to_underlying(access->reg) | (access->write ? 0 : M6522_RW);
        M6522_SET_DATA(pins, access->data);
    }
    // Advance the model before recording a successfully completed write.
    m6522_tick(&chip_, pins);
    if (access) writes_.record(*access);
}

// Overlay synthetic faults and derive the IRQ summary from enabled sources.
uint8_t Via6522::interrupt_flags() const {
    // Keep source flags separate from the derived summary bit.
    uint8_t flags = chip_.intr.ifr & ViaIrqSources;
    if (timer1_fault_ == Timer1Fault::StuckInterruptFlag) flags |= M6522_IRQ_T1;
    // Assert the summary only when a pending source is enabled.
    if (flags & chip_.intr.ier) flags |= M6522_IRQ_ANY;
    return flags;
}

// Inspect registers without side effects, including the synthetic IFR overlay.
uint8_t Via6522::peek(Register reg) const {
    validate({reg, 0, false, CycleTime{Cycles{0}}});
    if (reg == Register::Ifr) return interrupt_flags();
    return m6522_peek(&chip_, input_pins() | std::to_underlying(reg));
}

// Keep IER's set/clear selector separate from interrupt-source bits.
void Via6522::set_interrupts(ViaInterrupts sources, bool enabled, CycleTime at) {
    if (sources.contains(ViaInterruptBit::Summary))
        throw std::invalid_argument("VIA summary bit is not an interrupt source");
    write(*this, Register::Ier,
        static_cast<uint8_t>(sources.bits() | (enabled ? ViaIerSet : 0)), at);
}

// Resolve enabled chip and injected interrupts for board wiring.
bool Via6522::irq() const { return (interrupt_flags() & M6522_IRQ_ANY) != 0; }
// Resolve CA2 according to its configured direction.
bool Via6522::ca2() const { return M6522_PCR_CA2_OUTPUT((&chip_)) ? chip_.pa.c2_out : inputs_.peek_inputs().ca2; }
// Resolve CB2 according to its configured direction.
bool Via6522::cb2() const { return M6522_PCR_CB2_OUTPUT((&chip_)) ? chip_.pb.c2_out : inputs_.peek_inputs().cb2; }

// Set only supported adversarial fault states, independent of normal timers.
void Via6522::set_timer1_fault(Timer1Fault fault) {
    inputs_.require_mutable_device();
    switch (fault) {
    case Timer1Fault::None:
    case Timer1Fault::StuckInterruptFlag:
        timer1_fault_ = fault;
        return;
    }
    throw std::invalid_argument("unsupported 6522 timer-one fault");
}

// Latch one stable access at falling PHI2, with reset taking precedence.
void Io::sample(const BusSample& bus, CycleTime at) {
    if (edits_.guard) edits_.guard();
    if (edits_.active) throw std::logic_error("device inputs: sampling during editing is not allowed");
    const bool cpu_falling = previous_.phi2 && !bus.phi2 && !previous_.reset;
    const bool pia1_falling = previous_.pia1_phi2.value_or(previous_.phi2)
        && !bus.pia1_phi2.value_or(bus.phi2) && !previous_.reset;
    // Reset ignores physical selects and cancels any pending access.
    if (!bus.reset && bus.selects != ChipSelect::None && bus.selects != ChipSelect::Pia1
        && bus.selects != ChipSelect::Pia2 && bus.selects != ChipSelect::Via) {
        throw std::logic_error("external I/O requires at most one physical chip select");
    }
    // Reset only on assertion, canceling any pending access.
    if (bus.reset) {
        if (!previous_.reset) {
            pia1_.reset();
            pia2_.reset();
            via_.reset();
        }
    } else if (cpu_falling || pia1_falling) {
        // Decode the preceding stable sample, not the new falling-edge levels.
        const Pia6520::BusAccess pia_access{
            static_cast<PiaRegister>(previous_.reg & PiaRegisterMask), previous_.data, previous_.write, at};
        const Via6522::BusAccess via_access{
            static_cast<ViaRegister>(previous_.reg), previous_.data, previous_.write, at};
        // Preflight the selected access before any device can advance or latch IRQs.
        if ((pia1_falling && previous_.selects == ChipSelect::Pia1)
            || (cpu_falling && previous_.selects == ChipSelect::Pia2)) {
            Pia6520::validate(pia_access.reg);
        } else if (cpu_falling && previous_.selects == ChipSelect::Via) {
            Via6522::validate(via_access);
        }
        // Advance each fitted chip only at its own validated PHI2 boundary.
        if (pia1_falling)
            pia1_.clock(previous_.selects == ChipSelect::Pia1 ? std::optional<Pia6520::BusAccess>(pia_access) : std::nullopt);
        if (cpu_falling) {
            pia2_.clock(previous_.selects == ChipSelect::Pia2 ? std::optional<Pia6520::BusAccess>(pia_access) : std::nullopt);
            via_.clock(previous_.selects == ChipSelect::Via ? std::optional<Via6522::BusAccess>(via_access) : std::nullopt);
            ++clocks_;
        }
    }
    // Retain the current levels for the next latch/reset transition.
    previous_ = bus;
}

// Clear observations independently of reset, fault injection and bus levels.
void Io::clear_observations() {
    pia1_.clear_observations();
    pia2_.clear_observations();
    via_.clear_observations();
    clocks_ = 0;
}

} // namespace econopet::io

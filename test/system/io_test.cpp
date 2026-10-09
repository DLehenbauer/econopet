// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#include "io.h"

#include <array>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include <gtest/gtest.h>

namespace {
using namespace econopet::io;

// Copying or moving identities must never replace guards or share edit ownership.
template<class Device>
constexpr bool HasFixedIdentity =
    !std::is_copy_constructible_v<Device> && !std::is_move_constructible_v<Device>
    && !std::is_copy_assignable_v<Device> && !std::is_move_assignable_v<Device>;
static_assert(HasFixedIdentity<Pia6520>);
static_assert(HasFixedIdentity<Via6522>);
static_assert(HasFixedIdentity<Io>);
static_assert(!std::is_assignable_v<decltype(std::declval<Io&>().via()), Via6522>);
static_assert(std::is_copy_constructible_v<Inputs>);
static_assert(std::is_copy_constructible_v<m6520_t>);
static_assert(std::is_copy_constructible_v<m6522_t>);

// Shared fixture bounds and GPIO direction masks, not chip register values.
constexpr unsigned PeekRepetitions = 8;
constexpr unsigned StableBusSamples = 32;
constexpr unsigned IdleTimerClocks = 100;
constexpr unsigned TimerPipelineClocks = 2;
constexpr unsigned MaximumTimerLoad = 0xffff;
constexpr unsigned MaximumTimerPreExpiryClocks = MaximumTimerLoad + 1;
constexpr unsigned FreeRunPeriods = 3;
constexpr unsigned DeselectedTimerClocks = 10;
constexpr unsigned MaximumRegisterValue = std::numeric_limits<uint8_t>::max();
constexpr uint8_t LowNibbleOutputs = 0x0f;
constexpr uint8_t HighNibbleOutputs = 0xf0;
constexpr uint8_t OutputLatchReset = 0;
constexpr uint8_t NoInterrupts = 0;
constexpr econopet::CycleTime DeviceSetupTime{econopet::Cycles{0}};

constexpr uint8_t PiaIrqFlags = (PiaControlBit::Irq1 | PiaControlBit::Irq2).bits();
constexpr PiaControl PiaIrqEnables = PiaControlBit::C1IrqEnable | PiaControlBit::C2Mode;
constexpr uint8_t ViaControlReset = 0;
constexpr uint8_t ViaPcrCa2ModeMask = 0x0e;
constexpr uint8_t ViaPcrCb2ModeMask = 0xe0;
constexpr uint8_t ViaPcrCa2Pulse = 0x0a;
constexpr uint8_t ViaPcrCb2Pulse = 0xa0;
constexpr uint8_t ViaPcrAllPositiveEdges = 0x55;
constexpr uint8_t ViaPcrCa2IndependentIrq = 0x02;
constexpr uint8_t ViaPcrBothHandshake = 0x88;
constexpr uint8_t ViaPcrBothLow = 0xcc;
constexpr uint8_t ViaPcrBothHigh = 0xee;
constexpr uint8_t ViaIerSet = std::to_underlying(ViaInterruptBit::Summary);
constexpr uint8_t ViaIrqSources = std::to_underlying(ViaInterruptBit::All) & ~ViaIerSet;
constexpr uint8_t ViaControlIrqs = (ViaInterruptBit::Ca2 | ViaInterruptBit::Ca1
    | ViaInterruptBit::Cb2 | ViaInterruptBit::Cb1).bits();
constexpr uint8_t ViaTimerIrqs = (ViaInterruptBit::Timer1 | ViaInterruptBit::Timer2).bits();
constexpr uint8_t ViaTimer1IrqStatus = (ViaInterruptBit::Summary | ViaInterruptBit::Timer1).bits();

// Compare all detached input levels without ownership or guard machinery.
auto model_values(const Inputs& inputs) {
    return std::make_tuple(inputs.port_a, inputs.port_b,
        inputs.ca1, inputs.ca2, inputs.cb1, inputs.cb2);
}

// Compare all meaningful PIA port fields without reading struct padding.
auto model_values(const m6520_port_t& port) {
    return std::make_tuple(port.inpr, port.outr, port.ddr, port.cr, port.pins,
        port.c1_in, port.c1_triggered, port.c2_in, port.c2_out, port.c2_triggered, port.c2_pulse);
}

// Capture both PIA ports and the complete pin state as detached comparable values.
auto model_values(const m6520_t& chip) {
    return std::make_tuple(model_values(chip.pa), model_values(chip.pb), chip.pins);
}

// Compare all meaningful VIA port fields without reading struct padding.
auto model_values(const m6522_port_t& port) {
    return std::make_tuple(port.inpr, port.outr, port.ddr, port.pins,
        port.c1_in, port.c1_out, port.c1_triggered, port.c2_in, port.c2_out, port.c2_triggered);
}

// Include timer pipelines as well as counters to detect any premature clocking.
auto model_values(const m6522_timer_t& timer) {
    return std::make_tuple(timer.latch, timer.counter, timer.t_bit, timer.t_out, timer.pip);
}

// Include interrupt delivery pipelines as well as source flags and enables.
auto model_values(const m6522_int_t& interrupts) {
    return std::make_tuple(interrupts.ier, interrupts.ifr, interrupts.pip);
}

// Capture all VIA state, including ports, timers, interrupts, modes and pins.
auto model_values(const m6522_t& chip) {
    return std::make_tuple(model_values(chip.pa), model_values(chip.pb),
        model_values(chip.t1), model_values(chip.t2), model_values(chip.intr),
        chip.acr, chip.pcr, chip.pins);
}
} // namespace

// Named getters preserve device identity and flags retain their register domains.
TEST(ExternalTypes, TypedSelectionsAndFlagsUseTheActualDeviceLatches) {
    Io io;
    auto& pia = io.pia1();
    EXPECT_EQ(&std::as_const(io).pia1(), &pia);
    EXPECT_EQ(&std::as_const(io).pia2(), &io.pia2());
    EXPECT_NE(&pia, &io.pia2());
    const auto control = PiaControlBit::PortAccess | PiaControlBit::C1IrqEnable;
    pia.set_control(PiaRegister::ControlA, control, DeviceSetupTime);
    EXPECT_EQ(pia.peek(PiaRegister::ControlA), control.bits());
    EXPECT_EQ(pia.writes().last()->reg, PiaRegister::ControlA);
    const auto writes = pia.writes().count();
    EXPECT_THROW(pia.set_control(PiaRegister::PortA, control, DeviceSetupTime), std::invalid_argument);
    EXPECT_EQ(pia.writes().count(), writes);

    // Enable and clear one VIA source without confusing IER's selector with IFR.
    auto& via = io.via();
    via.set_interrupts(ViaInterruptBit::Timer1, true, DeviceSetupTime);
    via.set_timer1_fault(Via6522::Timer1Fault::StuckInterruptFlag);
    EXPECT_TRUE(via.interrupts().contains(ViaInterruptBit::Timer1));
    EXPECT_TRUE(via.interrupts().contains(ViaInterruptBit::Summary));
    via.set_interrupts(ViaInterruptBit::Timer1, false, DeviceSetupTime);
    EXPECT_FALSE(via.interrupts().contains(ViaInterruptBit::Summary));
    const auto via_writes = via.writes().count();
    EXPECT_THROW(via.set_interrupts(ViaInterruptBit::Summary, true, DeviceSetupTime), std::invalid_argument);
    EXPECT_THROW(via.set_acr(ViaAcrBit::LatchA, DeviceSetupTime), std::logic_error);
    EXPECT_EQ(via.writes().count(), via_writes);
}

// Named PIA modes pass directly to the typed API and retain their hardware encodings.
TEST(ExternalPia, NamedControlsWorkWithTypedWritesOnBothPorts) {
    constexpr std::array controls{
        std::pair<PiaControl, uint8_t>{PiaDdrAccess, 0x00},
        std::pair<PiaControl, uint8_t>{PiaPortAccess, 0x04},
        std::pair<PiaControl, uint8_t>{PiaC2Handshake, 0x24},
        std::pair<PiaControl, uint8_t>{PiaC2Pulse, 0x2c},
        std::pair<PiaControl, uint8_t>{PiaC2Low, 0x34},
        std::pair<PiaControl, uint8_t>{PiaC2High, 0x3c},
    };
    const econopet::CycleTime at{econopet::Cycles{7}};
    Pia6520 pia;
    for (const auto reg : {PiaRegister::ControlA, PiaRegister::ControlB}) {
        for (const auto& [control, encoding] : controls) {
            SCOPED_TRACE(static_cast<unsigned>(encoding));
            pia.set_control(reg, control, at);
            EXPECT_EQ(pia.peek(reg), encoding);
            ASSERT_TRUE(pia.writes().last());
            EXPECT_EQ(pia.writes().last()->data, encoding);
            EXPECT_EQ(pia.writes().last()->reg, reg);
            EXPECT_EQ(pia.writes().last()->at, at);
        }
    }
}

// Convenience accesses clock once, preserve timestamps, and distinguish reads from peeks.
TEST(ExternalVia, RegisterHelpersClockExactlyOnceAndTimestampWrites) {
    constexpr uint8_t TimerLoad = 100;
    const econopet::CycleTime low_at{econopet::Cycles{7}};
    const auto high_at = low_at + econopet::Cycles{1};
    const auto read_at = high_at + econopet::Cycles{1};
    const auto write_at = read_at + econopet::Cycles{1};
    Via6522 via;
    // Load and prime a timer so one clock is observable as exactly one decrement.
    write(via, ViaRegister::Timer1Low, TimerLoad, low_at);
    write(via, ViaRegister::Timer1High, 0, high_at);
    for (unsigned i = 0; i < TimerPipelineClocks; ++i) via.clock();
    const auto counter = via.state().t1.counter;
    ASSERT_GT(counter, 1);
    const auto before_peek = model_values(via.state());
    const auto writes = via.writes().count();
    // Peeking changes nothing, while a completed read consumes one peripheral clock.
    EXPECT_EQ(via.peek(ViaRegister::Timer1Low), counter);
    EXPECT_EQ(model_values(via.state()), before_peek);
    EXPECT_EQ(read(via, ViaRegister::Timer1Low, read_at), counter);
    EXPECT_EQ(via.state().t1.counter, counter - 1);
    EXPECT_EQ(via.writes().count(), writes);
    ASSERT_TRUE(via.writes().last());
    EXPECT_EQ(via.writes().last()->at, high_at);
    // A raw write consumes one more clock and records exactly the supplied timestamp.
    write(via, ViaRegister::PortB, 0x42, write_at);
    EXPECT_EQ(via.state().t1.counter, counter - 2);
    EXPECT_EQ(via.writes().count(), writes + 1);
    EXPECT_EQ(via.writes().last()->reg, ViaRegister::PortB);
    EXPECT_EQ(via.writes().last()->data, 0x42);
    EXPECT_EQ(via.writes().last()->at, write_at);
}

// Keep the typed ACR bit definitions aligned with the pinned model's decoding.
TEST(ExternalTypes, AcrBitsMatchPinnedChipPredicates) {
    m6522_t control{};
    // Check the latch and shift fields against the model's mode predicates.
    control.acr = std::to_underlying(ViaAcrBit::LatchA);
    EXPECT_TRUE(M6522_ACR_PA_LATCH_ENABLE((&control)));
    control.acr = std::to_underlying(ViaAcrBit::LatchB);
    EXPECT_TRUE(M6522_ACR_PB_LATCH_ENABLE((&control)));
    control.acr = std::to_underlying(ViaAcrBit::ShiftBit0);
    EXPECT_TRUE(M6522_ACR_SI_T2_CONTROL((&control)));
    control.acr = std::to_underlying(ViaAcrBit::ShiftBit1);
    EXPECT_TRUE(M6522_ACR_SI_O2_CONTROL((&control)));
    control.acr = std::to_underlying(ViaAcrBit::ShiftBit2);
    EXPECT_TRUE(M6522_ACR_SO_T2_RATE((&control)));
    // Check each timer mode independently of the adapter's validation masks.
    control.acr = std::to_underlying(ViaAcrBit::Timer2CountPb6);
    EXPECT_TRUE(M6522_ACR_T2_COUNT_PB6((&control)));
    control.acr = std::to_underlying(ViaAcrBit::Timer1Continuous);
    EXPECT_TRUE(M6522_ACR_T1_CONTINUOUS((&control)));
    control.acr = std::to_underlying(ViaAcrBit::Timer1Pb7);
    EXPECT_TRUE(M6522_ACR_T1_SET_PB7((&control)));
}

// Standalone devices allow independent edits but reject their own reentrant mutations.
TEST(ExternalBus, StandaloneDevicesOwnIndependentEditContexts) {
    Pia6520 pia;
    Via6522 via;
    EXPECT_THROW(pia.inputs([&](auto& inputs) {
        inputs.port_a = 0x42;
        EXPECT_THROW(pia.inputs([](auto&) {}), std::logic_error);
        EXPECT_THROW(pia.clock(), std::logic_error);
        via.inputs([](auto& levels) { levels.port_b = 0x37; });
        throw std::runtime_error("injected standalone edit failure");
    }), std::runtime_error);
    EXPECT_EQ(pia.peek_inputs().port_a, PortAllHigh);
    EXPECT_EQ(via.peek_inputs().port_b, 0x37);
    EXPECT_NO_THROW(pia.clock());
    pia.inputs([](auto& inputs) { inputs.port_a = 0x42; });
    EXPECT_EQ(pia.peek_inputs().port_a, 0x42);
}

// Each fitted source excludes nested edits and sibling mutations, then recovers atomically.
TEST(ExternalBus, FittedDeviceEditsExcludeMutationAndRecoverAtomically) {
    using Device = std::variant<Pia6520*, Via6522*>;
    Io devices;
    const std::array<Device, 3> fitted{&devices.pia1(), &devices.pia2(), &devices.via()};
    for (size_t source_index = 0; source_index < fitted.size(); ++source_index) {
        SCOPED_TRACE(source_index);
        const std::array before_inputs{
            devices.pia1().peek_inputs(), devices.pia2().peek_inputs(), devices.via().peek_inputs()};
        const auto before_pia1 = model_values(devices.pia1().state());
        const auto before_pia2 = model_values(devices.pia2().state());
        const auto before_via = model_values(devices.via().state());
        // Exercise every target while a detached edit owns the fitted hierarchy.
        const auto reject_mutation = [&] {
            EXPECT_TRUE(devices.editing_inputs());
            for (const auto& target : fitted) {
                std::visit([](auto* device) {
                    EXPECT_THROW(device->inputs([](auto&) {}), std::logic_error);
                    EXPECT_THROW(device->clock(), std::logic_error);
                    EXPECT_THROW(device->reset(), std::logic_error);
                    EXPECT_THROW(device->clear_observations(), std::logic_error);
                }, target);
            }
            EXPECT_THROW(devices.sample({}, DeviceSetupTime), std::logic_error);
            EXPECT_THROW(devices.clear_observations(), std::logic_error);
        };
        // A failed callback discards every changed input and releases edit ownership.
        std::visit([&](auto* source) {
            EXPECT_THROW(source->inputs([&](auto& inputs) {
                inputs.port_a = 0x42;
                inputs.ca1 = false;
                reject_mutation();
                EXPECT_EQ(model_values(source->peek_inputs()), model_values(before_inputs[source_index]));
                throw std::runtime_error("injected fitted edit failure");
            }), std::runtime_error);
        }, fitted[source_index]);
        EXPECT_FALSE(devices.editing_inputs());
        for (size_t index = 0; index < fitted.size(); ++index) {
            std::visit([&](auto* device) {
                EXPECT_EQ(model_values(device->peek_inputs()), model_values(before_inputs[index]));
            }, fitted[index]);
        }
        EXPECT_EQ(model_values(devices.pia1().state()), before_pia1);
        EXPECT_EQ(model_values(devices.pia2().state()), before_pia2);
        EXPECT_EQ(model_values(devices.via().state()), before_via);
        // A subsequent successful edit commits only to its source.
        std::visit([&](auto* source) {
            source->inputs([&](auto& inputs) {
                inputs.port_a = 0x37;
                inputs.ca1 = false;
                reject_mutation();
            });
        }, fitted[source_index]);
        EXPECT_FALSE(devices.editing_inputs());
        for (size_t index = 0; index < fitted.size(); ++index) {
            auto expected = before_inputs[index];
            if (index == source_index) {
                expected.port_a = 0x37;
                expected.ca1 = false;
            }
            std::visit([&](auto* device) {
                EXPECT_EQ(model_values(device->peek_inputs()), model_values(expected));
                EXPECT_NO_THROW(device->inputs([](auto&) {}));
                EXPECT_NO_THROW(device->clock());
                EXPECT_NO_THROW(device->reset());
                EXPECT_NO_THROW(device->clear_observations());
            }, fitted[index]);
        }
        EXPECT_NO_THROW(devices.sample({}, DeviceSetupTime));
        EXPECT_NO_THROW(devices.clear_observations());
    }
}

// Recheck an optional mutation guard after callbacks before committing their edits.
TEST(ExternalBus, InputCommitRechecksMutationGuard) {
    bool blocked = false;
    Io devices([&] {
        if (blocked) throw std::logic_error("injected peripheral mutation guard");
    });
    EXPECT_THROW(devices.via().inputs([&](auto& inputs) {
        inputs.port_b = 0x42;
        blocked = true;
    }), std::logic_error);
    EXPECT_EQ(devices.via().peek_inputs().port_b, PortAllHigh);
    EXPECT_FALSE(devices.editing_inputs());
    EXPECT_THROW(devices.via().clock(), std::logic_error);
    blocked = false;
    EXPECT_NO_THROW(devices.via().clock());
}

// Separate owners retain independent guards and callback edit ownership.
TEST(ExternalBus, IndependentOwnersRetainGuardsAndEditOwnership) {
    bool blocked = false;
    Io guarded([&] {
        if (blocked) throw std::logic_error("injected peripheral mutation guard");
    });
    Io independent;
    auto& pia = guarded.pia1();
    auto& via = guarded.via();
    blocked = true;
    EXPECT_THROW(pia.clock(), std::logic_error);
    EXPECT_THROW(via.reset(), std::logic_error);
    independent.pia1().inputs([&](auto& inputs) {
        EXPECT_TRUE(independent.editing_inputs());
        EXPECT_FALSE(guarded.editing_inputs());
        EXPECT_THROW(via.inputs([](auto&) {}), std::logic_error);
        inputs.port_a = 0x42;
    });
    EXPECT_EQ(independent.pia1().peek_inputs().port_a, 0x42);
    EXPECT_EQ(pia.peek_inputs().port_a, PortAllHigh);
    EXPECT_FALSE(independent.editing_inputs());
    blocked = false;
    EXPECT_NO_THROW(via.clock());
}

// Snapshot values remain copyable and detached without replacing live ownership.
TEST(ExternalBus, ModelSnapshotsDoNotTransferGuardOwnership) {
    bool blocked = false;
    Io devices([&] {
        if (blocked) throw std::logic_error("injected peripheral mutation guard");
    });
    auto& via = devices.via();
    write(via, ViaRegister::PortB, 0x42, DeviceSetupTime);
    const auto snapshot = via.state();
    const auto inputs = via.peek_inputs();
    blocked = true;
    EXPECT_THROW(write(via, ViaRegister::PortB, 0x37, DeviceSetupTime), std::logic_error);
    EXPECT_EQ(via.state().pb.outr, snapshot.pb.outr);
    EXPECT_EQ(via.peek_inputs().port_b, inputs.port_b);
    blocked = false;
    write(via, ViaRegister::PortB, 0x37, DeviceSetupTime);
    EXPECT_EQ(via.state().pb.outr, 0x37);
    EXPECT_EQ(snapshot.pb.outr, 0x42);
}

// Verify DDR/port selection preserves independent latches on both PIA ports.
TEST(ExternalPia, DirectionAndOutputLatchesAreIndependentOnBothPorts) {
    // Configure mixed directions, then write output latches.
    Pia6520 pia;
    pia.inputs([](auto& inputs) { inputs.port_a = 0xa5; inputs.port_b = 0x5a; });
    write(pia, PiaRegister::PortA, LowNibbleOutputs, DeviceSetupTime);
    write(pia, PiaRegister::PortB, HighNibbleOutputs, DeviceSetupTime);
    pia.set_control(PiaRegister::ControlA, PiaPortAccess, DeviceSetupTime);
    pia.set_control(PiaRegister::ControlB, PiaPortAccess, DeviceSetupTime);
    write(pia, PiaRegister::PortA, 0x33, DeviceSetupTime);
    write(pia, PiaRegister::PortB, 0x33, DeviceSetupTime);
    // Reads combine external inputs and output bits according to DDR.
    EXPECT_EQ(read(pia, PiaRegister::PortA, DeviceSetupTime), 0xa3);
    EXPECT_EQ(read(pia, PiaRegister::PortB, DeviceSetupTime), 0x3a);
    // Switching back to DDR access must not overwrite output latches.
    pia.set_control(PiaRegister::ControlA, PiaDdrAccess, DeviceSetupTime);
    pia.set_control(PiaRegister::ControlB, PiaDdrAccess, DeviceSetupTime);
    EXPECT_EQ(read(pia, PiaRegister::PortA, DeviceSetupTime), LowNibbleOutputs);
    EXPECT_EQ(read(pia, PiaRegister::PortB, DeviceSetupTime), HighNibbleOutputs);
    EXPECT_EQ(pia.state().pa.outr, 0x33);
    EXPECT_EQ(pia.state().pb.outr, 0x33);
    // Reject the first register outside the PIA's decoded range.
    const auto invalid = static_cast<PiaRegister>(PiaRegisterMask + 1);
    EXPECT_THROW(pia.peek(invalid), std::out_of_range);
    EXPECT_THROW(write(pia, invalid, 0, DeviceSetupTime), std::out_of_range);
}

// Verify both edge polarities latch IRQs independently of control storage.
TEST(ExternalPia, InterruptEdgesFlagsAndEnablesAreNotRegisterStorage) {
    for (bool positive : {false, true}) {
        for (auto side : {PiaRegister::PortA, PiaRegister::PortB}) {
            // Prime each input opposite its configured active edge.
            Pia6520 pia;
            const auto set_edges = [&](bool level) {
                pia.inputs([&](auto& inputs) {
                    (side == PiaRegister::PortA ? inputs.ca1 : inputs.cb1) = level;
                    (side == PiaRegister::PortA ? inputs.ca2 : inputs.cb2) = level;
                });
            };
            set_edges(!positive);
            pia.clock();
            const auto control_reg = side == PiaRegister::PortA ? PiaRegister::ControlA : PiaRegister::ControlB;
            const auto edges = positive
                ? PiaControlBit::C1PositiveEdge | PiaControlBit::C2PositiveEdge : PiaControl{};
            const auto control = PiaPortAccess | edges;
            pia.set_control(control_reg, control | PiaIrqEnables, DeviceSetupTime);
            // Trigger both flags and prove peeking does not acknowledge them.
            set_edges(positive);
            pia.clock();
            EXPECT_EQ(pia.peek(control_reg) & PiaIrqFlags, PiaIrqFlags);
            EXPECT_TRUE(pia.irq());
            for (unsigned i = 0; i < PeekRepetitions; ++i) pia.peek(side);
            EXPECT_EQ(pia.peek(control_reg) & PiaIrqFlags, PiaIrqFlags);
            // Disable delivery without clearing flags, then acknowledge by read.
            pia.set_control(control_reg, control, DeviceSetupTime);
            EXPECT_FALSE(pia.irq());
            EXPECT_EQ(pia.peek(control_reg) & PiaIrqFlags, PiaIrqFlags);
            read(pia, side, DeviceSetupTime);
            EXPECT_EQ(pia.peek(control_reg) & PiaIrqFlags, NoInterrupts);
        }
    }
}

// Verify fixed, handshake and one-cycle pulse C2 outputs on both PIA ports.
TEST(ExternalPia, ManualHandshakeAndPulseControlOutputs) {
    Pia6520 pia;
    pia.clock();
    // Fixed output modes directly control each C2 level.
    for (auto reg : {PiaRegister::ControlA, PiaRegister::ControlB}) {
        pia.set_control(reg, PiaC2Low, DeviceSetupTime);
        EXPECT_FALSE(reg == PiaRegister::ControlA ? pia.ca2() : pia.cb2());
        pia.set_control(reg, PiaC2High, DeviceSetupTime);
        EXPECT_TRUE(reg == PiaRegister::ControlA ? pia.ca2() : pia.cb2());
    }
    // Port A handshakes on reads, releasing on its C1 edge.
    pia.set_control(PiaRegister::ControlA, PiaC2Handshake, DeviceSetupTime);
    write(pia, PiaRegister::PortA, 0, DeviceSetupTime);
    EXPECT_TRUE(pia.ca2());
    read(pia, PiaRegister::PortA, DeviceSetupTime);
    EXPECT_FALSE(pia.ca2());
    pia.inputs([](auto& inputs) { inputs.ca1 = false; });
    pia.clock();
    EXPECT_TRUE(pia.ca2());
    // Port B handshakes on writes rather than reads.
    pia.set_control(PiaRegister::ControlB, PiaC2Handshake, DeviceSetupTime);
    read(pia, PiaRegister::PortB, DeviceSetupTime);
    EXPECT_TRUE(pia.cb2());
    write(pia, PiaRegister::PortB, 0, DeviceSetupTime);
    EXPECT_FALSE(pia.cb2());
    pia.inputs([](auto& inputs) { inputs.cb1 = false; });
    pia.clock();
    EXPECT_TRUE(pia.cb2());
    // Pulse modes release automatically on the next device clock.
    pia.set_control(PiaRegister::ControlA, PiaC2Pulse, DeviceSetupTime);
    read(pia, PiaRegister::PortA, DeviceSetupTime);
    EXPECT_FALSE(pia.ca2());
    pia.clock();
    EXPECT_TRUE(pia.ca2());
    pia.set_control(PiaRegister::ControlB, PiaC2Pulse, DeviceSetupTime);
    write(pia, PiaRegister::PortB, 0, DeviceSetupTime);
    EXPECT_FALSE(pia.cb2());
    pia.clock();
    EXPECT_TRUE(pia.cb2());
}

// Verify VIA mixed GPIO and no-handshake reads use the same DDR/latch data.
TEST(ExternalVia, GpioUsesDirectionAndOutputLatches) {
    // Drive external inputs and configure complementary direction masks.
    Via6522 via;
    via.inputs([](auto& inputs) { inputs.port_a = 0xa5; inputs.port_b = 0x5a; });
    write(via, ViaRegister::DdrA, LowNibbleOutputs, DeviceSetupTime);
    write(via, ViaRegister::DdrB, HighNibbleOutputs, DeviceSetupTime);
    write(via, ViaRegister::PortA, 0x33, DeviceSetupTime);
    write(via, ViaRegister::PortB, 0x33, DeviceSetupTime);
    // Check mixed levels without altering the direction registers.
    EXPECT_EQ(read(via, ViaRegister::PortA, DeviceSetupTime), 0xa3);
    EXPECT_EQ(read(via, ViaRegister::PortB, DeviceSetupTime), 0x3a);
    EXPECT_EQ(read(via, ViaRegister::PortANoHandshake, DeviceSetupTime), 0xa3);
    EXPECT_EQ(via.state().pa.ddr, LowNibbleOutputs);
    EXPECT_EQ(via.state().pb.ddr, HighNibbleOutputs);
}

// Verify T1's load-plus-two latency, enable gating and read acknowledgment.
TEST(ExternalVia, TimerOneExpiresAfterLoadedValuePlusTwoClocksAndAcknowledges) {
    constexpr unsigned TimerLoad = 7;
    Via6522 via;
    // Load a one-shot and prove it stays quiet until the exact expiry edge.
    write(via, ViaRegister::Ier, ViaIerSet | M6522_IRQ_T1, DeviceSetupTime);
    write(via, ViaRegister::Timer1Low, TimerLoad, DeviceSetupTime);
    write(via, ViaRegister::Timer1High, 0, DeviceSetupTime);
    ASSERT_EQ(via.state().t1.counter, TimerLoad);
    for (unsigned i = 0; i < TimerLoad + TimerPipelineClocks - 1; ++i) {
        via.clock();
        EXPECT_EQ(via.peek(ViaRegister::Ifr) & M6522_IRQ_T1, NoInterrupts);
    }
    via.clock();
    EXPECT_EQ(via.peek(ViaRegister::Ifr) & ViaTimer1IrqStatus, ViaTimer1IrqStatus);
    EXPECT_TRUE(via.irq());
    // The summary bit cannot clear a source, and peeking cannot tick the timer.
    write(via, ViaRegister::Ifr, M6522_IRQ_ANY, DeviceSetupTime);
    EXPECT_EQ(via.peek(ViaRegister::Ifr) & ViaTimer1IrqStatus, ViaTimer1IrqStatus);
    const auto counter = via.state().t1.counter;
    for (unsigned i = 0; i < PeekRepetitions; ++i) via.peek(ViaRegister::Timer1Low);
    EXPECT_EQ(via.state().t1.counter, counter);
    EXPECT_TRUE(via.irq());
    // Enable changes gate IRQ delivery but preserve the pending source flag.
    write(via, ViaRegister::Ier, M6522_IRQ_T1, DeviceSetupTime);
    EXPECT_FALSE(via.irq());
    EXPECT_EQ(via.peek(ViaRegister::Ifr) & ViaTimer1IrqStatus, M6522_IRQ_T1);
    write(via, ViaRegister::Ier, ViaIerSet | M6522_IRQ_T1, DeviceSetupTime);
    EXPECT_TRUE(via.irq());
    // A completed low-byte read acknowledges the one-shot until reloading.
    read(via, ViaRegister::Timer1Low, DeviceSetupTime);
    EXPECT_FALSE(via.irq());
    for (unsigned i = 0; i < IdleTimerClocks; ++i) via.clock();
    EXPECT_EQ(via.peek(ViaRegister::Ifr) & M6522_IRQ_T1, NoInterrupts);
    write(via, ViaRegister::Timer1High, 0, DeviceSetupTime);
    EXPECT_EQ(via.state().t1.counter, TimerLoad);
}

// Verify T1 free-running reloads and T2 timed versus PB6-edge counting.
TEST(ExternalVia, TimerOneFreeRunReloadsAndTimerTwoSupportsBothClockSources) {
    constexpr unsigned Timer1Load = 3;
    constexpr unsigned Timer2Load = 2;
    Via6522 via;
    // Exercise repeated free-running expiries and explicit T1 rearming.
    via.set_acr(ViaAcrBit::Timer1Continuous, DeviceSetupTime);
    write(via, ViaRegister::Timer1Low, Timer1Load, DeviceSetupTime);
    write(via, ViaRegister::Timer1High, 0, DeviceSetupTime);
    for (unsigned period = 0; period < FreeRunPeriods; ++period) {
        for (unsigned i = 0; i < Timer1Load + TimerPipelineClocks; ++i) via.clock();
        EXPECT_EQ(via.peek(ViaRegister::Ifr) & M6522_IRQ_T1, M6522_IRQ_T1);
        write(via, ViaRegister::Ifr, M6522_IRQ_T1, DeviceSetupTime);
        for (unsigned i = 0; i < Timer1Load; ++i) {
            via.clock();
            EXPECT_EQ(via.peek(ViaRegister::Ifr) & M6522_IRQ_T1, NoInterrupts);
        }
        via.clock();
        EXPECT_EQ(via.peek(ViaRegister::Ifr) & M6522_IRQ_T1, M6522_IRQ_T1);
        write(via, ViaRegister::Timer1High, 0, DeviceSetupTime);
    }
    // Timed T2 expires at the same load-plus-two clock boundary.
    write(via, ViaRegister::Timer2Low, Timer2Load, DeviceSetupTime);
    write(via, ViaRegister::Timer2High, 0, DeviceSetupTime);
    for (unsigned i = 0; i < Timer2Load + TimerPipelineClocks - 1; ++i) {
        via.clock();
        EXPECT_EQ(via.peek(ViaRegister::Ifr) & M6522_IRQ_T2, NoInterrupts);
    }
    via.clock();
    EXPECT_EQ(via.peek(ViaRegister::Ifr) & M6522_IRQ_T2, M6522_IRQ_T2);
    read(via, ViaRegister::Timer2Low, DeviceSetupTime);
    EXPECT_EQ(via.peek(ViaRegister::Ifr) & M6522_IRQ_T2, NoInterrupts);

    // PB6 mode ignores clocks with stable inputs and counts falling edges.
    via.set_acr(ViaAcrBit::Timer2CountPb6, DeviceSetupTime);
    write(via, ViaRegister::Timer2High, 0, DeviceSetupTime);
    for (unsigned i = 0; i < DeselectedTimerClocks; ++i) via.clock();
    EXPECT_EQ(via.state().t2.counter, Timer2Load);
    for (unsigned edge = 0; edge < Timer2Load + 1; ++edge) {
        via.inputs([](auto& inputs) { inputs.port_b &= ~ViaPb6; });
        via.clock();
        EXPECT_EQ(via.peek(ViaRegister::Ifr) & M6522_IRQ_T2,
            edge == Timer2Load ? M6522_IRQ_T2 : NoInterrupts);
        via.inputs([](auto& inputs) { inputs.port_b |= ViaPb6; });
        via.clock();
    }
}

// T2 must sample the driven PB6 level when its DDR bit selects an output.
TEST(ExternalVia, Pb6CountingUsesOutputLevelsInsteadOfExternalInputs) {
    constexpr uint8_t TimerLoad = 9;
    for (const bool external_high : {false, true}) {
        for (const bool output_high : {false, true}) {
            SCOPED_TRACE(testing::Message() << "external_high=" << external_high
                << " output_high=" << output_high);
            Via6522 via;
            // Configure opposing external/output levels before arming pulse counting.
            via.inputs([&](auto& inputs) { inputs.port_b = external_high ? ViaPb6 : 0; });
            write(via, ViaRegister::PortB, output_high ? ViaPb6 : 0, DeviceSetupTime);
            write(via, ViaRegister::DdrB, ViaPb6, DeviceSetupTime);
            via.set_acr(ViaAcrBit::Timer2CountPb6, DeviceSetupTime);
            write(via, ViaRegister::Timer2Low, TimerLoad, DeviceSetupTime);
            write(via, ViaRegister::Timer2High, 0, DeviceSetupTime);
            ASSERT_EQ(via.state().t2.counter, TimerLoad);
            // Stable outputs and external-only transitions must not produce pulses.
            for (unsigned i = 0; i < DeselectedTimerClocks; ++i) {
                via.clock();
                ASSERT_EQ(via.state().t2.counter, TimerLoad);
            }
            for (const bool high : {true, false, true, false}) {
                via.inputs([&](auto& inputs) { inputs.port_b = high ? ViaPb6 : 0; });
                via.clock();
                ASSERT_EQ(via.state().t2.counter, TimerLoad);
                EXPECT_EQ(via.state().pb.pins & ViaPb6, output_high ? ViaPb6 : 0);
            }
            // An output-latch falling edge counts once, not once per idle clock.
            write(via, ViaRegister::PortB, ViaPb6, DeviceSetupTime);
            for (unsigned i = 0; i < DeselectedTimerClocks; ++i) {
                via.clock();
                ASSERT_EQ(via.state().t2.counter, TimerLoad);
            }
            write(via, ViaRegister::PortB, 0, DeviceSetupTime);
            ASSERT_EQ(via.state().t2.counter, TimerLoad - 1);
            write(via, ViaRegister::PortB, 0, DeviceSetupTime);
            for (unsigned i = 0; i < DeselectedTimerClocks; ++i) {
                via.clock();
                ASSERT_EQ(via.state().t2.counter, TimerLoad - 1);
            }
            EXPECT_EQ(via.peek(ViaRegister::Ifr) & M6522_IRQ_T2, NoInterrupts);
        }
    }
}

// Direction changes count only actual falling edges on the resolved PB6 pin.
TEST(ExternalVia, Pb6CountingTracksDirectionChangesAndInputPulses) {
    constexpr uint8_t TimerLoad = 9;
    Via6522 via;
    // Arm T2 with PB6 driven high despite an external low input.
    via.inputs([](auto& inputs) { inputs.port_b = 0; });
    write(via, ViaRegister::PortB, ViaPb6, DeviceSetupTime);
    write(via, ViaRegister::DdrB, ViaPb6, DeviceSetupTime);
    via.set_acr(ViaAcrBit::Timer2CountPb6, DeviceSetupTime);
    write(via, ViaRegister::Timer2Low, TimerLoad, DeviceSetupTime);
    write(via, ViaRegister::Timer2High, 0, DeviceSetupTime);
    ASSERT_EQ(via.state().t2.counter, TimerLoad);
    // Releasing the high output to an external low produces exactly one pulse.
    write(via, ViaRegister::DdrB, 0, DeviceSetupTime);
    ASSERT_EQ(via.state().t2.counter, TimerLoad - 1);
    for (unsigned i = 0; i < DeselectedTimerClocks; ++i) via.clock();
    EXPECT_EQ(via.state().t2.counter, TimerLoad - 1);
    // Returning to a high output is a rising edge, not a counted pulse.
    write(via, ViaRegister::DdrB, ViaPb6, DeviceSetupTime);
    EXPECT_EQ(via.state().t2.counter, TimerLoad - 1);
    // Releasing a high output to an external high must not count.
    via.inputs([](auto& inputs) { inputs.port_b = ViaPb6; });
    write(via, ViaRegister::DdrB, 0, DeviceSetupTime);
    EXPECT_EQ(via.state().t2.counter, TimerLoad - 1);
    // Input mode counts an external falling edge regardless of its output latch.
    via.inputs([](auto& inputs) { inputs.port_b = 0; });
    via.clock();
    EXPECT_EQ(via.state().t2.counter, TimerLoad - 2);
    for (unsigned i = 0; i < DeselectedTimerClocks; ++i) via.clock();
    EXPECT_EQ(via.state().t2.counter, TimerLoad - 2);
}

// Verify control-input IRQ acknowledgment and both fixed C2 output levels.
TEST(ExternalVia, ControlInputsUseTheirOwnEdgesAndFixedOutputsReachPins) {
    Via6522 via;
    via.clock();
    // Trigger and acknowledge only the port B control sources.
    write(via, ViaRegister::Ifr, ViaIrqSources, DeviceSetupTime);
    write(via, ViaRegister::Ier, ViaIerSet | ViaControlIrqs, DeviceSetupTime);
    via.inputs([](auto& inputs) { inputs.cb1 = inputs.cb2 = false; });
    via.clock();
    EXPECT_EQ(via.peek(ViaRegister::Ifr) & (M6522_IRQ_CB1 | M6522_IRQ_CB2),
        M6522_IRQ_CB1 | M6522_IRQ_CB2);
    EXPECT_TRUE(via.irq());
    read(via, ViaRegister::PortB, DeviceSetupTime);
    EXPECT_EQ(via.peek(ViaRegister::Ifr) & (M6522_IRQ_CB1 | M6522_IRQ_CB2), NoInterrupts);
    // Port A's no-handshake alias leaves CA2 pending until a normal read.
    via.inputs([](auto& inputs) { inputs.ca2 = false; });
    via.clock();
    EXPECT_EQ(via.peek(ViaRegister::Ifr) & M6522_IRQ_CA2, M6522_IRQ_CA2);
    read(via, ViaRegister::PortANoHandshake, DeviceSetupTime);
    EXPECT_EQ(via.peek(ViaRegister::Ifr) & M6522_IRQ_CA2, M6522_IRQ_CA2);
    read(via, ViaRegister::PortA, DeviceSetupTime);
    EXPECT_EQ(via.peek(ViaRegister::Ifr) & M6522_IRQ_CA2, NoInterrupts);
    // Fixed output modes override externally driven C2 levels.
    write(via, ViaRegister::Pcr, ViaPcrBothLow, DeviceSetupTime);
    EXPECT_FALSE(via.ca2());
    EXPECT_FALSE(via.cb2());
    write(via, ViaRegister::Pcr, ViaPcrBothHigh, DeviceSetupTime);
    EXPECT_TRUE(via.ca2());
    EXPECT_TRUE(via.cb2());
}

// Verify unsupported VIA modes/accesses fail without writes or state changes.
TEST(ExternalVia, UnsupportedModesFailBeforeChangingRegistersOrObservations) {
    Via6522 via;
    // Reject each unsupported ACR bit independently.
    for (const auto mode : {ViaAcrBit::LatchA, ViaAcrBit::LatchB,
        ViaAcrBit::ShiftBit0, ViaAcrBit::ShiftBit1,
        ViaAcrBit::ShiftBit2, ViaAcrBit::Timer1Pb7}) {
        EXPECT_THROW(via.set_acr(mode, DeviceSetupTime), std::logic_error);
        EXPECT_EQ(via.state().acr, ViaControlReset);
    }
    // Reject pulse outputs on either control port.
    for (uint8_t mode : {ViaPcrCa2Pulse, ViaPcrCb2Pulse}) {
        EXPECT_THROW(write(via, ViaRegister::Pcr, mode, DeviceSetupTime), std::logic_error);
        EXPECT_EQ(via.state().pcr, ViaControlReset);
    }
    // Reject shift accesses and out-of-range indices before recording writes.
    EXPECT_THROW(write(via, ViaRegister::Shift, 0, DeviceSetupTime), std::logic_error);
    EXPECT_THROW(via.peek(ViaRegister::Shift), std::logic_error);
    EXPECT_THROW(read(via, ViaRegister::Shift, DeviceSetupTime), std::logic_error);
    const auto invalid = static_cast<ViaRegister>(ViaRegisterMask + 1);
    EXPECT_THROW(via.peek(invalid), std::out_of_range);
    EXPECT_THROW(write(via, invalid, 0, DeviceSetupTime), std::out_of_range);
    EXPECT_EQ(via.writes().count(), 0);
}

// Check the private rejection mask against the supported modes for every byte.
TEST(ExternalVia, AcrValidationPreservesAllRegisterEncodings) {
    constexpr uint8_t SupportedBits = (ViaAcrBit::Timer1Continuous | ViaAcrBit::Timer2CountPb6).bits();
    for (unsigned value = 0; value <= MaximumRegisterValue; ++value) {
        SCOPED_TRACE(value);
        Via6522 via;
        const auto data = static_cast<uint8_t>(value);
        const auto before = model_values(via.state());
        // Unsupported bits must reject before the model or history advances.
        if (data & ~SupportedBits) {
            EXPECT_THROW(write(via, ViaRegister::Acr, data, DeviceSetupTime), std::logic_error);
            EXPECT_EQ(model_values(via.state()), before);
            EXPECT_EQ(via.writes().count(), 0);
        } else {
            EXPECT_NO_THROW(write(via, ViaRegister::Acr, data, DeviceSetupTime));
            EXPECT_EQ(via.state().acr, data);
            EXPECT_EQ(via.writes().count(), 1);
        }
    }
}

// Check upstream PCR predicates against independent register encodings for every byte.
TEST(ExternalVia, PcrValidationPreservesAllRegisterEncodings) {
    for (unsigned value = 0; value <= MaximumRegisterValue; ++value) {
        SCOPED_TRACE(value);
        Via6522 via;
        const auto data = static_cast<uint8_t>(value);
        const auto before = model_values(via.state());
        const bool pulse = (data & ViaPcrCa2ModeMask) == ViaPcrCa2Pulse
            || (data & ViaPcrCb2ModeMask) == ViaPcrCb2Pulse;
        // Either pulse field must reject regardless of the other control bits.
        if (pulse) {
            EXPECT_THROW(write(via, ViaRegister::Pcr, data, DeviceSetupTime), std::logic_error);
            EXPECT_EQ(model_values(via.state()), before);
            EXPECT_EQ(via.writes().count(), 0);
        } else {
            EXPECT_NO_THROW(write(via, ViaRegister::Pcr, data, DeviceSetupTime));
            EXPECT_EQ(via.state().pcr, data);
            EXPECT_EQ(via.writes().count(), 1);
        }
    }
}

// Verify both control edge polarities, independent CA2 IRQ and C2 handshakes.
TEST(ExternalVia, BothControlEdgePolaritiesIndependentFlagsAndHandshakeOutputs) {
    for (bool positive : {false, true}) {
        // Prime inactive levels, then trigger all four control input sources.
        Via6522 via;
        via.inputs([&](auto& inputs) {
            inputs.ca1 = inputs.ca2 = !positive;
            inputs.cb1 = inputs.cb2 = !positive;
        });
        via.clock();
        write(via, ViaRegister::Pcr,
            positive ? ViaPcrAllPositiveEdges : ViaControlReset, DeviceSetupTime);
        write(via, ViaRegister::Ifr, ViaIrqSources, DeviceSetupTime);
        write(via, ViaRegister::Ier, ViaIerSet | ViaControlIrqs, DeviceSetupTime);
        via.inputs([&](auto& inputs) {
            inputs.ca1 = inputs.ca2 = positive;
            inputs.cb1 = inputs.cb2 = positive;
        });
        via.clock();
        EXPECT_EQ(via.peek(ViaRegister::Ifr) & ViaControlIrqs, ViaControlIrqs);
        EXPECT_TRUE(via.irq());
        // Normal port reads acknowledge their respective control sources.
        read(via, ViaRegister::PortA, DeviceSetupTime);
        read(via, ViaRegister::PortB, DeviceSetupTime);
        EXPECT_EQ(via.peek(ViaRegister::Ifr) & ViaControlIrqs, NoInterrupts);
        EXPECT_FALSE(via.irq());
    }
    // Independent CA2 flags survive port reads until explicitly cleared.
    Via6522 via;
    via.clock();
    write(via, ViaRegister::Pcr, ViaPcrCa2IndependentIrq, DeviceSetupTime);
    via.inputs([](auto& inputs) { inputs.ca2 = false; });
    via.clock();
    read(via, ViaRegister::PortA, DeviceSetupTime);
    EXPECT_EQ(via.peek(ViaRegister::Ifr) & M6522_IRQ_CA2, M6522_IRQ_CA2);
    write(via, ViaRegister::Ifr, M6522_IRQ_CA2, DeviceSetupTime);
    EXPECT_EQ(via.peek(ViaRegister::Ifr) & M6522_IRQ_CA2, NoInterrupts);
    // Handshakes activate on normal accesses and release on C1 transitions.
    write(via, ViaRegister::Pcr, ViaPcrBothHandshake, DeviceSetupTime);
    read(via, ViaRegister::PortANoHandshake, DeviceSetupTime);
    EXPECT_TRUE(via.ca2());
    read(via, ViaRegister::PortA, DeviceSetupTime);
    write(via, ViaRegister::PortB, 0, DeviceSetupTime);
    EXPECT_FALSE(via.ca2());
    EXPECT_FALSE(via.cb2());
    via.inputs([](auto& inputs) { inputs.ca1 = inputs.cb1 = false; });
    via.clock();
    EXPECT_TRUE(via.ca2());
    EXPECT_TRUE(via.cb2());
}

// Verify fault-overlay IRQs do not mutate timer flags and survive reset.
TEST(ExternalVia, StuckTimerFlagIsAnExplicitFaultNotNormalTimerState) {
    // Enable the injected flag while leaving the underlying IFR clear.
    Via6522 via;
    via.set_timer1_fault(Via6522::Timer1Fault::StuckInterruptFlag);
    write(via, ViaRegister::Ier, ViaIerSet | M6522_IRQ_T1, DeviceSetupTime);
    write(via, ViaRegister::Ifr, ViaIrqSources, DeviceSetupTime);
    EXPECT_EQ(via.state().intr.ifr & M6522_IRQ_T1, NoInterrupts);
    EXPECT_EQ(via.peek(ViaRegister::Ifr) & ViaTimer1IrqStatus, ViaTimer1IrqStatus);
    EXPECT_TRUE(via.irq());
    // Acknowledgment cannot clear a stuck fault, and reset only gates delivery.
    read(via, ViaRegister::Timer1Low, DeviceSetupTime);
    EXPECT_EQ(via.peek(ViaRegister::Ifr) & M6522_IRQ_T1, M6522_IRQ_T1);
    via.reset();
    EXPECT_EQ(via.timer1_fault(), Via6522::Timer1Fault::StuckInterruptFlag);
    EXPECT_FALSE(via.irq());
    EXPECT_THROW(via.set_timer1_fault(static_cast<Via6522::Timer1Fault>(0xff)),
        std::invalid_argument);
    EXPECT_EQ(via.timer1_fault(), Via6522::Timer1Fault::StuckInterruptFlag);
    // Removing injection restores the underlying clear timer flag.
    via.set_timer1_fault(Via6522::Timer1Fault::None);
    EXPECT_EQ(via.peek(ViaRegister::Ifr) & M6522_IRQ_T1, NoInterrupts);
}

// Verify reset leaves timers quiet and maximum loads expire at the exact edge.
TEST(ExternalVia, ResetDisablesTimerInterruptsAndMaximumLoadsDoNotExpireEarly) {
    constexpr uint8_t MaximumTimerByte = MaximumTimerLoad & PortAllHigh;
    // Unloaded timers must not raise flags merely because clocks advance.
    Via6522 via;
    for (unsigned i = 0; i < IdleTimerClocks; ++i) via.clock();
    EXPECT_EQ(via.peek(ViaRegister::Ifr) & ViaTimerIrqs, NoInterrupts);
    // Each timer loaded to its maximum must wait through load-plus-one clocks.
    for (auto low : {ViaRegister::Timer1Low, ViaRegister::Timer2Low}) {
        via.reset();
        const auto high = low == ViaRegister::Timer1Low ? ViaRegister::Timer1High : ViaRegister::Timer2High;
        write(via, low, MaximumTimerByte, DeviceSetupTime);
        write(via, high, MaximumTimerByte, DeviceSetupTime);
        ASSERT_EQ(via.peek(ViaRegister::Ifr) & ViaTimerIrqs, NoInterrupts);
        for (unsigned i = 0; i < MaximumTimerPreExpiryClocks; ++i) {
            via.clock();
            ASSERT_EQ(via.peek(ViaRegister::Ifr) & ViaTimerIrqs, NoInterrupts)
                << "clock " << i + 1;
        }
        // The next clock sets only the loaded timer's interrupt source.
        via.clock();
        EXPECT_EQ(via.peek(ViaRegister::Ifr) & ViaTimerIrqs,
            low == ViaRegister::Timer1Low ? M6522_IRQ_T1 : M6522_IRQ_T2);
    }
}

// Distinguish absent history from zero writes on both PIAs and the VIA.
TEST(ExternalBus, OptionalWriteRecordsDistinguishZeroWritesAndRetainLatchTimestamps) {
    const econopet::CycleTime high{econopet::Cycles{10}};
    const econopet::CycleTime falling{econopet::Cycles{20}};
    const econopet::CycleTime later{econopet::Cycles{30}};
    Io devices;
    EXPECT_FALSE(devices.pia1().writes().last());
    EXPECT_FALSE(devices.pia2().writes().last());
    EXPECT_FALSE(devices.via().writes().last());
    // Exercise register zero and data zero on every fitted chip.
    for (const auto select : {ChipSelect::Pia1, ChipSelect::Pia2, ChipSelect::Via}) {
        devices.sample({true, false, select, 0, 0, true}, high);
        devices.sample({false, false, select, 0, 0, true}, falling);
    }
    devices.sample({true, false, ChipSelect::Pia1, 0, 0x42, true}, later);
    devices.sample({false, true, ChipSelect::Pia1, 0, 0x42, true}, later);
    EXPECT_EQ(devices.pia1().writes().last()->at, falling);
    EXPECT_EQ(devices.pia1().writes().last()->data, 0);
    const auto check = [&](auto& device) {
        const auto saved = device.writes().last();
        ASSERT_TRUE(saved);
        EXPECT_EQ(std::to_underlying(saved->reg), 0);
        EXPECT_EQ(saved->data, 0);
        EXPECT_EQ(saved->at, falling);
        EXPECT_EQ(device.writes().count(), 1);
        device.clock(typename std::decay_t<decltype(device)>::BusAccess{saved->reg, 0, false, later});
        device.reset();
        EXPECT_EQ(device.writes().last()->at, falling);
        EXPECT_EQ(device.writes().count(), 1);
        EXPECT_THROW(device.clock(typename std::decay_t<decltype(device)>::BusAccess{
            static_cast<typename std::decay_t<decltype(device)>::Register>(0xff), 0, true, later}),
            std::out_of_range);
        EXPECT_EQ(device.writes().last()->at, falling);
        device.clear_observations();
        EXPECT_FALSE(device.writes().last());
        EXPECT_EQ(device.writes().count(), 0);
        EXPECT_EQ(saved->at, falling);
        device.clock(typename std::decay_t<decltype(device)>::BusAccess{saved->reg, 0, true, later});
        EXPECT_EQ(device.writes().last()->at, later);
        EXPECT_EQ(device.writes().count(), 1);
    };
    check(devices.pia1());
    check(devices.pia2());
    check(devices.via());
}

// Verify the final stable bus is latched exactly once at each falling edge.
TEST(ExternalBus, WritesLatchOnceAtFallingPhi2UsingFinalStableData) {
    // Stable low and high samples alone must not complete an access.
    Io devices;
    BusSample bus{false, false, ChipSelect::Pia1,
        std::to_underlying(PiaRegister::ControlA), PiaPortAccess.bits(), true};
    for (unsigned i = 0; i < StableBusSamples; ++i) devices.sample(bus, DeviceSetupTime);
    EXPECT_EQ(devices.pia1().writes().count(), 0);
    bus.phi2 = true;
    for (unsigned i = 0; i < StableBusSamples; ++i) devices.sample(bus, DeviceSetupTime);
    EXPECT_EQ(devices.pia1().writes().count(), 0);
    // Change data before the falling edge and latch its final stable value.
    bus.data = PiaC2High.bits();
    devices.sample(bus, DeviceSetupTime);
    bus.phi2 = false;
    devices.sample(bus, DeviceSetupTime);
    EXPECT_EQ(devices.pia1().writes().count(), 1);
    EXPECT_EQ(devices.pia1().state().pa.cr, PiaC2High.bits());
    // Repeated low samples do not duplicate the write, but a new cycle does.
    for (unsigned i = 0; i < StableBusSamples; ++i) devices.sample(bus, DeviceSetupTime);
    EXPECT_EQ(devices.pia1().writes().count(), 1);
    bus.phi2 = true;
    devices.sample(bus, DeviceSetupTime);
    bus.phi2 = false;
    devices.sample(bus, DeviceSetupTime);
    EXPECT_EQ(devices.pia1().writes().count(), 2);
    EXPECT_EQ(devices.clock_count(), 2);
    ASSERT_TRUE(devices.pia1().writes().last());
    EXPECT_EQ(devices.pia1().writes().last()->data, PiaC2High.bits());
    // Simultaneous physical selects are invalid, even without a clock edge.
    EXPECT_THROW(devices.sample({false, false, ChipSelect::Pia1 | ChipSelect::Pia2,
        std::to_underlying(PiaRegister::PortA), 0, false}, DeviceSetupTime), std::logic_error);
}

// Reject the latched VIA access before any chip advances, including on retry.
TEST(ExternalBus, InvalidViaAccessPreservesAllDevicesAndRecovers) {
    constexpr uint8_t TimerLoad = 7;
    const std::array unsupported{
        BusSample{true, false, ChipSelect::Via, std::to_underlying(ViaRegister::Acr),
            std::to_underlying(ViaAcrBit::LatchA), true},
        BusSample{true, false, ChipSelect::Via, std::to_underlying(ViaRegister::Pcr), ViaPcrCa2Pulse, true},
        BusSample{true, false, ChipSelect::Via, std::to_underlying(ViaRegister::Pcr), ViaPcrCb2Pulse, true},
        BusSample{true, false, ChipSelect::Via, std::to_underlying(ViaRegister::Shift), 0, true},
        BusSample{true, false, ChipSelect::Via, std::to_underlying(ViaRegister::Shift), 0, false},
        BusSample{true, false, ChipSelect::Via, ViaRegisterMask + 1, 0, true},
        BusSample{true, false, ChipSelect::Via, ViaRegisterMask + 1, 0, false},
    };
    const econopet::CycleTime high{econopet::Cycles{10}};
    const econopet::CycleTime falling{econopet::Cycles{20}};
    const econopet::CycleTime recovered{econopet::Cycles{30}};
    for (const auto& invalid : unsupported) {
        SCOPED_TRACE(testing::Message() << "register " << unsigned(invalid.reg)
            << ", data " << unsigned(invalid.data) << ", write " << invalid.write);
        Io devices;
        // Prime both PIAs, then leave enabled falling edges waiting for a clock.
        for (auto* pia : {&devices.pia1(), &devices.pia2()}) {
            pia->clock();
            pia->set_control(PiaRegister::ControlA, PiaPortAccess | PiaControlBit::C1IrqEnable, DeviceSetupTime);
            pia->inputs([](auto& inputs) { inputs.ca1 = false; });
            ASSERT_FALSE(pia->irq());
        }
        // Arm a VIA timer and preserve only bus-level write observations.
        write(devices.via(), ViaRegister::Timer1Low, TimerLoad, DeviceSetupTime);
        write(devices.via(), ViaRegister::Timer1High, 0, DeviceSetupTime);
        devices.clear_observations();
        const auto pia1 = model_values(devices.pia1().state());
        const auto pia2 = model_values(devices.pia2().state());
        const auto via = model_values(devices.via().state());
        const auto unchanged = [&] {
            EXPECT_EQ(model_values(devices.pia1().state()), pia1);
            EXPECT_EQ(model_values(devices.pia2().state()), pia2);
            EXPECT_EQ(model_values(devices.via().state()), via);
            EXPECT_FALSE(devices.irq());
            EXPECT_EQ(devices.clock_count(), 0);
            EXPECT_EQ(devices.pia1().writes().count(), 0);
            EXPECT_EQ(devices.pia2().writes().count(), 0);
            EXPECT_EQ(devices.via().writes().count(), 0);
            EXPECT_FALSE(devices.pia1().writes().last());
            EXPECT_FALSE(devices.pia2().writes().last());
            EXPECT_FALSE(devices.via().writes().last());
        };
        // Falling-edge levels intentionally differ from the latched access.
        const BusSample low{false, false, ChipSelect::None, 0, 0, false};
        devices.sample(invalid, high);
        unchanged();
        EXPECT_THROW(devices.sample(low, falling), std::logic_error);
        unchanged();
        // An unchanged retry must still reject the pending access without ticking.
        EXPECT_THROW(devices.sample(low, recovered), std::logic_error);
        unchanged();
        // Replace the pending high sample, then complete exactly one valid access.
        devices.sample({true, false, ChipSelect::Via, std::to_underlying(ViaRegister::Acr),
            ViaControlReset, true}, falling);
        unchanged();
        devices.sample(low, recovered);
        EXPECT_TRUE(devices.pia1().irq());
        EXPECT_TRUE(devices.pia2().irq());
        EXPECT_EQ(devices.clock_count(), 1);
        ASSERT_TRUE(devices.via().writes().last());
        EXPECT_EQ(devices.via().writes().last()->reg, ViaRegister::Acr);
        EXPECT_EQ(devices.via().writes().last()->data, ViaControlReset);
        EXPECT_EQ(devices.via().writes().last()->at, recovered);
        EXPECT_EQ(devices.via().writes().count(), 1);
        EXPECT_EQ(devices.pia1().writes().count(), 0);
        EXPECT_EQ(devices.pia2().writes().count(), 0);
        const auto recovered_pia1 = model_values(devices.pia1().state());
        const auto recovered_pia2 = model_values(devices.pia2().state());
        const auto recovered_via = model_values(devices.via().state());
        // Repeating the successful low sample cannot clock the devices again.
        devices.sample(low, recovered);
        EXPECT_EQ(devices.clock_count(), 1);
        EXPECT_EQ(devices.via().writes().count(), 1);
        EXPECT_EQ(model_values(devices.pia1().state()), recovered_pia1);
        EXPECT_EQ(model_values(devices.pia2().state()), recovered_pia2);
        EXPECT_EQ(model_values(devices.via().state()), recovered_via);
    }
}

// Reset cancels even an unsupported pending access without attempting validation.
TEST(ExternalBus, ResetCancelsInvalidPendingAccess) {
    Io devices;
    const BusSample invalid{true, false, ChipSelect::Via,
        std::to_underlying(ViaRegister::Shift), 0, true};
    devices.sample(invalid, DeviceSetupTime);
    auto reset = invalid;
    reset.phi2 = false;
    reset.reset = true;
    EXPECT_NO_THROW(devices.sample(reset, DeviceSetupTime));
    EXPECT_EQ(devices.clock_count(), 0);
    EXPECT_FALSE(devices.via().writes().last());
    // Release reset before starting a fresh physical cycle.
    devices.sample({false, false, ChipSelect::None, 0, 0, false}, DeviceSetupTime);
    devices.sample({true, false, ChipSelect::Pia1,
        std::to_underlying(PiaRegister::ControlA), PiaPortAccess.bits(), true}, DeviceSetupTime);
    devices.sample({false, false, ChipSelect::None, 0, 0, false}, DeviceSetupTime);
    EXPECT_EQ(devices.clock_count(), 1);
    EXPECT_EQ(devices.pia1().peek(PiaRegister::ControlA), PiaPortAccess.bits());
    EXPECT_EQ(devices.pia1().writes().count(), 1);
}

// Verify reset cancels an access but retains inputs, injected faults and history.
TEST(ExternalBus, ResetCancelsPendingAccessButPreservesInputsFaultsAndHistory) {
    // Complete one write and start a second access before asserting reset.
    Io devices;
    devices.pia1().inputs([](auto& inputs) { inputs.port_a = 0x42; });
    devices.via().set_timer1_fault(Via6522::Timer1Fault::StuckInterruptFlag);
    devices.sample({true, false, ChipSelect::Pia1, std::to_underlying(PiaRegister::ControlA), PiaPortAccess.bits(), true}, DeviceSetupTime);
    devices.sample({false, false, ChipSelect::Pia1, std::to_underlying(PiaRegister::ControlA), PiaPortAccess.bits(), true}, DeviceSetupTime);
    EXPECT_EQ(devices.pia1().writes().count(), 1);
    devices.sample({true, false, ChipSelect::Pia1, std::to_underlying(PiaRegister::PortA), PortAllHigh, true}, DeviceSetupTime);
    // Reset at the would-be latch boundary cancels the pending write.
    devices.sample({false, true, ChipSelect::Pia1, std::to_underlying(PiaRegister::PortA), PortAllHigh, true}, DeviceSetupTime);
    EXPECT_EQ(devices.pia1().state().pa.cr, PiaDdrAccess.bits());
    EXPECT_EQ(devices.pia1().state().pa.outr, OutputLatchReset);
    EXPECT_EQ(devices.pia1().peek_inputs().port_a, 0x42);
    EXPECT_EQ(devices.via().timer1_fault(), Via6522::Timer1Fault::StuckInterruptFlag);
    EXPECT_EQ(devices.pia1().writes().count(), 1);
    // Explicit history clearing leaves the external setup intact.
    devices.clear_observations();
    EXPECT_EQ(devices.pia1().writes().count(), 0);
    EXPECT_EQ(devices.pia1().peek_inputs().port_a, 0x42);
    EXPECT_EQ(devices.via().timer1_fault(), Via6522::Timer1Fault::StuckInterruptFlag);
}

// Verify neither peeks nor high-phase samples acknowledge a pending PIA IRQ.
TEST(ExternalBus, ReadsAcknowledgeOnlyOnceAtTheirCompletedLatchBoundary) {
    // Prime and trigger a falling-edge CA1 interrupt.
    Io devices;
    auto& pia = devices.pia1();
    pia.clock();
    pia.set_control(PiaRegister::ControlA, PiaPortAccess | PiaControlBit::C1IrqEnable, DeviceSetupTime);
    pia.inputs([](auto& inputs) { inputs.ca1 = false; });
    pia.clock();
    ASSERT_TRUE(pia.irq());
    // Keep the read pending throughout high PHI2, despite repeated peeks.
    for (unsigned i = 0; i < StableBusSamples; ++i) {
        pia.peek(PiaRegister::PortA);
        devices.sample({true, false, ChipSelect::Pia1, std::to_underlying(PiaRegister::PortA), 0, false}, DeviceSetupTime);
        EXPECT_TRUE(pia.irq());
    }
    // Only the completed falling-edge read clears IRQ, without adding a write.
    devices.sample({false, false, ChipSelect::Pia1, std::to_underlying(PiaRegister::PortA), 0, false}, DeviceSetupTime);
    EXPECT_FALSE(pia.irq());
    EXPECT_EQ(devices.clock_count(), 1);
    EXPECT_EQ(pia.writes().count(), 1);
}

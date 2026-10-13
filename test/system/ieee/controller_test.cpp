// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#include <type_traits>

#include <gmock/gmock.h>

#include "controller.h"
#include "fifo.h"
#include "framework/system_test.h"
#include "scheduler.h"
#include "trace.h"

namespace {
using namespace ieee_test;
using econopet::Cycles;
using testing::ElementsAre;
using testing::HasSubstr;

static_assert(!std::is_constructible_v<System::IeeeView<System>, System&&>);
static_assert(!std::is_constructible_v<System::IeeeView<const System>, const System&&>);
static_assert(!std::is_copy_constructible_v<IeeeController>);
static_assert(!std::is_move_constructible_v<IeeeController>);

struct InterruptedTransfer {};

// A completed bus write does not make an interrupted mailbox safe to reuse.
TEST_F(SystemTest, IeeeControllerRejectsReuseAfterInterruptedBusOperation) {
    constexpr econopet::CpuAddress write_address{0x0400};
    constexpr econopet::CpuAddress read_address{0x0401};
    constexpr uint8_t written = 0x11;
    constexpr uint8_t unread = 0x77;
    IeeeController controller(system);
    system.poke(econopet::SramAddress{write_address.value()}, 0);
    system.poke(econopet::SramAddress{read_address.value()}, unread);
    auto interrupt = system.observe([&](const System& observed) {
        if (observed.peek(econopet::SramAddress{write_address.value()}) == written)
            throw InterruptedTransfer{};
    });
    EXPECT_THROW(controller.write(write_address, written), InterruptedTransfer);
    interrupt.reset();
    ASSERT_EQ(system.peek(econopet::SramAddress{write_address.value()}), written);
    EXPECT_TRUE(controller.peek_failed());

    // Every active entry point rejects reuse before patching memory or clocking.
    const auto stopped = system.time();
    EXPECT_THROW(controller.read(read_address), std::logic_error);
    EXPECT_THROW(controller.write(read_address, 0), std::logic_error);
    EXPECT_THROW(controller.command(Unlisten), std::logic_error);
    EXPECT_THROW(controller.data('A'), std::logic_error);
    EXPECT_THROW(controller.receive(), std::logic_error);
    EXPECT_EQ(system.time(), stopped);
    EXPECT_EQ(system.peek(econopet::SramAddress{read_address.value()}), unread);

    // Letting the old CPU command finish is not an explicit controller recovery.
    system.tick(ByteBudget);
    EXPECT_THROW(controller.read(read_address), std::logic_error);
    EXPECT_TRUE(controller.peek_failed());
}

// Independently drive a physical listener that is not ready until a later boundary.
TEST_F(SystemTest, IeeeControllerWaitsForListenerReadinessBeforeDav) {
    IeeeController controller(system);
    system.io().via().inputs([](auto& inputs) {
        inputs.port_b &= static_cast<uint8_t>(~(pet::NrfdInput | pet::Ndac));
    });
    const auto start = system.time();
    const auto ready_at = start + Cycles{20000};
    const auto accepted_at = start + Cycles{30000};
    const auto rearmed_at = start + Cycles{40000};
    bool premature_dav = false;
    auto observer = system.observe([&](const System& observed) {
        if (!(observed.time() > ready_at) && !observed.snapshot().ieee_bus_dav_no)
            premature_dav = true;
    });
    Scheduler scheduler(system);
    scheduler.at(ready_at, [](System& board) {
        board.io().via().inputs([](auto& inputs) { inputs.port_b |= pet::NrfdInput; });
    });
    scheduler.at(accepted_at, [](System& board) {
        EXPECT_FALSE(board.snapshot().ieee_bus_dav_no);
        EXPECT_EQ(board.snapshot().ieee_bus_dio_o, static_cast<uint8_t>(~'A'));
        board.io().via().inputs([](auto& inputs) { inputs.port_b |= pet::Ndac; });
    });
    scheduler.at(rearmed_at, [](System& board) {
        EXPECT_TRUE(board.snapshot().ieee_bus_dav_no);
        board.io().via().inputs([](auto& inputs) {
            inputs.port_b &= static_cast<uint8_t>(~pet::Ndac);
        });
    });
    controller.data('A');
    EXPECT_FALSE(premature_dav);
    EXPECT_FALSE(system.time() < rearmed_at);
    EXPECT_FALSE(controller.peek_failed());
}

// A handshake deadline invalidates the controller even between mailbox commands.
TEST_F(SystemTest, IeeeControllerRejectsReuseAfterInterruptedHandshake) {
    IeeeController controller(system);
    system.io().via().inputs([](auto& inputs) {
        inputs.port_b &= static_cast<uint8_t>(~(pet::NrfdInput | pet::Ndac));
    });
    EXPECT_THROW(system.service_until([](const System&) { return false; },
        [&](System&, Cycles) { controller.data('A'); },
        Cycles{20000}, "listener never ready"), std::runtime_error);
    EXPECT_TRUE(controller.peek_failed());
    EXPECT_TRUE(system.snapshot().ieee_bus_dav_no);
    const auto stopped = system.time();
    EXPECT_THROW(controller.read(pet::ViaPortB), std::logic_error);
    EXPECT_EQ(system.time(), stopped);
}

// Captured sequence checks have no board effects and FIFO sampling consumes only on request.
TEST_F(SystemTest, IeeeTransactionsSeparateWireCaptureFromFifoConsumption) {
    IeeeController controller(system);
    system.ieee().write_control(fpga::IeeeControlBit::Enabled);
    auto wire = system.trace_ieee();
    controller.command(Listen(Device{8}));
    controller.data('A');
    controller.data('B', Eoi::Last);
    controller.command(Unlisten);
    const std::vector<Record> expected{
        command(Listen(Device{8})), data('A'), data('B', Eoi::Last), command(Unlisten)};
    const auto before = system.time();
    require_sequence(wire.records(), expected, "wire");
    EXPECT_EQ(system.time(), before);
    EXPECT_TRUE(system.snapshot().ieee_rx_ready_o);
    EXPECT_FALSE(rx_empty_now(system));
    EXPECT_TRUE(system.snapshot().ieee_rx_ready_o);
    const auto head = take_rx(system);
    ASSERT_TRUE(head);
    EXPECT_EQ(*head, fifo_record(command(Listen(Device{8}))));
    const auto rest = drain_rx(system, 3, ByteBudget, "remaining");
    EXPECT_THAT(rest, ElementsAre(fifo_record(data('A')), fifo_record(data('B', Eoi::Last)),
        fifo_record(command(Unlisten))));
    EXPECT_FALSE(take_rx(system));
    EXPECT_TRUE(rx_empty_now(system));
    ASSERT_EQ(wire.times().size(), expected.size());
    for (size_t index = 1; index < wire.times().size(); ++index)
        EXPECT_LT(wire.times()[index - 1], wire.times()[index]);
    require_sequence(wire.records(), expected, "survives draining");
}

// Raw malformed-command tests remain possible, but unobserved EOI cannot drive pins.
TEST_F(SystemTest, IeeeTransactionsRawStimulusRejectsUnknownEoiBeforeClocking) {
    IeeeController controller(system);
    system.ieee().write_control(fpga::IeeeControlBit::Enabled);
    const auto before = system.time();
    EXPECT_THROW(controller.raw_send(fifo_record(data('A'))), std::invalid_argument);
    EXPECT_EQ(system.time(), before);
    auto wire = system.trace_ieee();
    controller.raw_send(raw_record(0x19, true, Eoi::Last));
    EXPECT_THAT(wire.records(), ElementsAre(raw_record(0x19, true, Eoi::Last)));
}

// Device-to-controller EOI comes from the real wire, not FIFO status.
TEST_F(SystemTest, IeeeTransactionsReceivePreservesByteOrderAndFinalWireEoi) {
    IeeeController controller(system);
    system.ieee().write_control(fpga::IeeeControlBit::Enabled);
    auto wire = system.trace_ieee();
    controller.command(Talk(Device{8}));
    controller.command(Secondary(Channel{0}));
    EXPECT_TRUE(system.ieee().peek_talking());

    std::vector<bool> attention_edges;
    bool previous_atn = system.snapshot().ieee_bus_atn_no;
    bool sampled_data = false;
    bool sampled_while_ready = false;
    auto handshake = system.observe([&](const System& observed) {
        const auto pins = observed.snapshot();
        if (pins.ieee_bus_atn_no != previous_atn) {
            attention_edges.push_back(pins.ieee_bus_atn_no);
            previous_atn = pins.ieee_bus_atn_no;
        }
        if (pins.cpu_clock_o && pins.cpu_we_n_o && pins.cpu_addr_o == pet::Pia2PortA) {
            sampled_data = true;
            sampled_while_ready |= pins.ieee_cpu_nrfd_no;
        }
    });
    // Queue both bytes while ATN still inhibits the source handshake.
    system.spi().write(fpga::Register::IeeeTx, 'X');
    system.spi().write(fpga::Register::IeeeTxLast, 'Y');
    EXPECT_EQ(controller.receive(), data('X'));
    EXPECT_FALSE(system.ieee().peek_nrfd_released());
    EXPECT_FALSE(system.ieee().peek_ndac_released());
    EXPECT_FALSE(system.io().pia2().ca2());
    // Unrelated SPI clocks must not start another byte while the acceptor is busy.
    EXPECT_TRUE(system.ieee().read_status().contains(fpga::IeeeStatusBit::Talking));
    EXPECT_THAT(wire.records(), ElementsAre(command(Talk(Device{8})),
        command(Secondary(Channel{0})), data('X')));
    EXPECT_EQ(controller.receive(), data('Y', Eoi::Last));
    EXPECT_FALSE(system.ieee().peek_nrfd_released());
    EXPECT_FALSE(system.ieee().peek_ndac_released());
    EXPECT_FALSE(system.io().pia2().ca2());
    EXPECT_TRUE(sampled_data);
    EXPECT_FALSE(sampled_while_ready);
    EXPECT_THAT(attention_edges, ElementsAre(true));
    controller.command(Untalk);
    EXPECT_FALSE(system.ieee().peek_talking());
    EXPECT_THAT(attention_edges, ElementsAre(true, false));

    // Passive wire capture includes received data, while RX stores commands only.
    require_sequence(wire.records(), {command(Talk(Device{8})),
        command(Secondary(Channel{0})), data('X'), data('Y', Eoi::Last),
        command(Untalk)}, "received wire bytes");
    require_drained_sequence(system, fifo_records({command(Talk(Device{8})),
        command(Secondary(Channel{0})), command(Untalk)}), ByteBudget, "received commands");
}

// A separately timed physical source checks acceptance without the FPGA talker.
TEST_F(SystemTest, IeeeControllerReceiveRearmsBeforeTheNextPhysicalByte) {
    IeeeController controller(system);
    auto wire = system.trace_ieee();
    Scheduler scheduler(system);
    const auto start = system.time();
    constexpr Cycles source_period{20000};
    scheduler.at(start + source_period, [](System& board) {
        EXPECT_TRUE(board.ieee().peek_nrfd_released());
        EXPECT_FALSE(board.ieee().peek_ndac_released());
        board.io().pia2().inputs([](auto& inputs) { inputs.port_a = static_cast<uint8_t>(~'X'); });
        board.io().via().inputs([](auto& inputs) {
            inputs.port_b &= static_cast<uint8_t>(~pet::Dav);
        });
    });
    scheduler.at(start + source_period * 2, [](System& board) {
        EXPECT_FALSE(board.ieee().peek_nrfd_released());
        EXPECT_TRUE(board.ieee().peek_ndac_released());
        board.io().via().inputs([](auto& inputs) { inputs.port_b |= pet::Dav; });
    });
    scheduler.at(start + source_period * 3, [](System& board) {
        EXPECT_TRUE(board.ieee().peek_nrfd_released());
        EXPECT_FALSE(board.ieee().peek_ndac_released());
        board.io().pia2().inputs([](auto& inputs) { inputs.port_a = static_cast<uint8_t>(~'Y'); });
        board.io().pia1().inputs([](auto& inputs) {
            inputs.port_a &= static_cast<uint8_t>(~EoiInput);
        });
        board.io().via().inputs([](auto& inputs) {
            inputs.port_b &= static_cast<uint8_t>(~pet::Dav);
        });
    });
    scheduler.at(start + source_period * 4, [](System& board) {
        EXPECT_FALSE(board.ieee().peek_nrfd_released());
        EXPECT_TRUE(board.ieee().peek_ndac_released());
        board.io().via().inputs([](auto& inputs) { inputs.port_b |= pet::Dav; });
    });

    EXPECT_EQ(controller.receive(), data('X'));
    EXPECT_THAT(wire.records(), ElementsAre(data('X')));
    EXPECT_FALSE(system.ieee().peek_nrfd_released());
    EXPECT_FALSE(system.ieee().peek_ndac_released());
    EXPECT_EQ(controller.receive(), data('Y', Eoi::Last));
    EXPECT_THAT(wire.records(), ElementsAre(data('X'), data('Y', Eoi::Last)));
    EXPECT_FALSE(system.ieee().peek_nrfd_released());
    EXPECT_FALSE(system.ieee().peek_ndac_released());
    EXPECT_FALSE(controller.peek_failed());
}

// A source that never releases DAV cannot be treated as rearmed or reusable.
TEST_F(SystemTest, IeeeControllerReceiveRetainsFailureWhileWaitingForDavRelease) {
    IeeeController controller(system);
    system.io().pia2().inputs([](auto& inputs) { inputs.port_a = static_cast<uint8_t>(~'X'); });
    system.io().via().inputs([](auto& inputs) {
        inputs.port_b &= static_cast<uint8_t>(~pet::Dav);
    });
    EXPECT_THROW(system.service_until([](const System&) { return false; },
        [&](System&, Cycles) { controller.receive(); },
        Cycles{30000}, "source never releases DAV"), std::runtime_error);
    EXPECT_TRUE(controller.peek_failed());
    EXPECT_FALSE(system.ieee().peek_nrfd_released());
    EXPECT_TRUE(system.ieee().peek_ndac_released());
    const auto stopped = system.time();
    EXPECT_THROW(controller.receive(), std::logic_error);
    EXPECT_THROW(controller.command(Untalk), std::logic_error);
    EXPECT_EQ(system.time(), stopped);
}

// Switching through command mode preserves the next unread byte and its EOI.
TEST_F(SystemTest, IeeeControllerReceiveContinuesAcrossUntalkAndTalk) {
    IeeeController controller(system);
    system.ieee().write_control(fpga::IeeeControlBit::Enabled);
    auto wire = system.trace_ieee();
    controller.command(Talk(Device{8}));
    controller.command(Secondary(Channel{0}));
    system.spi().write(fpga::Register::IeeeTx, 'X');
    system.spi().write(fpga::Register::IeeeTxLast, 'Y');
    EXPECT_EQ(controller.receive(), data('X'));
    controller.command(Untalk);
    EXPECT_FALSE(system.ieee().peek_talking());
    controller.command(Talk(Device{8}));
    controller.command(Secondary(Channel{0}));
    EXPECT_EQ(controller.receive(), data('Y', Eoi::Last));
    EXPECT_FALSE(system.ieee().peek_nrfd_released());
    EXPECT_FALSE(system.ieee().peek_ndac_released());
    EXPECT_THAT(wire.records(), ElementsAre(command(Talk(Device{8})),
        command(Secondary(Channel{0})), data('X'), command(Untalk),
        command(Talk(Device{8})), command(Secondary(Channel{0})), data('Y', Eoi::Last)));
    EXPECT_FALSE(controller.peek_failed());
}

// Shared active assertions reject missing and trailing records without hiding flags.
TEST_F(SystemTest, IeeeTransactionsDrainingAssertionsAreExactAndBounded) {
    IeeeController controller(system);
    system.ieee().write_control(fpga::IeeeControlBit::Enabled);
    EXPECT_THROW(drain_rx(system, 0, ByteBudget, "bad limit"), std::invalid_argument);
    EXPECT_THROW(require_drained_sequence(system, fifo_records({command(Unlisten)}),
        ByteBudget, "missing"), std::runtime_error);
    controller.command(Listen(Device{8}));
    controller.data('A');
    EXPECT_THROW(drain_rx(system, 1, ByteBudget, "bounded"), std::runtime_error);
    EXPECT_EQ(take_rx(system), std::optional<Record>{fifo_record(data('A'))});
    controller.command(Unlisten);
    EXPECT_THROW(require_drained_sequence(system, {}, ByteBudget, "trailing"), std::runtime_error);
    EXPECT_TRUE(rx_empty_now(system));
    controller.command(Unlisten);
    controller.command(Untalk);
    try {
        require_drained_sequence(system, {}, ByteBudget, "two trailing");
        FAIL() << "accepted surplus FIFO records";
    } catch (const std::runtime_error& error) {
        EXPECT_THAT(error.what(), HasSubstr("index=0"));
        EXPECT_THAT(error.what(), HasSubstr("byte=0x3f ATN=1 EOI=unobserved"));
    }
    EXPECT_EQ(take_rx(system), std::optional<Record>{fifo_record(command(Untalk))});
}

// A drain's nested status/data/acknowledgment clocks share the entire operation budget.
TEST_F(SystemTest, IeeeTransactionsDrainHonorsDeadline) {
    IeeeController controller(system);
    system.ieee().write_control(fpga::IeeeControlBit::Enabled);
    controller.command(Unlisten);
    const auto before = system.time();
    EXPECT_THROW(drain_rx(system, 2, Cycles{1}, "tiny deadline"), std::runtime_error);
    EXPECT_FALSE(system.time() > before + Cycles{1});
    EXPECT_TRUE(system.spi().peek_failed());
}

// Quiet intervals include their endpoints and never consume an existing record.
TEST_F(SystemTest, IeeeTransactionsEmptyNowIsNotAQuietInterval) {
    IeeeController controller(system);
    system.ieee().write_control(fpga::IeeeControlBit::Enabled);
    EXPECT_TRUE(rx_empty_now(system));
    const auto before = system.time();
    require_rx_remains_empty(system, Cycles{100}, "quiet");
    EXPECT_EQ(system.time(), before + Cycles{100});
    require_rx_remains_empty(system, Cycles{0}, "empty endpoint");
    controller.command(Unlisten);
    EXPECT_THROW(require_rx_remains_empty(system, Cycles{0}, "nonempty endpoint"), std::runtime_error);
    EXPECT_TRUE(system.snapshot().ieee_rx_ready_o);
    EXPECT_EQ(take_rx(system), std::optional<Record>{fifo_record(command(Unlisten))});
}

// A future CPU DAV edge invalidates a quiet interval even after an empty SPI sample.
TEST_F(SystemTest, IeeeTransactionsQuietIntervalDetectsLaterCpuTransfer) {
    IeeeController controller(system);
    system.ieee().write_control(fpga::IeeeControlBit::Enabled);
    system.cpu().assert_reset();
    test_asm::Scenario scenario(system);
    const auto code = scenario.reserve("delayed DAV", test_asm::RegionKind::Code,
        econopet::CpuAddress{0x0300}, 0x100);
    scenario.reserve("vectors", test_asm::RegionKind::Vectors, econopet::CpuAddress{0xfffa}, 6);
    test_asm::mos6502::Program program(scenario, code);
    program.lda(test_asm::imm(PiaC2High.bits())).sta(pet::Pia2ControlB);
    program.lda(test_asm::imm(static_cast<uint8_t>(BusReleased & ~pet::Atn))).sta(pet::ViaPortB);
    program.lda(test_asm::imm(static_cast<uint8_t>(~Unlisten.byte()))).sta(pet::Pia2PortB);
    // LDX #255, DEX, BNE -3 is an explicit fixed-length timing delay.
    const uint8_t delay[] = {0xa2, 0xff, 0xca, 0xd0, 0xfd};
    program.raw_bytes(delay);
    program.lda(test_asm::imm(PiaC2Low.bits())).sta(pet::Pia2ControlB);
    program.jmp(program.mark("park"));
    program.install();
    system.cpu(CPU_SOFT_6502).start(code.start());
    system.tick(Cycles{0});
    EXPECT_TRUE(rx_empty_now(system));
    EXPECT_THROW(require_rx_remains_empty(system, Cycles{100000}, "delayed CPU byte"), std::runtime_error);
    EXPECT_TRUE(system.snapshot().ieee_rx_ready_o);
}

// Subscriptions disconnect cleanly and do not invent in-flight transfers at attachment.
TEST_F(SystemTest, IeeeTransactionsWireObserversComposeAndDisconnect) {
    IeeeController controller(system);
    system.ieee().write_control(fpga::IeeeControlBit::Enabled);
    auto first = system.trace_ieee();
    {
        auto second = system.trace_ieee();
        controller.command(Unlisten);
        EXPECT_EQ(first.records(), second.records());
    }
    controller.command(Untalk);
    EXPECT_THAT(first.records(), ElementsAre(command(Unlisten), command(Untalk)));
    controller.write(pet::Pia2ControlB, PiaC2Low.bits());
    auto attached = system.trace_ieee();
    system.tick(100);
    EXPECT_TRUE(attached.records().empty());
    controller.write(pet::Pia2ControlB, PiaC2High.bits());
    controller.command(Unlisten);
    EXPECT_THAT(attached.records(), ElementsAre(command(Unlisten)));
}

// Initial attachment and edge detection must use the same resolved physical DAV.
TEST_F(SystemTest, IeeeWireTraceAttachesDuringPhysicalDavWithoutInventingAByte) {
    IeeeController controller(system);
    system.io().pia2().inputs([](auto& inputs) { inputs.port_a = static_cast<uint8_t>(~0xa5); });
    system.io().pia1().inputs([](auto& inputs) {
        inputs.port_a &= static_cast<uint8_t>(~EoiInput);
    });
    system.io().via().inputs([](auto& inputs) {
        inputs.port_b &= static_cast<uint8_t>(~pet::Dav);
    });
    auto wire = system.trace_ieee();
    system.tick(10);
    EXPECT_TRUE(wire.records().empty());

    // A release followed by a new assertion creates exactly one physical byte.
    system.io().via().inputs([](auto& inputs) { inputs.port_b |= pet::Dav; });
    system.tick(1);
    system.io().via().inputs([](auto& inputs) {
        inputs.port_b &= static_cast<uint8_t>(~pet::Dav);
    });
    system.tick(10);
    EXPECT_THAT(wire.records(), ElementsAre(data(0xa5, Eoi::Last)));
    EXPECT_TRUE(wire.capture().complete());
}

// Any participant may hold a wire low, regardless of another driver's release.
TEST_F(SystemTest, IeeeWireTraceResolvesPhysicalInputsAndOverlappingDavDrivers) {
    IeeeController controller(system);
    controller.write(pet::Pia2ControlB, PiaC2High.bits());
    controller.write(pet::Pia2PortB, 0xf0);
    system.io().pia2().inputs([](auto& inputs) {
        inputs.port_a = 0x0f;
        inputs.ca1 = false;
    });
    system.io().pia1().inputs([](auto& inputs) {
        inputs.port_a &= static_cast<uint8_t>(~EoiInput);
    });
    auto wire = system.trace_ieee();
    controller.write(pet::Pia2ControlB, PiaC2Low.bits());
    EXPECT_THAT(wire.records(), ElementsAre(raw_record(0xff, true, Eoi::Last)));

    // Hand off DAV while it remains asserted, without inventing assertion edges.
    system.io().via().inputs([](auto& inputs) {
        inputs.port_b &= static_cast<uint8_t>(~pet::Dav);
    });
    system.tick(1);
    controller.write(pet::Pia2ControlB, PiaC2High.bits());
    controller.write(pet::Pia2ControlB, PiaC2Low.bits());
    system.io().via().inputs([](auto& inputs) { inputs.port_b |= pet::Dav; });
    system.tick(1);
    EXPECT_THAT(wire.records(), ElementsAre(raw_record(0xff, true, Eoi::Last)));

    // Only a complete bus release permits another byte's DAV assertion.
    controller.write(pet::Pia2ControlB, PiaC2High.bits());
    system.io().via().inputs([](auto& inputs) {
        inputs.port_b &= static_cast<uint8_t>(~pet::Dav);
    });
    system.tick(1);
    EXPECT_THAT(wire.records(), ElementsAre(raw_record(0xff, true, Eoi::Last),
        raw_record(0xff, true, Eoi::Last)));
}

// The fitted PIA can assert DAV in handshake mode without a fabric DAV edge.
TEST_F(SystemTest, IeeeWireTraceObservesFittedPiaHandshakeOutput) {
    IeeeController controller(system);
    controller.write(pet::Pia2ControlB, econopet::io::PiaDdrAccess.bits());
    controller.write(pet::Pia2PortB, BusReleased);
    controller.write(pet::Pia2ControlB, econopet::io::PiaC2Handshake.bits());
    auto wire = system.trace_ieee();
    controller.write(pet::Pia2PortB, static_cast<uint8_t>(~'R'));
    EXPECT_FALSE(system.io().pia2().cb2());
    EXPECT_TRUE(system.snapshot().ieee_bus_dav_no);
    EXPECT_THAT(wire.records(), ElementsAre(data('R')));
}

// Model outputs drive the bus only where DDR selects output, not from idle latches.
TEST_F(SystemTest, IeeeWireTraceResolvesOnlyEnabledFittedGpioOutputs) {
    using econopet::io::PiaRegister;
    using econopet::io::ViaRegister;
    using econopet::io::write;
    IeeeController controller(system);
    auto& pia = system.io().pia2();
    auto& via = system.io().via();
    const auto setup = system.time();

    // Configure fitted outputs independently of the fabric's snooped registers.
    pia.set_control(PiaRegister::ControlB, econopet::io::PiaDdrAccess, setup);
    write(pia, PiaRegister::PortB, BusReleased, setup);
    pia.set_control(PiaRegister::ControlB, PiaC2High, setup);
    write(pia, PiaRegister::PortB, static_cast<uint8_t>(~'R'), setup);
    write(via, ViaRegister::DdrB, pet::Atn, setup);
    write(via, ViaRegister::PortB, static_cast<uint8_t>(BusReleased & ~pet::Atn), setup);
    ASSERT_EQ(system.snapshot().ieee_bus_dio_o, BusReleased);
    ASSERT_TRUE(system.snapshot().ieee_bus_atn_no);
    auto wire = system.trace_ieee();
    pia.set_control(PiaRegister::ControlB, PiaC2Low, system.time());
    system.tick(1);
    EXPECT_THAT(wire.records(), ElementsAre(raw_record('R', true, Eoi::More)));

    // Changing direction releases DIO and ATN while preserving the output latches.
    pia.set_control(PiaRegister::ControlB, PiaC2High, system.time());
    system.tick(1);
    pia.set_control(PiaRegister::ControlB, econopet::io::PiaDdrAccess, system.time());
    write(pia, PiaRegister::PortB, 0, system.time());
    pia.set_control(PiaRegister::ControlB, PiaC2High, system.time());
    write(via, ViaRegister::DdrB, 0, system.time());
    pia.set_control(PiaRegister::ControlB, PiaC2Low, system.time());
    system.tick(1);
    EXPECT_THAT(wire.records(), ElementsAre(raw_record('R', true, Eoi::More), data(0)));
}

} // namespace

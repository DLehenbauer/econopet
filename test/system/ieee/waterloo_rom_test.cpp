// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <cstdint>
#include <stdexcept>
#include <string>

#include <gmock/gmock.h>

#include "fifo.h"
#include "fixture.h"
#include "framework/asm.h"
#include "framework/firmware_test.h"
#include "trace.h"

extern "C" {
#include "ieee/ieee_protocol.h"
}

namespace {
using Program = test_asm::mc6809::Program<System>;
using test_asm::imm;
using namespace ieee_test;

constexpr auto ControlBlock = WaterlooWorkspace::Block;
enum class FcbKind { Ieee, Disk };
constexpr uint8_t AbsentDeviceStatus = 0x03;
constexpr econopet::Cycles ServiceSliceCycles = 1000;
constexpr uint8_t ViaExternalIdle = pet::Ndac | pet::NrfdInput | pet::Dav;
// The RED fixture historically writes bit 3, reserved by the current control register.
constexpr uint8_t LegacyControlBit3 = 0x08;

// Share firmware ownership, seed reporting and failure diagnostics across ROM cases.
class WaterlooRomIeee : public FirmwareTest {
protected:
    // Enable the FPGA peripheral without implicitly running Waterloo initialization.
    void SetUp() override {
        FirmwareTest::SetUp();
        firmware.mount(disk_fixture::D64::empty(), "kernal.d64");
    }
};

// Executes Waterloo's IEEEInit through the production soft 6809 and PIA/VIA pins.
TEST_F(WaterlooRomIeee, waterloo_ieee_init_case) {
    WaterlooFixture rom(system);
    // Call the ROM initializer with a private RAM stack and completion marker.
    auto test = rom.program();
    // Require the initializer's return and exact idle VIA output state.
    test.within(WaterlooBudget).run([&](auto& p) {
        p.jsr(rom.entry(WaterlooRoutine::IeeeInit));
    });
    EXPECT_EQ(system.ieee().peek_via_orb(), ViaIdle);
}

// Verifies Waterloo's ATNDown_ VIA polarity after ROM initialization.
TEST_F(WaterlooRomIeee, waterloo_atn_down_case) {
    WaterlooFixture rom(system);
    system.io().via().inputs([](auto& inputs) { inputs.port_b = ViaExternalIdle; });

    auto test = rom.program();
    test.within(WaterlooBudget).run([&](auto& p) {
        rom.init_ieee(p);
        p.jsr(rom.entry(WaterlooRoutine::AtnDown));
    });
    EXPECT_EQ(system.ieee().peek_via_orb() & ViaIdle, pet::Nrfd);
    EXPECT_NE(system.io().pia2().state().pa.cr & M6520_CR_C2_MODE, 0);
}

// Verifies Waterloo's ATNUp_ precondition and VIA polarity after ROM initialization.
TEST_F(WaterlooRomIeee, waterloo_atn_up_case) {
    WaterlooFixture rom(system);
    // Establish ATNUp_'s required asserted-ATN precondition after IEEEInit.
    auto test = rom.program();
    test.within(WaterlooBudget).run([&](auto& p) {
        rom.init_ieee(p);
        p.ldb(imm(pet::Nrfd)).stb(pet::ViaPortB).jsr(rom.entry(WaterlooRoutine::AtnUp));
    });
    EXPECT_NE(system.ieee().peek_via_orb() & pet::Atn, 0);
}

// Verifies Waterloo's EOIDown_/EOIUp_ PIA1 control-register transition.
TEST_F(WaterlooRomIeee, waterloo_eoi_cases) {
    WaterlooFixture rom(system);

    // First check assertion alone, then exercise assertion and release together.
    auto asserted = rom.program();
    asserted.within(WaterlooBudget).run([&](auto& p) {
        rom.init_ieee(p);
        p.jsr(rom.entry(WaterlooRoutine::EoiDown));
    });
    EXPECT_EQ(system.io().pia1().state().pa.cr & M6520_CR_C2_MODE, 0);

    auto test = rom.program();
    const auto intermediate = test.byte("EOIDown returned");
    // Check both call markers and the released PIA1 EOI output.
    const auto execution = test.within(WaterlooBudget).run([&](auto& p) {
        rom.init_ieee(p);
        p.jsr(rom.entry(WaterlooRoutine::EoiDown)).lda(imm(0x56)).sta(intermediate)
         .jsr(rom.entry(WaterlooRoutine::EoiUp));
    });
    EXPECT_EQ(execution[intermediate], 0x56);
    EXPECT_NE(system.io().pia1().state().pa.cr & M6520_CR_C2_MODE, 0);
}

// Verifies a Waterloo ROM termination entry's command byte and ATN flag.
void run_waterloo_termination_case(System& system, FirmwareTestSession& firmware,
                                   WaterlooRoutine routine, Command expected) {
    WaterlooFixture rom(system);
    SCOPED_TRACE(rom.name(routine));
    system.io().via().inputs([](auto& inputs) { inputs.port_b = ViaExternalIdle; });
    Program program(rom, rom.code);
    program.lds(test_asm::imm16(rom.stack_top()));
    rom.device(program);
    rom.init_ieee(program);
    program.jsr(rom.entry(routine));
    program.finish(rom.completion(rom.completion_region, 0, 0x55));
    auto rx_trace = firmware.trace_rx();
    auto wire = system.trace_ieee();
    auto execution = system.execution();
    execution.within(WaterlooBudget)
        .every(ServiceSliceCycles, ServiceTiming::AfterSlice, firmware.service_adapter());
    const auto result = rom.run(program, rom.results, execution, rom.name(routine) + " firmware service");
    EXPECT_THAT(rx_trace.capture(result).records(),
        testing::ElementsAre(fifo_record(command(expected))));
    EXPECT_THAT(wire.records(), testing::ElementsAre(command(expected)));
}

// Runs a Waterloo ROM entry with the shared IEEE file-control-block setup.
// Preserve historical prepared-state probes without treating them as passing compatibility coverage.
void run_waterloo_red_case(System& system, WaterlooRoutine routine,
                           FcbKind fcb_kind = FcbKind::Ieee) {
    WaterlooFixture scenario(system);
    const auto diagnostic = scenario.name(routine);
    const auto name = diagnostic.c_str();
    // Preserve the legacy control-block bytes, adding disk fields only on request.
    system.io().via().inputs([](auto& inputs) { inputs.port_b = ViaExternalIdle; });
    // Pass the same control-block address in X and D at the original entry.
    Program program(scenario, scenario.code);
    program.emit(test_asm::mc6809::lds_imm(scenario.stack_top()));
    scenario.device(program);
    if (fcb_kind == FcbKind::Disk) {
        const uint8_t filename[] = {'T', 'E', 'S', 'T'};
        scenario.file(program, filename);
    }
    scenario.init_ieee(program);
    program.emit(test_asm::mc6809::ldd_imm(static_cast<uint16_t>(ControlBlock.value())));
    program.jsr(scenario.entry(routine));
    program.emit(test_asm::mc6809::ldaa_imm(0x5a));
    program.emit(test_asm::mc6809::sta_abs(CompletionAddress));
    program.emit(test_asm::mc6809::bra(program.mark("park")));
    program.install();
    // Retain the original control write and bounded return-marker expectation.
    system.spi().write(fpga::Register::IeeeControl,
        fpga::IeeeControl{fpga::IeeeControlBit::Enabled}.bits() | LegacyControlBit3);
    system.cpu(CPU_SOFT_6809).start(program.entry());
    system.tick(WaterlooBudget);
    if (system.peek(econopet::SramAddress{CompletionAddress.value()}) != 0x5a) {
        throw std::runtime_error(std::string("Waterloo ") + name
                                 + " did not return to its RAM harness");
    }
}

// TODO: The C++ external-PIA harness does not yet reproduce the legacy
// bench's no-device handshake through $BE6F. Keep the ROM contract visible.
// Retains ByteOut_'s absent-device status and empty-FIFO expectations.
TEST_F(WaterlooRomIeee, DISABLED_Waterloo_BE6F_absent_device) {
    WaterlooFixture scenario(system);
    system.io().via().inputs([](auto& inputs) { inputs.port_b = ViaExternalIdle; });
    // Initialize the active block and send LISTEN through the byte-output entry.
    Program program(scenario, scenario.code);
    program.emit(test_asm::mc6809::lds_imm(scenario.stack_top()));
    scenario.device(program);
    scenario.init_ieee(program);
    program.emit(test_asm::mc6809::ldd_imm(IEEE_CMD_LISTEN(DeviceAddress)));
    program.jsr(scenario.entry(WaterlooRoutine::ByteOut));
    program.jsr(scenario.entry(WaterlooRoutine::AtnUp));
    program.emit(test_asm::mc6809::ldaa_imm(0x5a));
    program.emit(test_asm::mc6809::sta_abs(CompletionAddress));
    program.emit(test_asm::mc6809::bra(program.mark("park")));
    program.install();
    // Require an absent-device result without admitting a receive record.
    system.spi().write(fpga::Register::IeeeControl,
        fpga::IeeeControl{fpga::IeeeControlBit::Enabled}.bits());
    system.cpu(CPU_SOFT_6809).start(program.entry());
    system.tick(WaterlooBudget);
    require_drained_sequence(system, {}, ByteBudget, "Waterloo absent device");
    if (system.peek(econopet::SramAddress{CompletionAddress.value()}) != 0x5a || system.peek(econopet::SramAddress{ControlBlock.value()}) != AbsentDeviceStatus) {
        throw std::runtime_error("Waterloo BE6F did not report an absent IEEE device");
    }
}

// Historical probes waited at $EE33 despite real SPI service every 1,000 cycles.
// Preserve their explicit setup, after-slice servicing and disabled policy.
// Retains UNListen's single ATN-tagged command expectation.
TEST_F(WaterlooRomIeee, DISABLED_UNListen) {
    run_waterloo_termination_case(system, firmware, WaterlooRoutine::Unlisten, Unlisten);
}

// Retains UNTalk's single ATN-tagged command expectation.
TEST_F(WaterlooRomIeee, DISABLED_UNTalk) {
    run_waterloo_termination_case(system, firmware, WaterlooRoutine::Untalk, Untalk);
}

// These are direct counterparts of the RED Waterloo cases in ieee_sys_tb.
// They retain the ROM entry points and file-control-block fixtures while the
// drive-side exchanges are completed under test-driven development.
// Retains the prepared LISTEN-addressing entry and its control-block fixture.
TEST_F(WaterlooRomIeee, DISABLED_Waterloo_C072_LISTEN) {
    run_waterloo_red_case(system, WaterlooRoutine::Listen);
}

// Retains the prepared TALK-addressing entry and its control-block fixture.
TEST_F(WaterlooRomIeee, DISABLED_Waterloo_C0DD_TALK) {
    run_waterloo_red_case(system, WaterlooRoutine::Talk);
}

// Retains the EOI-byte entry and its original return expectation.
TEST_F(WaterlooRomIeee, DISABLED_Waterloo_BE5F_EOI_byte) {
    run_waterloo_red_case(system, WaterlooRoutine::EoiByte);
}

// Retains the final-byte/UNLISTEN entry and its original return expectation.
TEST_F(WaterlooRomIeee, DISABLED_Waterloo_BE53_final_byte) {
    run_waterloo_red_case(system, WaterlooRoutine::FinalByte);
}

// Retains the generic OPEN entry with the legacy disk fixture.
TEST_F(WaterlooRomIeee, DISABLED_Waterloo_BCD0_OPEN) {
    run_waterloo_red_case(system, WaterlooRoutine::Open, FcbKind::Disk);
}

// Retains the CLOSE entry with the legacy disk fixture.
TEST_F(WaterlooRomIeee, DISABLED_Waterloo_BD11_CLOSE) {
    run_waterloo_red_case(system, WaterlooRoutine::Close, FcbKind::Disk);
}

// Retains the REL OPEN entry with the legacy disk fixture.
TEST_F(WaterlooRomIeee, DISABLED_Waterloo_BD51_REL_OPEN) {
    run_waterloo_red_case(system, WaterlooRoutine::RelOpen, FcbKind::Disk);
}

// Retains the SCRATCH entry with the legacy disk fixture.
TEST_F(WaterlooRomIeee, DISABLED_Waterloo_BD96_SCRATCH) {
    run_waterloo_red_case(system, WaterlooRoutine::Scratch, FcbKind::Disk);
}

// Retains the RENAME entry with the legacy disk fixture.
TEST_F(WaterlooRomIeee, DISABLED_Waterloo_BDC6_RENAME) {
    run_waterloo_red_case(system, WaterlooRoutine::Rename, FcbKind::Disk);
}

// Retains the drive-initialize entry with the legacy disk fixture.
TEST_F(WaterlooRomIeee, DISABLED_Waterloo_BE23_initialize) {
    run_waterloo_red_case(system, WaterlooRoutine::Initialize, FcbKind::Disk);
}

// Retains the byte-input entry and its original return expectation.
TEST_F(WaterlooRomIeee, DISABLED_Waterloo_BEFB_input) {
    run_waterloo_red_case(system, WaterlooRoutine::Input);
}

// Retains the REL-position entry with the legacy disk fixture.
TEST_F(WaterlooRomIeee, DISABLED_Waterloo_BF98_REL_position) {
    run_waterloo_red_case(system, WaterlooRoutine::RelPosition, FcbKind::Disk);
}

// Retains the status-reader entry with the legacy disk fixture.
TEST_F(WaterlooRomIeee, DISABLED_Waterloo_BFEF_status) {
    run_waterloo_red_case(system, WaterlooRoutine::Status, FcbKind::Disk);
}

} // namespace

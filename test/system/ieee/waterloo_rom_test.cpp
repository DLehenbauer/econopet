// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <cstddef>
#include <cstdint>
#include <vector>

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

constexpr uint8_t AbsentDeviceStatus = 0x03;
constexpr econopet::Cycles ServiceSliceCycles = 1000;
constexpr uint8_t ViaExternalIdle = pet::Ndac | pet::NrfdInput | pet::Dav;
constexpr econopet::CpuAddress GlobalStatus{0x006a}, ActiveRole{0x0077};
// Revision-12 BF98 takes its second word argument at 2,S (after JSR).
constexpr test_asm::mc6809::Instruction<2> PushD{{0x34, 0x06}}, DropWord{{0x32, 0x62}};

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
    test.within(WaterlooBudget).run([&](auto& p) { p.jsr(rom.entry(WaterlooRoutine::IeeeInit)); });
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
    require_drained_sequence(system, {}, ByteBudget, "Waterloo ATNUp metadata");
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
        p.jsr(rom.entry(WaterlooRoutine::EoiDown))
            .lda(imm(0x56))
            .sta(intermediate)
            .jsr(rom.entry(WaterlooRoutine::EoiUp));
    });
    EXPECT_EQ(execution[intermediate], 0x56);
    EXPECT_NE(system.io().pia1().state().pa.cr & M6520_CR_C2_MODE, 0);
}

// Verifies a Waterloo ROM termination entry's command byte and ATN flag.
void run_waterloo_termination_case(System& system, FirmwareTestSession& firmware, WaterlooRoutine routine,
                                   Command expected) {
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
    execution.within(WaterlooBudget).every(ServiceSliceCycles, ServiceTiming::AfterSlice, firmware.service_adapter());
    const auto result = rom.run(program, rom.results, execution, rom.name(routine) + " firmware service");
    EXPECT_THAT(rx_trace.capture(result).records(), testing::ElementsAre(command(expected)));
    EXPECT_THAT(wire.records(), testing::ElementsAre(command(expected)));
}

// Check ByteOut_'s absent-device branch without addressing a listener.
TEST_F(WaterlooRomIeee, Waterloo_BE6F_absent_device) {
    constexpr econopet::CpuAddress AbsentDeviceBranch{0xbe90}, OutputTimeoutBranch{0xbee1};
    WaterlooFixture rom(system);
    system.io().via().inputs([](auto& inputs) { inputs.port_b = ViaExternalIdle; });
    auto test = rom.program();
    const auto status = test.byte("absent-device status");
    auto wire = system.trace_ieee();
    const auto checkpoint = system.cpu(CPU_SOFT_6809).peek_fetch_checkpoint();
    // Keep ATN released, so enabled firmware cannot create a listener handshake.
    const auto result = test.within(WaterlooBudget)
                            .every(ServiceSliceCycles, ServiceTiming::AfterSlice, firmware.service_adapter())
                            .run([&](auto& p) {
                                rom.init_ieee(p);
                                rom.device(p);
                                p.lda(imm(0))
                                    .sta(GlobalStatus)
                                    .ldd(test_asm::imm16(IEEE_CMD_LISTEN(DeviceAddress)))
                                    .jsr(rom.entry(WaterlooRoutine::ByteOut))
                                    .lda(WaterlooWorkspace::Block)
                                    .sta(status);
                            });
    // $BE90 calls B653 for global status, then BRA at $BE96 reaches $BEE4
    // to store status 3 in the active block, bypassing the timeout call at $BEE1.
    EXPECT_EQ(result[status], AbsentDeviceStatus);
    EXPECT_EQ(system.peek(econopet::SramAddress{GlobalStatus.value()}), AbsentDeviceStatus);
    EXPECT_TRUE(system.cpu(CPU_SOFT_6809).peek_opcode_fetched_at(AbsentDeviceBranch, checkpoint));
    EXPECT_FALSE(system.cpu(CPU_SOFT_6809).peek_opcode_fetched_at(OutputTimeoutBranch, checkpoint));
    require_drained_sequence(system, {}, ByteBudget, "Waterloo absent device");
    EXPECT_THAT(wire.records(), testing::IsEmpty());
}

// Verify UNListen's single ATN-tagged command through both RX and wire evidence.
TEST_F(WaterlooRomIeee, UNListen) {
    run_waterloo_termination_case(system, firmware, WaterlooRoutine::Unlisten, Unlisten);
}

// Verify UNTalk's single ATN-tagged command through both RX and wire evidence.
TEST_F(WaterlooRomIeee, UNTalk) { run_waterloo_termination_case(system, firmware, WaterlooRoutine::Untalk, Untalk); }

// Execute revision-12 BF98's counted binary P sender, not an implemented REL engine.
TEST_F(WaterlooRomIeee, Waterloo_BF98_emits_binary_position_command) {
    constexpr uint16_t RecordNumber = 3;
    WaterlooFixture rom(system);
    auto test = rom.program();
    auto wire = system.trace_ieee();
    auto rx = firmware.trace_rx();
    test.within(WaterlooBudget)
        .every(ServiceSliceCycles, ServiceTiming::AfterSlice, firmware.service_adapter())
        .run([&](auto& p) {
            rom.init_ieee(p);
            rom.device(p, DeviceAddress, FileChannel);
            p.lda(imm(0)).sta(ActiveRole).sta(GlobalStatus).lda(imm(1)).sta(WaterlooWorkspace::WaitParameterLow);
            // BF98 reads the record argument above the return address on S.
            p.ldd(test_asm::imm16(RecordNumber));
            p.emit(PushD);
            p.ldd(test_asm::imm16(WaterlooWorkspace::Block)).jsr(rom.entry(WaterlooRoutine::RelPosition));
            p.emit(DropWord);
        });
    system.execution()
        .within(ByteBudget)
        .service(firmware.service_adapter())
        .until([](const System& board) { return !board.snapshot().ieee_rx_ready_o; })
        .run("Waterloo binary command service");
    // BF98 sends raw channel 2, little-endian record 3, and position zero.
    const std::vector<Record> sent{command(Listen(Device{DeviceAddress})),
                                   command(Secondary(Channel{CommandChannel})),
                                   data('P'),
                                   data(FileChannel),
                                   data(3),
                                   data(0),
                                   data(0, Eoi::Last),
                                   command(Unlisten)};
    EXPECT_THAT(rx.records(), testing::ElementsAreArray(sent));
    require_sequence(wire.records(), sent, "Waterloo BF98 binary command and EOI");
}

} // namespace

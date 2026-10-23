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
constexpr econopet::CpuAddress ReceivedBytes{0x0800}, ReceivedFlags{0x0900};
constexpr uint8_t EoiReceived = 0x01;
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
        testing::ElementsAre(command(expected)));
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
            p.lda(imm(0)).sta(GlobalStatus)
             .ldd(test_asm::imm16(IEEE_CMD_LISTEN(DeviceAddress)))
             .jsr(rom.entry(WaterlooRoutine::ByteOut))
             .lda(WaterlooWorkspace::Block).sta(status);
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
TEST_F(WaterlooRomIeee, UNTalk) {
    run_waterloo_termination_case(system, firmware, WaterlooRoutine::Untalk, Untalk);
}

// Execute revision-12 BF98's binary P command, then read the positioned record.
// The retired RED probes checked only return, not their named disk operations.
TEST_F(WaterlooRomIeee, Waterloo_BF98_REL_position) {
    constexpr uint8_t RecordLength = 2;
    constexpr uint16_t RecordNumber = 3;
    constexpr size_t DirectoryFileType = 2;
    constexpr uint8_t Filename[] = {'R', 'E', 'C', ',', 'L', 0};
    auto image = disk_fixture::D64{}.seq("REC", {1, 2, 3, 4, 5, 6}).bytes();
    // Decorate the bounded data chain for positioning, not side-sector validation.
    const auto directory = disk_fixture::D64::offset(18, 1);
    image[directory + DirectoryFileType] = 0x80 | DISKIMAGE_FTYPE_REL;
    image[directory + DISKIMAGE_DIRECTORY_REL_LENGTH_OFFSET] = RecordLength;
    firmware.mount(image, "position.d64");

    WaterlooFixture rom(system);
    auto test = rom.program(test_asm::ProgramMode::Phased);
    const auto received = test.data("positioned record", ReceivedBytes, RecordLength,
        test_asm::Capture::Yes);
    const auto flags = test.data("positioned EOI flags", ReceivedFlags, RecordLength,
        test_asm::Capture::Yes);
    const auto status = test.byte("positioned input status");
    auto wire = system.trace_ieee();
    auto rx = firmware.trace_rx();
    auto opened = test.within(WaterlooBudget)
        .every(ServiceSliceCycles, ServiceTiming::AfterSlice, firmware.service_adapter())
        .pause([&](auto& p) {
            rom.init_ieee(p);
            rom.device(p, DeviceAddress, FileChannel);
            rom.file(p, Filename);
            p.lda(imm(0)).sta(ActiveRole).sta(GlobalStatus)
             .lda(imm(1)).sta(WaterlooWorkspace::WaitParameterLow)
             .ldd(test_asm::imm16(WaterlooWorkspace::Block)).jsr(rom.entry(WaterlooRoutine::Open));
            p.pause("REL OPEN unlistened");
            // BF98 reads the record argument above the return address on S.
            p.ldd(test_asm::imm16(RecordNumber));
            p.emit(PushD);
            p.ldd(test_asm::imm16(WaterlooWorkspace::Block)).jsr(rom.entry(WaterlooRoutine::RelPosition));
            p.emit(DropWord);
            p.pause("REL position unlistened");
            // OPEN and P complete at idle after UNLISTEN, before TALK is serviced.
            p.ldd(test_asm::imm16(WaterlooWorkspace::Block)).jsr(rom.entry(WaterlooRoutine::Talk))
             .lda(imm(0)).sta(WaterlooWorkspace::DiskFlags);
            for (size_t i = 0; i < RecordLength; ++i)
                p.jsr(rom.entry(WaterlooRoutine::Input)).stb(received.byte(i).address())
                 .lda(WaterlooWorkspace::DiskFlags).sta(flags.byte(i));
            p.lda(WaterlooWorkspace::Block).sta(status)
             .jsr(rom.entry(WaterlooRoutine::Untalk));
        });
    // Park the CPU at each idle boundary so dispatch cannot depend on EOI alone.
    const auto service_idle = [&] {
        system.execution().within(ByteBudget).service(firmware.service_adapter())
            .until([](const System& board) { return !board.snapshot().ieee_rx_ready_o; })
            .run("Waterloo idle command dispatch");
        firmware.service();
    };
    ASSERT_EQ(opened.name(), "REL OPEN unlistened");
    service_idle();
    // Each resume owns a fresh execution configuration, including SPI service.
    auto position_execution = system.execution();
    position_execution.within(WaterlooBudget)
        .every(ServiceSliceCycles, ServiceTiming::AfterSlice, firmware.service_adapter());
    auto positioned = test.resume(opened, position_execution);
    ASSERT_EQ(positioned.name(), "REL position unlistened");
    service_idle();
    auto read_execution = system.execution();
    read_execution.within(WaterlooBudget)
        .every(ServiceSliceCycles, ServiceTiming::AfterSlice, firmware.service_adapter());
    const auto complete = test.resume(positioned, read_execution);
    ASSERT_TRUE(complete.complete());
    const auto result = test.capture(complete);
    EXPECT_EQ(result[received], (std::vector<uint8_t>{5, 6}));
    EXPECT_EQ(result[flags.byte(0)] & EoiReceived, 0);
    EXPECT_EQ(result[flags.byte(1)] & EoiReceived, EoiReceived);
    EXPECT_EQ(result[status], 0);
    EXPECT_EQ(system.peek(econopet::SramAddress{GlobalStatus.value()}), 0);
    // BF98 sends raw channel 2, little-endian record 3, and position zero.
    const std::vector<Record> sent{
        command(Listen(Device{DeviceAddress})), command(Open(Channel{FileChannel})),
        data('R'), data('E'), data('C'), data(','), data('L', Eoi::Last), command(Unlisten),
        command(Listen(Device{DeviceAddress})), command(Secondary(Channel{CommandChannel})),
        data('P'), data(FileChannel), data(3), data(0), data(0, Eoi::Last), command(Unlisten),
        command(Talk(Device{DeviceAddress})), command(Secondary(Channel{FileChannel})),
        command(Untalk)};
    EXPECT_THAT(rx.records(), testing::ElementsAreArray(sent));
    auto expected = sent;
    expected.insert(expected.end() - 1, {data(5), data(6, Eoi::Last)});
    require_sequence(wire.records(), expected, "Waterloo BF98 positioned record and final EOI");
}

} // namespace

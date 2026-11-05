// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#include "framework/asm.h"
#include "framework/firmware_test.h"

using test_asm::imm;
using test_asm::Layout;

// Checks production CPU selection, SRAM execution and firmware drive enablement.
TEST_F(FirmwareTest, SpiRegistersCpuAndDriveMount) {
    constexpr econopet::Cycles ExecutionBudget{20000};

    // Round-trip CPU selection through the production SPI/Wishbone register file.
    system.cpu().assert_reset();
    system.cpu(CPU_SOFT_6502).select();
    ASSERT_EQ(system.cpu().read_selection(), CPU_SOFT_6502);

    // Execute a checked RAM program on the selected CPU and capture its result.
    auto test = system.program6502(Layout::Pet);
    const auto answer = test.byte("answer");
    const auto result = test.within(ExecutionBudget).run([&](auto& p) {
        p.lda(imm(0x42)).sta(answer);
    });
    EXPECT_EQ(result[answer], 0x42);

    // Mount a real firmware image and observe the resulting fabric enable bit.
    firmware.mount(disk_fixture::D64::empty());
    EXPECT_TRUE(system.ieee().read_control().contains(fpga::IeeeControlStatusBit::Enabled));
}

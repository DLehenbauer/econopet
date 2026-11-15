// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "framework/firmware_test.h"

#include <fstream>

extern "C" {
#include "display/display.h"
#include "driver.h"
#include "input/keyboard_core.h"
#include "input/pet_keyboard.h"
#include "roms/roms.h"
#include "system/state.h"
#include "system/system.h"
}

/** Run both assembled ROM entries and observe CPU-published completion through SPI. */
TEST_F(FirmwareTest, AssembledDiagnosticRomCompletesBeforeBootDeadline) {
    constexpr size_t RomBytes = 0x100;
    constexpr unsigned JumpEntryBytes = 3;
    constexpr econopet::Cycles BootBudget{128'000'000};
    std::array<uint8_t, RomBytes> rom;
    std::ifstream file(ECONOPET_MENU_BOOT_ROM, std::ios::binary);
    ASSERT_TRUE(file.read(reinterpret_cast<char*>(rom.data()), rom.size()));
    ASSERT_EQ(file.peek(), std::ifstream::traits_type::eof());

    for (const auto reason : {MENU_ROM_BOOT_NORMAL, MENU_ROM_BOOT_ERROR}) {
        SCOPED_TRACE(reason);
        system.cpu(CPU_SOFT_6502).prepare();
        spi_write(MENU_ROM_START_ADDRESS, rom.data(), rom.size());
        spi_write_at(MENU_ROM_COMPLETION_ADDRESS, 0);
        system.cpu(CPU_SOFT_6502).start(econopet::CpuAddress{
            MENU_ROM_START_ADDRESS + JumpEntryBytes * static_cast<unsigned>(reason)});
        uint8_t observed = 0;
        const auto start = system.time();
        system.execution().within(BootBudget)
            .service([&](System&, econopet::Cycles) {
                observed = spi_read_once(MENU_ROM_COMPLETION_ADDRESS);
            })
            .until([&](const System&) { return observed == MENU_ROM_COMPLETION_VALUE; })
            .run("assembled diagnostic completion");
        EXPECT_LT(system.time() - start, BootBudget);
        EXPECT_EQ(system.peek(econopet::SramAddress{MENU_ROM_COMPLETION_ADDRESS}),
            MENU_ROM_COMPLETION_VALUE);
        EXPECT_NE(spi_read_at(ECONOPET_WB_STATUS_ADDR) & ECONOPET_REG_STATUS_TEXT_MODE_MASK, 0);
    }
}

/** Run real session/scanner SPI against production top, SRAM, and physical chip models. */
TEST_F(FirmwareTest, BackRestoresBorrowedBytesAndResumesAfterPhysicalScanning) {
    for (const auto selection : {CPU_SOFT_6502, CPU_SOFT_6809}) {
        const econopet::CpuAddress entry{0x0300};
        const econopet::SramAddress marker{0x0200};
        const std::array<uint8_t, 8> code6502{0x58, 0xee, 0x00, 0x02, 0x4c, 0x01, 0x03, 0xea};
        const std::array<uint8_t, 8> code6809{0x7c, 0x02, 0x00, 0x20, 0xfb, 0x12, 0x12, 0x12};
        const auto& code = selection == CPU_SOFT_6502 ? code6502 : code6809;
        for (size_t index = 0; index < code.size(); ++index) {
            system.poke(econopet::SramAddress{entry.value() + index}, code[index]);
        }
        system.cpu(selection).prepare();
        system.poke(econopet::SramAddress{0x0201}, 0);
        const std::array<uint8_t, 7> handler{0xee, 0x01, 0x02, 0xad, 0x44, 0xe8, 0x40};
        for (size_t index = 0; index < handler.size(); ++index) {
            system.poke(econopet::SramAddress{0x0500 + index}, handler[index]);
        }
        system.poke(econopet::SramAddress{0xfffe}, 0x00);
        system.poke(econopet::SramAddress{0xffff}, 0x05);
        std::array<uint8_t, PET_MAX_VIDEO_RAM_BYTES> screen;
        std::array<uint8_t, 1u << ECONOPET_BRAM_ADDR_WIDTH> font;
        for (size_t index = 0; index < screen.size(); ++index) {
            screen[index] = index * 17 + 3;
        }
        for (size_t index = 0; index < font.size(); ++index) {
            font[index] = index * 7 + 1;
        }
        spi_write(VIDEO_RAM_ADDRESS, screen.data(), screen.size());
        spi_write(ECONOPET_WB_BRAM_BASE_ADDR, font.data(), font.size());
        system_state.video_source = video_source_pet;
        system.cpu(selection).start(entry);
        system.tick(1024);
        set_cpu(CPU_HALT);
        pet_keyboard_begin_boot();
        pet_keyboard_end();
        pet_io_write(0xe813, 0x3c);
        pet_io_write(0xe84c, 0xcc);
        set_cpu(CPU_READY);
        const auto control = get_cpu();
        ASSERT_TRUE(system_suspend());
        const auto saved_marker = system.peek(marker);
        if (selection == CPU_SOFT_6502) {
            pet_io_write(0xe84e, 0xc0);
            pet_io_write(0xe844, 0x20);
            pet_io_write(0xe845, 0x00);
        }
        const auto via_writes = system.io().via().writes().count();
        const auto pia2_writes = system.io().pia2().writes().count();
        for (unsigned step = 0; step < 3 * ECONOPET_KBD_COL_COUNT; ++step) {
            pet_keyboard_task();
            sync_state();
            system.tick(6400);
            EXPECT_EQ(system.peek(marker), saved_marker);
            EXPECT_FALSE(system.snapshot().cpu_be_o);
        }
        ASSERT_TRUE(pet_keyboard_ready());
        if (selection == CPU_SOFT_6502) {
            EXPECT_NE(pet_io_read(0xe84d) & 0x40, 0);
            EXPECT_FALSE(system.snapshot().cpu_irq_n_o);
            EXPECT_EQ(system.peek(econopet::SramAddress{0x0201}), 0);
        }
        EXPECT_EQ(system.io().via().writes().count(), via_writes);
        EXPECT_EQ(system.io().pia2().writes().count(), pia2_writes);
        std::array<uint8_t, VIDEO_PLANE_CAPACITY> menu;
        menu.fill(0x20);
        spi_write(VIDEO_RAM_ADDRESS, menu.data(), menu.size());
        system_restore(true);
        EXPECT_EQ(get_cpu(), control);
        set_cpu(CPU_HALT);
        std::array<uint8_t, PET_MAX_VIDEO_RAM_BYTES> restored;
        std::array<uint8_t, 1u << ECONOPET_BRAM_ADDR_WIDTH> restored_font;
        spi_read(VIDEO_RAM_ADDRESS, restored.size(), restored.data());
        spi_read(ECONOPET_WB_BRAM_BASE_ADDR, restored_font.size(), restored_font.data());
        EXPECT_EQ(restored, screen);
        EXPECT_EQ(restored_font, font);
        EXPECT_EQ(pet_io_read(0xe84c), 0xcc);
        set_cpu(CPU_READY);
        const auto resumed = system.peek(marker);
        system.tick(4096);
        EXPECT_NE(system.peek(marker), resumed);
        if (selection == CPU_SOFT_6502) {
            EXPECT_EQ(system.peek(econopet::SramAddress{0x0201}), 1);
        }
    }
}

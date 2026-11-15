// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "framework/firmware_test.h"

extern "C" {
#include "display/display.h"
#include "driver.h"
#include "input/keyboard_core.h"
#include "input/pet_keyboard.h"
#include "system/state.h"
#include "system/system.h"
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
        std::array<uint8_t, VIDEO_RAM_UNIT_BYTES> menu;
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

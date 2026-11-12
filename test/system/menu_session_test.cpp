// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "framework/firmware_test.h"

#include <algorithm>

extern "C" {
#include "driver.h"
#include "input/keyboard_core.h"
#include "input/pet_keyboard.h"
#include "menu/menu_session.h"
#include "system_state.h"
}

/** Refresh the cache from raw bridge scans, including the final released-key handoff. */
TEST_F(FirmwareTest, BridgeScanningRefreshesCacheWithoutGuestKeyboardReads) {
    for (const auto selection : {CPU_PHYS_6502, CPU_SOFT_6502, CPU_SOFT_6809}) {
        system.cpu(selection).prepare();
        set_cpu(CPU_HALT);
        ASSERT_TRUE(cpu_suspend(true));
        pet_keyboard_begin_boot();
        std::fill_n(usb_key_matrix, KEY_COL_COUNT, 0x00);
        sync_state();

        // Nonuniform physical rows must be observed despite active USB injection.
        std::array<uint8_t, KEY_COL_COUNT> pressed;
        for (size_t row = 0; row < pressed.size(); ++row) {
            pressed[row] = static_cast<uint8_t>(0xff ^ (1u << (row % 8)));
            system.io().pia1().inputs([&](auto& inputs) { inputs.port_b = pressed[row]; });
            for (unsigned sample = 0; sample < 3; ++sample) {
                pet_keyboard_task();
                system.tick(6400);
            }
        }
        ASSERT_TRUE(pet_keyboard_ready());
        std::array<uint8_t, KEY_COL_COUNT> cached;
        spi_read(ECONOPET_WB_KBD_BASE_ADDR, cached.size(), cached.data());
        EXPECT_EQ(cached, pressed);
        EXPECT_TRUE(std::equal(pressed.begin(), pressed.end(), pet_key_matrix));

        // The release guard must leave the hardware cache clean before scanning stops.
        system.io().pia1().inputs([](auto& inputs) { inputs.port_b = 0xff; });
        pet_keyboard_wait_release_begin();
        for (unsigned step = 0; step < 30; ++step) {
            pet_keyboard_task();
            system.tick(6400);
        }
        ASSERT_TRUE(pet_keyboard_released());
        pet_keyboard_end();
        ASSERT_TRUE(cpu_suspend(false));
        sync_state();
        EXPECT_TRUE(std::all_of(std::begin(pet_key_matrix), std::end(pet_key_matrix),
            [](const auto value) { return value == 0xff; }));
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
        spi_write(MENU_SCREEN_ADDRESS, screen.data(), screen.size());
        spi_write(ECONOPET_WB_BRAM_BASE_ADDR, font.data(), font.size());
        system_state.video_source = video_source_pet;
        system.cpu(selection).start(entry);
        system.tick(1024);
        ASSERT_TRUE(cpu_suspend(true));
        pet_keyboard_begin_boot();
        pet_keyboard_end();
        pet_io_write(0xe813, 0x3c);
        pet_io_write(0xe84c, 0xcc);
        ASSERT_TRUE(cpu_suspend(false));
        const auto control = get_cpu();
        ASSERT_TRUE(menu_session_begin());
        const auto saved_marker = system.peek(marker);
        if (selection == CPU_SOFT_6502) {
            pet_io_write(0xe84e, 0xc0);
            pet_io_write(0xe844, 0x20);
            pet_io_write(0xe845, 0x00);
        }
        const auto via_writes = system.io().via().writes().count();
        const auto pia2_writes = system.io().pia2().writes().count();
        for (unsigned step = 0; step < 30; ++step) {
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
        std::array<uint8_t, MENU_SCREEN_BYTES> menu;
        menu.fill(0x20);
        spi_write(MENU_SCREEN_ADDRESS, menu.data(), menu.size());
        menu_session_restore(true);
        EXPECT_EQ(get_cpu(), control);
        ASSERT_TRUE(cpu_suspend(true));
        std::array<uint8_t, PET_MAX_VIDEO_RAM_BYTES> restored;
        std::array<uint8_t, 1u << ECONOPET_BRAM_ADDR_WIDTH> restored_font;
        spi_read(MENU_SCREEN_ADDRESS, restored.size(), restored.data());
        spi_read(ECONOPET_WB_BRAM_BASE_ADDR, restored_font.size(), restored_font.data());
        EXPECT_EQ(restored, screen);
        EXPECT_EQ(restored_font, font);
        EXPECT_EQ(pet_io_read(0xe84c), 0xcc);
        ASSERT_TRUE(cpu_suspend(false));
        const auto resumed = system.peek(marker);
        system.tick(4096);
        EXPECT_NE(system.peek(marker), resumed);
        if (selection == CPU_SOFT_6502) {
            EXPECT_EQ(system.peek(econopet::SramAddress{0x0201}), 1);
        }
    }
}

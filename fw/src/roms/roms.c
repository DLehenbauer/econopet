#include "pch.h"
#include "roms.h"

#include "driver.h"
#include "menu/menu.h"
#include "pet.h"
#include "system_state.h"
#include "usb/keyboard.h"

const uint8_t __in_flash(".rom_chars_e800") rom_chars_e800[] = {
    #include "roms/901447_10.h"
};

const uint8_t __in_flash(".rom_menu_ff00") rom_menu_ff00[] = {
    #include "roms/menu_rom.h"
};

const uint8_t* const p_video_font_000 = rom_chars_e800;
const uint8_t* const p_video_font_400 = rom_chars_e800 + 0x400;

void start_menu_rom(menu_rom_boot_reason_t reason) {
    // Suspended CPU while initializing ROMs.
    set_cpu(/* ready */ false, /* reset */ false, /* nmi: */ false);

    // Query the PET model (according to the onboard DIP switches).
    read_pet_model(&system_state);

    // Ensure we are in 40 column mode on startup.
    system_state.pet_display_columns = pet_display_columns_40;
    system_state_set_video_ram_mask(&system_state, 0);  // 0 = 1KB video RAM
    write_pet_model(&system_state);

    // We need to load a USB keymap to allow the user to navigate the menu with USB.
    // Menu is keymap agnostic (only uses cursor/enter keys), so any keymap will do.
    read_keymap("/ukm/us.bin", &system_state);

    spi_write(/* dest: */ 0xFF00, /* pSrc: */ rom_menu_ff00,  sizeof(rom_menu_ff00));   // Load menu ROM
    spi_write(/* dest: */ 0x68000, /* pSrc: */ rom_chars_e800, sizeof(rom_chars_e800));  // Load character ROM

    // Set reset vector to jump table entry at $FF00 + (reason * 3).
    // Each JMP instruction is 3 bytes. The jump table is at a fixed address $FF00.
    uint16_t reset_vector = 0xFF00 + (reason * 3);
    spi_write_at(0xFFFC, reset_vector & 0xFF);         // Low byte
    spi_write_at(0xFFFD, (reset_vector >> 8) & 0xFF);  // High byte

    pet_reset();
}

#include "pch.h"
#include "roms.h"

#include "driver.h"
#include "fatal.h"
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

// Mirror of the config's character ROM. 4KB = SuperPET (all 4 quadrants);
// 2KB ROMs leave the tail unused.
#define CUSTOM_CHAR_ROM_MAX_SIZE 4096
static uint8_t custom_char_rom[CUSTOM_CHAR_ROM_MAX_SIZE];
static size_t custom_char_rom_load_offset = 0;
static size_t custom_char_rom_size = 0;

void roms_begin_char_rom_load(void) {
    custom_char_rom_load_offset = 0;
    custom_char_rom_size = 0;  // Invalidate (fall back to flash) until load completes.
}

void roms_append_char_rom_data(const uint8_t* data, size_t len) {
    if (len > CUSTOM_CHAR_ROM_MAX_SIZE - custom_char_rom_load_offset) {
        // Larger than any known character ROM -- truncate so the size check
        // rejects it.
        len = CUSTOM_CHAR_ROM_MAX_SIZE - custom_char_rom_load_offset;
    }
    memcpy(custom_char_rom + custom_char_rom_load_offset, data, len);
    custom_char_rom_load_offset += len;
    custom_char_rom_size = custom_char_rom_load_offset;
}

const uint8_t* roms_get_char_rom(bool video_graphics) {
    const uint8_t* base = rom_chars_e800;
    size_t size = sizeof(rom_chars_e800);

    if (custom_char_rom_size == 2048 || custom_char_rom_size == 4096) {
        base = custom_char_rom;
        size = custom_char_rom_size;
    }

    if (size >= 4096) {
        // 4-quadrant ROM: quadrant is {crtc_chr_option, video_graphics},
        // matching video.sv. crtc_chr_option is MA13 -- bit 5 of R12.
        const bool crtc_chr_option =
            (system_state.pet_crtc_registers[CRTC_R12_START_ADDR_HI] & 0x20) != 0;
        const unsigned int quadrant = ((unsigned int) crtc_chr_option << 1) | (video_graphics ? 1u : 0u);
        return base + quadrant * 0x400;
    }

    // 2KB ROM: video_graphics selects the half.
    return base + (video_graphics ? 0x400 : 0x000);
}

void start_menu_rom(menu_rom_boot_reason_t reason) {
    vet(reason < 2, "Menu ROM boot reason out of range: %d", reason);
    
    const unsigned int MENU_ROM_START = 0xFF00;

    // Suspended CPU while initializing ROMs.
    set_cpu(/* ready */ false, /* reset */ false, /* nmi: */ false);

    // Query the PET model (according to the onboard DIP switches).
    read_pet_model(&system_state);

    // Ensure we are in 40 column mode on startup.
    system_state.pet_display_columns = pet_display_columns_40;
    system_state.video_graphics = false;                // Start with text/business charset
    system_state_set_video_ram_mask(&system_state, 0);  // 0 = 1KB video RAM
    write_pet_model(&system_state);

    // We need to load a USB keymap to allow the user to navigate the menu with USB.
    // Menu is keymap agnostic (only uses cursor/enter keys), so any keymap will do.
    read_keymap("/ukm/us.bin", &system_state);

    spi_write(/* dest: */ MENU_ROM_START_ADDRESS, /* pSrc: */ rom_menu_ff00,  sizeof(rom_menu_ff00));   // Load menu ROM
    spi_write(/* dest: */ 0x68000, /* pSrc: */ rom_chars_e800, sizeof(rom_chars_e800));         // Load character ROM

    // Set reset vector to jump table entry at $FF00 + reason.
    uint16_t reset_vector = MENU_ROM_START_ADDRESS + (reason * 3);
    spi_write_at(0xFFFC, reset_vector & 0xFF);         // Low byte
    spi_write_at(0xFFFD, (reset_vector >> 8) & 0xFF);  // High byte

    pet_reset();
}

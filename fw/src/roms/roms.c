#include "pch.h"
#include "roms.h"

#include "diag/log/log.h"
#include "driver.h"
#include "fatal.h"
#include "hardware_contract.h"
#include "menu/menu.h"
#include "pet.h"
#include "sd/sd.h"
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

#define CHAR_ROM_SRAM_ADDRESS ECONOPET_WB_BRAM_BASE_ADDR
#define CHAR_ROM_SRAM_SIZE (1u << ECONOPET_BRAM_ADDR_WIDTH)
#define MENU_ROM_TIMEOUT_US 10000000u

static uint8_t custom_char_rom[CHAR_ROM_SRAM_SIZE];

void menu_rom_wait(void (*service)(void)) {
    const uint64_t start = time_us_64();
    uint8_t marker;
    while ((marker = spi_read_once(MENU_ROM_COMPLETION_ADDRESS)) != MENU_ROM_COMPLETION_VALUE) {
        if (time_us_64() - start >= MENU_ROM_TIMEOUT_US) {
            // Capture harmless SRAM/control observations before fatal rendering resets the CPU.
            log_warn("ROM timeout: marker=%02X legacy=%02X", marker, spi_read_at(MENU_ROM_COMPLETION_ADDRESS));
            log_warn("ROM CPU=%02X select=%02X status=%02X suspend=%02X", (unsigned int)get_cpu(),
                     spi_read_at(ECONOPET_WB_CPU_SEL_ADDR), spi_read_at(ECONOPET_WB_STATUS_ADDR),
                     spi_read_at(ECONOPET_WB_SUSPEND_ADDR));
            log_warn("ROM breakpoint address=$%04X", bp_hit_addr());
            fatal("diagnostic ROM did not complete (marker $%02X)", marker);
        }
        service();
    }
}

void roms_refresh_char_rom(void) {
    spi_read(CHAR_ROM_SRAM_ADDRESS, sizeof(custom_char_rom), custom_char_rom);
}

const uint8_t* roms_get_char_rom(video_text_mode_t mode) {
    // Quadrant is {crtc_chr_option, mode}, matching video.sv.
    // crtc_chr_option is MA13 -- bit 5 of R12.
    const bool crtc_chr_option =
        (system_state.pet_crtc_registers[CRTC_R12_START_ADDR_HI] & 0x20) != 0;
    const unsigned int quadrant =
        ((unsigned int) crtc_chr_option << 1) | (mode == video_text_mode_lower ? 1u : 0u);
    return custom_char_rom + quadrant * 0x400;
}

void start_menu_rom(menu_rom_boot_reason_t reason) {
    vet(reason < 2, "Menu ROM boot reason out of range: %d", reason);
    log_info("diagnostic ROM: %s", reason == MENU_ROM_BOOT_NORMAL ? "startup" : "fatal tune");

    // Suspended CPU while initializing ROMs.
    set_cpu(CPU_HALT);

    // Menus and fatal rendering must not depend on physical CPU detection.
    set_cpu_type(CPU_SOFT_6502);

    // Query the PET model (according to the onboard DIP switches).
    read_pet_model(&system_state);

    // Ensure we are in 40 column mode on startup.
    system_state.pet_display_columns = pet_display_columns_40;
    system_state.video_text_mode = video_text_mode_graphics;
    system_state_set_video_ram_mask(&system_state, pet_video_ram_mask_1kb);
    write_pet_model(&system_state);

    // Only interactive menus need a keymap. Fatal rendering must not depend on SD access.
    if (reason == MENU_ROM_BOOT_NORMAL) {
        char keymap_path[SD_PATH_MAX];
        sd_make_path(keymap_path, SD_DIR_UKM, "us.bin");
        read_keymap(keymap_path, &system_state);
    }

    spi_write(/* dest: */ MENU_ROM_START_ADDRESS, /* pSrc: */ rom_menu_ff00,  sizeof(rom_menu_ff00));   // Load menu ROM
    spi_write(/* dest: */ CHAR_ROM_SRAM_ADDRESS, /* pSrc: */ rom_chars_e800, sizeof(rom_chars_e800));   // Load character ROM
    roms_refresh_char_rom();

    // Set reset vector to jump table entry at $FF00 + reason.
    uint16_t reset_vector = MENU_ROM_START_ADDRESS + (reason * 3);
    spi_write_at(0xFFFC, reset_vector & 0xFF);         // Low byte
    spi_write_at(0xFFFD, (reset_vector >> 8) & 0xFF);  // High byte

    spi_write_at(MENU_ROM_COMPLETION_ADDRESS, 0);
    pet_reset();
}

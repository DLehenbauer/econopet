// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "system_state.h"

#include "hardware_contract.h"

system_state_t system_state = {
    .pet_display_columns = pet_display_columns_40,
    .video_ram_mask = pet_video_ram_mask_1kb,
    .video_ram_bytes = 1024,    // 1KB    
    .video_source = video_source_firmware,
    .term_mode = term_mode_cli,
    .video_graphics_mode = video_graphics_mode_graphics,
    .pet_crtc_registers = {
        [CRTC_R0_H_TOTAL]        = 0x31, // Horizontal total (minus one)
        [CRTC_R1_H_DISPLAYED]    = 0x28, // Displayed (40 chars)
        [CRTC_R2_H_SYNC_POS]     = 0x29, // HSYNC position
        [CRTC_R3_SYNC_WIDTH]     = 0x0F, // Sync widths
        [CRTC_R4_V_TOTAL]        = 0x28, // Vertical total (minus one)
        [CRTC_R5_V_ADJUST]       = 0x05, // Vertical adjust
        [CRTC_R6_V_DISPLAYED]    = 0x19, // Vertical displayed (25 rows)
        [CRTC_R7_V_SYNC_POS]     = 0x21, // VSYNC position
        [CRTC_R8_MODE_CONTROL]   = 0x00, // Mode control (unused)
        [CRTC_R9_MAX_SCAN_LINE]  = 0x07, // Num scan lines per row (minus one)
        [CRTC_R12_START_ADDR_HI] = 0x10, // Display start high (ma[13]=0: no option ROM, ma[12]=1: normal video)
        [CRTC_R13_START_ADDR_LO] = 0x00, // Display start low
    }
};

// Translate raw pin levels without changing the firmware enums' opposite polarity.
void system_state_set_config_pins(system_state_t* state, bool crt, bool keyboard) {
    static const pet_video_type_t video_types[] = {
        [ECONOPET_CONFIG_CRT_CRTC] = pet_video_type_crtc,
        [ECONOPET_CONFIG_CRT_FIXED] = pet_video_type_fixed,
    };
    static const pet_keyboard_model_t keyboard_models[] = {
        [ECONOPET_CONFIG_KEYBOARD_BUSINESS] = pet_keyboard_model_business,
        [ECONOPET_CONFIG_KEYBOARD_GRAPHICS] = pet_keyboard_model_graphics,
    };
    state->pet_video_type = video_types[crt];
    state->pet_keyboard_model = keyboard_models[keyboard];
}

void system_state_set_video_ram_mask(system_state_t* state, uint8_t video_ram_mask) {
    state->video_ram_mask = video_ram_mask;
    state->video_ram_bytes = (size_t)(video_ram_mask + 1) * 1024u;
}

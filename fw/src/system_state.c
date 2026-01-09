/**
 * PET Clone - Open hardware implementation of the Commodore PET
 * by Daniel Lehenbauer and contributors.
 * 
 * https://github.com/DLehenbauer/commodore-pet-clone
 *
 * To the extent possible under law, I, Daniel Lehenbauer, have waived all
 * copyright and related or neighboring rights to this project. This work is
 * published from the United States.
 *
 * @copyright CC0 http://creativecommons.org/publicdomain/zero/1.0/
 * @author Daniel Lehenbauer <DLehenbauer@users.noreply.github.com> and contributors
 */

#include "pch.h"
#include "system_state.h"

system_state_t system_state = {
    .pet_display_columns = pet_display_columns_40,
    .video_ram_mask = 0,        // 1KB
    .video_ram_bytes = 1024,    // 1KB    
    .video_source = video_source_firmware,
    .term_mode = term_mode_cli,
    .video_graphics = false,
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

void system_state_set_video_ram_mask(system_state_t* state, uint8_t video_ram_mask) {
    state->video_ram_mask = video_ram_mask;
    state->video_ram_bytes = (size_t)(video_ram_mask + 1) * 1024u;
}

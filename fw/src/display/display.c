// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "display.h"

#include "char_encoding.h"
#include "driver.h"
#include "dvi/dvi.h"
#include "fatal.h"
#include "roms/roms.h"
#include "system/state.h"
#include "ui/terminal.h"
#include "window.h"

/** Canonical firmware CRTC state (R1 is 40 in both column modes). */
static const uint8_t firmware_crtc[CRTC_REG_COUNT] = {
    [CRTC_R0_H_TOTAL] = 0x31,
    [CRTC_R1_H_DISPLAYED] = DISPLAY_COLS_40,
    [CRTC_R2_H_SYNC_POS] = 0x29,
    [CRTC_R3_SYNC_WIDTH] = 0x0f,
    [CRTC_R4_V_TOTAL] = 0x31,
    [CRTC_R5_V_ADJUST] = 0,
    [CRTC_R6_V_DISPLAYED] = DISPLAY_ROWS,
    [CRTC_R7_V_SYNC_POS] = 0x25,
    [CRTC_R9_MAX_SCAN_LINE] = 0x07,
    [CRTC_R12_START_ADDR_HI] = 0x10,
    [CRTC_R13_START_ADDR_LO] = 0,
};

static const uint8_t firmware_crtc_indices[] = {
    CRTC_R0_H_TOTAL,
    CRTC_R1_H_DISPLAYED,
    CRTC_R2_H_SYNC_POS,
    CRTC_R3_SYNC_WIDTH,
    CRTC_R4_V_TOTAL,
    CRTC_R5_V_ADJUST,
    CRTC_R6_V_DISPLAYED,
    CRTC_R7_V_SYNC_POS,
    CRTC_R9_MAX_SCAN_LINE,
    CRTC_R12_START_ADDR_HI,
    CRTC_R13_START_ADDR_LO,
};

window_t display_window(void) {
    assert(system_state.pet_display_columns == pet_display_columns_40 ||
           system_state.pet_display_columns == pet_display_columns_80);
    return window_create_color(system_state.video_char_buffer,
                               system_state.video_char_buffer + VIDEO_COLOR_OFFSET,
                               system_state.pet_display_columns, DISPLAY_ROWS, VIDEO_DEFAULT_ATTRIBUTE);
}

window_t display_init_local(pet_display_columns_t columns, uint8_t attr) {
    vet(columns == pet_display_columns_40 || columns == pet_display_columns_80,
        "invalid display columns: %d", (int)columns);
    // Reset the local renderer without relying on FPGA access.
    roms_use_builtin_char_rom();
    memcpy(system_state.pet_crtc_registers, firmware_crtc, sizeof(firmware_crtc));
    system_state.pet_display_columns = columns;
    system_state.video_text_mode = video_text_mode_lower;
    memset(system_state.video_char_buffer, VROM_SPACE, VIDEO_PLANE_CAPACITY);
    memset(system_state.video_char_buffer + VIDEO_COLOR_OFFSET, attr, VIDEO_COLOR_CAPACITY);
    return window_create_color(system_state.video_char_buffer,
                               system_state.video_char_buffer + VIDEO_COLOR_OFFSET,
                               columns, DISPLAY_ROWS, attr);
}

window_t display_init_firmware(pet_display_columns_t columns, uint8_t attr) {
    const window_t window = display_init_local(columns, attr);
    // Program only CRTC registers initialized by the FPGA.
    system_state.video_source = video_source_firmware;
    system_state_set_video_ram_mask(&system_state, columns == pet_display_columns_80
                                    ? pet_video_ram_mask_2kb : pet_video_ram_mask_1kb);
    spi_write(ECONOPET_WB_BRAM_BASE_ADDR, rom_chars_e800, sizeof(rom_chars_e800));
    write_pet_model(&system_state);
    for (size_t i = 0; i < count_of(firmware_crtc_indices); ++i) {
        const uint8_t index = firmware_crtc_indices[i];
        spi_write_at(ECONOPET_WB_CRTC_BASE_ADDR + index, firmware_crtc[index]);
    }
    return window;
}

bool display_term_video_active(void) {
    return system_state.terminal_session == terminal_session_remote;
}

void display_init(void) {
    video_init();
}

void display_term_begin(void) {
    fputs(TERM_ALTERNATE_ENTER, stdout);
    fputs(TERM_ECHO_OFF, stdout);
    fputs(TERM_CURSOR_HIDE, stdout);
    fputs(TERM_ATTRIBUTES_RESET, stdout);
    fputs(TERM_CLEAR_SCREEN, stdout);
    fflush(stdout);
}

void display_term_end(void) {
    // Preserve the cursor restored from the primary screen when leaving remote mode.
    fputs(TERM_ALTERNATE_EXIT TERM_CLI_PROMPT_NORMALIZE, stdout);
    fflush(stdout);
}

// Render video buffer to terminal using ANSI escape sequences
static void display_term_render(void) {
    bool reverse = false;

    fputs(TERM_HOME, stdout);

    const unsigned int cols = system_state.pet_display_columns;
    const unsigned int rows = DISPLAY_ROWS;
    assert(cols == DISPLAY_COLS_40 || cols == DISPLAY_COLS_80);
    uint8_t* char_buffer = system_state.video_char_buffer;

    for (unsigned int r = 0; r < rows; r++) {
        for (unsigned int c = 0; c < cols; c++) {
            const uint8_t ch = *char_buffer++;

            if (reverse != ((ch & VROM_REVERSE_MASK) != 0)) {
                reverse = !reverse;
                fputs(reverse ? TERM_REVERSE_ON : TERM_ATTRIBUTES_RESET, stdout);
            }

            fputs(vrom_to_term(ch), stdout);
        }
        fputs("\r\n", stdout);
    }

    fflush(stdout);
}

void display_term_refresh(void) {
    display_term_render();
}

// Synchronize one character or attribute span.
static void display_sync_range(const video_source_t video_source, const size_t offset, const size_t length) {
    assert(length > 0);
    assert(offset < sizeof(system_state.video_char_buffer));
    assert(length <= sizeof(system_state.video_char_buffer) - offset);

    uint8_t* const buffer = system_state.video_char_buffer + offset;

    if (video_source == video_source_pet) {
        spi_read(VIDEO_RAM_ADDRESS + offset, length, buffer);
    } else {
        spi_write(VIDEO_RAM_ADDRESS + offset, buffer, length);
    }
}

void display_clear(void) {
    memset(system_state.video_char_buffer, VROM_SPACE, VIDEO_PLANE_CAPACITY);
    display_sync_range(video_source_firmware, 0, VIDEO_PLANE_CAPACITY);
    if (display_term_video_active()) {
        display_term_refresh();
    }
}

void display_flush(void) {
    const video_source_t video_source = system_state.video_source;
    assert(video_source == video_source_pet || video_source == video_source_firmware);

    const uint8_t mask = system_state.video_ram_mask;
    assert(mask <= pet_video_ram_mask_4kb);

    switch (mask) {
    case pet_video_ram_mask_colourpet:
        // The 40-column ColourPET has a 1KB gap between character and color RAM:
        //
        //      $8000-$83FF: character RAM
        //      $8400-$87FF: (unmapped / wraps)
        //      $8800-$8BFF: color RAM
        //
        // As an optimization, use separate transfers to skip the unmapped hole.
        display_sync_range(video_source, 0, VIDEO_RAM_UNIT_BYTES);
        display_sync_range(video_source, VIDEO_COLOR_OFFSET, VIDEO_RAM_UNIT_BYTES);
        break;
    default:
        display_sync_range(video_source, 0, system_state.video_ram_bytes);
        break;
    }

    // Also update the terminal if it is mirroring PET video.
    if (display_term_video_active()) {
        display_term_render();
    }
}

void display_task(void) {
    display_flush();
}

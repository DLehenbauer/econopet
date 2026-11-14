// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "display.h"

#include "char_encoding.h"
#include "driver.h"
#include "dvi/dvi.h"
#include "fatal.h"
#include "system/state.h"

#define VIDEO_RAM_ADDRESS 0x8000u
#define VIDEO_RAM_UNIT_BYTES 0x400u
#define VIDEO_COLOR_OFFSET (2u * VIDEO_RAM_UNIT_BYTES)

// Terminal escape sequences
static const char* const term_home = "\e[H";
static const char* const term_reverse_off = "\e[m";
static const char* const term_reverse_on = "\e[7m";
static const char* const term_cursor_off = "\e[?25l";
static const char* const term_cursor_on = "\e[?25h";
static const char* const term_enter_alternate = "\e[?1049h";
static const char* const term_exit_alternate = "\e[?1049l";
static const char* const term_clear_screen = "\e[2J\e[H";
static const char* const term_echo_off = "\e[12l";
static const char* const term_echo_on = "\e[12h";

static const uint screen_width  = 40;
static const uint screen_height = 25;

bool display_term_video_active(void) {
    return system_state.terminal_session == terminal_session_remote;
}

void display_init(void) {
    video_init();
}

void display_term_begin(void) {
    fputs(term_enter_alternate, stdout);
    fputs(term_echo_off, stdout);
    fputs(term_cursor_off, stdout);
    fputs(term_reverse_off, stdout);
    fputs(term_clear_screen, stdout);
    fflush(stdout);
}

void display_term_end(void) {
    fputs(term_reverse_off, stdout);
    fputs(term_cursor_on, stdout);
    fputs(term_echo_on, stdout);
    fputs(term_exit_alternate, stdout);
    fflush(stdout);
}

// Render video buffer to terminal using ANSI escape sequences
static void display_term_render(void) {
    bool reverse = false;

    fputs(term_home, stdout);

    const unsigned int cols = screen_width;
    const unsigned int rows = screen_height;
    uint8_t* char_buffer = system_state.video_char_buffer;

    for (unsigned int r = 0; r < rows; r++) {
        for (unsigned int c = 0; c < cols; c++) {
            const char ch = *char_buffer++;

            if (reverse != ((ch & 0x80) != 0)) {
                reverse = !reverse;
                fputs(reverse ? term_reverse_on : term_reverse_off, stdout);
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

void display_task(void) {
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

// Window-based terminal display for fatal/config error screens
void display_window_begin(const window_t* window) {
    fputs(term_enter_alternate, stdout);
    fputs(term_echo_off, stdout);
    fputs(term_cursor_off, stdout);
    fputs(term_reverse_off, stdout);
    fputs(term_clear_screen, stdout);
    fflush(stdout);

    window_fill(window, CH_SPACE);
}

// Render a window to the terminal
void display_window_show(const window_t* window) {
    bool reverse = false;

    fputs(term_home, stdout);

    const unsigned int cols = window->width;
    const unsigned int rows = window->height;
    uint8_t* char_buffer = window->start;

    for (unsigned int r = 0; r < rows; r++) {
        for (unsigned int c = 0; c < cols; c++) {
            const char ch = *char_buffer++;

            if (reverse != ((ch & 0x80) != 0)) {
                reverse = !reverse;
                fputs(reverse ? term_reverse_on : term_reverse_off, stdout);
            }

            fputs(vrom_to_term(ch), stdout);
        }
        fputs("\r\n", stdout);
    }

    fflush(stdout);
}

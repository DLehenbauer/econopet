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
#include "display.h"

#include "char_encoding.h"
#include "driver.h"
#include "dvi/dvi.h"
#include "system_state.h"

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

void display_task(void) {
    // Local pointer to avoid repeated struct member access
    uint8_t* const video_char_buffer = system_state.video_char_buffer;

    // Sync video buffer based on video source
    if (system_state.video_source == video_source_pet) {
        // Read PET video RAM into buffer (6502 drives display)
        spi_read(0x8000, system_state.video_ram_bytes, video_char_buffer);
    } else {
        // Write buffer to PET video RAM (firmware drives display)
        spi_write(0x8000, video_char_buffer, PET_MAX_VIDEO_RAM_BYTES);
    }
    
    // HDMI DVI core continuously reads video_char_buffer - no action needed
    
    // Render to terminal if in video mode (not in CLI or log mode)
    if (system_state.term_mode == term_mode_video) {
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

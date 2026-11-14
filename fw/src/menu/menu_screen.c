// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "menu_screen.h"

#include "display/display.h"
#include "display/window.h"
#include "driver.h"
#include "system/state.h"

// Both 40- and 80-column displays address at most 2 KiB of character RAM.
#define MENU_CHARACTER_RAM_ADDRESS 0x8000u
#define MENU_CHARACTER_RAM_SIZE 0x800u

void menu_screen_clear(void) {
    memset(system_state.video_char_buffer, CH_SPACE, MENU_CHARACTER_RAM_SIZE);
    spi_write(MENU_CHARACTER_RAM_ADDRESS, system_state.video_char_buffer, MENU_CHARACTER_RAM_SIZE);
    if (display_term_video_active()) {
        display_term_refresh();
    }
}

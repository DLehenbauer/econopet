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
#include "fatal.h"

#include "display/display.h"
#include "display/dvi/dvi.h"
#include "display/window.h"
#include "global.h"
#include "input.h"
#include "reset.h"
#include "roms/roms.h"
#include "system_state.h"

static void __attribute__((noreturn)) vfatal(const char* const format, va_list args) {
    start_menu_rom(MENU_ROM_BOOT_ERROR);

    const window_t window = window_create(system_state.video_char_buffer, 40, 25);
    display_window_begin(&window);

    uint8_t* pOut = window_puts(&window, window.start, "E: ");
    window_reverse(&window, window.start, 2);
    pOut = window_vprintln(&window, pOut, format, args);

    if (errno != 0) {
        pOut = window_println(&window, pOut, "");
        pOut = window_print(&window, pOut, "(%d): %s", errno, strerror(errno));
    }

    display_window_show(&window);
    
    video_graphics = true;  // Use lower case for bit-banged DVI display

    // Wait for a key press (keyboard or menu button)
    while (input_getch() == EOF) {
        input_task();
        tight_loop_contents();
    }

    system_reset();
}

void fatal(const char* const format, ...) {
    va_list args;
    va_start(args, format);
    vfatal(format, args);
    va_end(args);
}

void* vetted_malloc(size_t __size) {
    // TODO: Use preallocated memory only in low memory situations.
    void* p = malloc(__size);
    vet(p != NULL, "malloc failed");
    return p;
}

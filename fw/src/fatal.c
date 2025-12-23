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

#include "fatal.h"
#include "global.h"
#include "menu/window.h"
#include "roms/roms.h"
#include "term.h"
#include "video/video.h"

static void __attribute__((noreturn)) vfatal(const char* const format, va_list args) {
    start_menu_rom(MENU_ROM_BOOT_ERROR);

    const window_t window = window_create(video_char_buffer, 40, 25);
    term_begin(&window);

    uint8_t* pOut = window_puts(&window, window.start, "E: ");
    window_reverse(&window, window.start, 2);
    pOut = window_vprintln(&window, pOut, format, args);

    if (errno != 0) {
        pOut = window_println(&window, pOut, "");
        pOut = window_print(&window, pOut, "(%d): %s", errno, strerror(errno));
    }

    term_display(&window);
    
    video_graphics = true;  // Use lower case for bit-banged DVI display

    // Wait for a key press (keyboard or menu button)
    while (term_input_char() == EOF) {
        tight_loop_contents();
    }

    // We use the watchdog to reset the cores and peripherals to get back to
    // a known state before running the firmware.
    watchdog_enable(/* delay_ms: */ 0, /* pause_on_debug: */ true);

    // Wait for watchdog to reset the device
    while (true) {
        __wfi();
    }
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

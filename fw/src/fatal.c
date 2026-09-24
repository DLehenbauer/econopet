// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

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
    
    system_state.video_source = video_source_firmware;  // Copy from `video_char_buffer` to $8000
    system_state.term_mode = term_mode_video;           // Also copy to the terminal
    system_state.video_graphics_mode = video_graphics_mode_text;
    display_task();

    // Put core0 into an infinite low-power wait loop. Core 1 continues running
    // to bit bang DVI video.
    while (true) {
        __wfi();
        tight_loop_contents();
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

// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "fatal.h"

#include "display/display.h"
#include "display/window.h"
#include "roms/roms.h"
#include "system_state.h"

#define FATAL_COLUMNS 40
#define FATAL_ROWS    25
#define FATAL_PREFIX  "E: "
#define FATAL_MESSAGE_SIZE \
    (FATAL_COLUMNS * FATAL_ROWS - (sizeof(FATAL_PREFIX) - 1) + 1)

static void __attribute__((noreturn)) fatal_no_alloc(const char* const message) {
    start_menu_rom(MENU_ROM_BOOT_ERROR);

    const window_t window = window_create(system_state.video_char_buffer, FATAL_COLUMNS, FATAL_ROWS);
    display_window_begin(&window);

    uint8_t* const pOut = window_puts(&window, window.start, FATAL_PREFIX);
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
    static char message[FATAL_MESSAGE_SIZE];

    const int saved_errno = errno;
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    size_t length;
    if (written < 0) {
        strcpy(message, "could not format fatal error");
        length = strlen(message);
    } else {
        const size_t formatted_length = (size_t)written;
        length = MIN(formatted_length, sizeof(message) - 1);
    }

    if (saved_errno != 0 && length < sizeof(message) - 1) {
        snprintf(
            message + length,
            sizeof(message) - length,
            "\n\n(%d): %s",
            saved_errno,
            strerror(saved_errno)
        );
    }

    fatal_no_alloc(message);
}

void* vetted_malloc(size_t __size) {
    void* p = malloc(__size);
    if (p == NULL) {
        fatal_no_alloc("malloc failed");
    }
    return p;
}

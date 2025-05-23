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
#include "term.h"
#include "video/video.h"

static void vfatal(const char* const format, va_list args) {
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

    abort();
}

void fatal(const char* const format, ...) {
    va_list args;
    va_start(args, format);
    vfatal(format, args);
    va_end(args);
}

void vet(bool condition, const char* const format, ...) {
    if (!condition) {
        va_list args;
        va_start(args, format);
        vfatal(format, args);
        va_end(args);
    }
}

void* vetted_malloc(size_t __size) {
    void* p = malloc(__size);
    vet(p != NULL, "malloc failed");
    return p;
}

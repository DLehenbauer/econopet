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

void fatal(const char* const format, ...) {
    const window_t window = window_create(video_char_buffer, 40, 25);
    term_begin(&window);

    va_list args;
    va_start(args, format);
    uint8_t* pOut = window_vprintln(&window, pOut, format, args);
    va_end(args);

    if (errno != 0) {
        pOut = window_println(&window, pOut, "");
        pOut = window_print(&window, pOut, "(%d): %s", errno, strerror(errno));
    }

    term_display(&window);
    abort();
}

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

#include <limits.h>
#include "model.h"
#include "video/video.h"
#include "sd/sd.h"
#include "mock.h"

uint8_t video_char_buffer[VIDEO_CHAR_BUFFER_BYTE_SIZE];
bool video_graphics = false;
bool video_is_80_col = false;

// All pico-vfs paths are absolute, starting with '/'.
// /workspaces/econopet/build/sdcard/sdcard_root/config.yaml
// /workspaces/econopet/sdcard/config.yaml
static const char* map_path(const char* const path) {
    static char relative_path[PATH_MAX];
    snprintf(relative_path, sizeof(relative_path), "../../../../sdcard%s", path);
    return relative_path;
}

// Wrapper for fopen
FILE* sd_open(const char* path, const char* mode) {
    assert(path[0] == '/');
    return fopen(map_path(path), mode);
}

void term_present() {
    fflush(stdout);
}

void set_cpu(bool ready, bool reset, bool nmi) {
    (void)ready;
    (void)reset;
    (void)nmi;
}

void start_menu_rom() { }

void spi_fill(uint32_t addr, uint8_t byte, size_t byteLength) {
    (void)addr;
    (void)byte;
    (void)byteLength;
}

void test_ram() { }

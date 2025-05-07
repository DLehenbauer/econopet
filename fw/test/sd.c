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

#include "sd/sd.h"

// All pico-vfs paths are absolute, starting with '/'.
static const char* map_path(const char* const path) {
    static char mapped_path[2048];
    snprintf(mapped_path, sizeof(mapped_path), "../../../build/sdcard/sdcard_root/%s", path);
    return mapped_path;
}

// Wrapper for fopen
FILE* sd_open(const char* path, const char* mode) {
    assert(path[0] == '/');
    return fopen(map_path(path), mode);
}

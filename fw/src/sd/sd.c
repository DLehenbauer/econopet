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

#include "sd.h"
#include "driver.h"
#include "fatal.h"
#include "global.h"
#include "hw.h"

#include "blockdevice/flash.h"
#include "blockdevice/sd.h"
#include "filesystem/fat.h"
#include "filesystem/littlefs.h"
#include "filesystem/vfs.h"

bool sd_init() {
    blockdevice_t* sd = blockdevice_sd_create(
        SD_SPI_INSTANCE,
        /* tx: */ SD_CMD_GP,
        /* rx: */ SD_DAT_GP,
        SD_CLK_GP,
        SD_CSN_GP,
        SD_SPI_MHZ * MHZ,
        /* enable_crc: */ false);

    filesystem_t* fat = filesystem_fat_create();

    if (fs_mount("/", fat, sd) == -1) {
        printf("fs_mount error: %s", strerror(errno));
        return false;
    }

    return true;
}

FILE* sd_open(const char* path, const char* mode) {
    if (path[0] != '/') {
        fatal("path must start with '/', but got '%s'", path);
    }

    FILE* file = fopen(path, mode);
    if (!file) {
        fatal("file not found '%s'", path);
    }

    return file;
}

void sd_read(const char* filename, FILE* file, sd_read_callback_t callback, void* context) {
    uint8_t* buffer = acquire_temp_buffer();

    while (true) {
        size_t bytes_read = fread(buffer, 1, TEMP_BUFFER_SIZE, file);

        if (ferror(file)) {
            fatal("error reading '%s'", filename);
        }

        if (bytes_read == 0) {
            break;
        }

        callback(buffer, bytes_read, context);
    }

    release_temp_buffer(&buffer);
}

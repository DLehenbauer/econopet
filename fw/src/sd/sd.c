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
    // Deassert SD CS
    gpio_init(SD_CSN_GP);
    gpio_set_dir(SD_CSN_GP, GPIO_OUT);
    gpio_put(SD_CSN_GP, 1);

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

size_t sd_read(const char* filename, FILE* file, uint8_t* dest, size_t size) {
    size_t bytes_read = fread(dest, 1, size, file);

    if (ferror(file)) {
        fatal("error reading '%s'", filename);
    }

    return bytes_read;
}

void sd_read_file(const char* filename, sd_read_callback_t callback, void* context, size_t max_bytes) {
    FILE* file = sd_open(filename, "rb");
    if (!file) {
        fatal("failed to open file '%s'", filename);
    }

    uint8_t* buffer = acquire_temp_buffer();
    size_t offset = 0;
    size_t remaining = max_bytes;

    while (remaining > 0) {
        size_t to_read = remaining < TEMP_BUFFER_SIZE ? remaining : TEMP_BUFFER_SIZE;
        size_t bytes_read = sd_read(filename, file, buffer, to_read);

        if (bytes_read == 0) {
            break;
        }

        callback(offset, buffer, bytes_read, context);
        offset += bytes_read;
        remaining -= bytes_read;
    }

    release_temp_buffer(&buffer);
}

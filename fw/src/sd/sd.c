// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "sd.h"

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "blockdevice/sd.h"
#endif
#undef PATH_MAX
#include "filesystem/fat.h"
#include "filesystem/vfs.h"

#include "ff.h"

#include "diag/log/log.h"
#include "driver.h"
#include "fatal.h"
#include "global.h"
#include "hw.h"
#include "sd/activity.h"

_Static_assert(SD_PATH_MAX == PATH_MAX,
               "SD path capacity must match pico-vfs");

// Ensure FatFs is built with variable sector size support.
_Static_assert(FF_MAX_SS != FF_MIN_SS,
               "FatFs must use a variable sector size so FATFS::ssize exists");

static blockdevice_t* root_sd;
static filesystem_t* root_fat;
static bool root_mounted;

bool sd_init() {
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
    if (root_sd == NULL) {
        // Deassert SD CS before constructing the SPI-backed block device.
        gpio_init(SD_CSN_GP);
        gpio_set_dir(SD_CSN_GP, GPIO_OUT);
        gpio_put(SD_CSN_GP, true);

        root_sd = blockdevice_sd_create(
            SD_SPI_INSTANCE,
            SD_CMD_GP,
            SD_DAT_GP,
            SD_CLK_GP,
            SD_CSN_GP,
            SD_SPI_MHZ * MHZ,
            SD_SPI_ENABLE_CRC);
        if (root_sd != NULL) {
            // Install the singleton activity wrapper before exposing the retained device.
            sd_activity_install(root_sd);
        }
    }
#endif
    if (root_fat == NULL) root_fat = filesystem_fat_create();

    return sd_mount_root();
}

struct blockdevice* sd_blockdevice(void) {
    return root_sd;
}

struct filesystem* sd_fat_filesystem(void) {
    return root_fat;
}

bool sd_is_mounted(void) {
    return root_mounted;
}

bool sd_mount_root(void) {
    if (root_mounted) return true;
    if (root_sd == NULL || root_fat == NULL) return false;
    root_mounted = fs_mount(sd_dir_prefix(SD_DIR_ROOT), root_fat, root_sd) == 0;
    if (!root_mounted) {
        log_warn("fs_mount error: %s", strerror(errno));
        // pico-vfs registers the FatFs drive before mount validation completes.
        // Retire that partial registration so a wiped card can be mounted later.
        if (root_sd->is_initialized && root_fat->unmount(root_fat) != 0)
            log_warn("fs_mount rollback error: %s", strerror(errno));
    }
    return root_mounted;
}

bool sd_unmount_root(void) {
    if (!root_mounted) return true;
    if (fs_unmount(sd_dir_prefix(SD_DIR_ROOT)) != 0) {
        log_warn("fs_unmount error: %s", strerror(errno));
        return false;
    }
    root_mounted = false;
    return true;
}

FILE* sd_open(const char* path, const char* mode) {
    vet_path_length(strlen(path));
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
        size_t to_read = remaining < TEMP_BUFFER_SIZE
            ? remaining
            : TEMP_BUFFER_SIZE;

        size_t bytes_read = sd_read(filename, file, buffer, to_read);

        if (bytes_read == 0) {
            break;
        }

        callback(offset, buffer, bytes_read, context);
        
        offset += bytes_read;
        remaining -= bytes_read;
    }

    release_temp_buffer(&buffer);
    fclose(file);
}

uint64_t sd_free_bytes(void) {
    // The FAT backend (pico-vfs) registers the SD card as FatFs logical drive
    // "0:". Query it directly for the free cluster count.
    FATFS* fs = NULL;
    DWORD free_clusters = 0;
    if (f_getfree("0:", &free_clusters, &fs) != FR_OK || fs == NULL) {
        log_warn("sd: f_getfree failed");
        return 0;
    }

    return (uint64_t)free_clusters * fs->csize * fs->ssize;
}

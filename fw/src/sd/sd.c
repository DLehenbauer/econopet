#include "sd.h"
#include "../hw.h"

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

    int err = fs_mount("/", fat, sd);
    if (err == -1) {
        printf("fs_mount error: %s", strerror(errno));
        return false;
    }

    return true;
}

FILE* sd_open(const char* path, const char* mode) {
    if (path[0] != '/') {
        assert(false);
        return NULL;
    }

    return fopen(path, mode);
}

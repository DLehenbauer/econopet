#include "sd.h"
#include "../hw.h"

#include "blockdevice/flash.h"
#include "blockdevice/sd.h"
#include "filesystem/fat.h"
#include "filesystem/littlefs.h"
#include "filesystem/vfs.h"

void sd_init() {
    printf("fs_init FAT on SD card\n");
    blockdevice_t *sd = blockdevice_sd_create(
        SD_SPI_INSTANCE,
        /* mosi: */ SD_CMD_GP,
        /* miso: */ SD_DAT_GP,
        SD_CLK_GP,
        SD_CSN_GP,
        SPI_MHZ * MHZ,
        false);
    filesystem_t *fat = filesystem_fat_create();
    int err = fs_mount("/", fat, sd);
    if (err == -1) {
        printf("format / with FAT\n");
        err = fs_format(fat, sd);
        if (err == -1) {
            printf("fs_format error: %s", strerror(errno));
            return false;
        }
        err = fs_mount("/", fat, sd);
        if (err == -1) {
            printf("fs_mount error: %s", strerror(errno));
            return false;
        }
    }
}

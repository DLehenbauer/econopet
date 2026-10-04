// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "sd.h"

#include "fatal.h"

const char* sd_dir_prefix(sd_dir_t directory) {
    switch (directory) {
        case SD_DIR_NONE: return "";
        case SD_DIR_ROOT: return "/";
        case SD_DIR_DISKS: return "/disks/";
        case SD_DIR_ROMS: return "/roms/";
        case SD_DIR_PRGS: return "/prgs/";
        case SD_DIR_UKM: return "/ukm/";
        case SD_DIR_FPGA: return "/fpga/";
        default: fatal("invalid SD directory: %d", directory);
    }
}

void sd_make_path(char path[SD_PATH_MAX], sd_dir_t directory, const char* name) {
    const int written = snprintf(path, SD_PATH_MAX, "%s%s",
                                 sd_dir_prefix(directory), name);
    vet(written >= 0, "could not format SD path");
    vet_path_length((size_t) written);
}

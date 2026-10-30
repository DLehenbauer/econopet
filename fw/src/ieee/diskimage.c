// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "diskimage.h"

bool diskimage_track_sectors(diskimage_type_t type, unsigned track, unsigned* sectors) {
    if (type != diskimage_type_d64 || sectors == NULL || track == 0
        || track > DISKIMAGE_D64_TRACK_COUNT) return false;
    *sectors = track <= 17 ? 21 : track <= 24 ? 19 : track <= 30 ? 18 : 17;
    return true;
}

bool diskimage_sector_offset(diskimage_type_t type, unsigned track, unsigned sector, uint32_t* offset) {
    unsigned count;
    if (offset == NULL || !diskimage_track_sectors(type, track, &count) || sector >= count)
        return false;
    uint32_t blocks = sector;
    for (unsigned t = 1; t < track; ++t) {
        diskimage_track_sectors(type, t, &count);
        blocks += count;
    }
    *offset = blocks * DISKIMAGE_SECTOR_SIZE;
    return true;
}

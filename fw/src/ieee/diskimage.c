// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "diskimage.h"

_Static_assert(DISKIMAGE_DIRECTORY_ENTRY_SIZE* DISKIMAGE_DIRECTORY_ENTRIES_PER_SECTOR == DISKIMAGE_SECTOR_SIZE,
               "directory entries must fill one disk sector");

typedef struct {
    unsigned int last_track;
    unsigned int sectors;
} diskimage_zone_t;

// 4040 TRKTBL/SECTRK and single-sided 8050 NTRK80/NSEC80 define these zones.
static const diskimage_zone_t d64_zones[] = {
    {17, 21},
    {24, 19},
    {30, 18},
    {DISKIMAGE_D64_TRACK_COUNT, 17},
};
static const diskimage_zone_t d80_zones[] = {
    {39, 29},
    {53, 27},
    {64, 25},
    {DISKIMAGE_D80_TRACK_COUNT, 23},
};

// Return the sector count for a one-based native track, shared by DOS and media I/O.
// Unsupported types, invalid tracks, or NULL outputs fail without changing *sectors.
bool diskimage_track_sectors(diskimage_type_t type, unsigned int track, unsigned int* sectors) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/romtbl
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/dskint
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tstfnd
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/romtbl
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/begin
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tstfnd
    //
    // 4040 SETSEC copies SECTRK into NUMSEC, which MAXSEC indexes using TRKNUM.
    // Those tables and TRKTBL establish the D64 zones (unchanged by the approved
    // revision-2 overlays). DOS 2.7 CONFI1-CONFI5 selects 77 or 154 tracks and
    // loads NTRK80/NSEC80 for JMXSEC. This pure helper selects geometry by image
    // type instead of consulting controller-populated RAM or hardware mode.

    // Reject missing storage and track zero before selecting a native geometry.
    if (sectors == NULL || track == 0) {
        return false;
    }
    const diskimage_zone_t* zones;
    size_t count;
    // Image types select 35-track 4040, single-sided 8050, or double-sided 8250.
    switch (type) {
    case diskimage_type_d64:
        zones = d64_zones;
        count = sizeof(d64_zones) / sizeof(d64_zones[0]);
        break;
    case diskimage_type_d80:
        zones = d80_zones;
        count = sizeof(d80_zones) / sizeof(d80_zones[0]);
        break;
    case diskimage_type_d82:
        if (track > DISKIMAGE_D82_TRACK_COUNT) {
            return false;
        }
        // NTRK80's second set of boundaries is the first set plus 77.
        // Fold tracks 78-154 onto 1-77 instead of duplicating JMXSEC's lookup.
        if (track > DISKIMAGE_D80_TRACK_COUNT) {
            track -= DISKIMAGE_D80_TRACK_COUNT;
        }
        zones = d80_zones;
        count = sizeof(d80_zones) / sizeof(d80_zones[0]);
        break;
    default:
        return false;
    }
    // The first inclusive upper boundary replaces the ROM's descending search.
    // Oversized tracks fall through without changing the output.
    for (size_t zone = 0; zone < count; ++zone) {
        if (track <= zones[zone].last_track) {
            *sectors = zones[zone].sectors;
            return true;
        }
    }
    return false;
}

// Translate a one-based track and zero-based sector to a raw-image byte offset.
// Return false without changing *offset for invalid geometry or a NULL output.
bool diskimage_sector_offset(diskimage_type_t type, unsigned int track, unsigned int sector, uint32_t* offset) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tstfnd
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tstfnd
    //
    // No direct ROM entry: MAXSEC (4040) and JMXSEC (DOS 2.7) supply native
    // sector counts through diskimage_track_sectors. Linear byte addressing
    // is a host container adaptation used by the media read/write facade,
    // not the ROM's physical-sector search or allocation/interleave policy.

    // Check the destination track's sector limit before accumulating an offset.
    unsigned int sectors;
    if (offset == NULL || !diskimage_track_sectors(type, track, &sectors) || sector >= sectors) {
        return false;
    }
    // Raw images concatenate complete tracks in order, then sectors in order.
    // Keep the original D82 track so side two follows all 77 side-one tracks.
    uint32_t preceding = sector;
    for (unsigned int current = 1; current < track; ++current) {
        if (!diskimage_track_sectors(type, current, &sectors)) {
            return false;
        }
        preceding += sectors;
    }
    // Include all 256 bytes per sector, including any filesystem link bytes.
    // No callback runs and no sector contents or file EOF rules are inspected.
    *offset = preceding * DISKIMAGE_SECTOR_SIZE;
    return true;
}

// Initialize a caller-owned descriptor and detect an exact raw D64/D80/D82 size.
// img must be non-NULL. No I/O occurs, and read/ctx are borrowed, not validated.
// Failure still initializes the descriptor, with type none and no writer.
bool diskimage_open(diskimage_t* img, diskimage_read_fn read, void* ctx, uint32_t size) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/romtbl
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/romtbl
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/begin
    //
    // No direct ROM entry: SECTRK/TRKTBL and NSEC80/NTRK80 with CONFI1/CONFI2
    // establish the native geometries represented by these containers. Size
    // detection is firmware mount policy, not ROM OPEN or disk initialization.

    // Replace descriptor fields even on failure. Clear any previous writer
    // so callers must explicitly opt into writes after successful detection.
    img->read = read;
    img->write = NULL;
    img->ctx = ctx;
    img->size = size;

    // Accept only exact raw sector payloads, without headers, extra tracks,
    // or appended error maps. Size alone does not validate BAM or disk contents.
    switch (size) {
    case DISKIMAGE_D64_SIZE:
        img->type = diskimage_type_d64;
        return true;
    case DISKIMAGE_D80_SIZE:
        img->type = diskimage_type_d80;
        return true;
    case DISKIMAGE_D82_SIZE:
        img->type = diskimage_type_d82;
        return true;
    default:
        img->type = diskimage_type_none;
        return false;
    }
}

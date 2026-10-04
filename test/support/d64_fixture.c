// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#include "d64_fixture.h"

#include <string.h>

#include "cbm/petscii.h"
#include "fatal.h"
// Internal callers use valid D64 tracks, including preflighted explicit chains.
static unsigned int sectors_on_track(unsigned int track) {
    unsigned int sectors;
    vet(diskimage_track_sectors(diskimage_type_d64, track, &sectors),
        "D64 fixture internal track must be valid: %u", track);
    return sectors;
}

// Internal coordinates are validated constants or preflighted chain entries.
static size_t sector_offset(unsigned int track, unsigned int sector) {
    uint32_t offset;
    vet(diskimage_sector_offset(diskimage_type_d64, track, sector, &offset),
        "D64 fixture internal coordinates must be valid: %u/%u", track, sector);
    return offset;
}

const char* d64_fixture_offset(unsigned int track, unsigned int sector, size_t* offset) {
    if (offset == NULL) return "D64 offset: missing output";
    unsigned int sectors;
    if (!diskimage_track_sectors(diskimage_type_d64, track, &sectors))
        return "D64 offset: track outside 1..35";
    uint32_t result;
    if (!diskimage_sector_offset(diskimage_type_d64, track, sector, &result))
        return "D64 offset: sector outside track";
    *offset = result;
    return NULL;
}

const char* d64_fixture_name(const char* name, uint8_t encoded[16]) {
    if (name == NULL || encoded == NULL) return "D64 filename: missing input or output";
    const size_t length = strlen(name);
    if (length == 0 || length > 16) return "D64 filename: expected 1..16 ASCII characters";
    uint8_t candidate[16];
    memset(candidate, 0xa0, sizeof(candidate));
    for (size_t i = 0; i < length; ++i) {
        const unsigned char byte = (unsigned char) name[i];
        if (byte < 0x20 || byte > 0x7e || strchr(":,=*?\"", byte) != NULL)
            return "D64 filename: unsupported character or DOS delimiter";
        candidate[i] = ascii_to_petscii(byte, true);
    }
    memcpy(encoded, candidate, sizeof(candidate));
    return NULL;
}

// Keep the BAM bitmap, free count, and construction ownership in agreement.
static void allocate_sector(d64_fixture_layout_t* layout, uint8_t* image,
    unsigned int track, unsigned int sector) {
    layout->used[sector_offset(track, sector) / DISKIMAGE_SECTOR_SIZE] = true;
    uint8_t* const bam = image + sector_offset(18, 0);
    --bam[track * 4];
    bam[track * 4 + 1 + sector / 8] &= (uint8_t) ~(1u << (sector % 8));
}

const char* d64_fixture_empty(d64_fixture_layout_t* layout, uint8_t* image, size_t size) {
    if (layout == NULL || image == NULL) return "D64 empty: missing state or storage";
    if (size != DISKIMAGE_D64_SIZE) return "D64 empty: expected exactly 174848 bytes";
    memset(layout, 0, sizeof(*layout));
    memset(image, 0, size);
    uint8_t* const bam = image + sector_offset(18, 0);
    bam[0] = 18;
    bam[1] = 1;
    bam[2] = 'A';
    for (unsigned int track = 1; track <= DISKIMAGE_D64_TRACK_COUNT; ++track) {
        bam[track * 4] = (uint8_t) sectors_on_track(track);
        for (unsigned int sector = 0; sector < sectors_on_track(track); ++sector)
            bam[track * 4 + 1 + sector / 8] |= (uint8_t) (1u << (sector % 8));
    }
    memset(bam + 0x90, 0xa0, 27);
    memcpy(bam + 0x90, "TEST DISK", 9);
    bam[0xa2] = '0';
    bam[0xa3] = '0';
    bam[0xa5] = '2';
    bam[0xa6] = 'A';
    image[sector_offset(18, 1) + 1] = 0xff;
    allocate_sector(layout, image, 18, 0);
    allocate_sector(layout, image, 18, 1);
    layout->initialized = true;
    return NULL;
}

const char* d64_fixture_prg(d64_fixture_layout_t* layout, uint8_t* image, size_t size,
    const char* name, const uint8_t* payload, size_t length,
    const d64_fixture_sector_t* chain, size_t chain_length) {
    return d64_fixture_file(layout, image, size, DISKIMAGE_FTYPE_PRG, name, payload,
        length, chain, chain_length);
}

const char* d64_fixture_file(d64_fixture_layout_t* layout, uint8_t* image, size_t size,
    unsigned int type, const char* name, const uint8_t* payload, size_t length,
    const d64_fixture_sector_t* chain, size_t chain_length) {
    if (type != DISKIMAGE_FTYPE_SEQ && type != DISKIMAGE_FTYPE_PRG && type != DISKIMAGE_FTYPE_USR)
        return "D64 file: unsupported file type";
    if (layout == NULL || image == NULL) return "D64 file: missing state or storage";
    if (size != DISKIMAGE_D64_SIZE) return "D64 file: expected exactly 174848 bytes";
    if (!layout->initialized) return "D64 file: construction state is not initialized";
    if (length != 0 && payload == NULL) return "D64 file: missing payload";
    if (length > D64_FIXTURE_DATA_SECTORS * DISKIMAGE_SECTOR_PAYLOAD_SIZE)
        return "D64 file: payload exceeds data capacity";
    if (length != 0) {
        // Compare address differences, avoiding unrelated-pointer ordering and overflow.
        const uintptr_t payload_start = (uintptr_t) payload;
        const uintptr_t image_start = (uintptr_t) image;
        const bool overlaps = payload_start >= image_start
            ? payload_start - image_start < size
            : image_start - payload_start < length;
        if (overlaps) return "D64 file: payload overlaps image storage";
    }
    if (layout->files >= D64_FIXTURE_FILES) return "D64 file: directory full";
    uint8_t encoded[16];
    const char* const error = d64_fixture_name(name, encoded);
    if (error != NULL) return error;
    for (unsigned int file = 0; file < layout->files; ++file) {
        const uint8_t* const entry = image + sector_offset(18,
            1 + file / DISKIMAGE_DIRECTORY_ENTRIES_PER_SECTOR)
            + (file % DISKIMAGE_DIRECTORY_ENTRIES_PER_SECTOR) * DISKIMAGE_DIRECTORY_ENTRY_SIZE;
        if (memcmp(entry + 5, encoded, sizeof(encoded)) == 0)
            return "D64 file: duplicate filename";
    }
    const size_t required = length == 0 ? 1 : 1 + (length - 1) / DISKIMAGE_SECTOR_PAYLOAD_SIZE;
    d64_fixture_sector_t selected[D64_FIXTURE_DATA_SECTORS];
    if (chain != NULL) {
        if (chain_length != required) return "D64 file: explicit chain length does not match payload";
        for (size_t i = 0; i < required; ++i) {
            size_t offset;
            const char* const coordinate_error = d64_fixture_offset(chain[i].track, chain[i].sector, &offset);
            if (coordinate_error != NULL) return coordinate_error;
            if (chain[i].track == 18 || layout->used[offset / DISKIMAGE_SECTOR_SIZE])
                return "D64 file: chain uses directory track or occupied sector";
            for (size_t previous = 0; previous < i; ++previous) {
                if (chain[previous].track == chain[i].track && chain[previous].sector == chain[i].sector)
                    return "D64 file: repeated sector in chain";
            }
            selected[i] = chain[i];
        }
    } else {
        if (chain_length != 0) return "D64 file: chain length without chain";
        size_t found = 0;
        for (unsigned int step = 0; step < DISKIMAGE_D64_TRACK_COUNT - 1 && found < required; ++step) {
            const unsigned int track = step < 17 ? 17 - step : step + 2;
            for (unsigned int sector = 0; sector < sectors_on_track(track) && found < required; ++sector) {
                if (!layout->used[sector_offset(track, sector) / DISKIMAGE_SECTOR_SIZE])
                    selected[found++] = (d64_fixture_sector_t) { (uint8_t) track, (uint8_t) sector };
            }
        }
        if (found != required) return "D64 file: disk full";
    }
    const unsigned int directory_sector = 1 + layout->files / DISKIMAGE_DIRECTORY_ENTRIES_PER_SECTOR;
    if (layout->files != 0 && layout->files % DISKIMAGE_DIRECTORY_ENTRIES_PER_SECTOR == 0) {
        uint8_t* const previous = image + sector_offset(18, directory_sector - 1);
        previous[0] = 18;
        previous[1] = (uint8_t) directory_sector;
        image[sector_offset(18, directory_sector) + 1] = 0xff;
        allocate_sector(layout, image, 18, directory_sector);
    }
    size_t copied = 0;
    for (size_t i = 0; i < required; ++i) {
        uint8_t* const data = image + sector_offset(selected[i].track, selected[i].sector);
        const size_t remaining = length - copied;
        const size_t take = remaining < DISKIMAGE_SECTOR_PAYLOAD_SIZE ? remaining : DISKIMAGE_SECTOR_PAYLOAD_SIZE;
        if (i + 1 < required) {
            data[0] = selected[i + 1].track;
            data[1] = selected[i + 1].sector;
        } else {
            data[0] = 0;
            data[1] = (uint8_t) (take + 1);
        }
        if (take != 0) memcpy(data + DISKIMAGE_SECTOR_LINK_SIZE, payload + copied, take);
        copied += take;
        allocate_sector(layout, image, selected[i].track, selected[i].sector);
    }
    uint8_t* const entry = image + sector_offset(18, directory_sector)
        + (layout->files % DISKIMAGE_DIRECTORY_ENTRIES_PER_SECTOR) * DISKIMAGE_DIRECTORY_ENTRY_SIZE;
    entry[2] = (uint8_t) (0x80 | type);
    entry[3] = selected[0].track;
    entry[4] = selected[0].sector;
    memcpy(entry + 5, encoded, sizeof(encoded));
    entry[30] = (uint8_t) required;
    entry[31] = (uint8_t) (required >> 8);
    ++layout->files;
    return NULL;
}

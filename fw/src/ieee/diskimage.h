// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Commodore disk image container. Supports:
//   .d64 (2031/4040, 35 tracks, 174848 bytes) -- directory at 18/1
//   .d80 (8050, 77 tracks, 533248 bytes)      -- directory at 39/1
//   .d82 (8250, 154 tracks, 1066496 bytes)    -- directory at 39/1
//
// Access is through a caller-supplied read callback so images can stream
// from the SD card (FatFs) without holding an image in RAM. Tests supply a
// memory-backed callback instead.

typedef enum {
    diskimage_type_none = 0,
    diskimage_type_d64,
    diskimage_type_d80,
    diskimage_type_d82,
} diskimage_type_t;

// Read 'len' bytes at byte 'offset' of the image into 'buf'.
// Returns true on success.
typedef bool (*diskimage_read_fn)(void* ctx, uint32_t offset, void* buf, size_t len);

// Write 'len' bytes at byte 'offset' of the image. Returns true on success.
typedef bool (*diskimage_write_fn)(void* ctx, uint32_t offset, const void* buf, size_t len);

typedef struct {
    diskimage_read_fn read;
    diskimage_write_fn write;   // NULL = image is read-only
    void* ctx;
    uint32_t size;
    diskimage_type_t type;
} diskimage_t;

#define DISKIMAGE_D64_SIZE 174848u
#define DISKIMAGE_D80_SIZE 533248u
#define DISKIMAGE_D82_SIZE 1066496u
#define DISKIMAGE_D64_TRACK_COUNT 35u
#define DISKIMAGE_D80_TRACK_COUNT 77u
#define DISKIMAGE_D82_TRACK_COUNT (2u * DISKIMAGE_D80_TRACK_COUNT)

// All supported containers have fixed-size sectors, independent of contents.
#define DISKIMAGE_SECTOR_SIZE 256u
#define DISKIMAGE_SECTOR_LINK_SIZE 2u
#define DISKIMAGE_SECTOR_PAYLOAD_SIZE (DISKIMAGE_SECTOR_SIZE - DISKIMAGE_SECTOR_LINK_SIZE)

// Directory sectors contain eight 32-byte slots. Offsets are relative to the
// slot start (the first slot includes the sector link at bytes 0 and 1).
#define DISKIMAGE_DIRECTORY_ENTRY_SIZE 32u
#define DISKIMAGE_DIRECTORY_ENTRIES_PER_SECTOR 8u
#define DISKIMAGE_DIRECTORY_REL_LENGTH_OFFSET 23u
#define DISKIMAGE_FILENAME_SIZE 16u
#define DISKIMAGE_FILENAME_PADDING 0xa0u

// CBM directory entry file types (low 3 bits of the type byte).
#define DISKIMAGE_FTYPE_DEL 0
#define DISKIMAGE_FTYPE_SEQ 1
#define DISKIMAGE_FTYPE_PRG 2
#define DISKIMAGE_FTYPE_USR 3
#define DISKIMAGE_FTYPE_REL 4

// Pure D64/D80/D82 geometry shared by image I/O and fixture construction.
// Return false for unsupported types, invalid coordinates, or missing outputs.
// Outputs are unchanged on failure.
bool diskimage_track_sectors(diskimage_type_t type, unsigned int track, unsigned int* sectors);
bool diskimage_sector_offset(diskimage_type_t type, unsigned int track, unsigned int sector,
    uint32_t* offset);

// Detects the image type from 'size'. Returns false if the size matches no
// supported container.
bool diskimage_open(diskimage_t* img, diskimage_read_fn read, void* ctx, uint32_t size);

#ifdef __cplusplus
}
#endif

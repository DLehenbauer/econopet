// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Geometry constants retained for host D64 fixtures, not a C filesystem engine.
typedef enum {diskimage_type_none, diskimage_type_d64} diskimage_type_t;
#define DISKIMAGE_D64_SIZE 174848u
#define DISKIMAGE_D64_TRACK_COUNT 35u
#define DISKIMAGE_SECTOR_SIZE 256u
#define DISKIMAGE_SECTOR_LINK_SIZE 2u
#define DISKIMAGE_SECTOR_PAYLOAD_SIZE 254u
#define DISKIMAGE_DIRECTORY_ENTRY_SIZE 32u
#define DISKIMAGE_DIRECTORY_ENTRIES_PER_SECTOR 8u
#define DISKIMAGE_FILENAME_SIZE 16u
#define DISKIMAGE_FILENAME_PADDING 0xa0u
#define DISKIMAGE_FTYPE_DEL 0u
#define DISKIMAGE_FTYPE_SEQ 1u
#define DISKIMAGE_FTYPE_PRG 2u
#define DISKIMAGE_FTYPE_USR 3u
#define DISKIMAGE_FTYPE_REL 4u

// Return sectors per valid D64 track, leaving output unchanged on invalid input.
bool diskimage_track_sectors(diskimage_type_t type, unsigned track, unsigned* sectors);

// Return a valid D64 sector's byte offset, leaving output unchanged on failure.
bool diskimage_sector_offset(diskimage_type_t type, unsigned track, unsigned sector, uint32_t* offset);

#ifdef __cplusplus
}
#endif

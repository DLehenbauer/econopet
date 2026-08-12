// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Commodore disk image container. Supports:
//   .d64 (2031/4040, 35 tracks, 174848 bytes) -- directory at 18/1
//   .d80 (8050, 77 tracks, 533248 bytes)      -- directory at 39/1
//
// Access is through a caller-supplied read callback so images can stream
// from the SD card (FatFs) without holding 533KB in RAM; tests supply a
// memory-backed callback instead.

typedef enum {
    diskimage_type_none = 0,
    diskimage_type_d64,
    diskimage_type_d80,
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

// CBM directory entry file types (low 3 bits of the type byte).
#define DISKIMAGE_FTYPE_DEL 0
#define DISKIMAGE_FTYPE_SEQ 1
#define DISKIMAGE_FTYPE_PRG 2
#define DISKIMAGE_FTYPE_USR 3
#define DISKIMAGE_FTYPE_REL 4

typedef struct {
    char name[17];      // NUL-terminated, $A0 padding stripped
    uint8_t file_type;  // DISKIMAGE_FTYPE_*
    uint8_t start_track;
    uint8_t start_sector;
    uint8_t record_len; // REL files: record length (0 otherwise)
} diskimage_entry_t;

// Detects the image type from 'size'. Returns false if the size matches no
// supported container.
bool diskimage_open(diskimage_t* img, diskimage_read_fn read, void* ctx, uint32_t size);

// Looks up 'name' (case-insensitive; '*' suffix wildcard; an optional
// leading drive prefix like "0:", "1:" or "1." is stripped) in the
// directory. Returns true and fills 'out' when found.
bool diskimage_find(const diskimage_t* img, const char* name, diskimage_entry_t* out);

// Iterates directory entries: 'index' counts valid (non-DEL) entries from 0.
// Returns false once past the last entry.
bool diskimage_entry(const diskimage_t* img, unsigned int index, diskimage_entry_t* out);

// Sequential reader over a file's sector chain.
typedef struct {
    const diskimage_t* img;
    uint8_t buf[256];
    uint16_t pos;       // next byte offset within buf (2..)
    uint16_t end;       // one past last valid byte offset within buf
    bool last_sector;   // buf is the final sector of the chain
    bool eof;
    uint16_t sectors;   // links followed; capped to detect circular chains
} diskstream_t;

// Opens a stream at the given start track/sector.
bool diskstream_open(diskstream_t* st, const diskimage_t* img, uint8_t track, uint8_t sector);

// Fetches the next byte. Returns false at end of file. '*last' is set true
// when the returned byte is the final byte of the file (for IEEE-488 EOI).
bool diskstream_next(diskstream_t* st, uint8_t* out, bool* last);

// ---------------------------------------------------------------------------
// Random access over a file's sector chain (for CBM RELative files).
//
// The record stream of a REL file is the concatenation of each chain
// sector's 254 data bytes; record N (1-based) starts at byte (N-1)*reclen.
// Side sectors are ignored: the chain itself fully determines the layout.
// ---------------------------------------------------------------------------

#define DISKCHAIN_MAX_SECTORS 800   // ~203KB of REL data

typedef struct {
    const diskimage_t* img;
    uint16_t count;                       // sectors in the chain
    uint16_t last_used;                   // data bytes used in final sector
    uint8_t ts[DISKCHAIN_MAX_SECTORS][2]; // track/sector of each chain link
} diskchain_t;

// Walks the chain from track/sector, caching every link for random access.
bool diskchain_build(diskchain_t* ch, const diskimage_t* img, uint8_t track, uint8_t sector);

// Total data bytes in the chain.
uint32_t diskchain_size(const diskchain_t* ch);

// Reads 'len' bytes at logical byte offset 'off' of the chain's data stream
// (spanning sector boundaries as needed). Returns false past end of chain.
bool diskchain_read(const diskchain_t* ch, uint32_t off, uint8_t* buf, uint16_t len);

// Writes 'len' bytes at logical byte offset 'off' of the chain's data
// stream (write-through to the underlying image; fails on a read-only
// image or past end of chain). Sector links are never modified.
bool diskchain_write(const diskchain_t* ch, uint32_t off, const uint8_t* buf, uint16_t len);

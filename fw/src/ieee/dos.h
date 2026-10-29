// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include "ieee/dos_media.h"

// INTTAB reserves logical indices 6/7 and leaves twelve shared data pages.
#define DOS_CHANNEL_COUNT 8u
#define DOS_FILE_CHANNEL_COUNT 6u
#define DOS_BUFFER_COUNT 12u
#define DOS_CHANNEL_BUFFER_COUNT 3u
#define DOS_COMMAND_LIMIT 58u
#define DOS_COMMAND_RECEIVE_CAPACITY (DOS_COMMAND_LIMIT + 1u)
#define DOS_FILENAME_COUNT 5u
#define DOS_UNUSED 0xffu
#define DOS_STAGE_CAPACITY 64u

// ERRBUF has 36 text bytes, followed here by transmitted CR and a host null.
#define DOS_STATUS_TEXT_MAX 36u
#define DOS_STATUS_LINE_CAPACITY (DOS_STATUS_TEXT_MAX + 1u + 1u)

typedef enum {
    DOS_STATUS_OK = 0,
    DOS_STATUS_FILES_SCRATCHED = 1,
    DOS_STATUS_READ_ERROR = 23,
    DOS_STATUS_WRITE_ERROR = 25,
    DOS_STATUS_WRITE_PROTECT = 26,
    DOS_STATUS_SYNTAX = 30,
    DOS_STATUS_INVALID_COMMAND = 31,
    DOS_STATUS_COMMAND_TOO_LONG = 32,
    DOS_STATUS_INVALID_NAME = 33,
    DOS_STATUS_NO_FILE = 34,
    DOS_STATUS_NO_RECORD = 50,
    DOS_STATUS_RECORD_OVERFLOW = 51,
    DOS_STATUS_FILE_TOO_LARGE = 52,
    DOS_STATUS_FILE_OPEN = 60,
    DOS_STATUS_NOT_FOUND = 62,
    DOS_STATUS_FILE_EXISTS = 63,
    DOS_STATUS_TYPE_MISMATCH = 64,
    DOS_STATUS_NO_BLOCK = 65,
    DOS_STATUS_ILLEGAL_TRACK_SECTOR = 66,
    DOS_STATUS_ILLEGAL_SYSTEM_TRACK_SECTOR = 67,
    DOS_STATUS_NO_CHANNEL = 70,
    DOS_STATUS_DIRECTORY_ERROR = 71,
    DOS_STATUS_DISK_FULL = 72,
    DOS_STATUS_POWER_ON = 73,
    DOS_STATUS_NOT_READY = 74,
} dos_status_code_t;

typedef struct {
    uint8_t bytes[DISKIMAGE_SECTOR_SIZE];
    uint8_t track;
    uint8_t sector;
    bool resident;
    bool failed;
} dos_buffer_t;

typedef enum {
    DOS_ACCESS_READ,
    DOS_ACCESS_WRITE,
    DOS_ACCESS_APPEND,
    DOS_ACCESS_MODIFY,
} dos_access_t;

typedef enum {
    DOS_DIRECTORY_NONE,
    DOS_DIRECTORY_HEADER,
    DOS_DIRECTORY_ENTRIES,
    DOS_DIRECTORY_FOOTER,
    DOS_DIRECTORY_END,
} dos_directory_phase_t;

typedef struct {
    dos_media_ref_t media;
    uint16_t traversed;
    uint16_t position;
    uint8_t buffers[DOS_CHANNEL_BUFFER_COUNT];
    uint8_t active;
    uint8_t secondary;
    uint8_t singleton;
    uint8_t access;
    uint8_t directory;
    uint8_t file_type;
    uint8_t directory_track;
    uint8_t directory_sector;
    uint8_t directory_entry;
    uint8_t first_track;
    uint8_t first_sector;
    uint8_t pattern_count;
    uint8_t line_length;
    bool allocated;
    bool ready;
    bool both_drives;
    bool line_eoi;
} dos_channel_t;

typedef struct {
    dos_buffer_t buffer;
    dos_media_ref_t media;
} dos_bam_t;

typedef struct {
    dos_media_ref_t media;
    uint8_t name[DISKIMAGE_FILENAME_SIZE];
    uint16_t visited;
    uint8_t track;
    uint8_t sector;
    uint8_t secondary;
    uint8_t type;
    uint8_t access;
    uint8_t scratch;
    uint8_t scratch_second;
    uint8_t last_track;
    uint8_t last_sector;
    uint8_t empty_track;
    uint8_t creation_drive;
    uint8_t record_size;
    bool record_parameter;
    bool replace;
    bool default_drive;
    bool tried_other;
    bool pending;
} dos_open_work_t;

typedef struct {
    dos_buffer_t buffers[DOS_BUFFER_COUNT];
    dos_channel_t channels[DOS_CHANNEL_COUNT];
    dos_bam_t bam[DOS_MEDIA_SLOTS_PER_UNIT];
    dos_open_work_t open;
    uint16_t buffer_use;
    uint16_t buffer_drives;
    uint16_t buffer_writes;
    uint16_t free_blocks[DOS_MEDIA_SLOTS_PER_UNIT];
    bool free_blocks_valid[DOS_MEDIA_SLOTS_PER_UNIT];
    uint8_t directory_internal;
    uint8_t selected_drive;
    uint8_t last_drive;
    uint8_t last_program_track;
    uint8_t last_program_sector;
    uint8_t last_program_drive;
    uint8_t command_bytes[DOS_COMMAND_RECEIVE_CAPACITY];
    char status[DOS_STATUS_LINE_CAPACITY];
    uint8_t status_length;
    uint8_t status_position;
    dos_status_code_t status_code;
} dos_unit_t;

// Reset bounded DOS resources, retaining external media bindings.
void dos_reset(dos_unit_t* unit, dos_model_t model);

// Parse bounded OPEN input into the caller's defaults without doing I/O.
bool dos_open_parse(const uint8_t input[DOS_COMMAND_RECEIVE_CAPACITY], unsigned int length, uint8_t* drive,
                    dos_open_work_t* work);

// Start an ordinary reader or directory, rejecting unsupported operations.
void dos_open(dos_unit_t* unit, dos_media_bank_t* media, unsigned int index, uint8_t secondary, const uint8_t* request,
              unsigned int length);

// Resume a pending lookup, yielding between directory sectors.
void dos_work(dos_unit_t* unit, dos_media_bank_t* media);

// Release one secondary, or all data channels for command secondary 15.
bool dos_close(dos_unit_t* unit, dos_model_t model, uint8_t secondary);

// Find an allocated secondary or return DOS_UNUSED.
uint8_t dos_find_channel(const dos_unit_t* unit, uint8_t secondary);

// Replace the retained status and restart its unread cursor.
void dos_status(dos_unit_t* unit, dos_model_t model, dos_status_code_t code, uint8_t track, uint8_t sector);

// Preview a bounded reader/directory prefix without committing consumption.
unsigned int dos_stage(dos_unit_t* unit, dos_media_bank_t* media, uint8_t channel, uint8_t* output,
                       unsigned int capacity, bool* eoi);

// Commit only bytes actually consumed from a previously staged prefix.
void dos_get(dos_unit_t* unit, dos_media_bank_t* media, uint8_t channel, unsigned int count);

// Preview the retained status suffix, including CR with EOI.
unsigned int dos_status_stage(const dos_unit_t* unit, uint8_t* output, unsigned int capacity, bool* eoi);

// Acknowledge actual status progress, clearing status only when CR is consumed.
void dos_status_get(dos_unit_t* unit, dos_model_t model, unsigned int count);

// Report whether the cooperative OPEN lookup has more work.
bool dos_work_pending(const dos_unit_t* unit);

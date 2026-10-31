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
    DOS_ACCESS_DIRECT,
} dos_access_t;

typedef enum {
    DOS_DIRECTORY_NONE,
    DOS_DIRECTORY_HEADER,
    DOS_DIRECTORY_ENTRIES,
    DOS_DIRECTORY_FOOTER,
    DOS_DIRECTORY_END,
} dos_directory_phase_t;

typedef enum {
    DOS_CLOSE_DATA,
    DOS_CLOSE_DIRECTORY,
    DOS_CLOSE_OLD_CHAIN,
    DOS_CLOSE_RECLAIM,
    DOS_CLOSE_BAM,
    DOS_CLOSE_FLUSH,
    DOS_CLOSE_ABORT_OPEN,
} dos_close_phase_t;

typedef struct {
    dos_media_ref_t media;
    uint16_t traversed;
    uint16_t position;
    uint8_t buffers[DOS_CHANNEL_BUFFER_COUNT];
    uint8_t active;
    uint8_t secondary;
    union {
        uint8_t singleton;
        uint8_t direct_last;
        uint8_t read_last;
    };
    uint8_t access;
    uint8_t directory;
    uint8_t file_type;
    uint8_t directory_track;
    uint8_t directory_sector;
    uint8_t directory_entry;
    uint8_t first_track;
    uint8_t first_sector;
    uint8_t old_track;
    uint8_t old_sector;
    uint8_t next_track;
    uint8_t next_sector;
    uint8_t pattern[DISKIMAGE_FILENAME_SIZE];
    uint8_t grow_from_sector;
    uint8_t closing;
    uint16_t blocks;
    uint8_t pattern_count;
    uint8_t directory_searches;
    union {
        uint8_t line_length;
        uint8_t direct_byte;
        uint8_t read_byte;
    };
    bool allocated;
    bool ready;
    union {
        bool line_eoi;
        bool direct_eoi;
        bool read_eoi;
    };
    bool replaced;
    bool failed;
    bool directory_dirty;
    bool directory_published;
    bool grow_pending;
    bool pointer_mode;
} dos_channel_t;

typedef struct {
    dos_buffer_t buffer;
    dos_media_ref_t media;
    bool dirty;
    bool reinitialize;
} dos_bam_t;

typedef struct {
    dos_media_ref_t media;
    uint8_t name[DISKIMAGE_FILENAME_SIZE];
    uint16_t visited;
    uint8_t track;
    uint8_t sector;
    uint8_t secondary;
    uint8_t name_length;
    uint8_t type;
    uint8_t access;
    uint8_t scratch;
    uint8_t scratch_second;
    uint8_t last_track;
    uint8_t last_sector;
    uint8_t empty_track;
    uint8_t empty_sector;
    uint8_t empty_entry;
    uint8_t append_channel;
    uint8_t creation_drive;
    uint8_t record_size;
    bool record_parameter;
    bool replace;
    bool wildcard;
    bool default_drive;
    bool first_program;
    bool tried_other;
    bool pending;
} dos_open_work_t;

// INITIALIZE owns selected media identities while reloading header/BAM state.
typedef struct {
    dos_media_ref_t media[DOS_MEDIA_SLOTS_PER_UNIT];
    uint16_t next_track;
    uint8_t id[2]; // Raw-image ID from the last successfully read directory header.
    uint8_t first_drive;
    uint8_t slots;
    bool started;
    bool pending;
    bool succeeded;
} dos_initialize_work_t;

// RENAME retains names and media identities while searching linked directories.
typedef struct {
    dos_media_ref_t media[DOS_MEDIA_SLOTS_PER_UNIT];
    uint8_t names[2][DISKIMAGE_FILENAME_SIZE];
    uint8_t name_lengths[2];
    uint16_t visited;
    uint8_t source_drive;
    uint8_t source_track;
    uint8_t source_sector;
    uint8_t source_entry;
    uint8_t track;
    uint8_t sector;
    uint8_t scratch[2];
    uint8_t found;
    bool both_drives;
    bool tried_peer;
    bool empty_target;
    bool name_conflict;
    bool commit;
    bool pending;
} dos_rename_work_t;

// SCRATCH retains bounded parser tables while releasing one linked sector at a time.
typedef struct {
    dos_media_ref_t media[DOS_MEDIA_SLOTS_PER_UNIT];
    uint8_t command[DOS_COMMAND_RECEIVE_CAPACITY];
    uint8_t starts[DOS_FILENAME_COUNT];
    uint8_t drives[DOS_FILENAME_COUNT];
    uint16_t visited;
    uint16_t chain_visited;
    uint8_t length;
    uint8_t names;
    uint8_t type;
    uint8_t patterns;
    uint8_t defaults;
    uint8_t matched;
    uint8_t drive;
    uint8_t scans;
    uint8_t track;
    uint8_t sector;
    uint8_t entry;
    uint8_t chain_track;
    uint8_t chain_sector;
    uint8_t next_chain_track;
    uint8_t next_chain_sector;
    uint8_t buffers[2];
    uint8_t phase;
    uint8_t count;
    bool data_pending;
    bool pending;
} dos_scratch_work_t;

// COPY saves the bounded LOOKUP table, not file data or whole-chain indexes.
typedef struct {
    uint8_t name[DISKIMAGE_FILENAME_SIZE];
    uint8_t name_length;
    uint8_t drive;
    uint8_t type;
    uint8_t track;
    uint8_t sector;
    uint8_t directory_track;
    uint8_t directory_sector;
    uint8_t directory_entry;
} dos_copy_file_t;

// All-files COPY retains its source pattern and native target-pattern flag in descriptor 2.
typedef struct {
    dos_media_ref_t media[DOS_MEDIA_SLOTS_PER_UNIT];
    dos_copy_file_t files[DOS_FILENAME_COUNT];
    uint16_t visited;
    uint8_t count;
    uint8_t found;
    uint8_t drive;
    uint8_t scans;
    uint8_t track;
    uint8_t sector;
    uint8_t reader;
    uint8_t writer;
    uint8_t source;
    uint8_t invalidated_file; // One-based descriptor retired by a peer replacement CLOSE.
    uint8_t phase;
    uint8_t empty_track;
    uint8_t empty_sector;
    uint8_t empty_entry;
    uint8_t last_sector;
    bool empty_target;
    bool name_conflict;
    bool pending;
    bool all_files;
    uint8_t all_track;
    uint8_t all_sector;
    uint8_t all_entry;
    uint16_t all_visited;
} dos_copy_work_t;

// VALIDATE borrows existing data buffers for DOS 2.7's rebuilt BAM pages.
typedef struct {
    dos_media_ref_t media;
    uint16_t owned;
    uint16_t visited;
    uint16_t chain_visited;
    uint8_t reader;
    uint8_t parts;
    uint8_t part;
    uint8_t phase;
    uint8_t track;
    uint8_t sector;
    uint8_t entry;
    uint8_t chain_track;
    uint8_t chain_sector;
    uint8_t next_track;
    uint8_t next_sector;
    uint8_t error;
    uint8_t error_track;
    uint8_t error_sector;
    bool directory_chain;
    bool data_pending;
} dos_validate_work_t;

// NEW and DUPLICATE retain only cursors, borrowing the existing BAM/data pages.
typedef struct {
    dos_media_ref_t media;
    dos_media_ref_t source;
    uint8_t name[DISKIMAGE_FILENAME_SIZE];
    uint8_t id[2];
    uint8_t phase;
    uint8_t track;
    uint8_t sector;
    uint8_t part;
    uint8_t group_low;
    uint8_t buffer;
    uint8_t first_buffer;
    uint8_t reader;
    bool empty_name;
    bool full;
    bool duplicate;
} dos_format_work_t;

// A block job retains its media identity while borrowing the channel's active page.
typedef struct {
    dos_media_ref_t media;
    uint8_t channel;
    uint8_t track;
    union {
        uint8_t sector;
        uint8_t offset;
    };
    bool raw;
} dos_block_work_t;

// Map commands retain one requested sector and scan higher tracks cooperatively.
typedef struct {
    dos_media_ref_t media;
    uint8_t track;
    uint8_t sector;
    bool allocate;
    bool suggestion;
} dos_block_map_work_t;

typedef enum {
    DOS_COMMAND_IDLE,
    DOS_COMMAND_BLOCK_READ,
    DOS_COMMAND_BLOCK_WRITE,
    DOS_COMMAND_BLOCK_MAP,
    DOS_COMMAND_READ_POINTER,
    DOS_COMMAND_RENAME,
    DOS_COMMAND_SCRATCH,
    DOS_COMMAND_COPY,
    DOS_COMMAND_FORMAT,
    DOS_COMMAND_VALIDATE,
} dos_command_work_t;

typedef struct {
    dos_buffer_t buffers[DOS_BUFFER_COUNT];
    dos_channel_t channels[DOS_CHANNEL_COUNT];
    dos_bam_t bam[DOS_MEDIA_SLOTS_PER_UNIT];
    dos_open_work_t open;
    dos_initialize_work_t initialize;
    // The command tag owns the transient worker and its shared pages.
    dos_command_work_t command;
    union {
        dos_block_work_t block;
        dos_block_map_work_t block_map;
        dos_rename_work_t rename;
        dos_scratch_work_t scratch;
        dos_copy_work_t copy;
        dos_format_work_t format;
        dos_validate_work_t validate;
    };
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

// Start an ordinary file, directory or direct page, rejecting REL operations.
void dos_open(dos_unit_t* unit, dos_media_bank_t* media, unsigned int index, uint8_t secondary, const uint8_t* request,
              unsigned int length);

// Advance one command phase or ordinary lookup/APPEND continuation.
void dos_work(dos_unit_t* unit, dos_media_bank_t* media);

// Dispatch an already-collected metadata, block, INITIALIZE or user request.
// Complete preceding work first. Command-channel transport collection is separate.
void dos_command(dos_unit_t* unit, dos_media_bank_t* media, unsigned int index, const uint8_t* request,
                 unsigned int length);

// Persist then release a secondary, retaining failed writers for retry.
// Command secondary 15 closes all data channels in descending order.
bool dos_close(dos_unit_t* unit, dos_media_bank_t* media, dos_model_t model, uint8_t secondary);

// Apply a decoded ordinary or direct PUT byte without treating EOI as file completion.
void dos_put(dos_unit_t* unit, dos_media_bank_t* media, uint8_t secondary, uint8_t byte);

// Persist writers, unit BAMs and backing stores without releasing successful owners.
// The caller must quiesce transport and complete pending lookup first.
bool dos_finalize(dos_unit_t* unit, dos_media_bank_t* media);

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

// Report whether cooperative OPEN, initialization or command work remains.
bool dos_work_pending(const dos_unit_t* unit);

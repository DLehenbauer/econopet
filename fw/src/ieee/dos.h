// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include "ieee/dos_media.h"
#include "ieee/dos_rel.h"

// Pinned equate plus DOS_4040 dskint / DOS_8250 begin reserve indices 6 and 7
// and initialize twelve data buffers. OPNRCH claims two buffers per ordinary file.
#define DOS_CHANNEL_COUNT 8u
#define DOS_FILE_CHANNEL_COUNT 6u
#define DOS_BUFFER_COUNT 12u
#define DOS_CHANNEL_BUFFER_COUNT 3u
#define DOS_COMMAND_LIMIT 58u
// Keep one over-limit byte so the revision-2 / DOS 2.7 length check can reject it.
#define DOS_COMMAND_RECEIVE_CAPACITY (DOS_COMMAND_LIMIT + 1u)
#define DOS_FILENAME_COUNT 5u
#define DOS_UNUSED 0xffu

// Bound speculative byte traversal so preview cannot evict an unread buffer.
#define DOS_STAGE_CAPACITY 64u

// DOS_4040/DOS_8250 ramvar reserve 36 bytes for ERRBUF before $4400.
// Include transmitted CR and the host-only terminating null.
// https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ramvar
// https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ramvar
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
    DOS_STATUS_INVALID_NAME = 33,
    DOS_STATUS_COMMAND_TOO_LONG = 32,
    DOS_STATUS_NO_RECORD = 50,
    DOS_STATUS_RECORD_OVERFLOW = 51,
    DOS_STATUS_FILE_TOO_LARGE = 52,
    DOS_STATUS_NO_FILE = 34,
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
    DOS_CLOSE_BAM,
    DOS_CLOSE_FLUSH,
    DOS_CLOSE_ABORT_OPEN,
    DOS_CLOSE_REL_SCRUB,
    DOS_CLOSE_REL_WAIT,
    DOS_CLOSE_REL_REFILL,
    DOS_CLOSE_REL_INDEX,
    DOS_CLOSE_REL_DIRECTORY_READ,
    DOS_CLOSE_REL_DIRECTORY_WRITE,
    DOS_CLOSE_REL_FLUSH,
    DOS_GROW_REL_END,
    DOS_GROW_REL_POSITION_END,
    DOS_GROW_REL_START,
    DOS_GROW_REL_LINK,
    DOS_GROW_REL_LINK_LAST,
    DOS_GROW_REL_OLD_WRITE,
    DOS_GROW_REL_NEXT_SELECT,
    DOS_GROW_REL_LAST_SELECT,
    DOS_GROW_REL_NEXT_WAIT,
    DOS_GROW_REL_LAST_WAIT,
    DOS_GROW_REL_NEXT_REFILL,
    DOS_GROW_REL_LAST_REFILL,
    DOS_GROW_REL_NEXT_ALLOCATE,
    DOS_GROW_REL_NEXT_FILL,
    DOS_GROW_REL_LAST_FILL,
    DOS_GROW_REL_DATA_WRITE,
    DOS_GROW_REL_INDEX,
    DOS_GROW_REL_DECIDE,
    DOS_GROW_REL_SIDE_ALLOCATE,
    DOS_GROW_REL_SIDE_SCRUB,
    DOS_GROW_REL_SIDE_REFILL,
    DOS_GROW_REL_SIDE_WRITE,
    DOS_GROW_REL_SIDE_OLD_WRITE,
    DOS_GROW_REL_SIDE_PREVIOUS,
    DOS_GROW_REL_SUPER_READ,
    DOS_GROW_REL_SUPER_WRITE,
    DOS_GROW_REL_GROUP_READ,
    DOS_GROW_REL_INDEX_WRITE,
    DOS_GROW_REL_MAP,
    DOS_GROW_REL_REPOSITION,
    DOS_GROW_REL_REPOSITION_REFILL,
    DOS_GROW_REL_POSITION,
} dos_close_phase_t;

typedef struct {
    dos_media_ref_t media;
    uint8_t buffers[DOS_CHANNEL_BUFFER_COUNT];
    // file_type selects the ordinary active index or internal REL write flags.
    union {
        uint8_t active;
        uint8_t rel_write;
    };
    uint8_t secondary;
    uint8_t file_type;
    bool allocated;
    bool ready;
    // Byte-sized access/directory/close enum tags keep per-channel state bounded.
    uint8_t access;
    // file_type selects the listing phase or native SSSGRP cache.
    union {
        uint8_t directory;
        uint8_t rel_group;
    };
    uint8_t closing;
    bool failed;
    union {
        struct {
            uint8_t first_track;
            uint8_t first_sector;
        };
        struct {
            uint8_t rel_root_track;
            uint8_t rel_root_sector;
        };
    };
    // REL readers and filtered directory listings use mutually exclusive state.
    union {
        uint8_t rel_model;
        uint8_t directory_file_type;
    };
    uint8_t directory_track;
    uint8_t directory_sector;
    uint8_t directory_entry;
    uint16_t blocks;
    // file_type selects ordinary metadata or the REL reader. Routing, teardown
    // and pool ownership stay outside this mutually exclusive state.
    union {
        struct {
            uint16_t traversed;
            uint16_t position;
            union {
                uint8_t singleton;
                uint8_t direct_last;
                uint8_t read_last;
            };
            uint8_t old_track;
            uint8_t old_sector;
            uint8_t next_track;
            uint8_t next_sector;
            uint8_t pattern[DISKIMAGE_FILENAME_SIZE];
            union {
                uint8_t line_length;
                uint8_t direct_byte;
                uint8_t read_byte;
            };
            uint8_t pattern_count;
            uint8_t grow_from_sector;
            union {
                bool line_eoi;
                bool direct_eoi;
                bool read_eoi;
            };
            bool both_drives;
            bool replaced;
            bool directory_dirty;
            bool directory_published;
            bool grow_pending;
            bool pointer_mode;
        };
        dos_rel_reader_t rel;
    };
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
    uint8_t type;
    uint8_t scratch;
    uint8_t scratch_second;
    uint8_t empty_track;
    uint8_t empty_sector;
    uint8_t empty_entry;
    uint8_t last_track;
    uint8_t last_sector;
    uint8_t creation_drive;
    uint8_t append_channel;
    uint8_t record_size;
    uint8_t rel_phase;
    dos_access_t access;
    bool record_parameter;
    bool replace;
    bool default_drive;
    bool tried_other;
    bool pending;
} dos_open_work_t;

// Decode OPEN's bounded command page before lookup. Supply all 59 command
// bytes so OP60 may inspect a retained tail byte after CMDSET trimming.
// The caller resolves missing-file creation and SA 0/1 defaults after lookup.
bool dos_open_parse(const uint8_t input[DOS_COMMAND_RECEIVE_CAPACITY], unsigned int length, uint8_t* drive,
                    dos_open_work_t* work);

// INITIALIZE owns selected media identities while reloading header/BAM state.
typedef struct {
    dos_media_ref_t media[DOS_MEDIA_SLOTS_PER_UNIT];
    uint16_t next_track;
    uint8_t first_drive;
    uint8_t slots;
    bool started;
    bool pending;
} dos_initialize_work_t;

// RENAME retains names and media identities while searching linked directories.
typedef struct {
    dos_media_ref_t media[DOS_MEDIA_SLOTS_PER_UNIT];
    uint8_t names[2][DISKIMAGE_FILENAME_SIZE];
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
    bool pending;
} dos_scratch_work_t;

// COPY saves the bounded LOOKUP table, not file data or whole-chain indexes.
typedef struct {
    uint8_t name[DISKIMAGE_FILENAME_SIZE];
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
    uint8_t phase;
    uint8_t empty_track;
    uint8_t empty_sector;
    uint8_t empty_entry;
    uint8_t last_sector;
    uint8_t record_size;
    uint8_t resume_phase;
    uint8_t end_group;
    uint8_t end_side;
    uint8_t end_index;
    bool missing_writer;
    bool empty_target;
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
    DOS_COMMAND_RENAME,
    DOS_COMMAND_SCRATCH,
    DOS_COMMAND_COPY,
    DOS_COMMAND_VALIDATE,
    DOS_COMMAND_FORMAT,
    DOS_COMMAND_BLOCK_READ,
    DOS_COMMAND_BLOCK_WRITE,
    DOS_COMMAND_BLOCK_MAP,
    DOS_COMMAND_READ_POINTER,
    DOS_COMMAND_RECORD,
    DOS_COMMAND_CLOSE,
} dos_command_work_t;

// Command/OPEN collection is per unit, independent of bus listener ownership.
typedef struct {
    uint8_t bytes[DOS_COMMAND_RECEIVE_CAPACITY];
    unsigned int length;
    uint8_t secondary;
    bool collecting;
    bool complete;
} dos_input_t;

// Retain the resolved source across TALK changes until its staged copy is retired.
typedef struct {
    uint8_t channel;
    bool status;
} dos_source_t;

// Read-only diagnostic snapshot, not mutable transport or DOS ownership.
typedef struct {
    unsigned int position;
    unsigned int length;
} dos_status_progress_t;

typedef struct {
    dos_buffer_t buffers[DOS_BUFFER_COUNT];
    dos_channel_t channels[DOS_CHANNEL_COUNT];
    uint16_t buffer_use;
    // Native LSTJOB drive bits survive buffer release and 4040 explicit OPEN.
    uint16_t buffer_drives;
    // TSTWRT distinguishes the most recent READ and WRITE for each pool page.
    uint16_t buffer_writes;
    uint8_t selected_drive;
    uint8_t last_drive;
    uint8_t status_position;
    uint8_t status_length;
    char status[DOS_STATUS_LINE_CAPACITY];
    dos_status_code_t status_code;
    dos_open_work_t open;
    dos_initialize_work_t initialize;
    // Ordered commands cannot overlap. The tag, not aliased pending flags,
    // determines which transient state owns buffers and channels.
    dos_command_work_t command;
    union {
        dos_rename_work_t rename;
        dos_scratch_work_t scratch;
        dos_copy_work_t copy;
        dos_validate_work_t validate;
        dos_format_work_t format;
        dos_block_work_t block;
        dos_block_map_work_t block_map;
        // The channel retains RECORD's decoded counter and positioning job.
        uint8_t record_channel;
        struct {
            uint8_t secondary;
            uint8_t model;
        } close;
    };
    dos_input_t input;
    dos_bam_t bam[DOS_MEDIA_SLOTS_PER_UNIT];
    uint8_t directory_internal;
    // Host-only durability for NXDRBK after REL's native MAPOUT.
    uint8_t rel_directory_bam;
    uint8_t last_program_track;
    uint8_t last_program_sector;
    uint8_t last_program_drive;
    uint16_t free_blocks[DOS_MEDIA_SLOTS_PER_UNIT];
    bool free_blocks_valid[DOS_MEDIA_SLOTS_PER_UNIT];
} dos_unit_t;

// Internal REL ownership prerequisite, not OPEN or record preparation.
// The caller retires any prior owner of secondary first. Claim three shared
// pages and one logical channel, initially not ready. Return DOS_UNUSED on
// exhaustion, retaining peers and publishing no status or disk I/O.
uint8_t dos_rel_channel_claim(dos_unit_t* unit, dos_media_ref_t media, uint8_t secondary);

// Prepare OPNRCH on a fresh claim using the directory's first data link, not
// FNDREL's first side pointer. Preserve common directory ownership. Small REL
// reads its side root after STRDBL, while 8250 only captures the super root.
// Each worker step performs at most one read.
void dos_rel_channel_prepare_open(dos_unit_t* unit, uint8_t channel, dos_model_t model, uint8_t root_track,
                                  uint8_t root_sector, uint8_t data_track, uint8_t data_sector, uint8_t size);

// Queue OPNWCH/OW20 on a fresh claim after INTTS has allocated the supplied
// first data sector. Allocate/write the side and optional super sector, write
// NULBUF/NULLNK data and MAPOUT, then prepare OROW. Keep allocations and pages
// through failed jobs. The public OPEN/COPY owner then publishes ADDFIL.
void dos_rel_channel_create(dos_unit_t* unit, uint8_t channel, dos_model_t model, uint8_t data_track,
                            uint8_t data_sector, uint8_t size);

// Retry only a retained creation job. Never replay successful allocations.
bool dos_rel_channel_retry_create(dos_unit_t* unit, uint8_t channel);

// Queue native ADDREL for the current missing writer record after retiring
// FIFO copies. Retain the same three pages through NEWSS role transfers and
// failures. Success prepares RELPUT without storing its input byte. This does
// not publish ENDCMD or acknowledge EOI.
bool dos_rel_channel_grow(dos_unit_t* unit, uint8_t channel);

// Retry a retained growth job without repeating allocation or marker filling.
bool dos_rel_channel_retry_grow(dos_unit_t* unit, uint8_t channel);

// Internal clean positioned-read entry, not public OPEN or RECORD.
// Retire this channel's FIFO copies first. The caller supplies validated
// nonzero record length and side/super root from the selected directory entry.
// Preparation is cooperative and preserves status on success. No dirty-page
// reuse, initial command status or persistent directory metadata is supplied.
void dos_rel_channel_prepare(dos_unit_t* unit, uint8_t channel, dos_model_t model, uint8_t track, uint8_t sector,
                             uint16_t number, uint8_t size, uint8_t position);

// Internal RECORD/POSITN continuation after SSPOS selected a valid data link.
// Retire FIFO copies first. Preserve dirty pages, reuse matching native headers
// and perform SCRUB/STRDBL cooperatively. This does not resolve the side index
// or publish RECORD's final 50/51/ENDCMD. False leaves the current owner intact.
bool dos_rel_channel_position_buffer(dos_unit_t* unit, uint8_t channel, uint16_t number, uint8_t position,
                                     uint8_t track, uint8_t sector);

// Prepare RECORD using its resident side page and native 8250 group cache.
// Retire FIFO copies first. SSPOS reuses the side page, SSEND handles absent
// records, and successful lookups retain POSBUF's dirty-safe window behavior.
// Command completion/status, growth and malformed empty-super traversal remain
// the owner's responsibility. No allocation, ENDCMD or start-time I/O.
bool dos_rel_channel_position(dos_unit_t* unit, uint8_t channel, uint16_t number, uint8_t position);

// Explicitly retry a failed positioning job without replaying RECORD's start.
// Replaced-media cursors cannot be revived. Dirty ownership remains retained.
bool dos_rel_channel_retry_position(dos_unit_t* unit, uint8_t channel);

// Queue native CLSREL after retiring FIFO copies. SCRUB does not pad records.
// Retain the three pages through failed data/index/directory writes and the
// additional host flush. Successful completion frees only this channel.
// Public CLOSE and lifecycle finalization route through this retained owner.
bool dos_rel_channel_close(dos_unit_t* unit, uint8_t channel);

// Explicitly retry retained close I/O, not terminal corruption/stale media.
bool dos_rel_channel_retry_close(dos_unit_t* unit, uint8_t channel);

// Internal WRTOUT job leaf, not NRBUF, CLOSE or a durability acknowledgement.
// Retire read readiness/FIFO copies before cached writes. The owner retains
// the dirty mask and pages. Write only the dirty active data page to its saved
// header, using the existing BAM-version/storage gate. Success clears only
// that mask bit, without advancing the record or changing retained status.
// Failure keeps the bytes/mask and marks this channel failed for explicit
// retry. An already-clean active page performs no I/O. Backing-store flush,
// buffer switching, file flags and complete native error cleanup remain separate.
bool dos_rel_channel_writeback(dos_unit_t* unit, dos_media_bank_t* media, uint8_t channel, uint8_t* dirty);

// Continue a pending dirty NRBUF after its active page's writeback succeeds.
// Select the following page at pointer 2, using its matching resident header
// or at most one explicit read. A previous WRITE forces READ even when the
// saved header matches. No writes, prefetch, record advance or status
// completion. Missing links and dirty pages that would be overwritten block
// without I/O/status changes. Other record modes are unchanged and return false.
// Read/lifetime failure retains the old active page and pending cursor for
// explicit retry. The caller retains both pages and the unchanged dirty mask.
// Native speculative-job timing and controller-error cleanup remain separate.
bool dos_rel_channel_advance_write(dos_unit_t* unit, dos_media_bank_t* media, uint8_t channel, uint8_t dirty);

#define DOS_REL_WRITE_MODIFIED 4u
#define DOS_REL_WRITE_PADDING 8u
#define DOS_REL_WRITE_FINISHED 16u
#define DOS_REL_WRITE_READBACK 32u
#define DOS_REL_WRITE_EOI 64u
#define DOS_REL_WRITE_OVERFLOW 128u

// Internal cached writer ownership, not public OPEN/LISTEN or EOI completion.
// Retire all staged FIFO copies first. Preserve native read flags/counter and
// keep the three pages allocated until CLSREL finishes.
// A ready retained-writer read return may reenter without dropping dirty bits
// or DYFILE. An unfinished EOI, pending job or unexposed completion cannot.
bool dos_rel_channel_begin_write(dos_unit_t* unit, uint8_t channel);

// Store one byte through the channel's retained dirty mask, without I/O.
// Return true only for an actual store. Pending/padding/full/failed writers
// cannot accept bytes. Actual lifetime failure retains ownership and status.
bool dos_rel_channel_put(dos_unit_t* unit, dos_media_bank_t* media, uint8_t channel, uint8_t byte);

// Queue bounded CLREC padding across linked pages, not EOI/next-record/status.
// dos_work services padding and required NRBUF jobs outside the command union.
// FULL stops with its final dirty page still owned, without a flush or CLOSE.
bool dos_rel_channel_pad(dos_unit_t* unit, uint8_t channel);

// Permit one explicit retry of a retained NRBUF WRITE or awaited refill READ.
// No I/O/status reset and no repeated GET or record-counter advancement.
// A retired cached-write cursor cannot be revived after lifetime failure.
bool dos_rel_channel_retry_write(dos_unit_t* unit, uint8_t channel);

// Internal post-CLREC WRTREL completion for cached same-page RD40 only.
// overflow means the EOI-marked byte arrived after FULL, not merely that the
// last accepted byte filled the record. Publish delayed 51 after preparation.
// Otherwise 4040 retains status and DOS 2.7 publishes OK without ENDCMD.
// Retain dirty pages/flags and withhold transport readiness until resume_read.
// FINISHED blocks more input and clean reprepare. No I/O/flush.
// False leaves unsupported NRBUF/uncached continuations and status unchanged.
bool dos_rel_channel_finish_cached(dos_unit_t* unit, dos_media_bank_t* media, uint8_t channel, bool overflow);

// Queue complete existing-record WRTREL completion, including CLREC/NRBUF.
// Publish overflow/OK only after RD40 prepares the next byte. Pending work
// retains the three pages and consumed cursor across explicit job retries.
// overflow requires an EOI byte rejected after the record was already FULL.
// LISTEN retains its byte separately while this continuation completes.
bool dos_rel_channel_finish_write(dos_unit_t* unit, uint8_t channel, bool overflow);

// Restore cached staging/GET after finish_cached, retaining MODIFY ownership,
// pages, dirty/model flags and status. Validate captured media without I/O.
// A dirty NRBUF crossing retires readiness and queues its outgoing WRITE.
// Failure requires explicit retry. Cached SETLST uses the retained native
// window, without treating old WRITE data as a forward-link GET cache.
// Await reused WRITE pages and prefetch only on the native clean branch.
// These jobs retain writer ownership instead of using clean reprepare.
bool dos_rel_channel_resume_read(dos_unit_t* unit, dos_media_bank_t* media, uint8_t channel);

// Resets one native unit's resources/status, without changing mounted media.
void dos_reset(dos_unit_t* unit, dos_model_t model);

// Applies a decoded secondary/OPEN/CLOSE byte after transport synchronization.
// Listening controls input acceptance, not primary device addressing.
void dos_input_address(dos_unit_t* unit, dos_media_bank_t* media, dos_model_t model, uint8_t command, bool listening);

// Accepts one routed input byte and records EOI without executing the request.
void dos_input_put(dos_unit_t* unit, uint8_t byte, bool eoi);

// Route data or collect OPEN/command input. REL retains one popped byte per
// owner across growth/page/EOI jobs. dos_work_pending blocks subsequent RX.
void dos_listen_put(dos_unit_t* unit, dos_media_bank_t* media, uint8_t byte, bool eoi);

// Reports whether an ordered execution boundary has a completed request.
bool dos_input_ready(const dos_unit_t* unit);

// Executes a completed request after the caller retires affected staged output.
void dos_input_dispatch(dos_unit_t* unit, dos_media_bank_t* media, unsigned int index);

// Reports bounded work that must finish before later ordered commands execute.
bool dos_work_pending(const dos_unit_t* unit);

// Resolves a secondary and previews bytes, returning an owner for actual GETs.
// A zero count has no source to retain. Mutations require retiring old sources.
unsigned int dos_output_stage(dos_unit_t* unit, dos_media_bank_t* media, uint8_t secondary, uint8_t* output,
                              unsigned int capacity, bool* eoi, dos_source_t* source);

// Commits actual consumption against the source captured by dos_output_stage.
void dos_output_get(dos_unit_t* unit, dos_media_bank_t* media, dos_model_t model, dos_source_t source,
                    unsigned int count);

// Returns retained status progress for transport diagnostics without exposing text.
dos_status_progress_t dos_status_progress(const dos_unit_t* unit);

// Starts an ordinary-file or directory OPEN. Finish existing ordered work first.
// REL and direct channels remain outside the ordinary-file OPEN path.
void dos_open(dos_unit_t* unit, dos_media_bank_t* media, unsigned int index, uint8_t secondary, const uint8_t* name,
              unsigned int length);

// Advances bounded directory, append-chain, initialize, or command work.
void dos_work(dos_unit_t* unit, dos_media_bank_t* media);

// Persists and releases the secondary, retaining write ownership on I/O failure.
// CLOSE 15 visits data secondaries 14..0. REL queues cooperative CLSREL and
// returns false until work retires. Repeating CLOSE retries a failed CLSREL.
bool dos_close(dos_unit_t* unit, dos_media_bank_t* media, dos_model_t model, uint8_t secondary);

// Finalizes ordinary writers and closes quiescent REL owners without clearing
// errors. Retains failed and not-yet-completed owners for retry.
bool dos_finalize(dos_unit_t* unit, dos_media_bank_t* media);

// Formats a native status, retaining its drive and profile until replaced.
void dos_status(dos_unit_t* unit, dos_model_t model, dos_status_code_t code, uint8_t track, uint8_t sector);

// Returns an ordinary channel index, or DOS_UNUSED if it is not open.
uint8_t dos_find_channel(const dos_unit_t* unit, uint8_t secondary);

// Stages a bounded unread suffix without advancing the native GET cursor.
unsigned int dos_stage(dos_unit_t* unit, dos_media_bank_t* media, uint8_t channel, uint8_t* output,
                       unsigned int capacity, bool* eoi);

// Observes actual FIFO GET progress for a previously staged ordinary source.
void dos_get(dos_unit_t* unit, dos_media_bank_t* media, uint8_t channel, unsigned int count);

// Stages the retained status suffix and acknowledges only actual CR GET.
unsigned int dos_status_stage(const dos_unit_t* unit, uint8_t* output, unsigned int capacity, bool* eoi);
void dos_status_get(dos_unit_t* unit, dos_model_t model, unsigned int count);

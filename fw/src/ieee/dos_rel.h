// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include "ieee/dos_media.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DOS_REL_SIDE_COUNT 6u
#define DOS_REL_POINTER_COUNT 120u
#define DOS_REL_POINTER_OFFSET 16u
#define DOS_REL_SIDE_TABLE_OFFSET 4u
#define DOS_REL_GROUP_TABLE_OFFSET 3u
#define DOS_REL_SIDE_NUMBER_OFFSET 2u
#define DOS_REL_MAX_GROUP 91u
#define DOS_REL_GROUP_SUPER 254u
#define DOS_REL_GROUP_NONE 255u
// RDLG starts at $5A, unlike FNDREL's maximum arithmetic group above.
#define DOS_REL_END_GROUP_START 90u

typedef struct {
    uint8_t group;
    uint8_t side;
    uint8_t index;
    uint8_t offset;
    bool overflow;
} dos_rel_address_t;

typedef enum {
    DOS_REL_LOOKUP_IDLE,
    DOS_REL_LOOKUP_PENDING,
    DOS_REL_LOOKUP_FOUND,
    DOS_REL_LOOKUP_MISSING,
    DOS_REL_LOOKUP_FAILED,
} dos_rel_lookup_result_t;

typedef enum {
    DOS_REL_LOOKUP_ROOT,
    DOS_REL_LOOKUP_GROUP,
    DOS_REL_LOOKUP_SIDE,
} dos_rel_lookup_phase_t;

// No page or file-length-proportional index is owned here. The caller lends one
// shared DOS page for each step and retains the media token for later data I/O.
typedef struct {
    dos_media_ref_t media;
    dos_media_result_t media_result;
    dos_rel_address_t address;
    // Byte-sized dos_rel_lookup_result_t and dos_rel_lookup_phase_t tags.
    uint8_t result;
    uint8_t phase;
    uint8_t track;
    uint8_t sector;
    bool super;
} dos_rel_lookup_t;

// FNDREL/RECORD arithmetic only. Zero record/position aliases one. Overflow
// positions restart at the record's first byte and retain the error indication.
dos_rel_address_t dos_rel_address(dos_model_t model, uint16_t record, uint8_t record_size, uint8_t position);

// CLSREL's native low-word block count from SSEND's group/side/index result.
// Preserve byte-wide subtraction for malformed endpoints and 16-bit rollover.
uint16_t dos_rel_blocks(dos_model_t model, uint8_t group, uint8_t side, uint8_t index);

// NULBUF/ADDNR clear a new data payload and insert native empty-record markers.
// Return the retained byte-wide NR, including its zero/wrap conventions. The
// caller supplies the preceding NR and later installs NULLNK or a forward link.
uint8_t dos_rel_fill(uint8_t bytes[DISKIMAGE_SECTOR_SIZE], uint8_t size, uint8_t next);

typedef struct {
    dos_rel_address_t address;
    uint16_t blocks;
    bool too_large;
} dos_rel_growth_plan_t;

// ADDREL rounds the requested record's end, then computes its native low-word
// allocation preflight from SSEND. This is not a reservation or a corrected
// mathematical sector count. The owner compares blocks with NUMFRE separately.
dos_rel_growth_plan_t dos_rel_growth_plan(dos_model_t model, uint16_t number, uint8_t size, uint8_t group, uint8_t side,
                                          uint8_t index);

// CYEXT enters ADDR1 with the source SSEND address and RELPTR zero, bypassing
// FNDREL. Preserve that endpoint rather than inventing a target record number.
dos_rel_growth_plan_t dos_rel_growth_plan_at(dos_model_t model, dos_rel_address_t address, uint8_t size, uint8_t group,
                                             uint8_t side, uint8_t index);

// Capture the directory's side-sector root and a normalized record address.
// Starting again explicitly abandons the previous result without performing I/O.
void dos_rel_lookup_start(dos_rel_lookup_t* lookup, dos_model_t model, dos_media_ref_t media, uint8_t track,
                          uint8_t sector, dos_rel_address_t address);

// Read at most one index sector. FOUND exposes the data-sector coordinates,
// not a ready record. MISSING is SSTEST's range result, not a completed P/50
// reply (SSEND, data-buffer positioning and record-length checks still follow).
// SSPOS accepts a second ER1 without rereading: malformed data coordinates,
// including track zero, then reach the subsequent data read's geometry check.
// Failed reads may alter scratch but never publish a usable data link.
dos_rel_lookup_result_t dos_rel_lookup_step(dos_rel_lookup_t* lookup, dos_media_bank_t* media,
                                            uint8_t scratch[DISKIMAGE_SECTOR_SIZE]);

// Cancel only this lookup. Borrowed pages, channels and media remain caller-owned.
void dos_rel_lookup_cancel(dos_rel_lookup_t* lookup);

typedef enum {
    DOS_REL_END_IDLE,
    DOS_REL_END_PENDING,
    DOS_REL_END_FOUND,
    DOS_REL_END_CORRUPT,
    DOS_REL_END_NO_GROUP,
    DOS_REL_END_FAILED,
} dos_rel_end_result_t;

// SSEND's selected side page and byte-wide endpoint, not a data-sector lookup.
// Like lookup, this owns no page and retains the token for subsequent I/O.
typedef struct {
    dos_media_ref_t media;
    dos_media_result_t media_result;
    dos_rel_end_result_t result;
    dos_rel_lookup_phase_t phase;
    uint8_t track;
    uint8_t sector;
    uint8_t group;
    uint8_t side;
    uint8_t index;
    bool super;
} dos_rel_end_t;

// Start a cold, clean-index SSEND traversal from the directory's side/super root.
// Resident and dirty native buffer reuse remains the channel owner's concern.
void dos_rel_end_start(dos_rel_end_t* end, dos_model_t model, dos_media_ref_t media, uint8_t track, uint8_t sector);

// Read at most one page. FOUND leaves the last side in scratch, with its native
// group/side/index in end (valid only for the captured media). CORRUPT is SSEND's
// BREAK/67 path, not a host I/O failure. NO_GROUP stops before RDLG's unimplemented
// malformed-table underflow traversal. Neither result fabricates a record reply.
dos_rel_end_result_t dos_rel_end_step(dos_rel_end_t* end, dos_media_bank_t* media,
                                      uint8_t scratch[DISKIMAGE_SECTOR_SIZE]);

// Cancel only this end-index cursor, without releasing or changing shared pages.
void dos_rel_end_cancel(dos_rel_end_t* end);

typedef enum {
    DOS_REL_RECORD_IDLE,
    DOS_REL_RECORD_PENDING,
    DOS_REL_RECORD_READY,
    DOS_REL_RECORD_END,
    DOS_REL_RECORD_NO_INDEX,
    DOS_REL_RECORD_MISSING,
    DOS_REL_RECORD_NEEDS_LINK,
    DOS_REL_RECORD_FAILED,
} dos_rel_record_result_t;

typedef enum {
    DOS_REL_RECORD_INDEX,
    DOS_REL_RECORD_DATA,
    DOS_REL_RECORD_FOLLOWING,
} dos_rel_record_phase_t;

// Records over caller-owned native side/data/data pages. This does not own
// channels, publish status or implement dirty-buffer reuse/growth.
typedef struct {
    dos_rel_lookup_t lookup;
    // Byte-sized dos_rel_record_result_t and dos_rel_record_phase_t tags.
    uint8_t result;
    uint8_t phase;
    uint8_t size;
    uint8_t position;
    uint8_t pointer;
    uint8_t next;
    uint8_t last;
    uint8_t active;
    bool following;
    bool following_failed;
} dos_rel_record_t;

// Start positioning a valid nonzero-length record using the directory root.
// Restart abandons only the cursor. Page ownership remains with the caller.
void dos_rel_record_start(dos_rel_record_t* record, dos_model_t model, dos_media_ref_t media, uint8_t track,
                          uint8_t sector, uint16_t number, uint8_t size, uint8_t position);

// Prepare one record with at most one sector read per call. The same three
// borrowed pages must remain available until cancel/restart. Data pages are
// independent pool entries, not a contiguous 512-byte allocation. NO_INDEX needs the
// caller's SSEND fallback, not an immediate status 50. MISSING is NXOUT's
// truncated-record result. Host failures expose lookup.media_result. A failed
// prefetch is deferred until the end scan or GET actually needs that page.
dos_rel_record_result_t dos_rel_record_step(dos_rel_record_t* record, dos_media_bank_t* media,
                                            uint8_t side[DISKIMAGE_SECTOR_SIZE], uint8_t first[DISKIMAGE_SECTOR_SIZE],
                                            uint8_t following[DISKIMAGE_SECTOR_SIZE]);

// Preview the prepared byte/EOI without consuming it. MISSING previews CR/EOI.
// Validate the captured media token even for cached bytes. Failure leaves both
// outputs unchanged. The caller publishes native status separately.
bool dos_rel_record_peek(dos_rel_record_t* record, const dos_media_bank_t* media,
                         const uint8_t first[DISKIMAGE_SECTOR_SIZE], const uint8_t following[DISKIMAGE_SECTOR_SIZE],
                         uint8_t* byte, bool* eoi);

// Commit one actual GET, with no I/O. END allows record_next to prepare the next
// record. NEEDS_LINK still requires unbuffered native NRBUF handling.
// A missing record retains CR/EOI on repeated GETs until repositioned.
dos_rel_record_result_t dos_rel_record_get(dos_rel_record_t* record, const dos_media_bank_t* media);

// Continue only after END, using native NR and the current low-byte pointer.
// Does no I/O: PENDING schedules clean page reads through record_step. Reuse
// only consumed pages and retain deferred failures until their page is needed.
// The channel owner increments its record number and clears revision-specific
// GET/overflow flags. Missing records remain sticky until repositioning.
dos_rel_record_result_t dos_rel_record_next(dos_rel_record_t* record, const dos_media_bank_t* media,
                                            const uint8_t first[DISKIMAGE_SECTOR_SIZE],
                                            const uint8_t following[DISKIMAGE_SECTOR_SIZE]);

// Cancel only the preparation/read cursor, without flushing or freeing pages.
void dos_rel_record_cancel(dos_rel_record_t* record);

// Caller-owned pages contain the payload. This cursor retains only record
// progress. no_record describes the last actual GET, not a missing preview.
typedef struct {
    dos_rel_record_t record;
    uint16_t number;
    bool get_active;
    bool no_record;
} dos_rel_reader_t;

// Capture RECORD's raw 16-bit number (zero aliases one only for addressing).
// Repositioning clears the prior GET flag and missing-record GET indication.
void dos_rel_reader_start(dos_rel_reader_t* reader, dos_model_t model, dos_media_ref_t media, uint8_t track,
                          uint8_t sector, uint16_t number, uint8_t size, uint8_t position);

// Preview at most the native DOS staging bound, stopping at EOI or an
// unprepared page. Never read sectors, alter pages, commit GET flags/counters,
// or signal error 50. Stale media retires the real record before publishing.
unsigned int dos_rel_reader_stage(dos_rel_reader_t* reader, const dos_media_bank_t* media,
                                  const uint8_t first[DISKIMAGE_SECTOR_SIZE],
                                  const uint8_t following[DISKIMAGE_SECTOR_SIZE], uint8_t* output,
                                  unsigned int capacity, bool* eoi);

// Commit one actual GET from a staged prefix. At EOI, increment the native
// counter modulo 65536 and begin next-record preparation without I/O.
// PENDING is finished through record_step. Only an actual GET of a previously
// missing CR sets no_record (the owner's native error-50 publication event).
dos_rel_record_result_t dos_rel_reader_get(dos_rel_reader_t* reader, dos_model_t model, const dos_media_bank_t* media,
                                           const uint8_t first[DISKIMAGE_SECTOR_SIZE],
                                           const uint8_t following[DISKIMAGE_SECTOR_SIZE]);

#ifdef __cplusplus
}
#endif

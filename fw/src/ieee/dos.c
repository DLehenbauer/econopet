// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "dos.h"

#include <stdio.h>
#include <string.h>

#include "fatal.h"
#include "ieee/ieee_protocol.h"

// Read/write, metadata, direct/block, reset, COPY and NEW/DUPLICATE foundations of the replacement
// DOS. The production facade still selects the legacy engine.
// REL is not enabled here. Sector callbacks and cooperative workers are
// host adaptations, not controller jobs or electrical IEEE handshakes.
//
// The selected ROMs are 4040 revision 2 and shared DOS 2.7 in 8050 MPI or 8250
// MPI mode. Source links below are pinned to cbmsrc
// 01bd60f162ef92212ef0cb67546ae8f42be34168. See docs/dev/drive-read-status.md.

_Static_assert(sizeof(dos_unit_t) <= 4096 + DOS_MEDIA_SLOTS_PER_UNIT * DISKIMAGE_SECTOR_SIZE,
               "native unit state exceeds its native budget plus two BAM sectors");
_Static_assert(DOS_FILE_CHANNEL_COUNT + 2 == DOS_CHANNEL_COUNT, "command and error channels remain reserved");

static uint8_t allocate_channel(dos_unit_t* const unit, const dos_media_ref_t reference, const uint8_t secondary,
                                const unsigned int buffers);

static void free_channel(dos_unit_t* const unit, const uint8_t channel);

static unsigned int directory_stage(dos_unit_t* const unit, dos_media_bank_t* const media, dos_channel_t* const channel,
                                    uint8_t* const output, const unsigned int capacity, bool* const eoi);

static uint8_t* bam_entry(dos_unit_t* const unit, dos_media_bank_t* const media, const dos_media_ref_t reference,
                          const uint8_t track);
static bool free_blocks(dos_unit_t* const unit, dos_media_bank_t* const media, const dos_media_ref_t reference,
                        uint16_t* const blocks);
static bool valid_bitmap(dos_unit_t* const unit, dos_media_bank_t* const media, const dos_media_ref_t reference,
                         const uint8_t track, const uint8_t* const entry);
static bool prepare_mapout_4040(dos_unit_t* const unit, dos_media_bank_t* const media,
                                const dos_channel_t* const channel);
static bool finish_channel(dos_unit_t* const unit, dos_media_bank_t* const media, dos_channel_t* const channel);
static bool finish_directory_growth(dos_unit_t* const unit, dos_media_bank_t* const media,
                                    dos_channel_t* const channel);
static bool flush_bam(dos_unit_t* const unit, dos_media_bank_t* const media, dos_bam_t* const bam);

#define DOS_BAM_VERSION_OFFSET 2u
#define DOS_VERSION_4040 'A'
#define DOS_VERSION_8050 'C'
#define DOS_FILE_REPLACED_MASK 0x20u
#define DOS_DIRECTORY_REPLACE_TRACK 28u
#define DOS_DIRECTORY_REPLACE_SECTOR 29u
#define DOS_DIRECTORY_INTERLEAVE 3u
#define DOS_DATA_INTERLEAVE_4040 10u
#define DOS_DATA_INTERLEAVE_8050 5u
#define DOS_DIRECTORY_TRACK_4040 18u
#define DOS_DIRECTORY_TRACK_8050 39u
#define DOS_DIRECTORY_FIRST_SECTOR 1u
#define DOS_DIRECTORY_TYPE_OFFSET 2u
#define DOS_DIRECTORY_TRACK_OFFSET 3u
#define DOS_DIRECTORY_SECTOR_OFFSET 4u
#define DOS_DIRECTORY_NAME_OFFSET 5u
#define DOS_FILE_CLOSED_MASK 0x80u
#define DOS_FILE_TYPE_MASK 7u
#define DOS_FILE_LOCKED_MASK 0x40u
#define DOS_FILE_TRACK_MASK_4040 0x3fu
#define DOS_FILE_TRACK_PENDING_MASK_4040 0x80u
#define DOS_DIRECTORY_BLOCKS_LOW 30u
#define DOS_DIRECTORY_BLOCKS_HIGH 31u
#define DOS_BAM_TRACK_8050 38u
#define DOS_BAM_TRACKS_PER_PART 50u
#define DOS_DIRECTORY_LINE_SIZE 32u
#define DOS_DIRECTORY_NAME_SIZE 27u
#define DOS_DIRECTORY_PATTERN_DRIVE DISKIMAGE_FILENAME_SIZE
#define DOS_DIRECTORY_PATTERN_LENGTH (DISKIMAGE_FILENAME_SIZE + 1u)
#define DOS_DIRECTORY_PATTERN_FLAGS (DISKIMAGE_FILENAME_SIZE + 2u)
#define DOS_DIRECTORY_PATTERN_SIZE (DISKIMAGE_FILENAME_SIZE + 3u)
#define DOS_PATTERN_WILDCARD 1u
#define DOS_PATTERN_COMPLETE 2u
#define DOS_PATTERN_DEFAULT_DRIVE 0x80u
#define DOS_PATTERN_DRIVE_MASK 1u
#define DOS_PATTERN_TRACK_WILDCARD 0x80u
#define DOS_DIRECTORY_DEFAULT_BOTH_SEARCHES 2u
#define DOS_HEADER_NAME_4040 144u
#define DOS_HEADER_NAME_8050 6u

#define DOS_BAM_PART_INTERLEAVE 3u
#define DOS_RENAME_TARGET 0u
#define DOS_RENAME_SOURCE 1u
#define DOS_RENAME_TARGET_FOUND (1u << DOS_RENAME_TARGET)
#define DOS_RENAME_SOURCE_FOUND (1u << DOS_RENAME_SOURCE)
#define DOS_RENAME_ALL_FOUND (DOS_RENAME_TARGET_FOUND | DOS_RENAME_SOURCE_FOUND)
#define DOS_DIRECTORY_SIDE_TRACK 21u
#define DOS_DIRECTORY_SIDE_SECTOR 22u
#define DOS_INTERNAL_READ_SECONDARY 17u
#define DOS_DECIMAL_PAIR_LIMIT 100u
#define DOS_INTERNAL_WRITE_SECONDARY 18u
#define DOS_COPY_DEFAULT_DRIVE 0x80u
#define DOS_COPY_PATTERN_INDEX 2u
#define DOS_COPY_PATTERN_FLAG 0x80u
// CPYDTD moves source text to end at 40. TRFNME overwrites bytes 3..18.
#define DOS_COPY_PATTERN_TEXT_LIMIT 21u

typedef enum {
    DOS_COPY_SEARCH,
    DOS_COPY_MATCH_WAIT,
    DOS_COPY_APPEND,
    DOS_COPY_SOURCE,
    DOS_COPY_BYTES,
    DOS_COPY_CLOSE,
    DOS_COPY_ALL_NEXT,
} dos_copy_phase_t;

#define DOS_DUPLICATE_LAST_BUFFER 10u
#define DOS_DIRECTORY_LAST_MARKER 0xffu
#define DOS_DISK_ID_OFFSET 18u
#define DOS_DISK_VERSION_OFFSET 21u

typedef enum {
    DOS_FORMAT_INITIALIZE,
    DOS_FORMAT_START,
    DOS_FORMAT_ERASE,
    DOS_FORMAT_MAP,
    DOS_FORMAT_DIRECTORY,
    DOS_FORMAT_DISK_HEADER,
    DOS_FORMAT_BAM,
    DOS_FORMAT_CHAIN,
    DOS_FORMAT_RESERVE,
    DOS_FORMAT_RELOAD,
    DOS_FORMAT_COPY_READ,
    DOS_FORMAT_COPY_WRITE,
    DOS_FORMAT_COPY_FINISH,
} dos_format_phase_t;

typedef enum {
    DOS_SCRATCH_SEARCH,
    DOS_SCRATCH_CHAIN,
    DOS_SCRATCH_BAM,
} dos_scratch_phase_t;
typedef enum {
    DOS_VALIDATE_INITIALIZE,
    DOS_VALIDATE_MAP,
    DOS_VALIDATE_SEARCH,
    DOS_VALIDATE_CHAIN,
    DOS_VALIDATE_WRITE,
    DOS_VALIDATE_RECOVER,
} dos_validate_phase_t;

#define DOS_BLOCK_PARAMETER_SKIP 29u
#define DOS_BLOCK_FIRST_SECONDARY 2u
#define DOS_BLOCK_PARAMETER_START 3u
#define DOS_BLOCK_MIN_COUNT 1u
#define DOS_USER_SELECTOR_MASK 0x0fu
#define DOS_USER_WARM_RESET_INDEX 8u
#define DOS_USER_RESET_INDEX 9u
#define DOS_USER_INTERRUPT_INDEX 10u
#define DOS_SINGLETON_LAST_PHASE 4u
#define DOS_USER_BLOCK_READ_INDEX 0u
#define DOS_USER_BLOCK_WRITE_INDEX 1u

typedef struct {
    uint8_t patterns[DOS_FILENAME_COUNT][DOS_DIRECTORY_PATTERN_SIZE];
    uint8_t count;
    uint8_t type;
    uint8_t drive;
    uint8_t searches;
} dos_directory_request_t;

// Select expanded ROM status text for the codes implemented by this slice.
static const char* status_text(const dos_status_code_t code, const dos_model_t model) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/erproc
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/erproc
    //
    // In both erproc units, ERRTAB associates BCD error numbers with compressed
    // strings. MOVERR (4040) or ERMOVE (DOS 2.7) expands tokens, including their
    // implied spaces. This switch stores the already-expanded spellings, so the
    // leading space in " OK" and " FILE NOT FOUND" is significant. Decimal C
    // enum values replace the ROM's BCD numbers.
    switch (code) {
    case DOS_STATUS_OK:
        return " OK";
    case DOS_STATUS_FILES_SCRATCHED:
        return " FILES SCRATCHED";
    case DOS_STATUS_READ_ERROR:
        return "READ ERROR";
    case DOS_STATUS_WRITE_ERROR:
        return " WRITE ERROR";
    case DOS_STATUS_WRITE_PROTECT:
        return " WRITE PROTECT ON";
    // ROM ERRTAB shares SYNTAX ERROR across these syntax codes.
    case DOS_STATUS_SYNTAX:
    case DOS_STATUS_INVALID_COMMAND:
    case DOS_STATUS_COMMAND_TOO_LONG:
    case DOS_STATUS_INVALID_NAME:
    case DOS_STATUS_NO_FILE:
        return "SYNTAX ERROR";
    case DOS_STATUS_NO_RECORD:
        return " RECORD NOT PRESENT";
    case DOS_STATUS_RECORD_OVERFLOW:
        return "OVERFLOW IN RECORD";
    case DOS_STATUS_FILE_TOO_LARGE:
        return "FILE TOO LARGE";
    case DOS_STATUS_FILE_OPEN:
        return " WRITE FILE OPEN";
    case DOS_STATUS_NOT_FOUND:
        return " FILE NOT FOUND";
    case DOS_STATUS_FILE_EXISTS:
        return " FILE EXISTS";
    case DOS_STATUS_TYPE_MISMATCH:
        return " FILE TYPE MISMATCH";
    case DOS_STATUS_NO_BLOCK:
        return "NO BLOCK";
    case DOS_STATUS_ILLEGAL_TRACK_SECTOR:
    case DOS_STATUS_ILLEGAL_SYSTEM_TRACK_SECTOR:
        return "ILLEGAL TRACK OR SECTOR";
    case DOS_STATUS_NO_CHANNEL:
        return "NO CHANNEL";
    case DOS_STATUS_DIRECTORY_ERROR:
        return "DIR ERROR";
    case DOS_STATUS_DISK_FULL:
        return " DISK FULL";
    // ROM ERRTAB's code 73 contains the model-specific version string.
    case DOS_STATUS_POWER_ON:
        return model == DOS_MODEL_4040 ? "CBM DOS V2" : "CBM DOS V2.7";
    // Code 74 exists in DOS 2.7 ERRTAB, not the pinned 4040 table. Using
    // it for an absent 4040 image is M2 policy, not native-ROM behavior.
    case DOS_STATUS_NOT_READY:
        return "DRIVE NOT READY";
    }

    // Unsupported enum values are programming errors, not ROM messages.
    fatal("unsupported native status %u", code);
}

// Publish a fresh error-channel record, starting at its first unread byte.
// Publishing replaces the retained status without consuming any bytes.
void dos_status(dos_unit_t* const unit, const dos_model_t model, const dos_status_code_t code, const uint8_t track,
                const uint8_t sector) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/erproc
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/erproc
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4
    //
    // ROM erproc:ERRMSG/ERMSG2 (4040) and JERRMS/ERMSG2 (DOS 2.7) write the
    // error number, expanded message, track, and sector with comma separators.
    // BCDDEC/HEXDEC retain two decimal digits on 4040, while DOS 2.7 emits
    // an optional hundreds digit. DOS 2.7 appends DRVNUM masked to one bit,
    // whereas 4040 ends after the sector. tst4:GETERC supplies CR and EOI.
    // Here snprintf combines formatting and that eventual CR in one record,
    // preserving the model-specific drive field and using expanded status_text.
    const unsigned int status_track = model == DOS_MODEL_4040 ? track % DOS_DECIMAL_PAIR_LIMIT : track;
    const unsigned int status_sector = model == DOS_MODEL_4040 ? sector % DOS_DECIMAL_PAIR_LIMIT : sector;
    const int length = model == DOS_MODEL_4040
                           ? snprintf(unit->status, sizeof(unit->status), "%02u,%s,%02u,%02u\r", code,
                                      status_text(code, model), status_track, status_sector)
                           : snprintf(unit->status, sizeof(unit->status), "%02u,%s,%02u,%02u,%u\r", code,
                                      status_text(code, model), status_track, status_sector, unit->selected_drive);
    // Detect internal overflow rather than publishing a truncated status.
    vet(length > 0 && (size_t)length < sizeof(unit->status), "native status overflow");

    // ROM error-buffer pointer/last-character state becomes a retained length
    // and unread position. Replacing the record restarts it without GET progress.
    unit->status_length = (uint8_t)length;
    unit->status_position = 0;
    unit->status_code = code;
}

// CLRCB retires logical command RAM without changing status or channel ownership.
static void clear_command(dos_unit_t* const unit) {
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/parsex
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/parsex
    memset(unit->command_bytes, 0, DOS_COMMAND_LIMIT);
}

// Remember the successful command's drive and publish a fresh OK status.
// Call only after success, not for errors or nonzero completion statuses.
static void command_end(dos_unit_t* const unit, const dos_model_t model) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/parsex
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/parsex
    //
    // ENDCMD checks ERWORD before entering SCREND with zero status and
    // track/sector. SCREND formats status and copies DRVNUM to LSTDRV.
    // C callers select this success-only path instead of testing ERWORD here.
    // LED handling, internal-channel release, and
    // DOS 2.7's NODRV clearing are not performed by this helper.

    // Retain DRVNUM as LSTDRV for later default-drive selection. C saves it
    // before formatting status rather than after the ROM's ERRMSG call.
    unit->last_drive = unit->selected_drive;

    // Replace the retained response with model-specific OK and zero
    // track/sector, restarting its unread cursor without consuming bus data.
    dos_status(unit, model, DOS_STATUS_OK, 0, 0);

    // CLRCB clears CMDLEN bytes, leaving the extra receive byte untouched.
    clear_command(unit);
}

// Retire a failed command without changing its last-successful drive.
static void command_error(dos_unit_t* const unit, const dos_model_t model, const uint8_t secondary,
                          const dos_status_code_t code) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/erproc
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/erproc
    //
    // CMDERR/CMDER3 publish the error and call CLRCB without updating LSTDRV.
    // Keep dos_status itself independent of command retirement: an older
    // reader can report an error while a different OPEN is pending.
    dos_status(unit, model, code, 0, 0);
    clear_command(unit);

    // DOS 2.7 CMDER3 -> TLERR frees this SA's ordinary non-REL owner even
    // with no active bus role. DIRTYP exempts direct pages. A listing's
    // file_type is its filter, not TYPFIL, so a REL filter is not exempt.
    if ((model == DOS_MODEL_8050 || model == DOS_MODEL_8250) && secondary != IEEE_COMMAND_CHANNEL) {
        const uint8_t channel = dos_find_channel(unit, secondary);
        if (channel != DOS_UNUSED && unit->channels[channel].access != DOS_ACCESS_DIRECT &&
            (unit->channels[channel].directory != DOS_DIRECTORY_NONE ||
             unit->channels[channel].file_type < DISKIMAGE_FTYPE_REL)) {
            free_channel(unit, channel);
        }
    }
}

// Reset this unit's DOS resources and expose its power-on status.
// Mounted media survives. This is not a full hardware drive reset.
void dos_reset(dos_unit_t* const unit, const dos_model_t model) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/master
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/master
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/equate
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/equate
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/dskint
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/begin
    //
    // ROM 4040 dskint:INTTAB/SETERR and DOS_8250 begin:INTTAB initialize LINTAB,
    // BUF0/BUF1, LINUSE, BUFUSE, and command/error channel readiness. Ordinary
    // indices 0..5 and twelve data buffers are available, with indices 6 and 7
    // reserved for command and error channels.

    // Clearing the C state makes every ordinary allocation, cache residency,
    // cursor, and pending DOS work inactive. Zero-based structs replace the ROM's
    // sentinel values and resource bit-table initialization.
    memset(unit, 0, sizeof(*unit));

    // The ROM startup error is 73. Publish its model-specific version record.
    dos_status(unit, model, DOS_STATUS_POWER_ON, 0, 0);

    // Media bindings are outside unit and survive. RAM/ROM diagnostics,
    // peripheral initialization, head bumps, and full drive reset are not done.
}

// Resolve an already-decoded ordinary secondary address to a logical index.
// Return DOS_UNUSED if it is not open. dos_output_stage handles status routing.
uint8_t dos_find_channel(const dos_unit_t* const unit, const uint8_t secondary) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
    //
    // ROM tst2:FNDRCH indexes LINTAB by SA, rejects an unavailable read channel,
    // and extracts LINDX. Scan the six allocated C records for the saved SA
    // instead. Unlike FNDRCH, this does not mask raw command bytes or remap
    // command SA 15 to the error channel. The caller owns that decoding.
    for (uint8_t channel = 0; channel < DOS_FILE_CHANNEL_COUNT; ++channel) {
        if (unit->channels[channel].allocated && unit->channels[channel].secondary == secondary) {
            return channel;
        }
    }

    // Represent the ROM's unavailable-channel result with an explicit sentinel.
    return DOS_UNUSED;
}

// Release one allocated channel and all its owned data/listing buffers.
// The caller must supply an allocated channel with valid buffer indices.
static void free_channel(dos_unit_t* const unit, const uint8_t channel) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst3
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst3
    //
    // ROM tst3:FRECHN invalidates LINTAB, RELBUF returns BUF0/BUF1 through
    // FREBUF, and RELINX frees LINDX. C also returns a listing's pattern page.
    dos_channel_t* const state = &unit->channels[channel];

    // Return all owned buffers to the C pool and invalidate cached contents,
    // corresponding to RELBUF/FREBUF without the ROM's rotating bitmap.
    for (unsigned int index = 0; index < count_of(state->buffers); ++index) {
        if (state->buffers[index] == DOS_UNUSED) {
            continue;
        }
        unit->buffer_use &= (uint16_t)~(1u << state->buffers[index]);
        unit->buffers[state->buffers[index]].resident = false;
    }

    // Clearing allocated replaces LINTAB invalidation and LINUSE release.
    if (state->directory != DOS_DIRECTORY_NONE) {
        unit->directory_internal = 0;
    }
    *state = (dos_channel_t){0};
}

// INITDR/CLDCHN releases owners selected by their active page's last job drive.
static void free_drive_channels(dos_unit_t* const unit, const uint8_t drive) {
    // Both pinned ROMs: tst2:INITDR -> tst3:CLDCHN -> getact:GETACT.
    for (uint8_t candidate = IEEE_COMMAND_CHANNEL; candidate-- > IEEE_LOAD_CHANNEL;) {
        const uint8_t owner = dos_find_channel(unit, candidate);
        if (owner != DOS_UNUSED) {
            const dos_channel_t* const channel = &unit->channels[owner];
            const uint16_t page = 1u << channel->buffers[channel->active];
            if (((unit->buffer_drives & page) != 0) == (drive != 0)) {
                free_channel(unit, owner);
            }
        }
    }
}

// Select CLSALL's highest remaining data secondary, or one explicit owner.
static uint8_t close_channel(const dos_unit_t* const unit, const uint8_t secondary) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/close
    //
    // CLSALL/CLS20 visits SA 14 down to zero. This host selector skips absent
    // owners so cooperative CLOSE resumes at the highest remaining secondary.
    if (secondary != IEEE_COMMAND_CHANNEL) {
        return dos_find_channel(unit, secondary);
    }
    for (uint8_t candidate = IEEE_COMMAND_CHANNEL; candidate-- > IEEE_LOAD_CHANNEL;) {
        const uint8_t index = dos_find_channel(unit, candidate);
        if (index != DOS_UNUSED) {
            return index;
        }
    }
    return DOS_UNUSED;
}

// Close one secondary or continue CLSALL in native descending-SA order.
bool dos_close(dos_unit_t* const unit, dos_media_bank_t* const media, const dos_model_t model,
               const uint8_t secondary) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/close
    //
    // ROM close:CLS10/CLSCHN close one ordinary channel. CLSALL iterates the
    // data secondaries for SA 15. Select the matching allocated C record or
    // all data records, then release their resources through free_channel.
    // Missing records need no release. Command/error storage is not released.
    // Ordinary writers persist synchronously. Host I/O failures retain their
    // owners and the unclosed peers for a later retry.
    uint8_t channel;
    while ((channel = close_channel(unit, secondary)) != DOS_UNUSED) {
        dos_channel_t* const state = &unit->channels[channel];
        // Explicit CLOSE 0 enters FRECHN directly, unlike a CLSALL visit.
        if (secondary != IEEE_LOAD_CHANNEL && state->access == DOS_ACCESS_DIRECT && model == DOS_MODEL_4040 &&
            state->closing == DOS_CLOSE_DATA && !prepare_mapout_4040(unit, media, state)) {
            return false;
        }
        if (secondary != IEEE_LOAD_CHANNEL && state->access != DOS_ACCESS_READ && !finish_channel(unit, media, state)) {
            return false;
        }
        free_channel(unit, channel);
    }

    // CLOSE 0/1 bypass ENDCMD, so a SAVE warning survives its CLOSE.
    if (secondary > IEEE_SAVE_CHANNEL) {
        command_end(unit, model);
    }
    return true;
}

// Claim the highest-numbered free data buffer from the shared unit pool.
// Return DOS_UNUSED when the pool is exhausted.
static uint8_t get_buffer(dos_unit_t* const unit) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst3
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst3
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/dskint
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/begin
    //
    // ROM tst3:GETBUF (DOS 2.7 JGETBF) rotates BUFUSE from index 15 down and
    // claims the first free entry. Startup reserves 12..15, leaving 11..0.
    // Scan only those twelve C data entries and set the chosen ownership bit.
    for (unsigned int index = DOS_BUFFER_COUNT; index-- > 0;) {
        if (!(unit->buffer_use & (1u << index))) {
            unit->buffer_use |= (uint16_t)(1u << index);

            // ROM GETBU4 clears the job and saves DRVNUM. The approved 4040
            // overlay and DOS 2.7 first wait for its old job to finish.
            // C reads are synchronous, so no job remains. Invalidate residency
            // so the previous owner's bytes cannot satisfy the next request.
            unit->buffers[index].resident = false;
            unit->buffers[index].failed = false;
            unit->buffer_writes &= (uint16_t)~(1u << index);
            if (unit->selected_drive != 0) {
                unit->buffer_drives |= (uint16_t)(1u << index);
            } else {
                unit->buffer_drives &= (uint16_t)~(1u << index);
            }
            return (uint8_t)index;
        }
    }

    // Replace the ROM's negative no-buffer result with an explicit sentinel.
    return DOS_UNUSED;
}

// Record a native data page's last job drive, independent of its allocation drive.
// Return its pool bit, or zero for storage outside the unit's data-page pool.
static uint16_t remember_buffer_drive(dos_unit_t* const unit, const dos_media_ref_t reference,
                                      const uint8_t* const bytes) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tsutil
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tsutil
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/jobs
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/jobs
    //
    // RDAB/WRTAB enter SJ10, which stores DRVNUM in the active page's LSTJOB.
    // SETLJB uses LSTJOB bit 0 to select the job's drive. The job wrappers
    // separately retain the READ/WRITE distinction needed by TSTWRT.
    // This pointer-to-page lookup is a host adaptation with no direct ROM entry.

    // Match the page's base address exactly. Temporary sector arrays and BAM
    // storage have no entry in this pool and require no data-page bookkeeping.
    for (unsigned int buffer = 0; buffer < DOS_BUFFER_COUNT; ++buffer) {
        if (bytes == unit->buffers[buffer].bytes) {
            // Replace only this page's drive bit, preserving all other pages.
            const uint16_t bit = 1u << buffer;
            unit->buffer_drives = (unit->buffer_drives & (uint16_t)~bit) | (reference.slot != 0 ? bit : 0);
            return bit;
        }
    }
    return 0;
}

// Record the target drive for a pooled data page, then attempt one sector read.
// Return the media result unchanged, leaving status reporting to the caller.
static dos_media_result_t read_sector(dos_unit_t* const unit, dos_media_bank_t* const media,
                                      const dos_media_ref_t reference, const uint8_t track, const uint8_t sector,
                                      uint8_t* const bytes) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tsutil
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tsutil
    //
    // RDAB sets READ and enters SJ10 to save DRVNUM before submitting the
    // controller job. This synchronous host wrapper has no direct ROM entry
    // and does not reproduce controller scheduling or retry loops.

    // Record the attempted drive before I/O, even if media validation or the
    // read fails. Buffers outside the data-page pool leave the bitset unchanged.
    unit->buffer_writes &= (uint16_t)~remember_buffer_drive(unit, reference, bytes);

    // Delegate identity/geometry checks and the backing-store callback without
    // publishing cache residency, advancing a channel, or translating errors.
    return dos_media_read(media, reference, track, sector, bytes);
}

// Make the requested sector resident in the channel's active data buffer.
// Reset its payload cursor. Return false on failure for the caller to report.
static bool load_buffer(dos_unit_t* const unit, dos_media_bank_t* const media, dos_channel_t* const channel,
                        const uint8_t track, const uint8_t sector) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/opchnl
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/opchnl
    //
    // ROM tst2:STRDBL/RDBUF/SETHDR attach track, sector, and drive identity to
    // an active buffer job and wait before using its bytes. Select the active
    // C buffer and reuse it only when resident at the requested coordinates.
    dos_buffer_t* const buffer = &unit->buffers[channel->buffers[channel->active]];
    if (!buffer->resident || buffer->track != track || buffer->sector != sector) {
        // Read through the saved media reference, which rejects remounted
        // images before I/O (a host lifetime rule, not a ROM field). This
        // callback does not reproduce controller retries, prefetch timing,
        // disk-ID checks, or exact controller-error translation.
        buffer->failed = read_sector(unit, media, channel->media, track, sector, buffer->bytes) != DOS_MEDIA_OK;

        // Retain the completed job's coordinates and result, including failure.
        // A cancelled preview must not retry a failed page and hide its error.
        buffer->track = track;
        buffer->sector = sector;
        buffer->resident = true;
    }
    if (buffer->failed) {
        return false;
    }

    // ROM opchnl:INTPNT sets data pointers to 2, past the sector link.
    // Reset the C payload position on both cache hits and misses.
    channel->position = DISKIMAGE_SECTOR_LINK_SIZE;
    return true;
}

// Select the native directory/header track for the mounted drive model.
static uint8_t directory_track(const dos_model_t model) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lookup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/begin
    //
    // 4040 SRCHST uses track 18. DOS 2.7 CONFIG sets DIRTRK to 39 for
    // both hardware modes, so the 8250 shares the 8050 directory location.
    return model == DOS_MODEL_4040 ? DOS_DIRECTORY_TRACK_4040 : DOS_DIRECTORY_TRACK_8050;
}

// Translate a backing-store failure without releasing the failed operation.
static bool storage_error(dos_unit_t* const unit, dos_media_bank_t* const media, const dos_media_ref_t reference,
                          const dos_media_result_t result, const bool writing, const uint8_t track,
                          const uint8_t sector) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/erproc
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/erproc
    //
    // Success must not overwrite retained status. Host failures use a bounded
    // write-protect/read/write mapping, not the ROM's controller error decoding.
    if (result == DOS_MEDIA_OK) {
        return true;
    }
    // Retain the failed drive and coordinates without releasing buffers or
    // advancing the caller's persistence phase, allowing an in-place retry.
    unit->selected_drive = reference.slot;
    dos_status(unit, media->units[reference.unit].model,
               result == DOS_MEDIA_READ_ONLY ? DOS_STATUS_WRITE_PROTECT
               : writing                     ? DOS_STATUS_WRITE_ERROR
                                             : DOS_STATUS_READ_ERROR,
               track, sector);
    return false;
}

// SETMAP/MBAM follow the DOS 2.7 BAM links and select a track's count/bitmap.
static uint8_t* bam_entry(dos_unit_t* const unit, dos_media_bank_t* const media, const dos_media_ref_t reference,
                          const uint8_t track) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tstfnd
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/frets
    //
    // SETBMP/AVAIL address the fixed 4040 BAM. SETMAP/MBAM select a DOS 2.7
    // part by its low/high track bounds and multiply by its five-byte stride.
    const dos_model_t model = media->units[reference.unit].model;
    dos_media_ref_t current;
    // Media identities are a host lifetime guard, not an on-disk ROM field.
    if (dos_media_reference(media, reference.unit, reference.slot, &current) != DOS_MEDIA_OK ||
        current.identity != reference.identity) {
        storage_error(unit, media, reference, DOS_MEDIA_STALE, false, track, 0);
        return NULL;
    }
    dos_bam_t* const bam = &unit->bam[reference.slot];
    if (model != DOS_MODEL_4040 && bam->buffer.resident && bam->media.identity == reference.identity) {
        const unsigned int low = bam->buffer.bytes[4], high = bam->buffer.bytes[5];
        if (low != 0 && low < high && high - low <= DOS_BAM_TRACKS_PER_PART && track >= low && track < high) {
            return bam->buffer.bytes + 6u + (track - low) * 5u;
        }
    }
    // Start at the first BAM and follow links only when the cached part
    // cannot serve this track. Bound traversal as a corrupt-image safeguard.
    uint8_t bam_track = model == DOS_MODEL_4040 ? DOS_DIRECTORY_TRACK_4040 : DOS_BAM_TRACK_8050;
    uint8_t bam_sector = 0;
    for (unsigned int part = 0; part < 4; ++part) {
        if (!bam->buffer.resident || bam->media.identity != reference.identity || bam->buffer.track != bam_track ||
            bam->buffer.sector != bam_sector) {
            if (!flush_bam(unit, media, bam)) {
                return NULL;
            }
            uint8_t bytes[DISKIMAGE_SECTOR_SIZE];
            if (!storage_error(unit, media, reference,
                               read_sector(unit, media, reference, bam_track, bam_sector, bytes), false, bam_track,
                               bam_sector)) {
                return NULL;
            }
            // Preserve the old cache on failed reads, publishing only a full
            // successful replacement after its dirty predecessor was flushed.
            if (bam->media.identity != reference.identity) {
                unit->free_blocks_valid[reference.slot] = false;
            }
            memcpy(bam->buffer.bytes, bytes, sizeof(bytes));
            bam->buffer.track = bam_track;
            bam->buffer.sector = bam_sector;
            bam->buffer.resident = true;
            bam->media = reference;
        }
        if (model == DOS_MODEL_4040) {
            if (track != 0 && track <= DISKIMAGE_D64_TRACK_COUNT) {
                return bam->buffer.bytes + track * 4u;
            }
            break;
        }
        const unsigned int low = bam->buffer.bytes[4], high = bam->buffer.bytes[5];
        if (low != 0 && low < high && high - low <= DOS_BAM_TRACKS_PER_PART && track >= low && track < high) {
            return bam->buffer.bytes + 6u + (track - low) * 5u;
        }
        // RDBAM/RDNBAM use the part's link. A directory link is not a BAM.
        bam_track = bam->buffer.bytes[0];
        bam_sector = bam->buffer.bytes[1];
        if (bam_track == 0 || (bam_track == directory_track(model) && bam_sector == DOS_DIRECTORY_FIRST_SECTOR)) {
            break;
        }
    }
    unit->selected_drive = reference.slot;
    dos_status(unit, model, DOS_STATUS_DIRECTORY_ERROR, track, 0);
    return NULL;
}

// COMPAR treats '*' as an immediate suffix match and '?' as one filename byte.
static bool name_matches(const uint8_t* const pattern, const uint8_t length, const uint8_t* const name) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lookup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lookup
    //
    // COMPAR operates on fixed-width PETSCII names without case folding.
    // '*' accepts the remaining suffix, while '?' cannot extend past $A0.
    for (unsigned int index = 0; index < length; ++index) {
        if (pattern[index] == '*') {
            return true;
        }
        if (pattern[index] == '?' && name[index] != DISKIMAGE_FILENAME_PADDING) {
            continue;
        }
        if (pattern[index] != name[index]) {
            return false;
        }
    }
    // CP34 tests padding only after every explicitly supplied byte matched.
    return length == DISKIMAGE_FILENAME_SIZE || name[length] == DISKIMAGE_FILENAME_PADDING;
}

// NUMFRE totals native data-track counts, excluding the directory track only.
static bool free_blocks(dos_unit_t* const unit, dos_media_bank_t* const media, const dos_media_ref_t reference,
                        uint16_t* const blocks) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lstdir
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/map
    //
    // 4040 NUMFRE sums BAM counts except track 18. DOS 2.7 NUMFRE returns
    // maintained NDBL/NDBH totals. This read-only slice lazily sums each
    // model's data tracks and retains the total for this media identity.
    const dos_model_t model = media->units[reference.unit].model;
    const unsigned int tracks = model == DOS_MODEL_4040   ? DISKIMAGE_D64_TRACK_COUNT
                                : model == DOS_MODEL_8050 ? DISKIMAGE_D80_TRACK_COUNT
                                                          : DISKIMAGE_D82_TRACK_COUNT;
    if (unit->free_blocks_valid[reference.slot] && unit->bam[reference.slot].media.identity == reference.identity) {
        *blocks = unit->free_blocks[reference.slot];
        return true;
    }
    // A partial refill must not make an older total valid for this identity.
    unit->free_blocks_valid[reference.slot] = false;
    *blocks = 0;
    for (unsigned int track = 1; track <= tracks; ++track) {
        if (track == directory_track(model)) {
            continue;
        }
        const uint8_t* const entry = bam_entry(unit, media, reference, (uint8_t)track);
        if (entry == NULL) {
            return false;
        }
        *blocks += entry[0];
    }
    // Publish the cache only after every required BAM part was readable.
    unit->free_blocks[reference.slot] = *blocks;
    unit->free_blocks_valid[reference.slot] = true;
    return true;
}

// Allocate the lowest logical index and GETRCH's requested number of data buffers.
static uint8_t allocate_channel(dos_unit_t* const unit, const dos_media_ref_t reference, const uint8_t secondary,
                                const unsigned int buffers) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst3
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst3
    //
    // GETRCH/FNDLNX select a logical index before GETBUF claims buffers.
    // Exclude the directory's internal logical reservation from that scan.
    uint8_t channel = 0;
    while (channel < DOS_FILE_CHANNEL_COUNT &&
           (unit->channels[channel].allocated || unit->directory_internal == channel + 1u)) {
        ++channel;
    }
    if (channel == DOS_FILE_CHANNEL_COUNT) {
        return DOS_UNUSED;
    }
    // Roll back a partial claim without publishing an allocated channel.
    uint8_t claimed[DOS_CHANNEL_BUFFER_COUNT] = {DOS_UNUSED, DOS_UNUSED, DOS_UNUSED};
    vet(buffers <= count_of(claimed), "invalid native channel buffer count");
    for (unsigned int index = 0; index < buffers; ++index) {
        claimed[index] = get_buffer(unit);
        if (claimed[index] == DOS_UNUSED) {
            for (unsigned int prior = 0; prior < index; ++prior) {
                unit->buffer_use &= (uint16_t)~(1u << claimed[prior]);
            }
            return DOS_UNUSED;
        }
    }
    // Publish ownership together. Callers specialize read/write readiness
    // and payload position once the initial sector has been prepared.
    unit->channels[channel] = (dos_channel_t){
        .media = reference,
        .buffers = {claimed[0], claimed[1], claimed[2]},
        .secondary = secondary,
        .allocated = true,
        .ready = true,
        .traversed = 1,
    };
    for (unsigned int index = 0; index < buffers; ++index) {
        const uint16_t bit = 1u << unit->channels[channel].buffers[index];
        if (reference.slot != 0) {
            unit->buffer_drives |= bit;
        } else {
            unit->buffer_drives &= (uint16_t)~bit;
        }
    }
    return channel;
}

// Reserve native REL data/data/side ownership before any record is published.
uint8_t dos_rel_channel_claim(dos_unit_t* const unit, const dos_media_ref_t reference, const uint8_t secondary) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/opchnl
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/opchnl
    //
    // OPNRCH obtains two GETRCH pages, then a third GETBUF page for SS.
    // GBERR/RELBUF return partial claims on exhaustion. This host claim
    // retains the existing no-publication-before-success allocation policy.
    vet(dos_find_channel(unit, secondary) == DOS_UNUSED, "REL claim requires an unowned secondary");
    const uint8_t channel = allocate_channel(unit, reference, secondary, DOS_CHANNEL_BUFFER_COUNT);
    if (channel != DOS_UNUSED) {
        dos_channel_t* const state = &unit->channels[channel];
        state->file_type = DISKIMAGE_FTYPE_REL;
        state->ready = false;
        state->rel = (dos_rel_reader_t){0};
    }
    return channel;
}

// Check the cached BAM's DOS version before allowing a sector write.
// Return false with retained status for stale media, BAM failure, or a foreign version.
static bool write_version(dos_unit_t* const unit, dos_media_bank_t* const media, const dos_media_ref_t reference,
                          const uint8_t track, const uint8_t sector) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/jobs
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/jobs
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
    //
    // SETJOB/SJB2/SJB3 read the selected drive's BAM0/1+2 for WRITE jobs.
    // Zero bypasses the version comparison, otherwise VERNUM must match.
    // This helper extracts that gate, not SETJOB's geometry checks or job setup.
    const dos_model_t model = media->units[reference.unit].model;
    dos_bam_t* const bam = &unit->bam[reference.slot];
    dos_media_ref_t current = {0};

    // Media generations are a host lifetime guard. Reject a stale reference
    // before consulting cached bytes from a removed or replacement image.
    if (dos_media_reference(media, reference.unit, reference.slot, &current) != DOS_MEDIA_OK ||
        current.identity != reference.identity) {
        return storage_error(unit, media, reference, DOS_MEDIA_STALE, true, track, sector);
    }
    if (bam->media.identity != reference.identity || !bam->buffer.resident) {
        // Lazy loading is a host adaptation. 4040 INITDR leaves its single
        // BAM resident. DOS 2.7 INITDR first reads the directory header, then
        // NF05/NF10 use SETMAP through the final track, leaving the last BAM
        // part cached. Select that part only when this image has no valid cache.
        const uint8_t last_track = model == DOS_MODEL_4040   ? DISKIMAGE_D64_TRACK_COUNT
                                   : model == DOS_MODEL_8050 ? DISKIMAGE_D80_TRACK_COUNT
                                                             : DISKIMAGE_D82_TRACK_COUNT;
        if (bam_entry(unit, media, reference, last_track) == NULL) {
            return false;
        }
    }

    // Test the currently resident page, not a freshly read disk header.
    // SJB3/SJB4 accept an unspecified zero version as well as the model's own.
    const uint8_t version = bam->buffer.bytes[DOS_BAM_VERSION_OFFSET];
    if (version != 0 && version != (model == DOS_MODEL_4040 ? DOS_VERSION_4040 : DOS_VERSION_8050)) {
        // VNERR calls HED2TS before reporting 73, preserving the rejected
        // job's coordinates. The text matches startup, but not its 00,00 fields.
        dos_status(unit, model, DOS_STATUS_POWER_ON, track, sector);
        return false;
    }
    return true;
}

// Write a native sector, retaining its caller-owned bytes after any failure.
static bool write_sector(dos_unit_t* const unit, dos_media_bank_t* const media, const dos_media_ref_t reference,
                         const uint8_t track, const uint8_t sector, const uint8_t* const bytes) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tsutil
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tsutil
    //
    // WRTOUT/WRTAB submit controller writes. The C media callback completes
    // synchronously and storage_error publishes a host-policy failure.
    // This wrapper neither owns the source buffer nor releases it on failure.
    const uint16_t bit = remember_buffer_drive(unit, reference, bytes);
    if (!write_version(unit, media, reference, track, sector)) {
        return false;
    }
    unit->buffer_writes |= bit;
    return storage_error(unit, media, reference, dos_media_write(media, reference, track, sector, bytes), true, track,
                         sector);
}

// SCRBAM/MAPOUT write a dirty BAM before replacing its sole per-drive cache.
static bool flush_bam(dos_unit_t* const unit, dos_media_bank_t* const media, dos_bam_t* const bam) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/map
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/frets
    //
    // 4040 MAPOUT checks every track. DOS 2.7 SETMAP scrubs a dirty BAM
    // before loading another part, and MAPOUT validates its recorded range.
    if (bam->dirty) {
        const dos_model_t model = media->units[bam->media.unit].model;
        const unsigned int low = model == DOS_MODEL_4040 ? 1u : bam->buffer.bytes[4];
        const unsigned int high = model == DOS_MODEL_4040 ? DISKIMAGE_D64_TRACK_COUNT + 1u : bam->buffer.bytes[5];
        if (low == 0 || high <= low || high - low > DOS_BAM_TRACKS_PER_PART) {
            dos_status(unit, model, DOS_STATUS_DIRECTORY_ERROR, bam->buffer.track, bam->buffer.sector);
            return false;
        }
        // Validate each count/bitmap pair before publishing allocation changes.
        for (unsigned int track = low; track < high; ++track) {
            const uint8_t* const entry =
                bam->buffer.bytes + (model == DOS_MODEL_4040 ? track * 4u : 6u + (track - low) * 5u);
            if (!valid_bitmap(unit, media, bam->media, (uint8_t)track, entry)) {
                return false;
            }
        }
        if (!write_sector(unit, media, bam->media, bam->buffer.track, bam->buffer.sector, bam->buffer.bytes)) {
            return false;
        }
    }
    // Retaining dirty until a successful write is a host retry policy.
    bam->dirty = false;
    return true;
}

// AVCK compares the recorded count with all bits, including padding bits.
static bool valid_bitmap(dos_unit_t* const unit, dos_media_bank_t* const media, const dos_media_ref_t reference,
                         const uint8_t track, const uint8_t* const entry) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tstfnd
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tstfnd
    //
    // AVCK counts every bit in the model's three or four bitmap bytes,
    // rather than restricting the count to geometrically valid sectors.
    const dos_model_t model = media->units[reference.unit].model;
    const unsigned int width = model == DOS_MODEL_4040 ? 3u : 4u;
    unsigned int count = 0;
    for (unsigned int index = 1; index <= width; ++index) {
        for (uint8_t bits = entry[index]; bits != 0; bits &= (uint8_t)(bits - 1u)) {
            ++count;
        }
    }
    // A mismatched recorded count is DIR ERROR, not an allocation hint.
    if (count == entry[0]) {
        return true;
    }
    dos_status(unit, model, DOS_STATUS_DIRECTORY_ERROR, track, 0);
    return false;
}

// FRETS/USEDTS update one native BAM bit and its free count together.
static bool mark_sector(dos_unit_t* const unit, dos_media_bank_t* const media, const dos_media_ref_t reference,
                        const uint8_t track, const uint8_t sector, const bool available) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/frets
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/frets
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
    //
    // FRETS sets a free bit and USEDTS clears it. Their count changes occur
    // only on an actual transition, making repeated release/claim idempotent.
    uint8_t* const entry = bam_entry(unit, media, reference, track);
    unsigned int sectors;
    const diskimage_type_t type = media->units[reference.unit].slots[reference.slot].io.image.type;
    if (entry == NULL || !diskimage_track_sectors(type, track, &sectors) || sector >= sectors) {
        return false;
    }
    const uint8_t mask = (uint8_t)(1u << (sector & 7u));
    uint8_t* const byte = entry + 1u + sector / 8u;
    if (((*byte & mask) != 0) != available) {
        *byte ^= mask;
        entry[0] = (uint8_t)(available ? entry[0] + 1u : entry[0] - 1u);
        unit->bam[reference.slot].dirty = true;
        // Maintain the cached NUMFRE total only for data tracks. DOS 2.7
        // also reports its low-free-block warning during allocation.
        if (track != directory_track(media->units[reference.unit].model) && unit->free_blocks_valid[reference.slot]) {
            uint16_t* const count = &unit->free_blocks[reference.slot];
            *count = available ? *count + 1u : *count - 1u;
            if (!available && media->units[reference.unit].model != DOS_MODEL_4040 && *count < 3u) {
                dos_status(unit, media->units[reference.unit].model, DOS_STATUS_DISK_FULL, 0, 0);
            }
        }
    }
    return true;
}

// GETSEC/FNDNXT select a free sector from the requested rotational position.
static bool allocate_on_track(dos_unit_t* const unit, dos_media_bank_t* const media, const dos_media_ref_t reference,
                              const uint8_t track, const unsigned int start, uint8_t* const sector) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tstfnd
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tstfnd
    //
    // AVAIL/AVCK and GETSEC validate the bitmap before FNDNXT claims a bit.
    // Scan from the interleave-selected position, wrapping once at track end.
    uint8_t* const entry = bam_entry(unit, media, reference, track);
    if (entry == NULL || !valid_bitmap(unit, media, reference, track, entry)) {
        return false;
    }
    unsigned int sectors;
    if (!diskimage_track_sectors(media->units[reference.unit].slots[reference.slot].io.image.type, track, &sectors)) {
        return false;
    }
    for (unsigned int index = 0; index < sectors; ++index) {
        const unsigned int candidate = (start + index) % sectors;
        if (entry[1u + candidate / 8u] & (1u << (candidate & 7u))) {
            *sector = (uint8_t)candidate;
            return mark_sector(unit, media, reference, track, *sector, false);
        }
    }
    // Distinguish a full track from a count claiming nonexistent free bits.
    dos_status(unit, media->units[reference.unit].model,
               entry[0] == 0 ? DOS_STATUS_DISK_FULL : DOS_STATUS_DIRECTORY_ERROR, 0, 0);
    return false;
}

// INTTS alternates outwards for a new file, NXTTS fills tracks away from DIRTRK.
static bool allocate_sector(dos_unit_t* const unit, dos_media_bank_t* const media, const dos_media_ref_t reference,
                            uint8_t* const track, uint8_t* const sector, const bool initial) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tstfnd
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tstfnd
    //
    // INTTS (JINTTS in DOS 2.7) searches below then above DIRTRK at each
    // distance. NXTTS/JNXTTS instead keep filling tracks away from DIRTRK.
    const dos_model_t model = media->units[reference.unit].model;
    const unsigned int tracks = model == DOS_MODEL_4040   ? DISKIMAGE_D64_TRACK_COUNT
                                : model == DOS_MODEL_8050 ? DISKIMAGE_D80_TRACK_COUNT
                                                          : DISKIMAGE_D82_TRACK_COUNT;
    const uint8_t directory = directory_track(model);
    uint16_t count;
    if (!free_blocks(unit, media, reference, &count)) {
        return false;
    }
    unsigned int candidate = *track;
    unsigned int previous_sector = *sector;
    for (unsigned int step = 0; step < tracks * 2u; ++step) {
        if (initial) {
            // Never allocate file data on the directory track itself.
            const unsigned int distance = step / 2u + 1u;
            if ((step & 1u) == 0) {
                if (distance >= directory) {
                    continue;
                }
                candidate = directory - distance;
            } else {
                candidate = directory + distance;
                if (candidate > tracks) {
                    if (model != DOS_MODEL_4040) {
                        // JINTTS/ITS2 reaches MAXTRK through CMDER2, retaining
                        // the lookup sector instead of CMDERR's zero fields.
                        dos_status(unit, model, DOS_STATUS_ILLEGAL_SYSTEM_TRACK_SECTOR, (uint8_t)candidate, *sector);
                        return false;
                    }
                    continue;
                }
            }
        }
        uint8_t* const entry = bam_entry(unit, media, reference, (uint8_t)candidate);
        if (entry == NULL) {
            return false;
        }
        if (entry[0] != 0) {
            unsigned int start = 0;
            if (!initial) {
                // FNDNXT adds SECINC and applies its subtract/decrement wrap
                // before searching for a free sector on this track.
                unsigned int sectors;
                if (!diskimage_track_sectors(media->units[reference.unit].slots[reference.slot].io.image.type,
                                             candidate, &sectors)) {
                    return false;
                }
                start =
                    previous_sector + (model == DOS_MODEL_4040 ? DOS_DATA_INTERLEAVE_4040 : DOS_DATA_INTERLEAVE_8050);
                if (start > sectors) {
                    start -= sectors;
                    if (start != 0) {
                        --start;
                    }
                }
            }
            *track = (uint8_t)candidate;
            return allocate_on_track(unit, media, reference, *track, start, sector);
        }
        if (!initial) {
            // Crossing a disk edge resumes on the other side of DIRTRK,
            // resetting the previous sector used to calculate interleave.
            if (candidate < directory) {
                candidate = candidate == 1 ? directory + 1u : candidate - 1u;
                if (candidate == directory + 1u) {
                    previous_sector = 0;
                }
            } else {
                ++candidate;
                if (candidate > tracks) {
                    candidate = directory - 1u;
                    previous_sector = 0;
                }
            }
        }
    }
    dos_status(unit, model, DOS_STATUS_DISK_FULL, 0, 0);
    return false;
}

// Keep a failed CLSDIR's reclamation cursor exclusive until metadata commits.
static bool directory_entry_busy(const dos_unit_t* const unit, const dos_media_ref_t reference, const uint8_t track,
                                 const uint8_t sector, const uint8_t entry, const dos_channel_t* const owner) {
    for (unsigned int index = 0; index < DOS_FILE_CHANNEL_COUNT; ++index) {
        const dos_channel_t* const channel = &unit->channels[index];
        if (channel != owner && channel->allocated && channel->closing == DOS_CLOSE_RECLAIM &&
            channel->media.unit == reference.unit && channel->media.slot == reference.slot &&
            channel->media.identity == reference.identity && channel->directory_track == track &&
            channel->directory_sector == sector && channel->directory_entry == entry) {
            return true;
        }
    }
    return false;
}

// Retire live COPY observations before a peer CLSDIR can release their chain.
static void retire_copy_binding(dos_unit_t* const unit, const dos_channel_t* const channel) {
    if (unit->command != DOS_COMMAND_COPY) {
        return;
    }
    dos_copy_work_t* const work = &unit->copy;
    if (work->invalidated_file != 0 || (work->writer != DOS_UNUSED && channel == &unit->channels[work->writer])) {
        return;
    }
    for (unsigned int index = 0; index < work->count; ++index) {
        if (!(work->found & (1u << index)) || (index != 0 && index < work->source) ||
            (index == 0 && (work->writer == DOS_UNUSED || unit->channels[work->writer].access != DOS_ACCESS_APPEND))) {
            continue;
        }
        const dos_copy_file_t* const file = &work->files[index];
        const dos_media_ref_t reference = work->media[file->drive & 1u];
        if (reference.identity == channel->media.identity && reference.unit == channel->media.unit &&
            reference.slot == channel->media.slot && file->directory_track == channel->directory_track &&
            file->directory_sector == channel->directory_sector && file->directory_entry == channel->directory_entry) {
            work->invalidated_file = index + 1u;
            return;
        }
    }
}

// Read-modify-write this entry, reclaiming only its current replacement chain.
static bool update_directory(dos_unit_t* const unit, dos_media_bank_t* const media, dos_channel_t* const channel,
                             const bool closed) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/addfil
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/addfil
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/close
    //
    // ADDFIL publishes an unclosed entry. CLSDIR rereads its sector before
    // updating the closed flag, first link, and final block count.
    const dos_model_t model = media->units[channel->media.unit].model;
    if (directory_entry_busy(unit, channel->media, channel->directory_track, channel->directory_sector,
                             channel->directory_entry, channel)) {
        dos_status(unit, model, DOS_STATUS_WRITE_ERROR, channel->directory_track, channel->directory_sector);
        return false;
    }
    if (channel->grow_pending && !finish_directory_growth(unit, media, channel)) {
        return false;
    }
    // Reread rather than restoring an old sector that could erase peer edits.
    uint8_t bytes[DISKIMAGE_SECTOR_SIZE];
    if (!storage_error(
            unit, media, channel->media,
            read_sector(unit, media, channel->media, channel->directory_track, channel->directory_sector, bytes), false,
            channel->directory_track, channel->directory_sector)) {
        return false;
    }
    uint8_t* const entry = bytes + channel->directory_entry;
    if (channel->access == DOS_ACCESS_WRITE && !channel->replaced && !channel->directory_published) {
        memset(entry + DOS_DIRECTORY_TYPE_OFFSET, 0, DISKIMAGE_DIRECTORY_ENTRY_SIZE - DOS_DIRECTORY_TYPE_OFFSET);
        entry[DOS_DIRECTORY_TYPE_OFFSET] = channel->file_type;
        entry[DOS_DIRECTORY_TRACK_OFFSET] = channel->first_track;
        entry[DOS_DIRECTORY_SECTOR_OFFSET] = channel->first_sector;
        memcpy(entry + DOS_DIRECTORY_NAME_OFFSET, channel->pattern, sizeof(channel->pattern));
    } else if (channel->replaced && (!closed || !channel->directory_published)) {
        // Keep the original chain visible until CLOSE commits the alternate.
        entry[DOS_DIRECTORY_TYPE_OFFSET] |= DOS_FILE_REPLACED_MASK;
        entry[DOS_DIRECTORY_REPLACE_TRACK] = channel->first_track;
        entry[DOS_DIRECTORY_REPLACE_SECTOR] = channel->first_sector;
    } else if (!closed && channel->access != DOS_ACCESS_WRITE) {
        entry[DOS_DIRECTORY_TYPE_OFFSET] &= DOS_FILE_LOCKED_MASK | DOS_FILE_TYPE_MASK;
    }
    if (closed) {
        if (entry[DOS_DIRECTORY_TYPE_OFFSET] & DOS_FILE_REPLACED_MASK) {
            retire_copy_binding(unit, channel);
        }
        if (channel->closing == DOS_CLOSE_DIRECTORY) {
            // CLSDIR tests the reread entry, not links saved by an earlier OPEN.
            const bool replaced = (entry[DOS_DIRECTORY_TYPE_OFFSET] & DOS_FILE_REPLACED_MASK) != 0;
            if (replaced && entry[DOS_DIRECTORY_REPLACE_TRACK] == 0) {
                dos_status(unit, model, DOS_STATUS_ILLEGAL_SYSTEM_TRACK_SECTOR, 0, entry[DOS_DIRECTORY_REPLACE_SECTOR]);
                return false;
            }
            channel->old_track = replaced ? entry[DOS_DIRECTORY_TRACK_OFFSET] : 0;
            channel->old_sector = entry[DOS_DIRECTORY_SECTOR_OFFSET];
            channel->closing = DOS_CLOSE_RECLAIM;
        }
        // Retain progress and entry ownership even if deletion or publication fails.
        unsigned int visited = 0;
        while (channel->old_track != 0) {
            uint8_t old[DISKIMAGE_SECTOR_SIZE];
            if (++visited >
                media->units[channel->media.unit].slots[channel->media.slot].io.image.size / DISKIMAGE_SECTOR_SIZE) {
                dos_status(unit, model, DOS_STATUS_DIRECTORY_ERROR, channel->old_track, channel->old_sector);
                return false;
            }
            if (!storage_error(unit, media, channel->media,
                               read_sector(unit, media, channel->media, channel->old_track, channel->old_sector, old),
                               false, channel->old_track, channel->old_sector) ||
                !mark_sector(unit, media, channel->media, channel->old_track, channel->old_sector, true)) {
                return false;
            }
            channel->old_track = old[0];
            channel->old_sector = old[1];
        }
        // A peer replacement may own the current alternate, even at this CLOSE.
        if (entry[DOS_DIRECTORY_TYPE_OFFSET] & DOS_FILE_REPLACED_MASK) {
            entry[DOS_DIRECTORY_TRACK_OFFSET] = entry[DOS_DIRECTORY_REPLACE_TRACK];
            entry[DOS_DIRECTORY_SECTOR_OFFSET] = entry[DOS_DIRECTORY_REPLACE_SECTOR];
            entry[DOS_DIRECTORY_REPLACE_TRACK] = 0;
            entry[DOS_DIRECTORY_REPLACE_SECTOR] = 0;
        }
        entry[DOS_DIRECTORY_TYPE_OFFSET] =
            (entry[DOS_DIRECTORY_TYPE_OFFSET] & DOS_FILE_TYPE_MASK) | DOS_FILE_CLOSED_MASK;
        entry[DOS_DIRECTORY_BLOCKS_LOW] = (uint8_t)channel->blocks;
        entry[DOS_DIRECTORY_BLOCKS_HIGH] = (uint8_t)(channel->blocks >> 8u);
    }
    if (!write_sector(unit, media, channel->media, channel->directory_track, channel->directory_sector, bytes)) {
        return false;
    }
    channel->directory_dirty = false;
    channel->directory_published = true;
    return true;
}

// WRT0 allocates the following block before writing a full linked data sector.
static bool advance_write(dos_unit_t* const unit, dos_media_bank_t* const media, dos_channel_t* const channel) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
    //
    // WRT0 calls NXTTS, writes the successor link, then WRTBUF/DBLBUF
    // switches to the new block with its payload pointer set to 2.
    dos_buffer_t* const buffer = &unit->buffers[channel->buffers[channel->active]];
    if (channel->next_track == 0) {
        // Retain the allocation across failed writes to avoid claiming a
        // different successor when the same full buffer is retried.
        uint8_t track = buffer->track, sector = buffer->sector;
        if (!allocate_sector(unit, media, channel->media, &track, &sector, false)) {
            return false;
        }
        channel->next_track = track;
        channel->next_sector = sector;
    }
    buffer->bytes[0] = channel->next_track;
    buffer->bytes[1] = channel->next_sector;
    if (!write_sector(unit, media, channel->media, buffer->track, buffer->sector, buffer->bytes)) {
        return false;
    }
    // Advance ownership/cursors only after the linked sector was persisted.
    ++channel->blocks;
    channel->active ^= 1u;
    dos_buffer_t* const next = &unit->buffers[channel->buffers[channel->active]];
    next->track = channel->next_track;
    next->sector = channel->next_sector;
    next->resident = true;
    channel->next_track = 0;
    channel->position = DISKIMAGE_SECTOR_LINK_SIZE;
    return true;
}

// Store one ordinary-file byte, advancing a full sector through the shared writer.
// Return false on allocation/I/O failure, possibly after the byte was already stored.
static bool write_byte(dos_unit_t* const unit, dos_media_bank_t* const media, dos_channel_t* const channel,
                       const uint8_t byte) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
    //
    // WRTBYT calls PUTBYT, then enters WRT0 when the byte pointer wraps.
    // WRT0 allocates/links a successor, writes the full page, switches buffers,
    // and resets the payload pointer to 2. C represents the wrapped end as 256.

    // A previous failed transition can leave the page full. Retry that
    // transition before accepting another byte, without indexing past the page.
    if (channel->position == DISKIMAGE_SECTOR_SIZE && !advance_write(unit, media, channel)) {
        return false;
    }

    // PUTBYT stores before testing fullness. Keep that ordering so a byte
    // filling the last payload offset survives a subsequent write failure.
    unit->buffers[channel->buffers[channel->active]].bytes[channel->position++] = byte;

    // Advance immediately at exact fullness, even if this is the final input
    // byte. CLOSE, not this helper, supplies the terminal link and closed entry.
    return channel->position != DISKIMAGE_SECTOR_SIZE || advance_write(unit, media, channel);
}

// Force native 4040 direct CLOSE to validate and write even an unchanged BAM.
static bool prepare_mapout_4040(dos_unit_t* const unit, dos_media_bank_t* const media,
                                const dos_channel_t* const channel) {
    // ROM source:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close
    // MAPOUT/MAPCHK always run AVCK and submit WRITE. The dirty-only cache
    // helper must therefore include a clean native CLOSE, unlike DOS 2.7.
    const dos_media_ref_t reference = channel->media;
    dos_media_ref_t current = {0};
    if (dos_media_reference(media, reference.unit, reference.slot, &current) != DOS_MEDIA_OK ||
        current.identity != reference.identity) {
        return storage_error(unit, media, reference, DOS_MEDIA_STALE, true, 0, 0);
    }
    // Lazy host initialization may not have loaded this drive's fixed BAM yet.
    if (bam_entry(unit, media, reference, 1) == NULL) {
        return false;
    }
    unit->bam[reference.slot].dirty = true;
    return true;
}

// CLSWRT/CLSDIR/MAPOUT finish data, replacement deletion, directory, then BAM.
static bool finish_channel(dos_unit_t* const unit, dos_media_bank_t* const media, dos_channel_t* const channel) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/close
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/map
    //
    // CLSWRT terminates data, CLSDIR publishes metadata, and MAPOUT writes
    // allocation changes. C records persistence phases so a host I/O failure
    // can be retried without repeating block counts or losing owned buffers.
    const dos_model_t model = media->units[channel->media.unit].model;
    // 4040 MAPOUT uses LSTJOB without changing DRVNUM. DOS 2.7 MO10 sets it.
    if (channel->access != DOS_ACCESS_DIRECT || model != DOS_MODEL_4040) {
        unit->selected_drive = channel->media.slot;
    }
    if (channel->access == DOS_ACCESS_DIRECT) {
        // CLSCHN skips file finalization but still reaches MAPOUT.
        if (channel->closing == DOS_CLOSE_DATA && !unit->bam[channel->media.slot].dirty &&
            !media->units[channel->media.unit].slots[channel->media.slot].dirty) {
            return true;
        }
        // Validate the owner before MAPOUT can write a newer cache generation.
        dos_media_ref_t current = {0};
        if (dos_media_reference(media, channel->media.unit, channel->media.slot, &current) != DOS_MEDIA_OK ||
            current.identity != channel->media.identity) {
            return storage_error(unit, media, channel->media, DOS_MEDIA_STALE, true, 0, 0);
        }
        // B-A/B-F can dirty the map again while a backing flush is pending.
        if (!flush_bam(unit, media, &unit->bam[channel->media.slot])) {
            return false;
        }
        channel->closing = DOS_CLOSE_FLUSH;
        return storage_error(unit, media, channel->media, dos_media_flush_unit(media, channel->media.unit), true, 0, 0);
    }
    // No directory coordinates were published if growth allocation failed.
    if (channel->closing == DOS_CLOSE_ABORT_OPEN) {
        if (channel->first_track != 0) {
            if (!mark_sector(unit, media, channel->media, channel->first_track, channel->first_sector, true)) {
                return false;
            }
            channel->first_track = 0;
        }
        return flush_bam(unit, media, &unit->bam[channel->media.slot]) &&
               storage_error(unit, media, channel->media, dos_media_flush_unit(media, channel->media.unit), true, 0, 0);
    }
    if (channel->grow_pending && !finish_directory_growth(unit, media, channel)) {
        return false;
    }
    if (channel->closing == DOS_CLOSE_DATA) {
        // Finish an interrupted full-block write before forming the tail.
        if (channel->next_track != 0 && !advance_write(unit, media, channel)) {
            return false;
        }
        dos_buffer_t* buffer = &unit->buffers[channel->buffers[channel->active]];
        if (channel->position == DISKIMAGE_SECTOR_LINK_SIZE && channel->blocks != 0) {
            // DOS 2.7 frees its speculative extra block, the selected 4040 does not.
            if (model != DOS_MODEL_4040 &&
                !mark_sector(unit, media, channel->media, buffer->track, buffer->sector, true)) {
                return false;
            }
            channel->active ^= 1u;
            buffer = &unit->buffers[channel->buffers[channel->active]];
            channel->position = DISKIMAGE_SECTOR_SIZE;
        } else {
            if (channel->position == DISKIMAGE_SECTOR_LINK_SIZE) {
                // CLSWRT gives a completely empty file one CR payload byte.
                buffer->bytes[channel->position++] = '\r';
            }
            ++channel->blocks;
        }
        // The final link's second byte is the last occupied offset, not
        // the number of payload bytes.
        buffer->bytes[0] = 0;
        buffer->bytes[1] = (uint8_t)(channel->position - 1u);
        // Cursor changes above must not be repeated if this write needs a retry.
        channel->closing = DOS_CLOSE_OLD_CHAIN;
        channel->directory_dirty = true;
    }
    if (channel->closing == DOS_CLOSE_OLD_CHAIN) {
        dos_buffer_t* const buffer = &unit->buffers[channel->buffers[channel->active]];
        if (!write_sector(unit, media, channel->media, buffer->track, buffer->sector, buffer->bytes)) {
            return false;
        }
        channel->closing = DOS_CLOSE_DIRECTORY;
    }
    if (channel->closing == DOS_CLOSE_DIRECTORY || channel->closing == DOS_CLOSE_RECLAIM) {
        if (!update_directory(unit, media, channel, true)) {
            return false;
        }
        channel->closing = DOS_CLOSE_BAM;
    }
    if (channel->closing == DOS_CLOSE_BAM) {
        if (!flush_bam(unit, media, &unit->bam[channel->media.slot])) {
            return false;
        }
        channel->closing = DOS_CLOSE_FLUSH;
    }
    // A backing-store flush is an additional host durability boundary.
    // Channel release belongs to the caller, only after this succeeds.
    return storage_error(unit, media, channel->media, dos_media_flush_unit(media, channel->media.unit), true, 0, 0);
}

// Finalize quiescent owners, retaining failed and not-yet-completed channels.
bool dos_finalize(dos_unit_t* const unit, dos_media_bank_t* const media) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/close
    //
    // No direct ROM entry: this host lifecycle operation retains ordinary
    // descriptors until its caller replaces/resets the unit. A failure retains
    // its owner and all unfinalized peers.
    // An unfinished APPEND has not changed media and must not CLOSE its read cursor.
    if (unit->open.pending && unit->open.append_channel != 0) {
        free_channel(unit, unit->open.append_channel - 1u);
        unit->open.pending = false;
        clear_command(unit);
    }
    // Stop on the first failed writer. Its phase and the remaining channels
    // survive for retry rather than being cleared by a partial finalization.
    for (uint8_t index = 0; index < DOS_FILE_CHANNEL_COUNT; ++index) {
        dos_channel_t* const channel = &unit->channels[index];
        if (channel->allocated && channel->access != DOS_ACCESS_READ && !finish_channel(unit, media, channel)) {
            return false;
        }
    }
    // Allocation work can outlive a DISK FULL-released writer.
    for (unsigned int slot = 0; slot < DOS_MEDIA_SLOTS_PER_UNIT; ++slot) {
        dos_bam_t* const bam = &unit->bam[slot];
        if (bam->media.identity != 0) {
            unit->selected_drive = bam->media.slot;
            if (!flush_bam(unit, media, bam)) {
                return false;
            }
        }
    }
    // A clean or failed-refill cache can still have pending backing-store I/O.
    for (unsigned int slot = 0; slot < DOS_MEDIA_SLOTS_PER_UNIT; ++slot) {
        const dos_media_ref_t reference = unit->bam[slot].media;
        if (reference.identity != 0) {
            dos_media_ref_t current;
            if (dos_media_reference(media, reference.unit, reference.slot, &current) != DOS_MEDIA_OK ||
                current.identity != reference.identity) {
                return storage_error(unit, media, reference, DOS_MEDIA_STALE, true, 0, 0);
            }
            if (!storage_error(unit, media, reference, dos_media_flush_unit(media, reference.unit), true, 0, 0)) {
                return false;
            }
        }
    }
    return true;
}

// NXDRBK links the old tail before initializing its new directory block.
static bool finish_directory_growth(dos_unit_t* const unit, dos_media_bank_t* const media,
                                    dos_channel_t* const channel) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4
    //
    // NXDRBK writes the old tail's successor before clearing the new block
    // and installing its terminal 0/$FF link. Allocation already happened.
    uint8_t bytes[DISKIMAGE_SECTOR_SIZE];
    // Reread the old tail so unrelated entries survive a delayed retry.
    if (!storage_error(
            unit, media, channel->media,
            read_sector(unit, media, channel->media, channel->directory_track, channel->grow_from_sector, bytes), false,
            channel->directory_track, channel->grow_from_sector)) {
        return false;
    }
    bytes[0] = channel->directory_track;
    bytes[1] = channel->directory_sector;
    if (!write_sector(unit, media, channel->media, channel->directory_track, channel->grow_from_sector, bytes)) {
        return false;
    }
    // Do not publish the first entry until this empty successor is durable.
    memset(bytes, 0, sizeof(bytes));
    bytes[1] = DOS_UNUSED;
    if (!write_sector(unit, media, channel->media, channel->directory_track, channel->directory_sector, bytes)) {
        return false;
    }
    // Keep both coordinates pending on failure so either write can be retried.
    channel->grow_pending = false;
    return true;
}

// APPEND traverses through GCBYTE/RDLNK, yielding between synchronous host reads.
static void append_work(dos_unit_t* const unit, dos_media_bank_t* const media, dos_open_work_t* const work,
                        const bool report) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/open
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/open
    //
    // APPEND reads through GCBYTE until the last-record flag, then RDLNK
    // positions after the final occupied byte (or WRT0 obtains a new block).
    // C follows one sector link per call rather than blocking the scheduler.
    const uint8_t index = work->append_channel - 1u;
    dos_channel_t* const channel = &unit->channels[index];
    const dos_model_t model = media->units[channel->media.unit].model;
    dos_buffer_t* const buffer = &unit->buffers[channel->buffers[channel->active]];
    if (buffer->bytes[0] != 0) {
        // The image-size visit limit is a host corruption safeguard.
        const unsigned int limit =
            media->units[channel->media.unit].slots[channel->media.slot].io.image.size / DISKIMAGE_SECTOR_SIZE;
        if (channel->traversed++ >= limit) {
            dos_status(unit, model, DOS_STATUS_DIRECTORY_ERROR, buffer->track, buffer->sector);
        } else {
            const uint8_t track = buffer->bytes[0], sector = buffer->bytes[1];
            channel->active ^= 1u;
            if (load_buffer(unit, media, channel, track, sector)) {
                ++channel->blocks;
                return;
            }
            dos_status(unit, model, DOS_STATUS_READ_ERROR, track, sector);
        }
        // Before reaching the tail this is still a read-only traversal.
        free_channel(unit, index);
        // Byte 1 holds the last occupied offset, so append starts one byte later.
        work->pending = false;
        clear_command(unit);
        return;
    }
    work->pending = false;
    clear_command(unit);
    channel->position = buffer->bytes[1] + 1u;
    if (channel->position < DISKIMAGE_SECTOR_LINK_SIZE) {
        dos_status(unit, model, DOS_STATUS_DIRECTORY_ERROR, buffer->track, buffer->sector);
        free_channel(unit, index);
        return;
    }
    // Mark the entry unclosed before accepting PUT data. Storage failures
    // here retain the channel, unlike failures during the read-only walk.
    if ((report && !update_directory(unit, media, channel, false)) ||
        (channel->position == DISKIMAGE_SECTOR_SIZE && !advance_write(unit, media, channel))) {
        channel->failed = true;
        if (unit->status_code == DOS_STATUS_DISK_FULL) {
            free_channel(unit, index);
        }
        return;
    }
    if (report) {
        command_end(unit, model);
    }
}

// Find retained host publication intent independently of visible directory bytes.
static const dos_channel_t* unpublished_writer(const dos_unit_t* const unit, const dos_media_ref_t reference,
                                               const uint8_t name[DISKIMAGE_FILENAME_SIZE], const uint8_t secondary) {
    for (unsigned int index = 0; index < DOS_FILE_CHANNEL_COUNT; ++index) {
        const dos_channel_t* const owner = &unit->channels[index];
        if (owner->allocated && owner->access == DOS_ACCESS_WRITE && !owner->directory_published &&
            owner->closing != DOS_CLOSE_ABORT_OPEN && owner->secondary != secondary &&
            owner->media.unit == reference.unit && owner->media.slot == reference.slot &&
            owner->media.identity == reference.identity && memcmp(owner->pattern, name, DISKIMAGE_FILENAME_SIZE) == 0) {
            return owner;
        }
    }
    return NULL;
}

// Reject directory reads while a retained writer owns either incomplete growth link.
static bool directory_growth_pending(const dos_unit_t* const unit, const dos_media_ref_t reference, const uint8_t track,
                                     const uint8_t sector) {
    for (unsigned int index = 0; index < DOS_FILE_CHANNEL_COUNT; ++index) {
        const dos_channel_t* const owner = &unit->channels[index];
        if (owner->allocated && owner->grow_pending && owner->media.identity == reference.identity &&
            owner->media.unit == reference.unit && owner->media.slot == reference.slot &&
            owner->directory_track == track &&
            (owner->directory_sector == sector || owner->grow_from_sector == sector)) {
            return true;
        }
    }
    return false;
}

// Keep deleted entries reserved until their retained writer publishes or retires.
static bool directory_slot_reserved(const dos_unit_t* const unit, const dos_media_ref_t reference, const uint8_t sector,
                                    const uint8_t entry) {
    for (unsigned int index = 0; index < DOS_FILE_CHANNEL_COUNT; ++index) {
        const dos_channel_t* const owner = &unit->channels[index];
        if (owner->allocated && owner->access != DOS_ACCESS_READ && owner->media.identity == reference.identity &&
            owner->media.unit == reference.unit && owner->media.slot == reference.slot &&
            owner->directory_sector == sector && (owner->grow_pending || owner->directory_entry == entry)) {
            return true;
        }
    }
    return false;
}

// OPNWCH/ADDFIL construct a new chain, or reserve a replacement's alternate link.
static bool open_write(dos_unit_t* const unit, dos_media_bank_t* const media, dos_channel_t* const channel,
                       const dos_open_work_t* const work, const bool existing) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/opchnl
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/opchnl
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/addfil
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/addfil
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4
    //
    // OPNWCH/INTPNT allocate the initial data block and start its cursor
    // past the link. ADDFIL uses a deleted entry or NXDRBK grows the directory.
    // The C caller has already allocated the logical channel and buffers.
    channel->access = DOS_ACCESS_WRITE;
    // Until directory publication is possible, CLOSE must reclaim only the
    // initial allocation rather than write to nonexistent entry coordinates.
    channel->closing = DOS_CLOSE_ABORT_OPEN;
    channel->file_type = work->type != 0 ? work->type : existing ? channel->file_type : DISKIMAGE_FTYPE_SEQ;
    channel->replaced = existing;
    memcpy(channel->pattern,
           existing ? unit->buffers[work->scratch].bytes + channel->directory_entry + DOS_DIRECTORY_NAME_OFFSET
                    : work->name,
           sizeof(channel->pattern));
    uint8_t track = 0, sector = work->last_sector;
    channel->first_track = 0;
    if (!allocate_sector(unit, media, channel->media, &track, &sector, true)) {
        return false;
    }
    channel->first_track = track;
    channel->first_sector = sector;
    dos_buffer_t* const buffer = &unit->buffers[channel->buffers[0]];
    buffer->track = track;
    buffer->sector = sector;
    buffer->resident = true;
    channel->position = DISKIMAGE_SECTOR_LINK_SIZE;
    channel->ready = false;
    if (!existing) {
        // Prefer the deleted slot retained by lookup. Replacement keeps its
        // original entry and publishes the new chain through alternate links.
        channel->directory_track = directory_track(media->units[channel->media.unit].model);
        channel->directory_sector = work->empty_sector;
        channel->directory_entry = work->empty_entry;
        if (work->empty_track == 0) {
            // NXDRBK temporarily uses directory interleave 3 instead of the
            // model's data interleave. Retain both ends for retryable growth.
            unsigned int sectors;
            const diskimage_type_t type = media->units[channel->media.unit].slots[channel->media.slot].io.image.type;
            if (!diskimage_track_sectors(type, channel->directory_track, &sectors)) {
                return false;
            }
            unsigned int start = work->last_sector + DOS_DIRECTORY_INTERLEAVE;
            if (start > sectors) {
                start -= sectors;
                if (start != 0) {
                    --start;
                }
            }
            if (!allocate_on_track(unit, media, channel->media, channel->directory_track, start,
                                   &channel->directory_sector)) {
                return false;
            }
            channel->directory_entry = 0;
            channel->grow_from_sector = work->last_sector;
            channel->grow_pending = true;
        }
    }
    // Once coordinates are reserved, failed publication retains writable
    // state for finalization instead of leaking the new block.
    channel->closing = DOS_CLOSE_DATA;
    channel->directory_dirty = true;
    return update_directory(unit, media, channel, false);
}

// Route ordinary PUT bytes incrementally, leaving EOI distinct from CLOSE.
void dos_put(dos_unit_t* const unit, dos_media_bank_t* const media, const uint8_t secondary, const uint8_t byte) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
    //
    // PUT distinguishes OPEN/command collection from ordinary channel data.
    // This direct API accepts only decoded ordinary data, not collection.
    // It has no EOI argument because a file needs CLOSE to publish its tail.
    const uint8_t index = dos_find_channel(unit, secondary);
    if (index == DOS_UNUSED) {
        return;
    }
    dos_channel_t* const channel = &unit->channels[index];
    if (channel->access == DOS_ACCESS_DIRECT) {
        // tst2:L46/PUTBYT refresh CHNDAT at the incremented pointer.
        uint8_t* const bytes = unit->buffers[channel->buffers[0]].bytes;
        bytes[channel->position] = byte;
        channel->position = (uint8_t)(channel->position + 1u);
        channel->direct_byte = bytes[channel->position];
        if (channel->position == channel->direct_last) {
            channel->direct_eoi = true;
        }
        return;
    }
    // Accept ordinary data only for a writable channel still in its data
    // phase. Failed channels retain their state for finalization or retry.
    if (channel->access == DOS_ACCESS_READ || channel->access == DOS_ACCESS_MODIFY ||
        channel->closing != DOS_CLOSE_DATA || channel->failed) {
        return;
    }
    unit->selected_drive = channel->media.slot;
    if (!write_byte(unit, media, channel, byte)) {
        channel->failed = true;
        // Native CMDERR releases an ordinary channel on DISK FULL, not host I/O faults.
        if (unit->status_code == DOS_STATUS_DISK_FULL) {
            free_channel(unit, index);
        }
    }
}

// Parse block parameters into retained low bytes, defaulting omitted values to zero.
// Return false for table overflow or an unsupported scratch-dependent retained value.
static bool block_parameters(const uint8_t* const input, const unsigned int length, unsigned int cursor,
                             uint8_t* const values, const unsigned int retained) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/block
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/block
    //
    // BP05 skips separators, ASCHEX shifts three digit slots, and AH40 stores
    // low/high results in FILSEC/FILTRK. Callers here consume only low bytes.
    // Partial output on false is not usable, and callers report syntax status.
    memset(values, 0, retained);
    unsigned int parameters = 0;
    uint8_t hundreds = 0;
    bool numeric_seen = false;
    while (cursor < length) {
        if (input[cursor] == ' ' || input[cursor] == DOS_BLOCK_PARAMETER_SKIP || input[cursor] == ',') {
            ++cursor;
            continue;
        }
        // AH10 accepts $30..$3F, not just decimal ASCII. Retain the last
        // three low nibbles, then weight them by DECTAB's 1/10/100.
        uint8_t digits[3] = {0, 0, hundreds};
        const unsigned int start = cursor;
        while (cursor < length && input[cursor] >= '0' && input[cursor] < '@') {
            digits[2] = digits[1];
            digits[1] = digits[0];
            digits[0] = input[cursor++] & 0x0fu;
        }
        if (parameters < retained) {
            // A preceding numeric field determines retained TEMP+2. Before
            // the first one, it still depends on unmodeled caller scratch RAM.
            if (cursor == start && !numeric_seen) {
                return false;
            }
            values[parameters] = (uint8_t)(digits[0] + 10u * digits[1] + 100u * digits[2]);
        }
        hundreds = digits[2];
        numeric_seen |= cursor != start;
        // Even ignored extra values count toward BP20's four-value limit.
        if (++parameters == DOS_FILENAME_COUNT) {
            return false;
        }
        // BP10 also consumes the character which stopped ASCHEX.
        if (cursor < length) {
            ++cursor;
        }
    }
    return true;
}

// Return the position after a delimiter, or zero at EOF/the fourth preceding comma.
// Also return the preceding comma count for callers retaining PARSE's table index.
static unsigned int command_delimiter(const uint8_t* const bytes, const unsigned int length, const uint8_t delimiter,
                                      unsigned int* const commas) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/parsex
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/parsex
    //
    // PR10 increments Y before returning a match. PR25 bounds the comma
    // table at MXFILS-1, and PR30 returns Y=0 when no delimiter is reached.
    // This helper extracts only that scan, not PARSE's pattern/table updates.
    *commas = 0;
    for (unsigned int cursor = 0; cursor < length; ++cursor) {
        if (bytes[cursor] == delimiter) {
            return cursor + 1u;
        }
        if (bytes[cursor] == ',' && ++*commas == DOS_FILENAME_COUNT - 1u) {
            break;
        }
    }
    return 0;
}

// Validate block commands, queue storage work, or position a direct page in RAM.
// Use user_operation for U1/U2 aliases without parsing a B-* operation letter.
static void block_start(dos_unit_t* const unit, dos_media_bank_t* const media, const unsigned int index,
                        const unsigned int length, const uint8_t user_operation) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/block
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/block
    //
    // BLOCK selects a subcommand, BLKPAR parses parameters, and BUFTST/BLKTST
    // select a channel and coordinates. BLKPTR completes in RAM, while C
    // defers BLKRD/UBLKRD/BLKWT/UBLKWT and map operations to scheduled workers.
    const dos_model_t model = media->units[index].model;
    const uint8_t* const bytes = unit->command_bytes;
    unsigned int commas;
    uint8_t operation = user_operation;
    if (user_operation == 0) {
        // BLOCK accepts a longer prefix but uses the first '-' and rejects
        // any preceding comma. Missing/unknown operations give BADCMD.
        const unsigned int delimiter = command_delimiter(bytes, length, '-', &commas);
        if (delimiter == 0) {
            dos_status(unit, model, DOS_STATUS_INVALID_COMMAND, 0, 0);
            return;
        }
        if (commas != 0) {
            dos_status(unit, model, DOS_STATUS_SYNTAX, 0, 0);
            return;
        }
        if (delimiter >= length) {
            dos_status(unit, model, DOS_STATUS_INVALID_COMMAND, 0, 0);
            return;
        }
        operation = bytes[delimiter];
        if (operation != 'R' && operation != 'W' && operation != 'P' && operation != 'A' && operation != 'F') {
            // Arbitrary code execution remains explicitly excluded.
            const bool known = operation == 'E';
            dos_status(unit, model, known ? DOS_STATUS_SYNTAX : DOS_STATUS_INVALID_COMMAND, 0, 0);
            return;
        }
    }
    // BLKPAR starts after the first colon, or at byte index 3 without one.
    // A/F use drive,track,sector without a channel. P uses channel,offset.
    // Read/write commands retain all four values, including their channel.
    const unsigned int colon = command_delimiter(bytes, length, ':', &commas);
    const bool mapping = operation == 'A' || operation == 'F';
    uint8_t parameters[DOS_FILENAME_COUNT - 1u];
    if (!block_parameters(bytes, length, colon != 0 ? colon : DOS_BLOCK_PARAMETER_START, parameters,
                          mapping            ? 3u
                          : operation != 'P' ? count_of(parameters)
                                             : 2u)) {
        dos_status(unit, model, DOS_STATUS_SYNTAX, 0, 0);
        return;
    }
    dos_channel_t* state = NULL;
    const uint8_t secondary = parameters[0];
    const uint8_t channel = dos_find_channel(unit, secondary);
    if (!mapping) {
        // BUFTST uses the byte-sized (SA - 2) < BAMJOB test, excluding SA 14.
        if ((uint8_t)(secondary - DOS_BLOCK_FIRST_SECONDARY) >= DOS_BUFFER_COUNT || channel == DOS_UNUSED ||
            unit->channels[channel].access == DOS_ACCESS_WRITE || unit->channels[channel].access == DOS_ACCESS_APPEND) {
            dos_status(unit, model, DOS_STATUS_NO_CHANNEL, 0, 0);
            return;
        }
        state = &unit->channels[channel];
        if (state->access != DOS_ACCESS_DIRECT && operation != 'P' && state->directory != DOS_DIRECTORY_NONE) {
            // Formatted LOAD listings remain an explicit host-policy exclusion.
            dos_status(unit, model, DOS_STATUS_SYNTAX, 0, 0);
            return;
        }
    }
    if (operation != 'P') {
        // BLKTST folds the drive to bit zero. Validate against model geometry
        // before scheduling, without consulting the image or changing its BAM.
        const unsigned int drive_index = mapping ? 0u : 1u;
        const uint8_t drive = parameters[drive_index] & 1u, track = parameters[drive_index + 1u],
                      sector = parameters[drive_index + 2u];
        unit->selected_drive = drive;
        const diskimage_type_t type = model == DOS_MODEL_4040   ? diskimage_type_d64
                                      : model == DOS_MODEL_8050 ? diskimage_type_d80
                                      : model == DOS_MODEL_8250 ? diskimage_type_d82
                                                                : diskimage_type_none;
        if (type == diskimage_type_none) {
            dos_status(unit, model, DOS_STATUS_NOT_READY, 0, 0);
            return;
        }
        unsigned int sectors;
        if (!diskimage_track_sectors(type, track, &sectors) || sector >= sectors) {
            // 4040 erproc:HEXDEC retains two BCD digits. DOS 2.7 emits hundreds.
            dos_status(unit, model, DOS_STATUS_ILLEGAL_TRACK_SECTOR, model == DOS_MODEL_4040 ? track % 100u : track,
                       model == DOS_MODEL_4040 ? sector % 100u : sector);
            return;
        }
        // Capture the host media generation for deferred work. OK here is
        // the command's working status, not proof that its I/O has completed.
        dos_media_ref_t reference = {.unit = index, .slot = drive};
        dos_media_reference(media, index, drive, &reference);
        if (mapping) {
            unit->block_map = (dos_block_map_work_t){
                .media = reference, .track = track, .sector = sector, .allocate = operation == 'A'};
            unit->command = DOS_COMMAND_BLOCK_MAP;
            dos_status(unit, model, DOS_STATUS_OK, 0, 0);
            return;
        }
        // Keep U1/U2 raw-page semantics distinct from B-R/B-W count-byte
        // handling. No storage job is issued at this dispatch boundary.
        unit->block = (dos_block_work_t){
            .media = reference, .channel = channel, .track = track, .sector = sector, .raw = user_operation != 0};
        unit->command = operation == 'W' ? DOS_COMMAND_BLOCK_WRITE : DOS_COMMAND_BLOCK_READ;
        dos_status(unit, model, DOS_STATUS_OK, 0, 0);
        return;
    }
    if (state->access != DOS_ACCESS_DIRECT) {
        // Ordinary readers need lazy-to-native active-page cursor conversion,
        // which may require reading a successor before BLKPTR can be applied.
        unit->block = (dos_block_work_t){.channel = channel, .offset = parameters[1]};
        unit->command = DOS_COMMAND_READ_POINTER;
        dos_status(unit, model, DOS_STATUS_OK, 0, 0);
        return;
    }
    // BLKPTR/GETPRE prepare the byte without incrementing. RNGET2 can set
    // EOI at LSTCHR, but positioning elsewhere does not clear an existing EOI.
    // B-P leaves drive selection unchanged and issues no sector I/O here.
    state->position = parameters[1];
    state->direct_byte = unit->buffers[state->buffers[state->active]].bytes[state->position];
    if (state->position == state->direct_last) {
        state->direct_eoi = true;
    }
    command_end(unit, model);
}

// Consume a deferred DOS 2.7 reload flag by invalidating host metadata caches.
// Leave actual sector reads to the next lazy metadata consumer.
static void auto_initialize(dos_unit_t* const unit, const uint8_t drive) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/begin
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/romtbl
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lookup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
    //
    // CONFIG/CONF10 copies MSCCOM's WPSW flags. JAUTOI consumes the selected
    // flag and calls INITDR after a successful trial, without MAPOUT.
    // This host helper has no direct ROM entry: it implements only deferred
    // cache invalidation, not the physical trial or INITDR's channel sweep.
    if (unit->bam[drive].reinitialize) {
        // Discard dirty cached allocation state without flushing it. Clearing
        // the entire cache also consumes the flag, making this a one-shot step.
        unit->bam[drive] = (dos_bam_t){0};
        unit->free_blocks[drive] = 0;
        unit->free_blocks_valid[drive] = false;
    }
}

// NMI/DIAGOK reinitialize channel ownership without the cold-start RAM tests.
static void warm_reset(dos_unit_t* const unit, const dos_model_t model) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/vector
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/dskint
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/vector
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/begin
    // U9/UI enter NMI -> (VNMI) -> DIAGOK, not DSKINT's RAM clearing.
    memset(unit->channels, 0, sizeof(unit->channels));
    unit->open = (dos_open_work_t){0};
    unit->buffer_use = 0;
    unit->directory_internal = 0;
    for (unsigned int buffer = 0; buffer < DOS_BUFFER_COUNT; ++buffer) {
        unit->buffers[buffer].resident = false;
        unit->buffers[buffer].failed = false;
    }
    // DOS 2.7 CONFIG/MSCCOM sets both WPSW flags. Only a later AUTOI path
    // reloads metadata. Block commands alone can still use the retained BAM.
    if (model != DOS_MODEL_4040) {
        for (unsigned int drive = 0; drive < DOS_MEDIA_SLOTS_PER_UNIT; ++drive) {
            unit->bam[drive].reinitialize = true;
        }
    }
    // Data bytes, LSTJOB drive bits, last-drive/program state and the command
    // buffer survive. 4040 also retains its BAMs. Nothing is flushed.
    dos_status(unit, model, DOS_STATUS_POWER_ON, 0, 0);
}

// Select the starting drive for INITIALIZE/VALIDATE and report an omitted selector.
// The caller supplies retained command storage with its command byte at offset 0.
static uint8_t initialize_drive(const uint8_t* const bytes, const unsigned int length, const uint8_t last_drive,
                                bool* const both) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/parsex
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/parsex
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/setdrv
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/setdrv
    //
    // SIMPRS uses PARSE to find ':' and backs its returned pointer up to the
    // preceding character. Without a colon, FILTBL stays at the command byte.
    // SETANY/TST0V1 try that character, then the final command character.

    // PARSE stops collecting separators at MXFILS-1, so a later colon must
    // not override the default starting position after the filename limit.
    unsigned int start = 0, commas = 0;
    for (unsigned int position = 0; position < length; ++position) {
        if (bytes[position] == ':') {
            start = position - 1u;
            break;
        }
        if (bytes[position] == ',' && ++commas == DOS_FILENAME_COUNT - 1u) {
            break;
        }
    }
    // Preserve the initial command-buffer read even for zero logical length.
    // Explicit '0'/'1' at the first candidate takes precedence over the suffix.
    const uint8_t first = bytes[start], last = length == 0 ? first : bytes[length - 1u];

    // SETANY sets IMAGE to $FF and uses LSTDRV when neither candidate is a
    // drive digit. C exposes that omission through both: INITIALIZE uses it
    // to visit both drives, while VALIDATE uses only the returned starting drive.
    *both = first != '0' && first != '1' && last != '0' && last != '1';
    return first == '0' || first == '1' ? first - '0' : last == '0' || last == '1' ? last - '0' : last_drive;
}

// Copy a command name into a sixteen-byte, $A0-padded field and return its length.
// The caller must provide readable retained storage at start, even at logical EOF.
static unsigned int command_name(const uint8_t* const bytes, const unsigned int length, unsigned int start,
                                 uint8_t name[DISKIMAGE_FILENAME_SIZE]) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/trnsfr
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/trnsfr
    //
    // FNDLMT/FL05 scan until comma, equals, sixteen bytes, or CMDSIZ.
    // TRNAME/TRCMBF copy the bounded name and pad its remaining field with $A0.
    // C combines scanning/copying and initializes padding before the scan.
    memset(name, DISKIMAGE_FILENAME_PADDING, DISKIMAGE_FILENAME_SIZE);
    unsigned int used = 0;

    // FL05 reads before checking CMDSIZ. Keep the do/while ordering so a
    // starting byte at logical EOF is still inspected, not silently skipped.
    do {
        const uint8_t byte = bytes[start++];
        if (byte == ',' || byte == '=') {
            // Delimiters end the name without entering the padded field.
            break;
        }
        name[used++] = byte;
    } while (used < DISKIMAGE_FILENAME_SIZE && start < length);
    return used;
}

// Bind media and reserve COPY's internal reader for named or all-files work.
// Require the caller's parsed descriptors, initial drive, and phase to be set.
static void copy_queue(dos_unit_t* const unit, dos_media_bank_t* const media, const unsigned int index,
                       dos_copy_work_t* const work) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rename
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rename
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lookup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lookup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst3
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst3
    //
    // COPY uses LOOKUP before CY opens its internal channels. This host
    // setup helper has no direct ROM entry: it captures image generations and
    // reserves a GETRCH-style two-page reader for cooperative lookup/copy.
    const dos_model_t model = media->units[index].model;
    for (unsigned int slot = 0; slot < DOS_MEDIA_SLOTS_PER_UNIT; ++slot) {
        work->media[slot] = (dos_media_ref_t){.unit = index, .slot = slot};
        dos_media_reference(media, index, slot, &work->media[slot]);
    }
    unit->selected_drive = work->drive;

    // The target disk must exist. Whole-drive file copy also requires the
    // fixed source disk, whereas named lookup can search an available peer.
    if (work->media[work->files[0].drive].identity == 0 ||
        (work->all_files && work->media[work->drive].identity == 0)) {
        dos_status(unit, model, DOS_STATUS_NOT_READY, 0, 0);
        return;
    }
    // Host absent-image policy narrows the lookup cursor to the present disk.
    if (work->media[work->drive].identity == 0) {
        work->drive ^= 1u;
        work->scans = 0;
    }
    // Retained host publication intent survives CLOSE during a yielded lookup.
    if (!work->all_files) {
        uint8_t name[DISKIMAGE_FILENAME_SIZE];
        memcpy(name, work->files[0].name, sizeof(name));
        if (work->empty_target) {
            name[0] = '=';
        }
        work->name_conflict = unpublished_writer(unit, work->media[work->files[0].drive], name, DOS_UNUSED) != NULL;
    }
    // Allocate before publishing the job. allocate_channel rolls back partial
    // page claims, so failure leaves no queued work or abandoned reader.
    work->reader = allocate_channel(unit, work->media[work->drive], DOS_INTERNAL_READ_SECONDARY, 2);
    if (work->reader == DOS_UNUSED) {
        dos_status(unit, model, DOS_STATUS_NO_CHANNEL, 0, 0);
        return;
    }
    // This internal reader is owned but not bus-ready. No source sector or
    // destination file has been opened yet, and OK is only the working status.
    unit->channels[work->reader].ready = false;
    unit->copy = *work;
    unit->command = DOS_COMMAND_COPY;
    dos_status(unit, model, DOS_STATUS_OK, 0, 0);
}

// Parse named/concatenating COPY or directory-pattern file copy and queue it.
// Capture names and drive-search state without copying data during dispatch.
static void copy_start(dos_unit_t* const unit, dos_media_bank_t* const media, const unsigned int index,
                       const unsigned int length) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/copysetup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/copysetup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/parsex
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/parsex
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/copyall
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/copyall
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/setdrv
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/setdrv
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lookup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lookup
    //
    // DSKCPY uses PRSEQ/MOVLP2 without a colon, otherwise TAGCMD/ALLDRS
    // selects named COPY or CPYDTD. C retains descriptors for cooperative lookup/copy
    // rather than rewriting CMDBUF and using the ROM's stacked iterator.
    const dos_model_t model = media->units[index].model;
    const uint8_t* const bytes = unit->command_bytes;
    unsigned int commas;
    const unsigned int after_colon = command_delimiter(bytes, length, ':', &commas);
    unsigned int equals = 0;
    if (after_colon == 0) {
        // PRSEQ restarts Y, but retains PARSE's comma-table index. Only the
        // bytes immediately beside the first '=' select drives. Prefixes
        // and suffixes are not filenames, and the source byte can be stale.
        while (equals < length && bytes[equals] != '=') {
            if (bytes[equals] == ',' && ++commas >= DOS_FILENAME_COUNT - 1u) {
                equals = length;
                break;
            }
            ++equals;
        }
        if (equals == 0 || equals == length || (bytes[equals - 1u] != '0' && bytes[equals - 1u] != '1') ||
            (bytes[equals + 1u] != '0' && bytes[equals + 1u] != '1') || bytes[equals - 1u] == bytes[equals + 1u]) {
            dos_status(unit, model, DOS_STATUS_SYNTAX, 0, 0);
            return;
        }
        // PRSEQ requires distinct ASCII 0/1 selectors. MOVLP2 copies files
        // by name in directory order, not a sector-for-sector disk image.
        dos_copy_work_t work = {
            .count = 2,
            .drive = bytes[equals + 1u] - '0',
            .reader = DOS_UNUSED,
            .writer = DOS_UNUSED,
            .phase = DOS_COPY_ALL_NEXT,
            .pending = true,
            .all_files = true,
            .all_track = directory_track(model),
            .all_sector = DOS_DIRECTORY_FIRST_SECTOR,
        };
        work.files[0].drive = bytes[equals - 1u] - '0';
        work.files[1].drive = work.drive;
        memset(work.files[DOS_COPY_PATTERN_INDEX].name, DISKIMAGE_FILENAME_PADDING, DISKIMAGE_FILENAME_SIZE);
        work.files[DOS_COPY_PATTERN_INDEX].name[0] = '*';
        work.files[DOS_COPY_PATTERN_INDEX].name_length = 1;
        copy_queue(unit, media, index, &work);
        return;
    }
    // TAGCMD permits one target before '=' and a bounded source group after
    // it, but no comma before the required colon.
    const unsigned int colon = after_colon - 1u;
    if (commas != 0) {
        dos_status(unit, model, DOS_STATUS_NO_FILE, 0, 0);
        return;
    }
    equals = colon + 1u;
    while (equals < length && bytes[equals] != '=') {
        ++equals;
    }
    if (equals == length) {
        dos_status(unit, model, DOS_STATUS_SYNTAX, 0, 0);
        return;
    }
    // Match the parsed group boundary: shifted CR terminates the source
    // list, and text past its table capacity is not checked as another name.
    // CPYDTD accepts exactly one source when the destination begins '*'.
    const bool pattern_copy = bytes[colon + 1u] == '*';
    unsigned int sources = 1;
    for (unsigned int position = 0; position < length; ++position) {
        if (position > equals && bytes[position] == 0x8d) {
            break;
        }
        if (position > equals && bytes[position] == ',') {
            if (pattern_copy) {
                dos_status(unit, model, DOS_STATUS_SYNTAX, 0, 0);
                return;
            }
            if (++sources == DOS_FILENAME_COUNT) {
                break;
            }
        }
        if ((!pattern_copy && (bytes[position] == '*' || bytes[position] == '?')) ||
            (position < equals && bytes[position] == ',')) {
            dos_status(unit, model, DOS_STATUS_SYNTAX, 0, 0);
            return;
        }
    }
    dos_copy_work_t work = {
        .count = 1,
        .reader = DOS_UNUSED,
        .writer = DOS_UNUSED,
        .track = directory_track(model),
        .sector = DOS_DIRECTORY_FIRST_SECTOR,
        .pending = true,
    };
    // ALLDRS starts from LSTDRV. Descriptor zero is the target, followed by
    // source descriptors with command_name's bounded, padded names.
    const uint8_t selector = bytes[colon - 1u];
    uint8_t drive = selector == '0' || selector == '1' ? selector - '0' : unit->last_drive;
    work.files[0].drive = drive;
    work.files[0].name_length = command_name(bytes, length, colon + 1u, work.files[0].name);
    work.empty_target = work.files[0].name_length == 0;
    unsigned int mask = 1u << drive;
    unsigned int start = equals + 1u;
    do {
        dos_copy_file_t* const file = &work.files[work.count++];
        bool omitted = false;
        // SETDRV consumes ':' or a one-byte selector plus ':'. Invalid
        // explicit selectors retain the inherited drive, whereas no prefix
        // sets the native default-drive flag used by LOOKUP's fallback.
        if (start + 1u < length && bytes[start + 1u] == ':') {
            if (bytes[start] == '0' || bytes[start] == '1') {
                drive = bytes[start] - '0';
            }
            start += 2u;
        } else if (start < length && bytes[start] == ':') {
            ++start;
        } else {
            omitted = true;
        }
        // Empty or overlapping CPYDTD relocation corrupts native command RAM.
        // Reject these unaccepted cases instead of silently normalizing them.
        if (pattern_copy && (start >= length || length - start > DOS_COPY_PATTERN_TEXT_LIMIT)) {
            dos_status(unit, model, DOS_STATUS_SYNTAX, 0, 0);
            return;
        }
        if (start >= DOS_COMMAND_RECEIVE_CAPACITY) {
            dos_status(unit, model, DOS_STATUS_SYNTAX, 0, 0);
            return;
        }
        file->drive = drive | (omitted ? DOS_COPY_DEFAULT_DRIVE : 0);
        file->name_length = command_name(bytes, length, start, file->name);
        mask |= 1u << (drive + (omitted ? 2u : 0u));
        // PARSE collects commas through the source group. FNDLMT's '='
        // terminates this filename, not the remaining filename table.
        while (start < length && bytes[start] != ',' && bytes[start] != 0x8d) {
            ++start;
        }
        if (start == length || bytes[start] != ',') {
            break;
        }
        ++start;
    } while (work.count < DOS_FILENAME_COUNT);
    if (pattern_copy) {
        // DOS 2.7 compares the complete ALLDRS bytes, including the default
        // flag. The 4040 lacks that explicit same-drive rejection.
        if (model != DOS_MODEL_4040 && work.files[0].drive == work.files[1].drive) {
            dos_status(unit, model, DOS_STATUS_SYNTAX, 0, 0);
            return;
        }
        memcpy(work.files[DOS_COPY_PATTERN_INDEX].name, work.files[1].name, DISKIMAGE_FILENAME_SIZE);
        work.files[DOS_COPY_PATTERN_INDEX].name_length = work.files[1].name_length;
        work.files[DOS_COPY_PATTERN_INDEX].type = DOS_COPY_PATTERN_FLAG;
        work.files[1].drive &= 1u;
        work.drive = work.files[1].drive;
        work.phase = DOS_COPY_ALL_NEXT;
        work.all_files = true;
        work.all_track = directory_track(model);
        work.all_sector = DOS_DIRECTORY_FIRST_SECTOR;
        copy_queue(unit, media, index, &work);
        return;
    }
    // OPTSCH/SCHTBL combine explicit drives in the low two mask bits and
    // defaulted drives in the next two, choosing the start and remaining scans.
    work.drive = mask < 4u    ? (mask == 3u ? work.files[0].drive : mask - 1u)
                 : mask < 8u  ? 0
                 : mask < 12u ? 1
                              : work.files[0].drive;
    work.scans = mask < 3u ? 0 : mask < 12u ? 1 : 2;
    copy_queue(unit, media, index, &work);
}

// Validate RENAME's two unpatterned names and reserve its cooperative scan pages.
// Queue directory work only after parsing, media binding, and resource checks.
static void rename_start(dos_unit_t* const unit, dos_media_bank_t* const media, const unsigned int index,
                         const unsigned int length) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rename
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rename
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/parsex
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/parsex
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/setdrv
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/setdrv
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/romtbl
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/romtbl
    //
    // TAGCMD checks STRUCT before RENAME calls ALLDRS/LOOKUP/CHKIO.
    // C extracts that parsing and drive setup here, deferring lookup and
    // in-place directory publication to rename_work without sector I/O now.
    const dos_model_t model = media->units[index].model;
    const uint8_t* const bytes = unit->command_bytes;

    // TAGCMD requires a colon before any comma (NOFILE/34). Once that
    // structure exists, a missing equals is a distinct BADSYN/30 failure.
    unsigned int colon = 0;
    while (colon < length && bytes[colon] != ':' && bytes[colon] != ',') {
        ++colon;
    }
    if (colon == length || bytes[colon] != ':') {
        dos_status(unit, model, DOS_STATUS_NO_FILE, 0, 0);
        return;
    }
    unsigned int equals = colon + 1u;
    while (equals < length && bytes[equals] != '=') {
        ++equals;
    }
    if (equals == length) {
        dos_status(unit, model, DOS_STATUS_SYNTAX, 0, 0);
        return;
    }
    // PARSE's shifted CR terminates structural scanning of the second stream.
    for (unsigned int position = 0; position < length; ++position) {
        const uint8_t byte = bytes[position];
        if (position > equals && byte == 0x8d) {
            break;
        }
        if (byte == ',' || byte == '*' || byte == '?') {
            dos_status(unit, model, DOS_STATUS_SYNTAX, 0, 0);
            return;
        }
    }
    // ALLDRS inherits LSTDRV for the target. SETDRV consumes ':' or a
    // one-byte selector plus ':', retaining that drive for invalid selectors.
    const uint8_t selector = bytes[colon - 1u];
    const uint8_t target = selector == '0' || selector == '1' ? selector - '0' : unit->last_drive;
    uint8_t source = target;
    unsigned int start = equals + 1u;
    if (start + 1u < length && bytes[start + 1u] == ':') {
        if (bytes[start] == '0' || bytes[start] == '1') {
            source = bytes[start] - '0';
        }
        start += 2u;
    } else if (start < length && bytes[start] == ':') {
        ++start;
    }
    // RENAME binds the source disk, with differing prefixes widening target
    // collision checks rather than moving the file to the destination disk.
    dos_rename_work_t work = {
        .source_drive = source,
        .track = directory_track(model),
        .sector = DOS_DIRECTORY_FIRST_SECTOR,
        .both_drives = target != source,
        .pending = true,
    };
    // Retain the zero-length target marker so rename_work can reproduce
    // TRNAME's native empty-target spill instead of sanitizing the request.
    work.name_lengths[DOS_RENAME_TARGET] = command_name(bytes, length, colon + 1u, work.names[DOS_RENAME_TARGET]);
    work.empty_target = work.name_lengths[DOS_RENAME_TARGET] == 0;
    work.name_lengths[DOS_RENAME_SOURCE] = command_name(bytes, length, start, work.names[DOS_RENAME_SOURCE]);
    unit->selected_drive = source;
    if (dos_media_reference(media, index, source, &work.media[source]) != DOS_MEDIA_OK) {
        dos_status(unit, model, DOS_STATUS_NOT_READY, 0, 0);
        return;
    }
    // A differing destination prefix checks both disks, but never moves the file.
    dos_media_reference(media, index, source ^ 1u, &work.media[source ^ 1u]);
    // Capture intent before yielding: CLOSE can publish and release its owner
    // after lookup has already passed that directory sector.
    uint8_t name[DISKIMAGE_FILENAME_SIZE];
    memcpy(name, work.names[DOS_RENAME_TARGET], sizeof(name));
    if (work.empty_target) {
        name[0] = '=';
    }
    for (unsigned int slot = 0; slot < DOS_MEDIA_SLOTS_PER_UNIT; ++slot) {
        if ((slot == source || work.both_drives) &&
            unpublished_writer(unit, work.media[slot], name, DOS_UNUSED) != NULL) {
            work.name_conflict = true;
        }
    }

    // Host resource policy probes an available logical index but publishes
    // no channel for this job. Only the two scratch pages are actually claimed.
    unsigned int internal = 0;
    while (internal < DOS_FILE_CHANNEL_COUNT &&
           (unit->channels[internal].allocated || unit->directory_internal == internal + 1u)) {
        ++internal;
    }
    work.scratch[0] = get_buffer(unit);
    work.scratch[1] = get_buffer(unit);
    if (internal == DOS_FILE_CHANNEL_COUNT || work.scratch[0] == DOS_UNUSED || work.scratch[1] == DOS_UNUSED) {
        // Roll back only successful claims, leaving peer owners untouched.
        for (unsigned int buffer = 0; buffer < count_of(work.scratch); ++buffer) {
            if (work.scratch[buffer] != DOS_UNUSED) {
                unit->buffer_use &= (uint16_t)~(1u << work.scratch[buffer]);
            }
        }
        dos_status(unit, model, DOS_STATUS_NO_CHANNEL, 0, 0);
        return;
    }
    // Publish the fully reserved job. OK is its working status, not proof
    // that either name exists or the rename has reached persistent storage.
    unit->rename = work;
    unit->command = DOS_COMMAND_RENAME;
    dos_status(unit, model, DOS_STATUS_OK, 0, 0);
}

// Parse SCRATCH's pattern group/type filter and reserve its cooperative work.
// Defer matching, active/locked checks, deletion, and allocation release.
static void scratch_start(dos_unit_t* const unit, dos_media_bank_t* const media, const unsigned int index,
                          const unsigned int length) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/parsex
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/parsex
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/setdrv
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/setdrv
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lookup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lookup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/scrtch
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/scrtch
    //
    // TAGCMD supplies pattern groups. SCRTCH calls FS1SET, ALLDRS, and
    // OPTSCH before iterating matches. C retains this setup in a bounded job
    // rather than deleting entries synchronously inside command dispatch.
    const dos_model_t model = media->units[index].model;
    const uint8_t* const bytes = unit->command_bytes;
    unsigned int colon = 0;
    while (colon < length && bytes[colon] != ':' && bytes[colon] != ',') {
        ++colon;
    }
    if (colon == length || bytes[colon] != ':') {
        dos_status(unit, model, DOS_STATUS_NO_FILE, 0, 0);
        return;
    }
    dos_scratch_work_t work = {
        .length = length,
        .names = 1,
        .track = directory_track(model),
        .sector = DOS_DIRECTORY_FIRST_SECTOR,
        .pending = true,
    };
    // Keep private command storage: dispatch may clear the input buffer
    // before scratch_work resumes matching names from these saved offsets.
    memcpy(work.command, bytes, sizeof(work.command));
    work.starts[0] = colon + 1u;
    // PARSE's pattern flag includes the command prefix preceding the colon.
    unsigned int name = 0;
    for (unsigned int position = 0; position < length; ++position) {
        const uint8_t byte = bytes[position];
        if (byte == '*' || byte == '?') {
            work.patterns |= 1u << name;
        }
        if (position <= colon) {
            continue;
        }
        if (byte == '=') {
            // FS1SET consumes only the first type character. Unknown types
            // disable filtering, while TAGCMD rejects a patterned/grouped FS2.
            for (unsigned int tail = position + 1u; tail < length && bytes[tail] != 0x8d; ++tail) {
                if (bytes[tail] == ',' || bytes[tail] == '*' || bytes[tail] == '?') {
                    dos_status(unit, model, DOS_STATUS_SYNTAX, 0, 0);
                    return;
                }
            }
            const char* const types = "DSPUR";
            const char* const type = bytes[position + 1u] == 0 ? NULL : strchr(types, bytes[position + 1u]);
            work.type = type == NULL ? 0 : (uint8_t)(type - types);
            break;
        }
        if (byte == ',') {
            // PARSE's MXFILS bound retains five starts, then stops scanning.
            work.starts[work.names++] = position + 1u;
            if (++name == DOS_FILENAME_COUNT - 1u) {
                break;
            }
        }
    }
    // ALLDRS carries each resolved drive into the next pattern. A missing
    // group prefix keeps the default flag so LOOKUP can match on a peer.
    const uint8_t selector = bytes[colon - 1u];
    uint8_t drive = selector == '0' || selector == '1' ? selector - '0' : unit->last_drive;
    unsigned int mask = 0;
    for (unsigned int pattern = 0; pattern < work.names; ++pattern) {
        unsigned int start = work.starts[pattern];
        bool omitted = false;
        if (pattern != 0) {
            if (start + 1u < length && bytes[start + 1u] == ':') {
                if (bytes[start] == '0' || bytes[start] == '1') {
                    drive = bytes[start] - '0';
                }
                start += 2u;
            } else if (bytes[start] == ':') {
                ++start;
            } else {
                omitted = true;
                work.defaults |= 1u << pattern;
            }
        }
        if (start >= sizeof(work.command)) {
            // SETDRV can advance past a retained EOF colon. Native RAM
            // beyond this saved region is not modeled as adjacent C state.
            dos_status(unit, model, DOS_STATUS_SYNTAX, 0, 0);
            return;
        }
        work.starts[pattern] = start;
        work.drives[pattern] = drive;
        mask |= 1u << (drive + (omitted ? 2u : 0u));
    }
    // SCHTBL encodes the optimal start drive and zero, one, or two toggles.
    work.drive = mask < 4u    ? (mask == 3u ? work.drives[0] : mask - 1u)
                 : mask < 8u  ? 0
                 : mask < 12u ? 1
                              : work.drives[0];
    work.scans = mask < 3u ? 0 : mask < 12u ? 1 : 2;
    unit->selected_drive = work.drive;

    // Host media identities bind deferred traversal to these image instances.
    // No disk sectors are read while choosing the initial available drive.
    for (unsigned int slot = 0; slot < DOS_MEDIA_SLOTS_PER_UNIT; ++slot) {
        work.media[slot] = (dos_media_ref_t){.unit = index, .slot = slot};
        dos_media_reference(media, index, slot, &work.media[slot]);
    }
    if (work.media[work.drive].identity == 0) {
        // DOS 2.7 OPTSCH omits an absent peer during a multi-drive search.
        if (work.scans != 0 && work.media[work.drive ^ 1u].identity != 0) {
            work.drive ^= 1u;
            work.scans = 0;
            unit->selected_drive = work.drive;
        } else {
            dos_status(unit, model, DOS_STATUS_NOT_READY, 0, 0);
            return;
        }
    }

    if (work.scans != 0 && work.media[work.drive ^ 1u].identity == 0) {
        // DOS 2.7 OS50 reduces DRVCNT for an inactive peer. Missing 4040
        // bindings use the same host policy as the absent starting drive.
        work.scans = 0;
    }

    // As with RENAME, probe logical capacity without publishing a channel,
    // then reserve two shared pages for directory and chain traversal.
    unsigned int internal = 0;
    while (internal < DOS_FILE_CHANNEL_COUNT &&
           (unit->channels[internal].allocated || unit->directory_internal == internal + 1u)) {
        ++internal;
    }
    work.buffers[0] = get_buffer(unit);
    work.buffers[1] = get_buffer(unit);
    if (internal == DOS_FILE_CHANNEL_COUNT || work.buffers[0] == DOS_UNUSED || work.buffers[1] == DOS_UNUSED) {
        // Undo partial reservations before reporting NO CHANNEL.
        for (unsigned int buffer = 0; buffer < count_of(work.buffers); ++buffer) {
            if (work.buffers[buffer] != DOS_UNUSED) {
                unit->buffer_use &= (uint16_t)~(1u << work.buffers[buffer]);
            }
        }
        dos_status(unit, model, DOS_STATUS_NO_CHANNEL, 0, 0);
        return;
    }
    // The worker, not this setup path, publishes the FILES SCRATCHED count.
    unit->scratch = work;
    unit->command = DOS_COMMAND_SCRATCH;
    dos_status(unit, model, DOS_STATUS_OK, 0, 0);
}

// Retire a successfully reserved rename job without changing its final status.
static void rename_finish(dos_unit_t* const unit) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4
    //
    // FREICH releases internal read/write channels through FRECHN. This host
    // helper has no direct ROM entry: rename_start reserves scratch pages
    // without publishing an internal channel, so release only those page bits.
    dos_rename_work_t* const work = &unit->rename;
    for (unsigned int buffer = 0; buffer < count_of(work->scratch); ++buffer) {
        unit->buffer_use &= (uint16_t)~(1u << work->scratch[buffer]);
    }

    // Stop cooperative execution on success or error, retaining peer owners.
    work->pending = false;
    unit->command = DOS_COMMAND_IDLE;
}

// Search both rename names cooperatively, then update the source entry in place.
// Preserve source-before-collision errors and leave file payloads untouched.
static void rename_work(dos_unit_t* const unit, dos_media_bank_t* const media) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rename
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rename
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lookup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lookup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/trnsfr
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/trnsfr
    //
    // RENAME performs LOOKUP for both names, then CHKIO calls CHKIN before
    // testing destination existence. RDAB/TRNAME/WRTOUT rename the source
    // directory entry, even when differing prefixes require collision checks
    // on both disks. C retains scan/commit state between scheduler calls.
    dos_rename_work_t* const work = &unit->rename;
    const uint8_t drive = work->source_drive ^ work->tried_peer;
    const dos_media_ref_t reference = work->media[drive];
    const dos_model_t model = media->units[reference.unit].model;
    auto_initialize(unit, drive);
    uint8_t* const bytes = unit->buffers[work->scratch[0]].bytes;
    unit->selected_drive = drive;
    if (work->commit) {
        const dos_media_ref_t source = work->media[work->source_drive];
        unit->selected_drive = work->source_drive;
        // A failed retained publication must keep one canonical name. A
        // partial rename cannot safely give that intent another visible alias.
        for (unsigned int index = 0; index < DOS_FILE_CHANNEL_COUNT; ++index) {
            const dos_channel_t* const owner = &unit->channels[index];
            if (owner->allocated && owner->access == DOS_ACCESS_WRITE && !owner->directory_published &&
                owner->closing != DOS_CLOSE_ABORT_OPEN && owner->media.unit == source.unit &&
                owner->media.slot == source.slot && owner->media.identity == source.identity &&
                owner->directory_track == work->source_track && owner->directory_sector == work->source_sector &&
                owner->directory_entry == work->source_entry) {
                dos_status(unit, model, DOS_STATUS_WRITE_ERROR, work->source_track, work->source_sector);
                rename_finish(unit);
                return;
            }
        }
        // Reread at commit so unrelated directory edits cannot be rolled back.
        if (storage_error(unit, media, source,
                          read_sector(unit, media, source, work->source_track, work->source_sector, bytes), false,
                          work->source_track, work->source_sector)) {
            uint8_t* const name = bytes + work->source_entry + DOS_DIRECTORY_NAME_OFFSET;
            // TRNAME replaces the bounded, padded name, preserving normal
            // entry metadata regardless of its closed or locked state.
            memcpy(name, work->names[DOS_RENAME_TARGET], DISKIMAGE_FILENAME_SIZE);
            if (work->empty_target) {
                // TRCMBF copies '=' even at a zero-length limit, then TRNAME
                // pads sixteen bytes, spilling into the next entry field.
                name[0] = '=';
                name[DISKIMAGE_FILENAME_SIZE] = DISKIMAGE_FILENAME_PADDING;
            }
            if (write_sector(unit, media, source, work->source_track, work->source_sector, bytes)) {
                for (unsigned int index = 0; index < DOS_FILE_CHANNEL_COUNT; ++index) {
                    dos_channel_t* const channel = &unit->channels[index];
                    if (channel->allocated && channel->access == DOS_ACCESS_WRITE &&
                        channel->media.identity == source.identity && channel->media.slot == source.slot &&
                        channel->directory_track == work->source_track &&
                        channel->directory_sector == work->source_sector &&
                        channel->directory_entry == work->source_entry) {
                        // C writers retain a name for retryable publication.
                        // Keep a later CLOSE from undoing a native rename.
                        memcpy(channel->pattern, name, sizeof(channel->pattern));
                    }
                }
                // Host flush is separate from native WRTOUT/WATJOB. A flush
                // failure reports an error without undoing the directory write.
                if (storage_error(unit, media, source, dos_media_flush_unit(media, source.unit), true, 0, 0)) {
                    command_end(unit, model);
                }
            }
        }
        rename_finish(unit);
        return;
    }
    // Host corruption bound: a directory cycle cannot keep the job alive
    // indefinitely. Media reads also validate the captured image identity.
    const unsigned int limit = media->units[reference.unit].slots[drive].io.image.size / DISKIMAGE_SECTOR_SIZE;
    if (work->visited++ >= limit) {
        dos_status(unit, model, DOS_STATUS_DIRECTORY_ERROR, work->track, work->sector);
        rename_finish(unit);
        return;
    }
    if (!storage_error(unit, media, reference, read_sector(unit, media, reference, work->track, work->sector, bytes),
                       false, work->track, work->sector)) {
        rename_finish(unit);
        return;
    }
    // LOOKUP/COMPAR retain first matches. Only the source disk can supply the
    // source entry, while a target collision may be found on either disk.
    for (unsigned int offset = 0; offset < DISKIMAGE_SECTOR_SIZE; offset += DISKIMAGE_DIRECTORY_ENTRY_SIZE) {
        const uint8_t* const entry = bytes + offset;
        if (entry[DOS_DIRECTORY_TYPE_OFFSET] == 0 ||
            (entry[DOS_DIRECTORY_TRACK_OFFSET] & (model == DOS_MODEL_4040 ? 0x7fu : 0xffu)) == 0) {
            continue;
        }
        if (drive == work->source_drive && !(work->found & DOS_RENAME_SOURCE_FOUND) &&
            name_matches(work->names[DOS_RENAME_SOURCE], work->name_lengths[DOS_RENAME_SOURCE],
                         entry + DOS_DIRECTORY_NAME_OFFSET)) {
            work->found |= DOS_RENAME_SOURCE_FOUND;
            work->source_track = work->track;
            work->source_sector = work->sector;
            work->source_entry = (uint8_t)offset;
        }
        if (!(work->found & DOS_RENAME_TARGET_FOUND) &&
            name_matches(work->names[DOS_RENAME_TARGET], work->name_lengths[DOS_RENAME_TARGET],
                         entry + DOS_DIRECTORY_NAME_OFFSET)) {
            work->found |= DOS_RENAME_TARGET_FOUND;
        }
        if (work->found == DOS_RENAME_ALL_FOUND) {
            break;
        }
    }
    // Follow one directory-sector link per pass before considering the peer.
    if (work->found != DOS_RENAME_ALL_FOUND && bytes[0] != 0) {
        work->track = bytes[0];
        work->sector = bytes[1];
        return;
    }
    if (work->found != DOS_RENAME_ALL_FOUND && work->both_drives && !work->tried_peer &&
        work->media[drive ^ 1u].identity != 0) {
        // Preserve earlier matches across the optional second-disk scan.
        work->tried_peer = true;
        work->track = directory_track(model);
        work->sector = DOS_DIRECTORY_FIRST_SECTOR;
        work->visited = 0;
        return;
    }
    // CHKIN's missing source wins over CHKIO's collision, including when both
    // conditions hold. Renaming a file to itself is an existing-target error.
    if (!(work->found & DOS_RENAME_SOURCE_FOUND)) {
        dos_status(unit, model, DOS_STATUS_NOT_FOUND, 0, 0);
    } else if (work->found & DOS_RENAME_TARGET_FOUND) {
        dos_status(unit, model, DOS_STATUS_FILE_EXISTS, 0, 0);
    } else if (work->name_conflict) {
        dos_status(unit, model, DOS_STATUS_WRITE_ERROR, 0, 0);
    } else {
        work->commit = true;
        return;
    }
    rename_finish(unit);
}

// Retire SCRATCH's reserved pages and scheduled work without replacing status.
static void scratch_finish(dos_unit_t* const unit) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4
    //
    // FREICH releases native internal channels. This host helper has no
    // direct ROM entry: the cooperative scratch job owns two page reservations
    // rather than published internal channels, and must leave peer pages alone.
    for (unsigned int buffer = 0; buffer < count_of(unit->scratch.buffers); ++buffer) {
        unit->buffer_use &= (uint16_t)~(1u << unit->scratch.buffers[buffer]);
    }

    // Cleanup is shared by successful and failed jobs, without a new status.
    unit->scratch.pending = false;
    unit->command = DOS_COMMAND_IDLE;
}

// TSTCHN protects an entry by drive and directory coordinates, not its name.
static bool scratch_active(const dos_unit_t* const unit, const dos_media_ref_t reference, const uint8_t track,
                           const uint8_t sector, const uint8_t entry) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tstflg
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tstflg
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/open
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/open
    //
    // TSTCHN compares drive and packed DIRENT (4040), or DSEC/DIND (DOS 2.7),
    // returning carry clear for an active entry. C returns true for that case.
    // Track and host media generation additionally qualify the stored location.
    // A listing's search cursor is not OPREAD's saved file-entry binding.
    for (unsigned int index = 0; index < DOS_FILE_CHANNEL_COUNT; ++index) {
        const dos_channel_t* const channel = &unit->channels[index];
        if (channel->allocated && channel->directory == DOS_DIRECTORY_NONE &&
            channel->media.identity == reference.identity && channel->media.slot == reference.slot &&
            channel->directory_track == track && channel->directory_sector == sector &&
            channel->directory_entry == entry) {
            return true;
        }
    }

    // No allocated owner matches: the caller may continue its scratch checks.
    return false;
}

// SCRTCH/FFRE remove one entry, then DELFIL/FRETS release its chains before resuming.
static void scratch_work(dos_unit_t* const unit, dos_media_bank_t* const media) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/scrtch
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/scrtch
    // Lookup re-entry rereads the directory after DELFIL reuses its buffers.
    dos_scratch_work_t* const work = &unit->scratch;
    const dos_media_ref_t reference = work->media[work->drive];
    const dos_model_t model = media->units[reference.unit].model;
    const unsigned int limit = media->units[reference.unit].slots[reference.slot].io.image.size / DISKIMAGE_SECTOR_SIZE;
    unit->selected_drive = work->drive;
    auto_initialize(unit, work->drive);
    uint8_t* const bytes = unit->buffers[work->buffers[0]].bytes;
    if (work->phase == DOS_SCRATCH_CHAIN) {
        unsigned int sectors;
        // FRETS precedes the read, so a failed read may leave a dirty BAM.
        if (work->chain_visited++ >= limit) {
            dos_status(unit, model, DOS_STATUS_DIRECTORY_ERROR, work->chain_track, work->chain_sector);
        } else if (!diskimage_track_sectors(media->units[reference.unit].slots[reference.slot].io.image.type,
                                            work->chain_track, &sectors) ||
                   work->chain_sector >= sectors) {
            // Both SETJOB paths report BADTS ($66). Check before any
            // unchecked native BAM indexing would touch host memory.
            dos_status(unit, model, DOS_STATUS_ILLEGAL_TRACK_SECTOR, work->chain_track, work->chain_sector);
        } else if (mark_sector(unit, media, reference, work->chain_track, work->chain_sector, true)) {
            if (storage_error(unit, media, reference,
                              read_sector(unit, media, reference, work->chain_track, work->chain_sector, bytes), false,
                              work->chain_track, work->chain_sector)) {
                work->chain_track = bytes[0];
                work->chain_sector = bytes[1];
                if (work->chain_track == 0) {
                    work->phase = DOS_SCRATCH_BAM;
                }
                return;
            }
        } else if (unit->status_code == DOS_STATUS_OK) {
            dos_status(unit, model, DOS_STATUS_READ_ERROR, work->chain_track, work->chain_sector);
        }
        scratch_finish(unit);
        return;
    }
    if (work->phase == DOS_SCRATCH_BAM) {
        if (!flush_bam(unit, media, &unit->bam[work->drive])) {
            scratch_finish(unit);
            return;
        }
        if (work->data_pending) {
            work->chain_track = work->next_chain_track;
            work->chain_sector = work->next_chain_sector;
            work->next_chain_track = 0;
            work->data_pending = false;
            work->chain_visited = 0;
            work->phase = DOS_SCRATCH_CHAIN;
        } else {
            ++work->count;
            work->phase = DOS_SCRATCH_SEARCH;
        }
        return;
    }
    const bool all_found = work->patterns == 0 && work->matched == (1u << work->names) - 1u;
    if (work->track == 0 || all_found) {
        if (!all_found && work->scans != 0 && work->media[work->drive ^ 1u].identity != 0) {
            --work->scans;
            work->drive ^= 1u;
            work->track = directory_track(model);
            work->sector = DOS_DIRECTORY_FIRST_SECTOR;
            work->entry = 0;
            work->visited = 0;
            return;
        }
        // Unit flush resolves current bindings, unlike sector I/O. Reject an
        // old pair generation even when no further sector access is needed.
        dos_media_ref_t current = {0};
        if (dos_media_reference(media, reference.unit, reference.slot, &current) != DOS_MEDIA_OK ||
            current.identity != reference.identity) {
            dos_status(unit, model, DOS_STATUS_WRITE_ERROR, 0, 0);
        } else if (storage_error(unit, media, reference, dos_media_flush_unit(media, reference.unit), true, 0, 0)) {
            dos_status(unit, model, DOS_STATUS_FILES_SCRATCHED, work->count, 0);
            unit->last_drive = unit->selected_drive;
        }
        scratch_finish(unit);
        return;
    }
    if (work->entry == 0 && work->visited++ >= limit) {
        dos_status(unit, model, DOS_STATUS_DIRECTORY_ERROR, work->track, work->sector);
        scratch_finish(unit);
        return;
    }
    if (!storage_error(unit, media, reference, read_sector(unit, media, reference, work->track, work->sector, bytes),
                       false, work->track, work->sector)) {
        scratch_finish(unit);
        return;
    }
    for (; work->entry < DISKIMAGE_SECTOR_SIZE / DISKIMAGE_DIRECTORY_ENTRY_SIZE; ++work->entry) {
        const unsigned int offset = work->entry * DISKIMAGE_DIRECTORY_ENTRY_SIZE;
        uint8_t* const entry = bytes + offset;
        const uint8_t type = entry[DOS_DIRECTORY_TYPE_OFFSET];
        if (type == 0) {
            continue;
        }
        bool matched = false;
        // COMPAR considers later patterns first and resolves exact names once,
        // even when TSTCHN, the lock bit, or a type restriction then skips them.
        for (unsigned int pattern = work->names; pattern-- != 0;) {
            const uint8_t bit = 1u << pattern;
            if ((work->matched & bit) && !(work->patterns & bit)) {
                continue;
            }
            if (work->drives[pattern] != work->drive && (!(work->defaults & bit) || work->scans == 2)) {
                continue;
            }
            uint8_t name[DISKIMAGE_FILENAME_SIZE];
            const uint8_t name_length = command_name(work->command, work->length, work->starts[pattern], name);
            if (name_matches(name, name_length, entry + DOS_DIRECTORY_NAME_OFFSET)) {
                work->matched |= bit;
                matched = true;
                break;
            }
        }
        if (!matched || (work->type != 0 && work->type != (type & DOS_FILE_TYPE_MASK)) ||
            (type & DOS_FILE_LOCKED_MASK) || scratch_active(unit, reference, work->track, work->sector, offset)) {
            continue;
        }
        entry[DOS_DIRECTORY_TYPE_OFFSET] = 0;
        if (!write_sector(unit, media, reference, work->track, work->sector, bytes)) {
            scratch_finish(unit);
            return;
        }
        const uint8_t data_track = type & DOS_FILE_CLOSED_MASK
                                       ? entry[DOS_DIRECTORY_TRACK_OFFSET] & (model == DOS_MODEL_4040 ? 0x7fu : 0xffu)
                                       : 0;
        // SC15 completes the side chain's DELFIL/MAPOUT before SC17 tries
        // a closed entry's data start, including an invalid track zero.
        work->data_pending = entry[DOS_DIRECTORY_SIDE_TRACK] != 0 && (type & DOS_FILE_CLOSED_MASK) != 0;
        work->chain_track = entry[DOS_DIRECTORY_SIDE_TRACK] != 0 ? entry[DOS_DIRECTORY_SIDE_TRACK] : data_track;
        work->chain_sector = entry[DOS_DIRECTORY_SIDE_TRACK] != 0 ? entry[DOS_DIRECTORY_SIDE_SECTOR]
                                                                  : entry[DOS_DIRECTORY_SECTOR_OFFSET];
        work->next_chain_track = entry[DOS_DIRECTORY_SIDE_TRACK] != 0 ? data_track : 0;
        work->next_chain_sector = entry[DOS_DIRECTORY_SECTOR_OFFSET];
        work->chain_visited = 0;
        work->phase =
            work->chain_track != 0 || (type & DOS_FILE_CLOSED_MASK) != 0 ? DOS_SCRATCH_CHAIN : DOS_SCRATCH_SEARCH;
        ++work->entry;
        if (work->phase == DOS_SCRATCH_SEARCH) {
            ++work->count;
        }
        return;
    }
    work->track = bytes[0];
    work->sector = bytes[1];
    work->entry = 0;
}

// Advance scheduled initialization, abandoning each selected disk's channels
// without CLOSE and reloading metadata through the captured media references.
static void initialize_work(dos_unit_t* const unit, dos_media_bank_t* const media) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst3
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst3
    //
    // INITDR calls CLDCHN/FRECHN, not writable CLOSE. 4040 then reads its
    // single BAM, while DOS 2.7 reads the directory header and NF05/NF10
    // sums free counts through SETMAP. C schedules these reads cooperatively,
    // without emulating physical seek/trial jobs or controller disk-ID state.
    dos_initialize_work_t* const work = &unit->initialize;

    // The caller supplies the slot mask and first-drive order. Finish that
    // disk before its peer, retaining progress across scheduler calls.
    const uint8_t drive = (work->slots & (1u << work->first_drive)) ? work->first_drive : work->first_drive ^ 1u;
    const dos_media_ref_t reference = work->media[drive];
    const dos_model_t model = media->units[reference.unit].model;
    dos_bam_t* const bam = &unit->bam[drive];
    unit->selected_drive = drive;
    if (!work->started) {
        // CLDCHN selects the active page's LSTJOB drive, preserving peer owners.
        // Abandoned writers get no final links, closed entry, or CLOSE BAM flush.
        free_drive_channels(unit, drive);
        // Discard dirty cached allocation state instead of persisting it.
        // The free-block total stays invalid until every map range is counted.
        // Backing flush ownership survives even if the first refill fails.
        *bam = (dos_bam_t){.media = reference};
        unit->free_blocks[drive] = 0;
        unit->free_blocks_valid[drive] = false;
        work->started = true;
        work->next_track = 0;
    }
    if (reference.identity == 0) {
        // Retain the approved absent-image policy, not physical seek errors.
        dos_status(unit, model, DOS_STATUS_NOT_READY, 0, 0);
        work->pending = false;
        return;
    }
    if (work->next_track == 0) {
        // Sector 0 is the BAM itself on 4040, but only the directory header on
        // 8050/8250. Do not expose that header as a resident allocation-map part.
        const uint8_t track = directory_track(model);
        if (!storage_error(unit, media, reference, read_sector(unit, media, reference, track, 0, bam->buffer.bytes),
                           false, track, 0)) {
            work->pending = false;
            return;
        }
        bam->media = reference;
        bam->buffer.track = track;
        bam->buffer.sector = 0;
        bam->buffer.resident = model == DOS_MODEL_4040;
        const unsigned int name = model == DOS_MODEL_4040 ? DOS_HEADER_NAME_4040 : DOS_HEADER_NAME_8050;
        memcpy(work->id, bam->buffer.bytes + name + DOS_DISK_ID_OFFSET, sizeof(work->id));
        work->next_track = 1;
        if (model != DOS_MODEL_4040) {
            return;
        }
    }
    const unsigned int tracks = model == DOS_MODEL_4040   ? DISKIMAGE_D64_TRACK_COUNT
                                : model == DOS_MODEL_8050 ? DISKIMAGE_D80_TRACK_COUNT
                                                          : DISKIMAGE_D82_TRACK_COUNT;
    // Sum one BAM range per pass. Loading a later linked part is bounded by
    // the model's four-part limit, never by an arbitrary directory chain.
    const uint8_t* entry = bam_entry(unit, media, reference, (uint8_t)work->next_track);
    if (entry == NULL) {
        work->pending = false;
        return;
    }
    const unsigned int high = model == DOS_MODEL_4040 ? tracks + 1u : bam->buffer.bytes[5];
    const unsigned int stride = model == DOS_MODEL_4040 ? 4u : 5u;
    // NF10 excludes the entire directory track. C also caches this sum for
    // 4040, whose INITDR does not itself perform DOS 2.7's NF05 calculation.
    while (work->next_track < high && work->next_track <= tracks) {
        if (work->next_track != directory_track(model)) {
            unit->free_blocks[drive] += entry[0];
        }
        ++work->next_track;
        entry += stride;
    }
    if (work->next_track <= tracks) {
        return;
    }

    // Commit this disk's total only on completion. DOS 2.7 leaves its last
    // BAM part resident after the ascending SETMAP scan, as native NF10 does.
    unit->free_blocks_valid[drive] = true;
    work->slots &= (uint8_t)~(1u << drive);
    work->started = false;
    if (work->slots == 0) {
        work->pending = false;
        work->succeeded = true;
        unit->selected_drive = work->first_drive;
        if (unit->command == DOS_COMMAND_IDLE) {
            // VALIDATE owns its eventual success/recovery status. An ordinary
            // initialization publishes OK only after all requested disks finish.
            command_end(unit, model);
        }
    }
}

// Release VALIDATE's reader and scratch reservations, then retire its scheduled work.
// Preserve the current status and leave unrelated channel allocations intact.
static void validate_finish(dos_unit_t* const unit) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/verdir
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/verdir
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/erproc
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/erproc
    //
    // MRKBAM/VMKBAM release their internal reader after a chain, and command
    // termination/error cleanup frees internal channels. This host helper has
    // no direct ROM entry: it retires the cooperative job's bounded resources
    // on either outcome without publishing a new completion status.
    dos_validate_work_t* const work = &unit->validate;

    // Release the reader's owned buffers only if allocation actually succeeded.
    if (work->reader != DOS_UNUSED && unit->channels[work->reader].allocated) {
        free_channel(unit, work->reader);
    }

    // Return only scratch pages recorded by this job, not the whole pool.
    unit->buffer_use &= (uint16_t)~work->owned;

    // Stop any delegated initialization and return command scheduling to idle.
    unit->initialize.pending = false;
    unit->command = DOS_COMMAND_IDLE;
}

// Save VALIDATE's failure and schedule reinitialization of its selected disk.
// validate_work restores the saved status only after successful recovery.
static void validate_recover(dos_unit_t* const unit, const dos_model_t model, const uint8_t track,
                             const uint8_t sector) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/erproc
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/erproc
    //
    // ERROR (4040) and JERROR/ERROR1 (DOS 2.7) special-case CMDNUM=VAL:
    // format the controller error, call INITDR, then enter CMDER3 cleanup.
    // C splits recovery across scheduler calls and also uses this path for
    // host I/O/corruption failures.
    dos_validate_work_t* const work = &unit->validate;

    // Retain the original status and supplied coordinates across INITDR work.
    work->error = unit->status_code;
    work->error_track = track;
    work->error_sector = sector;
    work->phase = DOS_VALIDATE_RECOVER;

    // Reinitialize just the failed disk using its captured media identity.
    // Keep VALIDATE's resources until validate_work observes recovery completion.
    unit->initialize = (dos_initialize_work_t){
        .first_drive = work->media.slot,
        .slots = 1u << work->media.slot,
        .pending = true,
    };
    unit->initialize.media[work->media.slot] = work->media;

    // OK is the cooperative initialization's working status, not successful
    // VALIDATE completion. A recovery failure keeps its own error instead.
    dos_status(unit, model, DOS_STATUS_OK, 0, 0);
}

// Return a track's count/bitmap record in VALIDATE's rebuilt allocation map.
// Require a valid model track and initialized, reserved map storage.
static uint8_t* validate_entry(dos_unit_t* const unit, const dos_model_t model, const uint8_t track) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/verdir
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/frets
    //
    // 4040 FREUSE indexes four-byte track records in the drive's BAM.
    // DOS 2.7 VUSED selects a VBAMS page and MBAM computes its five-byte
    // record offset. C derives the rebuilt page directly from the track.
    if (model == DOS_MODEL_4040) {
        // The single 4040 BAM is rebuilt in its normal per-drive cache.
        return unit->bam[unit->validate.media.slot].buffer.bytes + track * 4u;
    }

    // DOS 2.7 reserves pages 0..3 for rebuilt parts, independent of the
    // ordinary one-part cache. Skip the six-byte header within each page.
    const unsigned int part = (track - 1u) / DOS_BAM_TRACKS_PER_PART;
    return unit->buffers[part].bytes + 6u + ((track - 1u) % DOS_BAM_TRACKS_PER_PART) * 5u;
}

// Mark one sector used in VALIDATE's rebuilt map and adjust its free counts.
// Accept duplicate marks on 4040, but report NO BLOCK for them on DOS 2.7.
static bool validate_used(dos_unit_t* const unit, const dos_media_bank_t* const media, const uint8_t track,
                          const uint8_t sector) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/verdir
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/verdir
    //
    // MRKBAM calls USEDTS, whose already-used branch makes no change.
    // VMKBAM/VUSED instead enters USDERR with NOBLK for duplicate ownership.
    // A successful first claim clears the bit and decrements its track count.
    const dos_media_ref_t reference = unit->validate.media;
    const dos_model_t model = media->units[reference.unit].model;
    unsigned int sectors;
    if (!diskimage_track_sectors(media->units[reference.unit].slots[reference.slot].io.image.type, track, &sectors) ||
        sector >= sectors) {
        // VMKBAM/TSCHK and 4040's later SETJOB report BADTS ($66).
        // Reject geometry before unchecked native BAM indexing.
        dos_status(unit, model, DOS_STATUS_ILLEGAL_TRACK_SECTOR, track, sector);
        return false;
    }
    // Inspect rebuilt state rather than mutating the persisted map's cache.
    uint8_t* const entry = validate_entry(unit, model, track);
    const uint8_t mask = 1u << (sector % 8u);
    if (!(entry[1u + sector / 8u] & mask)) {
        if (model != DOS_MODEL_4040) {
            dos_status(unit, model, DOS_STATUS_NO_BLOCK, track, sector);
            return false;
        }
        return true;
    }
    // Change counts only for a newly claimed sector. Keep directory-track
    // allocation out of the free-block total, as DOS 2.7 VUSED does.
    // C also maintains that total for 4040 instead of resumming it for NUMFRE.
    entry[1u + sector / 8u] &= (uint8_t)~mask;
    --entry[0];
    if (track != directory_track(model)) {
        --unit->free_blocks[reference.slot];
    }
    return true;
}

// Run one cooperative VALIDATE step, rebuilding maps from closed file chains.
// Directory deletions and completed map writes are not rolled back on failure.
static void validate_work(dos_unit_t* const unit, dos_media_bank_t* const media) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/verdir
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/verdir
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/map
    //
    // VALDAT initializes, builds free maps, scans entries at VD25/VD15, then
    // marks the directory chain at VD10 before MO10 (4040) or WTMAPS (DOS 2.7).
    // C splits this synchronous command into bounded scheduler steps, retaining
    // one reader across chains instead of opening/releasing it for each chain.
    dos_validate_work_t* const work = &unit->validate;
    const dos_media_ref_t reference = work->media;
    const dos_model_t model = media->units[reference.unit].model;
    const diskimage_t* const image = &media->units[reference.unit].slots[reference.slot].io.image;
    const unsigned int tracks = model == DOS_MODEL_4040   ? DISKIMAGE_D64_TRACK_COUNT
                                : model == DOS_MODEL_8050 ? DISKIMAGE_D80_TRACK_COUNT
                                                          : DISKIMAGE_D82_TRACK_COUNT;
    dos_bam_t* const bam = &unit->bam[reference.slot];
    unit->selected_drive = reference.slot;

    // Host identity guard: never continue an old traversal on replacement media.
    // Initialization/recovery perform their own captured-reference checks.
    dos_media_ref_t current = {0};
    if (work->phase != DOS_VALIDATE_INITIALIZE && work->phase != DOS_VALIDATE_RECOVER &&
        (dos_media_reference(media, reference.unit, reference.slot, &current) != DOS_MEDIA_OK ||
         current.identity != reference.identity)) {
        dos_status(unit, model, DOS_STATUS_READ_ERROR, 0, 0);
        if (bam->media.identity == reference.identity) {
            *bam = (dos_bam_t){0};
        }
        unit->free_blocks_valid[reference.slot] = false;
        validate_finish(unit);
        return;
    }
    if (work->phase == DOS_VALIDATE_INITIALIZE || work->phase == DOS_VALIDATE_RECOVER) {
        // Delegate INITDR until complete. A recovery failure keeps its new
        // status, while successful recovery restores the saved VALIDATE error.
        initialize_work(unit, media);
        if (unit->initialize.pending) {
            return;
        }
        if (!unit->initialize.succeeded) {
            validate_finish(unit);
            return;
        }
        if (work->phase == DOS_VALIDATE_RECOVER) {
            dos_status(unit, model, (dos_status_code_t)work->error, work->error_track, work->error_sector);
            validate_finish(unit);
            return;
        }
        // 4040 NEWMAP reuses its BAM cache. DOS 2.7 VSETB/NEWMPV builds
        // two (8050) or four (8250) scratch pages in the fixed VBAMS region.
        work->parts = model == DOS_MODEL_4040 ? 1 : (tracks + DOS_BAM_TRACKS_PER_PART - 1u) / DOS_BAM_TRACKS_PER_PART;
        if (model != DOS_MODEL_4040) {
            for (unsigned int part = 0; part < work->parts; ++part) {
                const uint16_t bit = 1u << part;
                if (unit->buffer_use & bit) {
                    // Native VBAMS uses fixed pages 0..3 without protecting peers.
                    // Do not overwrite a live peer under the host's bounded resource policy.
                    dos_status(unit, model, DOS_STATUS_NO_CHANNEL, 0, 0);
                    validate_finish(unit);
                    return;
                }
                unit->buffer_use |= bit;
                work->owned |= bit;
                unit->buffers[part].resident = false;
            }
        }
        // Keep directory bytes separate from chain reads so yielding during
        // MRKBAM/VMKBAM traversal does not lose the next entry's scan position.
        work->reader = allocate_channel(unit, reference, DOS_INTERNAL_READ_SECONDARY, 2);
        if (work->reader == DOS_UNUSED) {
            dos_status(unit, model, DOS_STATUS_NO_CHANNEL, 0, 0);
            validate_finish(unit);
            return;
        }
        unit->channels[work->reader].ready = false;
        unit->free_blocks[reference.slot] = 0;
        unit->free_blocks_valid[reference.slot] = true;
        work->phase = DOS_VALIDATE_MAP;
        return;
    }
    if (work->phase == DOS_VALIDATE_MAP) {
        // NEWMAP preserves the 4040 disk header, replacing its link and records.
        // DOS 2.7 NEWMPV/BAMOUT builds cleared pages with next-map/directory
        // links, version 'C', and a half-open track range in header bytes 4/5.
        uint8_t* const bytes = model == DOS_MODEL_4040 ? bam->buffer.bytes : unit->buffers[work->part].bytes;
        const unsigned int low = model == DOS_MODEL_4040 ? 1 : work->part * DOS_BAM_TRACKS_PER_PART + 1u;
        const unsigned int high =
            model == DOS_MODEL_4040 ? tracks + 1u : MIN(low + DOS_BAM_TRACKS_PER_PART, tracks + 1u);
        if (model == DOS_MODEL_4040) {
            bytes[0] = DOS_DIRECTORY_TRACK_4040;
            bytes[1] = DOS_DIRECTORY_FIRST_SECTOR;
            bam->dirty = true;
        } else {
            memset(bytes, 0, DISKIMAGE_SECTOR_SIZE);
            bytes[0] = work->part + 1u < work->parts ? DOS_BAM_TRACK_8050 : DOS_DIRECTORY_TRACK_8050;
            bytes[1] = work->part + 1u < work->parts ? (work->part + 1u) * DOS_BAM_PART_INTERLEAVE
                                                     : DOS_DIRECTORY_FIRST_SECTOR;
            bytes[2] = 'C';
            bytes[4] = low;
            bytes[5] = high;
        }
        // Start every legal sector free. Traversal clears ownership bits later.
        // Count only non-directory tracks, as DOS 2.7's NDBL/NDBH accounting does.
        for (unsigned int track = low; track < high; ++track) {
            unsigned int sectors;
            vet(diskimage_track_sectors(image->type, track, &sectors), "invalid validate track %u", track);
            uint8_t* const entry = validate_entry(unit, model, track);
            memset(entry, 0, model == DOS_MODEL_4040 ? 4u : 5u);
            entry[0] = sectors;
            for (unsigned int sector = 0; sector < sectors; ++sector) {
                entry[1u + sector / 8u] |= (uint8_t)(1u << (sector % 8u));
            }
            if (track != directory_track(model)) {
                unit->free_blocks[reference.slot] += sectors;
            }
        }
        if (++work->part == work->parts) {
            work->part = 0;
            work->track = directory_track(model);
            work->sector = DOS_DIRECTORY_FIRST_SECTOR;
            work->phase = DOS_VALIDATE_SEARCH;
        }
        return;
    }
    dos_channel_t* const reader = &unit->channels[work->reader];
    uint8_t* const bytes = unit->buffers[reader->buffers[work->phase == DOS_VALIDATE_CHAIN ? 1 : 0]].bytes;
    if (work->phase == DOS_VALIDATE_SEARCH) {
        if (work->track == 0) {
            // VD10 starts at directory-track sector 0, following header/BAM
            // links through the directory sectors, not just marking file data.
            work->chain_track = directory_track(model);
            work->chain_sector = 0;
            work->chain_visited = 0;
            work->directory_chain = true;
            work->phase = DOS_VALIDATE_CHAIN;
            return;
        }
        if (work->entry == 0) {
            // Bound corrupt linked directories by image capacity (host policy),
            // rather than allowing a cycle to monopolize the cooperative job.
            if (++work->visited > image->size / DISKIMAGE_SECTOR_SIZE) {
                dos_status(unit, model, DOS_STATUS_READ_ERROR, work->track, work->sector);
                validate_recover(unit, model, work->track, work->sector);
                return;
            }
            if (!storage_error(unit, media, reference,
                               read_sector(unit, media, reference, work->track, work->sector, bytes), false,
                               work->track, work->sector)) {
                validate_recover(unit, model, work->track, work->sector);
                return;
            }
        }
        if (work->entry == DISKIMAGE_SECTOR_SIZE / DISKIMAGE_DIRECTORY_ENTRY_SIZE) {
            work->track = bytes[0];
            work->sector = bytes[1];
            work->entry = 0;
            return;
        }
        // SRCHST/SRRE skip deleted entries. VD25 tests only the closed bit:
        // locked closed files survive, and their directory counts stay intact.
        uint8_t* const entry = bytes + work->entry++ * DISKIMAGE_DIRECTORY_ENTRY_SIZE;
        if (entry[DOS_DIRECTORY_TYPE_OFFSET] == 0) {
            return;
        }
        if (!(entry[DOS_DIRECTORY_TYPE_OFFSET] & DOS_FILE_CLOSED_MASK)) {
            // DELDIR removes a splat even when locked. Persist the type-byte
            // deletion now without traversing or modifying its abandoned data.
            entry[DOS_DIRECTORY_TYPE_OFFSET] = 0;
            if (!write_sector(unit, media, reference, work->track, work->sector, bytes)) {
                validate_recover(unit, model, work->track, work->sector);
            }
            return;
        }
        work->chain_track = entry[DOS_DIRECTORY_TRACK_OFFSET];
        work->chain_sector = entry[DOS_DIRECTORY_SECTOR_OFFSET];
        work->data_pending = entry[DOS_DIRECTORY_SIDE_TRACK] != 0;
        if (work->data_pending) {
            // VD15 saves the data start and marks a nonzero side chain first.
            // This also applies to side-link bytes in otherwise ordinary files.
            work->next_track = work->chain_track;
            work->next_sector = work->chain_sector;
            work->chain_track = entry[DOS_DIRECTORY_SIDE_TRACK];
            work->chain_sector = entry[DOS_DIRECTORY_SIDE_SECTOR];
        }
        work->chain_visited = 0;
        work->phase = DOS_VALIDATE_CHAIN;
        return;
    }
    if (work->phase == DOS_VALIDATE_CHAIN) {
        const uint8_t track = work->chain_track, sector = work->chain_sector;
        // Apply the same host cycle bound to data, side, and directory chains.
        if (++work->chain_visited > image->size / DISKIMAGE_SECTOR_SIZE) {
            dos_status(unit, model, DOS_STATUS_READ_ERROR, track, sector);
            validate_recover(unit, model, track, sector);
            return;
        }
        // MRKBAM/VMKBAM mark the current sector before reading its link.
        // validate_used preserves 4040 duplicate tolerance versus DOS 2.7 VUSED.
        if (!validate_used(unit, media, track, sector)) {
            // NO BLOCK and TSCHK's BADTS are direct command errors, not
            // controller failures that enter the INITDR recovery path.
            if (unit->status_code == DOS_STATUS_NO_BLOCK || unit->status_code == DOS_STATUS_ILLEGAL_TRACK_SECTOR) {
                validate_finish(unit);
            } else {
                validate_recover(unit, model, track, sector);
            }
            return;
        }
        if (!storage_error(unit, media, reference, read_sector(unit, media, reference, track, sector, bytes), false,
                           track, sector)) {
            validate_recover(unit, model, track, sector);
            return;
        }
        work->chain_track = bytes[0];
        work->chain_sector = bytes[1];
        if (work->chain_track == 0) {
            // A zero track terminates the chain. Resume saved data after a
            // side chain, otherwise scan more entries or publish the final maps.
            if (work->data_pending) {
                work->chain_track = work->next_track;
                work->chain_sector = work->next_sector;
                work->next_track = 0;
                work->data_pending = false;
                work->chain_visited = 0;
            } else {
                work->phase = work->directory_chain ? DOS_VALIDATE_WRITE : DOS_VALIDATE_SEARCH;
            }
        }
        return;
    }
    vet(work->phase == DOS_VALIDATE_WRITE, "invalid validate phase %u", work->phase);
    // MO10/WTMAPS publish only after traversal succeeds. Write one part per
    // step, without making this sequence atomic or refreshing DOS 2.7's cache.
    if (work->part < work->parts) {
        const uint8_t track = model == DOS_MODEL_4040 ? directory_track(model) : DOS_BAM_TRACK_8050;
        const uint8_t sector = work->part * DOS_BAM_PART_INTERLEAVE;
        const uint8_t* const map = model == DOS_MODEL_4040 ? bam->buffer.bytes : unit->buffers[work->part].bytes;
        if (!write_sector(unit, media, reference, track, sector, map)) {
            validate_recover(unit, model, track, sector);
            return;
        }
        ++work->part;
        if (model == DOS_MODEL_4040) {
            bam->dirty = false;
        }
        return;
    }
    // Host flush is a separate failure boundary after all map writes. Recovery
    // reloads persisted metadata, not an undo image of deletions or earlier parts.
    if (!storage_error(unit, media, reference, dos_media_flush_unit(media, reference.unit), true, 0, 0)) {
        validate_recover(unit, model, 0, 0);
        return;
    }
    // Release cooperative resources before VALDAT's successful ENDCMD status.
    validate_finish(unit);
    command_end(unit, model);
}

// Release COPY's internal channels and retire its job without changing status.
// Finalize successful output before calling: this cleanup never closes a writer.
static void copy_finish(dos_unit_t* const unit) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4
    //
    // FREICH frees IRSA and IWSA through FRECHN, not CLSCHN. Mirror that
    // resource release so error cleanup does not mark partial output closed.
    // Pending/command fields are cooperative scheduling state, not ROM fields.
    dos_copy_work_t* const work = &unit->copy;

    // Release only allocated channels, including partially constructed jobs.
    if (work->reader != DOS_UNUSED && unit->channels[work->reader].allocated) {
        free_channel(unit, work->reader);
    }
    if (work->writer != DOS_UNUSED && unit->channels[work->writer].allocated) {
        free_channel(unit, work->writer);
    }
    // Preserve the completion/error record already selected by the caller.
    work->pending = false;
    unit->command = DOS_COMMAND_IDLE;
}

// Reuse an internal reader for an ordinary COPY source.
// Return false with status on failure, leaving channel cleanup to the caller.
static bool copy_read_start(dos_unit_t* const unit, dos_media_bank_t* const media, const uint8_t index,
                            const dos_copy_file_t* const file) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rename
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rename
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/opchnl
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/opchnl
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/open
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/open
    //
    // OPIRFL opens the source directory through OPNIRD, then OPREAD reuses
    // that channel for file data. For ordinary files it resets SETPNT to 2,
    // so GIBYTE starts at payload rather than IEEE's prepared-byte cursor.
    dos_copy_work_t* const work = &unit->copy;
    dos_channel_t* const channel = &unit->channels[index];
    const dos_model_t model = media->units[work->media[0].unit].model;
    const dos_media_ref_t reference = work->media[file->drive & 1u];
    unit->selected_drive = reference.slot;
    // A peer CLOSE may have reclaimed this entry after lookup yielded.
    if (directory_entry_busy(unit, reference, file->directory_track, file->directory_sector, file->directory_entry,
                             NULL)) {
        dos_status(unit, model, DOS_STATUS_WRITE_ERROR, file->directory_track, file->directory_sector);
        return false;
    }
    // OP130 masks the directory track only on 4040, after CHKIN's raw test.
    const uint8_t track = file->track & (model == DOS_MODEL_4040 ? DOS_FILE_TRACK_MASK_4040 : UINT8_MAX);
    if (channel->buffers[2] != DOS_UNUSED) {
        unit->buffer_use &= (uint16_t)~(1u << channel->buffers[2]);
        channel->buffers[2] = DOS_UNUSED;
    }

    // Bind the matched disk/entry and restart traversal without reallocating
    // the two pages, preserving the ROM's allocation-visible page contents.
    channel->media = reference;
    channel->active = 0;
    channel->traversed = 1;
    channel->blocks = 0;
    channel->file_type = file->type & DOS_FILE_TYPE_MASK;
    channel->first_track = track;
    channel->first_sector = file->sector;
    channel->directory_track = file->directory_track;
    channel->directory_sector = file->directory_sector;
    channel->directory_entry = file->directory_entry;
    memcpy(channel->pattern, file->name, sizeof(channel->pattern));
    // The same raw coordinates on another disk must not hit the old buffer.
    unit->buffers[channel->buffers[0]].resident = false;
    unit->buffers[channel->buffers[1]].resident = false;
    unit->selected_drive = channel->media.slot;
    // OPIRFL first opens the directory on IRSA. STRDBL can prefetch its
    // successor before OPREAD reuses those same two buffers for file data.
    // Unused bytes survive this reuse and later COPY allocations expose them.
    dos_buffer_t* const initial = &unit->buffers[channel->buffers[0]];
    dos_buffer_t* const alternate = &unit->buffers[channel->buffers[1]];
    if (!storage_error(
            unit, media, channel->media,
            read_sector(unit, media, channel->media, file->directory_track, file->directory_sector, initial->bytes),
            false, file->directory_track, file->directory_sector)) {
        return false;
    }
    // A successful peer replacement may already have released its retry owner
    // and reassigned the captured chain. Reject the stale binding, not its new data.
    const uint8_t* const entry = initial->bytes + file->directory_entry;
    if (entry[DOS_DIRECTORY_TRACK_OFFSET] != file->track || entry[DOS_DIRECTORY_SECTOR_OFFSET] != file->sector ||
        (entry[DOS_DIRECTORY_TYPE_OFFSET] & DOS_FILE_TYPE_MASK) != (file->type & DOS_FILE_TYPE_MASK) ||
        !name_matches(file->name, file->name_length, entry + DOS_DIRECTORY_NAME_OFFSET)) {
        dos_status(unit, model, DOS_STATUS_WRITE_ERROR, file->directory_track, file->directory_sector);
        return false;
    }
    if (initial->bytes[0] != 0) {
        // OPREAD's GETRCH/FRECHN and GETBUF discard this unused job result.
        // Keep its partial buffer contents, but do not observe its error.
        (void)read_sector(unit, media, channel->media, initial->bytes[0], initial->bytes[1], alternate->bytes);
    }
    // OPNRCH/STRDBL replace the first page with file data, then prefetch its
    // successor when linked. A terminal first block leaves alternate residue.
    if (!load_buffer(unit, media, channel, track, file->sector)) {
        dos_status(unit, model, DOS_STATUS_READ_ERROR, track, file->sector);
        return false;
    }
    if (initial->bytes[0] != 0) {
        alternate->track = initial->bytes[0];
        alternate->sector = initial->bytes[1];
        const dos_media_result_t result =
            read_sector(unit, media, channel->media, alternate->track, alternate->sector, alternate->bytes);
        alternate->failed = result != DOS_MEDIA_OK;
        // STRDBL submits without waiting. Preserve even a failed job until
        // RDBYT/DBLBUF selects it, rather than aborting a speculative read.
        alternate->resident = true;
    }
    return true;
}

// Resolve COPY lookup results into an append or new-output job.
// Publish failures and release internal resources before returning.
static void copy_lookup_finish(dos_unit_t* const unit, dos_media_bank_t* const media) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rename
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rename
    //
    // CHKIN checks all source entries before CHKIO rejects existing output.
    // COPY/COP05 instead rebinds the first source as IWSA for self-concat,
    // while CY creates a new file whose type comes from the first source.
    dos_copy_work_t* const work = &unit->copy;
    const dos_copy_file_t* const target = &work->files[0];
    const dos_copy_file_t* const first = &work->files[1];
    const dos_model_t model = media->units[work->media[0].unit].model;
    // DOS 2.7 CMPCHK keeps PATTYP's pattern flag after FIXIT replaces the
    // target name. A matching existing target makes LK25/COMPAR repeat
    // the same entry forever. 4040 tests FILTRK instead and reaches 63.
    // Retain the pending command cooperatively rather than spinning the MCU.
    if (model != DOS_MODEL_4040 && work->all_files &&
        work->files[DOS_COPY_PATTERN_INDEX].type == DOS_COPY_PATTERN_FLAG && (work->found & 1u)) {
        work->phase = DOS_COPY_MATCH_WAIT;
        return;
    }
    // Preserve missing-source precedence even when the target already exists.
    for (unsigned int index = work->count - 1u; index != 0; --index) {
        if (!(work->found & (1u << index)) || work->files[index].track == 0) {
            dos_status(unit, model, DOS_STATUS_NOT_FOUND, 0, 0);
            copy_finish(unit);
            return;
        }
    }
    // Match OPEN's retained-entry exclusion before creating any output or
    // converting a source to a self-append writer.
    for (unsigned int index = 0; index < work->count; ++index) {
        const dos_copy_file_t* const file = &work->files[index];
        if ((work->found & (1u << index)) &&
            directory_entry_busy(unit, work->media[file->drive & 1u], file->directory_track, file->directory_sector,
                                 file->directory_entry, NULL)) {
            unit->selected_drive = file->drive & 1u;
            dos_status(unit, model, DOS_STATUS_WRITE_ERROR, file->directory_track, file->directory_sector);
            copy_finish(unit);
            return;
        }
    }
    for (unsigned int index = 1; index < work->count; ++index) {
        if ((work->files[index].type & DOS_FILE_TYPE_MASK) == DISKIMAGE_FTYPE_REL) {
            // REL transfer belongs to a later dependency slice.
            dos_status(unit, model, DOS_STATUS_SYNTAX, 0, 0);
            copy_finish(unit);
            return;
        }
    }
    const dos_media_ref_t reference = work->media[target->drive];
    unit->selected_drive = target->drive;
    uint8_t name[DISKIMAGE_FILENAME_SIZE];
    memcpy(name, target->name, sizeof(name));
    if (work->empty_target) {
        name[0] = '=';
    }
    if (work->name_conflict || unpublished_writer(unit, reference, name, DOS_UNUSED) != NULL) {
        // Preserve the same host namespace reservation enforced by OPEN/RENAME.
        dos_status(unit, model, DOS_STATUS_WRITE_ERROR, 0, 0);
        copy_finish(unit);
        return;
    }
    work->source = 1;
    // Self-concat requires the same resolved entry and additional sources,
    // not merely equal filename bytes. COPY rejects PRG/REL append types.
    if ((work->found & 1u) && work->count >= 3u && target->drive == first->drive &&
        target->directory_track == first->directory_track && target->directory_sector == first->directory_sector &&
        target->directory_entry == first->directory_entry) {
        const uint8_t type = first->type & DOS_FILE_TYPE_MASK;
        if (type != DISKIMAGE_FTYPE_SEQ && type != DISKIMAGE_FTYPE_USR) {
            dos_status(unit, model, DOS_STATUS_TYPE_MISMATCH, 0, 0);
            copy_finish(unit);
            return;
        }
        // Native COPY moves IRSA's existing channel to IWSA before APPEND.
        work->writer = work->reader;
        work->reader = DOS_UNUSED;
        dos_channel_t* const writer = &unit->channels[work->writer];
        writer->secondary = DOS_INTERNAL_WRITE_SECONDARY;
        writer->access = DOS_ACCESS_APPEND;
        if (!copy_read_start(unit, media, work->writer, first)) {
            copy_finish(unit);
            return;
        }
        // STRDBL/RDBUF count the initial read too. COPY keeps that count
        // when converting the source channel to a writer, then CLSWRT adds
        // its write count. Native self-concat can overstate the chain length.
        writer->blocks = 1;
        work->source = 2;
        work->phase = DOS_COPY_APPEND;
        return;
    }
    // Without the self-concat exception, an existing target is FILE EXISTS.
    if (work->found & 1u) {
        dos_status(unit, model, DOS_STATUS_FILE_EXISTS, 0, 0);
        copy_finish(unit);
        return;
    }
    // CY opens IWSA and ADDFIL publishes an unclosed entry before data copying.
    work->writer = allocate_channel(unit, reference, DOS_INTERNAL_WRITE_SECONDARY, 2);
    if (work->writer == DOS_UNUSED) {
        dos_status(unit, model, DOS_STATUS_NO_CHANNEL, 0, 0);
        copy_finish(unit);
        return;
    }
    dos_open_work_t creation = {
        .type = first->type & DOS_FILE_TYPE_MASK,
        .empty_track = work->empty_track,
        .empty_sector = work->empty_sector,
        .empty_entry = work->empty_entry,
        .last_sector = work->last_sector,
        .last_track = directory_track(model),
    };
    memcpy(creation.name, target->name, sizeof(creation.name));
    if (work->empty_target) {
        // TRCMBF reads '=' once even for an empty target. ADDFIL then clears
        // the reserved fields, unlike RENAME's extra padding-byte spill.
        creation.name[0] = '=';
    }
    if (!open_write(unit, media, &unit->channels[work->writer], &creation, false)) {
        copy_finish(unit);
        return;
    }
    work->phase = DOS_COPY_SOURCE;
}

// Search one directory sector for COPY's unresolved source and target names.
// Retain search/allocation hints across yields and finish lookup at exhaustion.
static void copy_search(dos_unit_t* const unit, dos_media_bank_t* const media) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lookup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lookup
    //
    // LOOKUP/SRCHST/SEARCH collect entry descriptors and COMPAR applies
    // OPTSCH's drive/default ordering. C retains that search in work instead
    // of blocking across the directory chain. COPY's names are literal here.
    dos_copy_work_t* const work = &unit->copy;
    const dos_media_ref_t reference = work->media[work->drive];
    const dos_model_t model = media->units[reference.unit].model;
    uint8_t* const bytes = unit->buffers[unit->channels[work->reader].buffers[0]].bytes;
    unit->selected_drive = work->drive;
    auto_initialize(unit, work->drive);
    // Bound chain visits by image size as a host corruption safeguard.
    const unsigned int limit = media->units[reference.unit].slots[reference.slot].io.image.size / DISKIMAGE_SECTOR_SIZE;
    if (++work->visited > limit) {
        dos_status(unit, model, DOS_STATUS_DIRECTORY_ERROR, work->track, work->sector);
        copy_finish(unit);
        return;
    }
    if (directory_growth_pending(unit, reference, work->track, work->sector)) {
        dos_status(unit, model, DOS_STATUS_WRITE_ERROR, work->track, work->sector);
        copy_finish(unit);
        return;
    }
    if (!storage_error(unit, media, reference, read_sector(unit, media, reference, work->track, work->sector, bytes),
                       false, work->track, work->sector)) {
        copy_finish(unit);
        return;
    }
    if (work->drive == work->files[0].drive) {
        work->last_sector = work->sector;
    }
    for (unsigned int offset = 0; offset < DISKIMAGE_SECTOR_SIZE; offset += DOS_DIRECTORY_LINE_SIZE) {
        const uint8_t* const entry = bytes + offset;
        if (entry[DOS_DIRECTORY_TYPE_OFFSET] == 0) {
            // Remember a deleted slot only on the destination disk for ADDFIL.
            if (work->drive == work->files[0].drive && work->empty_track == 0 &&
                !directory_slot_reserved(unit, reference, work->sector, offset)) {
                work->empty_track = work->track;
                work->empty_sector = work->sector;
                work->empty_entry = offset;
            }
            continue;
        }
        for (unsigned int index = work->count; index-- != 0;) {
            dos_copy_file_t* const file = &work->files[index];
            if (work->found & (1u << index)) {
                continue;
            }
            // An omitted drive becomes eligible on the alternate pass, while
            // an explicit drive remains restricted to its selected disk.
            if ((file->drive & 1u) != work->drive && (!(file->drive & DOS_COPY_DEFAULT_DRIVE) || work->scans == 2u)) {
                continue;
            }
            if (!name_matches(file->name, file->name_length, entry + DOS_DIRECTORY_NAME_OFFSET)) {
                continue;
            }
            // Save exact entry coordinates for source reads and self-concat.
            file->drive = (file->drive & DOS_COPY_DEFAULT_DRIVE) | work->drive;
            file->type = entry[DOS_DIRECTORY_TYPE_OFFSET];
            file->track = entry[DOS_DIRECTORY_TRACK_OFFSET];
            file->sector = entry[DOS_DIRECTORY_SECTOR_OFFSET];
            file->directory_track = work->track;
            file->directory_sector = work->sector;
            file->directory_entry = offset;
            // Both CMPCHK/CC15 routines leave zero FILTRK unresolved.
            // 4040 also retains negative FILTRK. LK26 repeats this match.
            if (file->track == 0 || (model == DOS_MODEL_4040 && (file->track & DOS_FILE_TRACK_PENDING_MASK_4040))) {
                work->phase = DOS_COPY_MATCH_WAIT;
                return;
            }
            work->found |= 1u << index;
        }
        if (work->found == (1u << work->count) - 1u) {
            copy_lookup_finish(unit, media);
            return;
        }
    }
    // Follow this directory before toggling drives. Yield after each sector.
    if (bytes[0] != 0) {
        work->track = bytes[0];
        work->sector = bytes[1];
        return;
    }
    if (work->scans != 0 && work->media[work->drive ^ 1u].identity != 0) {
        --work->scans;
        work->drive ^= 1u;
        work->track = directory_track(model);
        work->sector = DOS_DIRECTORY_FIRST_SECTOR;
        work->visited = 0;
        return;
    }
    copy_lookup_finish(unit, media);
}

// Select the next copy-all source and schedule its destination lookup.
// Retain the source directory iterator across each per-file COPY operation.
static void copy_all_next(dos_unit_t* const unit, dos_media_bank_t* const media) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/copyall
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/copyall
    //
    // MOVLP2/EXLP1 enumerate through FFST/FFRE. FIXIT saves search state,
    // TRFNME supplies the target name, and CY copies one entry before EXLP0
    // restores iteration. C uses saved coordinates rather than the ROM stack.
    dos_copy_work_t* const work = &unit->copy;
    const dos_media_ref_t reference = work->media[work->files[1].drive];
    const dos_model_t model = media->units[reference.unit].model;
    unit->selected_drive = reference.slot;
    auto_initialize(unit, reference.slot);
    // Limit sector visits as a host corruption safeguard, not a ROM timeout.
    const unsigned int limit = media->units[reference.unit].slots[reference.slot].io.image.size / DISKIMAGE_SECTOR_SIZE;
    if (work->all_entry == 0 && ++work->all_visited > limit) {
        dos_status(unit, model, DOS_STATUS_DIRECTORY_ERROR, work->all_track, work->all_sector);
        copy_finish(unit);
        return;
    }
    if (directory_growth_pending(unit, reference, work->all_track, work->all_sector)) {
        dos_status(unit, model, DOS_STATUS_WRITE_ERROR, work->all_track, work->all_sector);
        copy_finish(unit);
        return;
    }
    dos_channel_t* const reader = &unit->channels[work->reader];
    if (reader->buffers[2] != DOS_UNUSED) {
        unit->buffer_use &= (uint16_t)~(1u << reader->buffers[2]);
        reader->buffers[2] = DOS_UNUSED;
    }
    reader->file_type = DISKIMAGE_FTYPE_SEQ;
    dos_buffer_t* const buffer = &unit->buffers[reader->buffers[0]];
    dos_buffer_t* const alternate = &unit->buffers[reader->buffers[1]];
    reader->media = reference;
    reader->active = 0;
    // Per-file lookup/data reads reuse these pages, so reload the iterator's
    // saved sector rather than trusting its previous residency.
    buffer->resident = false;
    if (!load_buffer(unit, media, reader, work->all_track, work->all_sector)) {
        dos_status(unit, model, DOS_STATUS_READ_ERROR, work->all_track, work->all_sector);
        copy_finish(unit);
        return;
    }
    // OPNIRD/STRDBL prefetch without selecting the successor. A matching
    // entry's CY discards this job, and SRRE reopens it after that file.
    dos_media_result_t successor_result = DOS_MEDIA_OK;
    if (buffer->bytes[0] != 0) {
        successor_result = read_sector(unit, media, reference, buffer->bytes[0], buffer->bytes[1], alternate->bytes);
    }
    // Save the next slot before handing this entry to the per-file state machine.
    while (work->all_entry < DISKIMAGE_SECTOR_SIZE / DOS_DIRECTORY_LINE_SIZE) {
        const unsigned int offset = work->all_entry++ * DOS_DIRECTORY_LINE_SIZE;
        const uint8_t* const entry = buffer->bytes + offset;
        if (entry[DOS_DIRECTORY_TYPE_OFFSET] == 0 ||
            !name_matches(work->files[DOS_COPY_PATTERN_INDEX].name, work->files[DOS_COPY_PATTERN_INDEX].name_length,
                          entry + DOS_DIRECTORY_NAME_OFFSET)) {
            continue;
        }
        dos_copy_file_t* const source = &work->files[1];
        source->type = entry[DOS_DIRECTORY_TYPE_OFFSET];
        source->track = entry[DOS_DIRECTORY_TRACK_OFFSET];
        source->sector = entry[DOS_DIRECTORY_SECTOR_OFFSET];
        source->directory_track = work->all_track;
        source->directory_sector = work->all_sector;
        source->directory_entry = offset;
        memcpy(source->name, entry + DOS_DIRECTORY_NAME_OFFSET, sizeof(source->name));
        memcpy(work->files[0].name, source->name, sizeof(source->name));
        // TRFNME supplies the complete directory field, not a parsed short name.
        source->name_length = work->files[0].name_length = sizeof(source->name);
        work->name_conflict =
            unpublished_writer(unit, work->media[work->files[0].drive], source->name, DOS_UNUSED) != NULL;
        // FIXIT's LOOKUP searches only the destination. Preserve the source
        // descriptor obtained by FFST/FFRE and reset per-file creation state.
        work->files[0].track = 0;
        work->found = 1u << 1;
        work->source = 1;
        work->drive = work->files[0].drive;
        work->scans = 0;
        work->visited = 0;
        work->track = directory_track(model);
        work->sector = DOS_DIRECTORY_FIRST_SECTOR;
        work->empty_track = 0;
        work->empty_sector = 0;
        work->empty_entry = 0;
        work->last_sector = 0;
        work->phase = DOS_COPY_SEARCH;
        return;
    }
    if (buffer->bytes[0] != 0) {
        // Only SEARCH/NXTBUF observes the current job when this page is exhausted.
        if (!storage_error(unit, media, reference, successor_result, false, buffer->bytes[0], buffer->bytes[1])) {
            copy_finish(unit);
            return;
        }
        work->all_track = buffer->bytes[0];
        work->all_sector = buffer->bytes[1];
        work->all_entry = 0;
        return;
    }
    // ENDIT reports success only after enumeration reaches the terminal link.
    command_end(unit, model);
    copy_finish(unit);
}

// Advance ordered COPY, preserving native GIBYTE-before-CHNDAT semantics.
static void copy_work(dos_unit_t* const unit, dos_media_bank_t* const media) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rename
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rename
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4
    //
    // COPY/CY10/CY15/CY20 stream each source through GIBYTE/PIBYTE, then
    // CLSCHN closes IWSA. C separates these operations into cooperative phases.
    // Internal GET calls tst4:GBYTE before sampling CHNDAT. It must not use
    // externally staged reads, whose singleton EOF behavior is different.
    dos_copy_work_t* const work = &unit->copy;
    const dos_model_t model = media->units[work->media[0].unit].model;
    if (work->invalidated_file != 0) {
        const dos_copy_file_t* const file = &work->files[work->invalidated_file - 1u];
        unit->selected_drive = work->media[file->drive & 1u].slot;
        dos_status(unit, model, DOS_STATUS_WRITE_ERROR, file->directory_track, file->directory_sector);
        copy_finish(unit);
        return;
    }
    if (work->phase == DOS_COPY_MATCH_WAIT) {
        return;
    }
    if (work->phase == DOS_COPY_ALL_NEXT) {
        copy_all_next(unit, media);
        return;
    }
    if (work->phase == DOS_COPY_SEARCH) {
        copy_search(unit, media);
        return;
    }
    if (work->phase == DOS_COPY_APPEND) {
        // Continue finding the self-concat tail without publishing an OPEN
        // completion status. Only a successful append proceeds to new sources.
        unit->selected_drive = unit->channels[work->writer].media.slot;
        dos_open_work_t append = {.append_channel = work->writer + 1u, .pending = true};
        append_work(unit, media, &append, false);
        if (append.pending) {
            return;
        }
        if (!unit->channels[work->writer].allocated || unit->channels[work->writer].failed) {
            copy_finish(unit);
            return;
        }
        work->phase = DOS_COPY_SOURCE;
        return;
    }
    if (work->phase == DOS_COPY_SOURCE) {
        if (work->source == work->count) {
            work->phase = DOS_COPY_CLOSE;
            return;
        }
        const dos_copy_file_t* const file = &work->files[work->source];
        // Self-concat moved the original reader to IWSA. Allocate a replacement
        // IRSA only when needed, then reuse it for later sources.
        if (work->reader == DOS_UNUSED) {
            work->reader = allocate_channel(unit, work->media[file->drive & 1u], DOS_INTERNAL_READ_SECONDARY, 2);
            if (work->reader == DOS_UNUSED) {
                dos_status(unit, model, DOS_STATUS_NO_CHANNEL, 0, 0);
                copy_finish(unit);
                return;
            }
            unit->channels[work->reader].ready = false;
        }
        if (!copy_read_start(unit, media, work->reader, file)) {
            copy_finish(unit);
            return;
        }
        work->phase = DOS_COPY_BYTES;
        return;
    }
    if (work->phase == DOS_COPY_CLOSE) {
        // Persist data, directory metadata, and BAM before releasing output.
        // A failure abandons internal channels without a second CLOSE attempt.
        dos_channel_t* const writer = &unit->channels[work->writer];
        if (writer->allocated && !finish_channel(unit, media, writer)) {
            copy_finish(unit);
            return;
        }
        // Copy-all retains its reader/iterator but releases each finished writer.
        if (work->all_files) {
            free_channel(unit, work->writer);
            work->writer = DOS_UNUSED;
            work->phase = DOS_COPY_ALL_NEXT;
            return;
        }
        command_end(unit, model);
        copy_finish(unit);
        return;
    }
    dos_channel_t* const reader = &unit->channels[work->reader];
    dos_buffer_t* const buffer = &unit->buffers[reader->buffers[reader->active]];
    unit->selected_drive = reader->media.slot;
    // A final link's second byte is the last occupied offset. Include that
    // payload byte, but never copy the two-byte sector link as file data.
    const unsigned int end = buffer->bytes[0] == 0 ? buffer->bytes[1] + 1u : DISKIMAGE_SECTOR_SIZE;
    if (end < DISKIMAGE_SECTOR_LINK_SIZE) {
        dos_status(unit, model, DOS_STATUS_DIRECTORY_ERROR, buffer->track, buffer->sector);
        copy_finish(unit);
        return;
    }
    // Reuse the burst bound for scheduler fairness, not IEEE preview/GET.
    for (unsigned int copied = 0; copied < DOS_STAGE_CAPACITY && reader->position < end; ++copied) {
        unit->selected_drive = reader->media.slot;
        const uint8_t byte = buffer->bytes[reader->position++];
        const bool switching = reader->position == DISKIMAGE_SECTOR_SIZE && buffer->bytes[0] != 0;
        if (switching) {
            // CY20's GIBYTE waits for DBLBUF before CY15's PIBYTE can persist
            // this byte. Do not write an extra output block on read failure.
            const unsigned int limit =
                media->units[reader->media.unit].slots[reader->media.slot].io.image.size / DISKIMAGE_SECTOR_SIZE;
            if (reader->traversed++ >= limit) {
                dos_status(unit, model, DOS_STATUS_DIRECTORY_ERROR, buffer->track, buffer->sector);
                copy_finish(unit);
                return;
            }
            const uint8_t track = buffer->bytes[0], sector = buffer->bytes[1];
            reader->active ^= 1u;
            if (!load_buffer(unit, media, reader, track, sector)) {
                dos_status(unit, model, DOS_STATUS_READ_ERROR, track, sector);
                copy_finish(unit);
                return;
            }
        }
        unit->selected_drive = unit->channels[work->writer].media.slot;
        if (!write_byte(unit, media, &unit->channels[work->writer], byte)) {
            copy_finish(unit);
            return;
        }
        if (switching) {
            return;
        }
    }
    if (reader->position != end) {
        return;
    }
    // Concatenate the next source immediately after this terminal payload.
    if (buffer->bytes[0] == 0) {
        ++work->source;
        work->phase = DOS_COPY_SOURCE;
        return;
    }
}

// Schedule the shared INITDR prefix without giving it ownership of ENDCMD.
static void format_initialize(dos_unit_t* const unit, const dos_media_ref_t reference) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
    //
    // No direct ROM entry: retain NEW/DUPLICATE's INITDR prefix as a shared
    // cooperative job. Unlike INTDRV, this prefix must not finish the command.
    unit->initialize = (dos_initialize_work_t){
        .first_drive = reference.slot,
        .slots = 1u << reference.slot,
        .pending = true,
    };
    unit->initialize.media[reference.slot] = reference;
}

// Parse NEW's TAGCMD/ONEDRV names or DUPLICATE's adjacent PRSEQ selectors.
static void format_start(dos_unit_t* const unit, dos_media_bank_t* const media, const unsigned int index,
                         const unsigned int length, const bool duplicate) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/new
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/new
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/copysetup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/copysetup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/trnsfr
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/trnsfr
    //
    // NEW uses ONEDRV and FILTBL+1 to distinguish quick/full mode. DUPLCT
    // uses PRSEQ's adjacent drive selectors. Capture names, IDs and media
    // before CLRCB retires command storage, deferring all disk work.
    const dos_model_t model = media->units[index].model;
    const uint8_t* const bytes = unit->command_bytes;
    dos_format_work_t work = {.duplicate = duplicate, .phase = DOS_FORMAT_INITIALIZE, .reader = DOS_UNUSED};
    uint8_t drive = unit->last_drive;
    unsigned int commas;
    if (duplicate) {
        const unsigned int after = command_delimiter(bytes, length, '=', &commas);
        if (after < 2u || (bytes[after] != '0' && bytes[after] != '1') ||
            (bytes[after - 2u] != '0' && bytes[after - 2u] != '1') || bytes[after] == bytes[after - 2u]) {
            dos_status(unit, model, DOS_STATUS_SYNTAX, 0, 0);
            return;
        }
        drive = bytes[after - 2u] - '0';
        work.source = (dos_media_ref_t){.unit = index, .slot = drive ^ 1u};
        dos_media_reference(media, index, work.source.slot, &work.source);
        work.full = true;
    } else {
        const unsigned int after = command_delimiter(bytes, length, ':', &commas);
        if (after == 0 || commas != 0) {
            dos_status(unit, model, DOS_STATUS_NO_FILE, 0, 0);
            return;
        }
        unsigned int separators = 0;
        for (unsigned int position = 0; position < length; ++position) {
            if (bytes[position] == '*' || bytes[position] == '?' ||
                (position >= after - 2u && bytes[position] == '=')) {
                dos_status(unit, model, DOS_STATUS_SYNTAX, 0, 0);
                return;
            }
            if (bytes[position] == ',' && ++separators == DOS_FILENAME_COUNT - 1u) {
                break;
            }
        }
        const uint8_t selector = bytes[after - 2u];
        if (selector == '0' || selector == '1') {
            drive = selector - '0';
        }
        work.empty_name = command_name(bytes, length, after, work.name) == 0;
        if (work.empty_name) {
            // TRCMBF copies the delimiter once even when FNDLMT finds no name.
            work.name[0] = bytes[after];
        }
        unsigned int comma = after;
        while (comma < length && bytes[comma] != ',') {
            ++comma;
        }
        work.full = comma + 1u < length;
        if (work.full) {
            // TRNAME truncates the name, but FILTBL+1 still addresses the
            // original ID. Its second byte can be retained command storage.
            work.id[0] = bytes[comma + 1u];
            work.id[1] = bytes[comma + 2u];
            work.phase = DOS_FORMAT_START;
        }
    }
    work.media = (dos_media_ref_t){.unit = index, .slot = drive};
    dos_media_reference(media, index, drive, &work.media);
    if (duplicate && work.media.identity != 0 && work.source.identity != 0) {
        const diskimage_t* const target = &media->units[index].slots[drive].io.image;
        const diskimage_t* const source = &media->units[index].slots[work.source.slot].io.image;
        // Distinct slot generations do not make a shared backing image safe to erase.
        if (target->read == source->read && target->ctx == source->ctx) {
            unit->selected_drive = drive;
            dos_status(unit, model, DOS_STATUS_WRITE_ERROR, 0, 0);
            return;
        }
    }
    unit->format = work;
    unit->command = DOS_COMMAND_FORMAT;
    unit->selected_drive = duplicate ? work.source.slot : drive;
    dos_status(unit, model, DOS_STATUS_OK, 0, 0);
    if (work.phase == DOS_FORMAT_INITIALIZE) {
        format_initialize(unit, duplicate ? work.source : work.media);
    }
}

// Build one native NEWMAP page, optionally reserving the directory/BAM chain.
static void format_map(dos_unit_t* const unit, const dos_media_bank_t* const media, const bool reserve) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/verdir
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/new
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/map
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/new
    //
    // NEWMAP builds free-sector bits, then NEW reserves directory/BAM links.
    // Construct one host page at a time: four-byte 4040 entries or five-byte
    // DOS 2.7 entries with an exclusive upper track bound. No I/O occurs.
    const dos_format_work_t* const work = &unit->format;
    const dos_model_t model = media->units[work->media.unit].model;
    const diskimage_t* const image = &media->units[work->media.unit].slots[work->media.slot].io.image;
    const unsigned int tracks = model == DOS_MODEL_4040   ? DISKIMAGE_D64_TRACK_COUNT
                                : model == DOS_MODEL_8050 ? DISKIMAGE_D80_TRACK_COUNT
                                                          : DISKIMAGE_D82_TRACK_COUNT;
    const unsigned int parts = (tracks + DOS_BAM_TRACKS_PER_PART - 1u) / DOS_BAM_TRACKS_PER_PART;
    uint8_t* const bytes = unit->bam[work->media.slot].buffer.bytes;
    memset(bytes, 0, DISKIMAGE_SECTOR_SIZE);
    const unsigned int low = model == DOS_MODEL_4040 ? 1u : work->part * DOS_BAM_TRACKS_PER_PART + 1u;
    const unsigned int high = model == DOS_MODEL_4040 ? tracks + 1u : MIN(low + DOS_BAM_TRACKS_PER_PART, tracks + 1u);
    // Chain DOS 2.7's 50-track map parts before the common directory sector.
    bytes[0] = model == DOS_MODEL_4040   ? DOS_DIRECTORY_TRACK_4040
               : work->part + 1u < parts ? DOS_BAM_TRACK_8050
                                         : DOS_DIRECTORY_TRACK_8050;
    bytes[1] = model == DOS_MODEL_4040 || work->part + 1u == parts ? DOS_DIRECTORY_FIRST_SECTOR
                                                                   : (work->part + 1u) * DOS_BAM_PART_INTERLEAVE;
    bytes[DOS_BAM_VERSION_OFFSET] = model == DOS_MODEL_4040 ? DOS_VERSION_4040 : DOS_VERSION_8050;
    if (model != DOS_MODEL_4040) {
        bytes[4] = low;
        bytes[5] = high;
    }
    // Count only real geometry bits as free. Reservation clears the same
    // directory/header and BAM allocations that NEW marks through USEDTS.
    for (unsigned int track = low; track < high; ++track) {
        unsigned int sectors;
        vet(diskimage_track_sectors(image->type, track, &sectors), "invalid format track %u", track);
        uint8_t* const entry = bytes + (model == DOS_MODEL_4040 ? track * 4u : 6u + (track - low) * 5u);
        for (unsigned int sector = 0; sector < sectors; ++sector) {
            const bool used =
                reserve && ((track == directory_track(model) && sector <= DOS_DIRECTORY_FIRST_SECTOR) ||
                            (model != DOS_MODEL_4040 && track == DOS_BAM_TRACK_8050 &&
                             sector % DOS_BAM_PART_INTERLEAVE == 0 && sector / DOS_BAM_PART_INTERLEAVE < parts));
            if (!used) {
                ++entry[0];
                entry[1u + sector / 8u] |= (uint8_t)(1u << (sector % 8u));
            }
        }
    }
}

// Transfer NEW's padded label, ID and DOS version to its native header offsets.
static void format_label(dos_unit_t* const unit, const dos_model_t model) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/new
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/new
    //
    // NEW and DOS 2.7 TRNHDR install TRNAME's padded name, two-byte disk ID
    // and version 2A/2C. Header offsets differ from the BAM entry layout.
    const dos_format_work_t* const work = &unit->format;
    uint8_t* const bytes = unit->bam[work->media.slot].buffer.bytes;
    const unsigned int name = model == DOS_MODEL_4040 ? DOS_HEADER_NAME_4040 : DOS_HEADER_NAME_8050;
    // Empty TRNAME copies one delimiter plus all 27 padding bytes.
    memset(bytes + name, DISKIMAGE_FILENAME_PADDING, DOS_DIRECTORY_NAME_SIZE + work->empty_name);
    memcpy(bytes + name, work->name, DISKIMAGE_FILENAME_SIZE);
    memcpy(bytes + name + DOS_DISK_ID_OFFSET, work->id, sizeof(work->id));
    bytes[name + DOS_DISK_VERSION_OFFSET] = '2';
    bytes[name + DOS_DISK_VERSION_OFFSET + 1u] = model == DOS_MODEL_4040 ? DOS_VERSION_4040 : DOS_VERSION_8050;
}

// Begin the next native eleven-buffer DUPLICATE group on the current track.
static void format_copy_group(dos_format_work_t* const work) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/duplct
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/duplct
    //
    // SETRH/SETR3 fills jobs 10 down to zero while decrementing SECTOR.
    // This host phase initializer retains that eleven-page grouping, but
    // format_work submits reads and writes serially rather than in parallel.
    work->group_low = work->sector > DOS_DUPLICATE_LAST_BUFFER ? work->sector - DOS_DUPLICATE_LAST_BUFFER : 0;
    work->first_buffer = DOS_DUPLICATE_LAST_BUFFER - (work->sector - work->group_low);
    work->buffer = work->first_buffer;
    work->phase = DOS_FORMAT_COPY_READ;
}

// Advance destructive NEW/DUPLICATE by at most one synchronous media operation.
static void format_work(dos_unit_t* const unit, dos_media_bank_t* const media) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/new
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/new
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/frets
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/map
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/duplct
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/duplct
    //
    // Raw images expose formatter data bytes, not magnetic headers/gaps.
    // Both pinned formatter payloads write zero-filled sector data. Their
    // physical retries, parallel controller jobs and RAM code echo are not
    // modeled here. Captured identities and one-sector turns bound host work.
    dos_format_work_t* const work = &unit->format;
    const dos_media_ref_t reference = work->media;
    const dos_model_t model = media->units[reference.unit].model;
    const diskimage_t* const image = &media->units[reference.unit].slots[reference.slot].io.image;
    dos_bam_t* const bam = &unit->bam[reference.slot];
    uint8_t* const bytes = bam->buffer.bytes;
    const unsigned int tracks = model == DOS_MODEL_4040   ? DISKIMAGE_D64_TRACK_COUNT
                                : model == DOS_MODEL_8050 ? DISKIMAGE_D80_TRACK_COUNT
                                                          : DISKIMAGE_D82_TRACK_COUNT;
    const unsigned int parts = (tracks + DOS_BAM_TRACKS_PER_PART - 1u) / DOS_BAM_TRACKS_PER_PART;
    const uint8_t version = model == DOS_MODEL_4040 ? DOS_VERSION_4040 : DOS_VERSION_8050;
    if (work->phase == DOS_FORMAT_INITIALIZE || work->phase == DOS_FORMAT_COPY_FINISH) {
        if (unit->initialize.pending) {
            initialize_work(unit, media);
            return;
        }
        if (!unit->initialize.succeeded) {
            if (work->phase == DOS_FORMAT_COPY_FINISH) {
                unit->buffer_use &= (uint16_t)~((1u << (DOS_DUPLICATE_LAST_BUFFER + 1u)) - 1u);
            }
            unit->command = DOS_COMMAND_IDLE;
            return;
        }
        if (work->phase == DOS_FORMAT_COPY_FINISH) {
            unit->buffer_use &= (uint16_t)~((1u << (DOS_DUPLICATE_LAST_BUFFER + 1u)) - 1u);
            unit->command = DOS_COMMAND_IDLE;
            unit->selected_drive = reference.slot;
            command_end(unit, model);
            return;
        }
        const dos_media_ref_t initialized = work->duplicate ? work->source : reference;
        const dos_bam_t* const initialized_bam = &unit->bam[initialized.slot];
        if (initialized_bam->buffer.bytes[DOS_BAM_VERSION_OFFSET] != version) {
            unit->selected_drive = initialized.slot;
            dos_status(unit, model, DOS_STATUS_POWER_ON, initialized_bam->buffer.track, initialized_bam->buffer.sector);
            unit->command = DOS_COMMAND_IDLE;
            return;
        }
        unit->selected_drive = reference.slot;
        if (!work->duplicate) {
            memcpy(work->id, unit->initialize.id, sizeof(work->id));
        }
        work->phase = DOS_FORMAT_START;
        return;
    }
    dos_media_ref_t current;
    unit->selected_drive = reference.slot;
    if (dos_media_reference(media, reference.unit, reference.slot, &current) != DOS_MEDIA_OK ||
        current.identity != reference.identity) {
        storage_error(unit, media, reference, reference.identity == 0 ? DOS_MEDIA_ABSENT : DOS_MEDIA_STALE, true,
                      work->track, work->sector);
        goto failed;
    }
    if (work->duplicate &&
        (dos_media_reference(media, work->source.unit, work->source.slot, &current) != DOS_MEDIA_OK ||
         current.identity != work->source.identity)) {
        storage_error(unit, media, work->source, DOS_MEDIA_STALE, false, work->track, work->sector);
        goto failed;
    }
    if (work->phase == DOS_FORMAT_START) {
        if (work->full) {
            // DOS 2.7 NEW calls CLRCHN (SA 14..1, on either disk).
            // DUPLICATE calls CLDCHN for each disk. Neither path CLOSEs writers.
            if (work->duplicate) {
                free_drive_channels(unit, reference.slot);
            } else if (model != DOS_MODEL_4040) {
                for (uint8_t secondary = IEEE_COMMAND_CHANNEL; --secondary != IEEE_LOAD_CHANNEL;) {
                    const uint8_t owner = dos_find_channel(unit, secondary);
                    if (owner != DOS_UNUSED) {
                        free_channel(unit, owner);
                    }
                }
            }
            // Never overwrite live pages with formatter/copy scratch. This is
            // an explicit host resource safeguard, as for VALIDATE's fixed pages.
            const unsigned int scratch_count = work->duplicate           ? DOS_DUPLICATE_LAST_BUFFER + 1u
                                               : model == DOS_MODEL_4040 ? 3u
                                                                         : 4u;
            const uint16_t scratch = (1u << scratch_count) - 1u;
            if (unit->buffer_use & scratch) {
                dos_status(unit, model, DOS_STATUS_NO_CHANNEL, 0, 0);
                goto failed;
            }
            if (work->duplicate) {
                unit->buffer_use |= scratch;
            }
        }
        *bam = (dos_bam_t){.media = reference};
        bam->buffer.resident = true;
        bam->buffer.bytes[DOS_BAM_VERSION_OFFSET] = version;
        unit->free_blocks_valid[reference.slot] = false;
        work->track = 1;
        work->sector = 0;
        work->part = 0;
        work->phase = work->full ? DOS_FORMAT_ERASE : model == DOS_MODEL_4040 ? DOS_FORMAT_DIRECTORY : DOS_FORMAT_MAP;
        return;
    }
    if (work->phase == DOS_FORMAT_ERASE) {
        memset(bytes, 0, DISKIMAGE_SECTOR_SIZE);
        // Formatting replaces foreign versions without consulting their BAM.
        // It still goes through media generation/write-protection validation.
        if (!storage_error(unit, media, reference, dos_media_write(media, reference, work->track, work->sector, bytes),
                           true, work->track, work->sector)) {
            goto failed;
        }
        unsigned int sectors;
        vet(diskimage_track_sectors(image->type, work->track, &sectors), "invalid format track %u", work->track);
        if (++work->sector == sectors) {
            work->sector = 0;
            ++work->track;
        }
        if (work->track > tracks) {
            bytes[DOS_BAM_VERSION_OFFSET] = version;
            if (work->duplicate) {
                work->track = 1;
                vet(diskimage_track_sectors(image->type, work->track, &sectors), "invalid duplicate track");
                work->sector = sectors - 1u;
                format_copy_group(work);
            } else {
                work->phase = model == DOS_MODEL_4040 ? DOS_FORMAT_DIRECTORY : DOS_FORMAT_MAP;
            }
        }
        return;
    }
    if (work->phase == DOS_FORMAT_COPY_READ || work->phase == DOS_FORMAT_COPY_WRITE) {
        const uint8_t sector = work->group_low + work->buffer - work->first_buffer;
        uint8_t* const page = unit->buffers[work->buffer].bytes;
        if (work->phase == DOS_FORMAT_COPY_READ) {
            if (!storage_error(unit, media, work->source,
                               read_sector(unit, media, work->source, work->track, sector, page), false, work->track,
                               sector)) {
                goto failed;
            }
        } else if (!write_sector(unit, media, reference, work->track, sector, page)) {
            goto failed;
        }
        if (++work->buffer > DOS_DUPLICATE_LAST_BUFFER) {
            work->buffer = work->first_buffer;
            if (work->phase == DOS_FORMAT_COPY_READ) {
                work->phase = DOS_FORMAT_COPY_WRITE;
            } else if (work->group_low != 0) {
                work->sector = work->group_low - 1u;
                format_copy_group(work);
            } else if (++work->track <= tracks) {
                unsigned int sectors;
                vet(diskimage_track_sectors(image->type, work->track, &sectors), "invalid duplicate track");
                work->sector = sectors - 1u;
                format_copy_group(work);
            } else {
                work->phase = DOS_FORMAT_COPY_FINISH;
                format_initialize(unit, reference);
            }
        }
        return;
    }
    if (work->phase == DOS_FORMAT_BAM) {
        // NEW3/USEDTS refills the first map before OPNIRD can exhaust channels.
        if (!storage_error(unit, media, reference, read_sector(unit, media, reference, DOS_BAM_TRACK_8050, 0, bytes),
                           false, DOS_BAM_TRACK_8050, 0)) {
            goto failed;
        }
        bam->buffer.track = DOS_BAM_TRACK_8050;
        bam->buffer.sector = 0;
        if (!mark_sector(unit, media, reference, directory_track(model), 0, false)) {
            goto failed;
        }
        work->phase = DOS_FORMAT_CHAIN;
        return;
    }
    if (work->phase == DOS_FORMAT_CHAIN) {
        if (work->reader == DOS_UNUSED) {
            // NEW3 opens OPNIRD only after publishing the metadata prefix.
            work->reader = allocate_channel(unit, reference, DOS_INTERNAL_READ_SECONDARY, 2);
            if (work->reader == DOS_UNUSED) {
                dos_status(unit, model, DOS_STATUS_NO_CHANNEL, 0, 0);
                goto failed;
            }
            unit->channels[work->reader].ready = false;
            work->track = directory_track(model);
            work->sector = 0;
            work->part = 0;
        }
        // N150/NXTBUF must select every header/BAM/directory page. Split the
        // native double-buffered jobs into one synchronous read per turn.
        if (work->part++ >= parts + 2u) {
            dos_status(unit, model, DOS_STATUS_DIRECTORY_ERROR, work->track, work->sector);
            goto failed;
        }
        dos_channel_t* const reader = &unit->channels[work->reader];
        dos_buffer_t* const page = &unit->buffers[reader->buffers[reader->active]];
        if (!storage_error(unit, media, reference,
                           read_sector(unit, media, reference, work->track, work->sector, page->bytes), false,
                           work->track, work->sector)) {
            goto failed;
        }
        page->track = work->track;
        page->sector = work->sector;
        page->resident = true;
        work->track = page->bytes[0];
        work->sector = page->bytes[1];
        reader->active ^= 1u;
        if (work->track == 0) {
            work->part = 0;
            work->phase = DOS_FORMAT_RESERVE;
        }
        return;
    }
    if (work->phase == DOS_FORMAT_MAP || work->phase == DOS_FORMAT_RESERVE) {
        format_map(unit, media, work->phase == DOS_FORMAT_RESERVE);
        const uint8_t track = model == DOS_MODEL_4040 ? DOS_DIRECTORY_TRACK_4040 : DOS_BAM_TRACK_8050;
        const uint8_t sector = work->part * DOS_BAM_PART_INTERLEAVE;
        if (model == DOS_MODEL_4040) {
            format_label(unit, model);
        }
        if (!write_sector(unit, media, reference, track, sector, bytes)) {
            goto failed;
        }
        if (work->phase == DOS_FORMAT_RESERVE) {
            bam->buffer.track = track;
            bam->buffer.sector = sector;
            bam->dirty = false;
            unit->free_blocks[reference.slot] = 0;
            work->phase = DOS_FORMAT_RELOAD;
        } else if (++work->part == parts) {
            work->part = 0;
            work->phase = DOS_FORMAT_DIRECTORY;
        }
        return;
    }
    if (work->phase == DOS_FORMAT_DIRECTORY || work->phase == DOS_FORMAT_DISK_HEADER) {
        memset(bytes, 0, DISKIMAGE_SECTOR_SIZE);
        const uint8_t sector = work->phase == DOS_FORMAT_DIRECTORY ? DOS_DIRECTORY_FIRST_SECTOR : 0;
        if (sector == DOS_DIRECTORY_FIRST_SECTOR) {
            bytes[1] = DOS_DIRECTORY_LAST_MARKER;
        } else {
            bytes[0] = DOS_BAM_TRACK_8050;
            bytes[DOS_BAM_VERSION_OFFSET] = version;
            format_label(unit, model);
        }
        // The shared cache doubles as the write source, so keep its version
        // unset or native. A directory page's byte 2 is zero.
        if (!write_sector(unit, media, reference, directory_track(model), sector, bytes)) {
            goto failed;
        }
        work->phase = model == DOS_MODEL_4040                ? DOS_FORMAT_RESERVE
                      : sector == DOS_DIRECTORY_FIRST_SECTOR ? DOS_FORMAT_DISK_HEADER
                                                             : DOS_FORMAT_BAM;
        return;
    }
    vet(work->phase == DOS_FORMAT_RELOAD, "invalid format phase %u", work->phase);
    // NF05 leaves the final map range cached. Do not call INITDR here: NEW
    // does not abandon the additional channels that a fresh INITDR would.
    if (model == DOS_MODEL_4040) {
        // 4040 NEW ends after DRTWRT. Retain the generated map without a read.
        unsigned int free_blocks = 0;
        for (unsigned int current_track = 1; current_track <= tracks; ++current_track) {
            unsigned int sectors;
            vet(diskimage_track_sectors(image->type, current_track, &sectors), "invalid format track");
            if (current_track != directory_track(model)) {
                free_blocks += sectors;
            }
        }
        unit->free_blocks[reference.slot] = free_blocks;
    } else {
        // NF05 starts with the freshly scrubbed first part already cached.
        // Follow subsequent ranges one read per turn, retaining the final part.
        if (work->part != 0) {
            if (!storage_error(unit, media, reference,
                               read_sector(unit, media, reference, work->track, work->sector, bytes), false,
                               work->track, work->sector)) {
                goto failed;
            }
            bam->buffer.track = work->track;
            bam->buffer.sector = work->sector;
        }
        const unsigned int low = work->part * DOS_BAM_TRACKS_PER_PART + 1u;
        const unsigned int high = MIN(low + DOS_BAM_TRACKS_PER_PART, tracks + 1u);
        if (bytes[4] != low || bytes[5] != high) {
            dos_status(unit, model, DOS_STATUS_DIRECTORY_ERROR, bam->buffer.track, bam->buffer.sector);
            goto failed;
        }
        for (unsigned int track = low; track < high; ++track) {
            if (track != directory_track(model)) {
                unit->free_blocks[reference.slot] += bytes[6u + (track - low) * 5u];
            }
        }
        if (++work->part < parts) {
            work->track = bytes[0];
            work->sector = bytes[1];
            return;
        }
    }
    bam->buffer.resident = true;
    unit->free_blocks_valid[reference.slot] = true;
    if (work->reader != DOS_UNUSED) {
        free_channel(unit, work->reader);
        work->reader = DOS_UNUSED;
    }
    unit->command = DOS_COMMAND_IDLE;
    command_end(unit, model);
    return;

failed:
    // Generated formatter pages borrow BAM storage without keeping a valid map
    // binding. Preserve the cache only before scratch was touched.
    if (work->phase >= DOS_FORMAT_ERASE) {
        *bam = (dos_bam_t){.media = reference};
        unit->free_blocks_valid[reference.slot] = false;
    }
    if (work->reader != DOS_UNUSED) {
        free_channel(unit, work->reader);
        work->reader = DOS_UNUSED;
    }
    if (work->duplicate && work->phase >= DOS_FORMAT_ERASE) {
        unit->buffer_use &= (uint16_t)~((1u << (DOS_DUPLICATE_LAST_BUFFER + 1u)) - 1u);
    }
    unit->command = DOS_COMMAND_IDLE;
}

// Dispatch collected metadata, block, reset, COPY and NEW/DUPLICATE commands without taking over transport collection.
// The caller completes preceding work before submitting another request.
void dos_command(dos_unit_t* const unit, dos_media_bank_t* const media, const unsigned int index,
                 const uint8_t* const request, unsigned int length) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/parsex
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/parsex
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/verdir
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/verdir
    //
    // CMDSET trims either trailing CR position before LONGLN. PARSXQ dispatches
    // by the first byte. This host API schedules the supported workers, not the
    // native input collector, excluded command families, or production facade.
    vet(!dos_work_pending(unit), "native command requires its preceding job to complete");
    const dos_model_t model = media->units[index].model;
    if (length > DOS_COMMAND_RECEIVE_CAPACITY) {
        dos_status(unit, model, DOS_STATUS_COMMAND_TOO_LONG, 0, 0);
    } else {
        // Keep unread tail bytes available to native parsers, including a name
        // starting at logical EOF. The final receive byte lies beyond CLRCB.
        memmove(unit->command_bytes, request, length);
        const uint8_t* const bytes = unit->command_bytes;
        if (length >= 2u) {
            if (bytes[length - 1u] == '\r') {
                --length;
            } else if (bytes[length - 2u] == '\r') {
                length -= 2u;
            }
        }
        if (length > DOS_COMMAND_LIMIT) {
            dos_status(unit, model, DOS_STATUS_COMMAND_TOO_LONG, 0, 0);
        } else if (bytes[0] == 'B') {
            block_start(unit, media, index, length, 0);
        } else if (bytes[0] == 'I') {
            // INTDRV/SIMPRS selects one disk or both, starting with LSTDRV.
            // Capture identities now, before cooperative INITDR reloads them.
            // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
            // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
            bool both;
            const uint8_t drive = initialize_drive(bytes, length, unit->last_drive, &both);
            unit->initialize = (dos_initialize_work_t){
                .first_drive = drive,
                .slots = both ? (1u << DOS_MEDIA_SLOTS_PER_UNIT) - 1u : 1u << drive,
                .pending = true,
            };
            for (unsigned int slot = 0; slot < DOS_MEDIA_SLOTS_PER_UNIT; ++slot) {
                dos_media_ref_t* const reference = &unit->initialize.media[slot];
                *reference = (dos_media_ref_t){.unit = index, .slot = slot};
                dos_media_reference(media, index, slot, reference);
            }
            dos_status(unit, model, DOS_STATUS_OK, 0, 0);
        } else if (bytes[0] == 'R') {
            rename_start(unit, media, index, length);
        } else if (bytes[0] == 'S') {
            scratch_start(unit, media, index, length);
        } else if (bytes[0] == 'C') {
            copy_start(unit, media, index, length);
        } else if (bytes[0] == 'N' || bytes[0] == 'D') {
            format_start(unit, media, index, length, bytes[0] == 'D');
        } else if (bytes[0] == 'V') {
            // VALDAT uses SIMPRS but initializes only the selected disk.
            // Capture its identity before any deferred initialization I/O.
            bool both;
            const uint8_t drive = initialize_drive(bytes, length, unit->last_drive, &both);
            dos_media_ref_t reference = {.unit = index, .slot = drive};
            dos_media_reference(media, index, drive, &reference);
            unit->validate = (dos_validate_work_t){
                .media = reference,
                .reader = DOS_UNUSED,
                .phase = DOS_VALIDATE_INITIALIZE,
            };
            unit->initialize = (dos_initialize_work_t){
                .first_drive = drive,
                .slots = 1u << drive,
                .pending = true,
            };
            unit->initialize.media[drive] = reference;
            unit->selected_drive = drive;
            unit->command = DOS_COMMAND_VALIDATE;
            dos_status(unit, model, DOS_STATUS_OK, 0, 0);
        } else {
            if (bytes[0] == 'U') {
                // USER/USREXC selects the factory UBLOCK vector by low nibble.
                // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/block
                // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/block
                // USER reads CMDBUF+1 even for a short request. U0's earlier return
                // can leave its selector there. All storage remains inside the array.
                const uint8_t selector = bytes[1];
                if (selector == '0') {
                    // Custom vector mutation is excluded, so USRINT restores a table
                    // that is already fixed. It neither closes channels nor ENDCMDs.
                    dos_status(unit, model, DOS_STATUS_OK, 0, 0);
                    return;
                }
                // USREXC folds (selector - 1) to a four-bit table index, so alphabetic
                // and numeric aliases share vectors. U1/U2 select raw block transfers.
                if (((selector - 1u) & DOS_USER_SELECTOR_MASK) == DOS_USER_BLOCK_READ_INDEX) {
                    block_start(unit, media, index, length, 'R');
                    clear_command(unit);
                    return;
                }
                if (((selector - 1u) & DOS_USER_SELECTOR_MASK) == DOS_USER_BLOCK_WRITE_INDEX) {
                    block_start(unit, media, index, length, 'W');
                    clear_command(unit);
                    return;
                }
                if (((selector - 1u) & DOS_USER_SELECTOR_MASK) == DOS_USER_WARM_RESET_INDEX) {
                    // UBLOCK's U9/UI vector enters NMI, preserving command RAM.
                    warm_reset(unit, model);
                    return;
                }
                if (((selector - 1u) & DOS_USER_SELECTOR_MASK) == DOS_USER_RESET_INDEX) {
                    // U:/UJ enters DSKINT, abandoning DOS state without host CLOSE.
                    dos_reset(unit, model);
                    clear_command(unit);
                    return;
                }
                if (((selector - 1u) & DOS_USER_SELECTOR_MASK) == DOS_USER_INTERRUPT_INDEX) {
                    // Default vector:UBLOCK enters ieee:ATNIRQ, not NMI/DSKINT.
                    // The transport already handles ATN. PARSXQ's OK survives and
                    // the stack reset bypasses ENDCMD/CLRCB without touching channels.
                    dos_status(unit, model, DOS_STATUS_OK, 0, 0);
                    return;
                }
            }
            // Recognized but excluded commands return explicit host policy 30,
            // not a claimed native result. Preserve each model's command table.
            const char* const commands = model == DOS_MODEL_4040 ? "IVDMBUPCRSN" : "VIDMBUP&CRSN";
            const bool known = bytes[0] != 0 && strchr(commands, bytes[0]) != NULL;
            dos_status(unit, model, known ? DOS_STATUS_SYNTAX : DOS_STATUS_INVALID_COMMAND, 0, 0);
        }
    }
    // Workers retain their own names/patterns before native CLRCB retirement.
    clear_command(unit);
}

// Parse OPEN/CKTM parameters, retaining OP60's raw binary record-size byte.
// Update the supplied defaults. Return false for unsupported or invalid syntax.
bool dos_open_parse(const uint8_t input[DOS_COMMAND_RECEIVE_CAPACITY], const unsigned int length, uint8_t* const drive,
                    dos_open_work_t* const work) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/open
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/open
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/parsex
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/parsex
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lookup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lookup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/trnsfr
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/trnsfr
    //
    // ROM open:OPEN/OP042/OP20 use PRSCLN and PARSE to separate drive, name,
    // and parameters. An omitted drive retains the two-drive search flag.
    vet(length <= DOS_COMMAND_LIMIT, "OPEN parser requires bounded command input");
    unsigned int cursor = 0;
    work->replace = length != 0 && input[0] == '@';
    work->wildcard = false;
    work->default_drive = true;
    unsigned int commas = 0;
    // PRSCLN leaves its low PATFLG count for OP20's second PARSE. A comma
    // records that field's flag and clears the count, not the earlier flags.
    bool first_wildcard = false, scanned_wildcard = false;
    for (unsigned int index = 0; index < length && commas < DOS_FILENAME_COUNT - 1u; ++index) {
        scanned_wildcard |= input[index] == '*' || input[index] == '?';
        if (input[index] == ',') {
            if (commas == 0) {
                first_wildcard = scanned_wildcard;
            }
            scanned_wildcard = false;
            ++commas;
        }
        if (input[index] == ':') {
            if (commas != 0) {
                return false;
            }
            work->default_drive = false;
            cursor = index + 1u;
            if (index != 0 && (input[index - 1] == '0' || input[index - 1] == '1')) {
                *drive = input[index - 1] - '0';
            }
            break;
        }
    }
    work->wildcard = first_wildcard || scanned_wildcard;

    // ROM lookup:COMPAR compares up to 16 explicitly supplied bytes, including
    // literal $A0. Preserve that length independently of the padded storage,
    // without case folding or PETSCII conversion. trnsfr:FNDLMT ends the
    // compared name at comma or equals, independently of parameter scanning.
    memset(work->name, DISKIMAGE_FILENAME_PADDING, sizeof(work->name));
    unsigned int used = 0;
    while (cursor < length && input[cursor] != ',' && input[cursor] != '=') {
        const uint8_t byte = input[cursor++];
        if (byte == 0x8d) {
            return false;
        }
        work->wildcard |= byte == '*' || byte == '?';
        if (used < DISKIMAGE_FILENAME_SIZE) {
            work->name[used++] = byte;
        }
    }
    if (used == 0) {
        return false;
    }
    work->name_length = (uint8_t)used;
    // PARSE still finds comma-separated fields after an ignored equals suffix.
    while (cursor < length && input[cursor] != ',' && input[cursor] != 0x8d) {
        work->wildcard |= input[cursor] == '*' || input[cursor] == '?';
        ++cursor;
    }

    // PARSE counts empty fields too. OP60 reads the original byte even when
    // CMDSET trimmed a CR or the field begins in bounded stale command RAM.
    unsigned int parameter = 0;
    bool relative = false;
    while (cursor < length && input[cursor] != 0x8d && parameter < 2u) {
        ++cursor;
        const unsigned int start = cursor;
        while (cursor < length && input[cursor] != ',' && input[cursor] != 0x8d) {
            ++cursor;
        }
        if (parameter++ == 1u && relative) {
            work->record_size = input[start];
            work->record_parameter = true;
            break;
        }
        switch (input[start]) {
        case 'R':
            work->access = DOS_ACCESS_READ;
            break;
        case 'W':
            work->access = DOS_ACCESS_WRITE;
            break;
        case 'A':
            work->access = DOS_ACCESS_APPEND;
            break;
        case 'M':
            work->access = DOS_ACCESS_MODIFY;
            break;
        case 'D':
            work->type = 0;
            break;
        case 'P':
            work->type = DISKIMAGE_FTYPE_PRG;
            break;
        case 'S':
            work->type = DISKIMAGE_FTYPE_SEQ;
            break;
        case 'U':
            work->type = DISKIMAGE_FTYPE_USR;
            break;
        case 'L':
            work->type = DISKIMAGE_FTYPE_REL;
            relative = true;
            break;
        default:
            break;
        }
        if (cursor < length && input[cursor] == 0x8d) {
            break;
        }
    }

    return true;
}

// Parse every LOADIR pattern before OPTSCH chooses a drive for media admission.
static dos_directory_request_t parse_directory(const uint8_t* const input, const unsigned int length,
                                               const uint8_t last_drive) {
    // Both pinned ROMs: open:LOADIR, parsex:TC35, setdrv:ALLDRS/SETDRV,
    // lookup:OPTSCH/SCHTBL. A missing prefix retains the default-search bit.
    dos_directory_request_t request = {0};
    memset(request.patterns, DISKIMAGE_FILENAME_PADDING, sizeof(request.patterns));
    unsigned int cursor = 1, drive_mask = 0;
    uint8_t inherited_drive = last_drive;
    if (length == 1 || (length == 2 && (input[1] == '0' || input[1] == '1'))) {
        uint8_t* const pattern = request.patterns[request.count++];
        pattern[0] = '*';
        pattern[DOS_DIRECTORY_PATTERN_DRIVE] =
            length == 1 ? last_drive | DOS_PATTERN_DEFAULT_DRIVE : input[1] - (unsigned int)'0';
        pattern[DOS_DIRECTORY_PATTERN_LENGTH] = 1;
        pattern[DOS_DIRECTORY_PATTERN_FLAGS] = DOS_PATTERN_WILDCARD;
        cursor = length;
    }
    while (cursor < length && input[cursor] != '=' && request.count < DOS_FILENAME_COUNT) {
        uint8_t drive = inherited_drive | DOS_PATTERN_DEFAULT_DRIVE;
        if (input[cursor] == ':') {
            drive = inherited_drive;
            ++cursor;
        } else if (cursor + 1u < length && input[cursor + 1u] == ':') {
            drive = input[cursor] == '0' || input[cursor] == '1' ? input[cursor] - '0' : inherited_drive;
            cursor += 2u;
        }
        inherited_drive = drive & DOS_PATTERN_DRIVE_MASK;
        uint8_t* const pattern = request.patterns[request.count++];
        pattern[DOS_DIRECTORY_PATTERN_DRIVE] = drive;
        pattern[DOS_DIRECTORY_PATTERN_FLAGS] = 0;
        unsigned int used = 0;
        while (cursor < length && input[cursor] != ',' && input[cursor] != '=') {
            // PARSE records wildcards beyond COMPAR's cap, but stops scanning
            // after the fourth comma. The fifth pattern has no wildcard flag.
            if (request.count < DOS_FILENAME_COUNT && (input[cursor] == '*' || input[cursor] == '?')) {
                pattern[DOS_DIRECTORY_PATTERN_FLAGS] |= DOS_PATTERN_WILDCARD;
            }
            if (used < DISKIMAGE_FILENAME_SIZE) {
                pattern[used++] = input[cursor];
            }
            ++cursor;
        }
        pattern[DOS_DIRECTORY_PATTERN_LENGTH] = (uint8_t)used;
        if (cursor < length && input[cursor] == ',') {
            ++cursor;
        }
    }
    for (unsigned int index = 0; index < request.count; ++index) {
        const uint8_t drive = request.patterns[index][DOS_DIRECTORY_PATTERN_DRIVE];
        drive_mask |= 1u << ((drive & DOS_PATTERN_DRIVE_MASK) + ((drive & DOS_PATTERN_DEFAULT_DRIVE) ? 2u : 0u));
    }
    // SCHTBL starts one default drive first, or preserves the first pattern
    // when both explicit drives or both default preferences are represented.
    const unsigned int defaults = drive_mask >> 2u;
    request.drive = request.patterns[0][DOS_DIRECTORY_PATTERN_DRIVE] & DOS_PATTERN_DRIVE_MASK;
    if (defaults == 1 || (defaults == 0 && drive_mask == 1)) {
        request.drive = 0;
    } else if (defaults == 2 || (defaults == 0 && drive_mask == 2)) {
        request.drive = 1;
    }
    request.searches = defaults == 3 ? DOS_DIRECTORY_DEFAULT_BOTH_SEARCHES : defaults != 0 || drive_mask == 3;
    if (request.count < DOS_FILENAME_COUNT && cursor < length && input[cursor] == '=') {
        ++cursor;
        // TC35 stops at shifted CR or PARSE's shared field-table limit.
        // FS1SET selects the final retained field by its first character.
        unsigned int fields = request.count;
        for (unsigned int offset = cursor; offset < length && input[offset] != 0x8d; ++offset) {
            if (input[offset] == ',') {
                cursor = offset + 1u;
                if (++fields >= DOS_FILENAME_COUNT - 1u) {
                    break;
                }
            }
        }
        if (cursor < length) {
            switch (input[cursor]) {
            case 'P':
                request.type = DISKIMAGE_FTYPE_PRG;
                break;
            case 'S':
                request.type = DISKIMAGE_FTYPE_SEQ;
                break;
            case 'U':
                request.type = DISKIMAGE_FTYPE_USR;
                break;
            case 'R':
                request.type = DISKIMAGE_FTYPE_REL;
                break;
            default:
                break;
            }
        }
    }
    return request;
}

// SIMPRS/SETANY selects raw-directory drives independently of LOADIR patterns.
static uint8_t raw_directory_drive(const uint8_t* const input, const unsigned int length, const uint8_t last_drive,
                                   bool* const default_drive) {
    // Both pinned parsex:SIMPRS paths retain the byte before the first colon.
    // PARSE stops at the fourth comma. Without a colon, FILTBL remains zero.
    unsigned int selected = 0, commas = 0;
    for (unsigned int offset = 0; offset < length && commas < DOS_FILENAME_COUNT - 1u; ++offset) {
        if (input[offset] == ':') {
            selected = offset - 1u;
            break;
        }
        commas += input[offset] == ',';
    }
    // setdrv:SETANY tries that byte, then the final byte, then LSTDRV.
    uint8_t byte = input[selected];
    if (byte != '0' && byte != '1' && selected + 1u < length) {
        byte = input[length - 1u];
    }
    *default_drive = byte != '0' && byte != '1';
    return *default_drive ? last_drive : byte - (unsigned int)'0';
}

// CMPCHK keeps wildcard requests live and skips completed exact-name requests.
static bool directory_patterns_pending(const dos_unit_t* const unit, const dos_channel_t* const channel) {
    for (unsigned int index = 0; index < channel->pattern_count; ++index) {
        const uint8_t* const pattern = unit->buffers[channel->buffers[2]].bytes + index * DOS_DIRECTORY_PATTERN_SIZE;
        if (!(pattern[DOS_DIRECTORY_PATTERN_FLAGS] & DOS_PATTERN_COMPLETE)) {
            return true;
        }
    }
    return false;
}

// Start a raw header chain or a BASIC directory with already-admitted patterns.
static bool open_directory(dos_unit_t* const unit, dos_media_bank_t* const media, const dos_media_ref_t reference,
                           const uint8_t secondary, const dos_directory_request_t* const request) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/open
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/open
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lstdir
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lstdir
    //
    // OPEN distinguishes LOAD's BASIC listing from a nonzero secondary's
    // raw directory chain. LOADIR parses patterns and STDIR starts listing
    // output, with directory search retaining an internal channel.
    const dos_model_t model = media->units[reference.unit].model;
    const uint8_t previous = dos_find_channel(unit, secondary);
    if (previous != DOS_UNUSED) {
        // Retire a writable host owner without publishing CLOSE's ENDCMD.
        if (unit->channels[previous].access != DOS_ACCESS_READ &&
            unit->channels[previous].access != DOS_ACCESS_DIRECT &&
            !finish_channel(unit, media, &unit->channels[previous])) {
            return false;
        }
        free_channel(unit, previous);
    }
    unit->selected_drive = reference.slot;
    if (secondary == IEEE_LOAD_CHANNEL) {
        // DOS 2.7 LOADIR/NEWDIR enters AUTOI before its initial header.
        auto_initialize(unit, reference.slot);
    }
    const uint8_t index = allocate_channel(unit, reference, secondary, 2);
    if (index == DOS_UNUSED) {
        command_error(unit, model, secondary, DOS_STATUS_NO_CHANNEL);
        return false;
    }
    dos_channel_t* const channel = &unit->channels[index];
    if (secondary != IEEE_LOAD_CHANNEL) {
        // Raw directory reads start at the header, not directory sector 1.
        if (!load_buffer(unit, media, channel, directory_track(model), 0)) {
            free_channel(unit, index);
            command_error(unit, model, secondary, DOS_STATUS_READ_ERROR);
            return false;
        }
        return true;
    }
    // Reserve the internal search index separately from the public channel.
    // C uses two row/source buffers plus one bounded pattern buffer.
    uint8_t internal = 0;
    while (internal < DOS_FILE_CHANNEL_COUNT && unit->channels[internal].allocated) {
        ++internal;
    }
    const uint8_t extra = get_buffer(unit);
    if (internal == DOS_FILE_CHANNEL_COUNT || extra == DOS_UNUSED) {
        // A partial reservation must not leave an unreachable buffer/channel.
        if (extra != DOS_UNUSED) {
            unit->buffer_use &= (uint16_t)~(1u << extra);
        }
        free_channel(unit, index);
        command_error(unit, model, secondary, DOS_STATUS_NO_CHANNEL);
        return false;
    }
    unit->directory_internal = internal + 1u;
    channel->buffers[2] = extra;
    channel->directory = DOS_DIRECTORY_HEADER;
    channel->directory_track = directory_track(model);
    channel->directory_sector = DOS_DIRECTORY_FIRST_SECTOR;
    channel->position = 0;
    channel->traversed = 0;
    memcpy(unit->buffers[extra].bytes, request->patterns, sizeof(request->patterns));
    channel->pattern_count = request->count;
    channel->file_type = request->type;
    channel->directory_searches = request->searches;
    return true;
}

// Open a direct channel on one automatic or explicitly requested raw data page.
// Preserve page contents and prepare its binary page number as the first byte.
static void open_direct(dos_unit_t* const unit, dos_media_bank_t* const media, const unsigned int index,
                        const uint8_t secondary, const uint8_t* const input, const unsigned int length) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/block
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/block
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst3
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst3
    //
    // OPNBLK requests one GETRCH buffer for bare '#'. OB10 instead reserves
    // an explicit page before calling GETRCH with zero buffers. OB30 makes
    // the channel readable/writable without a filename lookup or ADDFIL.
    const dos_model_t model = media->units[index].model;
    if (model != DOS_MODEL_4040) {
        // DOS 2.7 starts OPNBLK with LSTDRV/AUTOI. C invalidates metadata
        // lazily rather than reproducing the ROM's immediate disk jobs.
        unit->selected_drive = unit->last_drive;
        auto_initialize(unit, unit->selected_drive);
    }
    const bool automatic = length == 1;
    uint8_t buffer = DOS_UNUSED;
    if (!automatic && !block_parameters(input, length, 1, &buffer, 1)) {
        command_error(unit, model, secondary, DOS_STATUS_SYNTAX);
        return;
    }
    if (!automatic) {
        // Test ownership before freeing this secondary's old channel, so an
        // explicitly requested page already owned by that channel is rejected.
        if (buffer >= DOS_BUFFER_COUNT || (unit->buffer_use & (1u << buffer))) {
            command_error(unit, model, secondary, DOS_STATUS_NO_CHANNEL);
            return;
        }
        // Native explicit reservation precedes GETRCH's channel search.
        unit->buffer_use |= (uint16_t)(1u << buffer);
    }

    // GETRCH/FRECHN abandon prior ownership without writable CLOSE semantics.
    const uint8_t previous = dos_find_channel(unit, secondary);
    if (previous != DOS_UNUSED) {
        free_channel(unit, previous);
    }
    if (model != DOS_MODEL_4040) {
        unit->selected_drive = unit->last_drive;
    }
    // Capture host media identity for subsequent sector operations. Merely
    // opening the RAM page does not require successful media binding here.
    dos_media_ref_t reference = {.unit = index, .slot = unit->selected_drive};
    dos_media_reference(media, index, reference.slot, &reference);
    const uint8_t channel = allocate_channel(unit, reference, secondary, automatic ? 1 : 0);
    if (channel == DOS_UNUSED) {
        // Match OB10's ordering: an explicit page reservation can survive
        // logical-channel exhaustion even though no channel was published.
        command_error(unit, model, secondary, DOS_STATUS_NO_CHANNEL);
        return;
    }
    dos_channel_t* const state = &unit->channels[channel];
    if (automatic) {
        // GETBUF chooses the page and initializes its last-job drive.
        buffer = state->buffers[0];
    } else {
        state->buffers[0] = buffer;
        if (model != DOS_MODEL_4040) {
            const uint16_t bit = 1u << buffer;
            unit->buffer_drives = (unit->buffer_drives & (uint16_t)~bit) | (reference.slot != 0 ? bit : 0);
        }
        // 4040 explicit buffers retain LSTJOB. DOS 2.7 refreshes it in OB10.
        reference = (dos_media_ref_t){.unit = index, .slot = (unit->buffer_drives >> buffer) & 1u};
        dos_media_reference(media, index, reference.slot, &reference);
        state->media = reference;
    }

    // OB30 sets LSTCHR=$FF, BUFTAB=1, and CHNDAT to the binary page number
    // (not bytes[0]). The initial response has no EOI and does not clear RAM.
    state->access = DOS_ACCESS_DIRECT;
    state->position = 1;
    state->direct_last = DISKIMAGE_SECTOR_SIZE - 1u;
    state->direct_byte = buffer;
    command_end(unit, model);
}

// Accept OPEN, handling special names or retaining a directory-search job.
// Finish preceding work first. Special names may perform synchronous disk I/O.
// Reject invalid requests with status instead of starting another job.
void dos_open(dos_unit_t* const unit, dos_media_bank_t* const media, const unsigned int index, const uint8_t secondary,
              const uint8_t* const request, unsigned int length) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/open
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/open
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/parsex
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/parsex
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/setdrv
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/setdrv
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lookup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lookup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/opchnl
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/opchnl
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst3
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst3
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/getact
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/getact
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/begin
    //
    // ROM open:OPEN parses the collected command, FFST searches, OP90 checks
    // read eligibility, and OPREAD calls OPNRCH. Splitting ordinary lookup
    // into dos_work is a firmware scheduling adaptation. That continuation
    // handles multi-drive search and ordinary construction. '$' and retained
    // '*' requests can finish here. '#' opens a raw shared page.
    vet(!dos_work_pending(unit), "native OPEN requires its preceding job to complete");
    const dos_model_t model = media->units[index].model;
    // ROM tst2:L42 collects bounded input. The approved 4040 revision-2 pointer
    // limit is 59, matching DOS 2.7, not the older source's 41. The C
    // receive capacity bounds the raw copy. CMDSET applies the logical
    // 58-byte limit only after removing a trailing CR or CR-plus-one.
    if (length > DOS_COMMAND_RECEIVE_CAPACITY) {
        command_error(unit, model, secondary, DOS_STATUS_COMMAND_TOO_LONG);
        return;
    }
    uint8_t command[DOS_COMMAND_RECEIVE_CAPACITY];
    memcpy(command, unit->command_bytes, sizeof(command));
    memcpy(command, request, length);
    memcpy(unit->command_bytes, command, sizeof(command));
    const uint8_t* const input = command;
    if (length > 1u) {
        if (input[length - 1u] == '\r') {
            --length;
        } else if (input[length - 2u] == '\r') {
            length -= 2u;
        }
    }
    if (length > DOS_COMMAND_LIMIT) {
        command_error(unit, model, secondary, DOS_STATUS_COMMAND_TOO_LONG);
        return;
    }
    if (length != 0 && input[0] == '#') {
        open_direct(unit, media, index, secondary, input, length);
        clear_command(unit);
        return;
    }

    // ROM open:OP40 makes SA 0/1 LOAD/SAVE with a PRG default. Apply that
    // default for LOAD/SAVE and infer other ordinary types from the match.
    // setdrv:ALLDRS seeds omitted and nonnumeric prefixes from LSTDRV,
    // which a failed explicit OPEN must not replace with its DRVNUM.
    const bool load_star = secondary == IEEE_LOAD_CHANNEL && length != 0 && input[0] == '*';
    const bool last_program = load_star && unit->last_program_track != 0;
    const bool first_program = load_star && !last_program;
    // OP0415 resets LSTDRV before parsing, even if the initial LOAD later fails.
    if (first_program) {
        unit->selected_drive = 0;
        unit->last_drive = 0;
        free_drive_channels(unit, 0);
    }
    uint8_t drive = unit->last_drive;
    dos_open_work_t parsed = {0};
    const bool directory = length != 0 && input[0] == '$';
    const bool formatted_directory = directory && secondary == IEEE_LOAD_CHANNEL;
    dos_directory_request_t listing = {0};
    if (formatted_directory) {
        listing = parse_directory(input, length, unit->last_drive);
        drive = listing.drive;
        parsed.default_drive = listing.searches != 0;
        // Only 4040 LOADIR/LD02 initializes exact $0/$1 shorthand.
        if (model == DOS_MODEL_4040 && length == 2 && (input[1] == '0' || input[1] == '1')) {
            free_drive_channels(unit, drive);
        }
    } else if (directory) {
        drive = raw_directory_drive(input, length, unit->last_drive, &parsed.default_drive);
    }
    // OP02 reloads the retained link before PRSCLN/CKTM inspect any suffix.
    if (!directory && !last_program && (length == 0 || !dos_open_parse(input, length, &drive, &parsed))) {
        command_error(unit, model, secondary, length == 0 ? DOS_STATUS_INVALID_NAME : DOS_STATUS_SYNTAX);
        return;
    }
    if (secondary < 2 && !directory) {
        parsed.access = secondary == IEEE_SAVE_CHANNEL ? DOS_ACCESS_WRITE : DOS_ACCESS_READ;
        if (parsed.type == 0) {
            parsed.type = DISKIMAGE_FTYPE_PRG;
        }
    }
    // REL readers and writers belong to later slices.
    if (!directory && parsed.type == DISKIMAGE_FTYPE_REL) {
        command_error(unit, model, secondary, DOS_STATUS_SYNTAX);
        return;
    }
    if (last_program) {
        drive = unit->last_program_drive;
    }

    // ROM lookup works against DRVNUM. Save that selection and capture the
    // host media identity so deferred work cannot read a replacement image.
    // M2 reports absent media immediately through its NOT_READY policy.
    unit->selected_drive = drive;
    dos_media_ref_t reference;
    if (dos_media_reference(media, index, drive, &reference) != DOS_MEDIA_OK) {
        if (!parsed.default_drive || parsed.access == DOS_ACCESS_WRITE || last_program ||
            dos_media_reference(media, index, drive ^ 1u, &reference) != DOS_MEDIA_OK) {
            command_error(unit, model, secondary, DOS_STATUS_NOT_READY);
            return;
        }
        drive ^= 1u;
        unit->selected_drive = drive;
        parsed.default_drive = false;
        listing.searches = 0;
    }
    if (directory) {
        // DOS 2.7 OPTSCH/OS50 reduces DRVCNT when only this drive is active.
        // No preferred-drive-only pass may hide a default pattern's fallback.
        dos_media_ref_t other;
        if (formatted_directory && listing.searches != 0 &&
            dos_media_reference(media, index, drive ^ 1u, &other) != DOS_MEDIA_OK) {
            listing.searches = 0;
        }
        if (open_directory(unit, media, reference, secondary, &listing)) {
            command_end(unit, model);
        } else {
            clear_command(unit);
        }
        return;
    }
    if (last_program) {
        const uint8_t previous = dos_find_channel(unit, secondary);
        if (previous != DOS_UNUSED && !dos_close(unit, media, model, secondary)) {
            clear_command(unit);
            return;
        }
        unit->selected_drive = reference.slot;
        const uint8_t channel = allocate_channel(unit, reference, secondary, 2);
        if (channel == DOS_UNUSED) {
            command_error(unit, model, secondary, DOS_STATUS_NO_CHANNEL);
        } else if (!load_buffer(unit, media, &unit->channels[channel], unit->last_program_track,
                                unit->last_program_sector)) {
            free_channel(unit, channel);
            command_error(unit, model, secondary, DOS_STATUS_READ_ERROR);
        } else {
            const uint8_t* const bytes = unit->buffers[unit->channels[channel].buffers[0]].bytes;
            if (bytes[0] == 0 && bytes[1] == DISKIMAGE_SECTOR_LINK_SIZE) {
                unit->channels[channel].singleton = 1;
            }
            command_end(unit, model);
        }
        return;
    }

    // ROM lookup:SRCHST opens an internal directory channel at 18/1 or
    // DIRTRK/1. Retain that starting link in a scratch-backed job instead,
    // so lookup can yield without allocating a file channel prematurely.
    const uint8_t scratch = get_buffer(unit), scratch_second = get_buffer(unit);
    if (scratch == DOS_UNUSED || scratch_second == DOS_UNUSED) {
        if (scratch != DOS_UNUSED) {
            unit->buffer_use &= (uint16_t)~(1u << scratch);
        }
        command_error(unit, model, secondary, DOS_STATUS_NO_CHANNEL);
        return;
    }

    // Retain name/type/SA and the starting link for dos_work. Keep scratch
    // ownership across yields until a match or model-specific error cleanup.
    parsed.media = reference;
    parsed.track = directory_track(model);
    parsed.sector = DOS_DIRECTORY_FIRST_SECTOR;
    parsed.secondary = secondary;
    parsed.scratch = scratch;
    parsed.scratch_second = scratch_second;
    parsed.pending = true;
    parsed.creation_drive = drive;
    parsed.first_program = first_program;
    unit->open = parsed;
}

// Select the alternate page, completing a deferred read only when one was submitted.
// Leave its prepared byte/EOI fields intact for the caller's pointer conversion.
static bool pointer_next_buffer(dos_unit_t* const unit, dos_media_bank_t* const media, dos_channel_t* const channel) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
    //
    // RDBYT/RD1 switch through DBLBUF while retaining the old page's last
    // prepared byte in DATA. This host helper has no direct ROM entry: it
    // materializes that switch for the lazy C cursor before applying B-P.
    const uint8_t* const bytes = unit->buffers[channel->buffers[channel->active]].bytes;
    const uint8_t track = channel->pointer_mode ? channel->next_track : bytes[0];
    const uint8_t sector = channel->pointer_mode ? channel->next_sector : bytes[1];
    const unsigned int limit =
        media->units[channel->media.unit].slots[channel->media.slot].io.image.size / DISKIMAGE_SECTOR_SIZE;
    // The visit bound is a host cycle safeguard, including retained-RAM cycles.
    if (channel->traversed >= limit) {
        return false;
    }
    // Read through a tentative cursor so failure does not advance channel
    // state, although load_buffer may still touch the shared page cache.
    dos_channel_t next = *channel;
    ++next.traversed;
    next.active ^= 1u;
    // A block read can replace the active page without changing the already
    // prefetched alternate or its job drive. DBLBUF selects that old job.
    const uint8_t buffer = next.buffers[next.active];
    const uint8_t drive = (unit->buffer_drives >> buffer) & 1u;
    // Both slots share the captured generation. Never renew an old file token.
    next.media.slot = drive;
    // A terminal link submitted no read, but DBLBUF still selects its retained
    // RAM. RD1 interprets that page's header, not the overwritten active link.
    if (track != 0 ? !load_buffer(unit, media, &next, track, sector) : unit->buffers[buffer].failed) {
        return false;
    }
    next.position = DISKIMAGE_SECTOR_LINK_SIZE;
    if (next.pointer_mode) {
        const uint8_t* const page = unit->buffers[buffer].bytes;
        next.read_last = page[0] == 0 ? page[1] : 0;
        next.next_track = page[0];
        next.next_sector = page[1];
    }
    *channel = next;
    return true;
}

// Materialize the native prepared byte, post-GET pointer and active page before edits.
static bool materialize_reader(dos_unit_t* const unit, dos_media_bank_t* const media, dos_channel_t* const channel) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
    //
    // GETBYT/RDBYT advance BUFTAB before CHNDAT is transmitted. GETPNT
    // observes that advanced pointer, including the singleton OPEN wrap.
    // No direct ROM entry: convert the host's lazy transmit cursor to this
    // prepared-byte representation before B-P or a descriptor-changing edit.
    if (channel->pointer_mode) {
        return true;
    }
    const uint8_t* bytes = unit->buffers[channel->buffers[channel->active]].bytes;
    const uint8_t singleton = channel->singleton;
    // Lazy position names the next transmitted byte. GETPNT instead observes
    // BUFTAB after RDBYT has already prepared CHNDAT (and possibly wrapped).
    if (channel->ready && channel->singleton == 0 && channel->position == DISKIMAGE_SECTOR_SIZE) {
        if (!pointer_next_buffer(unit, media, channel)) {
            return false;
        }
        bytes = unit->buffers[channel->buffers[channel->active]].bytes;
    }
    const uint8_t last = bytes[0] == 0 ? bytes[1] : 0;
    if (singleton != 0) {
        static const uint8_t offsets[] = {2, 0, 1, 2};
        const uint8_t offset = offsets[singleton - 1u];
        channel->read_byte = bytes[offset];
        channel->position = offset == 2 ? 0 : offset + 1u;
    } else if (channel->ready) {
        channel->read_byte = bytes[channel->position];
        channel->position = last != 0 && channel->position == last ? 0 : channel->position + 1u;
    } else {
        // GET0 retires readiness but leaves the final prepared byte in CHNDAT.
        channel->read_byte = bytes[(uint8_t)(channel->position - 1u)];
        channel->position = 0;
    }
    channel->read_eoi = channel->ready && (singleton == DOS_SINGLETON_LAST_PHASE ||
                                           (singleton == 0 && last != 0 && channel->position == 0));
    // Native RDBYT has already switched when the old page's byte 255 is prepared.
    if (channel->ready && singleton == 0 && channel->position == DISKIMAGE_SECTOR_SIZE && bytes[0] != 0 &&
        !pointer_next_buffer(unit, media, channel)) {
        return false;
    }
    bytes = unit->buffers[channel->buffers[channel->active]].bytes;
    channel->pointer_mode = true;
    channel->read_last = bytes[0] == 0 ? bytes[1] : 0;
    channel->next_track = bytes[0];
    channel->next_sector = bytes[1];
    return true;
}

// Apply B-P to an ordinary read channel, preserving prepared-byte boundary state.
// Restore the channel cursor and report READ ERROR if successor loading fails.
static void read_pointer_work(dos_unit_t* const unit, dos_media_bank_t* const media) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/block
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/block
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4
    //
    // BLKPTR sets BUFTAB and calls RNGET2 to replace CHNDAT without a GET
    // increment. RNGET2 sets EOI on LSTCHR equality but does not clear an
    // older EOI on inequality. C first materializes the ordinary lazy cursor.
    const dos_block_work_t work = unit->block;
    dos_channel_t* const channel = &unit->channels[work.channel];
    const dos_channel_t before = *channel;
    unit->command = DOS_COMMAND_IDLE;
    if (!materialize_reader(unit, media, channel)) {
        goto read_error;
    }
    // B-P changes the shared pointer and prepared byte, not the underlying
    // page contents. Only last-character equality forces readiness/EOI.
    const uint8_t* const bytes = unit->buffers[channel->buffers[channel->active]].bytes;
    channel->position = work.offset;
    channel->read_byte = bytes[work.offset];
    if (work.offset == channel->read_last) {
        channel->read_eoi = true;
        channel->ready = true;
    }
    command_end(unit, media->units[channel->media.unit].model);
    return;

read_error:
    // Roll back channel metadata, not any shared cache I/O already attempted.
    *channel = before;
    dos_status(unit, media->units[channel->media.unit].model, DOS_STATUS_READ_ERROR, 0, 0);
}

// Read the native active page and prepare B-R/U1 without changing its file type.
// Ordinary GET retains sequential readiness and its already prefetched alternate.
static void block_read_work(dos_unit_t* const unit, dos_media_bank_t* const media) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/block
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/block
    //
    // BLKRD2 reads the sector, then BLKRD3 clears old EOI and takes LSTCHR
    // from byte 0. BLKRD/RNGET1 prepares byte 1, whereas UBLKRD prepares
    // byte 0 and sets LSTCHR=$FF to expose a full raw page.
    const dos_block_work_t work = unit->block;
    const dos_model_t model = media->units[work.media.unit].model;
    dos_channel_t* const channel = &unit->channels[work.channel];
    unit->command = DOS_COMMAND_IDLE;
    if (channel->access != DOS_ACCESS_DIRECT) {
        const dos_channel_t before = *channel;
        if (!materialize_reader(unit, media, channel)) {
            *channel = before;
            dos_status(unit, model, DOS_STATUS_READ_ERROR, 0, 0);
            return;
        }
    }
    dos_buffer_t* const buffer = &unit->buffers[channel->buffers[channel->active]];
    channel->media = work.media;
    unit->selected_drive = work.media.slot;
    // Retire this scheduled operation before synchronous I/O. Failure leaves
    // error reporting to storage_error without publishing successful residency.
    if (!storage_error(unit, media, work.media,
                       read_sector(unit, media, work.media, work.track, work.sector, buffer->bytes), false, work.track,
                       work.sector)) {
        return;
    }
    buffer->track = work.track;
    buffer->sector = work.sector;
    buffer->resident = true;
    buffer->failed = false;
    // Replace, rather than preserve, direct EOI state for the newly read page.
    channel->direct_last = work.raw ? DISKIMAGE_SECTOR_SIZE - 1u : buffer->bytes[0];
    channel->position = work.raw ? 0 : 1;
    channel->direct_byte = buffer->bytes[channel->position];
    channel->direct_eoi = !work.raw && channel->position == channel->direct_last;
    channel->failed = false;
    channel->ready = true;
    command_end(unit, model);
}

// Write the native active page without changing direct/ordinary channel type.
// Report completion only after version checks, storage write, and host flush succeed.
static void block_write_work(dos_unit_t* const unit, dos_media_bank_t* const media) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/block
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/block
    //
    // BLKWT/BW10 replace byte 0 with max(1, pointer-1), leaving the pointer
    // at 1 before DRTWRT. BW20/RNGET2 then refresh CHNDAT. UBLKWT skips
    // count/pointer changes and writes the page as-is. RNGET2 equality also
    // restores ordinary readiness, without changing its sequential file type.
    // Host flushes and media-generation rejection are policy, not ROM fields.
    const dos_block_work_t work = unit->block;
    const dos_model_t model = media->units[work.media.unit].model;
    dos_channel_t* const channel = &unit->channels[work.channel];
    unit->command = DOS_COMMAND_IDLE;
    if (channel->access != DOS_ACCESS_DIRECT) {
        const dos_channel_t before = *channel;
        if (!materialize_reader(unit, media, channel)) {
            *channel = before;
            dos_status(unit, model, DOS_STATUS_READ_ERROR, 0, 0);
            return;
        }
    }
    dos_buffer_t* const buffer = &unit->buffers[channel->buffers[channel->active]];
    unit->selected_drive = work.media.slot;
    if (!work.raw) {
        // These pre-write changes remain even if persistence fails.
        buffer->bytes[0] = channel->position <= DOS_BLOCK_MIN_COUNT ? DOS_BLOCK_MIN_COUNT : channel->position - 1u;
        channel->position = 1;
    }
    // DRTWRT enters SETJOB without WRTAB's earlier LSTJOB update. Reject its
    // version before the common write helper rebinds the page or this channel.
    if (!write_version(unit, media, work.media, work.track, work.sector)) {
        return;
    }
    channel->media = work.media;
    if (!write_sector(unit, media, work.media, work.track, work.sector, buffer->bytes)) {
        return;
    }
    // Revalidate the captured media generation before the host durability
    // boundary, so stale state cannot be acknowledged as completed output.
    dos_media_ref_t current = {0};
    if (dos_media_reference(media, work.media.unit, work.media.slot, &current) != DOS_MEDIA_OK ||
        current.identity != work.media.identity) {
        storage_error(unit, media, work.media, DOS_MEDIA_STALE, true, work.track, work.sector);
        return;
    }
    if (!storage_error(unit, media, work.media, dos_media_flush_unit(media, work.media.unit), true, work.track,
                       work.sector)) {
        return;
    }
    if (!work.raw) {
        // RNGET2 sets EOI on equality without clearing previously sticky EOI.
        channel->direct_byte = buffer->bytes[channel->position];
        if (channel->position == channel->direct_last) {
            channel->direct_eoi = true;
            channel->ready = true;
        }
    }
    command_end(unit, model);
}

// Free a requested BAM sector or advance a B-A allocation/suggestion search.
// Claim only the exact requested free sector, never a suggested alternative.
static void block_map_work(dos_unit_t* const unit, dos_media_bank_t* const media) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/block
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/block
    //
    // BLKFRE calls FRETS and succeeds even for an already-free bit. BLKALC
    // uses AVAIL/AV2 (4040) or GETSEC (DOS 2.7), claiming through USEDTS only
    // if the request itself is free. Otherwise NO BLOCK carries a suggestion.
    dos_block_map_work_t* const work = &unit->block_map;
    const dos_model_t model = media->units[work->media.unit].model;
    unit->selected_drive = work->media.slot;
    uint8_t* const entry = bam_entry(unit, media, work->media, work->track);
    if (entry == NULL) {
        unit->command = DOS_COMMAND_IDLE;
        return;
    }
    if (!work->allocate) {
        // Modify the cached map only. This command does not flush it to disk.
        if (mark_sector(unit, media, work->media, work->track, work->sector, true)) {
            command_end(unit, model);
        }
        unit->command = DOS_COMMAND_IDLE;
        return;
    }
    if (!valid_bitmap(unit, media, work->media, work->track, entry)) {
        unit->command = DOS_COMMAND_IDLE;
        return;
    }
    const diskimage_type_t type = media->units[work->media.unit].slots[work->media.slot].io.image.type;
    unsigned int sectors;
    vet(diskimage_track_sectors(type, work->track, &sectors), "invalid block allocation track: %u", work->track);
    // Search forward on this track without wrapping to earlier sectors.
    for (unsigned int sector = work->sector; sector < sectors; ++sector) {
        if (entry[1u + sector / 8u] & (1u << (sector & 7u))) {
            if (work->suggestion || sector != work->sector) {
                dos_status(unit, model, DOS_STATUS_NO_BLOCK, work->track, sector);
            } else if (mark_sector(unit, media, work->media, work->track, (uint8_t)sector, false)) {
                command_end(unit, model);
            }
            unit->command = DOS_COMMAND_IDLE;
            return;
        }
    }
    // Yield between tracks as a host scheduling adaptation. Once searching
    // alternatives, even a free sector is a suggestion, not an allocation.
    ++work->track;
    work->sector = 0;
    work->suggestion = true;
    if (!diskimage_track_sectors(type, work->track, &sectors)) {
        // Exhaustion reports NO BLOCK with zero track/sector, not a claim.
        dos_status(unit, model, DOS_STATUS_NO_BLOCK, 0, 0);
        unit->command = DOS_COMMAND_IDLE;
    }
}

// Service a queued metadata command, ordinary OPEN or APPEND continuation.
// Directory lookup yields at an unmatched link, then constructs the matched
// channel or reports failure. Initial data jobs may also run on a match.
void dos_work(dos_unit_t* const unit, dos_media_bank_t* const media) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lookup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lookup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/open
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/open
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst3
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst3
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/opchnl
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/opchnl
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4
    //
    // No direct ROM entry for this cooperative dispatcher. Its OPEN path
    // splits FFST/SRCHST/SEARCH and GETRCH/OPNRCH/OPNWCH into retained work.

    // The tag owns transient command state, independent of OPEN's pending flag.
    switch (unit->command) {
    case DOS_COMMAND_RENAME:
        rename_work(unit, media);
        return;
    case DOS_COMMAND_SCRATCH:
        scratch_work(unit, media);
        return;
    case DOS_COMMAND_COPY:
        copy_work(unit, media);
        return;
    case DOS_COMMAND_FORMAT:
        format_work(unit, media);
        return;
    case DOS_COMMAND_VALIDATE:
        validate_work(unit, media);
        return;
    case DOS_COMMAND_BLOCK_READ:
        block_read_work(unit, media);
        return;
    case DOS_COMMAND_BLOCK_WRITE:
        block_write_work(unit, media);
        return;
    case DOS_COMMAND_BLOCK_MAP:
        block_map_work(unit, media);
        return;
    case DOS_COMMAND_READ_POINTER:
        read_pointer_work(unit, media);
        return;
    case DOS_COMMAND_IDLE:
        break;
    }
    if (unit->initialize.pending) {
        initialize_work(unit, media);
        return;
    }
    // ROM lookup:FFST/SRCHST/SEARCH save a match's type and first link.
    // Resume the retained search only when an ordinary OPEN is pending.
    dos_open_work_t* const work = &unit->open;
    if (!work->pending) {
        return;
    }
    // Another channel can report a read error between cooperative work calls.
    unit->selected_drive = work->media.slot;
    if (work->append_channel != 0) {
        append_work(unit, media, work, true);
        return;
    }
    const dos_model_t model = media->units[work->media.unit].model;
    const uint8_t secondary = work->secondary, type = work->type;
    const dos_media_ref_t reference = work->media;
    auto_initialize(unit, reference.slot);
    uint8_t found_track = 0, found_sector = 0, found_type = 0, found_entry = 0;
    bool matched = false;
    uint16_t found_blocks = 0;
    const uint8_t searched_track = work->track, searched_sector = work->sector;

    // Bound directory visits by image size as a corruption safeguard, not a
    // native ROM timeout. Read one sector into the job's retained scratch.
    const unsigned int limit = media->units[reference.unit].slots[reference.slot].io.image.size / DISKIMAGE_SECTOR_SIZE;
    bool failed = work->visited++ >= limit;
    uint8_t* const bytes = unit->buffers[work->scratch].bytes;
    if (directory_growth_pending(unit, reference, searched_track, searched_sector)) {
        dos_status(unit, model, DOS_STATUS_WRITE_ERROR, searched_track, searched_sector);
        work->pending = false;
        clear_command(unit);
        unit->buffer_use &= (uint16_t)~((1u << work->scratch) | (1u << work->scratch_second));
        return;
    }
    if (!failed) {
        failed = read_sector(unit, media, reference, work->track, work->sector, bytes) != DOS_MEDIA_OK;
    }
    if (!failed) {
        // ROM SEARCH steps 32 bytes and COMPAR compares up to 16 name bytes.
        // Retain closed and unclosed matches for OPEN's mode-specific checks.
        for (unsigned int entry = 0; entry < DISKIMAGE_SECTOR_SIZE; entry += DISKIMAGE_DIRECTORY_ENTRY_SIZE) {
            const uint8_t* const file = bytes + entry;
            if (file[DOS_DIRECTORY_TYPE_OFFSET] == 0) {
                if (work->empty_track == 0 && !directory_slot_reserved(unit, reference, searched_sector, entry)) {
                    work->empty_track = searched_track;
                    work->empty_sector = searched_sector;
                    work->empty_entry = (uint8_t)entry;
                }
                // OP0415/FF30 filter initial LOAD '*' before OP110's type check.
            } else if ((!work->first_program ||
                        (file[DOS_DIRECTORY_TYPE_OFFSET] & DOS_FILE_TYPE_MASK) == DISKIMAGE_FTYPE_PRG) &&
                       name_matches(work->name, work->name_length, file + DOS_DIRECTORY_NAME_OFFSET)) {
                found_type = file[DOS_DIRECTORY_TYPE_OFFSET];
                // 4040 OP90/OP130 discard FILTRK's two high bits. DOS 2.7
                // keeps the full track byte for its larger geometry.
                found_track =
                    file[DOS_DIRECTORY_TRACK_OFFSET] & (model == DOS_MODEL_4040 ? DOS_FILE_TRACK_MASK_4040 : UINT8_MAX);
                found_sector = file[DOS_DIRECTORY_SECTOR_OFFSET];
                found_entry = (uint8_t)entry;
                matched = true;
                found_blocks = file[DOS_DIRECTORY_BLOCKS_LOW] | ((uint16_t)file[DOS_DIRECTORY_BLOCKS_HIGH] << 8u);
                break;
            }
        }
        work->last_track = searched_track;
        work->last_sector = searched_sector;
        work->track = bytes[0];
        work->sector = bytes[1];
    }

    // ROM SR40/NXTBUF follow the directory link until its track is zero.
    // Yield at a live link, otherwise return the scratch buffer before any
    // GETRCH-style allocation so lookup does not permanently cost a buffer.
    // FFST returns a zero-track name match to OP90, not another search pass.
    if (!failed && !matched && work->track != 0) {
        return;
    }
    if (!failed && !matched && work->default_drive) {
        unsigned int next_drive = reference.slot;
        bool retry = false;
        if (!work->tried_other) {
            next_drive ^= 1u;
            work->tried_other = true;
            retry = dos_media_reference(media, reference.unit, next_drive, &work->media) == DOS_MEDIA_OK;
        } else if (work->access == DOS_ACCESS_WRITE && reference.slot != work->creation_drive) {
            next_drive = work->creation_drive;
            work->default_drive = false;
            retry = dos_media_reference(media, reference.unit, next_drive, &work->media) == DOS_MEDIA_OK;
        }
        if (retry) {
            unit->selected_drive = (uint8_t)next_drive;
            work->track = directory_track(model);
            work->sector = DOS_DIRECTORY_FIRST_SECTOR;
            work->visited = 0;
            work->empty_track = 0;
            return;
        }
    }
    work->pending = false;
    if (failed ||
        (matched && directory_entry_busy(unit, reference, searched_track, searched_sector, found_entry, NULL))) {
        unit->buffer_use &= (uint16_t)~((1u << work->scratch) | (1u << work->scratch_second));
        const uint8_t owner = dos_find_channel(unit, secondary);
        if (failed && (owner == DOS_UNUSED || unit->channels[owner].access == DOS_ACCESS_READ)) {
            command_error(unit, model, secondary, DOS_STATUS_READ_ERROR);
        } else {
            // Host lookup/retry failures must not discard writable owner state.
            dos_status(unit, model, failed ? DOS_STATUS_READ_ERROR : DOS_STATUS_WRITE_ERROR,
                       failed ? 0 : searched_track, failed ? 0 : searched_sector);
            clear_command(unit);
        }
        return;
    }
    dos_status_code_t error = DOS_STATUS_OK;
    // ROM open:OP95 reports 62 for a missing file. M2 read failures instead
    // become generic 23 with zero track/sector, not full controller recovery.
    if (found_track == 0 && work->access != DOS_ACCESS_WRITE) {
        error = DOS_STATUS_NOT_FOUND;
    }

    // ROM open:OP110/OP115 compare the requested file type and report 64.
    // A zero C type means infer it from the match.
    const uint8_t file_type = found_type & DOS_FILE_TYPE_MASK;
    const bool wildcard = work->wildcard;
    const bool unclosed_write =
        found_track != 0 && !(found_type & DOS_FILE_CLOSED_MASK) && work->access == DOS_ACCESS_WRITE && !wildcard;
    if (found_track != 0) {
        if (work->access == DOS_ACCESS_WRITE && !work->replace && !unclosed_write) {
            error = DOS_STATUS_FILE_EXISTS;
        } else if (work->access == DOS_ACCESS_WRITE && work->replace && file_type == DISKIMAGE_FTYPE_REL &&
                   !unclosed_write) {
            error = DOS_STATUS_TYPE_MISMATCH;
        } else if (!(found_type & DOS_FILE_CLOSED_MASK) && work->access != DOS_ACCESS_MODIFY &&
                   work->access != DOS_ACCESS_WRITE) {
            error = DOS_STATUS_FILE_OPEN;
        } else if (type != 0 && type != file_type && !unclosed_write) {
            error = DOS_STATUS_TYPE_MISMATCH;
        } else if (file_type == DISKIMAGE_FTYPE_REL) {
            error = DOS_STATUS_TYPE_MISMATCH;
        }
    }
    if (work->access == DOS_ACCESS_WRITE && wildcard) {
        if (!work->replace) {
            error = DOS_STATUS_INVALID_NAME;
        } else if (found_track == 0) {
            error = DOS_STATUS_TYPE_MISMATCH;
        }
    }
    if (error == DOS_STATUS_OK && (work->access == DOS_ACCESS_WRITE || work->access == DOS_ACCESS_APPEND) &&
        media->units[reference.unit].slots[reference.slot].io.image.write == NULL) {
        error = DOS_STATUS_WRITE_PROTECT;
    }
    if (error != DOS_STATUS_OK) {
        unit->buffer_use &= (uint16_t)~((1u << work->scratch) | (1u << work->scratch_second));
        command_error(unit, model, secondary, error);
        return;
    }
    if (work->access == DOS_ACCESS_WRITE || work->access == DOS_ACCESS_APPEND) {
        const uint8_t* const name = found_track != 0 ? bytes + found_entry + DOS_DIRECTORY_NAME_OFFSET : work->name;
        const dos_channel_t* const owner = unpublished_writer(unit, reference, name, secondary);
        if (owner != NULL) {
            // A failed publication still owns its planned namespace entry.
            unit->buffer_use &= (uint16_t)~((1u << work->scratch) | (1u << work->scratch_second));
            dos_status(unit, model, DOS_STATUS_WRITE_ERROR, owner->directory_track, owner->directory_sector);
            clear_command(unit);
            return;
        }
    }
    const uint8_t previous = dos_find_channel(unit, secondary);
    if (previous != DOS_UNUSED && unit->channels[previous].access != DOS_ACCESS_READ &&
        unit->channels[previous].access != DOS_ACCESS_DIRECT) {
        if (!finish_channel(unit, media, &unit->channels[previous])) {
            unit->buffer_use &= (uint16_t)~((1u << work->scratch) | (1u << work->scratch_second));
            clear_command(unit);
            return;
        }
        free_channel(unit, previous);
        // Host retirement can publish entries and free chains. Discard all
        // lookup observations before any DELDIR, reservation or allocation.
        if (work->default_drive || work->tried_other) {
            work->default_drive = true;
            work->tried_other = false;
            dos_media_ref_t preferred;
            if (dos_media_reference(media, reference.unit, work->creation_drive, &preferred) == DOS_MEDIA_OK) {
                work->media = preferred;
            }
        }
        work->track = directory_track(model);
        work->sector = DOS_DIRECTORY_FIRST_SECTOR;
        work->visited = 0;
        work->empty_track = 0;
        work->pending = true;
        return;
    }
    if (unclosed_write) {
        // OP77/DELDIR removes only the entry, deliberately not the abandoned chain.
        bytes[found_entry + DOS_DIRECTORY_TYPE_OFFSET] = DISKIMAGE_FTYPE_DEL;
        if (!write_sector(unit, media, reference, searched_track, searched_sector, bytes)) {
            unit->buffer_use &= (uint16_t)~((1u << work->scratch) | (1u << work->scratch_second));
            clear_command(unit);
            return;
        }
        if (work->empty_track == 0) {
            work->empty_track = searched_track;
            work->empty_sector = searched_sector;
            work->empty_entry = found_entry;
        }
        if (work->type == 0) {
            work->type = file_type;
        }
        found_track = 0;
    }
    // ROM tst3:GETRCH first calls FRECHN, then FNDLNX picks the lowest free
    // logical index and GETBUF claims the requested two buffers. Preserve
    // that order after successful lookup, rolling back partial buffer claims
    // on exhaustion. Only ordinary indices participate in this scan.
    if (previous != DOS_UNUSED) {
        free_channel(unit, previous);
    }
    unit->selected_drive = reference.slot;
    const uint8_t channel = allocate_channel(unit, reference, secondary, 2);
    if (channel == DOS_UNUSED) {
        unit->buffer_use &= (uint16_t)~((1u << work->scratch) | (1u << work->scratch_second));
        command_error(unit, model, secondary, DOS_STATUS_NO_CHANNEL);
        return;
    }

    // ROM OPNRCH/INTPNT select BUF0, initialize both data pointers to 2, and
    // STRDBL reads the initial sector (and prefetches a successor if present).
    // Retain the prefetched successor even if INITDR abandons this reader.
    // A later writer can expose those bytes in its unused sector tail.
    // Failure of the first read releases this newly allocated channel.
    dos_channel_t* const state = &unit->channels[channel];
    state->directory_track = searched_track;
    state->directory_sector = searched_sector;
    state->directory_entry = found_entry;
    state->file_type = file_type;
    state->first_track = found_track;
    state->first_sector = found_sector;
    state->blocks = found_blocks;
    unit->buffer_use &= (uint16_t)~((1u << work->scratch) | (1u << work->scratch_second));
    if (work->access == DOS_ACCESS_WRITE) {
        state->blocks = 0;
        if (!open_write(unit, media, state, work, found_track != 0)) {
            state->failed = true;
            if (state->first_track == 0 || unit->status_code == DOS_STATUS_DISK_FULL) {
                free_channel(unit, channel);
            }
            clear_command(unit);
            return;
        }
        if (secondary < 2) {
            unit->last_program_track = state->first_track;
            unit->last_program_sector = state->first_sector;
            unit->last_program_drive = reference.slot;
        }
        command_end(unit, model);
        return;
    }
    if (!load_buffer(unit, media, state, found_track, found_sector)) {
        free_channel(unit, channel);
        command_error(unit, model, secondary, DOS_STATUS_READ_ERROR);
        return;
    }
    const uint8_t* const first = unit->buffers[state->buffers[0]].bytes;
    if (first[0] != 0) {
        dos_buffer_t* const alternate = &unit->buffers[state->buffers[1]];
        alternate->track = first[0];
        alternate->sector = first[1];
        alternate->failed = read_sector(unit, media, reference, first[0], first[1], alternate->bytes) != DOS_MEDIA_OK;
        alternate->resident = true;
        // STRDBL submits the alternate job without waiting for it. Report
        // its failure only when RDBYT/DBLBUF actually selects that buffer.
    }

    // ROM OPNRCH saves the final-sector marker in LSTCHR, then OR30 calls
    // RDBYT and unconditionally overwrites CHNRDY with RDYTLK. For a one-byte
    // first sector (link 0/2), that loses the just-prepared EOI. Remember this
    // special initial state so next_byte reproduces it rather than "fixing"
    // the ROM. Other files use the ordinary payload cursor.
    if (work->access == DOS_ACCESS_APPEND) {
        state->access = DOS_ACCESS_APPEND;
        state->ready = false;
        state->blocks = 1;
        work->append_channel = channel + 1u;
        work->pending = true;
        return;
    }
    const uint8_t* const initial = unit->buffers[state->buffers[0]].bytes;
    if (state->access == DOS_ACCESS_READ && initial[0] == 0 && initial[1] == DISKIMAGE_SECTOR_LINK_SIZE) {
        state->singleton = 1;
    }
    if (secondary < 2) {
        unit->last_program_track = found_track;
        unit->last_program_sector = found_sector;
        unit->last_program_drive = reference.slot;
    }
    command_end(unit, model);
}

// GETNAM/BLKNB construct one native 27-byte, PETSCII directory name field.
static void directory_name(const uint8_t* const entry, uint8_t* const name) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/trnsfr
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/trnsfr
    //
    // GETNAM adjusts NAMBUF spacing for the decimal block count before
    // adding type text, '<' for locked files, and '*' for unclosed files.
    static const char types[][4] = {"DEL", "SEQ", "PRG", "USR", "REL"};
    memset(name, ' ', DOS_DIRECTORY_NAME_SIZE);
    const unsigned int blocks =
        entry[DOS_DIRECTORY_BLOCKS_LOW] | ((unsigned int)entry[DOS_DIRECTORY_BLOCKS_HIGH] << 8u);
    const unsigned int end = blocks < 10u ? 24u : blocks < 100u ? 23u : 22u;
    const uint8_t type = entry[DOS_DIRECTORY_TYPE_OFFSET];
    const unsigned int file_type = type & DOS_FILE_TYPE_MASK;
    memcpy(name + end - 2u, types[file_type < count_of(types) ? file_type : 0], 3);
    if (type & DOS_FILE_LOCKED_MASK) {
        name[end + 1u] = '<';
    }
    if (!(type & DOS_FILE_CLOSED_MASK)) {
        name[end - 3u] = '*';
    }
    // Copy all 16 PETSCII name bytes without folding case. GETNAM terminates
    // the quoted name at the first quote or $A0, then clears trailing bit 7.
    const unsigned int quote = end - 21u;
    name[quote] = '"';
    memcpy(name + quote + 1u, entry + DOS_DIRECTORY_NAME_OFFSET, DISKIMAGE_FILENAME_SIZE);
    name[end - 4u] = DISKIMAGE_FILENAME_PADDING;
    unsigned int index = quote + 1u;
    while (index < DOS_DIRECTORY_NAME_SIZE && name[index] != '"' && name[index] != DISKIMAGE_FILENAME_PADDING) {
        ++index;
    }
    if (index < DOS_DIRECTORY_NAME_SIZE) {
        name[index++] = '"';
    }
    for (; index < DOS_DIRECTORY_NAME_SIZE; ++index) {
        name[index] &= 0x7fu;
    }
}

// Prepare at most one directory sector per call, retaining any unconsumed row.
static bool prepare_directory_line(dos_unit_t* const unit, dos_media_bank_t* const media,
                                   dos_channel_t* const channel) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/trnsfr
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/trnsfr
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lstdir
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lstdir
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lookup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lookup
    //
    // NEWDIR, GETNAM, and MSGFRE supply header, file, and free-block text.
    // STDIR/DIR1/DIR3 wrap those fields as BASIC lines. C retains one row
    // at a time and yields between directory sectors rather than filling a
    // ROM listing buffer across several entries.
    const dos_model_t model = media->units[channel->media.unit].model;
    uint8_t* const line = unit->buffers[channel->buffers[0]].bytes;
    dos_buffer_t* const source = &unit->buffers[channel->buffers[1]];
    uint8_t name[DOS_DIRECTORY_NAME_SIZE];
    uint16_t number = 0;
    unsigned int prefix = 0;
    channel->line_eoi = false;
    if (channel->directory == DOS_DIRECTORY_HEADER) {
        // NEWDIR quotes the disk name and substitutes header $A0 padding.
        // Only the first drive header carries the BASIC load address $0401.
        const uint8_t track = directory_track(model);
        if (!storage_error(unit, media, channel->media,
                           read_sector(unit, media, channel->media, track, 0, source->bytes), false, track, 0)) {
            channel->ready = false;
            return false;
        }
        source->resident = false;
        memset(name, ' ', sizeof(name));
        const unsigned int offset = model == DOS_MODEL_4040 ? DOS_HEADER_NAME_4040 : DOS_HEADER_NAME_8050;
        for (unsigned int index = 0; index < 23u; ++index) {
            const uint8_t byte = source->bytes[offset + index];
            name[index + 2u] = byte == DISKIMAGE_FILENAME_PADDING ? (index == 22u ? '1' : ' ') : byte;
        }
        name[0] = 0x12;
        name[1] = name[18] = '"';
        name[19] = ' ';
        number = channel->media.slot;
        if (channel->singleton == 0) {
            line[0] = 1;
            line[1] = 4;
            prefix = 2;
            channel->singleton = 1;
        }
        channel->directory = DOS_DIRECTORY_ENTRIES;
    } else if (channel->directory == DOS_DIRECTORY_ENTRIES) {
        // Retain the directory sector while scanning its slots. Pattern
        // drive/type filtering corresponds to lookup's COMPAR/FNDFIL work.
        // Count each sector visit once, including a cached self-link, not each row.
        const unsigned int limit =
            media->units[channel->media.unit].slots[channel->media.slot].io.image.size / DISKIMAGE_SECTOR_SIZE;
        if (channel->directory_entry == 0 && channel->traversed++ >= limit) {
            unit->selected_drive = channel->media.slot;
            dos_status(unit, model, DOS_STATUS_READ_ERROR, channel->directory_track, channel->directory_sector);
            channel->ready = false;
            return false;
        }
        if (!source->resident || source->track != channel->directory_track ||
            source->sector != channel->directory_sector) {
            if (!storage_error(unit, media, channel->media,
                               read_sector(unit, media, channel->media, channel->directory_track,
                                           channel->directory_sector, source->bytes),
                               false, channel->directory_track, channel->directory_sector)) {
                channel->ready = false;
                return false;
            }
            source->track = channel->directory_track;
            source->sector = channel->directory_sector;
            source->resident = true;
        }
        bool found = false;
        while (channel->directory_entry != DOS_UNUSED && directory_patterns_pending(unit, channel)) {
            const uint8_t* const entry = source->bytes + channel->directory_entry;
            channel->directory_entry =
                channel->directory_entry == DISKIMAGE_SECTOR_SIZE - DISKIMAGE_DIRECTORY_ENTRY_SIZE
                    ? DOS_UNUSED
                    : channel->directory_entry + DISKIMAGE_DIRECTORY_ENTRY_SIZE;
            const uint8_t type = entry[DOS_DIRECTORY_TYPE_OFFSET] & DOS_FILE_TYPE_MASK;
            bool matches = false;
            for (unsigned int index = channel->pattern_count; index-- > 0;) {
                uint8_t* const pattern = unit->buffers[channel->buffers[2]].bytes + index * DOS_DIRECTORY_PATTERN_SIZE;
                const uint8_t drive = pattern[DOS_DIRECTORY_PATTERN_DRIVE];
                if (entry[DOS_DIRECTORY_TYPE_OFFSET] == 0 ||
                    (pattern[DOS_DIRECTORY_PATTERN_FLAGS] & DOS_PATTERN_COMPLETE) ||
                    ((drive & DOS_PATTERN_DRIVE_MASK) != channel->media.slot &&
                     (!(drive & DOS_PATTERN_DEFAULT_DRIVE) ||
                      channel->directory_searches == DOS_DIRECTORY_DEFAULT_BOTH_SEARCHES)) ||
                    !name_matches(pattern, pattern[DOS_DIRECTORY_PATTERN_LENGTH], entry + DOS_DIRECTORY_NAME_OFFSET)) {
                    continue;
                }
                // CP40 records an exact match before FF30's type restriction.
                // Zero FILTRK stays pending. 4040 also retains its high bit
                // as wildcard state across subsequent matches.
                const uint8_t track = entry[DOS_DIRECTORY_TRACK_OFFSET];
                if (model == DOS_MODEL_4040 && (track & DOS_PATTERN_TRACK_WILDCARD)) {
                    pattern[DOS_DIRECTORY_PATTERN_FLAGS] |= DOS_PATTERN_WILDCARD;
                }
                if (!(pattern[DOS_DIRECTORY_PATTERN_FLAGS] & DOS_PATTERN_WILDCARD) && track != 0) {
                    pattern[DOS_DIRECTORY_PATTERN_FLAGS] |= DOS_PATTERN_COMPLETE;
                }
                pattern[DOS_DIRECTORY_PATTERN_DRIVE] = (drive & DOS_PATTERN_DEFAULT_DRIVE) | channel->media.slot;
                matches = true;
                break;
            }
            if (entry[DOS_DIRECTORY_TYPE_OFFSET] != 0 && (channel->file_type == 0 || type == channel->file_type) &&
                matches) {
                directory_name(entry, name);
                number = entry[DOS_DIRECTORY_BLOCKS_LOW] | ((uint16_t)entry[DOS_DIRECTORY_BLOCKS_HIGH] << 8u);
                found = true;
                if (!directory_patterns_pending(unit, channel)) {
                    channel->directory = DOS_DIRECTORY_FOOTER;
                }
                break;
            }
        }
        if (!found) {
            // Yield even at the terminal link. The next call formats MSGFRE.
            channel->directory_track = source->bytes[0];
            channel->directory_sector = source->bytes[1];
            channel->directory_entry = 0;
            if (channel->directory_track == 0 || !directory_patterns_pending(unit, channel)) {
                channel->directory = DOS_DIRECTORY_FOOTER;
            }
            return false;
        }
    } else if (channel->directory == DOS_DIRECTORY_FOOTER) {
        // A surviving listing cannot refill or recount a formatter-owned BAM.
        if (unit->command == DOS_COMMAND_FORMAT && channel->media.unit == unit->format.media.unit &&
            (channel->media.slot == unit->format.media.slot ||
             (unit->format.duplicate && channel->media.slot == unit->format.source.slot))) {
            return false;
        }
        if (!free_blocks(unit, media, channel->media, &number)) {
            channel->ready = false;
            return false;
        }
        memset(name, ' ', sizeof(name));
        memcpy(name, "BLOCKS FREE.", sizeof("BLOCKS FREE.") - 1u);
        dos_media_ref_t other;
        if (channel->directory_searches != 0 && directory_patterns_pending(unit, channel) &&
            dos_media_reference(media, channel->media.unit, channel->media.slot ^ 1u, &other) == DOS_MEDIA_OK) {
            // Continue with the peer's header after this drive's footer,
            // without repeating the load address or ending the stream.
            other.identity = channel->media.identity;
            channel->media = other;
            unit->selected_drive = other.slot;
            auto_initialize(unit, other.slot);
            --channel->directory_searches;
            channel->directory = DOS_DIRECTORY_HEADER;
            channel->directory_track = directory_track(model);
            channel->directory_sector = DOS_DIRECTORY_FIRST_SECTOR;
            channel->directory_entry = 0;
            channel->traversed = 0;
            source->resident = false;
        } else {
            // DIR3 terminates the BASIC listing with zero bytes and EOI.
            channel->directory = DOS_DIRECTORY_END;
            channel->line_eoi = true;
        }
    } else {
        return false;
    }
    // STDIR uses synthetic $0101 links and little-endian line numbers.
    // The header/footer shorten the text field to keep each row at 32 bytes.
    line[prefix] = line[prefix + 1u] = 1;
    line[prefix + 2u] = (uint8_t)number;
    line[prefix + 3u] = (uint8_t)(number >> 8u);
    const unsigned int name_length =
        channel->line_eoi || prefix != 0 ? DOS_DIRECTORY_NAME_SIZE - 2u : DOS_DIRECTORY_NAME_SIZE;
    memcpy(line + prefix + 4u, name, name_length);
    memset(line + prefix + 4u + name_length, 0, DOS_DIRECTORY_LINE_SIZE - prefix - 4u - name_length);
    channel->position = 0;
    channel->line_length = DOS_DIRECTORY_LINE_SIZE;
    return true;
}

// Directory preview ends at a retained row, so a cancelled suffix cannot be evicted.
static unsigned int directory_stage(dos_unit_t* const unit, dos_media_bank_t* const media, dos_channel_t* const channel,
                                    uint8_t* const output, const unsigned int capacity, bool* const eoi) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lstdir
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lstdir
    //
    // GETDIR advances listing bytes as they are consumed. FIFO preview is
    // a host adaptation: prepare a new row only after dos_get consumed the
    // previous one, and leave its unread suffix resident across interruptions.
    *eoi = false;
    if (!channel->ready ||
        (channel->position == channel->line_length && !prepare_directory_line(unit, media, channel))) {
        return 0;
    }
    unsigned int count = channel->line_length - channel->position;
    if (count > capacity) {
        count = capacity;
    }
    memcpy(output, unit->buffers[channel->buffers[0]].bytes + channel->position, count);
    // Only a preview reaching the final row's end carries listing EOI.
    *eoi = channel->line_eoi && channel->position + count == channel->line_length;
    return count;
}

// Produce one ordinary-file byte and its EOI, advancing the supplied cursor.
// Return false when not ready, exhausted, or unable to read the next byte.
static bool next_byte(dos_unit_t* const unit, dos_media_bank_t* const media, dos_channel_t* const channel,
                      uint8_t* const byte, bool* const eoi) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/opchnl
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/opchnl
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
    //
    // ROM tst4:GET0 makes a channel NOTRDY after its EOI byte is sent.
    // The C ready flag likewise prevents subsequent reads after completion.
    if (!channel->ready) {
        return false;
    }
    const uint8_t* bytes = unit->buffers[channel->buffers[channel->active]].bytes;
    if (channel->access == DOS_ACCESS_DIRECT) {
        // tst4:GET/RNDGET retain direct readiness after EOI and skip byte zero
        // when the pointer reaches LSTCHR. PUT and GET share this cursor.
        *byte = channel->direct_byte;
        *eoi = channel->direct_eoi;
        if (channel->secondary == IEEE_LOAD_CHANNEL && !*eoi) {
            // GET1 dispatches SA 0 through SEQGET/RDBYT before testing type.
            // GETBYT reads before incrementing, so the prepared byte repeats.
            // After EOI, GET00 instead takes the ordinary direct RNDGET path.
            channel->direct_byte = bytes[channel->position];
            channel->direct_eoi = channel->position == channel->direct_last;
            channel->position = channel->direct_eoi ? 0 : (uint8_t)(channel->position + 1u);
            return true;
        }
        if (*eoi) {
            channel->direct_eoi = false;
        }
        channel->position = channel->position == channel->direct_last ? 1 : (uint8_t)(channel->position + 1u);
        channel->direct_byte = bytes[channel->position];
        if (channel->position == channel->direct_last) {
            channel->direct_eoi = true;
        }
        return true;
    }
    if (channel->pointer_mode) {
        if (channel->failed) {
            return false;
        }
        *byte = channel->read_byte;
        *eoi = channel->read_eoi;
        if (*eoi) {
            channel->ready = false;
            return true;
        }
        // SEQGET rereads the selected byte. GETBYT wraps at LSTCHR or 255,
        // and RDBYT prepares EOI or switches pages before this byte is sent.
        const uint8_t last = channel->read_last;
        channel->read_byte = bytes[channel->position];
        channel->position = last != 0 && channel->position == last ? 0 : (uint8_t)(channel->position + 1u);
        if (channel->position == 0) {
            if (last != 0) {
                channel->read_eoi = true;
            } else if (!pointer_next_buffer(unit, media, channel)) {
                channel->failed = true;
            }
        }
        return true;
    }

    // ROM opchnl:OPNRCH/OR30 loses the initial one-byte EOF: RDBYT reads
    // offset 2 and wraps BUFTAB on equality with LSTCHR, but OR30 overwrites
    // its EOI state. Later reads expose offsets 0, 1, and 2, finally with EOI.
    // Reproduce payload, 0, 2, payload only for a final first-sector link 0/2.
    if (channel->singleton != 0) {
        static const uint8_t offsets[] = {2, 0, 1, 2};
        *byte = bytes[offsets[channel->singleton - 1]];
        *eoi = channel->singleton == count_of(offsets);
        if (*eoi) {
            channel->ready = false;
        } else {
            ++channel->singleton;
        }
        return true;
    }

    // ROM tst4:GET/SEQGET calls tst2:RDBYT. GETBYT advances BUFTAB, while
    // RDBYT/DBLBUF switch and refill buffers at a sector boundary. Here
    // position 256 selects the other owned buffer and follows the old link,
    // with load_buffer restarting the payload at 2. The traversed-sector
    // bound and failed-read returns are M2 safeguards, not retry emulation.
    if (channel->position == DISKIMAGE_SECTOR_SIZE) {
        if (bytes[0] == 0 ||
            channel->traversed++ >=
                media->units[channel->media.unit].slots[channel->media.slot].io.image.size / DISKIMAGE_SECTOR_SIZE) {
            return false;
        }
        const uint8_t track = bytes[0], sector = bytes[1];
        channel->active ^= 1u;
        if (!load_buffer(unit, media, channel, track, sector)) {
            return false;
        }
        bytes = unit->buffers[channel->buffers[channel->active]].bytes;
    }

    // ROM RDBYT records LSTCHR when the next-track byte is zero and marks
    // that last byte EOIOUT. Byte 1 is the last occupied offset, not a payload
    // count. Emit EOI when advancing beyond it, then retire C readiness.
    if (bytes[0] == 0 && channel->position > bytes[1]) {
        return false;
    }
    *byte = bytes[channel->position++];
    *eoi = bytes[0] == 0 && channel->position > bytes[1];
    if (*eoi) {
        channel->ready = false;
    }
    return true;
}

// Preview a bounded transmit prefix without committing the channel's GET cursor.
// Stop at capacity or EOI. Report a read error if a ready channel yields no byte.
unsigned int dos_stage(dos_unit_t* const unit, dos_media_bank_t* const media, const uint8_t channel,
                       uint8_t* const output, const unsigned int capacity, bool* const eoi) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4
    //
    // ROM CHNDAT holds a prepared byte, advanced by tst4:GET as IEEE transfer
    // progresses. A FIFO burst may contain an unconsumed suffix after TALK
    // interruption, so preview must not commit that suffix as ROM GET progress.
    // The burst limit also bounds preview's use of the two shared buffer caches.
    vet(channel < DOS_FILE_CHANNEL_COUNT && capacity <= DOS_STAGE_CAPACITY, "invalid native staging window");
    *eoi = false;
    if (capacity == 0) {
        return 0;
    }
    if (unit->channels[channel].directory != DOS_DIRECTORY_NONE) {
        return directory_stage(unit, media, &unit->channels[channel], output, capacity, eoi);
    }

    // Advance a copy through next_byte until capacity or EOI. Cache fills may
    // occur, but the real position/active/ready stay unchanged on success.
    // dos_get later commits only the prefix actually taken from the FIFO.
    dos_channel_t cursor = unit->channels[channel];
    *eoi = false;
    unsigned int length = 0;
    while (length < capacity && next_byte(unit, media, &cursor, output + length, eoi)) {
        ++length;
        if (*eoi) {
            break;
        }
    }

    // M2 retires a ready channel and publishes generic 23 if no byte could be
    // staged. Failure after a nonempty prefix returns that prefix without EOI,
    // deferring the error until progress reaches it, unlike full ROM ERROR.
    if (length == 0 && unit->channels[channel].ready) {
        unit->channels[channel].ready = false;
        unit->selected_drive = cursor.media.slot;
        dos_status(unit, media->units[cursor.media.unit].model, DOS_STATUS_READ_ERROR, 0, 0);
    }
    return length;
}

// Commit ordinary-channel progress observed in the hardware FIFO GET count.
// The caller must supply only consumed bytes from a previously staged prefix.
void dos_get(dos_unit_t* const unit, dos_media_bank_t* const media, const uint8_t channel, const unsigned int count) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4
    //
    // ROM tst4:GET advances after CHNDAT is taken, preparing the next byte
    // or clearing readiness after EOI. Replay next_byte against the real
    // channel to commit position, buffer switches, and readiness for count.
    // Bytes and EOI are already staged, so no second output copy is needed.
    vet(channel < DOS_FILE_CHANNEL_COUNT, "invalid native GET channel");
    dos_channel_t* const state = &unit->channels[channel];
    if (state->directory != DOS_DIRECTORY_NONE) {
        vet(count <= (unsigned int)state->line_length - state->position, "directory GET exceeded its staged row");
        state->position += count;
        if (state->line_eoi && state->position == state->line_length) {
            state->ready = false;
        }
        return;
    }
    for (unsigned int index = 0; index < count; ++index) {
        uint8_t byte;
        bool eoi;
        // Exceeding the staged prefix violates the transport/core contract.
        // Preparing or cancelling unconsumed FIFO bytes must not commit them.
        vet(next_byte(unit, media, &unit->channels[channel], &byte, &eoi), "native GET exceeded its staged prefix");
    }
}

// Preview the retained error-channel suffix, including CR and its EOI.
// Do not consume status or clear the error when bytes are merely queued.
unsigned int dos_status_stage(const dos_unit_t* const unit, uint8_t* const output, const unsigned int capacity,
                              bool* const eoi) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4
    //
    // ROM tst4:GETERC/GE10 presents ERRBUF text via CHNDAT. Copy the retained
    // C suffix, bounded by capacity, without changing status_position so a
    // partial or cancelled TALK preserves its still-unconsumed status.
    unsigned int length = unit->status_length - unit->status_position;
    if (length > capacity) {
        length = capacity;
    }
    memcpy(output, unit->status + unit->status_position, length);
    // ROM GETERC substitutes terminal CR with EOIOUT. The C record already
    // contains CR, so flag EOI only when preview reaches the record's end.
    *eoi = unit->status_position + length == unit->status_length;
    return length;
}

// Acknowledge consumed error-channel bytes and reset status at the CR boundary.
// Partial consumption neither clears the error nor restarts the old record.
void dos_status_get(dos_unit_t* const unit, const dos_model_t model, const unsigned int count) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst4
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst4
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/erproc
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/erproc
    //
    // ROM tst4:GETERC recognizes the terminal pointer, selects CR, calls
    // ERROFF and erproc:ERRTS0 with zero, then presents CR with EOIOUT.
    // The FIFO adaptation keeps the old response until actual GET includes
    // that staged CR. Validate and advance only the consumed prefix.
    vet(count <= (unsigned int)unit->status_length - unit->status_position,
        "native status GET exceeded its staged prefix");
    unit->status_position += count;

    // ROM ERRTS0 clears track/sector and formats the next OK response.
    // Publish that model-specific record only after CR consumption. The
    // ROM's LED manipulation is outside this retained-status implementation.
    if (unit->status_position == unit->status_length) {
        dos_status(unit, model, DOS_STATUS_OK, 0, 0);
    }
}

// Expose retained OPEN/metadata scheduling state without advancing any work.
bool dos_work_pending(const dos_unit_t* const unit) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lookup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lookup
    //
    // Host-only scheduler predicate for split directory searches and commands.
    // There is no separate ROM entry or controller-job emulation here.
    return unit->open.pending || unit->initialize.pending || unit->command != DOS_COMMAND_IDLE;
}

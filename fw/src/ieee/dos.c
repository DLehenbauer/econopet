// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "dos.h"

#include <stdio.h>
#include <string.h>

#include "fatal.h"
#include "ieee/ieee_protocol.h"

// Read/status foundation of the replacement DOS. The production facade still
// selects the legacy engine. Writes, commands, direct channels and REL are not
// enabled here. Sector callbacks and cooperative OPEN/preview are host
// adaptations, not emulated controller jobs or electrical IEEE handshakes.
//
// The selected ROMs are 4040 revision 2 and shared DOS 2.7 in 8050 MPI or 8250
// MPI mode. Source links below are pinned to cbmsrc
// 01bd60f162ef92212ef0cb67546ae8f42be34168. See docs/dev/drive-read-status.md.

_Static_assert(sizeof(dos_unit_t) <= 4096 + DOS_MEDIA_SLOTS_PER_UNIT * DISKIMAGE_SECTOR_SIZE,
               "native unit state exceeds its native budget plus two BAM sectors");
_Static_assert(DOS_FILE_CHANNEL_COUNT + 2 == DOS_CHANNEL_COUNT, "command and error channels remain reserved");

static uint8_t allocate_channel(dos_unit_t* const unit, const dos_media_ref_t reference, const uint8_t secondary,
                                const unsigned int buffers);

static unsigned int directory_stage(dos_unit_t* const unit, dos_media_bank_t* const media, dos_channel_t* const channel,
                                    uint8_t* const output, const unsigned int capacity, bool* const eoi);

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
#define DOS_DIRECTORY_BLOCKS_LOW 30u
#define DOS_DIRECTORY_BLOCKS_HIGH 31u
#define DOS_BAM_TRACK_8050 38u
#define DOS_BAM_TRACKS_PER_PART 50u
#define DOS_DIRECTORY_LINE_SIZE 32u
#define DOS_DIRECTORY_NAME_SIZE 27u
#define DOS_DIRECTORY_PATTERN_SIZE (DISKIMAGE_FILENAME_SIZE + 1u)
#define DOS_HEADER_NAME_4040 144u
#define DOS_HEADER_NAME_8050 6u

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
    // BCDDEC/HEXDEC convert numbers. DOS 2.7 appends DRVNUM masked to one bit,
    // whereas 4040 ends after the sector. tst4:GETERC supplies CR and EOI.
    // Here snprintf combines formatting and that eventual CR in one record,
    // preserving the model-specific drive field and using expanded status_text.
    // This handles M2's bounded fields, not all out-of-range conversion quirks.
    const int length = model == DOS_MODEL_4040
                           ? snprintf(unit->status, sizeof(unit->status), "%02u,%s,%02u,%02u\r", code,
                                      status_text(code, model), track, sector)
                           : snprintf(unit->status, sizeof(unit->status), "%02u,%s,%02u,%02u,%u\r", code,
                                      status_text(code, model), track, sector, unit->selected_drive);
    // Detect internal overflow rather than publishing a truncated status.
    vet(length > 0 && (size_t)length < sizeof(unit->status), "native status overflow");

    // ROM error-buffer pointer/last-character state becomes a retained length
    // and unread position. Replacing the record restarts it without GET progress.
    unit->status_length = (uint8_t)length;
    unit->status_position = 0;
    unit->status_code = code;
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
    memset(unit->command_bytes, 0, DOS_COMMAND_LIMIT);
}

// Retire a failed command without changing its last-successful drive.
static void command_error(dos_unit_t* const unit, const dos_model_t model, const dos_status_code_t code) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/erproc
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/erproc
    //
    // CMDERR/CMDER3 publish the error and call CLRCB without updating LSTDRV.
    // Keep dos_status itself independent of command retirement: an older
    // reader can report an error while a different OPEN is pending.
    dos_status(unit, model, code, 0, 0);
    memset(unit->command_bytes, 0, DOS_COMMAND_LIMIT);
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
bool dos_close(dos_unit_t* const unit, const dos_model_t model, const uint8_t secondary) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/close
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/close
    //
    // ROM close:CLS10/CLSCHN close one ordinary channel. CLSALL iterates the
    // data secondaries for SA 15. Select the matching allocated C record or
    // all data records, then release their resources through free_channel.
    // Missing records need no release. Command/error storage is not released.
    // This slice owns only readers, so no persistence work is needed.
    uint8_t channel;
    while ((channel = close_channel(unit, secondary)) != DOS_UNUSED) {
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
            uint8_t bytes[DISKIMAGE_SECTOR_SIZE];
            if (!storage_error(unit, media, reference,
                               read_sector(unit, media, reference, bam_track, bam_sector, bytes), false, bam_track,
                               bam_sector)) {
                return NULL;
            }
            // Preserve the old cache on failure, publishing only a full read.
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
static bool name_matches(const uint8_t* const pattern, const uint8_t* const name) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lookup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lookup
    //
    // COMPAR operates on fixed-width PETSCII names without case folding.
    // '*' accepts the remaining suffix, while '?' cannot extend past $A0.
    for (unsigned int index = 0; index < DISKIMAGE_FILENAME_SIZE; ++index) {
        if (pattern[index] == '*') {
            return true;
        }
        if (pattern[index] == '?' && name[index] != DISKIMAGE_FILENAME_PADDING) {
            continue;
        }
        if (pattern[index] != name[index]) {
            return false;
        }
        // Equal padding terminates an exact match before the 16-byte limit.
        if (name[index] == DISKIMAGE_FILENAME_PADDING) {
            return true;
        }
    }
    return true;
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
    work->default_drive = true;
    unsigned int commas = 0;
    for (unsigned int index = 0; index < length && commas < DOS_FILENAME_COUNT - 1u; ++index) {
        commas += input[index] == ',';
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

    // ROM lookup:COMPAR compares up to 16 name bytes and accepts $A0 as the
    // directory terminator. Pad the C name with $A0 and copy exact input bytes,
    // with no case folding or PETSCII conversion. trnsfr:FNDLMT ends the
    // compared name at comma or equals, independently of parameter scanning.
    memset(work->name, DISKIMAGE_FILENAME_PADDING, sizeof(work->name));
    unsigned int used = 0;
    while (cursor < length && input[cursor] != ',' && input[cursor] != '=') {
        const uint8_t byte = input[cursor++];
        if (byte == 0x8d) {
            return false;
        }
        if (used < DISKIMAGE_FILENAME_SIZE) {
            work->name[used++] = byte;
        }
    }
    if (used == 0) {
        return false;
    }
    // PARSE still finds comma-separated fields after an ignored equals suffix.
    while (cursor < length && input[cursor] != ',' && input[cursor] != 0x8d) {
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

// Start a raw header chain or native BASIC directory without a filename lookup.
static bool open_directory(dos_unit_t* const unit, dos_media_bank_t* const media, const dos_media_ref_t reference,
                           const uint8_t secondary, const uint8_t* const input, const unsigned int length) {
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
        // OPNRCH/GETRCH frees the old owner, not a separately completed CLOSE.
        free_channel(unit, previous);
    }
    const uint8_t index = allocate_channel(unit, reference, secondary, 2);
    if (index == DOS_UNUSED) {
        command_error(unit, model, DOS_STATUS_NO_CHANNEL);
        return false;
    }
    dos_channel_t* const channel = &unit->channels[index];
    if (secondary != IEEE_LOAD_CHANNEL) {
        // Raw directory reads start at the header, not directory sector 1.
        if (!load_buffer(unit, media, channel, directory_track(model), 0)) {
            free_channel(unit, index);
            command_error(unit, model, DOS_STATUS_READ_ERROR);
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
        command_error(unit, model, DOS_STATUS_NO_CHANNEL);
        return false;
    }
    unit->directory_internal = internal + 1u;
    channel->buffers[2] = extra;
    channel->directory = DOS_DIRECTORY_HEADER;
    channel->directory_track = directory_track(model);
    channel->directory_sector = DOS_DIRECTORY_FIRST_SECTOR;
    channel->position = 0;
    channel->traversed = 0;
    // LOADIR/TC35/ALLDRS retain bounded comma-separated patterns and '=type'.
    uint8_t* const patterns = unit->buffers[extra].bytes;
    memset(patterns, DISKIMAGE_FILENAME_PADDING, DOS_FILENAME_COUNT * DOS_DIRECTORY_PATTERN_SIZE);
    unsigned int cursor = 1, drive_mask = 0;
    if (length == 1 || (length == 2 && (input[1] == '0' || input[1] == '1'))) {
        patterns[0] = '*';
        patterns[DISKIMAGE_FILENAME_SIZE] = length == 1 ? DOS_UNUSED : reference.slot;
        channel->pattern_count = 1;
        drive_mask = length == 1 ? 3u : 1u << reference.slot;
        cursor = length;
    }
    while (cursor < length && input[cursor] != '=' && channel->pattern_count < DOS_FILENAME_COUNT) {
        uint8_t drive = DOS_UNUSED;
        if (input[cursor] == ':') {
            drive = reference.slot;
            ++cursor;
        } else if (cursor + 1u < length && input[cursor + 1u] == ':') {
            drive = input[cursor] == '0' || input[cursor] == '1' ? input[cursor] - '0' : reference.slot;
            cursor += 2u;
        }
        uint8_t* const pattern = patterns + channel->pattern_count++ * DOS_DIRECTORY_PATTERN_SIZE;
        pattern[DISKIMAGE_FILENAME_SIZE] = drive;
        unsigned int used = 0;
        while (cursor < length && input[cursor] != ',' && input[cursor] != '=') {
            if (used < DISKIMAGE_FILENAME_SIZE) {
                pattern[used++] = input[cursor];
            }
            ++cursor;
        }
        drive_mask |= drive == DOS_UNUSED ? 3u : 1u << drive;
        if (cursor < length && input[cursor] == ',') {
            ++cursor;
        }
    }
    // An omitted drive permits the later footer to continue on the peer.
    channel->both_drives = drive_mask == 3u;
    if (cursor < length && input[cursor] == '=') {
        ++cursor;
        // FS1SET selects the final file-type field by its first character.
        for (unsigned int offset = cursor; offset < length; ++offset) {
            if (input[offset] == ',') {
                cursor = offset + 1u;
            }
        }
        if (cursor < length) {
            switch (input[cursor]) {
            case 'P':
                channel->file_type = DISKIMAGE_FTYPE_PRG;
                break;
            case 'S':
                channel->file_type = DISKIMAGE_FTYPE_SEQ;
                break;
            case 'U':
                channel->file_type = DISKIMAGE_FTYPE_USR;
                break;
            case 'R':
                channel->file_type = DISKIMAGE_FTYPE_REL;
                break;
            default:
                break;
            }
        }
    }
    return true;
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
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/begin
    //
    // ROM open:OPEN parses the collected command, FFST searches, OP90 checks
    // read eligibility, and OPREAD calls OPNRCH. Splitting ordinary lookup
    // into dos_work is a firmware scheduling adaptation. That continuation
    // handles multi-drive search. '$' and retained '*' requests can instead
    // finish here, including their required I/O. '#' is not enabled yet.
    vet(!dos_work_pending(unit), "native OPEN requires its preceding job to complete");
    const dos_model_t model = media->units[index].model;
    // ROM tst2:L42 collects bounded input. The approved 4040 revision-2 pointer
    // limit is 59, matching DOS 2.7, not the older source's 41. The C
    // receive capacity bounds the raw copy. CMDSET applies the logical
    // 58-byte limit only after removing a trailing CR or CR-plus-one.
    if (length > DOS_COMMAND_RECEIVE_CAPACITY) {
        command_error(unit, model, DOS_STATUS_COMMAND_TOO_LONG);
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
        command_error(unit, model, DOS_STATUS_COMMAND_TOO_LONG);
        return;
    }
    if (length != 0 && input[0] == '#') {
        command_error(unit, model, DOS_STATUS_SYNTAX);
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
    }
    uint8_t drive = unit->last_drive;
    dos_open_work_t parsed = {0};
    const bool directory = length != 0 && input[0] == '$';
    if (directory) {
        parsed.default_drive = length == 1 || (input[1] != '0' && input[1] != '1' && input[1] != ':');
    }
    // OP02 reloads the retained link before PRSCLN/CKTM inspect any suffix.
    if (!directory && !last_program && (length == 0 || !dos_open_parse(input, length, &drive, &parsed))) {
        command_error(unit, model, length == 0 ? DOS_STATUS_INVALID_NAME : DOS_STATUS_SYNTAX);
        return;
    }
    if (secondary < 2 && !directory) {
        parsed.access = secondary == IEEE_SAVE_CHANNEL ? DOS_ACCESS_WRITE : DOS_ACCESS_READ;
        if (parsed.type == 0) {
            parsed.type = DISKIMAGE_FTYPE_PRG;
        }
    }
    // Later slices add writers and REL. Reject them before claiming resources.
    if (!directory && (parsed.access == DOS_ACCESS_WRITE || parsed.access == DOS_ACCESS_APPEND ||
                       parsed.type == DISKIMAGE_FTYPE_REL)) {
        command_error(unit, model, DOS_STATUS_SYNTAX);
        return;
    }
    if (directory && length > 1 && (input[1] == '0' || input[1] == '1')) {
        drive = input[1] - '0';
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
            command_error(unit, model, DOS_STATUS_NOT_READY);
            return;
        }
        drive ^= 1u;
        unit->selected_drive = drive;
        parsed.default_drive = false;
    }
    if (directory) {
        if (open_directory(unit, media, reference, secondary, input, length)) {
            command_end(unit, model);
        }
        return;
    }
    if (last_program) {
        const uint8_t previous = dos_find_channel(unit, secondary);
        if (previous != DOS_UNUSED && !dos_close(unit, model, secondary)) {
            return;
        }
        const uint8_t channel = allocate_channel(unit, reference, secondary, 2);
        if (channel == DOS_UNUSED) {
            command_error(unit, model, DOS_STATUS_NO_CHANNEL);
        } else if (!load_buffer(unit, media, &unit->channels[channel], unit->last_program_track,
                                unit->last_program_sector)) {
            free_channel(unit, channel);
            command_error(unit, model, DOS_STATUS_READ_ERROR);
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
        command_error(unit, model, DOS_STATUS_NO_CHANNEL);
        return;
    }

    // Retain name/type/SA and the starting link for dos_work. Keep scratch
    // ownership across yields, leaving any old channel intact until a match.
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

// Service one queued ordinary OPEN continuation.
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
    // splits FFST/SRCHST/SEARCH and GETRCH/OPNRCH into retained work.

    // ROM lookup:FFST/SRCHST/SEARCH walk linked directory sectors and COMPAR
    // saves a match's type and first link. Resume that search from the saved
    // C job, doing no work when there is no pending OPEN.
    dos_open_work_t* const work = &unit->open;
    if (!work->pending) {
        return;
    }
    const dos_model_t model = media->units[work->media.unit].model;
    const uint8_t secondary = work->secondary, type = work->type;
    const dos_media_ref_t reference = work->media;
    // Another channel can report a read error between cooperative work calls.
    unit->selected_drive = reference.slot;
    uint8_t found_track = 0, found_sector = 0, found_type = 0, found_entry = 0;
    const uint8_t searched_track = work->track, searched_sector = work->sector;

    // Bound directory visits by image size as a corruption safeguard, not a
    // native ROM timeout. Read one sector into the job's retained scratch.
    const unsigned int limit = media->units[reference.unit].slots[reference.slot].io.image.size / DISKIMAGE_SECTOR_SIZE;
    bool failed = work->visited++ >= limit;
    uint8_t* const bytes = unit->buffers[work->scratch].bytes;
    if (!failed) {
        failed = read_sector(unit, media, reference, work->track, work->sector, bytes) != DOS_MEDIA_OK;
    }
    if (!failed) {
        // ROM SEARCH steps 32 bytes and COMPAR compares up to 16 name bytes.
        // Retain closed and unclosed matches for OPEN's mode-specific checks.
        for (unsigned int entry = 0; entry < DISKIMAGE_SECTOR_SIZE; entry += DISKIMAGE_DIRECTORY_ENTRY_SIZE) {
            const uint8_t* const file = bytes + entry;
            // OP0415/FF30 filter initial LOAD '*' matches before OP110's type check.
            if (file[DOS_DIRECTORY_TYPE_OFFSET] != 0 &&
                (!work->first_program ||
                 (file[DOS_DIRECTORY_TYPE_OFFSET] & DOS_FILE_TYPE_MASK) == DISKIMAGE_FTYPE_PRG) &&
                name_matches(work->name, file + DOS_DIRECTORY_NAME_OFFSET)) {
                found_type = file[DOS_DIRECTORY_TYPE_OFFSET];
                found_track = file[DOS_DIRECTORY_TRACK_OFFSET];
                found_sector = file[DOS_DIRECTORY_SECTOR_OFFSET];
                found_entry = (uint8_t)entry;
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
    if (!failed && found_track == 0 && work->track != 0) {
        return;
    }
    if (!failed && found_track == 0 && work->default_drive) {
        unsigned int next_drive = reference.slot;
        bool retry = false;
        if (!work->tried_other) {
            next_drive ^= 1u;
            work->tried_other = true;
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
    dos_status_code_t error = DOS_STATUS_OK;
    // ROM open:OP95 reports 62 for a missing file. M2 read failures instead
    // become generic 23 with zero track/sector, not full controller recovery.
    if (failed) {
        error = DOS_STATUS_READ_ERROR;
    } else if (found_track == 0) {
        error = DOS_STATUS_NOT_FOUND;
    }

    // ROM open:OP110/OP115 compare the requested file type and report 64.
    // A zero C type means infer it from the match.
    const uint8_t file_type = found_type & DOS_FILE_TYPE_MASK;
    if (found_track != 0) {
        if (!(found_type & DOS_FILE_CLOSED_MASK) && work->access != DOS_ACCESS_MODIFY) {
            error = DOS_STATUS_FILE_OPEN;
        } else if (type != 0 && type != file_type) {
            error = DOS_STATUS_TYPE_MISMATCH;
        } else if (file_type == DISKIMAGE_FTYPE_REL) {
            // REL lookup has no reader in this slice.
            error = DOS_STATUS_TYPE_MISMATCH;
        }
    }
    if (error != DOS_STATUS_OK) {
        unit->buffer_use &= (uint16_t)~((1u << work->scratch) | (1u << work->scratch_second));
        command_error(unit, model, error);
        return;
    }
    // ROM tst3:GETRCH first calls FRECHN, then FNDLNX picks the lowest free
    // logical index and GETBUF claims the requested two buffers. Preserve
    // that order after successful lookup, rolling back partial buffer claims
    // on exhaustion. Only ordinary indices participate in this scan.
    const uint8_t previous = dos_find_channel(unit, secondary);
    if (previous != DOS_UNUSED) {
        free_channel(unit, previous);
    }
    const uint8_t channel = allocate_channel(unit, reference, secondary, 2);
    if (channel == DOS_UNUSED) {
        unit->buffer_use &= (uint16_t)~((1u << work->scratch) | (1u << work->scratch_second));
        command_error(unit, model, DOS_STATUS_NO_CHANNEL);
        return;
    }

    // ROM OPNRCH/INTPNT select BUF0, initialize both data pointers to 2, and
    // STRDBL reads the initial sector (and prefetches a successor if present).
    // Failure of the first read releases this newly allocated channel.
    dos_channel_t* const state = &unit->channels[channel];
    state->directory_track = searched_track;
    state->directory_sector = searched_sector;
    state->directory_entry = found_entry;
    state->file_type = file_type;
    state->first_track = found_track;
    state->first_sector = found_sector;
    unit->buffer_use &= (uint16_t)~((1u << work->scratch) | (1u << work->scratch_second));
    if (!load_buffer(unit, media, state, found_track, found_sector)) {
        free_channel(unit, channel);
        command_error(unit, model, DOS_STATUS_READ_ERROR);
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
        while (channel->directory_entry != DOS_UNUSED) {
            const uint8_t* const entry = source->bytes + channel->directory_entry;
            channel->directory_entry =
                channel->directory_entry == DISKIMAGE_SECTOR_SIZE - DISKIMAGE_DIRECTORY_ENTRY_SIZE
                    ? DOS_UNUSED
                    : channel->directory_entry + DISKIMAGE_DIRECTORY_ENTRY_SIZE;
            const uint8_t type = entry[DOS_DIRECTORY_TYPE_OFFSET] & DOS_FILE_TYPE_MASK;
            bool matches = false;
            for (unsigned int index = 0; index < channel->pattern_count; ++index) {
                const uint8_t* const pattern =
                    unit->buffers[channel->buffers[2]].bytes + index * DOS_DIRECTORY_PATTERN_SIZE;
                matches |= (pattern[DISKIMAGE_FILENAME_SIZE] == DOS_UNUSED ||
                            pattern[DISKIMAGE_FILENAME_SIZE] == channel->media.slot) &&
                           name_matches(pattern, entry + DOS_DIRECTORY_NAME_OFFSET);
            }
            if (entry[DOS_DIRECTORY_TYPE_OFFSET] != 0 && (channel->file_type == 0 || type == channel->file_type) &&
                matches) {
                directory_name(entry, name);
                number = entry[DOS_DIRECTORY_BLOCKS_LOW] | ((uint16_t)entry[DOS_DIRECTORY_BLOCKS_HIGH] << 8u);
                found = true;
                break;
            }
        }
        if (!found) {
            // Yield even at the terminal link. The next call formats MSGFRE.
            channel->directory_track = source->bytes[0];
            channel->directory_sector = source->bytes[1];
            channel->directory_entry = 0;
            if (channel->directory_track == 0) {
                channel->directory = DOS_DIRECTORY_FOOTER;
            }
            return false;
        }
    } else if (channel->directory == DOS_DIRECTORY_FOOTER) {
        if (!free_blocks(unit, media, channel->media, &number)) {
            channel->ready = false;
            return false;
        }
        memset(name, ' ', sizeof(name));
        memcpy(name, "BLOCKS FREE.", sizeof("BLOCKS FREE.") - 1u);
        dos_media_ref_t other;
        if (channel->both_drives &&
            dos_media_reference(media, channel->media.unit, channel->media.slot ^ 1u, &other) == DOS_MEDIA_OK) {
            // Continue with the peer's header after this drive's footer,
            // without repeating the load address or ending the stream.
            channel->media = other;
            unit->selected_drive = other.slot;
            channel->both_drives = false;
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

// Expose the retained lookup's scheduling state without advancing it.
bool dos_work_pending(const dos_unit_t* const unit) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/lookup
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/lookup
    //
    // Host-only scheduler predicate for the split FFST directory search.
    // There is no separate ROM entry or controller-job emulation here.
    return unit->open.pending;
}

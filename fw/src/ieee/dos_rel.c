// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"

#include "dos_rel.h"

#include "fatal.h"
#include "ieee/dos.h"

_Static_assert(sizeof(dos_rel_lookup_t) <= 24u, "REL lookup must not acquire a private page or index cache");
_Static_assert(sizeof(dos_rel_end_t) <= 32u, "REL end lookup must borrow the existing side page");
_Static_assert(sizeof(dos_rel_record_t) <= 36u, "REL record must borrow the existing three channel pages");
_Static_assert(sizeof(dos_rel_reader_t) <= 40u, "REL reader must not own staging bytes or another page");

// Normalize RECORD's byte position, then perform FNDREL's 24-bit calculation.
dos_rel_address_t dos_rel_address(const dos_model_t model, const uint16_t record, const uint8_t record_size,
                                  const uint8_t position) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/record
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/record
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/fndrel
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/fndrel
    //
    // R30/R40 normalizes RECPTR before MULPLY/DIV254/DIV120 calculate
    // RELPTR/SSNUM/SSIND. DOS 2.7 BIGREL additionally uses DIV006 for GRPNUM.
    // Pure arithmetic here does not position buffers or publish RECORD status.
    vet(model == DOS_MODEL_4040 || model == DOS_MODEL_8050 || model == DOS_MODEL_8250, "invalid REL model %u", model);
    // Position zero aliases one. An excessive nonzero offset restarts at the
    // record's beginning while retaining the caller's overflow indication.
    uint8_t offset = position == 0 ? 0 : position - 1u;
    const bool overflow = offset != 0 && offset >= record_size;
    if (overflow) {
        offset = 0;
    }
    // MULPLY aliases record zero to one. Each data page skips two link bytes,
    // and each side page indexes 120 data pages using pairs starting at byte 16.
    const uint32_t byte = (uint32_t)(record == 0 ? 0 : record - 1u) * record_size + offset;
    const uint32_t block = byte / DISKIMAGE_SECTOR_PAYLOAD_SIZE;
    uint32_t side = block / DOS_REL_POINTER_COUNT;
    uint32_t index = block % DOS_REL_POINTER_COUNT;
    if (model == DOS_MODEL_4040) {
        // Revision-2 DIV120 folds by 256 = 2*120 + 16, then subtracts
        // 120 only once after the remainder fits a byte. A remainder still
        // >=120 is observable on large positions (SSIND can wrap below 16).
        side = 0;
        index = block;
        do {
            const uint32_t folded = 2u * (index >> 8u) + ((index >> 7u) & 1u);
            side += folded;
            index = 8u * folded + (index & 0x7fu);
        } while (index > UINT8_MAX);
        if (index >= DOS_REL_POINTER_COUNT) {
            ++side;
            index -= DOS_REL_POINTER_COUNT;
        }
    }
    return (dos_rel_address_t){
        .group = model == DOS_MODEL_8250 ? side / DOS_REL_SIDE_COUNT : 0,
        // The small-file FNDREL stores only RESULT's low byte in SSNUM.
        .side = model == DOS_MODEL_8250 ? side % DOS_REL_SIDE_COUNT : (uint8_t)side,
        .index = (uint8_t)(DOS_REL_POINTER_OFFSET + 2u * index),
        .offset = DISKIMAGE_SECTOR_LINK_SIZE + byte % DISKIMAGE_SECTOR_PAYLOAD_SIZE,
        .overflow = overflow,
    };
}

// Begin a bounded SSTEST/RDSS1 lookup without allocating a separate index cache.
void dos_rel_lookup_start(dos_rel_lookup_t* const lookup, const dos_model_t model, const dos_media_ref_t media,
                          const uint8_t track, const uint8_t sector, const dos_rel_address_t address) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/sstest
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/sstest
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/system
    //
    // No direct ROM entry: initialize a host cursor for SSTEST and RDSS1.
    // Only 8250 starts at a super page. The caller retains pages and ownership,
    // and the captured media token binds every later read to this mount.
    vet(model == DOS_MODEL_4040 || model == DOS_MODEL_8050 || model == DOS_MODEL_8250, "invalid REL model %u", model);
    vet(!(address.index & 1u), "invalid REL index %u", address.index);
    vet(address.group <= DOS_REL_MAX_GROUP, "invalid REL group %u", address.group);
    *lookup = (dos_rel_lookup_t){
        .media = media,
        .address = address,
        .result = DOS_REL_LOOKUP_PENDING,
        .phase = DOS_REL_LOOKUP_ROOT,
        .track = track,
        .sector = sector,
        .super = model == DOS_MODEL_8250,
    };
}

// Retire an absent pointer without exposing stale root/side coordinates as data.
static dos_rel_lookup_result_t missing(dos_rel_lookup_t* const lookup) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/sstest
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/sstest
    //
    // No direct ROM entry: encode SSTEST's ST10/ST30 range outcome as a host
    // result. Clearing coordinates prevents stale index links becoming data.
    // This is not RECORD's SSEND fallback or publication of native error 50.
    lookup->track = 0;
    lookup->sector = 0;
    lookup->result = DOS_REL_LOOKUP_MISSING;
    return lookup->result;
}

// Resolve one native index level, keeping host-storage errors distinct from range.
dos_rel_lookup_result_t dos_rel_lookup_step(dos_rel_lookup_t* const lookup, dos_media_bank_t* const media,
                                            uint8_t scratch[DISKIMAGE_SECTOR_SIZE]) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/sstest
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/sstest
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/system
    //
    // SSTEST/ST50/ST20 and RDSS1C/RDSS1B select group, side and data links.
    // Native jobs wait synchronously. Yielding between index reads is a host
    // scheduling adaptation, with the same media token checked on every read.
    if (lookup->result != DOS_REL_LOOKUP_PENDING) {
        return lookup->result;
    }
    // Read at most one borrowed page. A callback may partially change scratch,
    // but no failed read may publish usable data coordinates.
    lookup->media_result = dos_media_read(media, lookup->media, lookup->track, lookup->sector, scratch);
    if (lookup->media_result != DOS_MEDIA_OK) {
        lookup->track = 0;
        lookup->sector = 0;
        lookup->result = DOS_REL_LOOKUP_FAILED;
        return lookup->result;
    }
    if (lookup->super && lookup->phase == DOS_REL_LOOKUP_ROOT) {
        // RDSS1 uses the group table, not the super sector's leading link.
        const uint8_t index = DOS_REL_GROUP_TABLE_OFFSET + 2u * lookup->address.group;
        lookup->track = scratch[index];
        lookup->sector = scratch[(uint8_t)(index + 1u)];
        lookup->phase = DOS_REL_LOOKUP_GROUP;
        return lookup->track == 0 ? missing(lookup) : lookup->result;
    }
    // Native header equality precedes the six-side bound. Once a selected
    // side is read, SSTEST consumes its pointer without another header check.
    if (lookup->phase != DOS_REL_LOOKUP_SIDE && scratch[DOS_REL_SIDE_NUMBER_OFFSET] != lookup->address.side) {
        if (lookup->address.side >= DOS_REL_SIDE_COUNT) {
            return missing(lookup);
        }
        const uint8_t index = DOS_REL_SIDE_TABLE_OFFSET + 2u * lookup->address.side;
        lookup->track = scratch[index];
        lookup->sector = scratch[index + 1u];
        lookup->phase = DOS_REL_LOOKUP_SIDE;
        return lookup->track == 0 ? missing(lookup) : lookup->result;
    }
    // SSTEST tests the track byte at SSIND. The sector may legitimately be zero.
    lookup->track = scratch[lookup->address.index];
    lookup->sector = scratch[lookup->address.index + 1u];
    if (lookup->track == 0) {
        return missing(lookup);
    }
    lookup->result = DOS_REL_LOOKUP_FOUND;
    return lookup->result;
}

// Abandon only this cursor, leaving pool ownership and backing storage untouched.
void dos_rel_lookup_cancel(dos_rel_lookup_t* const lookup) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/sstest
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/sstest
    //
    // No direct ROM entry: discard only the cooperative SSTEST cursor.
    // This performs no native channel teardown, buffer release or media I/O.
    *lookup = (dos_rel_lookup_t){0};
}

// Capture the root for SSEND without acquiring another native buffer.
void dos_rel_end_start(dos_rel_end_t* const end, const dos_model_t model, const dos_media_ref_t media,
                       const uint8_t track, const uint8_t sector) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ssend
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ssend
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/system
    //
    // No direct ROM entry: capture a cold SSEND traversal, including RDLG
    // for 8250. Resident/dirty side-page reuse remains the channel owner's job.
    vet(model == DOS_MODEL_4040 || model == DOS_MODEL_8050 || model == DOS_MODEL_8250, "invalid REL model %u", model);
    *end = (dos_rel_end_t){
        .media = media,
        .result = DOS_REL_END_PENDING,
        .phase = DOS_REL_LOOKUP_ROOT,
        .track = track,
        .sector = sector,
        .super = model == DOS_MODEL_8250,
    };
}

// Resolve SSEND's final side and pointer using one borrowed page per read.
dos_rel_end_result_t dos_rel_end_step(dos_rel_end_t* const end, dos_media_bank_t* const media,
                                      uint8_t scratch[DISKIMAGE_SECTOR_SIZE]) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ssend
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ssend
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/system
    //
    // SSEND/SE10/SE20/SE30 finds the last side and its endpoint; RDLG first
    // selects the last 8250 group. One host read per call replaces blocking
    // jobs. FOUND leaves the last side page in scratch for the owner.
    if (end->result != DOS_REL_END_PENDING) {
        return end->result;
    }
    // Keep storage failure distinct from native BREAK and the bounded
    // no-group safeguard below. Terminal calls retain the captured result.
    end->media_result = dos_media_read(media, end->media, end->track, end->sector, scratch);
    if (end->media_result != DOS_MEDIA_OK) {
        end->result = DOS_REL_END_FAILED;
        return end->result;
    }
    if (end->super && end->phase == DOS_REL_LOOKUP_ROOT) {
        // RDLG ignores group 91 even though FNDREL can calculate it. Stop at
        // group zero instead of emulating malformed underflow into adjacent RAM.
        for (unsigned int group = DOS_REL_END_GROUP_START + 1u; group != 0; --group) {
            const unsigned int index = DOS_REL_GROUP_TABLE_OFFSET + 2u * (group - 1u);
            if (scratch[index] != 0) {
                end->group = group - 1u;
                end->track = scratch[index];
                end->sector = scratch[index + 1u];
                end->phase = DOS_REL_LOOKUP_GROUP;
                return end->result;
            }
        }
        end->result = DOS_REL_END_NO_GROUP;
        return end->result;
    }
    // Scan the six-side table backward. An empty table is SSEND's BREAK path,
    // whereas a matching header lets SE20 reuse this page without another read.
    if (end->phase != DOS_REL_LOOKUP_SIDE) {
        unsigned int side = DOS_REL_SIDE_COUNT;
        while (side != 0 && scratch[DOS_REL_SIDE_TABLE_OFFSET + 2u * (side - 1u)] == 0) {
            --side;
        }
        if (side == 0) {
            end->result = DOS_REL_END_CORRUPT;
            return end->result;
        }
        end->side = side - 1u;
        // Equality skips the read even if the table's coordinates differ.
        if (scratch[DOS_REL_SIDE_NUMBER_OFFSET] != end->side) {
            const unsigned int index = DOS_REL_SIDE_TABLE_OFFSET + 2u * end->side;
            end->track = scratch[index];
            end->sector = scratch[index + 1u];
            end->phase = DOS_REL_LOOKUP_SIDE;
            return end->result;
        }
    }
    // SE30 checks the link, not the selected side's header number or endpoint
    // parity. Preserve DEY's wrap for malformed endpoint bytes zero and one.
    if (scratch[0] != 0) {
        end->result = DOS_REL_END_CORRUPT;
    } else {
        end->index = (uint8_t)(scratch[1] - 1u);
        end->result = DOS_REL_END_FOUND;
    }
    return end->result;
}

// Abandon only this last-index traversal, leaving media and pages untouched.
void dos_rel_end_cancel(dos_rel_end_t* const end) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ssend
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ssend
    //
    // No direct ROM entry: discard host SSEND progress, not the caller's
    // borrowed page, channel allocation or pending persistence obligations.
    *end = (dos_rel_end_t){0};
}

// Capture the requested record and normalized RECORD byte position.
void dos_rel_record_start(dos_rel_record_t* const record, const dos_model_t model, const dos_media_ref_t media,
                          const uint8_t track, const uint8_t sector, const uint16_t number, const uint8_t size,
                          const uint8_t position) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/record
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/record
    //
    // No direct ROM entry for cursor initialization. R30/R40 and FNDREL supply
    // POSITN's normalized byte position, split into cooperative phases here.
    // Restart abandons old cursor state, so the owner must first retire writes
    // and FIFO previews. The nonzero-size requirement is a helper safeguard.
    vet(size != 0, "REL record size must be nonzero");
    const dos_rel_address_t address = dos_rel_address(model, number, size, position);
    *record = (dos_rel_record_t){
        .result = DOS_REL_RECORD_PENDING,
        .phase = DOS_REL_RECORD_INDEX,
        .size = size,
        .position = address.overflow || position == 0 ? 0 : position - 1u,
        .pointer = address.offset,
    };
    dos_rel_lookup_start(&record->lookup, model, media, track, sector, address);
}

// Scan backward exactly like FNDLST, including its byte-wide remaining counter.
static bool record_find_last(const uint8_t page[DISKIMAGE_SECTOR_SIZE], uint8_t* const remaining, uint8_t pointer,
                             uint8_t* const last) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel4
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel4
    //
    // FNDLST/FNDL10/FNDL30 scans non-link bytes backward for a nonzero byte.
    // Preserve R2's extra decrement on failure so a cross-page scan shares
    // the native byte-wide remaining count. *last changes only when found.
    do {
        if (page[pointer] != 0) {
            *last = pointer;
            return true;
        }
        --pointer;
        if (pointer < DISKIMAGE_SECTOR_LINK_SIZE) {
            break;
        }
        --*remaining;
    } while (*remaining != 0);
    --*remaining;
    return false;
}

// Apply POSITN/NXOUT/SETLST after the native current/prefetch pages are resident.
static void record_prepare(dos_rel_record_t* const record, const uint8_t first[DISKIMAGE_SECTOR_SIZE],
                           const uint8_t following[DISKIMAGE_SECTOR_SIZE]) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/record
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/record
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel1
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel1
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel4
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel4
    //
    // POSITN/P2/P30 computes the record end, NXOUT sets NR/LRF, and
    // SETLST/FNDLST finds LSTCHR. Resident pages are borrowed, not fetched here.

    // Skip link bytes on a seam. A partial record without a forward link
    // becomes MISSING, but an exact page endpoint remains a readable record.
    const unsigned int end = record->pointer + record->size - record->position;
    const bool crosses = end >= DISKIMAGE_SECTOR_SIZE;
    const bool linked = first[0] != 0;
    const uint8_t next = (uint8_t)(end + (crosses ? DISKIMAGE_SECTOR_LINK_SIZE : 0u));
    record->last = record->pointer;
    if (crosses && !linked && next != DISKIMAGE_SECTOR_LINK_SIZE) {
        // NXTR45 sets LRF without storing the rejected endpoint in NR.
        record->result = DOS_REL_RECORD_MISSING;
        return;
    }
    record->next = next;

    // SETLST compares native low-byte pointers, not a widened absolute offset.
    uint8_t last = record->next == DISKIMAGE_SECTOR_LINK_SIZE ? UINT8_MAX : record->next - 1u;
    uint8_t remaining = record->size;
    if (record->pointer > last) {
        if (record->following_failed) {
            record->result = DOS_REL_RECORD_FAILED;
            return;
        }
        vet(record->following, "REL end scan requires the native prefetch page");
        if (record_find_last(following, &remaining, last, &record->last)) {
            record->result = DOS_REL_RECORD_READY;
            return;
        }
        last = UINT8_MAX;
    }
    record_find_last(first, &remaining, last, &record->last);
    // NXTR50 preserves the prepared last byte but marks no subsequent record.
    if (crosses && !linked) {
        record->next = 0;
    }
    record->result = DOS_REL_RECORD_READY;
}

// Prepare a clean record with at most one read into the owner's three pages.
// Failed reads may alter a page, but never make partial data ready.
dos_rel_record_result_t dos_rel_record_step(dos_rel_record_t* const record, dos_media_bank_t* const media,
                                            uint8_t side[DISKIMAGE_SECTOR_SIZE],
                                            uint8_t first_page[DISKIMAGE_SECTOR_SIZE],
                                            uint8_t following_page[DISKIMAGE_SECTOR_SIZE]) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/record
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/record
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/tst2
    //
    // POSITN/POSBUF and STRDBL prepare current data and start read-ahead.
    // DBLBUF waits only when the alternate is selected. Cooperative phases
    // replace blocking jobs here, excluding resident/dirty POSBUF reuse
    // (including the approved 4040 revision-2 sequencing) handled by the owner.
    if (record->result != DOS_REL_RECORD_PENDING) {
        return record->result;
    }
    vet(record->phase <= DOS_REL_RECORD_FOLLOWING, "REL native jobs require their channel owner");
    // Resolve only the index first. NO_INDEX needs the owner's SSEND fallback,
    // not an immediate missing-record response or native error 50.
    if (record->phase == DOS_REL_RECORD_INDEX) {
        const dos_rel_lookup_result_t result = dos_rel_lookup_step(&record->lookup, media, side);
        if (result == DOS_REL_LOOKUP_FOUND) {
            record->phase = DOS_REL_RECORD_DATA;
        } else if (result == DOS_REL_LOOKUP_MISSING) {
            record->result = DOS_REL_RECORD_NO_INDEX;
        } else if (result == DOS_REL_LOOKUP_FAILED) {
            record->result = DOS_REL_RECORD_FAILED;
        }
        return record->result;
    }
    // Data pages are independent pool entries. Read-ahead follows the active
    // page's link into the other page, never into contiguous private storage.
    uint8_t* const current_page = record->active == 0 ? first_page : following_page;
    uint8_t* const next_page = record->active == 0 ? following_page : first_page;
    const bool following = record->phase == DOS_REL_RECORD_FOLLOWING;
    const uint8_t track = following ? current_page[0] : record->lookup.track;
    const uint8_t sector = following ? current_page[1] : record->lookup.sector;
    record->lookup.media_result =
        dos_media_read(media, record->lookup.media, track, sector, following ? next_page : current_page);
    if (record->lookup.media_result != DOS_MEDIA_OK) {
        if (following &&
            (record->lookup.media_result == DOS_MEDIA_IO_ERROR || record->lookup.media_result == DOS_MEDIA_RANGE)) {
            // STRDBL submits prefetch without WATJOB. Native DBLBUF waits
            // only when SETLST/NRBUF selects that buffer. Preserve usable
            // current data, but never mark the partial prefetch resident.
            record->following_failed = true;
            record_prepare(record, current_page, next_page);
        } else {
            record->result = DOS_REL_RECORD_FAILED;
        }
        return record->result;
    }
    // STRDBL prefetches the following block even when this record fits in one.
    // This helper starts with no resident pages. Reuse/dirty-buffer ownership
    // in the selected POSBUF revision remains the channel owner's concern.
    if (!following && current_page[0] != 0) {
        record->phase = DOS_REL_RECORD_FOLLOWING;
    } else {
        record->following = following;
        record_prepare(record, current_page, next_page);
    }
    return record->result;
}

// Guard cached-byte access using the same captured generation as sector reads.
static bool record_current(dos_rel_record_t* const record, const dos_media_bank_t* const media) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel3
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel3
    //
    // No direct ROM entry: mount-generation validation protects cached RDREL
    // bytes from host remounts. This token is not the native disk-ID check.
    // Removal or replacement retires the cursor without publishing old data.
    const dos_media_ref_t captured = record->lookup.media;
    dos_media_ref_t current;
    dos_media_result_t result = dos_media_reference(media, captured.unit, captured.slot, &current);
    if (result == DOS_MEDIA_ABSENT || (result == DOS_MEDIA_OK && current.identity != captured.identity)) {
        result = DOS_MEDIA_STALE;
    }
    if (result != DOS_MEDIA_OK) {
        record->lookup.media_result = result;
        record->result = DOS_REL_RECORD_FAILED;
        return false;
    }
    return true;
}

// Preview RDREL's byte/EOI without advancing or publishing native status.
// Failure leaves outputs unchanged. A missing record previews CR with EOI.
bool dos_rel_record_peek(dos_rel_record_t* const record, const dos_media_bank_t* const media,
                         const uint8_t first[DISKIMAGE_SECTOR_SIZE], const uint8_t following[DISKIMAGE_SECTOR_SIZE],
                         uint8_t* const byte, bool* const eoi) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel3
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel3
    //
    // No direct ROM entry for preview-only access. RD25/RD30 publishes
    // CHNDAT/readiness, while RD05 supplies missing CR/EOI. Separate bytes from
    // GET side effects and error-50 publication, which remain with the owner.
    if (record->result != DOS_REL_RECORD_READY && record->result != DOS_REL_RECORD_MISSING) {
        return false;
    }
    // Validate the captured mount even when no storage read is needed.
    if (!record_current(record, media)) {
        return false;
    }
    vet(byte != NULL && eoi != NULL, "REL preview requires byte and EOI outputs");
    const bool missing_record = record->result == DOS_REL_RECORD_MISSING;
    *byte = missing_record ? '\r' : (record->active == 0 ? first : following)[record->pointer];
    *eoi = missing_record || record->pointer == record->last;
    return true;
}

// Commit actual GET only, leaving next-record preparation to an explicit call.
dos_rel_record_result_t dos_rel_record_get(dos_rel_record_t* const record, const dos_media_bank_t* const media) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel3
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel3
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel1
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel1
    //
    // RDREL/RD10 advances within a record and joins RD40 at LSTCHR.
    // This leaf splits off NXTREC and NRBUF I/O for the owner, committing only
    // a consumed byte. Reader-level code manages GETFLG and missing status.
    if (record->result != DOS_REL_RECORD_READY && record->result != DOS_REL_RECORD_MISSING) {
        return record->result;
    }
    if (!record_current(record, media) || record->result == DOS_REL_RECORD_MISSING) {
        return record->result;
    }
    // Record EOI wins over page wrap. Missing CR/EOI stays sticky across GETs.
    if (record->pointer == record->last) {
        record->result = DOS_REL_RECORD_END;
    } else if (record->pointer != UINT8_MAX) {
        ++record->pointer;
    } else {
        record->pointer = DISKIMAGE_SECTOR_LINK_SIZE;
        // A ready alternate permits the pointer swap, but no read is issued.
        // A deferred failed prefetch fails this cursor only now.
        if (record->following) {
            record->active ^= 1u;
            record->following = false;
        } else if (record->following_failed) {
            record->result = DOS_REL_RECORD_FAILED;
        } else {
            record->result = DOS_REL_RECORD_NEEDS_LINK;
        }
    }
    return record->result;
}

// Apply read-only NXTREC while preserving the original native buffer window.
dos_rel_record_result_t dos_rel_record_next(dos_rel_record_t* const record, const dos_media_bank_t* const media,
                                            const uint8_t first[DISKIMAGE_SECTOR_SIZE],
                                            const uint8_t following[DISKIMAGE_SECTOR_SIZE]) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel1
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel1
    //
    // NXTR15/NXTR20/NXTR45 uses NR and the current byte pointer to select the
    // next record. NRBU50/NRBU70 handles the clean alternate and read-ahead.
    // Here PENDING requests later I/O, while the caller owns record counting,
    // GET/overflow flags and any dirty NRBUF continuation.
    if (record->result != DOS_REL_RECORD_END || !record_current(record, media)) {
        return record->result;
    }
    // NXTR45 prepares the synthetic missing byte without another index lookup.
    if (record->next == 0) {
        record->last = record->pointer;
        record->result = DOS_REL_RECORD_MISSING;
        return record->result;
    }
    const uint8_t* current_page = record->active == 0 ? first : following;
    // Native low-byte comparison, not an absolute file offset, selects NRBUF.
    // Preserve a failed read-ahead until this transition actually needs it.
    if (record->pointer >= record->next) {
        if (record->following_failed) {
            record->result = DOS_REL_RECORD_FAILED;
            return record->result;
        }
        if (!record->following && current_page[0] == 0) {
            record->result = DOS_REL_RECORD_NEEDS_LINK;
            return record->result;
        }
        const bool resident = record->following;
        record->lookup.track = current_page[0];
        record->lookup.sector = current_page[1];
        record->active ^= 1u;
        record->following = false;
        current_page = record->active == 0 ? first : following;
        record->phase = resident ? DOS_REL_RECORD_FOLLOWING : DOS_REL_RECORD_DATA;
        record->pointer = record->next;
        record->position = 0;
        if (!resident || current_page[0] != 0) {
            record->result = DOS_REL_RECORD_PENDING;
            return record->result;
        }
    } else {
        record->pointer = record->next;
        record->position = 0;
        // A GET may have selected the prefetch page. Refill the now-consumed
        // page before SETLST, without ever rereading an index or failed job.
        if (!record->following && !record->following_failed && current_page[0] != 0) {
            record->phase = DOS_REL_RECORD_FOLLOWING;
            record->result = DOS_REL_RECORD_PENDING;
            return record->result;
        }
    }
    record_prepare(record, current_page, record->active == 0 ? following : first);
    return record->result;
}

// Abandon preparation and preview state without changing borrowed page ownership.
void dos_rel_record_cancel(dos_rel_record_t* const record) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/record
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/record
    //
    // No direct ROM entry: abandon host RECORD/POSITN preparation only.
    // This neither scrubs nor releases pages, so the owner must retain any
    // dirty mask and finish or explicitly retire its pending write jobs.
    *record = (dos_rel_record_t){0};
}

// Capture native RECORD state independently of normalized address arithmetic.
void dos_rel_reader_start(dos_rel_reader_t* const reader, const dos_model_t model, const dos_media_ref_t media,
                          const uint8_t track, const uint8_t sector, const uint16_t number, const uint8_t size,
                          const uint8_t position) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/record
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/record
    //
    // R30/R40 stores raw RECL/RECH before FNDREL aliases zero for addressing.
    // No direct ROM entry for the host reader wrapper: restarting also clears
    // GET/missing-input history without changing caller-owned pages or jobs.
    *reader = (dos_rel_reader_t){.number = number};
    dos_rel_record_start(&reader->record, model, media, track, sector, number, size, position);
}

// Stage a suffix through a cursor copy, never through native GET side effects.
unsigned int dos_rel_reader_stage(dos_rel_reader_t* const reader, const dos_media_bank_t* const media,
                                  const uint8_t first[DISKIMAGE_SECTOR_SIZE],
                                  const uint8_t following[DISKIMAGE_SECTOR_SIZE], uint8_t* const output,
                                  const unsigned int capacity, bool* const eoi) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/ieee
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/ieee
    //
    // No direct ROM entry for FIFO staging. TLK10 presents CHNDAT/CHNRDY,
    // while only TLK30 calls GET. Copying a cursor allows bounded preview
    // without committing record counters, GETFLG, missing status or page jobs.
    // Dirty-page seams are outside this clean-read foundation.
    vet(capacity <= DOS_STAGE_CAPACITY && eoi != NULL && (capacity == 0 || output != NULL),
        "invalid REL staging window");
    *eoi = false;
    uint8_t byte;
    bool final;
    if (capacity == 0 || !dos_rel_record_peek(&reader->record, media, first, following, &byte, &final)) {
        return 0;
    }
    // Check the real mount first, then advance only the copy. A short prefix
    // without EOI may mean capacity or an unprepared page, not end of record.
    dos_rel_record_t preview = reader->record;
    unsigned int length = 0;
    while (length < capacity) {
        output[length++] = byte;
        if (final) {
            *eoi = true;
            break;
        }
        if (length == capacity) {
            break;
        }
        dos_rel_record_get(&preview, media);
        if (!dos_rel_record_peek(&preview, media, first, following, &byte, &final)) {
            break;
        }
    }
    return length;
}

// Observe one actual GET and join RD40's next-record transition at EOI.
dos_rel_record_result_t dos_rel_reader_get(dos_rel_reader_t* const reader, const dos_model_t model,
                                           const dos_media_bank_t* const media,
                                           const uint8_t first[DISKIMAGE_SECTOR_SIZE],
                                           const uint8_t following[DISKIMAGE_SECTOR_SIZE]) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel3
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel3
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/rel1
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/rel1
    //
    // RDREL/RD10 handles a consumed byte; RD40/NXTREC advances after EOI.
    // RD05's error event is represented by no_record for the owner to publish.
    // This commits only an actually consumed staged byte, never speculative
    // FIFO preview, and leaves pending sector jobs to the channel scheduler.
    vet(model == DOS_MODEL_4040 || model == DOS_MODEL_8050 || model == DOS_MODEL_8250, "invalid REL model %u", model);
    dos_rel_record_t* const record = &reader->record;
    vet(record->result == DOS_REL_RECORD_READY || record->result == DOS_REL_RECORD_MISSING,
        "REL GET requires a prepared byte");
    reader->no_record = false;
    if (!record_current(record, media)) {
        return record->result;
    }
    // Reaching missing state after good EOI is not yet a missing-byte GET.
    // Only consuming that already-prepared CR/EOI requests native error 50.
    if (record->result == DOS_REL_RECORD_MISSING) {
        reader->no_record = true;
        return record->result;
    }
    // DOS 2.7 RD10 has no 4040 GETFLG operation.
    reader->get_active = model == DOS_MODEL_4040;
    // NXTREC increments the raw 16-bit record number with wrap and clears
    // 4040 GETFLG. The leaf next-record transition does not increment it again.
    if (dos_rel_record_get(record, media) == DOS_REL_RECORD_END) {
        reader->number = (uint16_t)(reader->number + 1u);
        reader->get_active = false;
        dos_rel_record_next(record, media, first, following);
    }
    return record->result;
}

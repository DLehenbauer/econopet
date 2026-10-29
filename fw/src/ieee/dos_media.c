// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "dos_media.h"
#include "pch.h"

/*
 * Bounded backing-storage boundary for the native DOS implementation.
 *
 * A unit represents one dual-drive device, with two independently present
 * image slots and one shared DOS model. Descriptors borrow callbacks and their
 * context from the facade. This layer never opens, closes, or owns a file and
 * never allocates an image-sized cache.
 *
 * Validation, mount generations, dirty tracking, and flush-before-rebind are
 * host-storage policies, not translated ROM algorithms. Only BAM location
 * selection below directly follows the ROM's model-specific tables.
 * DOS_MEDIA_* results describe storage/lifetime failures, not command-channel
 * status numbers. The DOS caller decides how to expose them to the controller.
 *
 * The facade serializes access, quiesces transfers before binding changes,
 * and manages DOS channels and callback-context lifetimes. These routines do
 * not provide locking or make a multi-assignment update atomic to observers.
 * Zero-initialize a bank once, then preserve its generation across DOS resets
 * and unmounts so retained work cannot acquire a replacement disk accidentally.
 */

#define DOS_MEDIA_ID_NONE 0u
#define DOS_BAM_TRACK_4040 18u
#define DOS_BAM_TRACK_8050_8250 38u

// Validate one borrowed raw descriptor without I/O or content inspection.
// Return a host-storage result without changing the descriptor.
static dos_media_result_t validate_media(const dos_media_io_t* const media) {
    // This is host descriptor validation, not a ROM disk-initialization path.
    // The caller supplies a non-NULL descriptor. An empty slot must have no
    // callbacks, context, size, or flush operation, so partially cleared
    // descriptors cannot masquerade as an intentional removal.
    const diskimage_t* const image = &media->image;
    if (image->type == diskimage_type_none) {
        return image->read == NULL && image->write == NULL && image->ctx == NULL && image->size == 0 &&
                       media->flush == NULL
                   ? DOS_MEDIA_OK
                   : DOS_MEDIA_INVALID;
    }
    // diskimage_open only detects the exact raw D64/D80/D82 size and fills a
    // temporary descriptor. It does not invoke read or inspect disk bytes.
    // Re-detect independently and require agreement with the declared type,
    // rejecting missing readers, unsupported sizes, and mismatched formats.
    // BAM validity, directory structure, and sector contents are not checked.
    diskimage_t detected;
    if (image->read == NULL || !diskimage_open(&detected, image->read, image->ctx, image->size) ||
        detected.type != image->type) {
        return DOS_MEDIA_UNSUPPORTED;
    }
    // A writer may buffer or partially modify data, so every writable mount
    // needs a flush callback for safe retirement. Read-only mounts need no
    // flush callback. The supplied context remains borrowed in either case.
    if (image->write != NULL && media->flush == NULL) {
        return DOS_MEDIA_INVALID;
    }
    return DOS_MEDIA_OK;
}

// Validate a complete unit pair without I/O and select its shared DOS model.
// Empty partners are allowed. Leave model unchanged on any failure.
dos_media_result_t dos_media_validate_pair(const dos_media_io_t pair[DOS_MEDIA_SLOTS_PER_UNIT],
                                           dos_model_t* const model) {
    // Pair compatibility is a host mount policy, not ROM media autodetection.
    // One device runs one model, so reject mixed nonempty formats rather than
    // selecting a model from only the slot being changed.
    if (pair == NULL || model == NULL) {
        return DOS_MEDIA_INVALID;
    }
    diskimage_type_t type = diskimage_type_none;
    for (unsigned int slot = 0; slot < DOS_MEDIA_SLOTS_PER_UNIT; ++slot) {
        const dos_media_result_t result = validate_media(&pair[slot]);
        if (result != DOS_MEDIA_OK) {
            return result;
        }
        // An empty slot neither selects the model nor redirects access to its
        // partner. Every nonempty slot must agree with the first mounted type.
        const diskimage_type_t candidate = pair[slot].image.type;
        if (candidate == diskimage_type_none) {
            continue;
        }
        if (type != diskimage_type_none && type != candidate) {
            return DOS_MEDIA_MIXED_PAIR;
        }
        type = candidate;
    }
    // Publish the model only after both descriptors pass. Exact raw size
    // selects 4040/D64, single-sided 8050/D80, or double-sided 8250/D82.
    // A fully empty pair has no model. This does not initialize native DOS
    // state, read a disk ID, or verify any filesystem contents.
    switch (type) {
    case diskimage_type_none:
        *model = DOS_MODEL_NONE;
        break;
    case diskimage_type_d64:
        *model = DOS_MODEL_4040;
        break;
    case diskimage_type_d80:
        *model = DOS_MODEL_8050;
        break;
    case diskimage_type_d82:
        *model = DOS_MODEL_8250;
        break;
    default:
        return DOS_MEDIA_UNSUPPORTED;
    }
    return DOS_MEDIA_OK;
}

// Flush this unit's dirty slots without closing or detaching their bindings.
// Stop on failure, retaining that slot's dirty state for a later retry.
dos_media_result_t dos_media_flush_unit(dos_media_bank_t* const bank, const unsigned int unit) {
    // This is backing-store persistence, not ROM CLOSE, BAM writeback, or a
    // native DOS acknowledgement. A validated writable descriptor supplies
    // flush whenever writes can mark its slot dirty.
    if (bank == NULL || unit >= DOS_MEDIA_UNIT_COUNT) {
        return DOS_MEDIA_INVALID;
    }
    for (unsigned int slot = 0; slot < DOS_MEDIA_SLOTS_PER_UNIT; ++slot) {
        dos_media_slot_t* const media = &bank->units[unit].slots[slot];
        // Skip clean slots, including read-only and empty ones. Clear dirty
        // only after the callback succeeds. Earlier successful flushes remain
        // clean if a later slot fails, but no binding or identity is changed.
        if (!media->dirty) {
            continue;
        }
        if (!media->io.flush(media->io.image.ctx)) {
            return DOS_MEDIA_IO_ERROR;
        }
        media->dirty = false;
    }
    return DOS_MEDIA_OK;
}

// Replace a unit's complete pair after validating it and flushing the old pair.
// On failure retain all old bindings. Success invalidates both slots' old work.
dos_media_result_t dos_media_set_unit(dos_media_bank_t* const bank, const unsigned int unit,
                                      const dos_media_io_t pair[DOS_MEDIA_SLOTS_PER_UNIT]) {
    // Rebinding is a host lifecycle operation with no direct ROM equivalent.
    // The facade must serialize access and quiesce the affected DOS/transport
    // work. This function only validates descriptors, retires buffered storage
    // writes, and publishes media state. It does not close files or DOS channels.
    if (bank == NULL || unit >= DOS_MEDIA_UNIT_COUNT) {
        return DOS_MEDIA_INVALID;
    }
    dos_model_t model;
    const dos_media_result_t validated = dos_media_validate_pair(pair, &model);
    if (validated != DOS_MEDIA_OK) {
        return validated;
    }
    // Validate before touching old storage. Refuse generation wrap before
    // flushing as well, so a rejected mount cannot reuse a past job's identity.
    // An all-empty replacement needs no new identity and is still permitted
    // after exhaustion, allowing existing media to be removed safely.
    if (model != DOS_MODEL_NONE && bank->generation == UINT32_MAX) {
        return DOS_MEDIA_ID_EXHAUSTED;
    }

    // Snapshot both proposed descriptors before publication can overwrite
    // bank-backed input. This copies borrowed handles, not their backing data.
    // Flush the old pair, not the candidate. A failed flush retains all old
    // bindings and the generation, although earlier successful flushes may
    // have cleared their dirty flags.
    dos_media_io_t proposed[DOS_MEDIA_SLOTS_PER_UNIT];
    for (unsigned int slot = 0; slot < DOS_MEDIA_SLOTS_PER_UNIT; ++slot) {
        proposed[slot] = pair[slot];
    }
    const dos_media_result_t flushed = dos_media_flush_unit(bank, unit);
    if (flushed != DOS_MEDIA_OK) {
        return flushed;
    }

    // Allocate one bank-wide generation for the complete nonempty pair.
    // Both present slots share it, but references also contain unit and slot.
    // Rebinding either slot therefore invalidates work for both old slots,
    // including an unchanged partner, without changing other units.
    // These assignments form a serialized publication, not a lock-free commit.
    const uint32_t identity = model == DOS_MODEL_NONE ? DOS_MEDIA_ID_NONE : ++bank->generation;
    for (unsigned int slot = 0; slot < DOS_MEDIA_SLOTS_PER_UNIT; ++slot) {
        // Empty slots keep identity zero. Replacing the whole slot also clears
        // dirty, since old pending writes were flushed before publication.
        bank->units[unit].slots[slot] = (dos_media_slot_t){
            .io = proposed[slot],
            .identity = proposed[slot].image.type == diskimage_type_none ? DOS_MEDIA_ID_NONE : identity,
        };
    }
    // Publish the shared model with the new pair. The facade may now reset
    // this unit's DOS state and close retired handles it owns.
    bank->units[unit].model = model;
    return DOS_MEDIA_OK;
}

// Replace or remove one slot using the complete unit's validation/lifecycle.
// NULL removes the slot. A successful change invalidates both old slot references.
dos_media_result_t dos_media_set_slot(dos_media_bank_t* const bank, const unsigned int unit, const unsigned int slot,
                                      const dos_media_io_t* const media) {
    // A single-slot host request still affects a dual-drive unit's shared
    // model. Copy the current pair and substitute only the requested slot,
    // rather than bypassing pair compatibility or partner flush requirements.
    if (bank == NULL || unit >= DOS_MEDIA_UNIT_COUNT || slot >= DOS_MEDIA_SLOTS_PER_UNIT) {
        return DOS_MEDIA_INVALID;
    }
    dos_media_io_t pair[DOS_MEDIA_SLOTS_PER_UNIT];
    for (unsigned int current = 0; current < DOS_MEDIA_SLOTS_PER_UNIT; ++current) {
        pair[current] = bank->units[unit].slots[current].io;
    }
    // A zero descriptor explicitly means removal. Delegate publication so
    // this path has the same failure retention and generation rules as a
    // full-pair replacement, even if the proposed descriptor is unchanged.
    pair[slot] = media == NULL ? (dos_media_io_t){0} : *media;
    return dos_media_set_unit(bank, unit, pair);
}

// Capture a mounted slot's current unit/slot/generation identity for later I/O.
// Leave reference unchanged on invalid arguments or absent media.
dos_media_result_t dos_media_reference(const dos_media_bank_t* const bank, const unsigned int unit,
                                       const unsigned int slot, dos_media_ref_t* const reference) {
    // This host lifetime token is not a ROM disk ID, channel number, or file
    // handle. Validate coordinates before consulting the binding. Identity
    // zero denotes absence even when the partner slot contains a usable disk.
    if (bank == NULL || reference == NULL || unit >= DOS_MEDIA_UNIT_COUNT || slot >= DOS_MEDIA_SLOTS_PER_UNIT) {
        return DOS_MEDIA_INVALID;
    }
    const uint32_t identity = bank->units[unit].slots[slot].identity;
    if (identity == DOS_MEDIA_ID_NONE) {
        return DOS_MEDIA_ABSENT;
    }
    // Publish all token fields only after success. Later sector access must
    // resolve this exact slot and generation, not whatever disk is then mounted.
    // The token does not own or extend the lifetime of the callback context.
    *reference = (dos_media_ref_t){.identity = identity, .unit = unit, .slot = slot};
    return DOS_MEDIA_OK;
}

// Resolve a current media reference and valid sector to a slot and byte offset.
// Reject stale work before any I/O callback can reach replacement media.
static dos_media_result_t resolve_sector(dos_media_bank_t* const bank, const dos_media_ref_t reference,
                                         const unsigned int track, const unsigned int sector,
                                         dos_media_slot_t** const media, uint32_t* const offset) {
    // This is the shared host guard for reads and writes. Callers provide
    // non-NULL output pointers. Validate token coordinates before indexing,
    // then compare its captured generation with the live slot. A removed,
    // remounted, or never-issued identity returns STALE, not ABSENT.
    if (bank == NULL || reference.unit >= DOS_MEDIA_UNIT_COUNT || reference.slot >= DOS_MEDIA_SLOTS_PER_UNIT) {
        return DOS_MEDIA_INVALID;
    }
    dos_media_slot_t* const selected = &bank->units[reference.unit].slots[reference.slot];
    if (reference.identity == DOS_MEDIA_ID_NONE || reference.identity != selected->identity) {
        return DOS_MEDIA_STALE;
    }
    // Translate one-based tracks and zero-based sectors using raw-image
    // geometry, including D82's second-side tracks. Reject invalid coordinates
    // before callbacks run. The geometry helper leaves offset unchanged on
    // failure. No filesystem link, BAM bit, or controller retry is interpreted.
    if (!diskimage_sector_offset(selected->io.image.type, track, sector, offset)) {
        return DOS_MEDIA_RANGE;
    }
    // Publish the resolved slot only after identity and geometry both pass.
    // Access remains serialized by the facade through the following callback.
    *media = selected;
    return DOS_MEDIA_OK;
}

// Read one complete raw sector through the referenced mounted slot.
// A failed storage callback may partially modify output.
dos_media_result_t dos_media_read(dos_media_bank_t* const bank, const dos_media_ref_t reference,
                                  const unsigned int track, const unsigned int sector,
                                  uint8_t output[DISKIMAGE_SECTOR_SIZE]) {
    // This host callback is the bounded sector boundary used by DOS, not an
    // emulated ROM controller job. Validate output and resolve media identity
    // and geometry before touching storage. Propagate those failures as-is.
    if (output == NULL) {
        return DOS_MEDIA_INVALID;
    }
    dos_media_slot_t* media;
    uint32_t offset;
    const dos_media_result_t result = resolve_sector(bank, reference, track, sector, &media, &offset);
    if (result != DOS_MEDIA_OK) {
        return result;
    }
    // Issue exactly one 256-byte read, without retry, caching, or staging a
    // second buffer for rollback. A false callback result becomes IO_ERROR,
    // even if output was partly filled. Bindings and dirty state are unchanged.
    return media->io.image.read(media->io.image.ctx, offset, output, DISKIMAGE_SECTOR_SIZE) ? DOS_MEDIA_OK
                                                                                            : DOS_MEDIA_IO_ERROR;
}

// Resolve a model's zero-based BAM part to its physical track and sector.
// Leave outputs unchanged if the model or part has no supported BAM location.
static dos_media_result_t bam_sector(const dos_model_t model, const unsigned int part, unsigned int* const track,
                                     unsigned int* const sector) {
    // ROM sources:
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_4040/tst2
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/begin
    // https://github.com/mist64/cbmsrc/blob/01bd60f162ef92212ef0cb67546ae8f42be34168/DOS_8250/romtbl
    //
    // ROM 4040 tst2:INITSU selects 18/0 and INITDR reads the BAM and saves
    // the disk ID. DOS 2.7 begin:CONFI1/CONFI2 copy romtbl:BMT80D/BMS80D
    // or BMT80S/BMS80S into BAMTRK/BAMSEC according to double/single-sided
    // mode. The 8050 BAM entries are 38/0 and 38/3. The 8250 adds 38/6
    // and 38/9, still on track 38. Both tables end in directory link 39/1,
    // which is not an additional BAM part.
    // All models use a prefix of the same sector sequence. Select the BAM
    // track and explicit part count together: one for 4040, two for 8050,
    // and four for 8250. This is not a generic "model track": DOS 2.7's
    // directory is on a different track. Callers supply non-NULL outputs.
    static const uint8_t sectors[] = {0, 3, 6, 9};
    unsigned int bam_track;
    unsigned int parts;
    switch (model) {
    case DOS_MODEL_4040:
        bam_track = DOS_BAM_TRACK_4040;
        parts = 1;
        break;
    case DOS_MODEL_8050:
        bam_track = DOS_BAM_TRACK_8050_8250;
        parts = 2;
        break;
    case DOS_MODEL_8250:
        bam_track = DOS_BAM_TRACK_8050_8250;
        parts = 4;
        break;
    default:
        return DOS_MEDIA_RANGE;
    }
    if (part >= parts) {
        return DOS_MEDIA_RANGE;
    }

    // Publish both coordinates only after the part is valid. This chooses a
    // location, without ROM initialization, seek, disk-ID extraction, or I/O.
    *track = bam_track;
    *sector = sectors[part];
    return DOS_MEDIA_OK;
}

// Read one model-specific BAM sector on demand using a zero-based part index.
// This is raw access, not filesystem validation, allocation, or BAM repair.
dos_media_result_t dos_media_read_bam(dos_media_bank_t* const bank, const dos_media_ref_t reference,
                                      const unsigned int part, uint8_t output[DISKIMAGE_SECTOR_SIZE]) {
    // bam_sector encapsulates the ROM-derived BAM locations, with source
    // references beside the shared table. Validate the unit before selecting its
    // model. An unconfigured model or invalid part returns RANGE before media
    // reference validation, preserving this entry point's error precedence.
    if (bank == NULL || reference.unit >= DOS_MEDIA_UNIT_COUNT) {
        return DOS_MEDIA_INVALID;
    }
    unsigned int track;
    unsigned int sector;
    const dos_media_result_t result = bam_sector(bank->units[reference.unit].model, part, &track, &sector);
    if (result != DOS_MEDIA_OK) {
        return result;
    }

    // Read the selected coordinates through the ordinary identity/geometry
    // guards and one-sector callback. No separate BAM cache or repair occurs.
    return dos_media_read(bank, reference, track, sector, output);
}

// Attempt one raw sector write, retaining dirty state even on partial failure.
// Success means callback completion, not a flush or native DOS acknowledgement.
dos_media_result_t dos_media_write(dos_media_bank_t* const bank, const dos_media_ref_t reference,
                                   const unsigned int track, const unsigned int sector,
                                   const uint8_t input[DISKIMAGE_SECTOR_SIZE]) {
    // This is host storage I/O, not ROM file writing or controller write-verify.
    // Validate input, current identity, and geometry before checking the writer,
    // so stale/range failures cannot reach a replacement or read-only image.
    if (input == NULL) {
        return DOS_MEDIA_INVALID;
    }

    dos_media_slot_t* media;
    uint32_t offset;
    const dos_media_result_t result = resolve_sector(bank, reference, track, sector, &media, &offset);
    if (result != DOS_MEDIA_OK) {
        return result;
    }

    // A missing writer means read-only. Do not mark dirty or invoke a callback
    // in that case. This result is not itself native WRITE PROTECT status 26.
    if (media->io.image.write == NULL) {
        return DOS_MEDIA_READ_ONLY;
    }

    // Mark dirty before invoking the callback because it can buffer or alter
    // bytes and still fail. Keep dirty on either result so later flush/rebind
    // cannot silently discard a partial write. There is no rollback or retry.
    media->dirty = true;

    return media->io.image.write(media->io.image.ctx, offset, input, DISKIMAGE_SECTOR_SIZE) ? DOS_MEDIA_OK
                                                                                            : DOS_MEDIA_IO_ERROR;
}

// Flush every unit before detaching any media. Leave all bindings on failure.
// The facade must first quiesce DOS work and later close its retired handles.
dos_media_result_t dos_media_unmount_all(dos_media_bank_t* const bank) {
    // Bulk teardown is a host lifecycle operation, not a native reset command.
    // First flush all units without withdrawing any binding. A late failure
    // retains every descriptor/identity and any still-dirty state, while
    // earlier successful flushes remain real progress and are not rolled back.
    if (bank == NULL) {
        return DOS_MEDIA_INVALID;
    }

    for (unsigned int unit = 0; unit < DOS_MEDIA_UNIT_COUNT; ++unit) {
        const dos_media_result_t result = dos_media_flush_unit(bank, unit);
        if (result != DOS_MEDIA_OK) {
            return result;
        }
    }

    // Only complete flush success allows detachment. Clearing units removes
    // their descriptors, models, dirty flags, and slot identities, making old
    // references stale without closing borrowed handles. Keep bank->generation
    // unchanged so a later remount cannot reuse an old identity.
    for (unsigned int unit = 0; unit < DOS_MEDIA_UNIT_COUNT; ++unit) {
        bank->units[unit] = (dos_media_unit_t){0};
    }

    return DOS_MEDIA_OK;
}

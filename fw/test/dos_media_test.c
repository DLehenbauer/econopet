// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "dos_media_test.h"
#include "pch.h"

#include <limits.h>
#include <string.h>

#include "ieee/dos_media.h"

typedef struct {
    unsigned int reads;
    unsigned int writes;
    unsigned int flushes;
    uint32_t offset;
    uint8_t value;
    bool fail_read;
    bool fail_write;
    bool fail_flush;
} storage_t;

static const uint32_t image_sizes[] = {
    DISKIMAGE_D64_SIZE,
    DISKIMAGE_D80_SIZE,
    DISKIMAGE_D82_SIZE,
};
static const dos_model_t models[] = {
    DOS_MODEL_4040,
    DOS_MODEL_8050,
    DOS_MODEL_8250,
};

// Records sector reads without allocating an image-sized test buffer.
static bool read_storage(void* const context, const uint32_t offset, void* const output, const size_t length) {
    storage_t* const storage = context;
    ck_assert_uint_eq(length, DISKIMAGE_SECTOR_SIZE);
    ++storage->reads;
    storage->offset = offset;
    memset(output, storage->value, length);
    return !storage->fail_read;
}

// Models a write that can change backing bytes before reporting failure.
static bool write_storage(void* const context, const uint32_t offset, const void* const input, const size_t length) {
    storage_t* const storage = context;
    ck_assert_uint_eq(length, DISKIMAGE_SECTOR_SIZE);
    ++storage->writes;
    storage->offset = offset;
    storage->value = *(const uint8_t*)input;
    return !storage->fail_write;
}

// Records flush attempts and independently injects persistence failures.
static bool flush_storage(void* const context) {
    storage_t* const storage = context;
    ++storage->flushes;
    return !storage->fail_flush;
}

// Creates a supported raw container with independently controlled I/O failures.
static dos_media_io_t make_media(storage_t* const storage, const unsigned int format) {
    dos_media_io_t media = {.flush = flush_storage};
    ck_assert(diskimage_open(&media.image, read_storage, storage, image_sizes[format]));
    media.image.write = write_storage;
    return media;
}

// Obtains a mounted reference and checks the full result, not a default value.
static dos_media_ref_t mounted_reference(const dos_media_bank_t* const bank, const unsigned int unit,
                                         const unsigned int slot) {
    dos_media_ref_t reference;
    ck_assert_int_eq(dos_media_reference(bank, unit, slot, &reference), DOS_MEDIA_OK);
    ck_assert_uint_ne(reference.identity, 0);
    ck_assert_uint_eq(reference.unit, unit);
    ck_assert_uint_eq(reference.slot, slot);
    return reference;
}

// Mounts a single disk into either slot without redirecting the empty partner.
START_TEST(test_single_slot_selects_model_and_keeps_partner_absent) {
    const unsigned int format = (unsigned int)_i / DOS_MEDIA_SLOTS_PER_UNIT;
    const unsigned int slot = (unsigned int)_i % DOS_MEDIA_SLOTS_PER_UNIT;
    storage_t storage = {.value = 0x5a};
    const dos_media_io_t media = make_media(&storage, format);
    dos_media_bank_t bank = {0};
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, slot, &media), DOS_MEDIA_OK);
    ck_assert_int_eq(bank.units[0].model, models[format]);
    const dos_media_ref_t reference = mounted_reference(&bank, 0, slot);
    dos_media_ref_t absent = reference;
    ck_assert_int_eq(dos_media_reference(&bank, 0, slot ^ 1u, &absent), DOS_MEDIA_ABSENT);
    ck_assert_uint_eq(absent.identity, reference.identity);
    ck_assert_uint_eq(absent.slot, reference.slot);
    uint8_t sector[DISKIMAGE_SECTOR_SIZE];
    ck_assert_int_eq(dos_media_read(&bank, reference, 1, 0, sector), DOS_MEDIA_OK);
    ck_assert_uint_eq(storage.offset, 0);
    for (size_t index = 0; index < sizeof(sector); ++index)
        ck_assert_uint_eq(sector[index], storage.value);
}
END_TEST

// Validates all three homogeneous pairs and all six ordered mixed pairs.
START_TEST(test_complete_pair_validation_is_side_effect_free) {
    const unsigned int first = (unsigned int)_i / count_of(image_sizes);
    const unsigned int second = (unsigned int)_i % count_of(image_sizes);
    storage_t storage[2] = {0};
    const dos_media_io_t pair[] = {
        make_media(&storage[0], first),
        make_media(&storage[1], second),
    };
    dos_model_t model = DOS_MODEL_NONE;
    const dos_media_result_t expected = first == second ? DOS_MEDIA_OK : DOS_MEDIA_MIXED_PAIR;
    ck_assert_int_eq(dos_media_validate_pair(pair, &model), expected);
    ck_assert_int_eq(model, first == second ? models[first] : DOS_MODEL_NONE);
    for (unsigned int slot = 0; slot < count_of(storage); ++slot) {
        ck_assert_uint_eq(storage[slot].reads, 0);
        ck_assert_uint_eq(storage[slot].writes, 0);
        ck_assert_uint_eq(storage[slot].flushes, 0);
    }

    // Exercise publication as well as preflight against a live, dirty old mount.
    storage_t old = {0};
    const dos_media_io_t original = make_media(&old, 0);
    dos_media_bank_t bank = {0};
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, &original), DOS_MEDIA_OK);
    const dos_media_ref_t reference = mounted_reference(&bank, 0, 0);
    const uint8_t sector[DISKIMAGE_SECTOR_SIZE] = {0};
    ck_assert_int_eq(dos_media_write(&bank, reference, 1, 0, sector), DOS_MEDIA_OK);
    ck_assert_int_eq(dos_media_set_unit(&bank, 0, pair), expected);
    if (first == second) {
        ck_assert_int_eq(bank.units[0].model, models[first]);
        ck_assert_uint_eq(old.flushes, 1);
        for (unsigned int slot = 0; slot < DOS_MEDIA_SLOTS_PER_UNIT; ++slot) {
            ck_assert_uint_ne(bank.units[0].slots[slot].identity, reference.identity);
            ck_assert_ptr_eq(bank.units[0].slots[slot].io.image.ctx, &storage[slot]);
        }
    } else {
        ck_assert_int_eq(bank.units[0].model, DOS_MODEL_4040);
        ck_assert_uint_eq(old.flushes, 0);
        ck_assert_uint_eq(bank.units[0].slots[0].identity, reference.identity);
        ck_assert(bank.units[0].slots[0].dirty);
        ck_assert_ptr_eq(bank.units[0].slots[0].io.image.ctx, &old);
        ck_assert_uint_eq(bank.units[0].slots[1].identity, 0);
    }
}
END_TEST

// Invalid pairs cannot flush or replace the old unit, including pending writes.
START_TEST(test_rejected_pair_retains_dirty_mount_and_identity) {
    storage_t old = {0}, proposed = {0};
    const dos_media_io_t d64 = make_media(&old, 0);
    const dos_media_io_t d80 = make_media(&proposed, 1);
    dos_media_bank_t bank = {0};
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, &d64), DOS_MEDIA_OK);
    const dos_media_ref_t reference = mounted_reference(&bank, 0, 0);
    const uint8_t sector[DISKIMAGE_SECTOR_SIZE] = {0x21};
    ck_assert_int_eq(dos_media_write(&bank, reference, 1, 0, sector), DOS_MEDIA_OK);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &d80), DOS_MEDIA_MIXED_PAIR);
    ck_assert_uint_eq(old.flushes, 0);
    ck_assert(bank.units[0].slots[0].dirty);
    ck_assert_int_eq(bank.units[0].model, DOS_MODEL_4040);
    ck_assert_uint_eq(bank.units[0].slots[0].identity, reference.identity);
    ck_assert_uint_eq(bank.units[0].slots[1].identity, 0);
    ck_assert_uint_eq(bank.generation, reference.identity);
}
END_TEST

// Unsupported sizes and inconsistent descriptors fail before any storage I/O.
START_TEST(test_invalid_descriptors_are_not_mounts) {
    storage_t storage = {0};
    dos_media_io_t pair[2] = {make_media(&storage, 0), {0}};
    const dos_media_io_t valid = pair[0];
    const uint32_t invalid_sizes[] = {
        0,
        DISKIMAGE_D64_SIZE - 1,
        DISKIMAGE_D64_SIZE + 1,
        DISKIMAGE_D64_SIZE + DISKIMAGE_D64_SIZE / DISKIMAGE_SECTOR_SIZE,
        DISKIMAGE_D80_SIZE - 1,
        DISKIMAGE_D80_SIZE + 1,
        DISKIMAGE_D80_SIZE + DISKIMAGE_D80_SIZE / DISKIMAGE_SECTOR_SIZE,
        DISKIMAGE_D82_SIZE - 1,
        DISKIMAGE_D82_SIZE + 1,
        DISKIMAGE_D82_SIZE + DISKIMAGE_D82_SIZE / DISKIMAGE_SECTOR_SIZE,
    };
    dos_model_t model = DOS_MODEL_8250;
    for (unsigned int index = 0; index < count_of(invalid_sizes); ++index) {
        pair[0].image.size = invalid_sizes[index];
        ck_assert_int_eq(dos_media_validate_pair(pair, &model), DOS_MEDIA_UNSUPPORTED);
        ck_assert_int_eq(model, DOS_MODEL_8250);
    }
    pair[0] = valid;
    pair[0].image.type = diskimage_type_d80;
    ck_assert_int_eq(dos_media_validate_pair(pair, &model), DOS_MEDIA_UNSUPPORTED);
    pair[0] = valid;
    pair[0].image.read = NULL;
    ck_assert_int_eq(dos_media_validate_pair(pair, &model), DOS_MEDIA_UNSUPPORTED);
    pair[0] = valid;
    pair[0].flush = NULL;
    ck_assert_int_eq(dos_media_validate_pair(pair, &model), DOS_MEDIA_INVALID);
    pair[0] = valid;
    pair[1].image.ctx = &storage;
    ck_assert_int_eq(dos_media_validate_pair(pair, &model), DOS_MEDIA_INVALID);
    ck_assert_uint_eq(storage.reads + storage.writes + storage.flushes, 0);
}
END_TEST

// Replacing one disk invalidates its entire unit, but not another unit's jobs.
START_TEST(test_remount_invalidates_unit_without_cross_unit_reset) {
    storage_t storage[4] = {{.value = 1}, {.value = 2}, {.value = 3}, {.value = 4}};
    dos_media_io_t pair[] = {make_media(&storage[0], 0), make_media(&storage[1], 0)};
    dos_media_bank_t bank = {0};
    ck_assert_int_eq(dos_media_set_unit(&bank, 0, pair), DOS_MEDIA_OK);
    pair[0] = make_media(&storage[2], 1);
    pair[1] = (dos_media_io_t){0};
    ck_assert_int_eq(dos_media_set_unit(&bank, 1, pair), DOS_MEDIA_OK);
    const dos_media_ref_t old0 = mounted_reference(&bank, 0, 0);
    const dos_media_ref_t old1 = mounted_reference(&bank, 0, 1);
    const dos_media_ref_t other = mounted_reference(&bank, 1, 0);
    const dos_media_io_t replacement = make_media(&storage[3], 0);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, &replacement), DOS_MEDIA_OK);
    uint8_t sector[DISKIMAGE_SECTOR_SIZE] = {0};
    ck_assert_int_eq(dos_media_read(&bank, old0, 1, 0, sector), DOS_MEDIA_STALE);
    ck_assert_int_eq(dos_media_write(&bank, old1, 1, 0, sector), DOS_MEDIA_STALE);
    ck_assert_uint_eq(storage[0].reads + storage[1].writes + storage[3].reads, 0);
    ck_assert_int_eq(dos_media_read(&bank, other, 1, 0, sector), DOS_MEDIA_OK);
    ck_assert_uint_eq(sector[0], storage[2].value);
    ck_assert_int_eq(bank.units[1].model, DOS_MODEL_8050);
    const dos_media_ref_t current = mounted_reference(&bank, 0, 0);
    ck_assert_uint_ne(current.identity, old0.identity);
    ck_assert_int_eq(dos_media_read(&bank, current, 1, 0, sector), DOS_MEDIA_OK);
    ck_assert_uint_eq(sector[0], storage[3].value);
}
END_TEST

// Removing the last disk makes only its unit absent, without losing peer work.
START_TEST(test_unit_removal_preserves_other_unit_and_does_not_redirect) {
    storage_t storage[2] = {{.value = 0x18}, {.value = 0x29}};
    const dos_media_io_t first = make_media(&storage[0], 0);
    const dos_media_io_t second = make_media(&storage[1], 2);
    dos_media_bank_t bank = {0};
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &first), DOS_MEDIA_OK);
    ck_assert_int_eq(dos_media_set_slot(&bank, 3, 0, &second), DOS_MEDIA_OK);
    const dos_media_ref_t removed = mounted_reference(&bank, 0, 1);
    const dos_media_ref_t peer = mounted_reference(&bank, 3, 0);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, NULL), DOS_MEDIA_OK);
    ck_assert_int_eq(bank.units[0].model, DOS_MODEL_NONE);
    ck_assert_int_eq(bank.units[3].model, DOS_MODEL_8250);
    dos_media_ref_t absent = removed;
    ck_assert_int_eq(dos_media_reference(&bank, 0, 0, &absent), DOS_MEDIA_ABSENT);
    ck_assert_int_eq(dos_media_reference(&bank, 0, 1, &absent), DOS_MEDIA_ABSENT);
    uint8_t sector[DISKIMAGE_SECTOR_SIZE];
    ck_assert_int_eq(dos_media_read(&bank, removed, 1, 0, sector), DOS_MEDIA_STALE);
    ck_assert_uint_eq(storage[0].reads, 0);
    ck_assert_int_eq(dos_media_read(&bank, peer, 78, 0, sector), DOS_MEDIA_OK);
    ck_assert_uint_eq(sector[0], storage[1].value);
    ck_assert_uint_eq(storage[1].offset, DISKIMAGE_D80_SIZE);
}
END_TEST

// A rejected replacement retains the old mount and dirty bytes until recovery.
START_TEST(test_partial_write_and_flush_failure_retain_old_media) {
    storage_t old = {.fail_write = true, .fail_flush = true}, replacement = {0};
    const dos_media_io_t first = make_media(&old, 0);
    const dos_media_io_t second = make_media(&replacement, 1);
    dos_media_bank_t bank = {0};
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, &first), DOS_MEDIA_OK);
    const dos_media_ref_t reference = mounted_reference(&bank, 0, 0);
    uint8_t sector[DISKIMAGE_SECTOR_SIZE] = {0x67};
    ck_assert_int_eq(dos_media_write(&bank, reference, 1, 0, sector), DOS_MEDIA_IO_ERROR);
    ck_assert(bank.units[0].slots[0].dirty);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, &second), DOS_MEDIA_IO_ERROR);
    ck_assert_uint_eq(old.flushes, 1);
    ck_assert(bank.units[0].slots[0].dirty);
    ck_assert_uint_eq(bank.units[0].slots[0].identity, reference.identity);
    ck_assert_int_eq(bank.units[0].model, DOS_MODEL_4040);
    ck_assert_int_eq(dos_media_read(&bank, reference, 1, 0, sector), DOS_MEDIA_OK);
    ck_assert_uint_eq(sector[0], 0x67);
    old.fail_flush = false;
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, &second), DOS_MEDIA_OK);
    ck_assert_uint_eq(old.flushes, 2);
    ck_assert(!bank.units[0].slots[0].dirty);
    ck_assert_int_eq(bank.units[0].model, DOS_MODEL_8050);
    ck_assert_int_eq(dos_media_read(&bank, reference, 1, 0, sector), DOS_MEDIA_STALE);
    ck_assert_uint_eq(replacement.reads, 0);
}
END_TEST

// Flush progress is real, but a later failure must not detach either old disk.
START_TEST(test_second_slot_flush_failure_keeps_complete_pair) {
    storage_t storage[2] = {{0}, {.fail_flush = true}};
    const dos_media_io_t pair[] = {make_media(&storage[0], 2), make_media(&storage[1], 2)};
    dos_media_bank_t bank = {0};
    ck_assert_int_eq(dos_media_set_unit(&bank, 0, pair), DOS_MEDIA_OK);
    const dos_media_ref_t references[] = {
        mounted_reference(&bank, 0, 0),
        mounted_reference(&bank, 0, 1),
    };
    const uint8_t sector[DISKIMAGE_SECTOR_SIZE] = {0x38};
    for (unsigned int slot = 0; slot < count_of(references); ++slot)
        ck_assert_int_eq(dos_media_write(&bank, references[slot], 78, 0, sector), DOS_MEDIA_OK);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, NULL), DOS_MEDIA_IO_ERROR);
    ck_assert(!bank.units[0].slots[0].dirty);
    ck_assert(bank.units[0].slots[1].dirty);
    ck_assert_int_eq(bank.units[0].model, DOS_MODEL_8250);
    for (unsigned int slot = 0; slot < count_of(references); ++slot) {
        ck_assert_uint_eq(bank.units[0].slots[slot].identity, references[slot].identity);
        ck_assert_uint_eq(storage[slot].flushes, 1);
    }
}
END_TEST

// Full teardown keeps all eight handles on failure and never reuses old IDs.
START_TEST(test_bulk_teardown_retains_bindings_then_invalidates_jobs) {
    dos_media_bank_t bank = {0};
    storage_t storage[DOS_MEDIA_UNIT_COUNT][DOS_MEDIA_SLOTS_PER_UNIT] = {0};
    dos_media_ref_t references[DOS_MEDIA_UNIT_COUNT][DOS_MEDIA_SLOTS_PER_UNIT];
    const uint8_t sector[DISKIMAGE_SECTOR_SIZE] = {0x49};
    for (unsigned int unit = 0; unit < DOS_MEDIA_UNIT_COUNT; ++unit) {
        const unsigned int format = unit % count_of(image_sizes);
        const dos_media_io_t pair[] = {
            make_media(&storage[unit][0], format),
            make_media(&storage[unit][1], format),
        };
        ck_assert_int_eq(dos_media_set_unit(&bank, unit, pair), DOS_MEDIA_OK);
        for (unsigned int slot = 0; slot < DOS_MEDIA_SLOTS_PER_UNIT; ++slot) {
            references[unit][slot] = mounted_reference(&bank, unit, slot);
            ck_assert_int_eq(dos_media_write(&bank, references[unit][slot], 1, 0, sector), DOS_MEDIA_OK);
        }
    }
    storage[2][1].fail_flush = true;
    ck_assert_int_eq(dos_media_unmount_all(&bank), DOS_MEDIA_IO_ERROR);
    for (unsigned int unit = 0; unit < DOS_MEDIA_UNIT_COUNT; ++unit)
        for (unsigned int slot = 0; slot < DOS_MEDIA_SLOTS_PER_UNIT; ++slot)
            ck_assert_uint_eq(bank.units[unit].slots[slot].identity, references[unit][slot].identity);
    ck_assert(bank.units[2].slots[1].dirty);
    ck_assert(bank.units[3].slots[0].dirty);
    storage[2][1].fail_flush = false;
    ck_assert_int_eq(dos_media_unmount_all(&bank), DOS_MEDIA_OK);
    for (unsigned int unit = 0; unit < DOS_MEDIA_UNIT_COUNT; ++unit) {
        ck_assert_int_eq(bank.units[unit].model, DOS_MODEL_NONE);
        for (unsigned int slot = 0; slot < DOS_MEDIA_SLOTS_PER_UNIT; ++slot)
            ck_assert_uint_eq(bank.units[unit].slots[slot].identity, 0);
    }
    const dos_media_io_t remount = make_media(&storage[0][0], 0);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, &remount), DOS_MEDIA_OK);
    ck_assert_uint_gt(bank.units[0].slots[0].identity, references[3][1].identity);
    ck_assert_int_eq(dos_media_write(&bank, references[0][0], 1, 0, sector), DOS_MEDIA_STALE);
}
END_TEST

// Invalid coordinates, read-only media, and failed reads retain binding state.
START_TEST(test_sector_bounds_and_read_only_fail_before_write) {
    storage_t storage = {0};
    dos_media_io_t media = make_media(&storage, 2);
    media.image.write = NULL;
    media.flush = NULL;
    dos_media_bank_t bank = {0};
    ck_assert_int_eq(dos_media_set_slot(&bank, 3, 1, &media), DOS_MEDIA_OK);
    const dos_media_ref_t reference = mounted_reference(&bank, 3, 1);
    uint8_t sector[DISKIMAGE_SECTOR_SIZE] = {0};
    ck_assert_int_eq(dos_media_read(&bank, reference, 0, 0, sector), DOS_MEDIA_RANGE);
    ck_assert_int_eq(dos_media_read(&bank, reference, 155, 0, sector), DOS_MEDIA_RANGE);
    ck_assert_int_eq(dos_media_read(&bank, reference, 154, 23, sector), DOS_MEDIA_RANGE);
    ck_assert_uint_eq(storage.reads, 0);
    ck_assert_int_eq(dos_media_read(&bank, reference, 154, 22, sector), DOS_MEDIA_OK);
    ck_assert_uint_eq(storage.offset, DISKIMAGE_D82_SIZE - DISKIMAGE_SECTOR_SIZE);
    ck_assert_int_eq(dos_media_write(&bank, reference, 1, 0, sector), DOS_MEDIA_READ_ONLY);
    ck_assert_uint_eq(storage.writes, 0);
    ck_assert(!bank.units[3].slots[1].dirty);
    storage.fail_read = true;
    ck_assert_int_eq(dos_media_read(&bank, reference, 78, 0, sector), DOS_MEDIA_IO_ERROR);
    ck_assert_uint_eq(storage.offset, DISKIMAGE_D80_SIZE);
    ck_assert_uint_eq(bank.units[3].slots[1].identity, reference.identity);
    ck_assert_int_eq(dos_media_unmount_all(&bank), DOS_MEDIA_OK);
    ck_assert_uint_eq(storage.flushes, 0);
}
END_TEST

// Identity exhaustion rejects new bindings without wrapping into an old job.
START_TEST(test_identity_exhaustion_preserves_old_mount) {
    storage_t storage = {0};
    const dos_media_io_t media = make_media(&storage, 0);
    dos_media_bank_t bank = {.generation = UINT32_MAX - 1};
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, &media), DOS_MEDIA_OK);
    const dos_media_ref_t reference = mounted_reference(&bank, 0, 0);
    ck_assert_uint_eq(reference.identity, UINT32_MAX);
    const uint8_t sector[DISKIMAGE_SECTOR_SIZE] = {0};
    ck_assert_int_eq(dos_media_write(&bank, reference, 1, 0, sector), DOS_MEDIA_OK);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, &media), DOS_MEDIA_ID_EXHAUSTED);
    ck_assert_uint_eq(storage.flushes, 0);
    ck_assert(bank.units[0].slots[0].dirty);
    ck_assert_uint_eq(bank.units[0].slots[0].identity, reference.identity);
    ck_assert_int_eq(dos_media_unmount_all(&bank), DOS_MEDIA_OK);
    ck_assert_uint_eq(bank.generation, UINT32_MAX);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, &media), DOS_MEDIA_ID_EXHAUSTED);
}
END_TEST

// Bad API inputs do not mutate valid output or touch storage.
START_TEST(test_invalid_api_inputs_preserve_outputs) {
    dos_media_bank_t bank = {0};
    const dos_media_io_t empty[2] = {0};
    dos_model_t model = DOS_MODEL_8250;
    ck_assert_int_eq(dos_media_validate_pair(NULL, &model), DOS_MEDIA_INVALID);
    ck_assert_int_eq(model, DOS_MODEL_8250);
    ck_assert_int_eq(dos_media_validate_pair(empty, NULL), DOS_MEDIA_INVALID);
    ck_assert_int_eq(dos_media_set_unit(&bank, DOS_MEDIA_UNIT_COUNT, empty), DOS_MEDIA_INVALID);
    ck_assert_int_eq(dos_media_set_unit(&bank, 0, NULL), DOS_MEDIA_INVALID);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, DOS_MEDIA_SLOTS_PER_UNIT, NULL), DOS_MEDIA_INVALID);
    dos_media_ref_t reference = {.identity = 7, .unit = 3, .slot = 1};
    ck_assert_int_eq(dos_media_reference(&bank, DOS_MEDIA_UNIT_COUNT, 0, &reference), DOS_MEDIA_INVALID);
    ck_assert_int_eq(dos_media_reference(&bank, 0, 0, &reference), DOS_MEDIA_ABSENT);
    ck_assert_uint_eq(reference.identity, 7);
    ck_assert_uint_eq(reference.unit, 3);
    ck_assert_uint_eq(reference.slot, 1);
    uint8_t sector[DISKIMAGE_SECTOR_SIZE] = {0};
    ck_assert_int_eq(dos_media_read(&bank, reference, 1, 0, sector), DOS_MEDIA_STALE);
    ck_assert_int_eq(dos_media_read(&bank, reference, 1, 0, NULL), DOS_MEDIA_INVALID);
    ck_assert_int_eq(dos_media_write(&bank, reference, 1, 0, NULL), DOS_MEDIA_INVALID);
    reference.unit = DOS_MEDIA_UNIT_COUNT;
    ck_assert_int_eq(dos_media_write(&bank, reference, 1, 0, sector), DOS_MEDIA_INVALID);
    ck_assert_uint_eq(bank.generation, 0);
}
END_TEST

// Model-specific BAM access is one sector callback, not an image-sized cache.
START_TEST(test_model_bam_access_is_bounded_and_media_bound) {
    const unsigned int format = (unsigned int)_i;
    storage_t storage = {.value = 0x55};
    const dos_media_io_t media = make_media(&storage, format);
    dos_media_bank_t bank = {0};
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &media), DOS_MEDIA_OK);
    const dos_media_ref_t reference = mounted_reference(&bank, 0, 1);
    const unsigned int parts = 1u << format;
    const uint32_t first_offset = format == 0 ? 357 * DISKIMAGE_SECTOR_SIZE : 37 * 29 * DISKIMAGE_SECTOR_SIZE;
    uint8_t sector[DISKIMAGE_SECTOR_SIZE];
    for (unsigned int part = 0; part < parts; ++part) {
        ck_assert_int_eq(dos_media_read_bam(&bank, reference, part, sector), DOS_MEDIA_OK);
        ck_assert_uint_eq(storage.reads, part + 1);
        ck_assert_uint_eq(storage.offset, first_offset + part * 3 * DISKIMAGE_SECTOR_SIZE);
        for (unsigned int byte = 0; byte < sizeof(sector); ++byte)
            ck_assert_uint_eq(sector[byte], storage.value);
    }
    ck_assert_int_eq(dos_media_read_bam(&bank, reference, parts, sector), DOS_MEDIA_RANGE);
    ck_assert_int_eq(dos_media_read_bam(&bank, reference, UINT_MAX, sector), DOS_MEDIA_RANGE);
    ck_assert_int_eq(dos_media_read_bam(&bank, reference, 0, NULL), DOS_MEDIA_INVALID);
    ck_assert_uint_eq(storage.reads, parts);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &media), DOS_MEDIA_OK);
    ck_assert_int_eq(dos_media_read_bam(&bank, reference, 0, sector), DOS_MEDIA_STALE);
    ck_assert_uint_eq(storage.reads, parts);
}
END_TEST

// An unconfigured unit has no BAM, and invalid units never reach storage.
START_TEST(test_bam_rejects_unconfigured_and_invalid_units) {
    dos_media_bank_t bank = {0};
    dos_media_ref_t reference = {0};
    uint8_t sector[DISKIMAGE_SECTOR_SIZE] = {0};
    ck_assert_int_eq(dos_media_read_bam(&bank, reference, 0, sector), DOS_MEDIA_RANGE);
    ck_assert_int_eq(dos_media_read_bam(NULL, reference, 0, sector), DOS_MEDIA_INVALID);
    reference.unit = DOS_MEDIA_UNIT_COUNT;
    ck_assert_int_eq(dos_media_read_bam(&bank, reference, 0, sector), DOS_MEDIA_INVALID);
}
END_TEST

// Registers standalone media-foundation tests, independent of the legacy DOS.
Suite* dos_media_suite(void) {
    Suite* const suite = suite_create("dos_media");
    TCase* const core = tcase_create("core");
    tcase_add_loop_test(core, test_single_slot_selects_model_and_keeps_partner_absent, 0,
                        count_of(image_sizes) * DOS_MEDIA_SLOTS_PER_UNIT);
    tcase_add_loop_test(core, test_complete_pair_validation_is_side_effect_free, 0,
                        count_of(image_sizes) * count_of(image_sizes));
    tcase_add_test(core, test_rejected_pair_retains_dirty_mount_and_identity);
    tcase_add_test(core, test_invalid_descriptors_are_not_mounts);
    tcase_add_test(core, test_remount_invalidates_unit_without_cross_unit_reset);
    tcase_add_test(core, test_unit_removal_preserves_other_unit_and_does_not_redirect);
    tcase_add_test(core, test_partial_write_and_flush_failure_retain_old_media);
    tcase_add_test(core, test_second_slot_flush_failure_keeps_complete_pair);
    tcase_add_test(core, test_bulk_teardown_retains_bindings_then_invalidates_jobs);
    tcase_add_test(core, test_sector_bounds_and_read_only_fail_before_write);
    tcase_add_test(core, test_identity_exhaustion_preserves_old_mount);
    tcase_add_test(core, test_invalid_api_inputs_preserve_outputs);
    tcase_add_loop_test(core, test_model_bam_access_is_bounded_and_media_bound, 0, 3);
    tcase_add_test(core, test_bam_rejects_unconfigured_and_invalid_units);
    suite_add_tcase(suite, core);
    return suite;
}

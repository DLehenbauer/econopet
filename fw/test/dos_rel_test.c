// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"

#include "dos_rel_test.h"

#include <string.h>

#include "ieee/dos.h"
#include "ieee/dos_rel.h"

typedef struct {
    uint8_t bytes[7][DISKIMAGE_SECTOR_SIZE];
    uint32_t offsets[7];
    unsigned int reads;
    unsigned int fail_read;
} rel_image_t;

static const dos_model_t models[] = {DOS_MODEL_4040, DOS_MODEL_8050, DOS_MODEL_8250};
static const uint32_t sizes[] = {DISKIMAGE_D64_SIZE, DISKIMAGE_D80_SIZE, DISKIMAGE_D82_SIZE};

// Supply only explicit index/data pages. Any unplanned traversal fails.
static bool read_index(void* const context, const uint32_t offset, void* const output, const size_t length) {
    rel_image_t* const image = context;
    ck_assert_uint_eq(length, DISKIMAGE_SECTOR_SIZE);
    ++image->reads;
    if (image->reads == image->fail_read) {
        memset(output, 0xee, length / 2u);
        return false;
    }
    for (unsigned int page = 0; page < count_of(image->bytes); ++page) {
        if (image->offsets[page] == offset) {
            memcpy(output, image->bytes[page], length);
            return true;
        }
    }
    ck_abort_msg("unexpected REL index read at offset %u", offset);
    return false;
}

// Bind two side pages, one super-side page and four native data pages.
static dos_media_ref_t mount_index(dos_media_bank_t* const bank, rel_image_t* const image, const unsigned int format,
                                   const unsigned int unit, const unsigned int slot) {
    dos_media_io_t io = {0};
    ck_assert(diskimage_open(&io.image, read_index, image, sizes[format]));
    for (unsigned int page = 0; page < count_of(image->bytes); ++page) {
        ck_assert(diskimage_sector_offset(io.image.type, 1, page, &image->offsets[page]));
    }
    ck_assert_int_eq(dos_media_set_slot(bank, unit, slot, &io), DOS_MEDIA_OK);
    dos_media_ref_t reference;
    ck_assert_int_eq(dos_media_reference(bank, unit, slot, &reference), DOS_MEDIA_OK);
    return reference;
}

// Assert concrete FNDREL boundaries, including native small-file byte truncation.
START_TEST(test_record_address_boundaries) {
    const dos_model_t model = models[_i];
    static const struct {
        uint16_t record;
        uint8_t size, position, group, side, index, offset;
        bool overflow;
    } cases[] = {
        {0, 127, 0, 0, 0, 16, 2, false},
        {1, 127, 1, 0, 0, 16, 2, false},
        {2, 127, 127, 0, 0, 16, 255, false},
        {3, 127, 0, 0, 0, 18, 2, false},
        {240, 127, 127, 0, 0, 254, 255, false},
        {241, 127, 1, 0, 1, 16, 2, false},
        {1440, 127, 127, 0, 5, 254, 255, false},
        {1441, 127, 1, 1, 0, 16, 2, false},
        {61441, 127, 1, 42, 4, 16, 2, false},
        {65535, 255, 255, 91, 2, 82, 4, false},
        {2, 10, 11, 0, 0, 16, 12, true},
        {1, 0, 0, 0, 0, 16, 2, false},
        {1, 0, 2, 0, 0, 16, 2, true},
    };
    for (unsigned int test = 0; test < count_of(cases); ++test) {
        const dos_rel_address_t address =
            dos_rel_address(model, cases[test].record, cases[test].size, cases[test].position);
        ck_assert_uint_eq(address.group, model == DOS_MODEL_8250 ? cases[test].group : 0);
        const unsigned int side = cases[test].group * 6u + cases[test].side;
        ck_assert_uint_eq(address.side, model == DOS_MODEL_8250 ? cases[test].side : (uint8_t)side);
        ck_assert_uint_eq(address.index, cases[test].index);
        ck_assert_uint_eq(address.offset, cases[test].offset);
        ck_assert_int_eq(address.overflow, cases[test].overflow);
    }
    const dos_rel_address_t folded = dos_rel_address(model, 65534, 127, 0);
    ck_assert_uint_eq(folded.side, model == DOS_MODEL_4040 ? 16 : model == DOS_MODEL_8050 ? 17 : 3);
    ck_assert_uint_eq(folded.index, model == DOS_MODEL_4040 ? 12 : 28);
    ck_assert_uint_eq(folded.offset, 129);
}
END_TEST

// Resolve the first and last pointer of a group without consulting data links.
START_TEST(test_lookup_uses_side_tables_and_super_groups) {
    const unsigned int format = (unsigned int)_i / 4u;
    const unsigned int variant = (unsigned int)_i % 4u;
    const unsigned int unit = variant, slot = variant & 1u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_index(&bank, &image, format, unit, slot);
    const bool last = variant & 1u;
    const uint16_t record = last ? (format == 2 ? 2880u : 1440u) : (format == 2 ? 1441u : 1u);
    const dos_rel_address_t address = dos_rel_address(models[format], record, 127, last ? 127 : 1);
    const uint8_t side_offset = 4u + 2u * address.side;
    image.bytes[0][2] = 0;
    image.bytes[0][side_offset] = 1;
    image.bytes[0][side_offset + 1u] = last ? 1 : 0;
    image.bytes[0][address.index] = 35;
    image.bytes[0][address.index + 1u] = 0;
    image.bytes[1][2] = 5;
    image.bytes[1][address.index] = format == 2 ? 154 : format == 1 ? 77 : 35;
    image.bytes[1][address.index + 1u] = 16;
    // Deliberately different leading link: RDSS1 must use 3 + group * 2.
    image.bytes[2][0] = 2;
    image.bytes[2][1] = 7;
    image.bytes[2][2] = variant == 2 ? 0 : 254;
    image.bytes[2][3u + address.group * 2u] = 1;
    image.bytes[2][4u + address.group * 2u] = 0;
    dos_rel_lookup_t lookup;
    dos_rel_lookup_start(&lookup, models[format], reference, 1, format == 2 ? 2 : 0, address);
    uint8_t scratch[DISKIMAGE_SECTOR_SIZE];
    for (unsigned int step = 0; step < 3 && lookup.result == DOS_REL_LOOKUP_PENDING; ++step) {
        const unsigned int reads = image.reads;
        dos_rel_lookup_step(&lookup, &bank, scratch);
        ck_assert_uint_eq(image.reads, reads + 1u);
        memset(scratch, 0xa5, sizeof(scratch));
    }
    ck_assert_int_eq(lookup.result, DOS_REL_LOOKUP_FOUND);
    ck_assert_uint_eq(lookup.track, last ? (format == 2 ? 154 : format == 1 ? 77 : 35) : 35);
    ck_assert_uint_eq(lookup.sector, last ? 16 : 0);
    ck_assert_uint_eq(image.reads, 1u + last + (format == 2));
    // A terminal result never rereads a borrowed page or changes ownership.
    ck_assert_int_eq(dos_rel_lookup_step(&lookup, &bank, scratch), DOS_REL_LOOKUP_FOUND);
    ck_assert_uint_eq(image.reads, 1u + last + (format == 2));
}
END_TEST

// SSTEST accepts a matching resident number before applying the six-side bound.
START_TEST(test_lookup_native_wrapped_indices_and_last_group) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool last_record = (unsigned int)_i % 2u;
    const dos_rel_address_t address =
        dos_rel_address(models[format], last_record ? 65535 : 65534, last_record ? 255 : 127, last_record ? 255 : 0);
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_index(&bank, &image, format, 3, 1);
    image.bytes[0][2] = address.side;
    image.bytes[0][address.index] = 35;
    image.bytes[0][address.index + 1u] = 0;
    image.bytes[2][3u + address.group * 2u] = 1;
    dos_rel_lookup_t lookup;
    dos_rel_lookup_start(&lookup, models[format], reference, 1, format == 2 ? 2 : 0, address);
    uint8_t scratch[DISKIMAGE_SECTOR_SIZE];
    if (format == 2) {
        ck_assert_int_eq(dos_rel_lookup_step(&lookup, &bank, scratch), DOS_REL_LOOKUP_PENDING);
    }
    ck_assert_int_eq(dos_rel_lookup_step(&lookup, &bank, scratch), DOS_REL_LOOKUP_FOUND);
    ck_assert_uint_eq(lookup.track, 35);
    ck_assert_uint_eq(lookup.sector, 0);
}
END_TEST

// SSPOS repeats SSTEST after IBRD, accepting ER1 without following another side.
START_TEST(test_lookup_retests_selected_side_range) {
    const unsigned int format = (unsigned int)_i / 5u;
    const unsigned int variant = (unsigned int)_i % 5u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_index(&bank, &image, format, 0, 0);
    image.bytes[2][DOS_REL_GROUP_TABLE_OFFSET] = 1;
    image.bytes[0][DOS_REL_SIDE_TABLE_OFFSET + 2u] = 1;
    image.bytes[0][DOS_REL_SIDE_TABLE_OFFSET + 3u] = 1;
    image.bytes[1][DOS_REL_SIDE_NUMBER_OFFSET] = variant >= 3 ? 1 : 0;
    image.bytes[1][DOS_REL_SIDE_TABLE_OFFSET + 2u] = variant == 0 || variant == 3 ? 0 : 1;
    image.bytes[1][DOS_REL_SIDE_TABLE_OFFSET + 3u] = 6;
    image.bytes[1][DOS_REL_POINTER_OFFSET] = variant == 2 || variant == 4 ? 0 : 1;
    image.bytes[1][DOS_REL_POINTER_OFFSET + 1u] = 3;
    dos_rel_lookup_t lookup;
    dos_rel_lookup_start(&lookup, models[format], reference, 1, format == 2 ? 2 : 0,
                         dos_rel_address(models[format], 241, 127, 1));
    uint8_t scratch[DISKIMAGE_SECTOR_SIZE];
    for (unsigned int step = 0; lookup.result == DOS_REL_LOOKUP_PENDING; ++step) {
        ck_assert_uint_lt(step, 3);
        const unsigned int reads = image.reads;
        dos_rel_lookup_step(&lookup, &bank, scratch);
        ck_assert_uint_eq(image.reads, reads + 1u);
    }
    const bool missing = variant == 0 || variant == 4;
    ck_assert_int_eq(lookup.result, missing ? DOS_REL_LOOKUP_MISSING : DOS_REL_LOOKUP_FOUND);
    ck_assert_int_eq(lookup.media_result, DOS_MEDIA_OK);
    ck_assert_uint_eq(lookup.track, missing || variant == 2 ? 0 : 1);
    ck_assert_uint_eq(lookup.sector, missing ? 0 : 3);
    const unsigned int reads = 2u + (format == 2);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_int_eq(dos_rel_lookup_step(&lookup, &bank, scratch),
                     missing ? DOS_REL_LOOKUP_MISSING : DOS_REL_LOOKUP_FOUND);
    ck_assert_uint_eq(image.reads, reads);
}
END_TEST

// Missing group, side or data pointers differ from storage failures.
START_TEST(test_lookup_missing_ranges) {
    const unsigned int format = (unsigned int)_i / 4u;
    const unsigned int variant = (unsigned int)_i % 4u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_index(&bank, &image, format, 0, 0);
    const dos_rel_address_t address = dos_rel_address(models[format], variant == 3 ? 1441 : 241, 127, 1);
    image.bytes[2][3u + address.group * 2u] = variant == 0 ? 0 : 1;
    image.bytes[0][6] = variant == 1 ? 0 : 1;
    image.bytes[0][7] = 1;
    image.bytes[1][2] = 1;
    dos_rel_lookup_t lookup;
    dos_rel_lookup_start(&lookup, models[format], reference, 1, format == 2 ? 2 : 0, address);
    uint8_t scratch[DISKIMAGE_SECTOR_SIZE];
    for (unsigned int step = 0; step < 3 && lookup.result == DOS_REL_LOOKUP_PENDING; ++step) {
        dos_rel_lookup_step(&lookup, &bank, scratch);
    }
    ck_assert_int_eq(lookup.result, DOS_REL_LOOKUP_MISSING);
    ck_assert_int_eq(lookup.media_result, DOS_MEDIA_OK);
    ck_assert_uint_eq(lookup.track, 0);
    ck_assert_uint_eq(lookup.sector, 0);
}
END_TEST

// Every index read rejects partial storage data and stale media before publication.
START_TEST(test_lookup_failure_and_replacement) {
    const unsigned int format = (unsigned int)_i / 6u;
    const unsigned int variant = (unsigned int)_i % 6u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_index(&bank, &image, format, 2, 1);
    const dos_rel_address_t address = dos_rel_address(models[format], 241, 127, 1);
    image.bytes[2][3] = 1;
    image.bytes[0][6] = 1;
    image.bytes[0][7] = 1;
    image.bytes[1][2] = 1;
    image.bytes[1][16] = 5;
    image.bytes[1][17] = 7;
    dos_rel_lookup_t lookup;
    dos_rel_lookup_start(&lookup, models[format], reference, 1, format == 2 ? 2 : 0, address);
    const unsigned int completed = variant % (format == 2 ? 3u : 2u);
    uint8_t scratch[DISKIMAGE_SECTOR_SIZE];
    for (unsigned int step = 0; step < completed; ++step) {
        ck_assert_int_eq(dos_rel_lookup_step(&lookup, &bank, scratch), DOS_REL_LOOKUP_PENDING);
    }
    const unsigned int reads = image.reads;
    if (variant < 3) {
        image.fail_read = reads + 1u;
    } else {
        mount_index(&bank, &replacement, format, 2, 1);
    }
    ck_assert_int_eq(dos_rel_lookup_step(&lookup, &bank, scratch), DOS_REL_LOOKUP_FAILED);
    ck_assert_int_eq(lookup.media_result, variant < 3 ? DOS_MEDIA_IO_ERROR : DOS_MEDIA_STALE);
    ck_assert_uint_eq(lookup.track, 0);
    ck_assert_uint_eq(lookup.sector, 0);
    ck_assert_uint_eq(image.reads, reads + (variant < 3));
    ck_assert_uint_eq(replacement.reads, 0);
    ck_assert_int_eq(dos_rel_lookup_step(&lookup, &bank, scratch), DOS_REL_LOOKUP_FAILED);
    ck_assert_uint_eq(image.reads, reads + (variant < 3));
}
END_TEST

// Reject invalid root and table coordinates before consulting host storage.
START_TEST(test_lookup_geometry_errors) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool invalid_root = (unsigned int)_i % 2u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_index(&bank, &image, format, 0, 0);
    const uint8_t invalid_track = format == 0 ? 36 : format == 1 ? 78 : 155;
    image.bytes[0][6] = invalid_track;
    image.bytes[2][3] = invalid_track;
    dos_rel_lookup_t lookup;
    dos_rel_lookup_start(&lookup, models[format], reference, invalid_root ? invalid_track : 1, format == 2 ? 2 : 0,
                         dos_rel_address(models[format], 241, 127, 1));
    uint8_t scratch[DISKIMAGE_SECTOR_SIZE];
    if (!invalid_root) {
        ck_assert_int_eq(dos_rel_lookup_step(&lookup, &bank, scratch), DOS_REL_LOOKUP_PENDING);
    }
    ck_assert_int_eq(dos_rel_lookup_step(&lookup, &bank, scratch), DOS_REL_LOOKUP_FAILED);
    ck_assert_int_eq(lookup.media_result, DOS_MEDIA_RANGE);
    ck_assert_uint_eq(image.reads, !invalid_root);
    ck_assert_uint_eq(lookup.track, 0);
}
END_TEST

// Cancelling one job neither advances another unit nor keeps an old result alive.
START_TEST(test_lookup_cancel_restart_and_peer_isolation) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t first = {0}, peer = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t a = mount_index(&bank, &first, format, 0, 0);
    const dos_media_ref_t b = mount_index(&bank, &peer, format, 1, 1);
    first.bytes[0][16] = peer.bytes[0][16] = 1;
    first.bytes[0][17] = 3;
    peer.bytes[0][17] = 4;
    first.bytes[2][3] = peer.bytes[2][3] = 1;
    const dos_rel_address_t address = dos_rel_address(models[format], 1, 127, 0);
    dos_rel_lookup_t one, two;
    dos_rel_lookup_start(&one, models[format], a, 1, format == 2 ? 2 : 0, address);
    dos_rel_lookup_start(&two, models[format], b, 1, format == 2 ? 2 : 0, address);
    const dos_rel_lookup_t before = two;
    uint8_t scratch[DISKIMAGE_SECTOR_SIZE];
    dos_rel_lookup_start(&one, models[format], a, 1, format == 2 ? 2 : 0, dos_rel_address(models[format], 241, 127, 0));
    first.bytes[0][6] = 1;
    first.bytes[0][7] = 1;
    ck_assert_int_eq(dos_rel_lookup_step(&one, &bank, scratch), DOS_REL_LOOKUP_PENDING);
    dos_rel_lookup_cancel(&one);
    ck_assert_mem_eq(&two, &before, sizeof(before));
    ck_assert_int_eq(dos_rel_lookup_step(&one, &bank, scratch), DOS_REL_LOOKUP_IDLE);
    ck_assert_uint_eq(first.reads, 1);
    ck_assert_uint_eq(peer.reads, 0);
    dos_rel_lookup_start(&one, models[format], a, 1, format == 2 ? 2 : 0, address);
    for (unsigned int step = 0; step < 2; ++step) {
        dos_rel_lookup_step(&one, &bank, scratch);
        dos_rel_lookup_step(&two, &bank, scratch);
    }
    ck_assert_int_eq(one.result, DOS_REL_LOOKUP_FOUND);
    ck_assert_int_eq(two.result, DOS_REL_LOOKUP_FOUND);
    ck_assert_uint_eq(one.sector, 3);
    ck_assert_uint_eq(two.sector, 4);
    // Completion does not grant access to replacement media via the old token.
    mount_index(&bank, &first, format, 0, 0);
    ck_assert_int_eq(dos_media_read(&bank, one.media, one.track, one.sector, scratch), DOS_MEDIA_STALE);
    dos_rel_lookup_cancel(&one);
    ck_assert_int_eq(one.result, DOS_REL_LOOKUP_IDLE);
    ck_assert_int_eq(two.result, DOS_REL_LOOKUP_FOUND);
}
END_TEST

// Complete at most three reads while checking the cooperative one-read budget.
static void finish_end(dos_rel_end_t* const end, dos_media_bank_t* const bank, rel_image_t* const image,
                       uint8_t scratch[DISKIMAGE_SECTOR_SIZE]) {
    for (unsigned int step = 0; step < 3 && end->result == DOS_REL_END_PENDING; ++step) {
        const unsigned int reads = image->reads;
        dos_rel_end_step(end, bank, scratch);
        ck_assert_uint_le(image->reads, reads + 1u);
    }
    ck_assert_int_ne(end->result, DOS_REL_END_PENDING);
}

// SSEND chooses the last present side, not its link or the requested record.
START_TEST(test_end_selects_native_last_side_and_group) {
    const unsigned int format = (unsigned int)_i / 6u;
    const unsigned int side = (unsigned int)_i % 6u;
    static const uint8_t groups[] = {0, 1, 45, 89, 90, 90};
    static const uint8_t indices[] = {16, 18, 254, 0, 255, 253};
    const uint8_t header = side & 1u ? side : 0;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_index(&bank, &image, format, side % 4u, side & 1u);
    image.bytes[0][2] = header;
    image.bytes[0][1] = image.bytes[1][1] = (uint8_t)(indices[side] + 1u);
    image.bytes[1][2] = 0xe7;
    for (unsigned int entry = 0; entry <= side; ++entry) {
        image.bytes[0][4u + 2u * entry] = 1;
        image.bytes[0][5u + 2u * entry] = 1;
    }
    // Leading link and group 91 must not override RDLG's backward 90..0 scan.
    image.bytes[2][0] = 35;
    image.bytes[2][1] = 16;
    image.bytes[2][2] = side & 1u ? 0 : 254;
    image.bytes[2][3] = 1;
    image.bytes[2][3u + 2u * groups[side]] = 1;
    image.bytes[2][3u + 2u * 91u] = 1;
    image.bytes[2][4u + 2u * 91u] = 2;
    dos_rel_end_t end;
    dos_rel_end_start(&end, models[format], reference, 1, format == 2 ? 2 : 0);
    uint8_t scratch[DISKIMAGE_SECTOR_SIZE];
    finish_end(&end, &bank, &image, scratch);
    ck_assert_int_eq(end.result, DOS_REL_END_FOUND);
    ck_assert_uint_eq(end.track, 1);
    ck_assert_uint_eq(end.sector, header == side ? 0 : 1);
    ck_assert_uint_eq(end.group, format == 2 ? groups[side] : 0);
    ck_assert_uint_eq(end.side, side);
    ck_assert_uint_eq(end.index, indices[side]);
    ck_assert_uint_eq(scratch[1], (uint8_t)(indices[side] + 1u));
    const unsigned int reads = 1u + (format == 2) + (header != side);
    ck_assert_uint_eq(image.reads, reads);
    memset(scratch, 0xa5, sizeof(scratch));
    ck_assert_int_eq(dos_rel_end_step(&end, &bank, scratch), DOS_REL_END_FOUND);
    ck_assert_uint_eq(image.reads, reads);
}
END_TEST

// Empty side tables and a nonterminal selected side reach BREAK, not status 50.
START_TEST(test_end_native_corrupt_index) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int variant = (unsigned int)_i % 3u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_index(&bank, &image, format, 0, 1);
    image.bytes[2][3] = 1;
    if (variant == 1) {
        image.bytes[0][4] = image.bytes[0][0] = 1;
    } else if (variant == 2) {
        image.bytes[0][14] = 1;
        image.bytes[0][15] = 1;
        image.bytes[1][0] = 1;
    }
    dos_rel_end_t end;
    dos_rel_end_start(&end, models[format], reference, 1, format == 2 ? 2 : 0);
    uint8_t scratch[DISKIMAGE_SECTOR_SIZE];
    finish_end(&end, &bank, &image, scratch);
    ck_assert_int_eq(end.result, DOS_REL_END_CORRUPT);
    ck_assert_int_eq(end.media_result, DOS_MEDIA_OK);
    const unsigned int reads = image.reads;
    ck_assert_int_eq(dos_rel_end_step(&end, &bank, scratch), DOS_REL_END_CORRUPT);
    ck_assert_uint_eq(image.reads, reads);
}
END_TEST

// An empty 8250 table stops before native RDLG underflows into adjacent RAM.
START_TEST(test_end_empty_super_table_handoff) {
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_index(&bank, &image, 2, 0, 0);
    image.bytes[2][2] = 254;
    image.bytes[2][3u + 2u * 91u] = _i ? 1 : 0;
    dos_rel_end_t end;
    dos_rel_end_start(&end, DOS_MODEL_8250, reference, 1, 2);
    uint8_t scratch[DISKIMAGE_SECTOR_SIZE];
    ck_assert_int_eq(dos_rel_end_step(&end, &bank, scratch), DOS_REL_END_NO_GROUP);
    ck_assert_int_eq(end.media_result, DOS_MEDIA_OK);
    ck_assert_int_eq(dos_rel_end_step(&end, &bank, scratch), DOS_REL_END_NO_GROUP);
    ck_assert_uint_eq(image.reads, 1);
}
END_TEST

// Every read phase rejects partial data, replacement and removal via its token.
START_TEST(test_end_storage_and_generation_failures) {
    const unsigned int format = (unsigned int)_i / 9u;
    const unsigned int variant = (unsigned int)_i % 9u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_index(&bank, &image, format, 2, 1);
    image.bytes[2][3] = image.bytes[0][14] = 1;
    image.bytes[0][15] = 1;
    const unsigned int completed = variant % 3u % (format == 2 ? 3u : 2u);
    dos_rel_end_t end;
    dos_rel_end_start(&end, models[format], reference, 1, format == 2 ? 2 : 0);
    uint8_t scratch[DISKIMAGE_SECTOR_SIZE];
    for (unsigned int step = 0; step < completed; ++step) {
        ck_assert_int_eq(dos_rel_end_step(&end, &bank, scratch), DOS_REL_END_PENDING);
    }
    if (variant < 3) {
        image.fail_read = image.reads + 1u;
    } else if (variant < 6) {
        mount_index(&bank, &replacement, format, 2, 1);
    } else {
        ck_assert_int_eq(dos_media_set_slot(&bank, 2, 1, NULL), DOS_MEDIA_OK);
    }
    ck_assert_int_eq(dos_rel_end_step(&end, &bank, scratch), DOS_REL_END_FAILED);
    ck_assert_int_eq(end.media_result, variant < 3 ? DOS_MEDIA_IO_ERROR : DOS_MEDIA_STALE);
    ck_assert_uint_eq(image.reads, completed + (variant < 3));
    ck_assert_uint_eq(replacement.reads, 0);
    ck_assert_int_eq(dos_rel_end_step(&end, &bank, scratch), DOS_REL_END_FAILED);
    ck_assert_uint_eq(image.reads, completed + (variant < 3));
}
END_TEST

// Root and selected-side coordinates are geometry-checked before host callbacks.
START_TEST(test_end_geometry_errors) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool root = (unsigned int)_i % 2u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_index(&bank, &image, format, 0, 0);
    const uint8_t invalid = format == 0 ? 36 : format == 1 ? 78 : 155;
    image.bytes[2][3] = 1;
    image.bytes[0][14] = invalid;
    dos_rel_end_t end;
    dos_rel_end_start(&end, models[format], reference, root ? invalid : 1, format == 2 ? 2 : 0);
    uint8_t scratch[DISKIMAGE_SECTOR_SIZE];
    finish_end(&end, &bank, &image, scratch);
    ck_assert_int_eq(end.result, DOS_REL_END_FAILED);
    ck_assert_int_eq(end.media_result, DOS_MEDIA_RANGE);
    ck_assert_uint_eq(image.reads, root ? 0 : 1u + (format == 2));
}
END_TEST

// Cancelling/restarting one traversal cannot mutate its borrowed page or peer.
START_TEST(test_end_cancel_restart_and_peer_generation) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t image = {0}, peer = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t a = mount_index(&bank, &image, format, 1, 0);
    const dos_media_ref_t b = mount_index(&bank, &peer, format, 3, 1);
    image.bytes[2][3] = peer.bytes[2][3] = 1;
    image.bytes[0][14] = peer.bytes[0][4] = 1;
    image.bytes[0][15] = 1;
    peer.bytes[0][1] = 255;
    dos_rel_end_t one, two;
    dos_rel_end_start(&one, models[format], a, 1, format == 2 ? 2 : 0);
    dos_rel_end_start(&two, models[format], b, 1, format == 2 ? 2 : 0);
    const dos_rel_end_t before = two;
    uint8_t scratch[DISKIMAGE_SECTOR_SIZE], saved[DISKIMAGE_SECTOR_SIZE];
    ck_assert_int_eq(dos_rel_end_step(&one, &bank, scratch), DOS_REL_END_PENDING);
    memcpy(saved, scratch, sizeof(saved));
    dos_rel_end_cancel(&one);
    ck_assert_int_eq(dos_rel_end_step(&one, &bank, scratch), DOS_REL_END_IDLE);
    ck_assert_mem_eq(saved, scratch, sizeof(saved));
    ck_assert_mem_eq(&two, &before, sizeof(before));
    dos_rel_end_start(&one, models[format], a, 1, format == 2 ? 2 : 0);
    ck_assert_int_eq(dos_rel_end_step(&one, &bank, scratch), DOS_REL_END_PENDING);
    mount_index(&bank, &replacement, format, 1, 0);
    ck_assert_int_eq(dos_rel_end_step(&one, &bank, scratch), DOS_REL_END_FAILED);
    finish_end(&two, &bank, &peer, scratch);
    ck_assert_int_eq(two.result, DOS_REL_END_FOUND);
    ck_assert_uint_eq(two.index, 254);
    ck_assert_uint_eq(replacement.reads, 0);
}
END_TEST

// Link only the requested native pointer into the shared sparse page fixture.
static dos_media_ref_t mount_record_at(dos_media_bank_t* const bank, rel_image_t* const image,
                                       const unsigned int format, const uint16_t number, const uint8_t size,
                                       const uint8_t position, const bool following, const unsigned int unit,
                                       const unsigned int slot) {
    const dos_media_ref_t reference = mount_index(bank, image, format, unit, slot);
    const dos_rel_address_t address = dos_rel_address(models[format], number, size, position);
    image->bytes[0][2] = address.side;
    image->bytes[0][address.index] = 1;
    image->bytes[0][address.index + 1u] = 3;
    image->bytes[2][3u + address.group * 2u] = 1;
    image->bytes[3][0] = following ? 1 : 0;
    image->bytes[3][1] = following ? 4 : UINT8_MAX;
    image->bytes[4][1] = UINT8_MAX;
    return reference;
}

// Most positioned-record cases use the first local drive of unit zero.
static dos_media_ref_t mount_record(dos_media_bank_t* const bank, rel_image_t* const image, const unsigned int format,
                                    const uint16_t number, const uint8_t size, const uint8_t position,
                                    const bool following) {
    return mount_record_at(bank, image, format, number, size, position, following, 0, 0);
}

// Bound every call to one read and reject previews before complete publication.
static dos_rel_record_result_t prepare_record(dos_rel_record_t* const record, dos_media_bank_t* const bank,
                                              rel_image_t* const image, uint8_t side[DISKIMAGE_SECTOR_SIZE],
                                              uint8_t first[DISKIMAGE_SECTOR_SIZE],
                                              uint8_t following[DISKIMAGE_SECTOR_SIZE]) {
    unsigned int steps = 0;
    while (record->result == DOS_REL_RECORD_PENDING) {
        ck_assert_uint_lt(steps++, 5);
        const unsigned int reads = image->reads;
        uint8_t byte = 0xa5;
        bool eoi = true;
        ck_assert(!dos_rel_record_peek(record, bank, first, following, &byte, &eoi));
        ck_assert_uint_eq(byte, 0xa5);
        ck_assert(eoi);
        dos_rel_record_step(record, bank, side, first, following);
        ck_assert_uint_le(image->reads - reads, 1);
        memset(side, 0x55, DISKIMAGE_SECTOR_SIZE);
    }
    return record->result;
}

// Match SETLST's trailing-zero scan and RDREL's native low-byte EOI comparison.
START_TEST(test_record_read_preparation_and_actual_get) {
    const unsigned int format = (unsigned int)_i / 9u;
    const unsigned int variant = (unsigned int)_i % 9u;
    const uint16_t number = variant == 4 || variant == 5 ? 2 : 1;
    const uint8_t size = variant == 2                   ? 254
                         : variant == 3 || variant == 8 ? 255
                         : variant == 4 || variant == 5 ? 200
                                                        : 10;
    const uint8_t position = variant == 6 ? 2 : variant == 7 ? 11 : 0;
    const bool following = variant >= 3 && variant <= 6;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, number, size, position, following);
    unsigned int count = 0;
    uint8_t final = 0, next = 0, last = 0;
    switch (variant) {
    case 0:
    case 7:
        image.bytes[3][2] = 'A';
        image.bytes[3][4] = 'B';
        count = 3;
        final = 'B';
        next = 12;
        last = 4;
        break;
    case 1:
        count = 1;
        next = 12;
        last = 2;
        break;
    case 2:
        image.bytes[3][255] = 'Z';
        count = 254;
        final = 'Z';
        last = 255;
        break;
    case 3:
        // End-of-record offset 2 in the next page also equals the current
        // pointer. Native RD15 marks the first byte EOI, not byte 255.
        image.bytes[3][2] = 'A';
        image.bytes[4][2] = 'B';
        count = 1;
        final = 'A';
        next = 3;
        last = 2;
        break;
    case 4:
        image.bytes[4][147] = 'Q';
        count = 200;
        final = 'Q';
        next = 148;
        last = 147;
        break;
    case 5:
        image.bytes[3][205] = 'C';
        count = 4;
        final = 'C';
        next = 148;
        last = 205;
        break;
    case 6:
        // P past trimmed data scans back before its current pointer, then
        // reads through the page boundary until the low offset matches.
        image.bytes[3][2] = 'X';
        image.bytes[4][2] = 'Y';
        count = 254;
        final = 'Y';
        next = 12;
        last = 2;
        break;
    case 8:
        break;
    }
    dos_rel_record_t record;
    dos_rel_record_start(&record, models[format], reference, 1, format == 2 ? 2 : 0, number, size, position);
    uint8_t side[DISKIMAGE_SECTOR_SIZE], data[2][DISKIMAGE_SECTOR_SIZE];
    const dos_rel_record_result_t result = prepare_record(&record, &bank, &image, side, data[0], data[1]);
    const unsigned int reads = image.reads;
    ck_assert_int_eq(result, variant == 8 ? DOS_REL_RECORD_MISSING : DOS_REL_RECORD_READY);
    ck_assert_int_eq(record.lookup.address.overflow, variant == 7);
    if (variant == 8) {
        for (unsigned int repeat = 0; repeat < 3; ++repeat) {
            uint8_t byte;
            bool eoi;
            ck_assert(dos_rel_record_peek(&record, &bank, data[0], data[1], &byte, &eoi));
            ck_assert_uint_eq(byte, '\r');
            ck_assert(eoi);
            ck_assert_int_eq(dos_rel_record_get(&record, &bank), DOS_REL_RECORD_MISSING);
        }
    } else {
        ck_assert_uint_eq(record.next, next);
        ck_assert_uint_eq(record.last, last);
        for (unsigned int got = 0; got < count; ++got) {
            uint8_t byte;
            bool eoi;
            const dos_rel_record_t before = record;
            ck_assert(dos_rel_record_peek(&record, &bank, data[0], data[1], &byte, &eoi));
            ck_assert_mem_eq(&record, &before, sizeof(before));
            ck_assert(dos_rel_record_peek(&record, &bank, data[0], data[1], &byte, &eoi));
            ck_assert_uint_eq(byte, got + 1u == count              ? final
                                    : variant == 0 || variant == 7 ? (got == 0 ? 'A' : 0)
                                                                   : 0);
            ck_assert_int_eq(eoi, got + 1u == count);
            ck_assert_int_eq(dos_rel_record_get(&record, &bank), eoi ? DOS_REL_RECORD_END : DOS_REL_RECORD_READY);
        }
        uint8_t byte = 0xa5;
        bool eoi = false;
        ck_assert(!dos_rel_record_peek(&record, &bank, data[0], data[1], &byte, &eoi));
        ck_assert_uint_eq(byte, 0xa5);
    }
    dos_rel_record_step(&record, &bank, side, data[0], data[1]);
    ck_assert_uint_eq(image.reads, reads);
}
END_TEST

// Distinguish absent indices and unbuffered NRBUF handoff from native CR/50.
START_TEST(test_record_explicit_incomplete_boundaries) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 1, 10, 2, false);
    image.bytes[3][2] = 'X';
    dos_rel_record_t record;
    dos_rel_record_start(&record, models[format], reference, 1, format == 2 ? 2 : 0, 1, 10, 2);
    uint8_t side[DISKIMAGE_SECTOR_SIZE], data[2][DISKIMAGE_SECTOR_SIZE];
    ck_assert_int_eq(prepare_record(&record, &bank, &image, side, data[0], data[1]), DOS_REL_RECORD_READY);
    for (unsigned int got = 0; got < 253; ++got) {
        uint8_t byte;
        bool eoi;
        ck_assert(dos_rel_record_peek(&record, &bank, data[0], data[1], &byte, &eoi));
        ck_assert_uint_eq(byte, 0);
        ck_assert(!eoi);
        dos_rel_record_get(&record, &bank);
    }
    ck_assert_int_eq(record.result, DOS_REL_RECORD_NEEDS_LINK);
    image.bytes[0][16] = 0;
    dos_rel_record_start(&record, models[format], reference, 1, format == 2 ? 2 : 0, 1, 10, 2);
    ck_assert_int_eq(prepare_record(&record, &bank, &image, side, data[0], data[1]), DOS_REL_RECORD_NO_INDEX);
    uint8_t byte = 0xa5;
    bool eoi = false;
    ck_assert(!dos_rel_record_peek(&record, &bank, data[0], data[1], &byte, &eoi));
    ck_assert_uint_eq(byte, 0xa5);
}
END_TEST

// A second ER3 requests SSEND, but accepted ER1 coordinates reach data validation.
START_TEST(test_record_selected_side_range_and_geometry_handoff) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool missing = (unsigned int)_i % 2u == 0;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_index(&bank, &image, format, 0, 0);
    image.bytes[2][DOS_REL_GROUP_TABLE_OFFSET] = 1;
    image.bytes[0][DOS_REL_SIDE_TABLE_OFFSET + 2u] = 1;
    image.bytes[0][DOS_REL_SIDE_TABLE_OFFSET + 3u] = 1;
    image.bytes[1][DOS_REL_SIDE_TABLE_OFFSET + 2u] = missing ? 0 : 1;
    image.bytes[1][DOS_REL_SIDE_TABLE_OFFSET + 3u] = 6;
    image.bytes[1][DOS_REL_POINTER_OFFSET] = missing ? 1 : 0;
    image.bytes[1][DOS_REL_POINTER_OFFSET + 1u] = 3;
    dos_rel_record_t record;
    dos_rel_record_start(&record, models[format], reference, 1, format == 2 ? 2 : 0, 241, 127, 1);
    uint8_t side[DISKIMAGE_SECTOR_SIZE], data[2][DISKIMAGE_SECTOR_SIZE];
    ck_assert_int_eq(prepare_record(&record, &bank, &image, side, data[0], data[1]),
                     missing ? DOS_REL_RECORD_NO_INDEX : DOS_REL_RECORD_FAILED);
    ck_assert_int_eq(record.lookup.media_result, missing ? DOS_MEDIA_OK : DOS_MEDIA_RANGE);
    ck_assert_uint_eq(image.reads, 2u + (format == 2));
    uint8_t byte = 0xa5;
    bool eoi = false;
    ck_assert(!dos_rel_record_peek(&record, &bank, data[0], data[1], &byte, &eoi));
    ck_assert_uint_eq(byte, 0xa5);
    ck_assert(!eoi);
}
END_TEST

// No partial index/current/prefetch read may make a byte ready.
START_TEST(test_record_storage_and_stale_failures) {
    const unsigned int format = (unsigned int)_i / 8u;
    const unsigned int variant = (unsigned int)_i % 8u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 2, 200, 0, true);
    image.bytes[3][2] = 'A';
    dos_rel_record_t record;
    dos_rel_record_start(&record, models[format], reference, 1, format == 2 ? 2 : 0, 2, 200, 0);
    uint8_t side[DISKIMAGE_SECTOR_SIZE], data[2][DISKIMAGE_SECTOR_SIZE];
    if (variant < 4) {
        // D64/D80 need three reads, D82 four, including super/current/prefetch.
        image.fail_read = 1u + variant % (format == 2 ? 4u : 3u);
        ck_assert_int_eq(prepare_record(&record, &bank, &image, side, data[0], data[1]), DOS_REL_RECORD_FAILED);
        ck_assert_int_eq(record.lookup.media_result, DOS_MEDIA_IO_ERROR);
    } else {
        const unsigned int completed = (variant - 4u) % (format == 2 ? 4u : 3u);
        for (unsigned int step = 0; step < completed; ++step) {
            dos_rel_record_step(&record, &bank, side, data[0], data[1]);
        }
        mount_record(&bank, &replacement, format, 1, 10, 0, true);
        ck_assert_int_eq(prepare_record(&record, &bank, &image, side, data[0], data[1]), DOS_REL_RECORD_FAILED);
        ck_assert_int_eq(record.lookup.media_result, DOS_MEDIA_STALE);
        ck_assert_uint_eq(replacement.reads, 0);
    }
    uint8_t byte = 0xa5;
    bool eoi = false;
    ck_assert(!dos_rel_record_peek(&record, &bank, data[0], data[1], &byte, &eoi));
    ck_assert_uint_eq(byte, 0xa5);
    ck_assert(!eoi);
    const unsigned int reads = image.reads;
    ck_assert_int_eq(dos_rel_record_step(&record, &bank, side, data[0], data[1]), DOS_REL_RECORD_FAILED);
    ck_assert_uint_eq(image.reads, reads);
}
END_TEST

// STRDBL's asynchronous prefetch error is observed only when its page is used.
START_TEST(test_record_defers_unused_prefetch_failure) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int variant = (unsigned int)_i % 3u;
    const uint8_t position = variant == 1 ? 2 : 0;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 1, 10, position, true);
    image.bytes[3][2] = 'A';
    if (variant == 2) {
        image.bytes[3][0] = 155;
    } else {
        image.fail_read = format == 2 ? 4 : 3;
    }
    dos_rel_record_t record;
    dos_rel_record_start(&record, models[format], reference, 1, format == 2 ? 2 : 0, 1, 10, position);
    uint8_t side[DISKIMAGE_SECTOR_SIZE], data[2][DISKIMAGE_SECTOR_SIZE];
    ck_assert_int_eq(prepare_record(&record, &bank, &image, side, data[0], data[1]), DOS_REL_RECORD_READY);
    ck_assert(record.following_failed);
    ck_assert(!record.following);
    ck_assert_int_eq(record.lookup.media_result, variant == 2 ? DOS_MEDIA_RANGE : DOS_MEDIA_IO_ERROR);
    const unsigned int reads = image.reads;
    const unsigned int count = variant == 1 ? 253 : 1;
    for (unsigned int got = 0; got < count; ++got) {
        uint8_t byte;
        bool eoi;
        ck_assert(dos_rel_record_peek(&record, &bank, data[0], data[1], &byte, &eoi));
        ck_assert_uint_eq(byte, variant == 1 ? 0 : 'A');
        ck_assert_int_eq(eoi, variant != 1);
        dos_rel_record_get(&record, &bank);
    }
    ck_assert_int_eq(record.result, variant == 1 ? DOS_REL_RECORD_FAILED : DOS_REL_RECORD_END);
    ck_assert_uint_eq(image.reads, reads);
}
END_TEST

// Cached bytes remain bound to the old medium, and cancellation never owns pages.
START_TEST(test_record_cancel_restart_and_cached_generation) {
    const unsigned int format = (unsigned int)_i / 4u;
    const bool use_get = (unsigned int)_i % 2u;
    const bool removed = (unsigned int)_i % 4u >= 2u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 1, 10, 0, false);
    image.bytes[3][2] = 'A';
    dos_rel_record_t record;
    dos_rel_record_start(&record, models[format], reference, 1, format == 2 ? 2 : 0, 1, 10, 0);
    uint8_t side[DISKIMAGE_SECTOR_SIZE], data[2][DISKIMAGE_SECTOR_SIZE];
    dos_rel_record_step(&record, &bank, side, data[0], data[1]);
    const unsigned int reads = image.reads;
    dos_rel_record_cancel(&record);
    ck_assert_int_eq(dos_rel_record_step(&record, &bank, side, data[0], data[1]), DOS_REL_RECORD_IDLE);
    ck_assert_uint_eq(image.reads, reads);
    dos_rel_record_start(&record, models[format], reference, 1, format == 2 ? 2 : 0, 1, 10, 0);
    ck_assert_int_eq(prepare_record(&record, &bank, &image, side, data[0], data[1]), DOS_REL_RECORD_READY);
    if (removed) {
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, NULL), DOS_MEDIA_OK);
    } else {
        mount_record(&bank, &replacement, format, 1, 10, 0, false);
    }
    uint8_t byte = 0xa5;
    bool eoi = false;
    if (use_get) {
        ck_assert_int_eq(dos_rel_record_get(&record, &bank), DOS_REL_RECORD_FAILED);
    } else {
        ck_assert(!dos_rel_record_peek(&record, &bank, data[0], data[1], &byte, &eoi));
        ck_assert_uint_eq(byte, 0xa5);
        ck_assert(!eoi);
    }
    ck_assert_int_eq(record.lookup.media_result, DOS_MEDIA_STALE);
    ck_assert_uint_eq(replacement.reads, 0);
}
END_TEST

// Interleaved records borrow distinct pages and replacing one unit spares its peer.
START_TEST(test_record_interleaved_units_and_peer_generation) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t first = {0}, peer = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t a = mount_record_at(&bank, &first, format, 1, 10, 0, true, 1, 0);
    const dos_media_ref_t b = mount_record_at(&bank, &peer, format, 1, 10, 0, true, 3, 1);
    first.bytes[3][2] = 'A';
    first.bytes[3][3] = 'B';
    peer.bytes[3][2] = 'X';
    peer.bytes[3][3] = 'Y';
    dos_rel_record_t one, two;
    dos_rel_record_start(&one, models[format], a, 1, format == 2 ? 2 : 0, 1, 10, 0);
    dos_rel_record_start(&two, models[format], b, 1, format == 2 ? 2 : 0, 1, 10, 0);
    uint8_t side[DISKIMAGE_SECTOR_SIZE];
    dos_buffer_t data_one[2] = {0}, data_two[2] = {0};
    // Native pool pages are not contiguous: each has its own metadata.
    for (unsigned int page = 0; page < 2; ++page) {
        data_one[page].track = data_two[page].track = 0xa5;
        data_one[page].sector = data_two[page].sector = 0x5a;
    }
    for (unsigned int step = 0; step < 4; ++step) {
        dos_rel_record_step(&one, &bank, side, data_one[0].bytes, data_one[1].bytes);
        memset(side, 0x55, sizeof(side));
        dos_rel_record_step(&two, &bank, side, data_two[0].bytes, data_two[1].bytes);
    }
    for (unsigned int page = 0; page < 2; ++page) {
        ck_assert_uint_eq(data_one[page].track, 0xa5);
        ck_assert_uint_eq(data_two[page].track, 0xa5);
        ck_assert_uint_eq(data_one[page].sector, 0x5a);
        ck_assert_uint_eq(data_two[page].sector, 0x5a);
        ck_assert(!data_one[page].resident && !data_two[page].resident);
    }
    const dos_rel_record_t before = two;
    uint8_t byte;
    bool eoi;
    ck_assert(dos_rel_record_peek(&one, &bank, data_one[0].bytes, data_one[1].bytes, &byte, &eoi));
    ck_assert_uint_eq(byte, 'A');
    ck_assert(!eoi);
    dos_rel_record_get(&one, &bank);
    ck_assert_mem_eq(&two, &before, sizeof(before));
    mount_record_at(&bank, &replacement, format, 1, 10, 0, false, 1, 0);
    ck_assert_int_eq(dos_rel_record_get(&one, &bank), DOS_REL_RECORD_FAILED);
    ck_assert(dos_rel_record_peek(&two, &bank, data_two[0].bytes, data_two[1].bytes, &byte, &eoi));
    ck_assert_uint_eq(byte, 'X');
    ck_assert(!eoi);
    ck_assert_int_eq(dos_rel_record_get(&two, &bank), DOS_REL_RECORD_READY);
    ck_assert(dos_rel_record_peek(&two, &bank, data_two[0].bytes, data_two[1].bytes, &byte, &eoi));
    ck_assert_uint_eq(byte, 'Y');
    ck_assert(eoi);
    ck_assert_int_eq(dos_rel_record_get(&two, &bank), DOS_REL_RECORD_END);
    ck_assert_uint_eq(replacement.reads, 0);
}
END_TEST

// Read several records through alternating nonadjacent native pool pages.
START_TEST(test_record_next_stream_and_native_pool_pages) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int variant = (unsigned int)_i % 3u;
    const uint8_t size = variant == 0 ? 127 : variant == 1 ? 200 : 10;
    const unsigned int records = 3u * DISKIMAGE_SECTOR_PAYLOAD_SIZE / size;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 1, size, 0, true);
    for (unsigned int page = 3; page < 6; ++page) {
        image.bytes[page][0] = page == 5 ? 0 : 1;
        image.bytes[page][1] = page == 5 ? UINT8_MAX : page + 1u;
    }
    for (unsigned int offset = 0; offset < 3u * DISKIMAGE_SECTOR_PAYLOAD_SIZE; ++offset) {
        const unsigned int number = offset / size;
        const uint8_t value = variant == 2 && number == 1 ? 0 : 'A' + number % 26u;
        image.bytes[3u + offset / DISKIMAGE_SECTOR_PAYLOAD_SIZE][2u + offset % DISKIMAGE_SECTOR_PAYLOAD_SIZE] =
            variant != 2 || offset % size == 0 ? value : 0;
    }
    dos_rel_record_t record;
    dos_rel_record_start(&record, models[format], reference, 1, format == 2 ? 2 : 0, 1, size, 0);
    dos_buffer_t buffers[3] = {0};
    for (unsigned int page = 0; page < count_of(buffers); ++page) {
        buffers[page].track = 0xa5;
        buffers[page].sector = 0x5a;
    }
    memset(buffers[1].bytes, 0xa5, sizeof(buffers[1].bytes));
    uint8_t side[DISKIMAGE_SECTOR_SIZE];
    ck_assert_int_eq(prepare_record(&record, &bank, &image, side, buffers[0].bytes, buffers[2].bytes),
                     DOS_REL_RECORD_READY);
    for (unsigned int number = 0; number < records; ++number) {
        const dos_rel_record_t before = record;
        ck_assert_int_eq(dos_rel_record_next(&record, &bank, buffers[0].bytes, buffers[2].bytes), DOS_REL_RECORD_READY);
        ck_assert_mem_eq(&record, &before, sizeof(before));
        const unsigned int count = variant == 2 ? 1 : size;
        for (unsigned int got = 0; got < count; ++got) {
            uint8_t byte;
            bool eoi;
            ck_assert(dos_rel_record_peek(&record, &bank, buffers[0].bytes, buffers[2].bytes, &byte, &eoi));
            ck_assert_uint_eq(byte, variant == 2 && number == 1 ? 0 : 'A' + number % 26u);
            ck_assert_int_eq(eoi, got + 1u == count);
            ck_assert_int_eq(dos_rel_record_get(&record, &bank), eoi ? DOS_REL_RECORD_END : DOS_REL_RECORD_READY);
        }
        dos_rel_record_next(&record, &bank, buffers[0].bytes, buffers[2].bytes);
        ck_assert_int_eq(prepare_record(&record, &bank, &image, side, buffers[0].bytes, buffers[2].bytes),
                         number + 1u == records ? DOS_REL_RECORD_MISSING : DOS_REL_RECORD_READY);
    }
    for (unsigned int repeat = 0; repeat < 3; ++repeat) {
        uint8_t byte;
        bool eoi;
        ck_assert(dos_rel_record_peek(&record, &bank, buffers[0].bytes, buffers[2].bytes, &byte, &eoi));
        ck_assert_uint_eq(byte, '\r');
        ck_assert(eoi);
        ck_assert_int_eq(dos_rel_record_get(&record, &bank), DOS_REL_RECORD_MISSING);
        ck_assert_int_eq(dos_rel_record_next(&record, &bank, buffers[0].bytes, buffers[2].bytes),
                         DOS_REL_RECORD_MISSING);
    }
    ck_assert_uint_eq(image.reads, 4u + (format == 2));
    for (unsigned int page = 0; page < count_of(buffers); ++page) {
        ck_assert_uint_eq(buffers[page].track, 0xa5);
        ck_assert_uint_eq(buffers[page].sector, 0x5a);
        ck_assert(!buffers[page].resident && !buffers[page].failed);
    }
    for (unsigned int offset = 0; offset < DISKIMAGE_SECTOR_SIZE; ++offset) {
        ck_assert_uint_eq(buffers[1].bytes[offset], 0xa5);
    }
}
END_TEST

// Failed initial or recycled-page prefetch survives END until NRBUF needs it.
START_TEST(test_record_next_retains_failed_prefetch) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int variant = (unsigned int)_i % 3u;
    const bool recycled = variant != 0;
    const uint8_t size = variant == 2 ? 200 : 127;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 1, size, 0, true);
    for (unsigned int page = 3; page < 6; ++page) {
        image.bytes[page][0] = page == 5 ? 0 : 1;
        image.bytes[page][1] = page == 5 ? UINT8_MAX : page + 1u;
        for (unsigned int offset = 2; offset < DISKIMAGE_SECTOR_SIZE; ++offset) {
            image.bytes[page][offset] = 'A' + ((page - 3u) * DISKIMAGE_SECTOR_PAYLOAD_SIZE + offset - 2u) / size;
        }
    }
    image.fail_read = 3u + (format == 2) + recycled;
    dos_rel_record_t record;
    dos_rel_record_start(&record, models[format], reference, 1, format == 2 ? 2 : 0, 1, size, 0);
    uint8_t side[DISKIMAGE_SECTOR_SIZE], data[2][DISKIMAGE_SECTOR_SIZE];
    ck_assert_int_eq(prepare_record(&record, &bank, &image, side, data[0], data[1]), DOS_REL_RECORD_READY);
    const unsigned int readable = variant == 1 ? 4 : 2;
    for (unsigned int number = 0; number < readable; ++number) {
        for (unsigned int got = 0; got < size; ++got) {
            uint8_t byte;
            bool eoi;
            ck_assert(dos_rel_record_peek(&record, &bank, data[0], data[1], &byte, &eoi));
            ck_assert_uint_eq(byte, 'A' + number);
            ck_assert_int_eq(eoi, got + 1u == size);
            dos_rel_record_get(&record, &bank);
        }
        ck_assert_int_eq(record.result, DOS_REL_RECORD_END);
        dos_rel_record_next(&record, &bank, data[0], data[1]);
        ck_assert_int_eq(prepare_record(&record, &bank, &image, side, data[0], data[1]),
                         number + 1u == readable ? DOS_REL_RECORD_FAILED : DOS_REL_RECORD_READY);
    }
    ck_assert(record.following_failed);
    ck_assert_int_eq(record.lookup.media_result, DOS_MEDIA_IO_ERROR);
    ck_assert_uint_eq(image.reads, image.fail_read);
    uint8_t byte = 0xa5;
    bool eoi = false;
    ck_assert(!dos_rel_record_peek(&record, &bank, data[0], data[1], &byte, &eoi));
    ck_assert_uint_eq(byte, 0xa5);
    ck_assert(!eoi);
    ck_assert_int_eq(dos_rel_record_next(&record, &bank, data[0], data[1]), DOS_REL_RECORD_FAILED);
    ck_assert_uint_eq(image.reads, image.fail_read);
}
END_TEST

// Replaced/removed media cannot advance cached END or fulfill a scheduled refill.
START_TEST(test_record_next_generation_guards) {
    const unsigned int format = (unsigned int)_i / 4u;
    const bool pending = (unsigned int)_i % 2u;
    const bool removed = (unsigned int)_i % 4u >= 2u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 2, 127, 0, true);
    image.bytes[3][255] = 'A';
    image.bytes[4][0] = 1;
    image.bytes[4][1] = 5;
    dos_rel_record_t record;
    dos_rel_record_start(&record, models[format], reference, 1, format == 2 ? 2 : 0, 2, 127, 0);
    uint8_t side[DISKIMAGE_SECTOR_SIZE], data[2][DISKIMAGE_SECTOR_SIZE];
    ck_assert_int_eq(prepare_record(&record, &bank, &image, side, data[0], data[1]), DOS_REL_RECORD_READY);
    for (unsigned int got = 0; got < 127; ++got) {
        dos_rel_record_get(&record, &bank);
    }
    ck_assert_int_eq(record.result, DOS_REL_RECORD_END);
    const unsigned int reads = image.reads;
    if (pending) {
        ck_assert_int_eq(dos_rel_record_next(&record, &bank, data[0], data[1]), DOS_REL_RECORD_PENDING);
    }
    if (removed) {
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, NULL), DOS_MEDIA_OK);
    } else {
        mount_record(&bank, &replacement, format, 1, 127, 0, true);
    }
    if (pending) {
        ck_assert_int_eq(prepare_record(&record, &bank, &image, side, data[0], data[1]), DOS_REL_RECORD_FAILED);
    } else {
        ck_assert_int_eq(dos_rel_record_next(&record, &bank, data[0], data[1]), DOS_REL_RECORD_FAILED);
    }
    ck_assert_int_eq(record.lookup.media_result, DOS_MEDIA_STALE);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(replacement.reads, 0);
    uint8_t byte = 0xa5;
    bool eoi = false;
    ck_assert(!dos_rel_record_peek(&record, &bank, data[0], data[1], &byte, &eoi));
    ck_assert_uint_eq(byte, 0xa5);
    ck_assert(!eoi);
}
END_TEST

// Cancelling a recycled-page refill changes neither borrowed pages nor a peer.
START_TEST(test_record_next_cancel_and_peer_isolation) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t image = {0}, peer = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t a = mount_record_at(&bank, &image, format, 2, 127, 0, true, 1, 0);
    const dos_media_ref_t b = mount_record_at(&bank, &peer, format, 2, 127, 0, true, 3, 1);
    image.bytes[3][255] = peer.bytes[3][255] = 'A';
    image.bytes[4][0] = peer.bytes[4][0] = 1;
    image.bytes[4][1] = peer.bytes[4][1] = 5;
    dos_rel_record_t one, two;
    dos_rel_record_start(&one, models[format], a, 1, format == 2 ? 2 : 0, 2, 127, 0);
    dos_rel_record_start(&two, models[format], b, 1, format == 2 ? 2 : 0, 2, 127, 0);
    uint8_t side[DISKIMAGE_SECTOR_SIZE], data_one[2][DISKIMAGE_SECTOR_SIZE], data_two[2][DISKIMAGE_SECTOR_SIZE];
    ck_assert_int_eq(prepare_record(&one, &bank, &image, side, data_one[0], data_one[1]), DOS_REL_RECORD_READY);
    ck_assert_int_eq(prepare_record(&two, &bank, &peer, side, data_two[0], data_two[1]), DOS_REL_RECORD_READY);
    const dos_rel_record_t before = two;
    for (unsigned int got = 0; got < 127; ++got) {
        dos_rel_record_get(&one, &bank);
    }
    ck_assert_int_eq(dos_rel_record_next(&one, &bank, data_one[0], data_one[1]), DOS_REL_RECORD_PENDING);
    uint8_t saved[sizeof(data_one)];
    memcpy(saved, data_one, sizeof(saved));
    const unsigned int reads = image.reads;
    dos_rel_record_cancel(&one);
    ck_assert_int_eq(dos_rel_record_step(&one, &bank, side, data_one[0], data_one[1]), DOS_REL_RECORD_IDLE);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_mem_eq(data_one, saved, sizeof(saved));
    ck_assert_mem_eq(&two, &before, sizeof(before));
    dos_rel_record_start(&one, models[format], a, 1, format == 2 ? 2 : 0, 1, 127, 0);
    ck_assert_int_eq(prepare_record(&one, &bank, &image, side, data_one[0], data_one[1]), DOS_REL_RECORD_READY);
    ck_assert_mem_eq(&two, &before, sizeof(before));
    uint8_t byte;
    bool eoi;
    ck_assert(dos_rel_record_peek(&two, &bank, data_two[0], data_two[1], &byte, &eoi));
    ck_assert_uint_eq(byte, 0);
    ck_assert(!eoi);
}
END_TEST

// Preview and discarded tails cannot commit counters, GETFLG or error 50.
START_TEST(test_reader_preview_prefix_missing_and_reposition) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 0, 254, 0, false);
    memcpy(image.bytes[3] + 2, "AB\0D", 4);
    dos_rel_reader_t reader;
    dos_rel_reader_start(&reader, models[format], reference, 1, format == 2 ? 2 : 0, 0, 254, 0);
    uint8_t side[DISKIMAGE_SECTOR_SIZE], data[2][DISKIMAGE_SECTOR_SIZE], output[DOS_STAGE_CAPACITY];
    ck_assert_int_eq(prepare_record(&reader.record, &bank, &image, side, data[0], data[1]), DOS_REL_RECORD_READY);
    const unsigned int reads = image.reads;
    const dos_rel_reader_t before = reader;
    uint8_t saved[sizeof(data)];
    memcpy(saved, data, sizeof(saved));
    bool eoi = true;
    ck_assert_uint_eq(dos_rel_reader_stage(&reader, &bank, data[0], data[1], NULL, 0, &eoi), 0);
    ck_assert(!eoi);
    ck_assert_uint_eq(dos_rel_reader_stage(&reader, &bank, data[0], data[1], output, sizeof(output), &eoi), 4);
    ck_assert_mem_eq(output, "AB\0D", 4);
    ck_assert(eoi);
    ck_assert_mem_eq(&reader, &before, sizeof(before));
    ck_assert_int_eq(dos_rel_reader_get(&reader, models[format], &bank, data[0], data[1]), DOS_REL_RECORD_READY);
    ck_assert_uint_eq(reader.number, 0);
    ck_assert_int_eq(reader.get_active, format == 0);
    ck_assert(!reader.no_record);
    for (unsigned int repeat = 0; repeat < 2; ++repeat) {
        const dos_rel_reader_t suffix = reader;
        ck_assert_uint_eq(dos_rel_reader_stage(&reader, &bank, data[0], data[1], output, 1, &eoi), 1);
        ck_assert_uint_eq(output[0], 'B');
        ck_assert(!eoi);
        ck_assert_uint_eq(dos_rel_reader_stage(&reader, &bank, data[0], data[1], output, sizeof(output), &eoi), 3);
        ck_assert_mem_eq(output, "B\0D", 3);
        ck_assert(eoi);
        ck_assert_mem_eq(&reader, &suffix, sizeof(suffix));
    }
    for (unsigned int got = 0; got < 3; ++got) {
        ck_assert_int_eq(dos_rel_reader_get(&reader, models[format], &bank, data[0], data[1]),
                         got == 2 ? DOS_REL_RECORD_MISSING : DOS_REL_RECORD_READY);
    }
    ck_assert_uint_eq(reader.number, 1);
    ck_assert(!reader.get_active && !reader.no_record);
    for (unsigned int repeat = 0; repeat < 3; ++repeat) {
        const dos_rel_reader_t missing = reader;
        ck_assert_uint_eq(dos_rel_reader_stage(&reader, &bank, data[0], data[1], output, sizeof(output), &eoi), 1);
        ck_assert_uint_eq(output[0], '\r');
        ck_assert(eoi);
        ck_assert_mem_eq(&reader, &missing, sizeof(missing));
        ck_assert_int_eq(dos_rel_reader_get(&reader, models[format], &bank, data[0], data[1]), DOS_REL_RECORD_MISSING);
        ck_assert(reader.no_record);
        ck_assert(!reader.get_active);
        ck_assert_uint_eq(reader.number, 1);
    }
    ck_assert_mem_eq(data, saved, sizeof(saved));
    ck_assert_uint_eq(image.reads, reads);
    dos_rel_reader_start(&reader, models[format], reference, 1, format == 2 ? 2 : 0, 1, 254, 0);
    ck_assert_int_eq(prepare_record(&reader.record, &bank, &image, side, data[0], data[1]), DOS_REL_RECORD_READY);
    ck_assert_uint_eq(dos_rel_reader_stage(&reader, &bank, data[0], data[1], output, sizeof(output), &eoi), 4);
    ck_assert_mem_eq(output, "AB\0D", 4);
    ck_assert_uint_eq(reader.number, 1);
    ck_assert(!reader.get_active && !reader.no_record);
}
END_TEST

// Native counters retain P0 and wrap, rather than deriving the next file offset.
START_TEST(test_reader_raw_counter_alias_and_wrap) {
    const unsigned int format = (unsigned int)_i / 3u;
    const uint16_t numbers[] = {0, 1, UINT16_MAX};
    const uint16_t number = numbers[(unsigned int)_i % 3u];
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, number, 1, 0, false);
    const dos_rel_address_t address = dos_rel_address(models[format], number, 1, 0);
    image.bytes[3][address.offset] = 'Q';
    dos_rel_reader_t reader;
    dos_rel_reader_start(&reader, models[format], reference, 1, format == 2 ? 2 : 0, number, 1, 0);
    uint8_t side[DISKIMAGE_SECTOR_SIZE], data[2][DISKIMAGE_SECTOR_SIZE], output[DOS_STAGE_CAPACITY];
    ck_assert_int_eq(prepare_record(&reader.record, &bank, &image, side, data[0], data[1]), DOS_REL_RECORD_READY);
    bool eoi;
    ck_assert_uint_eq(dos_rel_reader_stage(&reader, &bank, data[0], data[1], output, sizeof(output), &eoi), 1);
    ck_assert_uint_eq(output[0], 'Q');
    ck_assert(eoi);
    ck_assert_uint_eq(reader.number, number);
    ck_assert_int_eq(dos_rel_reader_get(&reader, models[format], &bank, data[0], data[1]), DOS_REL_RECORD_READY);
    ck_assert_uint_eq(reader.number, (uint16_t)(number + 1u));
    ck_assert(!reader.get_active && !reader.no_record);
}
END_TEST

// Interrupt a cross-page staged suffix while borrowing noncontiguous pool pages.
START_TEST(test_reader_cross_page_interrupted_prefix) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 2, 200, 0, true);
    memset(image.bytes[3] + 202, 'A', 54);
    memset(image.bytes[4] + 2, 'B', 146);
    dos_rel_reader_t reader;
    dos_rel_reader_start(&reader, models[format], reference, 1, format == 2 ? 2 : 0, 2, 200, 0);
    dos_buffer_t pool[3] = {0};
    memset(pool[1].bytes, 0xa5, sizeof(pool[1].bytes));
    uint8_t side[DISKIMAGE_SECTOR_SIZE], output[DOS_STAGE_CAPACITY], saved[sizeof(pool)];
    ck_assert_int_eq(prepare_record(&reader.record, &bank, &image, side, pool[0].bytes, pool[2].bytes),
                     DOS_REL_RECORD_READY);
    memcpy(saved, pool, sizeof(saved));
    const unsigned int reads = image.reads;
    unsigned int got = 0;
    while (got < 200) {
        const dos_rel_reader_t before = reader;
        bool eoi;
        const unsigned int count =
            dos_rel_reader_stage(&reader, &bank, pool[0].bytes, pool[2].bytes, output, sizeof(output), &eoi);
        ck_assert_uint_gt(count, 0);
        ck_assert_uint_eq(count, 200u - got < sizeof(output) ? 200u - got : sizeof(output));
        ck_assert_int_eq(eoi, got + count == 200);
        ck_assert_mem_eq(&reader, &before, sizeof(before));
        for (unsigned int byte = 0; byte < count; ++byte) {
            ck_assert_uint_eq(output[byte], got + byte < 54 ? 'A' : 'B');
        }
        const unsigned int prefix = count > 1 ? count / 2u : 1;
        for (unsigned int byte = 0; byte < prefix; ++byte) {
            ++got;
            ck_assert_int_eq(dos_rel_reader_get(&reader, models[format], &bank, pool[0].bytes, pool[2].bytes),
                             got == 200 ? DOS_REL_RECORD_MISSING : DOS_REL_RECORD_READY);
            ck_assert_int_eq(reader.get_active, format == 0 && got != 200);
            ck_assert_uint_eq(reader.number, got == 200 ? 3 : 2);
            ck_assert(!reader.no_record);
        }
    }
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_mem_eq(pool, saved, sizeof(saved));
}
END_TEST

// Preview stops before a failed prefetch without poisoning the real cursor.
START_TEST(test_reader_preview_defers_failed_page_until_actual_get) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 2, 127, 2, true);
    image.bytes[3][129] = 'A';
    image.fail_read = 3u + (format == 2);
    dos_rel_reader_t reader;
    dos_rel_reader_start(&reader, models[format], reference, 1, format == 2 ? 2 : 0, 2, 127, 2);
    uint8_t side[DISKIMAGE_SECTOR_SIZE], data[2][DISKIMAGE_SECTOR_SIZE], output[DOS_STAGE_CAPACITY];
    ck_assert_int_eq(prepare_record(&reader.record, &bank, &image, side, data[0], data[1]), DOS_REL_RECORD_READY);
    ck_assert(reader.record.following_failed);
    bool eoi;
    ck_assert_uint_eq(dos_rel_reader_stage(&reader, &bank, data[0], data[1], output, sizeof(output), &eoi), 64);
    ck_assert(!eoi);
    for (unsigned int got = 0; got < sizeof(output); ++got) {
        ck_assert_int_eq(dos_rel_reader_get(&reader, models[format], &bank, data[0], data[1]), DOS_REL_RECORD_READY);
    }
    const dos_rel_reader_t before = reader;
    ck_assert_uint_eq(dos_rel_reader_stage(&reader, &bank, data[0], data[1], output, sizeof(output), &eoi), 62);
    ck_assert(!eoi);
    ck_assert_mem_eq(&reader, &before, sizeof(before));
    for (unsigned int got = 0; got < 62; ++got) {
        ck_assert_uint_eq(output[got], 0);
        ck_assert_int_eq(dos_rel_reader_get(&reader, models[format], &bank, data[0], data[1]),
                         got == 61 ? DOS_REL_RECORD_FAILED : DOS_REL_RECORD_READY);
    }
    ck_assert_int_eq(reader.record.lookup.media_result, DOS_MEDIA_IO_ERROR);
    ck_assert_uint_eq(reader.number, 2);
    ck_assert_int_eq(reader.get_active, format == 0);
    ck_assert(!reader.no_record);
    ck_assert_uint_eq(image.reads, image.fail_read);
}
END_TEST

// EOI advances channel state before cooperative refill or a deferred I/O error.
START_TEST(test_reader_pending_refill_and_deferred_error_counter) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 2, 127, 0, true);
    memset(image.bytes[3] + 129, 'A', 127);
    memset(image.bytes[4] + 2, 'B', DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    image.bytes[4][0] = 1;
    image.bytes[4][1] = 5;
    image.fail_read = 4u + (format == 2);
    dos_rel_reader_t reader;
    dos_rel_reader_start(&reader, models[format], reference, 1, format == 2 ? 2 : 0, 2, 127, 0);
    uint8_t side[DISKIMAGE_SECTOR_SIZE], data[2][DISKIMAGE_SECTOR_SIZE], output[DOS_STAGE_CAPACITY];
    ck_assert_int_eq(prepare_record(&reader.record, &bank, &image, side, data[0], data[1]), DOS_REL_RECORD_READY);
    for (unsigned int number = 2; number <= 4; ++number) {
        for (unsigned int got = 0; got < 127; ++got) {
            bool eoi;
            ck_assert_uint_eq(dos_rel_reader_stage(&reader, &bank, data[0], data[1], output, 1, &eoi), 1);
            ck_assert_uint_eq(output[0], number == 2 ? 'A' : 'B');
            ck_assert_int_eq(eoi, got == 126);
            const dos_rel_record_result_t result = dos_rel_reader_get(&reader, models[format], &bank, data[0], data[1]);
            ck_assert_int_eq(result, !eoi || number == 3 ? DOS_REL_RECORD_READY
                                     : number == 2       ? DOS_REL_RECORD_PENDING
                                                         : DOS_REL_RECORD_FAILED);
            ck_assert_uint_eq(reader.number, eoi ? number + 1u : number);
            ck_assert_int_eq(reader.get_active, format == 0 && !eoi);
            ck_assert(!reader.no_record);
        }
        if (number == 2) {
            const unsigned int reads = image.reads;
            const dos_rel_reader_t before = reader;
            bool eoi = true;
            ck_assert_uint_eq(dos_rel_reader_stage(&reader, &bank, data[0], data[1], output, sizeof(output), &eoi), 0);
            ck_assert(!eoi);
            ck_assert_mem_eq(&reader, &before, sizeof(before));
            ck_assert_uint_eq(image.reads, reads);
            ck_assert_int_eq(prepare_record(&reader.record, &bank, &image, side, data[0], data[1]),
                             DOS_REL_RECORD_READY);
            ck_assert(reader.record.following_failed);
            ck_assert_uint_eq(reader.number, 3);
            ck_assert(!reader.get_active && !reader.no_record);
        }
    }
    ck_assert_int_eq(reader.record.lookup.media_result, DOS_MEDIA_IO_ERROR);
    ck_assert_uint_eq(image.reads, image.fail_read);
}
END_TEST

// A stale preview or stale actual GET cannot advance counters or report error 50.
START_TEST(test_reader_generation_guards) {
    const unsigned int format = (unsigned int)_i / 4u;
    const bool removed = (unsigned int)_i % 4u >= 2u;
    const bool actual = (unsigned int)_i % 2u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 0, 254, 0, false);
    image.bytes[3][2] = 'Q';
    dos_rel_reader_t reader;
    dos_rel_reader_start(&reader, models[format], reference, 1, format == 2 ? 2 : 0, 0, 254, 0);
    uint8_t side[DISKIMAGE_SECTOR_SIZE], data[2][DISKIMAGE_SECTOR_SIZE], output[DOS_STAGE_CAPACITY];
    ck_assert_int_eq(prepare_record(&reader.record, &bank, &image, side, data[0], data[1]), DOS_REL_RECORD_READY);
    bool eoi;
    ck_assert_uint_eq(dos_rel_reader_stage(&reader, &bank, data[0], data[1], output, sizeof(output), &eoi), 1);
    ck_assert(eoi);
    if (removed) {
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, NULL), DOS_MEDIA_OK);
    } else {
        mount_record(&bank, &replacement, format, 0, 254, 0, false);
    }
    const unsigned int reads = image.reads;
    if (actual) {
        ck_assert_int_eq(dos_rel_reader_get(&reader, models[format], &bank, data[0], data[1]), DOS_REL_RECORD_FAILED);
    } else {
        output[0] = 0xa5;
        ck_assert_uint_eq(dos_rel_reader_stage(&reader, &bank, data[0], data[1], output, sizeof(output), &eoi), 0);
        ck_assert_uint_eq(output[0], 0xa5);
        ck_assert(!eoi);
    }
    ck_assert_int_eq(reader.record.lookup.media_result, DOS_MEDIA_STALE);
    ck_assert_uint_eq(reader.number, 0);
    ck_assert(!reader.get_active && !reader.no_record);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(replacement.reads, 0);
}
END_TEST

// NULBUF retains BIT ER00's exact-wrap Z flag as well as byte-wide ADC carry.
START_TEST(test_fill_native_record_markers) {
    static const struct {
        uint8_t size, seed, next, first;
        unsigned int count;
    } cases[] = {
        {1, 255, 0, 2, 254},   {2, 254, 0, 2, 127},   {10, 246, 252, 2, 26}, {127, 129, 0, 2, 2},
        {128, 128, 130, 2, 2}, {200, 56, 202, 2, 2},  {253, 3, 255, 2, 2},   {254, 2, 0, 2, 1},
        {255, 1, 2, 2, 1},     {255, 254, 0, 255, 1}, {255, 255, 0, 0, 1},   {200, 100, 246, 46, 2},
    };
    for (unsigned int test = 0; test < count_of(cases); ++test) {
        uint8_t bytes[DISKIMAGE_SECTOR_SIZE], expected[DISKIMAGE_SECTOR_SIZE] = {0x12, 0x34};
        memset(bytes, 0x5a, sizeof(bytes));
        bytes[0] = expected[0];
        bytes[1] = expected[1];
        for (unsigned int marker = 0; marker < cases[test].count; ++marker) {
            expected[cases[test].first + marker * cases[test].size] = UINT8_MAX;
        }
        ck_assert_uint_eq(dos_rel_fill(bytes, cases[test].size, cases[test].seed), cases[test].next);
        ck_assert_mem_eq(bytes, expected, sizeof(expected));
    }
}
END_TEST

// Check preflight's native wrap and deliberate side-boundary undercount.
START_TEST(test_growth_native_preflight) {
    const dos_model_t model = models[_i];
    const dos_rel_growth_plan_t one = dos_rel_growth_plan(model, 3, 127, 0, 0, 16);
    ck_assert_uint_eq(one.blocks, 1);
    ck_assert(!one.too_large);
    const dos_rel_growth_plan_t boundary = dos_rel_growth_plan(model, 121, 254, 0, 0, 254);
    ck_assert_uint_eq(boundary.blocks, 1);
    ck_assert_uint_eq(boundary.address.side, 1);
    ck_assert_uint_eq(boundary.address.index, 16);
    const dos_rel_growth_plan_t group = dos_rel_growth_plan(model, 721, 254, 0, 0, 16);
    ck_assert_int_eq(group.too_large, model != DOS_MODEL_8250);
    if (model == DOS_MODEL_8250) {
        ck_assert_uint_eq(group.blocks, 726);
        ck_assert_uint_eq(group.address.group, 1);
    }
}
END_TEST

// CLSREL uses SSEND's byte-wide endpoint, including malformed DEY results.
START_TEST(test_blocks_native_endpoints_and_low_word) {
    const dos_model_t model = models[_i];
    static const struct {
        uint8_t group, side, index;
        uint16_t small, big;
    } cases[] = {
        {0, 0, 16, 2, 3},    {0, 0, 254, 121, 122},  {0, 5, 254, 726, 727},
        {0, 0, 0, 122, 123}, {0, 0, 1, 122, 123},    {0, 0, 13, 128, 129},
        {0, 0, 14, 1, 2},    {90, 5, 254, 726, 531}, {91, 5, 254, 726, 1257},
    };
    for (unsigned int test = 0; test < count_of(cases); ++test) {
        ck_assert_uint_eq(dos_rel_blocks(model, cases[test].group, cases[test].side, cases[test].index),
                          model == DOS_MODEL_8250 ? cases[test].big : cases[test].small);
    }
}
END_TEST

// ADDR1 rounds only spanning endpoints, with model-specific sixth-side carry.
START_TEST(test_growth_endpoint_span_and_group_carry) {
    const dos_model_t model = models[(unsigned int)_i / 4u];
    const unsigned int variant = (unsigned int)_i % 4u;
    const uint8_t size = variant < 2 ? 254 : 255;
    const uint8_t side = variant == 3 ? 5 : 0;
    const dos_rel_address_t address = {.side = side, .index = 254, .offset = variant == 1 ? 3 : 2};
    const dos_rel_growth_plan_t plan = dos_rel_growth_plan_at(model, address, size, 0, side, 254);
    ck_assert_int_eq(plan.too_large, variant == 3 && model != DOS_MODEL_8250);
    if (!plan.too_large) {
        ck_assert_uint_eq(plan.blocks, variant == 0 ? 0 : 1);
        ck_assert_uint_eq(plan.address.index, variant == 0 ? 254 : 16);
        ck_assert_uint_eq(plan.address.side, variant == 0 || variant == 3 ? 0 : 1);
        ck_assert_uint_eq(plan.address.group, variant == 3 ? 1 : 0);
    }
}
END_TEST

// Native SBC and decrement/sign loops are not mathematical endpoint subtraction.
START_TEST(test_growth_preflight_borrow_and_signed_loops) {
    const dos_model_t model = models[_i];
    static const struct {
        uint8_t target_side, current_side, current_index;
        uint16_t small, big;
    } cases[] = {
        {1, 1, 254, 256, 606},
        {0, 128, 16, 15488, 0},
        {0, 129, 16, 15367, 0},
    };
    for (unsigned int test = 0; test < count_of(cases); ++test) {
        const dos_rel_address_t address = {.side = cases[test].target_side, .index = 16, .offset = 2};
        const dos_rel_growth_plan_t plan =
            dos_rel_growth_plan_at(model, address, 1, 0, cases[test].current_side, cases[test].current_index);
        ck_assert(!plan.too_large);
        ck_assert_uint_eq(plan.blocks, model == DOS_MODEL_8250 ? cases[test].big : cases[test].small);
    }
}
END_TEST

// Register native index, clean-read and allocation-preflight contracts.
Suite* dos_rel_suite(void) {
    Suite* const suite = suite_create("dos_rel");
    TCase* const core = tcase_create("lookup and clean reads");
    tcase_add_loop_test(core, test_record_address_boundaries, 0, 3);
    tcase_add_loop_test(core, test_lookup_uses_side_tables_and_super_groups, 0, 12);
    tcase_add_loop_test(core, test_lookup_native_wrapped_indices_and_last_group, 0, 6);
    tcase_add_loop_test(core, test_lookup_retests_selected_side_range, 0, 15);
    tcase_add_loop_test(core, test_lookup_missing_ranges, 0, 12);
    tcase_add_loop_test(core, test_lookup_failure_and_replacement, 0, 18);
    tcase_add_loop_test(core, test_lookup_geometry_errors, 0, 6);
    tcase_add_loop_test(core, test_lookup_cancel_restart_and_peer_isolation, 0, 3);
    tcase_add_loop_test(core, test_end_selects_native_last_side_and_group, 0, 18);
    tcase_add_loop_test(core, test_end_native_corrupt_index, 0, 9);
    tcase_add_loop_test(core, test_end_empty_super_table_handoff, 0, 2);
    tcase_add_loop_test(core, test_end_storage_and_generation_failures, 0, 27);
    tcase_add_loop_test(core, test_end_geometry_errors, 0, 6);
    tcase_add_loop_test(core, test_end_cancel_restart_and_peer_generation, 0, 3);
    tcase_add_loop_test(core, test_record_read_preparation_and_actual_get, 0, 27);
    tcase_add_loop_test(core, test_record_explicit_incomplete_boundaries, 0, 3);
    tcase_add_loop_test(core, test_record_selected_side_range_and_geometry_handoff, 0, 6);
    tcase_add_loop_test(core, test_record_storage_and_stale_failures, 0, 24);
    tcase_add_loop_test(core, test_record_defers_unused_prefetch_failure, 0, 9);
    tcase_add_loop_test(core, test_record_cancel_restart_and_cached_generation, 0, 12);
    tcase_add_loop_test(core, test_record_interleaved_units_and_peer_generation, 0, 3);
    tcase_add_loop_test(core, test_record_next_stream_and_native_pool_pages, 0, 9);
    tcase_add_loop_test(core, test_record_next_retains_failed_prefetch, 0, 9);
    tcase_add_loop_test(core, test_record_next_generation_guards, 0, 12);
    tcase_add_loop_test(core, test_record_next_cancel_and_peer_isolation, 0, 3);
    tcase_add_loop_test(core, test_reader_preview_prefix_missing_and_reposition, 0, 3);
    tcase_add_loop_test(core, test_reader_raw_counter_alias_and_wrap, 0, 9);
    tcase_add_loop_test(core, test_reader_cross_page_interrupted_prefix, 0, 3);
    tcase_add_loop_test(core, test_reader_preview_defers_failed_page_until_actual_get, 0, 3);
    tcase_add_loop_test(core, test_reader_pending_refill_and_deferred_error_counter, 0, 3);
    tcase_add_loop_test(core, test_reader_generation_guards, 0, 12);
    tcase_add_test(core, test_fill_native_record_markers);
    tcase_add_loop_test(core, test_growth_native_preflight, 0, 3);
    tcase_add_loop_test(core, test_blocks_native_endpoints_and_low_word, 0, 3);
    tcase_add_loop_test(core, test_growth_endpoint_span_and_group_carry, 0, 12);
    tcase_add_loop_test(core, test_growth_preflight_borrow_and_signed_loops, 0, 3);
    suite_add_tcase(suite, core);
    return suite;
}

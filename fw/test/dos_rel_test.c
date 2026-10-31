// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"

#include "dos_rel_test.h"

#include <string.h>

#include "ieee/dos.h"
#include "ieee/dos_rel.h"
#include "ieee/ieee_protocol.h"

typedef struct {
    uint8_t bytes[7][DISKIMAGE_SECTOR_SIZE];
    uint32_t offsets[7];
    unsigned int reads;
    unsigned int fail_read;
    unsigned int writes;
    unsigned int flushes;
    unsigned int fail_write;
    bool fail_flush;
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

// Drive the compact reader in its real channel overlay and owned pool pages.
START_TEST(test_reader_claimed_channel_pool_and_teardown) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 2, 200, 0, true);
    memset(image.bytes[3] + 202, 'A', 54);
    memset(image.bytes[4] + 2, 'B', 146);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, IEEE_LOAD_CHANNEL);
    ck_assert_uint_eq(channel, 0);
    dos_channel_t* const state = &unit.channels[channel];
    uint8_t* const first = unit.buffers[state->buffers[0]].bytes;
    uint8_t* const following = unit.buffers[state->buffers[1]].bytes;
    uint8_t* const side = unit.buffers[state->buffers[2]].bytes;
    dos_rel_reader_start(&state->rel, models[format], reference, 1, format == 2 ? 2 : 0, 2, 200, 0);
    ck_assert_int_eq(prepare_record(&state->rel.record, &bank, &image, side, first, following), DOS_REL_RECORD_READY);
    for (unsigned int got = 0; got < 200; ++got) {
        uint8_t output;
        bool eoi;
        ck_assert_uint_eq(dos_rel_reader_stage(&state->rel, &bank, first, following, &output, 1, &eoi), 1);
        ck_assert_uint_eq(output, got < 54 ? 'A' : 'B');
        ck_assert_int_eq(eoi, got == 199);
        ck_assert_int_eq(dos_rel_reader_get(&state->rel, models[format], &bank, first, following),
                         eoi ? DOS_REL_RECORD_MISSING : DOS_REL_RECORD_READY);
    }
    ck_assert_uint_eq(state->rel.number, 3);
    ck_assert(!state->rel.no_record);
    ck_assert(dos_close(&unit, &bank, models[format], IEEE_LOAD_CHANNEL));
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(dos_find_channel(&unit, IEEE_LOAD_CHANNEL), DOS_UNUSED);
}
END_TEST

// The third REL page must not turn exhaustion into a partially published owner.
START_TEST(test_channel_claim_partial_pool_and_logical_exhaustion) {
    const unsigned int format = (unsigned int)_i / 4u;
    const unsigned int available = (unsigned int)_i % 4u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_index(&bank, &image, format, 0, format % DOS_MEDIA_SLOTS_PER_UNIT);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    unit.buffer_use = (1u << DOS_BUFFER_COUNT) - 1u;
    if (available < DOS_CHANNEL_BUFFER_COUNT) {
        for (unsigned int page = 0; page < available; ++page) {
            unit.buffer_use &= (uint16_t)~(1u << (DOS_BUFFER_COUNT - 1u - page));
        }
    } else {
        unit.buffer_use = 0;
        for (unsigned int channel = 0; channel < DOS_FILE_CHANNEL_COUNT; ++channel) {
            unit.channels[channel].allocated = true;
            unit.channels[channel].secondary = channel;
        }
    }
    const uint16_t used = unit.buffer_use;
    dos_channel_t saved[DOS_CHANNEL_COUNT];
    memcpy(saved, unit.channels, sizeof(saved));
    char status[DOS_STATUS_LINE_CAPACITY];
    memcpy(status, unit.status, sizeof(status));
    ck_assert_uint_eq(dos_rel_channel_claim(&unit, reference, IEEE_COMMAND_CHANNEL - 1u), DOS_UNUSED);
    ck_assert_uint_eq(unit.buffer_use, used);
    ck_assert_mem_eq(unit.channels, saved, sizeof(saved));
    ck_assert_mem_eq(unit.status, status, sizeof(status));
    ck_assert_uint_eq(dos_find_channel(&unit, IEEE_COMMAND_CHANNEL - 1u), DOS_UNUSED);
    ck_assert_uint_eq(image.reads, 0);
}
END_TEST

// REL teardown must not reinterpret its reader bytes as ordinary routing flags.
START_TEST(test_channel_claim_peer_teardown_and_ordinary_reuse) {
    const unsigned int format = (unsigned int)_i / DOS_MEDIA_SLOTS_PER_UNIT;
    const unsigned int slot = (unsigned int)_i % DOS_MEDIA_SLOTS_PER_UNIT;
    rel_image_t image = {0}, peer_image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_index(&bank, &image, format, 0, slot);
    const dos_media_ref_t peer_reference = mount_index(&bank, &peer_image, format, 0, slot ^ 1u);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    unit.directory_internal = 1;
    char status[DOS_STATUS_LINE_CAPACITY];
    memcpy(status, unit.status, sizeof(status));
    unit.selected_drive = slot ^ 1u;
    unit.buffer_writes = (1u << DOS_BUFFER_COUNT) - 1u;
    for (unsigned int page = 0; page < DOS_BUFFER_COUNT; ++page) {
        memset(unit.buffers[page].bytes, 0xa5, DISKIMAGE_SECTOR_SIZE);
        unit.buffers[page].resident = true;
        unit.buffers[page].failed = true;
    }
    const uint8_t secondary = 2;
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, secondary);
    ck_assert_uint_eq(channel, 1);
    dos_channel_t* const state = &unit.channels[channel];
    uint16_t claimed = 0;
    for (unsigned int page = 0; page < DOS_CHANNEL_BUFFER_COUNT; ++page) {
        const uint8_t buffer = DOS_BUFFER_COUNT - 1u - page;
        const uint16_t bit = 1u << buffer;
        claimed |= bit;
        ck_assert_uint_eq(state->buffers[page], buffer);
        ck_assert(!unit.buffers[buffer].resident);
        ck_assert(!unit.buffers[buffer].failed);
        ck_assert_uint_eq(unit.buffer_writes & bit, 0);
        ck_assert_int_eq((unit.buffer_drives & bit) != 0, slot != 0);
        for (unsigned int byte = 0; byte < DISKIMAGE_SECTOR_SIZE; ++byte) {
            ck_assert_uint_eq(unit.buffers[buffer].bytes[byte], 0xa5);
        }
    }
    ck_assert_uint_eq(unit.buffer_use, claimed);
    ck_assert_uint_eq(state->file_type, DISKIMAGE_FTYPE_REL);
    ck_assert(!state->ready);
    ck_assert_mem_eq(unit.status, status, sizeof(status));
    dos_rel_reader_start(&state->rel, models[format], reference, 1, 0, UINT16_MAX, UINT8_MAX, UINT8_MAX);
    const uint8_t peer = dos_rel_channel_claim(&unit, peer_reference, secondary + 1u);
    ck_assert_uint_eq(peer, 2);
    dos_rel_reader_start(&unit.channels[peer].rel, models[format], peer_reference, 1, 0, 7, 127, 3);
    // Park this synthetic owner now that pending clean readers are scheduled.
    unit.channels[peer].failed = true;
    const dos_channel_t saved = unit.channels[peer];
    dos_buffer_t pages[DOS_CHANNEL_BUFFER_COUNT];
    for (unsigned int page = 0; page < count_of(pages); ++page) {
        pages[page] = unit.buffers[saved.buffers[page]];
    }
    const uint16_t remaining = unit.buffer_use & (uint16_t)~claimed;
    ck_assert(dos_close(&unit, &bank, models[format], secondary));
    ck_assert_uint_eq(unit.buffer_use, remaining);
    ck_assert_uint_eq(unit.directory_internal, 1);
    ck_assert_uint_eq(dos_find_channel(&unit, secondary), DOS_UNUSED);

    // Reuse the released descriptor as an ordinary direct page, not REL state.
    const uint8_t request[] = "#";
    dos_open(&unit, &bank, 0, IEEE_LOAD_CHANNEL, request, sizeof(request) - 1u);
    ck_assert_uint_eq(dos_find_channel(&unit, IEEE_LOAD_CHANNEL), channel);
    ck_assert_int_eq(state->access, DOS_ACCESS_DIRECT);
    ck_assert_uint_eq(state->buffers[0], DOS_BUFFER_COUNT - 1u);
    ck_assert_uint_eq(state->buffers[1], DOS_UNUSED);
    ck_assert_uint_eq(state->buffers[2], DOS_UNUSED);
    ck_assert(dos_close(&unit, &bank, models[format], IEEE_LOAD_CHANNEL));
    ck_assert_uint_eq(unit.buffer_use, remaining);
    ck_assert_uint_eq(unit.directory_internal, 1);
    ck_assert_mem_eq(&unit.channels[peer], &saved, sizeof(saved));
    for (unsigned int page = 0; page < count_of(pages); ++page) {
        ck_assert_mem_eq(&unit.buffers[saved.buffers[page]], &pages[page], sizeof(pages[page]));
    }
    ck_assert(dos_close(&unit, &bank, models[format], secondary + 1u));
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(unit.directory_internal, 1);
    ck_assert_uint_eq(image.reads, 0);
    ck_assert_uint_eq(peer_image.reads, 0);
}
END_TEST

// Bound cooperative unit service to one media READ or WRITE per call.
static void finish_pipeline(dos_unit_t* const unit, dos_media_bank_t* const bank, rel_image_t* const image) {
    unsigned int steps = 0;
    while (dos_work_pending(unit)) {
        ck_assert_uint_lt(steps++, 16);
        const unsigned int reads = image->reads, writes = image->writes;
        dos_work(unit, bank);
        ck_assert_uint_le(image->reads - reads + image->writes - writes, 1);
    }
}

// Retained source GET, not speculative data or status staging, reports error 50.
START_TEST(test_pipeline_prefix_status_and_missing_get) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 0, 254, 0, false);
    memcpy(image.bytes[3] + 2, "AB\0D", 4);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, 2);
    dos_rel_channel_prepare(&unit, channel, models[format], 1, format == 2 ? 2 : 0, 0, 254, 0);
    dos_status(&unit, models[format], DOS_STATUS_FILE_EXISTS, 0, 0);
    dos_status_get(&unit, models[format], 2);
    char status[DOS_STATUS_LINE_CAPACITY];
    memcpy(status, unit.status, sizeof(status));
    uint8_t output[DOS_STAGE_CAPACITY];
    bool eoi;
    dos_source_t source, other;
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, output, sizeof(output), &eoi, &source), 0);
    ck_assert(!eoi);
    finish_pipeline(&unit, &bank, &image);
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, output, sizeof(output), &eoi, &source), 4);
    ck_assert_mem_eq(output, "AB\0D", 4);
    ck_assert(eoi);
    ck_assert_uint_eq(unit.channels[channel].rel.number, 0);
    ck_assert_uint_gt(dos_output_stage(&unit, &bank, 15, output, sizeof(output), &eoi, &other), 0);
    dos_output_get(&unit, &bank, models[format], source, 1);
    ck_assert_uint_eq(unit.channels[channel].rel.number, 0);
    ck_assert_int_eq(unit.channels[channel].rel.get_active, format == 0);
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, output, sizeof(output), &eoi, &source), 3);
    ck_assert_mem_eq(output, "B\0D", 3);
    ck_assert(eoi);
    dos_output_get(&unit, &bank, models[format], source, 3);
    ck_assert_uint_eq(unit.channels[channel].rel.number, 1);
    ck_assert_mem_eq(unit.status, status, sizeof(status));
    ck_assert_uint_eq(unit.status_position, 2);
    const unsigned int reads = image.reads;
    for (unsigned int repeat = 0; repeat < 3; ++repeat) {
        ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, output, sizeof(output), &eoi, &source), 1);
        ck_assert_uint_eq(output[0], '\r');
        ck_assert(eoi);
        if (repeat == 0) {
            ck_assert_mem_eq(unit.status, status, sizeof(status));
        }
        dos_output_get(&unit, &bank, models[format], source, 1);
        ck_assert_str_eq(unit.status,
                         format == 0 ? "50, RECORD NOT PRESENT,00,00\r" : "50, RECORD NOT PRESENT,00,00,0\r");
        ck_assert_uint_eq(unit.status_position, 0);
        ck_assert_uint_eq(unit.channels[channel].rel.number, 1);
    }
    ck_assert_uint_eq(image.reads, reads);
}
END_TEST

// Channel-owned pending work cannot overwrite another command's tagged union.
START_TEST(test_pipeline_pending_refill_and_close_cancel) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool cancel = (unsigned int)_i % 2u == 0;
    const uint8_t secondary = cancel ? IEEE_LOAD_CHANNEL : 2;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 2, 127, 0, true);
    memset(image.bytes[3] + 129, 'A', 127);
    memset(image.bytes[4] + 2, 'B', DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    image.bytes[4][0] = 1;
    image.bytes[4][1] = 5;
    if (!cancel) {
        image.fail_read = 4u + (format == 2);
    }
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, secondary);
    dos_rel_channel_prepare(&unit, channel, models[format], 1, format == 2 ? 2 : 0, 2, 127, 0);
    finish_pipeline(&unit, &bank, &image);
    unit.command = DOS_COMMAND_BLOCK_MAP;
    unit.block_map = (dos_block_map_work_t){.media = reference, .track = 3, .sector = 4, .suggestion = true};
    const dos_block_map_work_t other = unit.block_map;
    for (unsigned int got = 0; got < 127; ++got) {
        uint8_t output;
        bool eoi;
        dos_source_t source;
        ck_assert_uint_eq(dos_output_stage(&unit, &bank, secondary, &output, 1, &eoi, &source), 1);
        dos_output_get(&unit, &bank, models[format], source, 1);
    }
    ck_assert_int_eq(unit.command, DOS_COMMAND_BLOCK_MAP);
    ck_assert_mem_eq(&unit.block_map, &other, sizeof(other));
    ck_assert_int_eq(unit.channels[channel].rel.record.result, DOS_REL_RECORD_PENDING);
    ck_assert(!unit.channels[channel].ready);
    unit.command = DOS_COMMAND_IDLE;
    ck_assert(dos_work_pending(&unit));
    if (!cancel) {
        finish_pipeline(&unit, &bank, &image);
        ck_assert(unit.channels[channel].rel.record.following_failed);
        for (unsigned int got = 0; got < DISKIMAGE_SECTOR_PAYLOAD_SIZE; ++got) {
            uint8_t output;
            bool eoi;
            dos_source_t source;
            ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &output, 1, &eoi, &source), 1);
            ck_assert_uint_eq(output, 'B');
            dos_output_get(&unit, &bank, models[format], source, 1);
        }
        ck_assert(unit.channels[channel].failed);
        ck_assert(!unit.channels[channel].ready && !dos_work_pending(&unit));
        ck_assert_uint_eq(unit.channels[channel].rel.number, 5);
        ck_assert_str_eq(unit.status, format == 0 ? "23,READ ERROR,00,00\r" : "23,READ ERROR,00,00,0\r");
        ck_assert_uint_eq(image.reads, image.fail_read);
        return;
    }
    const unsigned int reads = image.reads;
    ck_assert(dos_close(&unit, &bank, models[format], secondary));
    ck_assert(!dos_work_pending(&unit));
    dos_work(&unit, &bank);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(dos_rel_channel_claim(&unit, reference, 3), channel);
    dos_work(&unit, &bank);
    ck_assert_uint_eq(image.reads, reads);
}
END_TEST

// Pending reads, cached previews and actual GETs retain their captured model.
START_TEST(test_pipeline_generation_and_io_failures) {
    const unsigned int format = (unsigned int)_i / 5u;
    const unsigned int variant = (unsigned int)_i % 5u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 1, 254, 0, false);
    image.bytes[3][2] = 'Q';
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, 2);
    dos_rel_channel_prepare(&unit, channel, models[format], 1, format == 2 ? 2 : 0, 1, 254, 0);
    if (variant < 2) {
        image.fail_read = variant + 1u;
        finish_pipeline(&unit, &bank, &image);
    } else {
        uint8_t output;
        bool eoi;
        dos_source_t source;
        if (variant != 2) {
            finish_pipeline(&unit, &bank, &image);
            ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &output, 1, &eoi, &source), 1);
        }
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, NULL), DOS_MEDIA_OK);
        const unsigned int reads = image.reads;
        if (variant == 2) {
            finish_pipeline(&unit, &bank, &image);
        } else if (variant == 3) {
            ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &output, 1, &eoi, &source), 0);
        } else {
            dos_output_get(&unit, &bank, models[format], source, 1);
        }
        ck_assert_uint_eq(image.reads, reads);
    }
    ck_assert(unit.channels[channel].failed);
    ck_assert(!unit.channels[channel].ready && !dos_work_pending(&unit));
    ck_assert_uint_eq(unit.channels[channel].rel.number, 1);
    ck_assert_str_eq(unit.status, format == 0 ? "23,READ ERROR,00,00\r" : "23,READ ERROR,00,00,0\r");
    const unsigned int reads = image.reads;
    dos_work(&unit, &bank);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_ne(unit.buffer_use, 0);
}
END_TEST

// Stale media stops a valid multi-byte GET without discarding its owner or pages.
START_TEST(test_pipeline_stale_media_stops_staged_batch) {
    const unsigned int format = (unsigned int)_i / 12u;
    const bool replace = (unsigned int)_i / 6u % 2u != 0;
    const bool retained = (unsigned int)_i / 3u % 2u != 0;
    const bool split = (unsigned int)_i % 3u == 1u;
    const bool failed_preview = (unsigned int)_i % 3u == 2u;
    rel_image_t image = {0}, peer_image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record_at(&bank, &image, format, 1, 254, 0, false, 0, 1);
    const dos_media_ref_t peer_reference = mount_record_at(&bank, &peer_image, format, 1, 254, 0, false, 1, 0);
    memcpy(image.bytes[3] + DISKIMAGE_SECTOR_LINK_SIZE, "ABCD", 4);
    peer_image.bytes[3][DISKIMAGE_SECTOR_LINK_SIZE] = 'P';
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, 2);
    const uint8_t peer = dos_rel_channel_claim(&unit, peer_reference, 3);
    dos_rel_channel_prepare(&unit, channel, models[format], 1, format == 2 ? 2 : 0, 1, 254, 0);
    dos_rel_channel_prepare(&unit, peer, models[format], 1, format == 2 ? 2 : 0, 1, 254, 0);
    finish_pipeline(&unit, &bank, &image);
    uint8_t output[DOS_STAGE_CAPACITY];
    bool eoi;
    dos_source_t source, selected;
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, output, sizeof(output), &eoi, &source), 4);
    ck_assert_mem_eq(output, "ABCD", 4);
    dos_output_get(&unit, &bank, models[format], source, 1);
    const unsigned int length = dos_output_stage(&unit, &bank, 2, output, sizeof(output), &eoi, &source);
    ck_assert_uint_eq(length, 3);
    ck_assert_mem_eq(output, "BCD", 3);
    ck_assert(eoi);
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 3, output, sizeof(output), &eoi, &selected), 1);
    dos_status(&unit, models[format], DOS_STATUS_FILE_EXISTS, 0, 0);
    dos_status_get(&unit, models[format], 2);
    const dos_channel_t before = unit.channels[channel], peer_before = unit.channels[peer];
    const uint16_t use = unit.buffer_use, writes = unit.buffer_writes, drives = unit.buffer_drives;
    dos_buffer_t pages[DOS_CHANNEL_BUFFER_COUNT];
    for (unsigned int page = 0; page < count_of(pages); ++page) {
        pages[page] = unit.buffers[before.buffers[page]];
    }
    if (replace) {
        mount_record_at(&bank, &replacement, format, 1, 254, 0, false, 0, 1);
    } else {
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, NULL), DOS_MEDIA_OK);
    }
    const unsigned int reads = image.reads, peer_reads = peer_image.reads;
    if (failed_preview) {
        ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, output, sizeof(output), &eoi, &selected), 0);
        ck_assert(!eoi);
    }
    if (retained) {
        dos_output_get(&unit, &bank, models[format], source, split ? 1u : length);
        if (split) {
            dos_output_get(&unit, &bank, models[format], source, length - 1u);
        }
    } else {
        dos_get(&unit, &bank, channel, split ? 1u : length);
        if (split) {
            dos_get(&unit, &bank, channel, length - 1u);
        }
    }
    ck_assert(unit.channels[channel].allocated && unit.channels[channel].failed);
    ck_assert(!unit.channels[channel].ready && !dos_work_pending(&unit));
    ck_assert_int_eq(unit.channels[channel].rel.record.result, DOS_REL_RECORD_FAILED);
    ck_assert_int_eq(unit.channels[channel].rel.record.lookup.media_result, DOS_MEDIA_STALE);
    ck_assert_uint_eq(unit.channels[channel].rel.record.pointer, before.rel.record.pointer);
    ck_assert_uint_eq(unit.channels[channel].rel.number, before.rel.number);
    ck_assert_str_eq(unit.status, format == 0 ? "23,READ ERROR,00,00\r" : "23,READ ERROR,00,00,1\r");
    ck_assert_uint_eq(unit.status_position, 0);
    ck_assert_uint_eq(unit.selected_drive, 1);
    ck_assert_uint_eq(unit.buffer_use, use);
    ck_assert_uint_eq(unit.buffer_writes, writes);
    ck_assert_uint_eq(unit.buffer_drives, drives);
    for (unsigned int page = 0; page < count_of(pages); ++page) {
        ck_assert_mem_eq(&unit.buffers[before.buffers[page]], &pages[page], sizeof(pages[page]));
    }
    ck_assert_mem_eq(&unit.channels[peer], &peer_before, sizeof(peer_before));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(peer_image.reads, peer_reads);
    ck_assert_uint_eq(replacement.reads, 0);
    dos_work(&unit, &bank);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(peer_image.reads, peer_reads);
    ck_assert_uint_eq(replacement.reads, 0);
}
END_TEST

// Preparing/repositioning one owner preserves a peer's cursor and borrowed pages.
START_TEST(test_pipeline_peer_reposition_and_drive_status) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t image = {0}, peer = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t a = mount_record_at(&bank, &image, format, 1, 254, 0, false, 1, 0);
    const dos_media_ref_t b = mount_record_at(&bank, &peer, format, 1, 254, 0, false, 1, 1);
    image.bytes[3][2] = 'A';
    peer.bytes[3][2] = 'B';
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t first = dos_rel_channel_claim(&unit, a, 2);
    const uint8_t second = dos_rel_channel_claim(&unit, b, 3);
    dos_rel_channel_prepare(&unit, first, models[format], 1, format == 2 ? 2 : 0, 1, 254, 0);
    finish_pipeline(&unit, &bank, &image);
    dos_rel_channel_prepare(&unit, second, models[format], 1, format == 2 ? 2 : 0, 1, 254, 0);
    finish_pipeline(&unit, &bank, &peer);
    const dos_channel_t before = unit.channels[first];
    dos_buffer_t saved[3];
    for (unsigned int index = 0; index < count_of(saved); ++index) {
        saved[index] = unit.buffers[before.buffers[index]];
    }
    for (unsigned int got = 0; got < 2; ++got) {
        uint8_t output;
        bool eoi;
        dos_source_t source;
        ck_assert_uint_eq(dos_output_stage(&unit, &bank, 3, &output, 1, &eoi, &source), 1);
        ck_assert_uint_eq(output, got == 0 ? 'B' : '\r');
        dos_output_get(&unit, &bank, models[format], source, 1);
    }
    ck_assert_str_eq(unit.status, format == 0 ? "50, RECORD NOT PRESENT,00,00\r" : "50, RECORD NOT PRESENT,00,00,1\r");
    dos_rel_channel_prepare(&unit, second, models[format], 1, format == 2 ? 2 : 0, 1, 254, 0);
    finish_pipeline(&unit, &bank, &peer);
    ck_assert_mem_eq(&unit.channels[first], &before, sizeof(before));
    for (unsigned int index = 0; index < count_of(saved); ++index) {
        ck_assert_mem_eq(&unit.buffers[before.buffers[index]], &saved[index], sizeof(saved[index]));
    }
}
END_TEST

// Command priority and job headers survive successful and partially failed reads.
START_TEST(test_pipeline_command_priority_and_job_headers) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int variant = (unsigned int)_i % 3u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record_at(&bank, &image, format, 1, 10, 0, true, 0, 1);
    memset(image.bytes[3] + DISKIMAGE_SECTOR_LINK_SIZE, 'A', DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    if (variant != 0) {
        image.fail_read = variant + 1u + (format == 2);
    }
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, 2);
    dos_rel_channel_prepare(&unit, channel, models[format], 1, format == 2 ? 2 : 0, 1, 10, 0);
    dos_channel_t* const state = &unit.channels[channel];
    const dos_channel_t pending = *state;
    unit.selected_drive = 0;
    unit.buffer_writes = unit.buffer_use;

    // A stale ordered command completes before any pending REL sector job.
    unit.command = DOS_COMMAND_BLOCK_MAP;
    unit.block_map = (dos_block_map_work_t){.media = reference, .track = 1};
    ++unit.block_map.media.identity;
    dos_work(&unit, &bank);
    ck_assert_int_eq(unit.command, DOS_COMMAND_IDLE);
    ck_assert_uint_eq(image.reads, 0);
    ck_assert_mem_eq(state, &pending, sizeof(pending));
    ck_assert(dos_work_pending(&unit));
    finish_pipeline(&unit, &bank, &image);

    const dos_buffer_t* const current = &unit.buffers[state->buffers[0]];
    const dos_buffer_t* const following = &unit.buffers[state->buffers[1]];
    ck_assert_uint_eq(current->track, 1);
    ck_assert_uint_eq(current->sector, 3);
    ck_assert_int_eq(current->resident, variant != 1);
    ck_assert_int_eq(current->failed, variant == 1);
    ck_assert_int_eq(state->ready, variant != 1);
    ck_assert_int_eq(state->failed, variant == 1);
    const uint16_t current_bit = 1u << state->buffers[0];
    const uint16_t following_bit = 1u << state->buffers[1];
    ck_assert_uint_eq(unit.buffer_drives & current_bit, current_bit);
    ck_assert_uint_eq(unit.buffer_writes & current_bit, 0);
    if (variant != 1) {
        ck_assert_uint_eq(following->track, 1);
        ck_assert_uint_eq(following->sector, 4);
        ck_assert_int_eq(following->resident, variant == 0);
        ck_assert_int_eq(following->failed, variant == 2);
        ck_assert_uint_eq(unit.buffer_drives & following_bit, following_bit);
        ck_assert_uint_eq(unit.buffer_writes & following_bit, 0);
    } else {
        ck_assert_uint_eq(unit.buffer_writes & following_bit, following_bit);
    }
    const unsigned int reads = image.reads;
    dos_rel_channel_prepare(&unit, channel, models[format], 1, format == 2 ? 2 : 0, 1, 10, 0);
    ck_assert(!current->resident && !current->failed && !following->resident && !following->failed);
    ck_assert_uint_eq(image.reads, reads);
}
END_TEST

// Cold SSEND remains incomplete, while clean terminal NRBUF retains NONE-job bytes.
START_TEST(test_pipeline_retains_incomplete_boundaries) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool link = (unsigned int)_i % 2u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 1, 10, 2, false);
    image.bytes[3][2] = 'A';
    if (!link) {
        image.bytes[0][DOS_REL_POINTER_OFFSET] = 0;
    }
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, 2);
    dos_rel_channel_prepare(&unit, channel, models[format], 1, format == 2 ? 2 : 0, 1, 10, 2);
    finish_pipeline(&unit, &bank, &image);
    for (unsigned int count = 0; link && unit.channels[channel].ready;) {
        uint8_t output[DOS_STAGE_CAPACITY];
        bool eoi;
        dos_source_t source;
        const unsigned int length = dos_output_stage(&unit, &bank, 2, output, sizeof(output), &eoi, &source);
        ck_assert_uint_gt(length, 0);
        ck_assert(!eoi);
        count += length;
        ck_assert_uint_le(count, DISKIMAGE_SECTOR_SIZE);
        dos_output_get(&unit, &bank, models[format], source, length);
    }
    if (link) {
        ck_assert_int_eq(unit.channels[channel].rel.record.result, DOS_REL_RECORD_PENDING);
        ck_assert(!unit.channels[channel].ready && dos_work_pending(&unit));
        const unsigned int reads = image.reads;
        const uint16_t number = unit.channels[channel].rel.number;
        finish_pipeline(&unit, &bank, &image);
        ck_assert_uint_eq(image.reads, reads);
        ck_assert(unit.channels[channel].ready && !unit.channels[channel].failed);
        ck_assert_uint_eq(unit.channels[channel].rel.number, number);
        ck_assert_int_eq(unit.status_code, DOS_STATUS_POWER_ON);
        uint8_t output = 0xa5;
        bool eoi;
        dos_source_t source;
        ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &output, 1, &eoi, &source), 1);
        ck_assert_uint_eq(output, 0);
        ck_assert(eoi);
        return;
    }
    ck_assert_int_eq(unit.channels[channel].rel.record.result, DOS_REL_RECORD_NO_INDEX);
    ck_assert(unit.channels[channel].allocated && unit.channels[channel].failed);
    ck_assert(!unit.channels[channel].ready && !dos_work_pending(&unit));
    ck_assert_int_eq(unit.status_code, DOS_STATUS_SYNTAX);
    ck_assert_uint_ne(unit.buffer_use, 0);
    const unsigned int reads = image.reads;
    dos_work(&unit, &bank);
    ck_assert_uint_eq(image.reads, reads);
}
END_TEST

// A later source selection cannot redirect status or ordinary prefix consumption.
START_TEST(test_output_sources_keep_status_and_ordinary_owners) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    mount_index(&bank, &image, format, 0, 0);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t request[] = "#";
    dos_open(&unit, &bank, 0, 2, request, sizeof(request) - 1u);
    dos_open(&unit, &bank, 0, 3, request, sizeof(request) - 1u);
    const uint8_t first = dos_find_channel(&unit, 2);
    const uint8_t second = dos_find_channel(&unit, 3);
    dos_status(&unit, models[format], DOS_STATUS_FILE_EXISTS, 0, 0);
    uint8_t output[DOS_STAGE_CAPACITY];
    bool eoi;
    dos_source_t status, ordinary, selected;
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, IEEE_COMMAND_CHANNEL, output, 2, &eoi, &status), 2);
    ck_assert_mem_eq(output, "63", 2);
    ck_assert(!eoi);
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, output, 1, &eoi, &ordinary), 1);
    ck_assert_uint_eq(output[0], unit.channels[first].buffers[0]);
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 3, output, 1, &eoi, &selected), 1);
    const dos_channel_t peer = unit.channels[second];
    dos_output_get(&unit, &bank, models[format], status, 2);
    ck_assert_uint_eq(unit.status_position, 2);
    dos_output_get(&unit, &bank, models[format], ordinary, 1);
    ck_assert_uint_eq(unit.channels[first].position, 2);
    ck_assert_mem_eq(&unit.channels[second], &peer, sizeof(peer));
    ck_assert_uint_eq(unit.status_position, 2);
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 4, output, 1, &eoi, &selected), 0);
    ck_assert(!eoi);
    const unsigned int unread = unit.status_length - unit.status_position;
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, IEEE_COMMAND_CHANNEL, output, sizeof(output), &eoi, &status),
                      unread);
    ck_assert(eoi);
    dos_output_get(&unit, &bank, models[format], status, unread);
    ck_assert_str_eq(unit.status, format == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r");
    ck_assert_uint_eq(image.reads, 0);
}
END_TEST

// Persist fixture bytes so unexpected clean-OPEN writes remain observable.
static bool write_open_index(void* const context, const uint32_t offset, const void* const input, const size_t length) {
    rel_image_t* const image = context;
    ck_assert_uint_eq(length, DISKIMAGE_SECTOR_SIZE);
    ++image->writes;
    for (unsigned int page = 0; page < count_of(image->bytes); ++page) {
        if (image->offsets[page] == offset) {
            memcpy(image->bytes[page], input, length);
            return true;
        }
    }
    ck_abort_msg("unexpected REL OPEN write at offset %u", offset);
    return false;
}

// Count backing-store flushes independently of sector publication.
static bool flush_open_index(void* const context) {
    ++((rel_image_t*)context)->flushes;
    return true;
}

// Bind three linked data pages with observable memory-backed writes.
static dos_media_ref_t mount_open_image(dos_media_bank_t* const bank, rel_image_t* const image,
                                        const unsigned int format, const unsigned int unit, const unsigned int slot) {
    dos_media_ref_t reference = mount_record_at(bank, image, format, 1, 200, 0, true, unit, slot);
    image->bytes[4][0] = 1;
    image->bytes[4][1] = 5;
    image->bytes[5][1] = UINT8_MAX;
    for (unsigned int page = 3; page < 6; ++page) {
        memset(image->bytes[page] + 2, 'A' + page - 3u, DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    }
    dos_media_io_t io = bank->units[unit].slots[slot].io;
    io.image.write = write_open_index;
    io.flush = flush_open_index;
    ck_assert_int_eq(dos_media_set_slot(bank, unit, slot, &io), DOS_MEDIA_OK);
    ck_assert_int_eq(dos_media_reference(bank, unit, slot, &reference), DOS_MEDIA_OK);
    return reference;
}

// OPNRCH uses the directory link before any side-index interpretation.
START_TEST(test_pipeline_open_directory_link_and_initial_record) {
    const unsigned int format = (unsigned int)_i / 6u;
    const unsigned int variant = (unsigned int)_i % 6u;
    static const uint8_t record_sizes[] = {1, 200, 127, 254, 255, 200};
    const uint8_t data_sector = variant == 1 ? 4 : 3;
    const bool linked = variant < 2 || variant == 5;
    const uint8_t root_sector = variant == 5 ? 6 : format == 2 ? 2 : 0;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_open_image(&bank, &image, format, 3, 1);
    if (!linked) {
        image.bytes[data_sector][0] = 0;
        image.bytes[data_sector][1] = UINT8_MAX;
    }
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    dos_status(&unit, models[format], DOS_STATUS_FILE_EXISTS, 0, 0);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, 2);
    dos_channel_t* const state = &unit.channels[channel];
    state->directory_track = 1;
    state->directory_sector = 6;
    state->directory_entry = DISKIMAGE_DIRECTORY_ENTRY_SIZE;
    dos_rel_channel_prepare_open(&unit, channel, models[format], 1, root_sector, 1, data_sector, record_sizes[variant]);
    finish_pipeline(&unit, &bank, &image);
    const dos_rel_record_t* const record = &state->rel.record;
    const bool missing = variant == 4;
    ck_assert(state->ready && !state->failed);
    ck_assert_int_eq(record->result, missing ? DOS_REL_RECORD_MISSING : DOS_REL_RECORD_READY);
    ck_assert_uint_eq(record->pointer, DISKIMAGE_SECTOR_LINK_SIZE);
    ck_assert_uint_eq(record->next, missing ? 2 : variant == 3 ? 0 : (uint8_t)(2u + record_sizes[variant]));
    ck_assert_uint_eq(record->last, missing ? 2 : (uint8_t)(1u + record_sizes[variant]));
    ck_assert_uint_eq(state->rel.number, 1);
    ck_assert(!state->rel.get_active && !state->rel.no_record);
    ck_assert_uint_eq(state->rel_root_track, 1);
    ck_assert_uint_eq(state->rel_root_sector, root_sector);
    ck_assert_uint_eq(state->directory_sector, 6);
    ck_assert_uint_eq(state->directory_entry, DISKIMAGE_DIRECTORY_ENTRY_SIZE);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_FILE_EXISTS);
    ck_assert_uint_eq(image.reads, 1u + linked + (format != 2));
    ck_assert_uint_eq(image.writes, 0);
    ck_assert_uint_eq(image.flushes, 0);
    const dos_buffer_t* const first = &unit.buffers[state->buffers[0]];
    const dos_buffer_t* const following = &unit.buffers[state->buffers[1]];
    const dos_buffer_t* const side = &unit.buffers[state->buffers[2]];
    ck_assert(first->resident && !first->failed);
    ck_assert_uint_eq(first->track, 1);
    ck_assert_uint_eq(first->sector, data_sector);
    ck_assert_int_eq(following->resident, linked);
    ck_assert_int_eq(side->resident, format != 2);
    if (format != 2) {
        ck_assert_uint_eq(side->track, 1);
        ck_assert_uint_eq(side->sector, root_sector);
        ck_assert_mem_eq(side->bytes, image.bytes[root_sector], DISKIMAGE_SECTOR_SIZE);
    }
    uint8_t output;
    bool eoi;
    dos_source_t source;
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &output, 1, &eoi, &source), 1);
    ck_assert_uint_eq(output, missing ? '\r' : variant == 1 ? 'B' : 'A');
    ck_assert_int_eq(eoi, missing || record_sizes[variant] == 1);
    dos_output_get(&unit, &bank, models[format], source, 1);
    ck_assert_int_eq(unit.status_code, missing ? DOS_STATUS_NO_RECORD : DOS_STATUS_FILE_EXISTS);
}
END_TEST

// Failed initial jobs never publish partial bytes or follow replacement media.
START_TEST(test_pipeline_open_failed_jobs_and_lifetime) {
    const unsigned int format = (unsigned int)_i / 4u;
    const unsigned int variant = (unsigned int)_i % 4u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_open_image(&bank, &image, format, 3, 1);
    image.fail_read = variant + 1u;
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, 2);
    dos_rel_channel_prepare_open(&unit, channel, models[format], 1, format == 2 ? 2 : 0, 1, 3, 10);
    if (variant == 3) {
        dos_work(&unit, &bank);
        mount_open_image(&bank, &replacement, format, 3, 1);
    }
    finish_pipeline(&unit, &bank, &image);
    dos_channel_t* const state = &unit.channels[channel];
    const bool failed = variant == 0 || variant == 3 || (variant == 2 && format != 2);
    ck_assert_int_eq(state->failed, failed);
    ck_assert_int_eq(state->ready, !failed);
    ck_assert(!dos_work_pending(&unit));
    ck_assert_uint_eq(replacement.reads, 0);
    ck_assert_uint_eq(image.writes, 0);
    ck_assert_uint_eq(image.flushes, 0);
    if (variant == 3) {
        ck_assert_int_eq(state->rel.record.lookup.media_result, DOS_MEDIA_STALE);
        ck_assert_uint_eq(image.reads, 1);
    }
    if (variant == 1) {
        ck_assert(state->rel.record.following_failed);
        ck_assert(!unit.buffers[state->buffers[1]].resident);
    }
    uint8_t output = 0xa5;
    bool eoi;
    dos_source_t source;
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &output, 1, &eoi, &source), !failed);
    ck_assert_uint_eq(output, failed ? 0xa5 : 'A');
    if (failed) {
        ck_assert_int_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    }
}
END_TEST

// A failed OPEN cursor does not prevent an independent peer from becoming ready.
START_TEST(test_pipeline_open_peer_isolation) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    rel_image_t images[2] = {0};
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    uint8_t channels[2];
    for (unsigned int slot = 0; slot < count_of(images); ++slot) {
        mount_open_image(&bank, &images[slot], format, 3, slot);
    }
    // Binding the complete pair invalidates earlier slot tokens.
    for (unsigned int slot = 0; slot < count_of(images); ++slot) {
        dos_media_ref_t reference;
        ck_assert_int_eq(dos_media_reference(&bank, 3, slot, &reference), DOS_MEDIA_OK);
        channels[slot] = dos_rel_channel_claim(&unit, reference, 2u + slot);
        dos_rel_channel_prepare_open(&unit, channels[slot], models[format], 1, format == 2 ? 2 : 0, 1, 3, 10);
    }
    images[0].fail_read = 1;
    images[1].bytes[3][2] = 'Z';
    finish_pipeline(&unit, &bank, &images[0]);
    ck_assert(unit.channels[channels[0]].failed && !unit.channels[channels[0]].ready);
    ck_assert(!unit.channels[channels[1]].failed && unit.channels[channels[1]].ready);
    uint8_t output;
    bool eoi;
    dos_source_t source;
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 3, &output, 1, &eoi, &source), 1);
    ck_assert_uint_eq(output, 'Z');
    ck_assert_uint_eq(unit.channels[channels[1]].rel.number, 1);
    ck_assert_uint_eq(images[0].reads, 1);
    ck_assert_uint_eq(images[1].writes, 0);
}
END_TEST

// Each initial job has native order, and CLOSE cancels either pending data phase.
START_TEST(test_pipeline_open_step_order_and_cancel) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int variant = (unsigned int)_i % 3u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_open_image(&bank, &image, format, 3, 1);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, 2);
    dos_channel_t* const state = &unit.channels[channel];
    dos_buffer_t* const first = &unit.buffers[state->buffers[0]];
    dos_buffer_t* const following = &unit.buffers[state->buffers[1]];
    dos_buffer_t* const side = &unit.buffers[state->buffers[2]];
    dos_rel_channel_prepare_open(&unit, channel, models[format], 1, format == 2 ? 2 : 0, 1, 3, 10);
    ck_assert_uint_eq(image.reads, 0);
    ck_assert(!state->ready && dos_work_pending(&unit));
    dos_work(&unit, &bank);
    ck_assert_uint_eq(image.reads, 1);
    ck_assert(first->resident && !following->resident && !side->resident);
    ck_assert_uint_eq(first->track, 1);
    ck_assert_uint_eq(first->sector, 3);
    ck_assert(!state->ready);
    if (variant != 1) {
        dos_work(&unit, &bank);
        ck_assert_uint_eq(image.reads, 2);
        ck_assert(first->resident && following->resident && !side->resident);
        ck_assert_uint_eq(following->track, 1);
        ck_assert_uint_eq(following->sector, 4);
        ck_assert(!state->ready);
    }
    if (variant == 0) {
        dos_work(&unit, &bank);
        ck_assert_uint_eq(image.reads, 2u + (format != 2));
        ck_assert_int_eq(side->resident, format != 2);
        ck_assert(state->ready && !state->failed && !dos_work_pending(&unit));
        ck_assert_uint_eq(unit.buffer_writes & unit.buffer_use, 0);
        ck_assert_uint_eq(unit.buffer_drives & unit.buffer_use, unit.buffer_use);
    }
    const unsigned int reads = image.reads;
    ck_assert(dos_close(&unit, &bank, models[format], 2));
    ck_assert_uint_eq(unit.buffer_use, 0);
    const dos_unit_t closed = unit;
    dos_work(&unit, &bank);
    ck_assert_mem_eq(&unit, &closed, sizeof(closed));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes | image.flushes, 0);
}
END_TEST

// Clean actual GETs queue native read-ahead without borrowing writer ownership.
START_TEST(test_pipeline_open_clean_nrbuf) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int variant = (unsigned int)_i % 3u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_open_image(&bank, &image, format, 3, 1);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, IEEE_LOAD_CHANNEL);
    dos_channel_t* const state = &unit.channels[channel];
    dos_rel_channel_prepare_open(&unit, channel, models[format], 1, format == 2 ? 2 : 0, 1, 3, 200);
    finish_pipeline(&unit, &bank, &image);
    for (unsigned int byte = 0; byte < DISKIMAGE_SECTOR_PAYLOAD_SIZE - 1u; ++byte) {
        uint8_t output;
        bool eoi;
        dos_source_t source;
        ck_assert_uint_eq(dos_output_stage(&unit, &bank, IEEE_LOAD_CHANNEL, &output, 1, &eoi, &source), 1);
        dos_output_get(&unit, &bank, models[format], source, 1);
        finish_pipeline(&unit, &bank, &image);
    }

    uint8_t output[16];
    bool eoi;
    dos_source_t source;
    const dos_unit_t before = unit;
    const unsigned int reads = image.reads;
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, IEEE_LOAD_CHANNEL, output, sizeof(output), &eoi, &source), 1);
    ck_assert_uint_eq(output[0], 'A');
    ck_assert(!eoi);
    ck_assert_mem_eq(&unit, &before, sizeof(before));
    dos_output_get(&unit, &bank, models[format], source, 1);
    ck_assert(!state->ready && !state->failed && dos_work_pending(&unit));
    ck_assert_int_eq(state->access, DOS_ACCESS_READ);
    ck_assert_uint_eq(state->rel.record.active, 1);
    ck_assert_uint_eq(image.reads, reads);
    if (variant == 1) {
        image.fail_read = reads + 1u;
    } else if (variant == 2) {
        mount_open_image(&bank, &replacement, format, 3, 1);
    }
    finish_pipeline(&unit, &bank, &image);
    ck_assert_uint_eq(replacement.reads, 0);
    ck_assert_uint_eq(image.reads, reads + (variant != 2));
    ck_assert_uint_eq(image.writes | image.flushes, 0);
    ck_assert_int_eq(state->ready, variant != 2);
    ck_assert_int_eq(state->failed, variant == 2);
    if (variant != 2) {
        const dos_buffer_t* const following = &unit.buffers[state->buffers[0]];
        ck_assert_uint_eq(following->sector, 5);
        ck_assert_int_eq(following->failed, variant == 1);
        ck_assert_int_eq(state->rel.record.following_failed, variant == 1);
        ck_assert_uint_eq(dos_output_stage(&unit, &bank, IEEE_LOAD_CHANNEL, output, 1, &eoi, &source), 1);
        ck_assert_uint_eq(output[0], 'B');
        ck_assert_int_eq(unit.status_code, DOS_STATUS_POWER_ON);
    }
    if (variant == 1) {
        for (unsigned int byte = 0; !state->failed; ++byte) {
            ck_assert_uint_lt(byte, DISKIMAGE_SECTOR_PAYLOAD_SIZE);
            ck_assert_uint_eq(dos_output_stage(&unit, &bank, IEEE_LOAD_CHANNEL, output, 1, &eoi, &source), 1);
            dos_output_get(&unit, &bank, models[format], source, 1);
            finish_pipeline(&unit, &bank, &image);
        }
        ck_assert_uint_eq(state->rel.number, 3);
        ck_assert_int_eq(unit.status_code, DOS_STATUS_READ_ERROR);
        ck_assert(!dos_work_pending(&unit));
    }
    ck_assert(dos_close(&unit, &bank, models[format], IEEE_LOAD_CHANNEL));
    ck_assert_uint_eq(unit.buffer_use, 0);
}
END_TEST

// A queued clean seam retains its owner through reused-page refill, failure or CLOSE.
START_TEST(test_clean_nrbuf_refill_failure_and_cancel) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int variant = (unsigned int)_i % 3u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_open_image(&bank, &image, format, 3, 1);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, 2);
    dos_rel_channel_prepare_open(&unit, channel, models[format], 1, format == 2 ? 2 : 0, 1, 3, 200);
    finish_pipeline(&unit, &bank, &image);
    for (unsigned int count = 0; count < DISKIMAGE_SECTOR_PAYLOAD_SIZE; ++count) {
        uint8_t byte;
        bool eoi;
        dos_source_t source;
        ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &byte, 1, &eoi, &source), 1);
        dos_output_get(&unit, &bank, models[format], source, 1);
        if (count + 1u < DISKIMAGE_SECTOR_PAYLOAD_SIZE) {
            finish_pipeline(&unit, &bank, &image);
        }
    }
    dos_channel_t* const state = &unit.channels[channel];
    ck_assert(!state->ready && !state->failed && dos_work_pending(&unit));
    const unsigned int reads = image.reads;
    const uint16_t use = unit.buffer_use;
    const uint16_t number = state->rel.number;
    if (variant == 2) {
        ck_assert(dos_close(&unit, &bank, models[format], 2));
        const dos_unit_t closed = unit;
        dos_work(&unit, &bank);
        ck_assert_mem_eq(&unit, &closed, sizeof(closed));
        ck_assert_uint_eq(image.reads, reads);
        ck_assert_uint_eq(unit.buffer_use, 0);
    } else {
        const uint8_t page = state->buffers[state->rel.record.active];
        unit.buffer_writes |= (uint16_t)(1u << page);
        if (variant == 1) {
            image.fail_read = reads + 1u;
        }
        finish_pipeline(&unit, &bank, &image);
        ck_assert_uint_eq(image.reads, reads + (variant == 0 ? 2u : 1u));
        ck_assert_int_eq(state->failed, variant == 1);
        ck_assert_int_eq(state->ready, variant == 0);
        ck_assert_uint_eq(state->rel.number, number);
        ck_assert_uint_eq(unit.buffer_use, use);
        ck_assert_uint_eq(unit.buffers[page].track, 1);
        ck_assert_uint_eq(unit.buffers[page].sector, 4);
        ck_assert_int_eq(unit.buffers[page].resident, variant == 0);
        ck_assert_int_eq(unit.buffers[page].failed, variant == 1);
        ck_assert_uint_eq(unit.buffer_writes & (1u << page), 0);
        ck_assert_int_eq(unit.status_code, variant == 1 ? DOS_STATUS_READ_ERROR : DOS_STATUS_POWER_ON);
        const dos_unit_t completed = unit;
        dos_work(&unit, &bank);
        ck_assert_mem_eq(&unit, &completed, sizeof(completed));
    }
    ck_assert_uint_eq(image.writes | image.flushes, 0);
}
END_TEST

// NXTREC selects NR after a terminal unbuffered seam without recounting the record.
START_TEST(test_clean_nrbuf_terminal_next_pointer) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 1, 10, 0, false);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, 2);
    dos_rel_channel_prepare(&unit, channel, models[format], 1, format == 2 ? 2 : 0, 1, 10, 0);
    finish_pipeline(&unit, &bank, &image);
    dos_channel_t* const state = &unit.channels[channel];
    dos_rel_record_t* const record = &state->rel.record;
    record->pointer = record->last = UINT8_MAX;
    record->next = DISKIMAGE_SECTOR_LINK_SIZE;
    unit.buffers[state->buffers[record->active ^ 1u]].bytes[DISKIMAGE_SECTOR_LINK_SIZE] = 'Q';
    uint8_t byte;
    bool eoi;
    dos_source_t source;
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &byte, 1, &eoi, &source), 1);
    ck_assert(eoi);
    const unsigned int reads = image.reads;
    const uint16_t number = state->rel.number;
    dos_output_get(&unit, &bank, models[format], source, 1);
    ck_assert(dos_work_pending(&unit));
    ck_assert_uint_eq(record->pointer, DISKIMAGE_SECTOR_LINK_SIZE);
    ck_assert_uint_eq(state->rel.number, (uint16_t)(number + 1u));
    finish_pipeline(&unit, &bank, &image);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(state->rel.number, (uint16_t)(number + 1u));
    ck_assert(state->ready && !state->failed);
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &byte, 1, &eoi, &source), 1);
    ck_assert_uint_eq(byte, 'Q');
    ck_assert(eoi);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_POWER_ON);
}
END_TEST

// Resident terminal NXTREC keeps the selected record, including wrapped NR zero.
START_TEST(test_clean_nrbuf_resident_terminal_records) {
    const unsigned int format = (unsigned int)_i / 4u;
    const bool direct_get = (unsigned int)_i % 2u != 0;
    const unsigned int size =
        (unsigned int)_i % 4u < 2u ? DISKIMAGE_SECTOR_PAYLOAD_SIZE / 2u : DISKIMAGE_SECTOR_PAYLOAD_SIZE;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_open_image(&bank, &image, format, 3, 1);
    image.bytes[4][0] = 0;
    image.bytes[4][1] = UINT8_MAX;
    for (unsigned int offset = 0; offset < 2u * DISKIMAGE_SECTOR_PAYLOAD_SIZE; ++offset) {
        image.bytes[3u + offset / DISKIMAGE_SECTOR_PAYLOAD_SIZE]
                   [DISKIMAGE_SECTOR_LINK_SIZE + offset % DISKIMAGE_SECTOR_PAYLOAD_SIZE] = 'A' + offset / size;
    }
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, 2);
    dos_rel_channel_prepare_open(&unit, channel, models[format], 1, format == 2 ? 2 : 0, 1, 3, size);
    finish_pipeline(&unit, &bank, &image);
    const unsigned int reads = image.reads;
    dos_channel_t* const state = &unit.channels[channel];
    for (unsigned int offset = 0; offset < 2u * DISKIMAGE_SECTOR_PAYLOAD_SIZE; ++offset) {
        uint8_t byte;
        bool eoi;
        dos_source_t source;
        ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &byte, 1, &eoi, &source), 1);
        ck_assert_uint_eq(byte, 'A' + offset / size);
        ck_assert_int_eq(eoi, (offset + 1u) % size == 0);
        ck_assert_uint_eq(state->rel.number, 1u + offset / size);
        if (direct_get) {
            dos_get(&unit, &bank, channel, 1);
        } else {
            dos_output_get(&unit, &bank, models[format], source, 1);
        }
        finish_pipeline(&unit, &bank, &image);
        ck_assert(state->ready && !state->failed);
        ck_assert_uint_eq(image.reads, reads);
        ck_assert_int_eq(unit.status_code, DOS_STATUS_POWER_ON);
    }
    uint8_t byte;
    bool eoi;
    dos_source_t source;
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &byte, 1, &eoi, &source), 1);
    ck_assert_uint_eq(byte, '\r');
    ck_assert(eoi);
    ck_assert_uint_eq(image.writes | image.flushes, 0);
    ck_assert(dos_close(&unit, &bank, models[format], 2));
    ck_assert_uint_eq(unit.buffer_use, 0);
}
END_TEST

// Clean POSBUF trusts BHERE headers, preserves the side page and rotates cold reads.
START_TEST(test_clean_position_buffer_reuse) {
    const unsigned int format = (unsigned int)_i / 8u;
    const unsigned int variant = (unsigned int)_i % 8u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_open_image(&bank, &image, format, 3, 1);
    if (variant == 4) {
        image.bytes[3][0] = 0;
    } else if (variant == 5) {
        image.bytes[4][0] = 0;
    }
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, 2);
    dos_rel_channel_prepare_open(&unit, channel, models[format], 1, format == 2 ? 2 : 0, 1, 3, 200);
    finish_pipeline(&unit, &bank, &image);
    dos_channel_t* const state = &unit.channels[channel];
    const uint8_t target = variant < 2 || variant == 6 ? 3 : variant == 2 || variant == 5 || variant == 7 ? 4 : 5;
    const uint8_t position = variant == 1 ? 2 : 0;
    const uint16_t number = variant == 6 ? 0 : 1;
    const uint8_t expected_active = variant == 2 || variant == 4 || variant == 5 || variant == 7;
    if (variant >= 6) {
        // Previous WRITE headers have valid bytes, but are not dirty ownership.
        const uint8_t page = state->buffers[expected_active];
        unit.buffer_writes |= (uint16_t)(1u << page);
        unit.buffers[page].resident = false;
    }
    state->rel.get_active = true;
    state->rel.no_record = true;
    dos_status(&unit, models[format], DOS_STATUS_FILE_EXISTS, 0, 0);
    dos_status_get(&unit, models[format], 2);
    const dos_unit_t before = unit;
    const dos_buffer_t side = unit.buffers[state->buffers[2]];
    const unsigned int reads = image.reads;
    ck_assert(dos_rel_channel_position_buffer(&unit, channel, number, position, 1, target));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert(!state->ready && !state->failed && dos_work_pending(&unit));
    ck_assert(!state->rel.no_record);
    ck_assert_int_eq(state->rel.get_active, format != 0);
    const dos_unit_t pending = unit;
    ck_assert(!dos_rel_channel_position_buffer(&unit, channel, 2, 0, 1, 3));
    ck_assert_mem_eq(&unit, &pending, sizeof(pending));
    uint8_t output;
    bool eoi;
    dos_source_t source;
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &output, 1, &eoi, &source), 0);
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->ready && !state->failed);
    ck_assert_uint_eq(image.reads, reads + (variant == 2 || variant == 3 || variant == 4 || variant == 7));
    ck_assert_uint_eq(image.writes | image.flushes, 0);
    ck_assert_uint_eq(state->rel.record.active, expected_active);
    ck_assert_uint_eq(state->rel.record.pointer, DISKIMAGE_SECTOR_LINK_SIZE + (position != 0));
    ck_assert_uint_eq(state->rel.number, number);
    ck_assert_uint_eq(unit.buffer_use, before.buffer_use);
    ck_assert_mem_eq(unit.status, before.status, sizeof(unit.status));
    ck_assert_uint_eq(unit.status_position, before.status_position);
    ck_assert_mem_eq(&unit.buffers[state->buffers[2]], &side, sizeof(side));
    ck_assert_uint_eq(unit.buffers[state->buffers[expected_active]].track, 1);
    ck_assert_uint_eq(unit.buffers[state->buffers[expected_active]].sector, target);
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &output, 1, &eoi, &source), 1);
    ck_assert_uint_eq(output, 'A' + target - 3u);
    ck_assert(!eoi);
}
END_TEST

// Positioned RDREL traverses complete records with native NRBUF reads and WRITE-header reuse.
START_TEST(test_clean_position_buffer_get_continuation) {
    const unsigned int format = (unsigned int)_i / 6u;
    const unsigned int variant = (unsigned int)_i % 6u;
    const uint8_t target = variant == 1 || variant == 4 ? 4 : variant == 2 ? 5 : 3;
    const uint16_t number = target - 1u;
    const unsigned int size = 200;
    const bool saved_write = variant == 3 || variant == 4;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_open_image(&bank, &image, format, 3, 1);
    image.bytes[5][0] = 1;
    image.bytes[5][1] = 6;
    image.bytes[6][1] = UINT8_MAX;
    for (unsigned int offset = 0; offset < 4u * DISKIMAGE_SECTOR_PAYLOAD_SIZE; ++offset) {
        image.bytes[3u + offset / DISKIMAGE_SECTOR_PAYLOAD_SIZE]
                   [DISKIMAGE_SECTOR_LINK_SIZE + offset % DISKIMAGE_SECTOR_PAYLOAD_SIZE] = 'A' + offset / size;
    }
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, 2);
    dos_rel_channel_prepare_open(&unit, channel, models[format], 1, format == 2 ? 2 : 0, 1, 3, size);
    finish_pipeline(&unit, &bank, &image);
    dos_channel_t* const state = &unit.channels[channel];
    if (saved_write || variant == 5) {
        // Distinct saved WRITE bytes prove BHERE reuse, while NRBUF must refill a selected WRITE alternate.
        const uint8_t page = state->buffers[saved_write ? target - 3u : 1u];
        unit.buffer_writes |= (uint16_t)(1u << page);
        unit.buffers[page].resident = false;
        for (unsigned int offset = DISKIMAGE_SECTOR_LINK_SIZE; offset < DISKIMAGE_SECTOR_SIZE; ++offset) {
            unit.buffers[page].bytes[offset] += 'a' - 'A';
        }
    }
    dos_status(&unit, models[format], DOS_STATUS_FILE_EXISTS, 0, 0);
    dos_status_get(&unit, models[format], 2);
    const unsigned int reads = image.reads;
    const unsigned int position_reads = target - 3u;
    const unsigned int continuation_reads = variant == 5 ? 3u : 5u - target;
    ck_assert(dos_rel_channel_position_buffer(&unit, channel, number, 0, 1, target));
    finish_pipeline(&unit, &bank, &image);
    ck_assert_uint_eq(image.reads, reads + position_reads);
    const dos_unit_t positioned = unit;
    const unsigned int start = (number - 1u) * size;
    for (unsigned int consumed = 0; consumed < 2u * size; ++consumed) {
        uint8_t byte;
        bool eoi;
        dos_source_t source;
        const unsigned int offset = start + consumed;
        const unsigned int record = offset / size;
        const bool saved = saved_write && 3u + offset / DISKIMAGE_SECTOR_PAYLOAD_SIZE == target;
        ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &byte, 1, &eoi, &source), 1);
        ck_assert_uint_eq(byte, (saved ? 'a' : 'A') + record);
        ck_assert_int_eq(eoi, (consumed + 1u) % size == 0);
        ck_assert_uint_eq(state->rel.number, number + consumed / size);
        dos_output_get(&unit, &bank, models[format], source, 1);
        finish_pipeline(&unit, &bank, &image);
        ck_assert(state->ready && !state->failed);
        ck_assert_uint_eq(state->rel.number, number + (consumed + 1u) / size);
        ck_assert_mem_eq(unit.status, positioned.status, sizeof(unit.status));
        ck_assert_uint_eq(unit.status_position, positioned.status_position);
    }
    ck_assert_uint_eq(image.reads, reads + position_reads + continuation_reads);
    ck_assert_uint_eq(image.writes | image.flushes, 0);
    ck_assert_mem_eq(&unit.buffers[state->buffers[2]], &positioned.buffers[state->buffers[2]], sizeof(dos_buffer_t));
    ck_assert(dos_close(&unit, &bank, models[format], 2));
    ck_assert_uint_eq(unit.buffer_use, 0);
}
END_TEST

// Clean positioning stops required jobs, defers unused failures and cancels without I/O.
START_TEST(test_clean_position_buffer_failure_and_cancel) {
    const unsigned int format = (unsigned int)_i / 5u;
    const unsigned int variant = (unsigned int)_i % 5u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_open_image(&bank, &image, format, 3, 1);
    if (variant == 2) {
        image.fail_read = 2;
    }
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, 2);
    dos_rel_channel_prepare_open(&unit, channel, models[format], 1, format == 2 ? 2 : 0, 1, 3, 200);
    finish_pipeline(&unit, &bank, &image);
    dos_channel_t* const state = &unit.channels[channel];
    const unsigned int reads = image.reads;
    const uint16_t use = unit.buffer_use;
    const uint8_t target = variant == 1 || variant == 2 ? 4 : 5;
    if (variant < 2) {
        image.fail_read = reads + 1u;
    }
    ck_assert(dos_rel_channel_position_buffer(&unit, channel, 1, 0, 1, target));
    if (variant >= 3) {
        // Stop after the two rotations and before the cold target READ.
        for (unsigned int step = 0; step < 4u; ++step) {
            dos_work(&unit, &bank);
        }
        ck_assert_uint_eq(image.reads, reads);
        if (variant == 3) {
            mount_open_image(&bank, &replacement, format, 3, 1);
        } else {
            ck_assert(dos_close(&unit, &bank, models[format], 2));
        }
    }
    finish_pipeline(&unit, &bank, &image);
    ck_assert_uint_eq(image.reads, reads + (variant < 2));
    ck_assert_uint_eq(image.writes | image.flushes | replacement.reads | replacement.writes, 0);
    if (variant == 4) {
        ck_assert(!state->allocated);
        ck_assert_uint_eq(unit.buffer_use, 0);
    } else {
        ck_assert(state->allocated);
        ck_assert_uint_eq(unit.buffer_use, use);
        ck_assert_uint_eq(state->rel.number, 1);
        ck_assert_int_eq(state->failed, variant != 1);
        ck_assert_int_eq(state->ready, variant == 1);
        if (variant == 1) {
            ck_assert(state->rel.record.following_failed);
            ck_assert_int_eq(unit.status_code, DOS_STATUS_POWER_ON);
            uint8_t output;
            bool eoi;
            dos_source_t source;
            ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &output, 1, &eoi, &source), 1);
            ck_assert_uint_eq(output, 'B');
        } else {
            ck_assert_int_eq(unit.status_code, DOS_STATUS_READ_ERROR);
            char expected[DOS_STATUS_LINE_CAPACITY];
            snprintf(expected, sizeof(expected),
                     format == 0 ? "23,READ ERROR,%02u,%02u\r" : "23,READ ERROR,%02u,%02u,1\r", variant == 3 ? 0u : 1u,
                     variant == 3 ? 0u : target);
            ck_assert_str_eq(unit.status, expected);
            const dos_unit_t failed = unit;
            ck_assert(!dos_rel_channel_position_buffer(&unit, channel, 1, 0, 1, 3));
            ck_assert_mem_eq(&unit, &failed, sizeof(failed));
        }
    }
    const dos_unit_t completed = unit;
    dos_work(&unit, &bank);
    ck_assert_mem_eq(&unit, &completed, sizeof(completed));
}
END_TEST

// SETLST reports a required failed alternate's header, not the selected data header.
START_TEST(test_clean_position_buffer_required_prefetch_error) {
    const unsigned int format = (unsigned int)_i / 4u;
    const unsigned int variant = (unsigned int)_i % 4u;
    const bool stale = variant == 3;
    const uint8_t target = variant < 3 ? 3u + variant : 3;
    const uint16_t number = variant < 3 ? 2u + variant : 2;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_open_image(&bank, &image, format, 3, 1);
    if (variant == 0 || stale) {
        image.fail_read = 2;
    } else if (variant == 2) {
        image.bytes[5][0] = 1;
        image.bytes[5][1] = 6;
    }
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, 2);
    dos_rel_channel_prepare_open(&unit, channel, models[format], 1, format == 2 ? 2 : 0, 1, 3, 200);
    finish_pipeline(&unit, &bank, &image);
    dos_channel_t* const state = &unit.channels[channel];
    ck_assert(state->ready && !state->failed);
    const unsigned int reads = image.reads;
    const uint16_t use = unit.buffer_use;
    const dos_buffer_t side = unit.buffers[state->buffers[2]];
    if (variant == 1 || variant == 2) {
        image.fail_read = reads + variant;
    }
    ck_assert(dos_rel_channel_position_buffer(&unit, channel, number, 0, 1, target));
    if (stale) {
        // Retire the generation after BHERE but before cached SETLST uses the failed page.
        dos_work(&unit, &bank);
        mount_open_image(&bank, &replacement, format, 3, 1);
    }
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->failed && !state->ready);
    ck_assert(state->allocated);
    ck_assert_uint_eq(state->rel.number, number);
    ck_assert_uint_eq(unit.buffer_use, use);
    ck_assert_mem_eq(&unit.buffers[state->buffers[2]], &side, sizeof(side));
    const dos_buffer_t* const following = &unit.buffers[state->buffers[state->rel.record.active ^ 1u]];
    ck_assert(following->failed);
    ck_assert_uint_eq(following->track, 1);
    ck_assert_uint_eq(following->sector, target + 1u);
    char expected[DOS_STATUS_LINE_CAPACITY];
    snprintf(expected, sizeof(expected), format == 0 ? "23,READ ERROR,%02u,%02u\r" : "23,READ ERROR,%02u,%02u,1\r",
             stale ? 0u : following->track, stale ? 0u : following->sector);
    ck_assert_str_eq(unit.status, expected);
    ck_assert_uint_eq(unit.selected_drive, 1);
    ck_assert_uint_eq(image.reads, reads + (stale ? 0u : variant));
    ck_assert_uint_eq(image.writes | image.flushes | replacement.reads | replacement.writes, 0);
    uint8_t byte;
    bool eoi;
    dos_source_t source;
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &byte, 1, &eoi, &source), 0);
    const dos_unit_t failed = unit;
    dos_work(&unit, &bank);
    ck_assert(!dos_rel_channel_position_buffer(&unit, channel, number, 0, 1, target));
    ck_assert_mem_eq(&unit, &failed, sizeof(failed));
    ck_assert(dos_close(&unit, &bank, models[format], 2));
    ck_assert_uint_eq(unit.buffer_use, 0);
}
END_TEST

// A failed clean positioning owner cannot consume another unit's retained prefix.
START_TEST(test_clean_position_buffer_peer_prefix) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t images[2] = {0};
    dos_media_bank_t bank = {0};
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    uint8_t channels[2];
    for (unsigned int owner = 0; owner < count_of(channels); ++owner) {
        const dos_media_ref_t reference = mount_open_image(&bank, &images[owner], format, owner, owner);
        channels[owner] = dos_rel_channel_claim(&unit, reference, owner + 2u);
        dos_rel_channel_prepare_open(&unit, channels[owner], models[format], 1, format == 2 ? 2 : 0, 1, 3, 200);
    }
    finish_pipeline(&unit, &bank, &images[0]);
    uint8_t output[8];
    bool eoi;
    dos_source_t source;
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 3, output, sizeof(output), &eoi, &source), sizeof(output));
    const dos_channel_t peer = unit.channels[channels[1]];
    images[0].fail_read = images[0].reads + 1u;
    ck_assert(dos_rel_channel_position_buffer(&unit, channels[0], 1, 0, 1, 5));
    finish_pipeline(&unit, &bank, &images[0]);
    ck_assert(unit.channels[channels[0]].failed);
    ck_assert_mem_eq(&unit.channels[channels[1]], &peer, sizeof(peer));
    ck_assert_uint_eq(unit.selected_drive, 0);
    const unsigned int reads = images[1].reads;
    const dos_channel_t failed = unit.channels[channels[0]];
    dos_output_get(&unit, &bank, models[format], source, sizeof(output));
    ck_assert_uint_eq(unit.channels[channels[1]].rel.record.pointer, peer.rel.record.pointer + sizeof(output));
    ck_assert_uint_eq(images[1].reads, reads);
    ck_assert_mem_eq(&unit.channels[channels[0]], &failed, sizeof(failed));
    ck_assert_int_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_uint_eq(images[0].writes | images[1].writes | images[0].flushes | images[1].flushes, 0);
}
END_TEST

// Prepare a clean owner with a real side/data window, without write-only setup.
static uint8_t prepare_clean_index_channel(dos_unit_t* const unit, dos_media_bank_t* const bank,
                                           rel_image_t* const image, const unsigned int format,
                                           const dos_media_ref_t reference, const uint8_t secondary) {
    const uint8_t channel = dos_rel_channel_claim(unit, reference, secondary);
    ck_assert_uint_ne(channel, DOS_UNUSED);
    dos_rel_channel_prepare(unit, channel, models[format], 1, format == 2 ? 2 : 0, 1, 200, 0);
    finish_pipeline(unit, bank, image);
    ck_assert(unit->channels[channel].ready && !unit->channels[channel].failed);
    return channel;
}

// Resident SSTEST precedes range checks and SSPOS's second test branches on N only.
START_TEST(test_clean_position_resident_index) {
    const unsigned int format = (unsigned int)_i / 9u;
    const unsigned int variant = (unsigned int)_i % 9u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_open_image(&bank, &image, format, 3, 1);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_clean_index_channel(&unit, &bank, &image, format, reference, 2);
    dos_channel_t* const state = &unit.channels[channel];
    dos_buffer_t* const side = &unit.buffers[state->buffers[2]];
    image.bytes[0][4] = 1;
    image.bytes[0][5] = 0;
    image.bytes[0][1] = UINT8_MAX;
    image.bytes[1][1] = UINT8_MAX;
    image.bytes[1][2] = variant == 2 || variant == 8 ? 0 : 1;
    image.bytes[1][6] = 1;
    image.bytes[1][7] = 1;
    image.bytes[1][16] = variant == 8 ? 0 : 1;
    image.bytes[1][17] = 4;
    uint16_t number = variant == 0 ? 0 : 1;
    if (variant == 1 || variant == 2 || variant == 8) {
        number = 154;
        image.bytes[0][6] = 1;
        image.bytes[0][7] = 1;
    } else if (variant >= 3) {
        image.bytes[0][16] = 0;
        if (variant == 4) {
            number = 916;
        } else if (variant == 5) {
            memset(image.bytes[0] + DOS_REL_SIDE_TABLE_OFFSET, 0, DOS_REL_SIDE_COUNT * 2u);
        } else if (variant == 6) {
            image.bytes[0][6] = 1;
            image.bytes[0][7] = 1;
        } else if (variant == 7) {
            image.bytes[0][0] = 1;
        }
    }
    memcpy(side->bytes, image.bytes[0], sizeof(side->bytes));
    const dos_rel_record_t before = state->rel.record;
    state->rel.get_active = true;
    state->rel.no_record = true;
    dos_status(&unit, models[format], DOS_STATUS_FILE_EXISTS, 0, 0);
    dos_status_get(&unit, models[format], 2);
    const unsigned int reads = image.reads;
    ck_assert(dos_rel_channel_position(&unit, channel, number, 0));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert(!state->ready && !state->failed && dos_work_pending(&unit));
    ck_assert(!state->rel.no_record);
    ck_assert_int_eq(state->rel.get_active, format != 0);
    const dos_unit_t pending = unit;
    ck_assert(!dos_rel_channel_position(&unit, channel, 2, 1));
    ck_assert_mem_eq(&unit, &pending, sizeof(pending));
    finish_pipeline(&unit, &bank, &image);
    ck_assert_uint_eq(state->rel.number, number);
    ck_assert_uint_eq(state->directory, DOS_DIRECTORY_NONE);
    ck_assert_uint_eq(image.writes | image.flushes, 0);
    const unsigned int extra_reads = variant == 1 || variant == 2 ? 2 : variant == 6 || variant == 8 ? 1 : 0;
    ck_assert_uint_eq(image.reads, reads + extra_reads + (format == 2 && variant >= 3 && variant < 8 ? 2 : 0));
    if (variant == 5 || variant == 7 || variant == 8) {
        ck_assert(state->failed && !state->ready);
        ck_assert_str_eq(unit.status, variant == 8  ? format == 0 ? "23,READ ERROR,00,04\r" : "23,READ ERROR,00,04,1\r"
                                      : format == 0 ? "67,ILLEGAL TRACK OR SECTOR,00,00\r"
                                                    : "67,ILLEGAL TRACK OR SECTOR,00,00,1\r");
        ck_assert_uint_eq(state->rel.record.pointer, before.pointer);
        return;
    }
    ck_assert(state->ready && !state->failed);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_FILE_EXISTS);
    ck_assert_uint_eq(unit.status_position, 2);
    uint8_t output = 0;
    bool eoi = false;
    ck_assert_uint_eq(dos_stage(&unit, &bank, channel, &output, 1, &eoi), 1);
    if (variant < 3) {
        ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_READY);
        ck_assert_uint_eq(output, variant == 0 ? 'A' : 'B');
        ck_assert_uint_eq(state->rel.record.pointer, variant == 0 ? 2 : 122);
        ck_assert_uint_eq(side->bytes[DOS_REL_SIDE_NUMBER_OFFSET], variant == 1 ? 1 : 0);
    } else {
        ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_MISSING);
        ck_assert_uint_eq(state->rel.record.pointer, before.pointer);
        ck_assert_uint_eq(state->rel.record.next, before.next);
        ck_assert_uint_eq(state->rel.record.lookup.address.index, 254);
        ck_assert_uint_eq(state->rel.record.lookup.address.side, variant == 6 ? 1 : 0);
        ck_assert_uint_eq(output, '\r');
        ck_assert(eoi);
        ck_assert_uint_eq(unit.buffers[state->buffers[0]].bytes[2], 'A');
        dos_get(&unit, &bank, channel, 1);
        ck_assert_str_eq(unit.status,
                         format == 0 ? "50, RECORD NOT PRESENT,00,00\r" : "50, RECORD NOT PRESENT,00,00,1\r");
        ck_assert_uint_eq(state->rel.number, number);
        ck_assert(state->rel.no_record);
    }
}
END_TEST

// Failed side/super/last-side waits retain pages, while remount/CLOSE stop all reads.
START_TEST(test_clean_position_index_failure_and_cancel) {
    const unsigned int format = (unsigned int)_i / 4u;
    const unsigned int variant = (unsigned int)_i % 4u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_open_image(&bank, &image, format, 3, 1);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_clean_index_channel(&unit, &bank, &image, format, reference, 2);
    dos_channel_t* const state = &unit.channels[channel];
    image.bytes[0][6] = 1;
    image.bytes[0][7] = 1;
    image.bytes[1][1] = UINT8_MAX;
    image.bytes[1][2] = 1;
    image.bytes[1][16] = 1;
    image.bytes[1][17] = 4;
    memcpy(unit.buffers[state->buffers[2]].bytes, image.bytes[0], DISKIMAGE_SECTOR_SIZE);
    if (variant == 1 && format == 2) {
        state->rel.record.cached_group = DOS_REL_GROUP_NONE;
    }
    ck_assert(dos_rel_channel_position(&unit, channel, variant == 1 && format != 2 ? 916 : 154, 0));
    const unsigned int reads = image.reads;
    const dos_rel_record_t before = state->rel.record;
    const uint16_t use = unit.buffer_use;
    if (variant == 2) {
        mount_open_image(&bank, &replacement, format, 3, 1);
    } else if (variant == 3) {
        ck_assert(dos_close(&unit, &bank, models[format], 2));
    } else {
        image.fail_read = reads + 1u;
    }
    finish_pipeline(&unit, &bank, &image);
    ck_assert_uint_eq(image.reads, reads + (variant < 2));
    ck_assert_uint_eq(replacement.reads | replacement.writes | image.writes | image.flushes, 0);
    if (variant == 3) {
        ck_assert(!state->allocated);
        ck_assert_uint_eq(unit.buffer_use, 0);
        return;
    }
    ck_assert(state->allocated && state->failed && !state->ready);
    ck_assert_uint_eq(unit.buffer_use, use);
    ck_assert_uint_eq(state->rel.record.pointer, before.pointer);
    ck_assert_uint_eq(state->rel.record.next, before.next);
    ck_assert_str_eq(unit.status, variant == 2  ? format == 0 ? "23,READ ERROR,00,00\r" : "23,READ ERROR,00,00,1\r"
                                  : format == 0 ? "23,READ ERROR,01,01\r"
                                  : variant == 1 && format == 2 ? "23,READ ERROR,01,02,1\r"
                                                                : "23,READ ERROR,01,01,1\r");
    const dos_unit_t failed = unit;
    dos_work(&unit, &bank);
    ck_assert(!dos_rel_channel_position(&unit, channel, 1, 0));
    ck_assert_mem_eq(&unit, &failed, sizeof(failed));
}
END_TEST

// RDLG directly reuses group 90, but scans a super table after other cached groups.
START_TEST(test_clean_position_last_group_cache) {
    const unsigned int variant = (unsigned int)_i;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_open_image(&bank, &image, 2, 3, 1);
    dos_unit_t unit;
    dos_reset(&unit, DOS_MODEL_8250);
    const uint8_t channel = prepare_clean_index_channel(&unit, &bank, &image, 2, reference, 2);
    dos_channel_t* const state = &unit.channels[channel];
    dos_buffer_t* const side = &unit.buffers[state->buffers[2]];
    image.bytes[0][1] = UINT8_MAX;
    image.bytes[0][4] = 1;
    image.bytes[0][16] = 0;
    image.bytes[2][DOS_REL_GROUP_TABLE_OFFSET + 2u * DOS_REL_END_GROUP_START] = 1;
    image.bytes[2][DOS_REL_GROUP_TABLE_OFFSET + 2u * DOS_REL_END_GROUP_START + 1u] = 0;
    state->rel.record.cached_group = variant == 0   ? DOS_REL_END_GROUP_START
                                     : variant == 1 ? DOS_REL_MAX_GROUP
                                     : variant == 2 ? DOS_REL_GROUP_SUPER
                                                    : DOS_REL_GROUP_NONE;
    memcpy(side->bytes, image.bytes[variant == 2 ? 2 : 0], sizeof(side->bytes));
    if (variant == 1) {
        side->bytes[DOS_REL_SIDE_NUMBER_OFFSET] = 2;
        side->bytes[82] = 0;
    }
    state->rel.record.size = UINT8_MAX;
    const dos_rel_record_t before = state->rel.record;
    const unsigned int reads = image.reads;
    ck_assert(dos_rel_channel_position(&unit, channel, variant == 1 ? UINT16_MAX : 64547, 0));
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->ready && !state->failed);
    ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_MISSING);
    ck_assert_uint_eq(state->directory, DOS_DIRECTORY_NONE);
    ck_assert_uint_eq(state->rel.record.pointer, before.pointer);
    ck_assert_uint_eq(state->rel.record.next, before.next);
    ck_assert_uint_eq(state->rel.record.cached_group, DOS_REL_END_GROUP_START);
    ck_assert_uint_eq(state->rel.record.lookup.address.group, DOS_REL_END_GROUP_START);
    ck_assert_uint_eq(state->rel.record.lookup.address.side, 0);
    ck_assert_uint_eq(state->rel.record.lookup.address.index, 254);
    ck_assert_uint_eq(image.reads, reads + (variant == 0 ? 0 : variant == 2 ? 1 : 2));
    ck_assert_uint_eq(image.writes | image.flushes, 0);
}
END_TEST

// Even wrapped high side numbers accept equal headers before the six-side limit.
START_TEST(test_clean_position_wrapped_resident_header) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_open_image(&bank, &image, format, 3, 1);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_clean_index_channel(&unit, &bank, &image, format, reference, 2);
    dos_channel_t* const state = &unit.channels[channel];
    state->rel.record.size = UINT8_MAX;
    const dos_rel_address_t address = dos_rel_address(models[format], UINT16_MAX, UINT8_MAX, 0);
    dos_buffer_t* const side = &unit.buffers[state->buffers[2]];
    memset(side->bytes, 0, sizeof(side->bytes));
    side->bytes[DOS_REL_SIDE_NUMBER_OFFSET] = address.side;
    side->bytes[address.index] = 1;
    side->bytes[address.index + 1u] = 3;
    state->rel.record.cached_group = address.group;
    const unsigned int reads = image.reads;
    ck_assert(dos_rel_channel_position(&unit, channel, UINT16_MAX, 0));
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->ready && !state->failed);
    ck_assert_uint_eq(state->directory, DOS_DIRECTORY_NONE);
    ck_assert_uint_eq(state->rel.record.pointer, address.offset);
    ck_assert_uint_eq(state->rel.number, UINT16_MAX);
    ck_assert_uint_eq(image.reads, reads);
    uint8_t output;
    bool eoi;
    ck_assert_uint_eq(dos_stage(&unit, &bank, channel, &output, 1, &eoi), 1);
    ck_assert_uint_eq(output, 'A');
    ck_assert_uint_eq(image.writes | image.flushes, 0);
}
END_TEST

// OPEN leaves the big index uncached, repeated positioning reuses it, CLOSE retires it.
START_TEST(test_clean_position_open_cache_lifetime) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_open_image(&bank, &image, format, 3, 1);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    uint8_t channel = DOS_UNUSED;
    for (unsigned int lifetime = 0; lifetime < 2u; ++lifetime) {
        const uint8_t reclaimed = dos_rel_channel_claim(&unit, reference, 2);
        ck_assert_uint_ne(reclaimed, DOS_UNUSED);
        if (lifetime != 0) {
            ck_assert_uint_eq(reclaimed, channel);
        }
        channel = reclaimed;
        dos_rel_channel_prepare_open(&unit, channel, models[format], 1, format == 2 ? 2 : 0, 1, 3, 200);
        finish_pipeline(&unit, &bank, &image);
        for (unsigned int request = 0; request < 2u; ++request) {
            const unsigned int reads = image.reads;
            ck_assert(dos_rel_channel_position(&unit, channel, 1, 0));
            finish_pipeline(&unit, &bank, &image);
            ck_assert_uint_eq(image.reads, reads + (format == 2 && request == 0 ? 2 : 0));
            uint8_t output;
            bool eoi;
            ck_assert_uint_eq(dos_stage(&unit, &bank, channel, &output, 1, &eoi), 1);
            ck_assert_uint_eq(output, 'A');
        }
        ck_assert(dos_close(&unit, &bank, models[format], 2));
        ck_assert_uint_eq(unit.buffer_use, 0);
    }
    ck_assert_uint_eq(image.writes | image.flushes, 0);
}
END_TEST

// A failed index owner must not replace another unit's retained output prefix.
START_TEST(test_clean_position_index_peer_prefix) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t images[2] = {0};
    dos_media_bank_t bank = {0};
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    uint8_t channels[2];
    for (unsigned int owner = 0; owner < count_of(channels); ++owner) {
        const dos_media_ref_t reference = mount_open_image(&bank, &images[owner], format, owner, owner);
        channels[owner] = prepare_clean_index_channel(&unit, &bank, &images[owner], format, reference, owner + 2u);
    }
    dos_buffer_t* const side = &unit.buffers[unit.channels[channels[0]].buffers[2]];
    side->bytes[6] = 1;
    side->bytes[7] = 1;
    uint8_t output[8];
    bool eoi;
    dos_source_t source;
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 3, output, sizeof(output), &eoi, &source), sizeof(output));
    const dos_channel_t peer = unit.channels[channels[1]];
    images[0].fail_read = images[0].reads + 1u;
    ck_assert(dos_rel_channel_position(&unit, channels[0], 154, 0));
    finish_pipeline(&unit, &bank, &images[0]);
    ck_assert(unit.channels[channels[0]].failed);
    ck_assert_mem_eq(&unit.channels[channels[1]], &peer, sizeof(peer));
    const unsigned int reads = images[1].reads;
    const dos_channel_t failed = unit.channels[channels[0]];
    dos_output_get(&unit, &bank, models[format], source, sizeof(output));
    ck_assert_uint_eq(unit.channels[channels[1]].rel.record.pointer, peer.rel.record.pointer + sizeof(output));
    ck_assert_uint_eq(images[1].reads, reads);
    ck_assert_mem_eq(&unit.channels[channels[0]], &failed, sizeof(failed));
    ck_assert_int_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_uint_eq(images[0].writes | images[1].writes | images[0].flushes | images[1].flushes, 0);
}
END_TEST

// Failed group replacement invalidates its tag, empty super-table underflow stays unsupported.
START_TEST(test_clean_position_group_failure_and_empty_table) {
    const unsigned int variant = (unsigned int)_i;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_open_image(&bank, &image, 2, 3, 1);
    dos_unit_t unit;
    dos_reset(&unit, DOS_MODEL_8250);
    const uint8_t channel = prepare_clean_index_channel(&unit, &bank, &image, 2, reference, 2);
    dos_channel_t* const state = &unit.channels[channel];
    dos_buffer_t* const side = &unit.buffers[state->buffers[2]];
    if (variant == 2) {
        side->bytes[DOS_REL_POINTER_OFFSET] = 0;
    } else {
        state->rel.record.cached_group = DOS_REL_GROUP_SUPER;
        memcpy(side->bytes, image.bytes[2], sizeof(side->bytes));
    }
    if (variant == 3) {
        memset(side->bytes, 0, sizeof(side->bytes));
    }
    state->rel.record.size = UINT8_MAX;
    const unsigned int reads = image.reads;
    image.fail_read = reads + 1u;
    ck_assert(dos_rel_channel_position(&unit, channel, variant == 1 || variant == 3 ? 64547 : 1, 0));
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->failed && !state->ready);
    ck_assert_uint_eq(state->directory, DOS_DIRECTORY_NONE);
    ck_assert_uint_eq(image.reads, reads + (variant != 3));
    ck_assert_uint_eq(image.writes | image.flushes, 0);
    if (variant == 3) {
        ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_NO_INDEX);
        ck_assert_int_eq(unit.status_code, DOS_STATUS_SYNTAX);
        uint8_t output = 0xee;
        bool eoi = false;
        ck_assert_uint_eq(dos_stage(&unit, &bank, channel, &output, 1, &eoi), 0);
        ck_assert_uint_eq(output, 0xee);
    } else {
        ck_assert_uint_eq(state->rel.record.cached_group, DOS_REL_GROUP_NONE);
        ck_assert_str_eq(unit.status, variant == 2 ? "23,READ ERROR,01,02,1\r" : "23,READ ERROR,01,00,1\r");
        ck_assert(side->failed && !side->resident);
    }
}
END_TEST

// SSEND's DEY endpoint wraps byte zero and one without moving the data cursor.
START_TEST(test_clean_position_terminal_endpoint_wrap) {
    const unsigned int format = (unsigned int)_i / 2u;
    const uint8_t endpoint = (unsigned int)_i % 2u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_open_image(&bank, &image, format, 3, 1);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_clean_index_channel(&unit, &bank, &image, format, reference, 2);
    dos_channel_t* const state = &unit.channels[channel];
    image.bytes[0][1] = endpoint;
    image.bytes[0][DOS_REL_SIDE_TABLE_OFFSET] = 1;
    image.bytes[0][DOS_REL_POINTER_OFFSET] = 0;
    memcpy(unit.buffers[state->buffers[2]].bytes, image.bytes[0], DISKIMAGE_SECTOR_SIZE);
    const dos_rel_record_t before = state->rel.record;
    ck_assert(dos_rel_channel_position(&unit, channel, 1, 0));
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->ready && !state->failed);
    ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_MISSING);
    ck_assert_uint_eq(state->rel.record.lookup.address.index, endpoint == 0 ? UINT8_MAX : 0);
    ck_assert_uint_eq(state->rel.record.pointer, before.pointer);
    ck_assert_uint_eq(state->rel.record.next, before.next);
    uint8_t output;
    bool eoi;
    ck_assert_uint_eq(dos_stage(&unit, &bank, channel, &output, 1, &eoi), 1);
    ck_assert_uint_eq(output, '\r');
    ck_assert(eoi);
    ck_assert_uint_eq(image.writes | image.flushes, 0);
}
END_TEST

// In-page writes use the current pointer, preserve read flags and pad only the record suffix.
START_TEST(test_record_put_and_pad_borrowed_pages) {
    const unsigned int format = (unsigned int)_i / 4u;
    const unsigned int active = (unsigned int)_i % 2u;
    const bool after_get = (unsigned int)_i % 4u >= 2u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 1, 10, 3, false);
    memset(image.bytes[3] + 2, 'A', DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    dos_unit_t pool = {0};
    uint8_t* const current = pool.buffers[5].bytes;
    uint8_t* const other = pool.buffers[2].bytes;
    memset(other, 0x99, DISKIMAGE_SECTOR_SIZE);
    dos_rel_reader_t reader;
    dos_rel_reader_start(&reader, models[format], reference, 1, format == 2 ? 2 : 0, 1, 10, 3);
    ck_assert_int_eq(prepare_record(&reader.record, &bank, &image, pool.buffers[8].bytes, current, other),
                     DOS_REL_RECORD_READY);
    reader.record.active = active;
    uint8_t* const first = active == 0 ? current : other;
    uint8_t* const following = active == 0 ? other : current;
    if (after_get) {
        ck_assert_int_eq(dos_rel_reader_get(&reader, models[format], &bank, first, following), DOS_REL_RECORD_READY);
    }
    const uint8_t pointer = after_get ? 5 : 4;
    ck_assert_uint_eq(reader.record.pointer, pointer);
    const bool get_active = reader.get_active;
    uint8_t expected[DISKIMAGE_SECTOR_SIZE];
    memcpy(expected, current, sizeof(expected));
    expected[pointer] = 'Z';
    const unsigned int reads = image.reads;
    uint8_t dirty = active == 0 ? DOS_REL_DIRTY_FOLLOWING : DOS_REL_DIRTY_FIRST;
    ck_assert_int_eq(dos_rel_record_put(&reader.record, &bank, first, following, 'Z', &dirty), DOS_REL_RECORD_WRITING);
    ck_assert_uint_eq(dirty, DOS_REL_DIRTY_FIRST | DOS_REL_DIRTY_FOLLOWING);
    ck_assert_uint_eq(reader.record.pointer, pointer + 1u);
    ck_assert_mem_eq(current, expected, sizeof(expected));
    const dos_rel_record_t writing = reader.record;
    uint8_t output = 0xee;
    bool eoi = true;
    ck_assert(!dos_rel_record_peek(&reader.record, &bank, first, following, &output, &eoi));
    ck_assert_uint_eq(output, 0xee);
    ck_assert(eoi);
    ck_assert_int_eq(dos_rel_record_get(&reader.record, &bank), DOS_REL_RECORD_WRITING);
    ck_assert_int_eq(dos_rel_record_next(&reader.record, &bank, first, following), DOS_REL_RECORD_WRITING);
    ck_assert_mem_eq(&reader.record, &writing, sizeof(writing));
    ck_assert_int_eq(dos_rel_record_pad(&reader.record, &bank, first, following, &dirty), DOS_REL_RECORD_FULL);
    memset(expected + pointer + 1u, 0, 11u - pointer);
    ck_assert_mem_eq(current, expected, sizeof(expected));
    ck_assert_uint_eq(reader.record.pointer, 11);
    ck_assert_uint_eq(reader.record.next, 12);
    ck_assert_int_eq(reader.get_active, get_active);
    ck_assert_int_eq(get_active, after_get && format == 0);
    ck_assert_uint_eq(reader.number, 1);
    for (unsigned int byte = 0; byte < DISKIMAGE_SECTOR_SIZE; ++byte) {
        ck_assert_uint_eq(other[byte], 0x99);
    }
    ck_assert_mem_eq(image.bytes[3] + 2, "AAAAAAAAAA", 10);
    ck_assert_uint_eq(image.reads, reads);
}
END_TEST

// Native NR=0/2 fill in place, while other wraps retain effects before NRBUF.
START_TEST(test_record_put_native_wrap_and_deferred_prefetch) {
    static const struct {
        uint16_t number;
        uint8_t size, position, next;
        bool following, failed_prefetch, writeback;
    } cases[] = {
        {2, 127, 127, 0, false, false, false}, {2, 127, 127, 2, true, false, false},
        {2, 200, 54, 148, true, false, true},  {1, 255, 253, 3, true, false, true},
        {2, 127, 127, 2, true, true, false},
    };
    const unsigned int format = (unsigned int)_i / count_of(cases);
    const unsigned int which = (unsigned int)_i % count_of(cases);
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, cases[which].number, cases[which].size,
                                                   cases[which].position, cases[which].following);
    memset(image.bytes[3] + 2, 'A', DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    memset(image.bytes[4] + 2, 'B', DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    image.fail_read = cases[which].failed_prefetch ? 3u + (format == 2) : 0;
    dos_rel_record_t record;
    dos_rel_record_start(&record, models[format], reference, 1, format == 2 ? 2 : 0, cases[which].number,
                         cases[which].size, cases[which].position);
    uint8_t side[DISKIMAGE_SECTOR_SIZE], first[DISKIMAGE_SECTOR_SIZE], following[DISKIMAGE_SECTOR_SIZE] = {0};
    ck_assert_int_eq(prepare_record(&record, &bank, &image, side, first, following), DOS_REL_RECORD_READY);
    ck_assert_uint_eq(record.pointer, which == 3 ? 254 : 255);
    ck_assert_uint_eq(record.next, cases[which].next);
    ck_assert_int_eq(record.following_failed, cases[which].failed_prefetch);
    uint8_t saved[DISKIMAGE_SECTOR_SIZE];
    memcpy(saved, following, sizeof(saved));
    const unsigned int reads = image.reads;
    uint8_t dirty = 0;
    if (which == 3) {
        ck_assert_int_eq(dos_rel_record_put(&record, &bank, first, following, 'Y', &dirty), DOS_REL_RECORD_WRITING);
        ck_assert_uint_eq(first[254], 'Y');
    }
    const dos_rel_record_result_t result =
        cases[which].writeback ? DOS_REL_RECORD_NEEDS_WRITEBACK : DOS_REL_RECORD_FULL;
    ck_assert_int_eq(dos_rel_record_put(&record, &bank, first, following, 'Z', &dirty), result);
    ck_assert_uint_eq(first[255], 'Z');
    ck_assert_uint_eq(record.pointer, cases[which].writeback ? 0 : 255);
    ck_assert_uint_eq(record.active, 0);
    ck_assert_uint_eq(dirty, DOS_REL_DIRTY_FIRST);
    const dos_rel_record_t before = record;
    for (unsigned int repeat = 0; repeat < 2; ++repeat) {
        ck_assert_int_eq(dos_rel_record_put(&record, &bank, first, following, 'X', &dirty), result);
        ck_assert_int_eq(dos_rel_record_pad(&record, &bank, first, following, &dirty), result);
        ck_assert_mem_eq(&record, &before, sizeof(before));
        ck_assert_uint_eq(first[255], 'Z');
        ck_assert_uint_eq(dirty, DOS_REL_DIRTY_FIRST);
    }
    ck_assert_mem_eq(following, saved, sizeof(saved));
    ck_assert_uint_eq(image.reads, reads);
}
END_TEST

// Missing growth, pending positioning and failed reads never accept cached writes.
START_TEST(test_record_put_requires_prepared_existing_record) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int variant = (unsigned int)_i % 3u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 2, 200, 54, false);
    image.fail_read = variant == 2 ? 1 : 0;
    dos_rel_record_t record;
    dos_rel_record_start(&record, models[format], reference, 1, format == 2 ? 2 : 0, 2, 200, 54);
    uint8_t side[DISKIMAGE_SECTOR_SIZE] = {0}, first[DISKIMAGE_SECTOR_SIZE] = {0};
    uint8_t following[DISKIMAGE_SECTOR_SIZE] = {0};
    const dos_rel_record_result_t result = variant == 0   ? DOS_REL_RECORD_MISSING
                                           : variant == 1 ? DOS_REL_RECORD_PENDING
                                                          : DOS_REL_RECORD_FAILED;
    if (variant != 1) {
        ck_assert_int_eq(prepare_record(&record, &bank, &image, side, first, following), result);
    }
    const dos_rel_record_t before = record;
    uint8_t saved[2][DISKIMAGE_SECTOR_SIZE];
    memcpy(saved[0], first, sizeof(first));
    memcpy(saved[1], following, sizeof(following));
    const unsigned int reads = image.reads;
    uint8_t dirty = 0;
    ck_assert_int_eq(dos_rel_record_put(&record, &bank, first, following, 'Z', &dirty), result);
    ck_assert_int_eq(dos_rel_record_pad(&record, &bank, first, following, &dirty), result);
    ck_assert_mem_eq(&record, &before, sizeof(before));
    ck_assert_mem_eq(first, saved[0], sizeof(first));
    ck_assert_mem_eq(following, saved[1], sizeof(following));
    ck_assert_uint_eq(dirty, 0);
    ck_assert_uint_eq(image.reads, reads);
}
END_TEST

// Replacement/removal rejects new stores without losing already-dirty cached bytes.
START_TEST(test_record_put_generation_retains_dirty_pages) {
    const unsigned int format = (unsigned int)_i / 4u;
    const unsigned int variant = (unsigned int)_i % 4u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 1, 10, 0, false);
    image.bytes[3][2] = 'A';
    dos_rel_record_t record;
    dos_rel_record_start(&record, models[format], reference, 1, format == 2 ? 2 : 0, 1, 10, 0);
    uint8_t side[DISKIMAGE_SECTOR_SIZE], first[DISKIMAGE_SECTOR_SIZE], following[DISKIMAGE_SECTOR_SIZE] = {0};
    ck_assert_int_eq(prepare_record(&record, &bank, &image, side, first, following), DOS_REL_RECORD_READY);
    uint8_t dirty = DOS_REL_DIRTY_FOLLOWING;
    if (variant >= 2) {
        ck_assert_int_eq(dos_rel_record_put(&record, &bank, first, following, 'Z', &dirty), DOS_REL_RECORD_WRITING);
    }
    if (variant & 1u) {
        mount_index(&bank, &replacement, format, 0, 0);
    } else {
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, NULL), DOS_MEDIA_OK);
    }
    uint8_t saved[DISKIMAGE_SECTOR_SIZE];
    memcpy(saved, first, sizeof(saved));
    const uint8_t pointer = record.pointer, prior_dirty = dirty;
    const unsigned int reads = image.reads;
    if (variant & 1u) {
        ck_assert_int_eq(dos_rel_record_pad(&record, &bank, first, following, &dirty), DOS_REL_RECORD_FAILED);
    } else {
        ck_assert_int_eq(dos_rel_record_put(&record, &bank, first, following, 'X', &dirty), DOS_REL_RECORD_FAILED);
    }
    ck_assert_int_eq(record.lookup.media_result, DOS_MEDIA_STALE);
    ck_assert_uint_eq(record.pointer, pointer);
    ck_assert_uint_eq(dirty, prior_dirty);
    ck_assert_mem_eq(first, saved, sizeof(saved));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(replacement.reads, 0);
}
END_TEST

// WR40/RD40 may reuse only a completed window, never schedule clean I/O over dirty pages.
START_TEST(test_record_finish_write_cached_boundaries) {
    static const struct {
        uint16_t number;
        uint8_t size, position;
        bool linked, resident, complete;
        uint8_t pointer, next, last;
    } cases[] = {
        {1, 10, 0, false, false, true, 12, 22, 21},     {1, 254, 0, false, false, true, 255, 0, 255},
        {2, 127, 127, true, false, false, 255, 2, 255}, {1, 10, 0, true, false, true, 12, 22, 21},
        {2, 125, 0, true, false, false, 251, 252, 251}, {2, 125, 0, true, true, true, 252, 123, 122},
    };
    const unsigned int format = (unsigned int)_i / (2u * count_of(cases));
    const unsigned int which = (unsigned int)_i / 2u % count_of(cases);
    const unsigned int active = (unsigned int)_i % 2u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, cases[which].number, cases[which].size,
                                                   cases[which].position, cases[which].linked);
    memset(image.bytes[3] + DISKIMAGE_SECTOR_LINK_SIZE, 'A', DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    memset(image.bytes[4] + DISKIMAGE_SECTOR_LINK_SIZE, 'B', DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    dos_rel_record_t record;
    dos_rel_record_start(&record, models[format], reference, 1, format == 2 ? 2 : 0, cases[which].number,
                         cases[which].size, cases[which].position);
    uint8_t side[DISKIMAGE_SECTOR_SIZE], current[DISKIMAGE_SECTOR_SIZE], other[DISKIMAGE_SECTOR_SIZE] = {0};
    ck_assert_int_eq(prepare_record(&record, &bank, &image, side, current, other), DOS_REL_RECORD_READY);
    record.active = active;
    record.following = cases[which].resident;
    uint8_t* const first = active == 0 ? current : other;
    uint8_t* const following = active == 0 ? other : current;
    uint8_t dirty = 0;
    ck_assert_int_eq(dos_rel_record_put(&record, &bank, first, following, 'Z', &dirty),
                     which == 2 ? DOS_REL_RECORD_FULL : DOS_REL_RECORD_WRITING);
    ck_assert_int_eq(dos_rel_record_pad(&record, &bank, first, following, &dirty), DOS_REL_RECORD_FULL);
    const dos_rel_record_t before = record;
    uint8_t saved[2][DISKIMAGE_SECTOR_SIZE];
    memcpy(saved[0], current, sizeof(current));
    memcpy(saved[1], other, sizeof(other));
    const unsigned int reads = image.reads;
    ck_assert_int_eq(dos_rel_record_finish_write(&record, &bank, first, following), cases[which].complete);
    ck_assert_uint_eq(record.active, active);
    ck_assert_uint_eq(record.pointer, cases[which].pointer);
    ck_assert_uint_eq(record.next, cases[which].next);
    ck_assert_uint_eq(record.last, cases[which].last);
    if (!cases[which].complete) {
        ck_assert_mem_eq(&record, &before, sizeof(before));
    } else {
        ck_assert_int_eq(record.result, which == 1 ? DOS_REL_RECORD_MISSING : DOS_REL_RECORD_READY);
        uint8_t byte = 0;
        bool eoi = false;
        ck_assert(dos_rel_record_peek(&record, &bank, first, following, &byte, &eoi));
        ck_assert_uint_eq(byte, which == 1 ? '\r' : 'A');
        ck_assert_int_eq(eoi, which == 1);
        const dos_rel_record_t completed = record;
        ck_assert(!dos_rel_record_finish_write(&record, &bank, first, following));
        ck_assert_mem_eq(&record, &completed, sizeof(completed));
    }
    ck_assert_uint_eq(dirty, active == 0 ? DOS_REL_DIRTY_FIRST : DOS_REL_DIRTY_FOLLOWING);
    ck_assert_mem_eq(current, saved[0], sizeof(current));
    ck_assert_mem_eq(other, saved[1], sizeof(other));
    ck_assert_uint_eq(image.reads, reads);
}
END_TEST

// SETLST may scan a completed inactive page without authorizing GET to follow it.
START_TEST(test_record_prepare_cached_completed_window) {
    const unsigned int format = (unsigned int)_i / 6u;
    const unsigned int variant = (unsigned int)_i % 6u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 1, 255, 253, true);
    memset(image.bytes[3] + DISKIMAGE_SECTOR_LINK_SIZE, 'A', DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    memset(image.bytes[4] + DISKIMAGE_SECTOR_LINK_SIZE, 'B', DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    dos_rel_record_t record;
    dos_rel_record_start(&record, models[format], reference, 1, format == 2 ? 2 : 0, 1, 255, 253);
    uint8_t side[DISKIMAGE_SECTOR_SIZE], first[DISKIMAGE_SECTOR_SIZE], following[DISKIMAGE_SECTOR_SIZE];
    ck_assert_int_eq(prepare_record(&record, &bank, &image, side, first, following), DOS_REL_RECORD_READY);

    // Model the owner's completed NRBUF window, not a proven read-ahead target.
    record.result = variant == 1 ? DOS_REL_RECORD_READY : DOS_REL_RECORD_PENDING;
    record.phase = variant == 2 ? DOS_REL_RECORD_INDEX : DOS_REL_RECORD_FOLLOWING;
    record.following = false;
    record.following_failed = variant == 4;
    if (variant == 3) {
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, NULL), DOS_MEDIA_OK);
    } else if (variant == 5) {
        first[0] = 0;
        record.pointer = DISKIMAGE_SECTOR_LINK_SIZE;
        record.position = 0;
    }
    const dos_rel_record_t before = record;
    uint8_t saved[2][DISKIMAGE_SECTOR_SIZE];
    memcpy(saved[0], first, sizeof(first));
    memcpy(saved[1], following, sizeof(following));
    const unsigned int reads = image.reads;
    ck_assert_int_eq(dos_rel_record_prepare_cached(&record, &bank, first, following), variant == 0 || variant == 5);
    ck_assert(!record.following);
    if (variant == 0) {
        ck_assert_int_eq(record.result, DOS_REL_RECORD_READY);
        ck_assert_uint_eq(record.pointer, 254);
        ck_assert_uint_eq(record.next, 3);
        ck_assert_uint_eq(record.last, 2);
        ck_assert_int_eq(dos_rel_record_get(&record, &bank), DOS_REL_RECORD_READY);
        ck_assert_int_eq(dos_rel_record_get(&record, &bank), DOS_REL_RECORD_NEEDS_LINK);
    } else if (variant == 1 || variant == 2) {
        ck_assert_mem_eq(&record, &before, sizeof(before));
    } else if (variant == 5) {
        ck_assert_int_eq(record.result, DOS_REL_RECORD_MISSING);
        uint8_t byte = 0;
        bool eoi = false;
        ck_assert(dos_rel_record_peek(&record, &bank, first, following, &byte, &eoi));
        ck_assert_uint_eq(byte, '\r');
        ck_assert(eoi);
    } else {
        ck_assert_int_eq(record.result, DOS_REL_RECORD_FAILED);
        if (variant == 3) {
            ck_assert_int_eq(record.lookup.media_result, DOS_MEDIA_STALE);
        }
    }
    ck_assert_mem_eq(first, saved[0], sizeof(first));
    ck_assert_mem_eq(following, saved[1], sizeof(following));
    ck_assert_uint_eq(image.reads, reads);
}
END_TEST

// Both cached completion entries retire stale cursors without losing dirty bytes.
START_TEST(test_record_cached_completion_generation) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool prepare = (unsigned int)_i % 2u != 0;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_record(&bank, &image, format, 1, 10, 0, false);
    dos_rel_record_t record;
    dos_rel_record_start(&record, models[format], reference, 1, format == 2 ? 2 : 0, 1, 10, 0);
    uint8_t side[DISKIMAGE_SECTOR_SIZE], first[DISKIMAGE_SECTOR_SIZE], following[DISKIMAGE_SECTOR_SIZE] = {0};
    ck_assert_int_eq(prepare_record(&record, &bank, &image, side, first, following), DOS_REL_RECORD_READY);
    uint8_t dirty = 0;
    ck_assert_int_eq(dos_rel_record_put(&record, &bank, first, following, 'Z', &dirty), DOS_REL_RECORD_WRITING);
    ck_assert_int_eq(dos_rel_record_pad(&record, &bank, first, following, &dirty), DOS_REL_RECORD_FULL);
    if (prepare) {
        record.result = DOS_REL_RECORD_PENDING;
        record.phase = DOS_REL_RECORD_FOLLOWING;
    }
    uint8_t saved[DISKIMAGE_SECTOR_SIZE];
    memcpy(saved, first, sizeof(saved));
    const uint8_t pointer = record.pointer;
    const unsigned int reads = image.reads;
    mount_record(&bank, &replacement, format, 1, 10, 0, false);
    if (prepare) {
        ck_assert(!dos_rel_record_prepare_cached(&record, &bank, first, following));
    } else {
        ck_assert(!dos_rel_record_finish_write(&record, &bank, first, following));
    }
    ck_assert_int_eq(record.result, DOS_REL_RECORD_FAILED);
    ck_assert_int_eq(record.lookup.media_result, DOS_MEDIA_STALE);
    ck_assert_uint_eq(record.pointer, pointer);
    ck_assert_mem_eq(first, saved, sizeof(saved));
    ck_assert_uint_eq(first[DISKIMAGE_SECTOR_LINK_SIZE], 'Z');
    ck_assert_uint_eq(dirty, DOS_REL_DIRTY_FIRST);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(replacement.reads, 0);
}
END_TEST

// Independent caller-owned cursors/pages/masks survive another unit's invalidation.
START_TEST(test_record_put_peer_isolation) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t images[2] = {0};
    dos_media_bank_t bank = {0};
    dos_rel_record_t records[2];
    dos_unit_t pool = {0};
    uint8_t dirty[2] = {0};
    for (unsigned int owner = 0; owner < count_of(records); ++owner) {
        const dos_media_ref_t reference = mount_record_at(&bank, &images[owner], format, 1, 10, 0, false, owner, owner);
        images[owner].bytes[3][2] = 'A' + owner;
        dos_rel_record_start(&records[owner], models[format], reference, 1, format == 2 ? 2 : 0, 1, 10, 0);
        ck_assert_int_eq(prepare_record(&records[owner], &bank, &images[owner], pool.buffers[8 + owner].bytes,
                                        pool.buffers[4 + owner].bytes, pool.buffers[1 + owner].bytes),
                         DOS_REL_RECORD_READY);
    }
    ck_assert_int_eq(
        dos_rel_record_put(&records[0], &bank, pool.buffers[4].bytes, pool.buffers[1].bytes, 'Z', &dirty[0]),
        DOS_REL_RECORD_WRITING);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, NULL), DOS_MEDIA_OK);
    ck_assert_int_eq(dos_rel_record_pad(&records[0], &bank, pool.buffers[4].bytes, pool.buffers[1].bytes, &dirty[0]),
                     DOS_REL_RECORD_FAILED);
    const dos_rel_record_t retired = records[0];
    const dos_buffer_t saved = pool.buffers[4];
    ck_assert_int_eq(dos_rel_record_pad(&records[1], &bank, pool.buffers[5].bytes, pool.buffers[2].bytes, &dirty[1]),
                     DOS_REL_RECORD_FULL);
    ck_assert_mem_eq(&records[0], &retired, sizeof(retired));
    ck_assert_mem_eq(&pool.buffers[4], &saved, sizeof(saved));
    ck_assert_uint_eq(pool.buffers[4].bytes[2], 'Z');
    ck_assert_uint_eq(pool.buffers[5].bytes[2], 0);
    ck_assert_uint_eq(dirty[0], DOS_REL_DIRTY_FIRST);
    ck_assert_uint_eq(dirty[1], DOS_REL_DIRTY_FIRST);
}
END_TEST

// Register native index, clean-read and allocation-preflight contracts.
// Consume one explicit sector, allowing a partial write before failure.
static bool write_index(void* const context, const uint32_t offset, const void* const input, const size_t length) {
    rel_image_t* const image = context;
    ck_assert_uint_eq(length, DISKIMAGE_SECTOR_SIZE);
    ++image->writes;
    for (unsigned int page = 0; page < count_of(image->bytes); ++page) {
        if (image->offsets[page] == offset) {
            const bool failed = image->writes == image->fail_write;
            memcpy(image->bytes[page], input, failed ? length / 2u : length);
            return !failed;
        }
    }
    ck_abort_msg("unexpected REL write at offset %u", offset);
    return false;
}

// Keep sector publication distinct from backing-store durability.
static bool flush_index(void* const context) {
    rel_image_t* const image = context;
    ++image->flushes;
    return !image->fail_flush;
}

// Bind existing records with optional writes and injected storage failures.
static dos_media_ref_t mount_job_image(dos_media_bank_t* const bank, rel_image_t* const image,
                                       const unsigned int format, const unsigned int unit, const unsigned int slot,
                                       const bool writable) {
    dos_media_ref_t reference = mount_record_at(bank, image, format, 1, 200, 0, true, unit, slot);
    image->bytes[4][0] = 1;
    image->bytes[4][1] = 5;
    image->bytes[5][1] = UINT8_MAX;
    for (unsigned int page = 3; page < 6; ++page) {
        memset(image->bytes[page] + 2, 'A' + page - 3u, DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    }
    if (writable) {
        dos_media_io_t io = bank->units[unit].slots[slot].io;
        io.image.write = write_index;
        io.flush = flush_index;
        ck_assert_int_eq(dos_media_set_slot(bank, unit, slot, &io), DOS_MEDIA_OK);
        ck_assert_int_eq(dos_media_reference(bank, unit, slot, &reference), DOS_MEDIA_OK);
    }
    return reference;
}

// Prepare an existing record and consume real GETs before taking writer ownership.
static uint8_t prepare_job_channel(dos_unit_t* const unit, dos_media_bank_t* const bank, rel_image_t* const image,
                                   const unsigned int format, const dos_media_ref_t reference, const uint8_t secondary,
                                   const unsigned int consumed) {
    const uint8_t channel = dos_rel_channel_claim(unit, reference, secondary);
    ck_assert_uint_ne(channel, DOS_UNUSED);
    dos_rel_channel_prepare(unit, channel, models[format], 1, format == 2 ? 2 : 0, 1, 200, 0);
    finish_pipeline(unit, bank, image);
    for (unsigned int byte = 0; byte < consumed; ++byte) {
        uint8_t output;
        bool eoi;
        dos_source_t source;
        ck_assert_uint_eq(dos_output_stage(unit, bank, secondary, &output, 1, &eoi, &source), 1);
        dos_output_get(unit, bank, models[format], source, 1);
        finish_pipeline(unit, bank, image);
    }
    dos_channel_t* const state = &unit->channels[channel];
    ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_READY);
    state->ready = false;
    // SETJOB permits version zero in the currently resident per-drive BAM.
    unit->bam[reference.slot] = (dos_bam_t){.media = reference, .buffer = {.resident = true}};
    return channel;
}

// CLDCHN uses the active data page's LSTJOB, never writer flags or the media slot.
START_TEST(test_initialize_relative_active_job) {
    const unsigned int format = (unsigned int)_i / 4u;
    const unsigned int variant = (unsigned int)_i % 4u;
    rel_image_t image = {0}, peer_image = {0};
    dos_media_bank_t bank = {0};
    mount_job_image(&bank, &peer_image, format, 3, 0, true);
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    dos_media_ref_t peer_reference;
    ck_assert_int_eq(dos_media_reference(&bank, 3, 0, &peer_reference), DOS_MEDIA_OK);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t peer = prepare_job_channel(&unit, &bank, &peer_image, format, peer_reference, 3, 0);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, 0);
    dos_channel_t* const state = &unit.channels[channel];
    uint8_t active;
    if (variant == 3) {
        state->directory = DOS_DIRECTORY_HEADER;
        state->active = 1;
        active = state->active;
    } else {
        ck_assert(dos_rel_channel_begin_write(&unit, channel));
        state->rel.record.active = variant == 1;
        ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
        if (variant == 2) {
            ck_assert(dos_rel_channel_pad(&unit, channel));
            finish_pipeline(&unit, &bank, &image);
        }
        active = state->rel.record.active;
    }
    const uint16_t active_bit = 1u << state->buffers[active];
    const uint16_t other_bit = 1u << state->buffers[active ^ 1u];
    unit.buffer_drives =
        (unit.buffer_drives & (uint16_t)~(active_bit | other_bit)) | (variant == 1 ? other_bit : active_bit);
    const dos_channel_t owner = *state, peer_before = unit.channels[peer];
    const unsigned int writes = image.writes;
    // Fail INITDR's metadata read after teardown so this fixture needs no blank disk.
    image.fail_read = image.reads + 1u;
    const uint8_t request[] = {'I', '1'};
    dos_command(&unit, &bank, 3, request, sizeof(request));
    dos_work(&unit, &bank);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_mem_eq(&unit.channels[peer], &peer_before, sizeof(peer_before));
    if (variant == 1) {
        ck_assert_mem_eq(state, &owner, sizeof(owner));
    } else {
        ck_assert(!state->allocated);
        for (unsigned int page = 0; page < count_of(owner.buffers); ++page) {
            ck_assert(!(unit.buffer_use & (1u << owner.buffers[page])));
        }
    }
}
END_TEST

// Ordinary block editors cannot reinterpret the retained REL cursor or writer flags.
START_TEST(test_relative_writer_rejects_ordinary_block_editors) {
    const unsigned int format = (unsigned int)_i / 5u;
    const unsigned int command = (unsigned int)_i % 5u;
    static const char* const requests[] = {"B-R:2,1,1,3", "B-W:2,1,1,3", "U1:2,1,1,3", "U2:2,1,1,3", "B-P:2,2"};
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, 0);
    ck_assert(dos_rel_channel_begin_write(&unit, channel));
    ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
    const dos_channel_t owner = unit.channels[channel];
    const unsigned int reads = image.reads, writes = image.writes;
    dos_command(&unit, &bank, 3, (const uint8_t*)requests[command], strlen(requests[command]));
    ck_assert_int_eq(unit.status_code, DOS_STATUS_SYNTAX);
    ck_assert(!dos_work_pending(&unit));
    ck_assert_mem_eq(&unit.channels[channel], &owner, sizeof(owner));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
}
END_TEST

// Required MODIFY SETLST failures retry only the failed alternate, then consume the record.
START_TEST(test_position_required_prefetch_retry) {
    const unsigned int format = (unsigned int)_i / 6u;
    const unsigned int variant = (unsigned int)_i % 3u;
    const bool remount = (unsigned int)_i % 6u >= 3u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    if (variant == 0) {
        image.fail_read = 3u + (format == 2);
    }
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, 0);
    dos_channel_t* const state = &unit.channels[channel];
    ck_assert(dos_rel_channel_begin_write(&unit, channel));
    ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
    if (variant != 0) {
        image.fail_read = image.reads + (variant == 2 ? 2u : 1u);
        if (variant == 2) {
            unit.buffers[state->buffers[1]].sector = 6;
        }
    }
    const uint16_t number = variant == 0 ? 2 : 3;
    ck_assert(dos_rel_channel_position_buffer(&unit, channel, number, 0, 1, variant == 0 ? 3 : 4));
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->failed && !state->ready);
    const dos_channel_t failed = *state;
    const uint8_t required = variant == 0 ? 4 : 5;
    char status[sizeof(unit.status)];
    snprintf(status, sizeof(status), format == 0 ? "%02u,READ ERROR,01,%02u\r" : "%02u,READ ERROR,01,%02u,1\r",
             DOS_STATUS_READ_ERROR, required);
    ck_assert_str_eq(unit.status, status);
    ck_assert(dos_rel_channel_retry_position(&unit, channel));
    image.fail_read = image.reads + 1u;
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->failed && !state->ready);
    ck_assert_uint_eq(state->rel.number, failed.rel.number);
    ck_assert_uint_eq(state->rel.record.active, failed.rel.record.active);
    ck_assert_uint_eq(state->rel_write, failed.rel_write);
    if (remount) {
        mount_job_image(&bank, &replacement, format, 3, 1, true);
        ck_assert(dos_rel_channel_retry_position(&unit, channel));
        finish_pipeline(&unit, &bank, &image);
        ck_assert(state->failed && !state->ready);
        ck_assert_int_eq(state->rel.record.lookup.media_result, DOS_MEDIA_STALE);
        ck_assert_uint_eq(replacement.reads | replacement.writes, 0);
        ck_assert(!dos_rel_channel_retry_position(&unit, channel));
        return;
    }
    ck_assert(dos_rel_channel_retry_position(&unit, channel));
    const unsigned int reads = image.reads, writes = image.writes;
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->ready && !state->failed);
    ck_assert_uint_eq(image.reads, reads + 1u);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_str_eq(unit.status, status);
    const unsigned int first_bytes = variant == 0 ? 54u : 108u;
    for (unsigned int byte = 0; byte < 200; ++byte) {
        uint8_t output;
        bool eoi;
        dos_source_t source;
        ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &output, 1, &eoi, &source), 1);
        ck_assert_uint_eq(output, (variant == 0 ? 'A' : 'B') + (byte >= first_bytes));
        ck_assert_int_eq(eoi, byte == 199);
        dos_output_get(&unit, &bank, models[format], source, 1);
        finish_pipeline(&unit, &bank, &image);
    }
    ck_assert_uint_eq(state->rel.number, number + 1u);
}
END_TEST

// Replace unused index fixture pages with an explicit valid native BAM chain.
static unsigned int prepare_job_bam(dos_media_bank_t* const bank, rel_image_t* const image, const unsigned int format,
                                    const dos_media_ref_t reference) {
    const unsigned int parts = format == 0 ? 1u : format == 1 ? 2u : 4u;
    const unsigned int pages[] = {0, 1, 2, 6};
    const unsigned int tracks[] = {DISKIMAGE_D64_TRACK_COUNT, DISKIMAGE_D80_TRACK_COUNT, DISKIMAGE_D82_TRACK_COUNT};
    const unsigned int tracks_per_part = 50u, interleave = 3u;
    for (unsigned int part = 0; part < parts; ++part) {
        const unsigned int page = pages[part];
        memset(image->bytes[page], 0, DISKIMAGE_SECTOR_SIZE);
        ck_assert(diskimage_sector_offset(bank->units[reference.unit].slots[reference.slot].io.image.type,
                                          format == 0 ? 18 : 38, part * interleave, &image->offsets[page]));
        image->bytes[page][0] = part + 1u < parts ? 38 : 0;
        image->bytes[page][1] = (part + 1u) * interleave;
        if (format != 0) {
            image->bytes[page][4] = 1u + part * tracks_per_part;
            image->bytes[page][5] = MIN(1u + (part + 1u) * tracks_per_part, tracks[format] + 1u);
        }
    }
    return parts;
}

// The initial version gate must walk the cold BAM without combining it with WRITE.
START_TEST(test_write_cold_bam_is_cooperative) {
    const unsigned int format = (unsigned int)_i / 8u;
    const unsigned int variant = (unsigned int)_i % 8u;
    rel_image_t image = {0}, peer_image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t peer_reference = mount_job_image(&bank, &peer_image, format, 0, 1, true);
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, 253);
    dos_channel_t* const state = &unit.channels[channel];
    const unsigned int parts = prepare_job_bam(&bank, &image, format, reference);
    const unsigned int pages[] = {0, 1, 2, 6};
    const unsigned int tracks_per_part = 50u, interleave = 3u;
    if (variant == 3 || variant == 4) {
        image.bytes[pages[variant == 3 ? parts - 1u : 0]][2] = UINT8_MAX;
    } else if (variant == 7 && format != 0) {
        image.bytes[0][0] = 38;
        image.bytes[0][1] = 0;
    }
    unit.bam[1] = (dos_bam_t){0};
    if (variant == 6) {
        // The previous cache remains a live dirty owner on an independent media unit.
        peer_image.offsets[0] = image.offsets[0];
        unit.bam[1] = (dos_bam_t){
            .media = peer_reference, .buffer = {.track = format == 0 ? 18 : 38, .resident = true}, .dirty = true};
        if (format != 0) {
            unit.bam[1].buffer.bytes[4] = 1;
            unit.bam[1].buffer.bytes[5] = tracks_per_part + 1u;
        }
        const unsigned int entry = format == 0 ? 4u : 6u;
        unit.bam[1].buffer.bytes[entry] = 1;
        unit.bam[1].buffer.bytes[entry + 1u] = 1;
    }
    ck_assert(dos_rel_channel_begin_write(&unit, channel));
    ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
    const unsigned int reads = image.reads;
    if (variant == 1 || variant == 2) {
        image.fail_read = reads + (variant == 1 ? 1u : parts);
        // Reach the failed BAM read one pass at a time, retaining the old complete cache.
        for (unsigned int pass = 0; pass < 16 && !state->failed; ++pass) {
            const dos_bam_t old_bam = unit.bam[1];
            const unsigned int old_reads = image.reads, old_writes = image.writes;
            dos_work(&unit, &bank);
            ck_assert_uint_le(image.reads - old_reads + image.writes - old_writes, 1);
            if (state->failed) {
                ck_assert_mem_eq(&unit.bam[1], &old_bam, sizeof(old_bam));
            }
        }
        ck_assert(state->failed);
        ck_assert_uint_eq(image.writes, 0);
        ck_assert(state->rel_write & DOS_REL_DIRTY_FIRST);
        ck_assert(dos_rel_channel_retry_write(&unit, channel));
    } else if (variant == 5) {
        dos_work(&unit, &bank);
        ck_assert_uint_eq(image.reads, reads + 1u);
        ck_assert_uint_eq(image.writes, 0);
        mount_job_image(&bank, &replacement, format, 3, 1, true);
        finish_pipeline(&unit, &bank, &image);
        ck_assert(state->failed);
        ck_assert_uint_eq(replacement.reads | replacement.writes, 0);
        ck_assert(state->rel_write & DOS_REL_DIRTY_FIRST);
        return;
    } else if (variant == 6) {
        dos_work(&unit, &bank);
        ck_assert_uint_eq(peer_image.writes, 1);
        ck_assert_uint_eq(peer_image.bytes[0][format == 0 ? 4u : 6u], 1);
        ck_assert_uint_eq(image.reads, reads);
        ck_assert_uint_eq(image.writes, 0);
        ck_assert(!unit.bam[1].dirty);
    }
    finish_pipeline(&unit, &bank, &image);
    if (variant == 7 && format != 0) {
        ck_assert(state->failed);
        ck_assert_int_eq(unit.status_code, DOS_STATUS_DIRECTORY_ERROR);
        ck_assert_uint_eq(image.reads, reads + 4u);
        ck_assert_uint_eq(image.writes, 0);
        return;
    }
    if (variant == 3 || (variant == 4 && parts == 1)) {
        ck_assert(state->failed);
        ck_assert_int_eq(unit.status_code, DOS_STATUS_POWER_ON);
        ck_assert_uint_eq(image.writes, 0);
        ck_assert(state->rel_write & DOS_REL_DIRTY_FIRST);
        return;
    }
    ck_assert(!state->failed);
    ck_assert_uint_eq(image.reads, reads + parts + (variant == 1 || variant == 2));
    ck_assert_uint_eq(image.writes, 1);
    ck_assert_uint_eq(image.bytes[3][UINT8_MAX], 'Z');
    ck_assert_uint_eq(unit.bam[1].buffer.sector, (parts - 1u) * interleave);
}
END_TEST

// B-F may replace a paused leaf or failed writer's BAM before explicit retry.
START_TEST(test_write_bam_command_replacement) {
    const unsigned int format = (unsigned int)_i / 4u;
    const unsigned int variant = (unsigned int)_i % 4u;
    rel_image_t image = {0}, peer_image = {0};
    dos_media_bank_t bank = {0};
    mount_job_image(&bank, &peer_image, format, 3, 0, true);
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, variant == 1 ? 253 : 0);
    dos_channel_t* const state = &unit.channels[channel];
    const unsigned int parts = prepare_job_bam(&bank, &image, format, reference);
    dos_media_ref_t peer;
    ck_assert_int_eq(dos_media_reference(&bank, 3, 0, &peer), DOS_MEDIA_OK);
    prepare_job_bam(&bank, &peer_image, format, peer);
    unit.bam[1] = (dos_bam_t){0};
    ck_assert(dos_rel_channel_begin_write(&unit, channel));
    ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
    if (variant == 1) {
        image.fail_read = image.reads + parts;
    }
    // The direct leaf is explicitly caller-driven. Failed cooperative jobs also stop polling.
    for (unsigned int part = 0; part < parts; ++part) {
        const unsigned int reads = image.reads, writes = image.writes;
        ck_assert(!dos_rel_channel_writeback(&unit, &bank, channel, &state->rel_write));
        ck_assert_uint_le(image.reads - reads + image.writes - writes, 1);
    }
    ck_assert_int_eq(state->failed, variant == 1);
    ck_assert(!dos_work_pending(&unit));
    const dos_channel_t saved = *state;
    const dos_bam_t cached = unit.bam[1];
    if (variant == 3 && format != 0) {
        image.fail_read = image.reads + 1u;
    }
    const uint8_t request[] = "B-F:1,1,1", peer_request[] = "B-F:0,1,1";
    dos_command(&unit, &bank, 3, variant == 2 ? peer_request : request, sizeof(request) - 1u);
    dos_work(&unit, &bank);
    ck_assert(!dos_work_pending(&unit));
    if (variant == 2 || (variant == 3 && format != 0)) {
        ck_assert_mem_eq(&unit.bam[1], &cached, sizeof(cached));
        ck_assert_mem_eq(state, &saved, sizeof(saved));
    } else {
        ck_assert_int_eq(unit.status_code, DOS_STATUS_OK);
    }
    if (variant == 1) {
        ck_assert(dos_rel_channel_retry_write(&unit, channel));
    }
    bool completed = false;
    for (unsigned int pass = 0; pass < 16 && !completed && !state->failed; ++pass) {
        const unsigned int reads = image.reads, writes = image.writes;
        completed = dos_rel_channel_writeback(&unit, &bank, channel, &state->rel_write);
        ck_assert_uint_le(image.reads - reads + image.writes - writes, 1);
    }
    ck_assert(completed && !state->failed);
    ck_assert_uint_eq(state->rel.number, saved.rel.number);
    ck_assert_uint_eq(state->rel.record.pointer, saved.rel.record.pointer);
    ck_assert_uint_eq(image.bytes[3][variant == 1 ? UINT8_MAX : 2], 'Z');
    ck_assert(!(state->rel_write & DOS_REL_DIRTY_FIRST));
}
END_TEST

// Dirty POSBUF must preserve the native cached job and explicit retry.
START_TEST(test_pipeline_position_buffer_reuse) {
    const unsigned int format = (unsigned int)_i / 8u;
    const unsigned int variant = (unsigned int)_i % 8u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    if (variant == 5 || variant == 7) {
        image.bytes[3][0] = 0;
    }
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, 0);
    dos_channel_t* const state = &unit.channels[channel];
    if (variant == 1 || variant == 3 || variant >= 6) {
        ck_assert(dos_rel_channel_begin_write(&unit, channel));
        if (variant == 3) {
            const dos_rel_record_t saved = state->rel.record;
            state->rel.record.active ^= 1u;
            ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Q'));
            ck_assert(dos_rel_channel_writeback(&unit, &bank, channel, &state->rel_write));
            state->rel.record = saved;
        } else {
            ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
        }
    }
    dos_status(&unit, models[format], DOS_STATUS_FILE_EXISTS, 0, 0);
    dos_status_get(&unit, models[format], 2);
    const dos_unit_t before = unit;
    const unsigned int reads = image.reads, writes = image.writes;
    const uint8_t target = variant < 2 ? 3 : variant < 4 ? 4 : 5;
    const uint8_t position = variant == 1 ? 2 : 0;
    ck_assert(dos_rel_channel_position_buffer(&unit, channel, 1, position, 1, target));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert(!state->ready && !state->failed);
    ck_assert(!dos_rel_channel_position_buffer(&unit, channel, 2, 0, 1, 3));
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->ready && !state->failed);
    ck_assert_uint_eq(image.reads, reads + (variant >= 2));
    ck_assert_uint_eq(image.writes, writes + (variant >= 6));
    ck_assert_uint_eq(image.flushes, 0);
    const bool other = variant == 2 || variant == 3 || variant == 5 || variant == 7;
    ck_assert_uint_eq(state->rel.record.active, other);
    ck_assert_uint_eq(state->rel.record.pointer, variant == 1 ? 3 : 2);
    ck_assert_uint_eq(state->rel.number, 1);
    ck_assert_uint_eq(state->rel_write & (DOS_REL_DIRTY_FIRST | DOS_REL_DIRTY_FOLLOWING),
                      variant == 1 ? DOS_REL_DIRTY_FIRST : 0);
    ck_assert_uint_eq(unit.buffer_use, before.buffer_use);
    ck_assert_mem_eq(unit.status, before.status, sizeof(unit.status));
    ck_assert_uint_eq(unit.status_position, before.status_position);
    if (variant == 1) {
        ck_assert_uint_eq(unit.buffers[state->buffers[0]].bytes[2], 'Z');
        ck_assert_uint_eq(image.bytes[3][2], 'A');
    } else if (variant >= 6) {
        ck_assert_uint_eq(image.bytes[3][2], 'Z');
        ck_assert_uint_eq(image.bytes[3][3], 'A');
    }
    uint8_t output;
    bool eoi;
    dos_source_t source;
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &output, 1, &eoi, &source), 1);
    ck_assert_uint_eq(output, variant == 3 ? 'Q' : 'A' + target - 3u);
    ck_assert(!eoi);
    ck_assert(!dos_rel_channel_retry_position(&unit, channel));
}
END_TEST

// Failed positioning retains its job and dirty ownership, never new media.
START_TEST(test_pipeline_position_buffer_failures) {
    const unsigned int format = (unsigned int)_i / 5u;
    const unsigned int variant = (unsigned int)_i % 5u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    if (variant == 4) {
        image.fail_read = 3u + (format == 2);
    }
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, 0);
    dos_channel_t* const state = &unit.channels[channel];
    if (variant == 0) {
        ck_assert(dos_rel_channel_begin_write(&unit, channel));
        ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
        image.fail_write = image.writes + 1u;
    } else if (variant == 1 || variant == 2) {
        image.fail_read = image.reads + 1u;
    }
    ck_assert(dos_rel_channel_position_buffer(&unit, channel, 1, 0, 1, variant == 2 ? 4 : 5));
    if (variant == 3) {
        mount_job_image(&bank, &replacement, format, 3, 1, true);
    }
    finish_pipeline(&unit, &bank, &image);
    ck_assert_int_eq(state->failed, variant != 2);
    ck_assert_uint_eq(replacement.reads | replacement.writes, 0);
    const unsigned int reads = image.reads, writes = image.writes;
    const dos_unit_t failed = unit;
    dos_work(&unit, &bank);
    ck_assert_mem_eq(&unit, &failed, sizeof(failed));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    if (variant == 3) {
        ck_assert_int_eq(state->rel.record.lookup.media_result, DOS_MEDIA_STALE);
        ck_assert(!dos_rel_channel_retry_position(&unit, channel));
    } else if (variant == 2) {
        ck_assert(state->ready && state->rel.record.following_failed);
    } else {
        ck_assert(dos_rel_channel_retry_position(&unit, channel));
        finish_pipeline(&unit, &bank, &image);
        ck_assert(state->ready && !state->failed);
        ck_assert_uint_eq(state->rel.number, 1);
        ck_assert_uint_eq(state->rel.record.pointer, 2);
        ck_assert_uint_ne(unit.buffer_use, 0);
        ck_assert_uint_eq(image.flushes, 0);
        if (variant == 0) {
            ck_assert_uint_eq(image.bytes[3][2], 'Z');
            ck_assert_uint_eq(image.bytes[3][3], 'A');
        }
    }
}
END_TEST

// Write the saved physical page without clearing the peer's dirty bit.
START_TEST(test_channel_writeback_saved_page_and_dirty_mask) {
    const unsigned int format = (unsigned int)_i / 6u;
    const unsigned int variant = (unsigned int)_i % 6u;
    const unsigned int consumed[] = {0, 199, 253, 254, 399, 507};
    const dos_rel_record_result_t results[] = {
        DOS_REL_RECORD_WRITING,
        DOS_REL_RECORD_FULL,
        DOS_REL_RECORD_NEEDS_WRITEBACK,
    };
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, variant % 2u, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, consumed[variant]);
    dos_channel_t* const state = &unit.channels[channel];
    const unsigned int active = state->rel.record.active;
    ck_assert_uint_eq(active, variant >= 3);
    const uint8_t peer_bit = active == 0 ? DOS_REL_DIRTY_FOLLOWING : DOS_REL_DIRTY_FIRST;
    uint8_t dirty = peer_bit;
    ck_assert_int_eq(dos_rel_record_put(&state->rel.record, &bank, unit.buffers[state->buffers[0]].bytes,
                                        unit.buffers[state->buffers[1]].bytes, 'Z', &dirty),
                     results[variant % 3u]);
    dos_status(&unit, models[format], DOS_STATUS_FILE_EXISTS, 0, 0);
    dos_status_get(&unit, models[format], 2);
    dos_unit_t before = unit;
    uint8_t expected[7][DISKIMAGE_SECTOR_SIZE];
    memcpy(expected, image.bytes, sizeof(expected));
    const dos_buffer_t* const buffer = &unit.buffers[state->buffers[active]];
    ck_assert_uint_eq(buffer->track, 1);
    ck_assert_uint_eq(buffer->sector, active == 0 ? 3 : 4);
    memcpy(expected[buffer->sector], buffer->bytes, DISKIMAGE_SECTOR_SIZE);
    const unsigned int reads = image.reads;
    ck_assert(dos_rel_channel_writeback(&unit, &bank, channel, &dirty));
    ck_assert_mem_eq(image.bytes, expected, sizeof(expected));
    before.buffer_writes |= (uint16_t)1u << state->buffers[active];
    ck_assert_mem_eq(&unit, &before, sizeof(before));
    ck_assert_uint_eq(dirty, peer_bit);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, 1);
    ck_assert_uint_eq(image.flushes, 0);
    ck_assert(bank.units[reference.unit].slots[reference.slot].dirty);
    ck_assert(!dos_work_pending(&unit));
    ck_assert(dos_rel_channel_writeback(&unit, &bank, channel, &dirty));
    ck_assert_uint_eq(image.writes, 1);
    ck_assert_uint_eq(image.flushes, 0);
    ck_assert_uint_eq(dirty, peer_bit);
}
END_TEST

// Storage/version/lifetime failures retain the page, mask and record for explicit retry.
START_TEST(test_channel_writeback_failures_and_retry) {
    const unsigned int format = (unsigned int)_i / 5u;
    const unsigned int variant = (unsigned int)_i % 5u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, variant != 1);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, 0);
    dos_channel_t* const state = &unit.channels[channel];
    uint8_t dirty = DOS_REL_DIRTY_FOLLOWING;
    ck_assert_int_eq(dos_rel_record_put(&state->rel.record, &bank, unit.buffers[state->buffers[0]].bytes,
                                        unit.buffers[state->buffers[1]].bytes, 'Z', &dirty),
                     DOS_REL_RECORD_WRITING);
    if (variant == 0) {
        image.fail_write = 1;
    } else if (variant == 2) {
        unit.bam[1].buffer.bytes[2] = 'X';
    } else if (variant == 3) {
        ck_assert_int_eq(dos_media_set_slot(&bank, 3, 1, NULL), DOS_MEDIA_OK);
    } else if (variant == 4) {
        mount_job_image(&bank, &replacement, format, 3, 1, true);
    }
    const dos_rel_reader_t before = state->rel;
    const dos_buffer_t saved = unit.buffers[state->buffers[0]];
    const unsigned int reads = image.reads;
    ck_assert(!dos_rel_channel_writeback(&unit, &bank, channel, &dirty));
    ck_assert(state->failed && !state->ready);
    ck_assert_mem_eq(&state->rel, &before, sizeof(before));
    ck_assert_mem_eq(&unit.buffers[state->buffers[0]], &saved, sizeof(saved));
    ck_assert_uint_eq(dirty, DOS_REL_DIRTY_FIRST | DOS_REL_DIRTY_FOLLOWING);
    ck_assert_uint_eq(image.writes, variant == 0);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.flushes, 0);
    ck_assert_uint_eq(replacement.writes, 0);
    ck_assert_uint_eq(replacement.reads, 0);
    ck_assert_int_eq(unit.status_code, variant == 1   ? DOS_STATUS_WRITE_PROTECT
                                       : variant == 2 ? DOS_STATUS_POWER_ON
                                                      : DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(unit.selected_drive, 1);
    ck_assert_ptr_nonnull(strstr(unit.status, format == 0 ? ",01,03\r" : ",01,03,1\r"));
    ck_assert(!dos_work_pending(&unit));
    if (variant == 0 || variant == 2) {
        image.fail_write = 0;
        unit.bam[1].buffer.bytes[2] = 0;
        char status[DOS_STATUS_LINE_CAPACITY];
        memcpy(status, unit.status, sizeof(status));
        ck_assert(dos_rel_channel_writeback(&unit, &bank, channel, &dirty));
        ck_assert(!state->failed && !state->ready);
        ck_assert_uint_eq(dirty, DOS_REL_DIRTY_FOLLOWING);
        ck_assert_mem_eq(image.bytes[3], saved.bytes, DISKIMAGE_SECTOR_SIZE);
        ck_assert_mem_eq(unit.status, status, sizeof(status));
        ck_assert_mem_eq(&state->rel, &before, sizeof(before));
    }
}
END_TEST

// Successful callback completion is not a flush or full record/CLOSE completion.
START_TEST(test_channel_writeback_flush_remains_separate) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 0, 0, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, 0);
    dos_channel_t* const state = &unit.channels[channel];
    uint8_t dirty = 0;
    dos_rel_record_put(&state->rel.record, &bank, unit.buffers[state->buffers[0]].bytes,
                       unit.buffers[state->buffers[1]].bytes, 'Z', &dirty);
    ck_assert(dos_rel_channel_writeback(&unit, &bank, channel, &dirty));
    ck_assert_uint_eq(dirty, 0);
    ck_assert(bank.units[0].slots[0].dirty);
    const dos_unit_t before = unit;
    image.fail_flush = true;
    ck_assert_int_eq(dos_media_flush_unit(&bank, 0), DOS_MEDIA_IO_ERROR);
    ck_assert(bank.units[0].slots[0].dirty);
    ck_assert(dos_rel_channel_writeback(&unit, &bank, channel, &dirty));
    ck_assert_uint_eq(image.writes, 1);
    ck_assert_uint_eq(image.flushes, 1);
    image.fail_flush = false;
    ck_assert_int_eq(dos_media_flush_unit(&bank, 0), DOS_MEDIA_OK);
    ck_assert(!bank.units[0].slots[0].dirty);
    ck_assert_uint_eq(image.flushes, 2);
    ck_assert_mem_eq(&unit, &before, sizeof(before));
}
END_TEST

// A peer drive's successful job cannot clear a failed writer or its retained status.
START_TEST(test_channel_writeback_peer_failure_isolation) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t images[2] = {0};
    dos_media_bank_t bank = {0};
    mount_job_image(&bank, &images[0], format, 2, 0, true);
    mount_job_image(&bank, &images[1], format, 2, 1, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    uint8_t channels[2], dirty[2] = {0};
    for (unsigned int owner = 0; owner < count_of(channels); ++owner) {
        dos_media_ref_t reference;
        ck_assert_int_eq(dos_media_reference(&bank, 2, owner, &reference), DOS_MEDIA_OK);
        channels[owner] = prepare_job_channel(&unit, &bank, &images[owner], format, reference, 2u + owner, 0);
        dos_channel_t* const state = &unit.channels[channels[owner]];
        dos_rel_record_put(&state->rel.record, &bank, unit.buffers[state->buffers[0]].bytes,
                           unit.buffers[state->buffers[1]].bytes, 'X' + owner, &dirty[owner]);
    }
    images[0].fail_write = 1;
    ck_assert(!dos_rel_channel_writeback(&unit, &bank, channels[0], &dirty[0]));
    const dos_channel_t failed = unit.channels[channels[0]];
    const dos_buffer_t saved = unit.buffers[failed.buffers[0]];
    char status[DOS_STATUS_LINE_CAPACITY];
    memcpy(status, unit.status, sizeof(status));
    ck_assert(dos_rel_channel_writeback(&unit, &bank, channels[1], &dirty[1]));
    ck_assert_mem_eq(&unit.channels[channels[0]], &failed, sizeof(failed));
    ck_assert_mem_eq(&unit.buffers[failed.buffers[0]], &saved, sizeof(saved));
    ck_assert_mem_eq(unit.status, status, sizeof(status));
    ck_assert_uint_eq(unit.selected_drive, 0);
    ck_assert_uint_eq(dirty[0], DOS_REL_DIRTY_FIRST);
    ck_assert_uint_eq(dirty[1], 0);
    ck_assert_uint_eq(images[0].writes, 1);
    ck_assert_uint_eq(images[1].writes, 1);
    ck_assert_uint_eq(images[1].bytes[3][2], 'Y');
}
END_TEST

// Dirty NRBUF selects the linked page at byte 2 without advancing the record.
START_TEST(test_channel_write_handoff_cached_and_refilled) {
    const unsigned int format = (unsigned int)_i / 6u;
    const unsigned int variant = (unsigned int)_i % 6u;
    const unsigned int consumed[] = {253, 507};
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, variant % 2u, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, consumed[variant / 3u]);
    dos_channel_t* const state = &unit.channels[channel];
    dos_buffer_t* const old = &unit.buffers[state->buffers[state->rel.record.active]];
    dos_buffer_t* const next = &unit.buffers[state->buffers[state->rel.record.active ^ 1u]];
    const uint8_t next_bit = state->rel.record.active == 0 ? DOS_REL_DIRTY_FOLLOWING : DOS_REL_DIRTY_FIRST;
    uint8_t dirty = variant % 3u == 2 ? next_bit : 0;
    ck_assert_int_eq(dos_rel_record_put(&state->rel.record, &bank, unit.buffers[state->buffers[0]].bytes,
                                        unit.buffers[state->buffers[1]].bytes, 'Z', &dirty),
                     DOS_REL_RECORD_NEEDS_WRITEBACK);
    ck_assert(dos_rel_channel_writeback(&unit, &bank, channel, &dirty));
    if (variant % 3u == 1) {
        // A recycled written page or deferred READ is not the following sector.
        if (variant < 3) {
            next->sector = old->sector;
        } else {
            unit.buffer_drives ^= (uint16_t)(1u << state->buffers[state->rel.record.active ^ 1u]);
        }
        state->rel.record.following = false;
    } else if (variant % 3u == 2) {
        next->bytes[2] = 'P';
    }
    dos_status(&unit, models[format], DOS_STATUS_FILE_EXISTS, 0, 0);
    dos_status_get(&unit, models[format], 2);
    const dos_unit_t before = unit;
    const unsigned int reads = image.reads;
    ck_assert(dos_rel_channel_advance_write(&unit, &bank, channel, dirty));
    ck_assert_uint_eq(image.reads, reads + (variant % 3u == 1));
    ck_assert_uint_eq(image.writes, 1);
    ck_assert_uint_eq(image.flushes, 0);
    ck_assert_uint_eq(dirty, variant % 3u == 2 ? next_bit : 0);
    dos_rel_reader_t expected = before.channels[channel].rel;
    expected.record.active ^= 1u;
    expected.record.pointer = 2;
    expected.record.following = false;
    expected.record.following_failed = false;
    expected.record.result = DOS_REL_RECORD_WRITING;
    ck_assert_mem_eq(&state->rel, &expected, sizeof(expected));
    ck_assert(!state->failed && !state->ready && !dos_work_pending(&unit));
    ck_assert_mem_eq(old, &before.buffers[before.channels[channel].buffers[before.channels[channel].rel.record.active]],
                     sizeof(*old));
    ck_assert(next->resident && !next->failed);
    ck_assert_uint_eq(next->track, old->bytes[0]);
    ck_assert_uint_eq(next->sector, old->bytes[1]);
    const uint8_t* const expected_bytes =
        variant % 3u == 1 ? image.bytes[old->bytes[1]] : before.buffers[state->buffers[expected.record.active]].bytes;
    ck_assert_mem_eq(next->bytes, expected_bytes, DISKIMAGE_SECTOR_SIZE);
    ck_assert_uint_eq((unit.buffer_drives >> state->buffers[expected.record.active]) & 1u, reference.slot);
    ck_assert_mem_eq(unit.status, before.status, sizeof(unit.status));
    ck_assert_uint_eq(unit.status_position, before.status_position);
    ck_assert_uint_eq(unit.selected_drive, before.selected_drive);
    ck_assert(!dos_rel_channel_advance_write(&unit, &bank, channel, dirty));
    ck_assert_uint_eq(image.reads, reads + (variant % 3u == 1));
    ck_assert_int_eq(dos_rel_record_put(&state->rel.record, &bank, unit.buffers[state->buffers[0]].bytes,
                                        unit.buffers[state->buffers[1]].bytes, 'Y', &dirty),
                     DOS_REL_RECORD_WRITING);
    ck_assert_uint_eq(next->bytes[2], 'Y');
    ck_assert_uint_eq(dirty, next_bit);
    ck_assert_int_eq(dos_rel_record_pad(&state->rel.record, &bank, unit.buffers[state->buffers[0]].bytes,
                                        unit.buffers[state->buffers[1]].bytes, &dirty),
                     DOS_REL_RECORD_FULL);
    ck_assert(dos_rel_channel_writeback(&unit, &bank, channel, &dirty));
    ck_assert_uint_eq(image.writes, 2);
    ck_assert_uint_eq(dirty, 0);
    ck_assert_mem_eq(image.bytes[next->sector], next->bytes, DISKIMAGE_SECTOR_SIZE);
}
END_TEST

// Native TSTWRT requires READ even when a completed WRITE header matches the link.
START_TEST(test_channel_write_handoff_matching_written_header) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 0, 0, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, 253);
    dos_channel_t* const state = &unit.channels[channel];
    uint8_t dirty = 0;
    ck_assert_int_eq(dos_rel_record_put(&state->rel.record, &bank, unit.buffers[state->buffers[0]].bytes,
                                        unit.buffers[state->buffers[1]].bytes, 'Z', &dirty),
                     DOS_REL_RECORD_NEEDS_WRITEBACK);
    ck_assert(dos_rel_channel_writeback(&unit, &bank, channel, &dirty));
    const dos_rel_record_t saved = state->rel.record;
    state->rel.record.active ^= 1u;
    state->rel.record.result = DOS_REL_RECORD_WRITING;
    dirty = DOS_REL_DIRTY_FOLLOWING;
    ck_assert(dos_rel_channel_writeback(&unit, &bank, channel, &dirty));
    state->rel.record = saved;
    const unsigned int reads = image.reads;
    ck_assert(dos_rel_channel_advance_write(&unit, &bank, channel, dirty));
    ck_assert_uint_eq(image.reads, reads + 1);
    ck_assert_uint_eq(image.writes, 2);
    ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_WRITING);
    ck_assert_uint_eq(state->rel.record.active, 1);
    ck_assert_uint_eq(state->rel.record.pointer, 2);
    ck_assert(!(unit.buffer_writes & (1u << state->buffers[1])));
    ck_assert(unit.buffer_writes & (1u << state->buffers[0]));
}
END_TEST

// Pending writes, protected dirty pages and missing links cannot be overwritten.
START_TEST(test_channel_write_handoff_blocked_ownership) {
    const unsigned int format = (unsigned int)_i / 5u;
    const unsigned int variant = (unsigned int)_i % 5u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 0, 0, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, variant == 4 ? 199 : 253);
    dos_channel_t* const state = &unit.channels[channel];
    uint8_t dirty = variant == 1 || variant == 2 ? DOS_REL_DIRTY_FOLLOWING : 0;
    dos_rel_record_put(&state->rel.record, &bank, unit.buffers[state->buffers[0]].bytes,
                       unit.buffers[state->buffers[1]].bytes, 'Z', &dirty);
    if (variant == 0) {
        image.fail_write = 1;
        ck_assert(!dos_rel_channel_writeback(&unit, &bank, channel, &dirty));
    } else {
        ck_assert(dos_rel_channel_writeback(&unit, &bank, channel, &dirty));
    }
    if (variant == 1) {
        unit.buffers[state->buffers[1]].sector = 3;
    } else if (variant == 2) {
        unit.buffers[state->buffers[1]].failed = true;
    } else if (variant == 3) {
        unit.buffers[state->buffers[0]].bytes[0] = 0;
    }
    const dos_unit_t before = unit;
    const unsigned int reads = image.reads, writes = image.writes;
    for (unsigned int attempt = 0; attempt < 2; ++attempt) {
        ck_assert(!dos_rel_channel_advance_write(&unit, &bank, channel, dirty));
        ck_assert_mem_eq(&unit, &before, sizeof(before));
        ck_assert_uint_eq(image.reads, reads);
        ck_assert_uint_eq(image.writes, writes);
        ck_assert_uint_eq(image.flushes, 0);
    }
}
END_TEST

// Failed reads keep the written page selected and block PUT until explicit retry.
START_TEST(test_channel_write_handoff_failure_and_retry) {
    const unsigned int format = (unsigned int)_i / 6u;
    const unsigned int variant = (unsigned int)_i % 6u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, 253);
    dos_channel_t* const state = &unit.channels[channel];
    uint8_t dirty = 0;
    dos_rel_record_put(&state->rel.record, &bank, unit.buffers[state->buffers[0]].bytes,
                       unit.buffers[state->buffers[1]].bytes, 'Z', &dirty);
    ck_assert(dos_rel_channel_writeback(&unit, &bank, channel, &dirty));
    if (variant < 4) {
        unit.buffers[state->buffers[1]].resident = false;
    }
    if (variant == 0) {
        image.fail_read = image.reads + 1u;
    } else if (variant == 1) {
        unit.buffers[state->buffers[0]].bytes[1] = UINT8_MAX;
    } else if (variant == 2 || variant == 4) {
        ck_assert_int_eq(dos_media_set_slot(&bank, 3, 1, NULL), DOS_MEDIA_OK);
    } else {
        mount_job_image(&bank, &replacement, format, 3, 1, true);
    }
    const dos_unit_t before = unit;
    const unsigned int reads = image.reads;
    ck_assert(!dos_rel_channel_advance_write(&unit, &bank, channel, dirty));
    ck_assert(state->failed && !state->ready && !dos_work_pending(&unit));
    ck_assert_uint_eq(dirty, 0);
    ck_assert_uint_eq(image.reads, reads + (variant == 0));
    ck_assert_uint_eq(image.writes, 1);
    ck_assert_uint_eq(replacement.reads, 0);
    ck_assert_uint_eq(replacement.writes, 0);
    ck_assert_mem_eq(&unit.buffers[state->buffers[0]], &before.buffers[state->buffers[0]], sizeof(dos_buffer_t));
    dos_rel_reader_t expected = before.channels[channel].rel;
    expected.record.lookup.media_result = variant == 0   ? DOS_MEDIA_IO_ERROR
                                          : variant == 1 ? DOS_MEDIA_RANGE
                                                         : DOS_MEDIA_STALE;
    ck_assert_mem_eq(&state->rel, &expected, sizeof(expected));
    ck_assert_int_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_uint_eq(unit.selected_drive, 1);
    const char* const coordinates =
        variant == 1 ? (format == 0 ? ",01,55\r" : ",01,255,1\r") : (format == 0 ? ",01,04\r" : ",01,04,1\r");
    ck_assert_ptr_nonnull(strstr(unit.status, coordinates));
    if (variant == 0) {
        dos_buffer_t* const next = &unit.buffers[state->buffers[1]];
        ck_assert(next->failed && !next->resident);
        ck_assert_uint_eq(next->bytes[0], 0xee);
        ck_assert_int_eq(dos_rel_record_put(&state->rel.record, &bank, unit.buffers[state->buffers[0]].bytes,
                                            next->bytes, 'X', &dirty),
                         DOS_REL_RECORD_NEEDS_WRITEBACK);
        char status[DOS_STATUS_LINE_CAPACITY];
        memcpy(status, unit.status, sizeof(status));
        ck_assert(dos_rel_channel_advance_write(&unit, &bank, channel, dirty));
        ck_assert_uint_eq(image.reads, reads + 2u);
        ck_assert_uint_eq(image.writes, 1);
        ck_assert(next->resident && !next->failed && !state->failed);
        ck_assert_uint_eq(state->rel.record.active, 1);
        ck_assert_uint_eq(state->rel.record.pointer, 2);
        ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_WRITING);
        ck_assert_mem_eq(next->bytes, image.bytes[4], DISKIMAGE_SECTOR_SIZE);
        ck_assert_mem_eq(unit.status, status, sizeof(status));
    }
}
END_TEST

// A peer's refill cannot retire the failed writer or overwrite its retained status.
START_TEST(test_channel_write_handoff_peer_isolation) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t images[2] = {0};
    dos_media_bank_t bank = {0};
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    uint8_t channels[2], dirty[2] = {0};
    for (unsigned int owner = 0; owner < count_of(channels); ++owner) {
        const dos_media_ref_t reference = mount_job_image(&bank, &images[owner], format, 2, owner, true);
        channels[owner] = prepare_job_channel(&unit, &bank, &images[owner], format, reference, 2u + owner, 253);
        dos_channel_t* const state = &unit.channels[channels[owner]];
        dos_rel_record_put(&state->rel.record, &bank, unit.buffers[state->buffers[0]].bytes,
                           unit.buffers[state->buffers[1]].bytes, 'X' + owner, &dirty[owner]);
        ck_assert(dos_rel_channel_writeback(&unit, &bank, channels[owner], &dirty[owner]));
        unit.buffers[state->buffers[1]].resident = false;
    }
    images[0].fail_read = images[0].reads + 1u;
    ck_assert(!dos_rel_channel_advance_write(&unit, &bank, channels[0], dirty[0]));
    const dos_channel_t failed = unit.channels[channels[0]];
    const dos_buffer_t old = unit.buffers[failed.buffers[0]], partial = unit.buffers[failed.buffers[1]];
    char status[DOS_STATUS_LINE_CAPACITY];
    memcpy(status, unit.status, sizeof(status));
    ck_assert(dos_rel_channel_advance_write(&unit, &bank, channels[1], dirty[1]));
    ck_assert_mem_eq(&unit.channels[channels[0]], &failed, sizeof(failed));
    ck_assert_mem_eq(&unit.buffers[failed.buffers[0]], &old, sizeof(old));
    ck_assert_mem_eq(&unit.buffers[failed.buffers[1]], &partial, sizeof(partial));
    ck_assert_mem_eq(unit.status, status, sizeof(status));
    ck_assert_uint_eq(unit.selected_drive, 0);
    ck_assert_uint_eq(images[0].writes, 1);
    ck_assert_uint_eq(images[1].writes, 1);
    ck_assert_uint_eq(dirty[0] | dirty[1], 0);
    ck_assert_int_eq(unit.channels[channels[1]].rel.record.result, DOS_REL_RECORD_WRITING);
}
END_TEST

// Owned CLREC resumes through cooperative NRBUF without consuming another byte.
START_TEST(test_owned_writer_cooperative_padding) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int variant = (unsigned int)_i % 3u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, 253);
    dos_channel_t* const state = &unit.channels[channel];
    const dos_rel_reader_t before = state->rel;
    const uint16_t buffers = unit.buffer_use;
    const unsigned int reads = image.reads;
    ck_assert(!dos_rel_channel_put(&unit, &bank, channel, 'X'));
    ck_assert(dos_rel_channel_begin_write(&unit, channel));
    ck_assert(!dos_rel_channel_begin_write(&unit, channel));
    if (variant == 1) {
        unit.buffers[state->buffers[1]].sector = 3;
    }
    if (variant != 2) {
        ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
        ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_NEEDS_WRITEBACK);
    }
    ck_assert(dos_rel_channel_pad(&unit, channel));
    ck_assert(dos_rel_channel_pad(&unit, channel));
    ck_assert(!dos_rel_channel_put(&unit, &bank, channel, 'X'));
    ck_assert_uint_eq(image.writes, 0);
    ck_assert_uint_eq(image.reads, reads);
    dos_status(&unit, models[format], DOS_STATUS_FILE_EXISTS, 0, 0);
    dos_status_get(&unit, models[format], 2);
    char status[DOS_STATUS_LINE_CAPACITY];
    memcpy(status, unit.status, sizeof(status));
    unsigned int steps = 0;
    while (dos_work_pending(&unit)) {
        ck_assert_uint_lt(steps++, 5);
        const unsigned int prior_reads = image.reads, prior_writes = image.writes;
        dos_work(&unit, &bank);
        ck_assert_uint_le(image.reads - prior_reads + image.writes - prior_writes, 1);
        ck_assert(!state->ready && !state->failed);
    }
    ck_assert_uint_eq(steps, variant == 2 ? 4 : 3);
    ck_assert_uint_eq(image.writes, 1);
    ck_assert_uint_eq(image.reads, reads + (variant == 1));
    ck_assert_uint_eq(image.flushes, 0);
    ck_assert_uint_eq(image.bytes[3][255], variant == 2 ? 0 : 'Z');
    ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_FULL);
    ck_assert_uint_eq(state->rel.record.active, 1);
    ck_assert_uint_eq(state->rel.record.pointer, state->rel.record.next - 1u);
    for (unsigned int offset = DISKIMAGE_SECTOR_LINK_SIZE; offset < state->rel.record.next; ++offset) {
        ck_assert_uint_eq(unit.buffers[state->buffers[1]].bytes[offset], 0);
    }
    ck_assert_uint_eq(state->rel_write, DOS_REL_DIRTY_FOLLOWING | (format != 0 ? DOS_REL_WRITE_MODIFIED : 0));
    ck_assert_uint_eq(state->rel.number, before.number);
    ck_assert_int_eq(state->rel.get_active, before.get_active);
    ck_assert_int_eq(state->rel.no_record, before.no_record);
    ck_assert_uint_eq(unit.buffer_use, buffers);
    ck_assert_mem_eq(unit.status, status, sizeof(status));
    ck_assert_uint_eq(unit.status_position, 2);
    ck_assert(!dos_rel_channel_put(&unit, &bank, channel, 'X'));
    ck_assert(dos_rel_channel_pad(&unit, channel));
    ck_assert(!dos_work_pending(&unit));
    dos_channel_t owned = *state;
    ck_assert(!dos_close(&unit, &bank, models[format], 2));
    owned.closing = DOS_CLOSE_REL_SCRUB;
    ck_assert_mem_eq(state, &owned, sizeof(owned));
    ck_assert_uint_eq(unit.buffer_use, buffers);
    ck_assert_uint_eq(image.writes, 1);
    ck_assert_uint_eq(image.flushes, 0);
}
END_TEST

// Failed owned jobs stop polling, retain flags/pages and require explicit retry.
START_TEST(test_owned_writer_failure_retry_and_lifetime) {
    const unsigned int format = (unsigned int)_i / 6u;
    const unsigned int variant = (unsigned int)_i % 6u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, variant == 5 ? 252 : 253);
    dos_channel_t* const state = &unit.channels[channel];
    unsigned int external_flushes = 0;
    ck_assert(dos_rel_channel_begin_write(&unit, channel));
    ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
    if (variant == 5) {
        const dos_buffer_t saved = unit.buffers[state->buffers[0]];
        const uint8_t flags = state->rel_write;
        ck_assert_int_eq(dos_media_set_slot(&bank, 3, 1, NULL), DOS_MEDIA_OK);
        ck_assert(!dos_rel_channel_put(&unit, &bank, channel, 'X'));
        ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_FAILED);
        ck_assert(state->failed && !state->ready);
        ck_assert_uint_eq(state->rel_write, flags);
        ck_assert_mem_eq(&unit.buffers[state->buffers[0]], &saved, sizeof(saved));
        ck_assert(!dos_rel_channel_retry_write(&unit, channel));
        ck_assert(!dos_work_pending(&unit));
        ck_assert_int_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
        ck_assert_uint_eq(image.writes, 0);
    } else {
        ck_assert(dos_rel_channel_pad(&unit, channel));
        if (variant == 0) {
            image.fail_write = 1;
        } else if (variant == 1) {
            unit.bam[1].buffer.bytes[2] = 'X';
        } else if (variant == 2 || variant == 4) {
            dos_work(&unit, &bank);
            ck_assert_uint_eq(image.writes, 1);
            if (variant == 2) {
                unit.buffers[state->buffers[1]].resident = false;
                image.fail_read = image.reads + 1u;
            } else {
                mount_job_image(&bank, &replacement, format, 3, 1, true);
                external_flushes = image.flushes;
            }
        } else {
            ck_assert_int_eq(dos_media_set_slot(&bank, 3, 1, NULL), DOS_MEDIA_OK);
        }
        const dos_buffer_t saved = unit.buffers[state->buffers[0]];
        const uint8_t flags = state->rel_write;
        dos_work(&unit, &bank);
        ck_assert(state->failed && !state->ready);
        ck_assert(!dos_work_pending(&unit));
        ck_assert_uint_eq(state->rel_write, flags);
        ck_assert_mem_eq(&unit.buffers[state->buffers[0]], &saved, sizeof(saved));
        ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_NEEDS_WRITEBACK);
        const dos_unit_t failed = unit;
        const unsigned int reads = image.reads, writes = image.writes;
        dos_work(&unit, &bank);
        ck_assert(!dos_rel_channel_put(&unit, &bank, channel, 'X'));
        ck_assert(!dos_rel_channel_pad(&unit, channel));
        ck_assert_mem_eq(&unit, &failed, sizeof(failed));
        ck_assert_uint_eq(image.reads, reads);
        ck_assert_uint_eq(image.writes, writes);
        unit.bam[1].buffer.bytes[2] = 0;
        ck_assert(dos_rel_channel_retry_write(&unit, channel));
        ck_assert(dos_work_pending(&unit));
        if (variant < 3) {
            unsigned int steps = 0;
            while (dos_work_pending(&unit)) {
                ck_assert_uint_lt(steps++, 5);
                dos_work(&unit, &bank);
            }
            ck_assert(!state->failed);
            ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_FULL);
            ck_assert_mem_eq(unit.status, failed.status, sizeof(unit.status));
            ck_assert_uint_eq(state->rel_write & DOS_REL_WRITE_PADDING, 0);
            ck_assert_uint_eq(state->rel_write & DOS_REL_DIRTY_FOLLOWING, DOS_REL_DIRTY_FOLLOWING);
        } else {
            dos_work(&unit, &bank);
            ck_assert(state->failed && !dos_work_pending(&unit));
            ck_assert_uint_eq(image.reads, reads);
            ck_assert_uint_eq(image.writes, writes);
        }
    }
    ck_assert_uint_eq(replacement.reads, 0);
    ck_assert_uint_eq(replacement.writes, 0);
    ck_assert_uint_eq(image.flushes, external_flushes);
    ck_assert_uint_eq(unit.selected_drive, 1);
}
END_TEST

// A failed owner cannot prevent a peer's CLREC jobs or lose its retained state.
START_TEST(test_owned_writer_peer_and_pool_ownership) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t images[2] = {0};
    dos_media_bank_t bank = {0};
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    uint8_t channels[2];
    for (unsigned int owner = 0; owner < count_of(channels); ++owner) {
        mount_job_image(&bank, &images[owner], format, 2, owner, true);
    }
    uint16_t owned_buffers = 0;
    for (unsigned int owner = 0; owner < count_of(channels); ++owner) {
        dos_media_ref_t reference;
        ck_assert_int_eq(dos_media_reference(&bank, 2, owner, &reference), DOS_MEDIA_OK);
        channels[owner] = prepare_job_channel(&unit, &bank, &images[owner], format, reference, 2u + owner, 253);
        for (unsigned int page = 0; page < DOS_CHANNEL_BUFFER_COUNT; ++page) {
            const uint16_t bit = 1u << unit.channels[channels[owner]].buffers[page];
            ck_assert_uint_eq(owned_buffers & bit, 0);
            owned_buffers |= bit;
        }
    }
    for (unsigned int owner = 0; owner < count_of(channels); ++owner) {
        ck_assert(dos_rel_channel_begin_write(&unit, channels[owner]));
        ck_assert(dos_rel_channel_put(&unit, &bank, channels[owner], 'X' + owner));
        ck_assert(dos_rel_channel_pad(&unit, channels[owner]));
    }
    images[0].fail_write = 1;
    dos_work(&unit, &bank);
    const dos_channel_t failed = unit.channels[channels[0]];
    const dos_buffer_t saved = unit.buffers[failed.buffers[0]];
    char status[DOS_STATUS_LINE_CAPACITY];
    memcpy(status, unit.status, sizeof(status));
    ck_assert(failed.failed && dos_work_pending(&unit));
    unsigned int steps = 0;
    while (dos_work_pending(&unit)) {
        ck_assert_uint_lt(steps++, 4);
        dos_work(&unit, &bank);
    }
    ck_assert_mem_eq(&unit.channels[channels[0]], &failed, sizeof(failed));
    ck_assert_mem_eq(&unit.buffers[failed.buffers[0]], &saved, sizeof(saved));
    ck_assert_mem_eq(unit.status, status, sizeof(status));
    ck_assert_int_eq(unit.channels[channels[1]].rel.record.result, DOS_REL_RECORD_FULL);
    ck_assert_uint_eq(images[0].writes, 1);
    ck_assert_uint_eq(images[1].writes, 1);
    ck_assert_uint_eq(unit.buffer_use, owned_buffers);
}
END_TEST

// Prepare an explicit selected index entry, retaining the shared sparse job image.
// Prepare an explicit selected index entry in the shared job image.
static uint8_t prepare_completion_channel(dos_unit_t* const unit, dos_media_bank_t* const bank,
                                          rel_image_t* const image, const unsigned int format,
                                          const dos_media_ref_t reference, const uint16_t number, const uint8_t size,
                                          const uint8_t position) {
    const dos_rel_address_t address = dos_rel_address(models[format], number, size, position);
    image->bytes[0][2] = address.side;
    image->bytes[0][address.index] = 1;
    image->bytes[0][address.index + 1u] = 3;
    image->bytes[2][DOS_REL_GROUP_TABLE_OFFSET + 2u * address.group] = 1;
    const uint8_t channel = dos_rel_channel_claim(unit, reference, 2);
    ck_assert_uint_ne(channel, DOS_UNUSED);
    dos_rel_channel_prepare(unit, channel, models[format], 1, format == 2 ? 2 : 0, number, size, position);
    finish_pipeline(unit, bank, image);
    ck_assert_int_eq(unit->channels[channel].rel.record.result, DOS_REL_RECORD_READY);
    unit->bam[reference.slot] = (dos_bam_t){.media = reference, .buffer = {.resident = true}};
    return channel;
}

// Cached RD40 precedes overflow status and never releases dirty ownership.
START_TEST(test_owned_cached_completion_status_and_cursor) {
    const unsigned int format = (unsigned int)_i / 12u;
    const unsigned int variant = (unsigned int)_i % 12u;
    const unsigned int index = variant / 2u;
    const bool overflow = variant % 2u;
    static const struct {
        uint16_t number;
        uint8_t size, position, pointer, last, active;
        bool missing;
    } cases[] = {
        {0, 10, 0, 12, 12, 0, false},    {1, 1, 0, 3, 3, 0, false},    {UINT16_MAX, 1, 0, 5, 5, 0, false},
        {1, 200, 0, 202, 147, 0, false}, {26, 10, 4, 8, 17, 1, false}, {1, 254, 0, 255, 255, 0, true},
    };
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, index % 2u, true);
    if (index == 0) {
        for (unsigned int page = 3; page < 6; ++page) {
            memset(image.bytes[page] + DISKIMAGE_SECTOR_LINK_SIZE, 0, DISKIMAGE_SECTOR_PAYLOAD_SIZE);
        }
    } else if (cases[index].missing) {
        image.bytes[3][0] = 0;
    }
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_completion_channel(&unit, &bank, &image, format, reference, cases[index].number,
                                                       cases[index].size, cases[index].position);
    if (index == 3) {
        uint8_t byte;
        bool eoi;
        dos_source_t source;
        ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &byte, 1, &eoi, &source), 1);
        dos_output_get(&unit, &bank, models[format], source, 1);
    }
    dos_channel_t* const state = &unit.channels[channel];
    ck_assert(dos_rel_channel_begin_write(&unit, channel));
    ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
    ck_assert(dos_rel_channel_pad(&unit, channel));
    finish_pipeline(&unit, &bank, &image);
    ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_FULL);
    dos_status(&unit, models[format], DOS_STATUS_FILE_EXISTS, 0, 0);
    dos_status_get(&unit, models[format], 2);
    unit.last_drive = unit.selected_drive = reference.slot ^ 1u;
    const dos_unit_t before = unit;
    const unsigned int reads = image.reads, writes = image.writes;
    ck_assert(dos_rel_channel_finish_cached(&unit, &bank, channel, overflow));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(image.flushes, 0);
    ck_assert_mem_eq(unit.buffers, before.buffers, sizeof(unit.buffers));
    ck_assert_uint_eq(unit.buffer_use, before.buffer_use);
    ck_assert_uint_eq(state->rel_write, before.channels[channel].rel_write | DOS_REL_WRITE_FINISHED);
    ck_assert_uint_eq(state->rel.number, (uint16_t)(cases[index].number + 1u));
    ck_assert_uint_eq(state->rel.record.pointer, cases[index].pointer);
    ck_assert_uint_eq(state->rel.record.last, cases[index].last);
    ck_assert_uint_eq(state->rel.record.active, cases[index].active);
    ck_assert_int_eq(state->rel.record.result, cases[index].missing ? DOS_REL_RECORD_MISSING : DOS_REL_RECORD_READY);
    ck_assert(!state->ready && !state->failed && !state->rel.get_active && !state->rel.no_record);
    ck_assert(!dos_work_pending(&unit));
    ck_assert_uint_eq(unit.last_drive, before.last_drive);
    ck_assert_uint_eq(unit.selected_drive, reference.slot);
    if (!overflow && format == 0) {
        ck_assert_mem_eq(unit.status, before.status, sizeof(unit.status));
        ck_assert_uint_eq(unit.status_position, 2);
        ck_assert_int_eq(unit.status_code, DOS_STATUS_FILE_EXISTS);
    } else {
        char expected[DOS_STATUS_LINE_CAPACITY];
        const char* const text = overflow ? "51,OVERFLOW IN RECORD" : "00, OK";
        if (format == 0) {
            snprintf(expected, sizeof(expected), "%s,00,00\r", text);
        } else {
            snprintf(expected, sizeof(expected), "%s,00,00,%u\r", text, reference.slot);
        }
        ck_assert_str_eq(unit.status, expected);
        ck_assert_uint_eq(unit.status_position, 0);
        ck_assert_int_eq(unit.status_code, overflow ? DOS_STATUS_RECORD_OVERFLOW : DOS_STATUS_OK);
    }
    uint8_t byte = 0xee;
    bool eoi = false;
    ck_assert(dos_rel_record_peek(&state->rel.record, &bank, unit.buffers[state->buffers[0]].bytes,
                                  unit.buffers[state->buffers[1]].bytes, &byte, &eoi));
    ck_assert_uint_eq(byte, cases[index].missing ? '\r' : index == 0 ? 0 : cases[index].active == 0 ? 'A' : 'B');
    ck_assert_int_eq(eoi, cases[index].pointer == cases[index].last);
    const dos_unit_t finished = unit;
    ck_assert(!dos_rel_channel_finish_cached(&unit, &bank, channel, !overflow));
    ck_assert(!dos_rel_channel_put(&unit, &bank, channel, 'X'));
    ck_assert(!dos_rel_channel_pad(&unit, channel));
    ck_assert(!dos_rel_channel_begin_write(&unit, channel));
    dos_source_t source;
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &byte, 1, &eoi, &source), 0);
    ck_assert_mem_eq(&unit, &finished, sizeof(finished));
}
END_TEST

// Only completed padding with its full native window can publish cached 51.
START_TEST(test_owned_cached_completion_boundaries) {
    const unsigned int format = (unsigned int)_i / 4u;
    const unsigned int variant = (unsigned int)_i % 4u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 0, 0, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel =
        variant == 3 ? prepare_completion_channel(&unit, &bank, &image, format, reference, 2, 127, 127)
                     : prepare_job_channel(&unit, &bank, &image, format, reference, 2, variant == 2 ? 399 : 0);
    ck_assert(dos_rel_channel_begin_write(&unit, channel));
    ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
    if (variant == 1) {
        ck_assert(dos_rel_channel_pad(&unit, channel));
    }
    dos_status(&unit, models[format], DOS_STATUS_FILE_EXISTS, 0, 0);
    dos_status_get(&unit, models[format], 2);
    const dos_unit_t before = unit;
    const unsigned int reads = image.reads, writes = image.writes;
    ck_assert_int_eq(dos_rel_channel_finish_cached(&unit, &bank, channel, true), variant == 2);
    if (variant == 2) {
        ck_assert_int_eq(unit.status_code, DOS_STATUS_RECORD_OVERFLOW);
        ck_assert_uint_eq(unit.channels[channel].rel.number, before.channels[channel].rel.number + 1u);
        ck_assert_uint_ne(unit.channels[channel].rel_write & DOS_REL_WRITE_FINISHED, 0);
    } else {
        ck_assert_mem_eq(&unit, &before, sizeof(before));
    }
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
}
END_TEST

// Stale media cannot commit the counter or a successful end-record status.
START_TEST(test_owned_cached_completion_stale_retains_dirty) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool replace = (unsigned int)_i % 2u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, 199);
    ck_assert(dos_rel_channel_begin_write(&unit, channel));
    ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
    const dos_unit_t before = unit;
    if (replace) {
        mount_job_image(&bank, &replacement, format, 3, 1, true);
    } else {
        ck_assert_int_eq(dos_media_set_slot(&bank, 3, 1, NULL), DOS_MEDIA_OK);
    }
    const unsigned int reads = image.reads;
    ck_assert(!dos_rel_channel_finish_cached(&unit, &bank, channel, true));
    dos_channel_t* const state = &unit.channels[channel];
    ck_assert(state->failed && !state->ready && !dos_work_pending(&unit));
    ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_FAILED);
    ck_assert_uint_eq(state->rel.number, before.channels[channel].rel.number);
    ck_assert_uint_eq(state->rel_write, before.channels[channel].rel_write);
    ck_assert_mem_eq(unit.buffers, before.buffers, sizeof(unit.buffers));
    ck_assert_uint_eq(unit.buffer_use, before.buffer_use);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, 0);
    ck_assert_uint_eq(replacement.reads | replacement.writes, 0);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert_ptr_nonnull(strstr(unit.status, format == 0 ? ",01,03\r" : ",01,03,1\r"));
    ck_assert(!dos_rel_channel_retry_write(&unit, channel));
}
END_TEST

// A peer's model-specific completion changes status, never another owner's pages.
START_TEST(test_owned_cached_completion_peer_isolation) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t images[2] = {0};
    dos_media_bank_t bank = {0};
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    uint8_t channels[2];
    for (unsigned int owner = 0; owner < count_of(channels); ++owner) {
        mount_job_image(&bank, &images[owner], format, 2, owner, true);
    }
    for (unsigned int owner = 0; owner < count_of(channels); ++owner) {
        dos_media_ref_t reference;
        ck_assert_int_eq(dos_media_reference(&bank, 2, owner, &reference), DOS_MEDIA_OK);
        channels[owner] = prepare_job_channel(&unit, &bank, &images[owner], format, reference, 2u + owner, 199);
    }
    for (unsigned int owner = 0; owner < count_of(channels); ++owner) {
        ck_assert(dos_rel_channel_begin_write(&unit, channels[owner]));
        ck_assert(dos_rel_channel_put(&unit, &bank, channels[owner], 'X' + owner));
    }
    ck_assert(dos_rel_channel_finish_cached(&unit, &bank, channels[0], true));
    const dos_channel_t first = unit.channels[channels[0]];
    const dos_buffer_t saved = unit.buffers[first.buffers[first.rel.record.active]];
    ck_assert(dos_rel_channel_finish_cached(&unit, &bank, channels[1], false));
    ck_assert_mem_eq(&unit.channels[channels[0]], &first, sizeof(first));
    ck_assert_mem_eq(&unit.buffers[first.buffers[first.rel.record.active]], &saved, sizeof(saved));
    ck_assert_int_eq(unit.status_code, format == 0 ? DOS_STATUS_RECORD_OVERFLOW : DOS_STATUS_OK);
    ck_assert_uint_eq(images[0].writes | images[1].writes, 0);
}
END_TEST

// Finish an in-page write, optionally reversing the two physical data roles.
// Complete an in-page write, optionally reversing physical data roles.
static uint8_t prepare_cached_read_return(dos_unit_t* const unit, dos_media_bank_t* const bank,
                                          rel_image_t* const image, const unsigned int format,
                                          const dos_media_ref_t reference, const uint8_t size, const bool reverse) {
    const uint8_t channel = prepare_completion_channel(unit, bank, image, format, reference, 1, size, 0);
    dos_channel_t* const state = &unit->channels[channel];
    if (reverse) {
        const uint8_t first = state->buffers[0];
        state->buffers[0] = state->buffers[1];
        state->buffers[1] = first;
        state->rel.record.active ^= 1u;
    }
    ck_assert(!dos_rel_channel_resume_read(unit, bank, channel));
    ck_assert(dos_rel_channel_begin_write(unit, channel));
    ck_assert(dos_rel_channel_put(unit, bank, channel, 'Z'));
    ck_assert(dos_rel_channel_pad(unit, channel));
    finish_pipeline(unit, bank, image);
    ck_assert(dos_rel_channel_finish_cached(unit, bank, channel, true));
    return channel;
}

// Commit a previously staged prefix only through the outgoing dirty page seam.
// Retire a real short prefix before consuming the outgoing dirty-page seam.
static void consume_dirty_read_page(dos_unit_t* const unit, dos_media_bank_t* const bank, const unsigned int format,
                                    const uint8_t channel) {
    dos_channel_t* const state = &unit->channels[channel];
    const unsigned int remaining = DISKIMAGE_SECTOR_SIZE - state->rel.record.pointer;
    unsigned int consumed = 0;
    while (state->ready) {
        ck_assert_uint_lt(consumed, remaining);
        const dos_unit_t before = *unit;
        uint8_t output[DOS_STAGE_CAPACITY];
        bool eoi;
        dos_source_t source;
        const unsigned int count =
            dos_output_stage(unit, bank, state->secondary, output, sizeof(output), &eoi, &source);
        ck_assert_uint_gt(count, 0);
        ck_assert_uint_le(count, remaining - consumed);
        ck_assert_mem_eq(unit, &before, sizeof(before));
        for (unsigned int index = 0; index < count; ++index) {
            ck_assert_uint_eq(output[index], 'A');
        }
        // Retire one short actual prefix first, abandoning its speculative tail.
        const unsigned int taken = consumed == 0 ? 1 : count;
        dos_output_get(unit, bank, models[format], source, taken);
        consumed += taken;
    }
    ck_assert_uint_eq(consumed, remaining);
    ck_assert(state->rel_write & DOS_REL_WRITE_READBACK);
    ck_assert(dos_work_pending(unit));
}

// A second LISTEN resumes at the actual GET cursor and preserves dirty pages.
START_TEST(test_cached_read_return_reenters_write) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int variant = (unsigned int)_i % 3u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_cached_read_return(&unit, &bank, &image, format, reference, 200, false);
    dos_channel_t* const state = &unit.channels[channel];
    ck_assert(!dos_rel_channel_begin_write(&unit, channel));
    ck_assert(dos_rel_channel_resume_read(&unit, &bank, channel));
    if (variant == 2) {
        consume_dirty_read_page(&unit, &bank, format, channel);
        const dos_unit_t pending = unit;
        ck_assert(!dos_rel_channel_begin_write(&unit, channel));
        ck_assert_mem_eq(&unit, &pending, sizeof(pending));
        finish_pipeline(&unit, &bank, &image);
    } else if (variant == 1) {
        uint8_t byte;
        bool eoi;
        dos_source_t source;
        ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &byte, 1, &eoi, &source), 1);
        dos_output_get(&unit, &bank, models[format], source, 1);
    }

    const dos_unit_t before = unit;
    const uint8_t active = state->rel.record.active, pointer = state->rel.record.pointer;
    const unsigned int reads = image.reads, writes = image.writes;
    ck_assert(dos_rel_channel_begin_write(&unit, channel));
    ck_assert(!state->ready && !state->failed);
    ck_assert_int_eq(state->access, DOS_ACCESS_MODIFY);
    ck_assert_uint_eq(state->rel_write, before.channels[channel].rel_write & (uint8_t)~DOS_REL_WRITE_FINISHED);
    ck_assert_mem_eq(&state->rel, &before.channels[channel].rel, sizeof(state->rel));
    ck_assert_mem_eq(unit.buffers, before.buffers, sizeof(unit.buffers));
    ck_assert_uint_eq(unit.buffer_use, before.buffer_use);
    ck_assert_mem_eq(unit.status, before.status, sizeof(unit.status));
    ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Y'));
    ck_assert_uint_eq(unit.buffers[state->buffers[active]].bytes[pointer], 'Y');
    ck_assert_uint_ne(state->rel_write & (active == 0 ? DOS_REL_DIRTY_FIRST : DOS_REL_DIRTY_FOLLOWING), 0);
    ck_assert_uint_eq(unit.buffers[state->buffers[0]].bytes[2], 'Z');
    ck_assert_uint_eq(unit.buffers[state->buffers[0]].bytes[3], 0);
    ck_assert_int_eq(state->rel.get_active, before.channels[channel].rel.get_active);
    ck_assert_uint_eq(state->rel.number, 2);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert(dos_rel_channel_finish_write(&unit, channel, false));
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->ready && !state->failed);
    ck_assert_uint_eq(state->rel.number, 3);
    ck_assert_int_eq(unit.status_code, format == 0 ? DOS_STATUS_RECORD_OVERFLOW : DOS_STATUS_OK);
    ck_assert_uint_eq(image.flushes, 0);
}
END_TEST

// Dirty NRBUF writes the outgoing saved header before exposing the next page.
START_TEST(test_cached_read_return_dirty_boundary) {
    const unsigned int format = (unsigned int)_i / 8u;
    const unsigned int variant = (unsigned int)_i % 8u;
    const unsigned int slot = variant / 4u;
    const bool reverse = (variant / 2u) % 2u, eoi_crossing = variant % 2u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, slot, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel =
        prepare_cached_read_return(&unit, &bank, &image, format, reference, eoi_crossing ? 127 : 200, reverse);
    dos_channel_t* const state = &unit.channels[channel];
    const dos_unit_t finished = unit;
    const unsigned int reads = image.reads;
    ck_assert(dos_rel_channel_resume_read(&unit, &bank, channel));
    ck_assert(state->ready && state->access == DOS_ACCESS_MODIFY);
    ck_assert_mem_eq(&state->rel, &finished.channels[channel].rel, sizeof(state->rel));
    ck_assert_mem_eq(unit.buffers, finished.buffers, sizeof(unit.buffers));
    ck_assert_uint_eq(state->rel_write, finished.channels[channel].rel_write);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, 0);
    const uint8_t outgoing = state->rel.record.active;
    const dos_buffer_t saved = unit.buffers[state->buffers[outgoing]];
    consume_dirty_read_page(&unit, &bank, format, channel);
    ck_assert_uint_eq(state->rel.record.active, outgoing ^ 1u);
    ck_assert_uint_eq(state->rel.number, eoi_crossing ? 3 : 2);
    const dos_rel_reader_t cursor = state->rel;
    const uint8_t flags = state->rel_write;
    const unsigned int callbacks = image.reads + image.writes;
    uint8_t output[DOS_STAGE_CAPACITY];
    bool eoi;
    dos_source_t source;
    ck_assert(!dos_rel_channel_resume_read(&unit, &bank, channel));
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, output, sizeof(output), &eoi, &source), 0);
    ck_assert_uint_eq(image.reads + image.writes, callbacks);
    dos_work(&unit, &bank);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, 1);
    ck_assert_uint_eq(image.flushes, 0);
    ck_assert_mem_eq(image.bytes[3], saved.bytes, sizeof(saved.bytes));
    ck_assert_mem_eq(unit.buffers, finished.buffers, sizeof(unit.buffers));
    if (eoi_crossing) {
        ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_READY);
        ck_assert_uint_eq(state->rel.record.pointer, 2);
        ck_assert_uint_eq(state->rel.record.next, 129);
        ck_assert_uint_eq(state->rel.record.last, 128);
        ck_assert_uint_eq(state->rel.number, cursor.number);
    } else {
        ck_assert_mem_eq(&state->rel, &cursor, sizeof(cursor));
    }
    const uint8_t bit = outgoing == 0 ? DOS_REL_DIRTY_FIRST : DOS_REL_DIRTY_FOLLOWING;
    ck_assert_uint_eq(state->rel_write, flags & (uint8_t)~(bit | DOS_REL_WRITE_READBACK));
    ck_assert_mem_eq(unit.status, finished.status, sizeof(unit.status));
    ck_assert_uint_eq(unit.status_position, finished.status_position);
    ck_assert_uint_eq(unit.selected_drive, finished.selected_drive);
    ck_assert_uint_eq(unit.last_drive, finished.last_drive);
    ck_assert(state->ready);
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, output, 1, &eoi, &source), 1);
    ck_assert_uint_eq(output[0], 'B');
    while (state->ready) {
        const unsigned int count = dos_output_stage(&unit, &bank, 2, output, sizeof(output), &eoi, &source);
        ck_assert_uint_gt(count, 0);
        dos_output_get(&unit, &bank, models[format], source, count);
    }
    ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_PENDING);
    ck_assert_int_eq(state->rel.record.phase, eoi_crossing ? DOS_REL_RECORD_CLEAN_NEXT : DOS_REL_RECORD_CLEAN_GET);
    ck_assert(!state->failed && dos_work_pending(&unit));
    const dos_unit_t boundary = unit;
    ck_assert(!dos_rel_channel_resume_read(&unit, &bank, channel));
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->ready && !state->failed && !dos_work_pending(&unit));
    ck_assert_uint_eq(state->rel.number, boundary.channels[channel].rel.number);
    ck_assert_uint_eq(state->rel.record.pointer, boundary.channels[channel].rel.record.pointer);
    ck_assert_mem_eq(unit.status, boundary.status, sizeof(unit.status));
    ck_assert_uint_eq(image.reads, reads + 1);
    ck_assert_uint_eq(image.writes, 1);
    ck_assert_uint_eq(image.flushes, 0);
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, output, 1, &eoi, &source), 1);
    ck_assert_uint_eq(output[0], 'C');
}
END_TEST

// Failed post-GET writes keep the consumed cursor and physical dirty page for retry.
START_TEST(test_cached_read_return_write_failure_retry) {
    const unsigned int format = (unsigned int)_i / 8u;
    const unsigned int variant = (unsigned int)_i % 8u;
    const unsigned int failure = variant / 2u;
    const bool eoi_crossing = variant % 2u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel =
        prepare_cached_read_return(&unit, &bank, &image, format, reference, eoi_crossing ? 127 : 200, true);
    dos_channel_t* const state = &unit.channels[channel];
    ck_assert(dos_rel_channel_resume_read(&unit, &bank, channel));
    consume_dirty_read_page(&unit, &bank, format, channel);
    if (failure == 0) {
        image.fail_write = 1;
    } else if (failure == 1) {
        unit.bam[1].buffer.bytes[2] = UINT8_MAX;
    } else if (failure == 2) {
        ck_assert_int_eq(dos_media_set_slot(&bank, 3, 1, NULL), DOS_MEDIA_OK);
    } else {
        mount_job_image(&bank, &replacement, format, 3, 1, true);
    }
    const dos_unit_t before = unit;
    const unsigned int reads = image.reads;
    dos_work(&unit, &bank);
    ck_assert(state->failed && !state->ready && !dos_work_pending(&unit));
    ck_assert_mem_eq(&state->rel, &before.channels[channel].rel, sizeof(state->rel));
    ck_assert_uint_eq(state->rel_write, before.channels[channel].rel_write);
    ck_assert_mem_eq(unit.buffers, before.buffers, sizeof(unit.buffers));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, failure == 0);
    ck_assert_uint_eq(replacement.reads | replacement.writes, 0);
    ck_assert_int_eq(unit.status_code, failure == 1 ? DOS_STATUS_POWER_ON : DOS_STATUS_WRITE_ERROR);
    if (failure != 1) {
        ck_assert_ptr_nonnull(strstr(unit.status, format == 0 ? ",01,03\r" : ",01,03,1\r"));
    }
    const dos_unit_t failed = unit;
    dos_work(&unit, &bank);
    ck_assert_mem_eq(&unit, &failed, sizeof(failed));
    ck_assert(!dos_rel_channel_resume_read(&unit, &bank, channel));
    ck_assert(dos_rel_channel_retry_write(&unit, channel));
    unit.bam[1].buffer.bytes[2] = 0;
    dos_work(&unit, &bank);
    if (failure < 2 && eoi_crossing) {
        ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_READY);
        ck_assert_uint_eq(state->rel.record.pointer, before.channels[channel].rel.record.pointer);
        ck_assert_uint_eq(state->rel.number, before.channels[channel].rel.number);
    } else {
        ck_assert_mem_eq(&state->rel, &before.channels[channel].rel, sizeof(state->rel));
    }
    ck_assert_mem_eq(unit.status, failed.status, sizeof(unit.status));
    if (failure < 2) {
        ck_assert(!state->failed && state->ready);
        ck_assert_uint_eq(image.writes, failure == 0 ? 2 : 1);
        ck_assert(!(state->rel_write & (DOS_REL_DIRTY_FOLLOWING | DOS_REL_WRITE_READBACK)));
    } else {
        ck_assert(state->failed && !state->ready);
        ck_assert_uint_eq(state->rel_write, before.channels[channel].rel_write);
        ck_assert_uint_eq(image.writes | replacement.writes | replacement.reads, 0);
    }
    ck_assert(!dos_work_pending(&unit));
}
END_TEST

// Read restoration and actual GET both reject replaced media without losing dirty bytes.
START_TEST(test_cached_read_return_lifetime) {
    const unsigned int format = (unsigned int)_i / 4u;
    const unsigned int variant = (unsigned int)_i % 4u;
    const bool resumed = variant >= 2, replace = variant % 2u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_cached_read_return(&unit, &bank, &image, format, reference, 200, false);
    uint8_t byte;
    bool eoi;
    dos_source_t source;
    if (resumed) {
        ck_assert(dos_rel_channel_resume_read(&unit, &bank, channel));
        ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &byte, 1, &eoi, &source), 1);
    }
    const dos_unit_t before = unit;
    if (replace) {
        mount_job_image(&bank, &replacement, format, 3, 1, true);
    } else {
        ck_assert_int_eq(dos_media_set_slot(&bank, 3, 1, NULL), DOS_MEDIA_OK);
    }
    if (resumed) {
        dos_output_get(&unit, &bank, models[format], source, 1);
    } else {
        ck_assert(!dos_rel_channel_resume_read(&unit, &bank, channel));
    }
    const dos_channel_t* const state = &unit.channels[channel];
    ck_assert(state->failed && !state->ready && !dos_work_pending(&unit));
    ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_FAILED);
    ck_assert_uint_eq(state->rel.record.pointer, before.channels[channel].rel.record.pointer);
    ck_assert_uint_eq(state->rel.number, before.channels[channel].rel.number);
    ck_assert_uint_eq(state->rel_write, before.channels[channel].rel_write);
    ck_assert_mem_eq(unit.buffers, before.buffers, sizeof(unit.buffers));
    ck_assert_int_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_uint_eq(image.writes | replacement.reads | replacement.writes, 0);
    ck_assert(!dos_rel_channel_retry_write(&unit, channel));
}
END_TEST

// Restoring a missing CR/EOI does not publish 50 until an actual GET.
START_TEST(test_cached_read_return_missing_get) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool overflow = (unsigned int)_i % 2u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    image.bytes[3][0] = 0;
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_completion_channel(&unit, &bank, &image, format, reference, 1, 254, 0);
    ck_assert(dos_rel_channel_begin_write(&unit, channel));
    ck_assert(dos_rel_channel_pad(&unit, channel));
    finish_pipeline(&unit, &bank, &image);
    ck_assert(dos_rel_channel_finish_cached(&unit, &bank, channel, overflow));
    const dos_unit_t before = unit;
    ck_assert(dos_rel_channel_resume_read(&unit, &bank, channel));
    ck_assert_mem_eq(unit.status, before.status, sizeof(unit.status));
    for (unsigned int repeat = 0; repeat < 2; ++repeat) {
        uint8_t output[DOS_STAGE_CAPACITY];
        bool eoi;
        dos_source_t source;
        ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, output, sizeof(output), &eoi, &source), 1);
        ck_assert_uint_eq(output[0], '\r');
        ck_assert(eoi);
        dos_output_get(&unit, &bank, models[format], source, 1);
        ck_assert_str_eq(unit.status,
                         format == 0 ? "50, RECORD NOT PRESENT,00,00\r" : "50, RECORD NOT PRESENT,00,00,1\r");
        ck_assert(unit.channels[channel].ready && !dos_work_pending(&unit));
        ck_assert_uint_eq(unit.channels[channel].rel.number, before.channels[channel].rel.number);
        ck_assert_uint_eq(unit.channels[channel].rel_write, before.channels[channel].rel_write);
        ck_assert_mem_eq(unit.buffers, before.buffers, sizeof(unit.buffers));
        ck_assert_uint_eq(image.writes | image.flushes, 0);
    }
}
END_TEST

// A peer can finish its cached read WRITE while the first owner retains a failure.
START_TEST(test_cached_read_return_peer_isolation) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t images[2] = {0};
    dos_media_bank_t bank = {0};
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    uint8_t channels[2];
    for (unsigned int owner = 0; owner < count_of(channels); ++owner) {
        mount_job_image(&bank, &images[owner], format, 2, owner, true);
    }
    for (unsigned int owner = 0; owner < count_of(channels); ++owner) {
        dos_media_ref_t reference;
        ck_assert_int_eq(dos_media_reference(&bank, 2, owner, &reference), DOS_MEDIA_OK);
        channels[owner] = prepare_job_channel(&unit, &bank, &images[owner], format, reference, 2u + owner, 199);
    }
    const uint16_t buffers = unit.buffer_use;
    for (unsigned int owner = 0; owner < count_of(channels); ++owner) {
        const uint8_t channel = channels[owner];
        ck_assert(dos_rel_channel_begin_write(&unit, channel));
        ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
        ck_assert(dos_rel_channel_finish_cached(&unit, &bank, channel, true));
        ck_assert(dos_rel_channel_resume_read(&unit, &bank, channel));
        consume_dirty_read_page(&unit, &bank, format, channel);
    }
    images[0].fail_write = 1;
    dos_work(&unit, &bank);
    const dos_channel_t failed = unit.channels[channels[0]];
    const dos_buffer_t saved = unit.buffers[failed.buffers[0]];
    char status[DOS_STATUS_LINE_CAPACITY];
    memcpy(status, unit.status, sizeof(status));
    ck_assert(failed.failed && dos_work_pending(&unit));
    dos_work(&unit, &bank);
    ck_assert(!dos_work_pending(&unit));
    ck_assert(unit.channels[channels[1]].ready && !unit.channels[channels[1]].failed);
    while (unit.channels[channels[1]].rel.number == 2) {
        uint8_t output[DOS_STAGE_CAPACITY];
        bool eoi;
        dos_source_t source;
        const unsigned int count = dos_output_stage(&unit, &bank, 3, output, sizeof(output), &eoi, &source);
        ck_assert_uint_gt(count, 0);
        dos_output_get(&unit, &bank, models[format], source, count);
    }
    ck_assert(unit.channels[channels[1]].ready);
    ck_assert_mem_eq(&unit.channels[channels[0]], &failed, sizeof(failed));
    ck_assert_mem_eq(&unit.buffers[failed.buffers[0]], &saved, sizeof(saved));
    ck_assert_mem_eq(unit.status, status, sizeof(status));
    ck_assert_uint_eq(images[0].writes, 1);
    ck_assert_uint_eq(images[1].writes, 1);
    ck_assert_uint_eq(unit.buffer_use, buffers);
}
END_TEST

// Native SETLST observes old WRITE bytes, not a speculative forward-link refill.
START_TEST(test_cached_next_retained_write_window) {
    const unsigned int format = (unsigned int)_i / 6u;
    const unsigned int variant = (unsigned int)_i % 6u;
    const bool reverse = variant >= 3;
    const unsigned int pattern = variant % 3u;
    const unsigned int prefixes[] = {0, 1, 100};
    const uint8_t expected_last[] = {255, 2, 93};
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_completion_channel(&unit, &bank, &image, format, reference, 1, 200, 0);
    dos_channel_t* const state = &unit.channels[channel];
    if (reverse) {
        const uint8_t first = state->buffers[0];
        state->buffers[0] = state->buffers[1];
        state->buffers[1] = first;
        state->rel.record.active ^= 1u;
    }
    ck_assert(dos_rel_channel_begin_write(&unit, channel));
    for (unsigned int prefix = 0; prefix < prefixes[pattern]; ++prefix) {
        ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
    }
    ck_assert(dos_rel_channel_pad(&unit, channel));
    finish_pipeline(&unit, &bank, &image);
    ck_assert(dos_rel_channel_finish_cached(&unit, &bank, channel, true));
    ck_assert(dos_rel_channel_resume_read(&unit, &bank, channel));
    const unsigned int reads = image.reads;
    consume_dirty_read_page(&unit, &bank, format, channel);
    dos_work(&unit, &bank);
    ck_assert(state->ready && state->rel.number == 2);
    const dos_buffer_t inactive = unit.buffers[state->buffers[state->rel.record.active ^ 1u]];
    uint8_t output[DOS_STAGE_CAPACITY];
    bool eoi;
    dos_source_t source;
    while (state->rel.number == 2) {
        const unsigned int count = dos_output_stage(&unit, &bank, 2, output, sizeof(output), &eoi, &source);
        ck_assert_uint_gt(count, 0);
        dos_output_get(&unit, &bank, models[format], source, count);
    }
    ck_assert(state->ready && !state->failed && !state->rel.record.following);
    ck_assert_uint_eq(state->rel.record.pointer, 148);
    ck_assert_uint_eq(state->rel.record.next, 94);
    ck_assert_uint_eq(state->rel.record.last, expected_last[pattern]);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, 1);
    ck_assert_mem_eq(&unit.buffers[state->buffers[state->rel.record.active ^ 1u]], &inactive, sizeof(inactive));
    while (state->ready) {
        const unsigned int count = dos_output_stage(&unit, &bank, 2, output, sizeof(output), &eoi, &source);
        ck_assert_uint_gt(count, 0);
        for (unsigned int index = 0; index < count; ++index) {
            ck_assert_uint_eq(output[index], 'B');
        }
        dos_output_get(&unit, &bank, models[format], source, count);
    }
    ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_PENDING);
    ck_assert_int_eq(state->rel.record.phase, pattern == 0 ? DOS_REL_RECORD_CLEAN_NEXT : DOS_REL_RECORD_CLEAN_GET);
    ck_assert_uint_eq(state->rel.number, pattern == 0 ? 4 : 3);
    ck_assert(!state->failed && dos_work_pending(&unit));
    ck_assert_int_eq(unit.status_code, DOS_STATUS_RECORD_OVERFLOW);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, 1);
    ck_assert_uint_eq(image.flushes, 0);
    const dos_rel_reader_t before = state->rel;
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->ready && !state->failed && !dos_work_pending(&unit));
    ck_assert_uint_eq(state->rel.number, before.number);
    ck_assert_uint_eq(state->rel.record.pointer, before.record.pointer);
    ck_assert_int_eq(state->rel.record.result, pattern == 0 ? DOS_REL_RECORD_MISSING : DOS_REL_RECORD_READY);
    ck_assert_uint_eq(state->rel.record.next, 94);
    ck_assert_uint_eq(image.reads, reads + 1);
    ck_assert_uint_eq(image.writes, 1);
}
END_TEST

// Dirty NRBUF awaits a reused WRITE page without clean-branch prefetch.
START_TEST(test_owned_nrbuf_dirty_refill) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool next = (unsigned int)_i % 2u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    if (next) {
        memset(image.bytes[3] + 2, 0, DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    }
    image.bytes[5][0] = 1;
    image.bytes[5][1] = 6;
    image.bytes[6][1] = UINT8_MAX;
    memset(image.bytes[6] + 2, 'D', DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_completion_channel(&unit, &bank, &image, format, reference, 26, 10, 4);
    dos_channel_t* const state = &unit.channels[channel];
    ck_assert(dos_rel_channel_begin_write(&unit, channel));
    ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
    ck_assert(dos_rel_channel_pad(&unit, channel));
    finish_pipeline(&unit, &bank, &image);
    ck_assert(dos_rel_channel_finish_cached(&unit, &bank, channel, true));
    ck_assert(dos_rel_channel_resume_read(&unit, &bank, channel));
    const unsigned int reads = image.reads, writes = image.writes;
    unsigned int consumed = 0;
    while (state->ready) {
        uint8_t output[DOS_STAGE_CAPACITY];
        bool eoi;
        const unsigned int count = dos_stage(&unit, &bank, channel, output, sizeof(output), &eoi);
        ck_assert_uint_gt(count, 0);
        for (unsigned int index = 0; index < count; ++index) {
            ck_assert_uint_eq(output[index], 'B');
        }
        dos_get(&unit, &bank, channel, count);
        consumed += count;
        ck_assert_uint_le(consumed, DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    }
    ck_assert(state->rel_write & DOS_REL_WRITE_READBACK);
    ck_assert_int_eq(state->rel.record.phase, next ? DOS_REL_RECORD_DIRTY_NEXT : DOS_REL_RECORD_DIRTY_GET);
    const uint16_t number = state->rel.number;
    const uint8_t pointer = state->rel.record.pointer;
    dos_work(&unit, &bank);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes + 1);
    ck_assert(!state->ready && dos_work_pending(&unit));
    dos_work(&unit, &bank);
    ck_assert_uint_eq(image.reads, reads + 1);
    ck_assert_int_eq(state->rel.record.phase, next ? DOS_REL_RECORD_RESUME_NEXT : DOS_REL_RECORD_RESUME_GET);
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->ready && !state->failed && !state->rel.record.following);
    ck_assert_uint_eq(state->rel.number, number);
    ck_assert_uint_eq(state->rel.record.pointer, pointer);
    ck_assert_uint_eq(image.reads, reads + 1);
    ck_assert_uint_eq(image.writes, writes + 1);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_RECORD_OVERFLOW);
}
END_TEST

// Host refill failures stop polling, retain the consumed cursor and retry only I/O.
START_TEST(test_owned_nrbuf_read_failure_retry) {
    const unsigned int format = (unsigned int)_i / 6u;
    const unsigned int variant = (unsigned int)_i % 6u;
    const bool next = variant % 2u;
    const unsigned int failure = variant / 2u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel =
        prepare_cached_read_return(&unit, &bank, &image, format, reference, next ? 127 : 200, false);
    dos_channel_t* const state = &unit.channels[channel];
    ck_assert(dos_rel_channel_resume_read(&unit, &bank, channel));
    consume_dirty_read_page(&unit, &bank, format, channel);
    dos_work(&unit, &bank);
    while (state->ready) {
        uint8_t output[DOS_STAGE_CAPACITY];
        bool eoi;
        const unsigned int count = dos_stage(&unit, &bank, channel, output, sizeof(output), &eoi);
        ck_assert_uint_gt(count, 0);
        dos_get(&unit, &bank, channel, count);
    }
    const dos_rel_reader_t cursor = state->rel;
    const unsigned int reads = image.reads, writes = image.writes;
    if (failure == 0) {
        image.fail_read = reads + 1;
    } else if (failure == 1) {
        ck_assert_int_eq(dos_media_set_slot(&bank, 3, 1, NULL), DOS_MEDIA_OK);
    } else {
        mount_job_image(&bank, &replacement, format, 3, 1, true);
    }
    dos_work(&unit, &bank);
    ck_assert(state->failed && !state->ready && !dos_work_pending(&unit));
    ck_assert_uint_eq(state->rel.number, cursor.number);
    ck_assert_uint_eq(state->rel.record.pointer, cursor.record.pointer);
    ck_assert_uint_eq(image.reads, reads + (failure == 0));
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(replacement.reads | replacement.writes, 0);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    const dos_unit_t failed = unit;
    dos_work(&unit, &bank);
    ck_assert_mem_eq(&unit, &failed, sizeof(failed));
    ck_assert_int_eq(dos_rel_channel_retry_write(&unit, channel), failure == 0);
    if (failure == 0) {
        finish_pipeline(&unit, &bank, &image);
        ck_assert(state->ready && !state->failed);
        ck_assert_uint_eq(state->rel.number, cursor.number);
        ck_assert_uint_eq(state->rel.record.pointer, cursor.record.pointer);
        ck_assert_uint_eq(image.reads, reads + 2);
        ck_assert_uint_eq(image.writes, writes);
        ck_assert_mem_eq(unit.status, failed.status, sizeof(unit.status));
    }
}
END_TEST

// A failed clean-branch prefetch is deferred until SETLST or NRBUF needs it.
START_TEST(test_owned_nrbuf_deferred_prefetch_failure) {
    const unsigned int format = (unsigned int)_i / 4u;
    const bool next = (unsigned int)_i % 2u;
    const bool remount = (unsigned int)_i % 4u >= 2u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    image.bytes[5][0] = 1;
    image.bytes[5][1] = 6;
    image.bytes[6][1] = UINT8_MAX;
    memset(image.bytes[6] + 2, 'D', DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel =
        prepare_cached_read_return(&unit, &bank, &image, format, reference, next ? 127 : 200, false);
    dos_channel_t* const state = &unit.channels[channel];
    ck_assert(dos_rel_channel_resume_read(&unit, &bank, channel));
    consume_dirty_read_page(&unit, &bank, format, channel);
    dos_work(&unit, &bank);
    while (state->ready) {
        uint8_t output[DOS_STAGE_CAPACITY];
        bool eoi;
        const unsigned int count = dos_stage(&unit, &bank, channel, output, sizeof(output), &eoi);
        ck_assert_uint_gt(count, 0);
        dos_get(&unit, &bank, channel, count);
    }
    const unsigned int reads = image.reads;
    image.fail_read = reads + 2;
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->ready && !state->failed && state->rel.record.following_failed);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_RECORD_OVERFLOW);
    unsigned int consumed = 0;
    while (state->ready) {
        uint8_t output[DOS_STAGE_CAPACITY];
        bool eoi;
        const unsigned int count = dos_stage(&unit, &bank, channel, output, sizeof(output), &eoi);
        ck_assert_uint_gt(count, 0);
        for (unsigned int index = 0; index < count; ++index) {
            ck_assert_uint_eq(output[index], 'C');
        }
        dos_get(&unit, &bank, channel, count);
        consumed += count;
        ck_assert_uint_le(consumed, DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    }
    ck_assert_uint_eq(consumed, next ? DISKIMAGE_SECTOR_PAYLOAD_SIZE : 1);
    ck_assert(state->failed && !dos_work_pending(&unit));
    ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_PENDING);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_uint_eq(image.reads, reads + 2);
    ck_assert_uint_eq(image.writes, 1);
    ck_assert_uint_eq(image.flushes, 0);
    const uint16_t number = state->rel.number;
    ck_assert(dos_rel_channel_retry_write(&unit, channel));
    image.fail_read = image.reads + 1u;
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->failed && !state->ready);
    ck_assert_uint_eq(state->rel.number, number);
    if (remount) {
        mount_job_image(&bank, &replacement, format, 3, 1, true);
        ck_assert(dos_rel_channel_retry_write(&unit, channel));
        finish_pipeline(&unit, &bank, &image);
        ck_assert(state->failed && !state->ready);
        ck_assert_int_eq(state->rel.record.lookup.media_result, DOS_MEDIA_STALE);
        ck_assert_uint_eq(replacement.reads | replacement.writes, 0);
        return;
    }
    ck_assert(dos_rel_channel_retry_write(&unit, channel));
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->ready && !state->failed);
    ck_assert_uint_eq(state->rel.number, number);
    ck_assert_uint_eq(image.reads, reads + 4u);
    const unsigned int length = next ? 127u : 200u;
    for (unsigned int byte = 0; byte < length; ++byte) {
        uint8_t output;
        bool eoi;
        dos_source_t source;
        ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &output, 1, &eoi, &source), 1);
        ck_assert_uint_eq(output, next || byte >= 162u ? 'D' : 'C');
        ck_assert_int_eq(eoi, byte == length - 1u);
        dos_output_get(&unit, &bank, models[format], source, 1);
        finish_pipeline(&unit, &bank, &image);
    }
    ck_assert_uint_eq(state->rel.number, number + 1u);
}
END_TEST

// EOI commits NXTREC once even when an earlier speculative READ is now required.
START_TEST(test_eoi_required_prefetch_retry) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    image.fail_read = 3u + (format == 2);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_completion_channel(&unit, &bank, &image, format, reference, 2, 127, 0);
    dos_channel_t* const state = &unit.channels[channel];
    ck_assert(state->rel.record.following_failed);
    ck_assert(dos_rel_channel_begin_write(&unit, channel));
    ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
    ck_assert(dos_rel_channel_finish_write(&unit, channel, false));
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->failed && !state->ready);
    ck_assert_uint_eq(state->rel.number, 3);
    ck_assert(state->rel_write & DOS_REL_WRITE_EOI);
    ck_assert(dos_rel_channel_retry_write(&unit, channel));
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->ready && !state->failed);
    ck_assert_uint_eq(state->rel.number, 3);
    ck_assert(!(state->rel_write & DOS_REL_WRITE_EOI));
    for (unsigned int byte = 0; byte < 127; ++byte) {
        uint8_t output;
        bool eoi;
        dos_source_t source;
        ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &output, 1, &eoi, &source), 1);
        ck_assert_uint_eq(output, 'B');
        ck_assert_int_eq(eoi, byte == 126);
        dos_output_get(&unit, &bank, models[format], source, 1);
        finish_pipeline(&unit, &bank, &image);
    }
    ck_assert_uint_eq(state->rel.number, 4);
}
END_TEST

// Complete EOI includes queued padding, dirty NRBUF and delayed status.
START_TEST(test_owned_full_eoi_completion) {
    const unsigned int format = (unsigned int)_i / 18u;
    const unsigned int variant = (unsigned int)_i % 18u;
    const unsigned int shape = variant / 3u, completion = variant % 3u;
    const uint8_t sizes[] = {254, 127, 200, 254, 10, 255};
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    if (shape == 3) {
        image.bytes[3][0] = 0;
    }
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint16_t number = shape == 1 ? 2 : shape == 4 ? 26 : 1;
    const uint8_t channel =
        prepare_completion_channel(&unit, &bank, &image, format, reference, number, sizes[shape], shape == 4 ? 4 : 0);
    dos_channel_t* const state = &unit.channels[channel];
    ck_assert(dos_rel_channel_begin_write(&unit, channel));
    ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
    if (completion != 0) {
        ck_assert(dos_rel_channel_pad(&unit, channel));
        finish_pipeline(&unit, &bank, &image);
    } else if (state->rel.record.result != DOS_REL_RECORD_FULL) {
        const dos_unit_t before = unit;
        ck_assert(!dos_rel_channel_finish_write(&unit, channel, true));
        ck_assert_mem_eq(&unit, &before, sizeof(before));
    }
    unit.selected_drive = unit.last_drive = 0;
    dos_status(&unit, models[format], DOS_STATUS_FILE_EXISTS, 0, 0);
    dos_status_get(&unit, models[format], 2);
    char status[DOS_STATUS_LINE_CAPACITY];
    memcpy(status, unit.status, sizeof(status));
    const uint16_t buffers = unit.buffer_use;
    const unsigned int reads = image.reads, writes = image.writes;
    ck_assert(dos_rel_channel_finish_write(&unit, channel, completion == 2));
    ck_assert(!state->ready && dos_work_pending(&unit));
    ck_assert_uint_eq(state->rel.number, number);
    ck_assert_mem_eq(unit.status, status, sizeof(status));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    const dos_unit_t queued = unit;
    ck_assert(!dos_rel_channel_finish_write(&unit, channel, false));
    ck_assert_mem_eq(&unit, &queued, sizeof(queued));
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->ready && !state->failed && !dos_work_pending(&unit));
    ck_assert_uint_eq(state->rel.number, number + 1u);
    ck_assert(!state->rel.no_record);
    ck_assert(!(state->rel_write & (DOS_REL_WRITE_EOI | DOS_REL_WRITE_OVERFLOW | DOS_REL_WRITE_PADDING)));
    ck_assert(state->rel_write & DOS_REL_WRITE_FINISHED);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes + (shape < 2 || (shape == 4 && completion == 0)));
    ck_assert_uint_eq(image.flushes, 0);
    ck_assert_uint_eq(unit.buffer_use, buffers);
    ck_assert_uint_eq(unit.last_drive, 0);
    ck_assert_uint_eq(unit.selected_drive, 1);
    if (completion == 2) {
        ck_assert_str_eq(unit.status,
                         format == 0 ? "51,OVERFLOW IN RECORD,00,00\r" : "51,OVERFLOW IN RECORD,00,00,1\r");
    } else if (format == 0) {
        ck_assert_mem_eq(unit.status, status, sizeof(status));
        ck_assert_uint_eq(unit.status_position, 2);
    } else {
        ck_assert_int_eq(unit.status_code, DOS_STATUS_OK);
    }
    if (shape == 3) {
        ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_MISSING);
        ck_assert_uint_eq(state->rel.record.last, UINT8_MAX);
        ck_assert(state->rel_write & DOS_REL_DIRTY_FIRST);
    }
    ck_assert(!dos_rel_channel_finish_write(&unit, channel, false));
    ck_assert(!dos_rel_channel_resume_read(&unit, &bank, channel));
}
END_TEST

// Failed completion jobs retain EOI/overflow until an explicit successful retry.
START_TEST(test_owned_full_eoi_write_failure_retry) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int failure = (unsigned int)_i % 3u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_completion_channel(&unit, &bank, &image, format, reference, 1, 254, 0);
    dos_channel_t* const state = &unit.channels[channel];
    ck_assert(dos_rel_channel_begin_write(&unit, channel));
    ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
    ck_assert(dos_rel_channel_pad(&unit, channel));
    finish_pipeline(&unit, &bank, &image);
    dos_status(&unit, models[format], DOS_STATUS_FILE_EXISTS, 0, 0);
    ck_assert(dos_rel_channel_finish_write(&unit, channel, true));
    dos_work(&unit, &bank);
    ck_assert_uint_eq(state->rel.number, 2);
    ck_assert(state->rel_write & DOS_REL_WRITE_READBACK);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_FILE_EXISTS);
    if (failure == 0) {
        image.fail_write = 1;
    } else if (failure == 1) {
        unit.bam[1].buffer.bytes[2] = UINT8_MAX;
    } else {
        mount_job_image(&bank, &replacement, format, 3, 1, true);
    }
    const dos_rel_reader_t cursor = state->rel;
    const dos_buffer_t saved = unit.buffers[state->buffers[0]];
    dos_work(&unit, &bank);
    ck_assert(state->failed && !state->ready && !dos_work_pending(&unit));
    ck_assert(state->rel_write & DOS_REL_WRITE_EOI);
    ck_assert(state->rel_write & DOS_REL_WRITE_OVERFLOW);
    ck_assert(state->rel_write & DOS_REL_DIRTY_FIRST);
    ck_assert_mem_eq(&state->rel, &cursor, sizeof(cursor));
    ck_assert_mem_eq(&unit.buffers[state->buffers[0]], &saved, sizeof(saved));
    ck_assert_int_eq(unit.status_code, failure == 1 ? DOS_STATUS_POWER_ON : DOS_STATUS_WRITE_ERROR);
    ck_assert(dos_rel_channel_retry_write(&unit, channel));
    unit.bam[1].buffer.bytes[2] = 0;
    finish_pipeline(&unit, &bank, &image);
    ck_assert_uint_eq(state->rel.number, 2);
    ck_assert_uint_eq(replacement.reads | replacement.writes, 0);
    if (failure < 2) {
        ck_assert(state->ready && !state->failed);
        ck_assert(!(state->rel_write & (DOS_REL_WRITE_EOI | DOS_REL_WRITE_OVERFLOW | DOS_REL_DIRTY_FIRST)));
        ck_assert_str_eq(unit.status,
                         format == 0 ? "51,OVERFLOW IN RECORD,00,00\r" : "51,OVERFLOW IN RECORD,00,00,1\r");
    } else {
        ck_assert(state->failed && !state->ready);
        ck_assert(state->rel_write & DOS_REL_WRITE_EOI);
        ck_assert_mem_eq(&state->rel, &cursor, sizeof(cursor));
    }
}
END_TEST

// A failed EOI owner cannot block or mutate a peer's queued completion.
START_TEST(test_owned_full_eoi_peer_isolation) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t images[2] = {0};
    dos_media_bank_t bank = {0};
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    uint8_t channels[2];
    for (unsigned int owner = 0; owner < count_of(channels); ++owner) {
        mount_job_image(&bank, &images[owner], format, 2, owner, true);
    }
    for (unsigned int owner = 0; owner < count_of(channels); ++owner) {
        dos_media_ref_t reference;
        ck_assert_int_eq(dos_media_reference(&bank, 2, owner, &reference), DOS_MEDIA_OK);
        channels[owner] = prepare_completion_channel(&unit, &bank, &images[owner], format, reference, 1, 254, 0);
        unit.channels[channels[owner]].secondary = 4u + owner;
        ck_assert(dos_rel_channel_begin_write(&unit, channels[owner]));
        ck_assert(dos_rel_channel_put(&unit, &bank, channels[owner], 'Z'));
        ck_assert(dos_rel_channel_pad(&unit, channels[owner]));
        finish_pipeline(&unit, &bank, &images[owner]);
    }
    images[0].fail_write = 1;
    ck_assert(dos_rel_channel_finish_write(&unit, channels[0], true));
    ck_assert(dos_rel_channel_finish_write(&unit, channels[1], false));
    for (unsigned int step = 0; !unit.channels[channels[0]].failed; ++step) {
        ck_assert_uint_lt(step, 8);
        dos_work(&unit, &bank);
    }
    const dos_channel_t failed = unit.channels[channels[0]];
    const dos_buffer_t saved = unit.buffers[failed.buffers[0]];
    const uint16_t buffers = unit.buffer_use;
    finish_pipeline(&unit, &bank, &images[1]);
    ck_assert_mem_eq(&unit.channels[channels[0]], &failed, sizeof(failed));
    ck_assert_mem_eq(&unit.buffers[failed.buffers[0]], &saved, sizeof(saved));
    ck_assert(unit.channels[channels[1]].ready && !unit.channels[channels[1]].failed);
    ck_assert_uint_eq(unit.channels[channels[1]].rel.number, 2);
    ck_assert_uint_eq(images[0].writes, 1);
    ck_assert_uint_eq(images[1].writes, 1);
    ck_assert_uint_eq(unit.buffer_use, buffers);
    ck_assert(dos_rel_channel_retry_write(&unit, channels[0]));
    finish_pipeline(&unit, &bank, &images[0]);
    ck_assert(unit.channels[channels[0]].ready && !unit.channels[channels[0]].failed);
    ck_assert_uint_eq(unit.channels[channels[0]].rel.number, 2);
    ck_assert_uint_eq(images[0].writes, 2);
    ck_assert_uint_eq(images[1].writes, 1);
    ck_assert_uint_eq(images[0].flushes | images[1].flushes, 0);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_RECORD_OVERFLOW);
}
END_TEST

// Cached SETLST requires completed same-drive pages and a current media token.
START_TEST(test_cached_next_window_guards) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int variant = (unsigned int)_i % 3u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_cached_read_return(&unit, &bank, &image, format, reference, 200, false);
    dos_channel_t* const state = &unit.channels[channel];
    ck_assert(dos_rel_channel_resume_read(&unit, &bank, channel));
    consume_dirty_read_page(&unit, &bank, format, channel);
    dos_work(&unit, &bank);
    const uint8_t inactive_index = state->buffers[state->rel.record.active ^ 1u];
    if (variant == 0) {
        unit.buffers[inactive_index].resident = false;
    } else if (variant == 1) {
        unit.buffers[inactive_index].failed = true;
    } else {
        unit.buffer_drives ^= (uint16_t)1u << inactive_index;
    }
    const unsigned int reads = image.reads, writes = image.writes;
    while (state->ready) {
        uint8_t output[DOS_STAGE_CAPACITY];
        bool eoi;
        dos_source_t source;
        const unsigned int count = dos_output_stage(&unit, &bank, 2, output, sizeof(output), &eoi, &source);
        ck_assert_uint_gt(count, 0);
        dos_output_get(&unit, &bank, models[format], source, count);
    }
    ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_PENDING);
    ck_assert_int_eq(state->rel.record.phase, DOS_REL_RECORD_FOLLOWING);
    ck_assert_uint_eq(state->rel.number, 3);
    ck_assert(!state->failed && !dos_work_pending(&unit));
    const dos_unit_t boundary = unit;
    dos_work(&unit, &bank);
    ck_assert_mem_eq(&unit, &boundary, sizeof(boundary));
    if (variant == 0) {
        state->rel.record.phase = DOS_REL_RECORD_DATA;
        const dos_rel_record_t before = state->rel.record;
        ck_assert(!dos_rel_record_prepare_cached(&state->rel.record, &bank, unit.buffers[state->buffers[0]].bytes,
                                                 unit.buffers[state->buffers[1]].bytes));
        ck_assert_mem_eq(&state->rel.record, &before, sizeof(before));
    } else {
        if (variant == 1) {
            mount_job_image(&bank, &replacement, format, 3, 1, true);
        } else {
            ck_assert_int_eq(dos_media_set_slot(&bank, 3, 1, NULL), DOS_MEDIA_OK);
        }
        ck_assert(!dos_rel_record_prepare_cached(&state->rel.record, &bank, unit.buffers[state->buffers[0]].bytes,
                                                 unit.buffers[state->buffers[1]].bytes));
        ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_FAILED);
        ck_assert_int_eq(state->rel.record.lookup.media_result, DOS_MEDIA_STALE);
        ck_assert_uint_eq(state->rel.number, boundary.channels[channel].rel.number);
    }
    ck_assert_mem_eq(unit.buffers, boundary.buffers, sizeof(unit.buffers));
    ck_assert_uint_eq(state->rel_write, boundary.channels[channel].rel_write);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(replacement.reads | replacement.writes, 0);
}
END_TEST

// Register REL indexing independently of full record-channel implementation.

// Dirty indexed positioning retains page bits through lookup and retry.
START_TEST(test_pipeline_position_resident_index) {
    const unsigned int format = (unsigned int)_i / 8u;
    const unsigned int variant = (unsigned int)_i % 8u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, 0);
    dos_channel_t* const state = &unit.channels[channel];
    dos_buffer_t* const side = &unit.buffers[state->buffers[2]];
    image.bytes[0][4] = 1;
    image.bytes[0][5] = 0;
    image.bytes[0][1] = UINT8_MAX;
    image.bytes[1][1] = UINT8_MAX;
    image.bytes[1][2] = variant == 2 ? 0 : 1;
    image.bytes[1][6] = 1;
    image.bytes[1][7] = 1;
    image.bytes[1][16] = 1;
    image.bytes[1][17] = 4;
    uint16_t number = 1;
    if (variant == 1 || variant == 2) {
        number = 154;
        image.bytes[0][6] = 1;
        image.bytes[0][7] = 1;
    } else if (variant >= 3) {
        image.bytes[0][16] = 0;
        if (variant == 4) {
            number = 916;
        } else if (variant == 5) {
            memset(image.bytes[0] + 4, 0, DOS_REL_SIDE_COUNT * 2u);
        } else if (variant == 6) {
            image.bytes[0][6] = 1;
            image.bytes[0][7] = 1;
        } else if (variant == 7) {
            image.bytes[0][0] = 1;
        }
    }
    memcpy(side->bytes, image.bytes[0], sizeof(side->bytes));
    ck_assert(side->resident && !side->failed);
    ck_assert_uint_eq(side->track, 1);
    ck_assert_uint_eq(side->sector, 0);
    if (format == 2) {
        ck_assert_uint_eq(state->rel.record.cached_group, 0);
    }
    ck_assert(dos_rel_channel_begin_write(&unit, channel));
    ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
    const dos_rel_record_t before = state->rel.record;
    const uint8_t flags = state->rel_write;
    dos_status(&unit, models[format], DOS_STATUS_FILE_EXISTS, 0, 0);
    const unsigned int reads = image.reads, writes = image.writes;
    ck_assert(dos_rel_channel_position(&unit, channel, number, 0));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert(!state->ready && dos_work_pending(&unit));
    finish_pipeline(&unit, &bank, &image);
    ck_assert_uint_eq(state->rel.number, number);
    ck_assert(!state->rel.no_record);
    ck_assert_uint_eq(image.writes, writes + (variant == 1 || variant == 2));
    ck_assert_uint_eq(state->rel_write & DOS_REL_WRITE_MODIFIED, flags & DOS_REL_WRITE_MODIFIED);
    ck_assert_uint_eq(image.flushes, 0);
    const unsigned int extra_reads = variant == 1 || variant == 2 ? 2 : variant == 6 ? 1 : 0;
    ck_assert_uint_eq(image.reads, reads + extra_reads + (format == 2 && variant >= 3 ? 2 : 0));
    if (variant == 5 || variant == 7) {
        ck_assert(state->failed && !state->ready);
        ck_assert_int_eq(unit.status_code, DOS_STATUS_ILLEGAL_SYSTEM_TRACK_SECTOR);
        ck_assert_str_eq(unit.status,
                         format == 0 ? "67,ILLEGAL TRACK OR SECTOR,00,00\r" : "67,ILLEGAL TRACK OR SECTOR,00,00,1\r");
        ck_assert_uint_eq(state->rel.record.pointer, before.pointer);
        return;
    }
    ck_assert(state->ready && !state->failed);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_FILE_EXISTS);
    uint8_t output = 0;
    bool eoi = false;
    ck_assert_uint_eq(dos_stage(&unit, &bank, channel, &output, 1, &eoi), 1);
    if (variant < 3) {
        ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_READY);
        ck_assert_uint_eq(output, variant == 0 ? 'Z' : 'B');
        ck_assert_uint_eq(state->rel.record.pointer, variant == 0 ? 2 : 122);
        ck_assert_uint_eq(side->bytes[2], variant == 1 ? 1 : 0);
    } else {
        ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_MISSING);
        ck_assert_uint_eq(state->rel.record.pointer, before.pointer);
        ck_assert_uint_eq(state->rel.record.next, before.next);
        ck_assert_uint_eq(state->rel_write & DOS_REL_DIRTY_FIRST, DOS_REL_DIRTY_FIRST);
        ck_assert_uint_eq(state->rel.record.lookup.address.index, 254);
        ck_assert_uint_eq(state->rel.record.lookup.address.side, variant == 6 ? 1 : 0);
        ck_assert_uint_eq(output, '\r');
        ck_assert(eoi);
        ck_assert_uint_eq(unit.buffers[state->buffers[0]].bytes[2], 'Z');
        ck_assert_uint_eq(unit.buffers[state->buffers[0]].bytes[3], 'A');
        dos_get(&unit, &bank, channel, 1);
        ck_assert_int_eq(unit.status_code, DOS_STATUS_NO_RECORD);
        ck_assert_uint_eq(state->rel.number, number);
        ck_assert(state->rel.no_record);
    }
}
END_TEST

// Failed side jobs retain their precise retry phase and never poll new media.
START_TEST(test_pipeline_position_index_failure) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int failure = (unsigned int)_i % 3u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, 0);
    dos_channel_t* const state = &unit.channels[channel];
    image.bytes[0][6] = 1;
    image.bytes[0][7] = 1;
    image.bytes[1][1] = UINT8_MAX;
    image.bytes[1][2] = 1;
    image.bytes[1][16] = 1;
    image.bytes[1][17] = 4;
    memcpy(unit.buffers[state->buffers[2]].bytes, image.bytes[0], DISKIMAGE_SECTOR_SIZE);
    if (failure == 1 && format == 2) {
        state->rel.record.cached_group = DOS_REL_GROUP_NONE;
    }
    ck_assert(dos_rel_channel_begin_write(&unit, channel));
    ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
    const uint8_t flags = state->rel_write;
    ck_assert(dos_rel_channel_position(&unit, channel, 154, 0));
    const unsigned int reads = image.reads, writes = image.writes;
    if (failure == 2) {
        mount_job_image(&bank, &replacement, format, 3, 1, true);
    } else {
        image.fail_read = reads + 1u;
    }
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->failed && !state->ready);
    ck_assert_uint_eq(state->rel_write, flags | DOS_REL_WRITE_FINISHED);
    ck_assert_uint_eq(unit.buffers[state->buffers[0]].bytes[2], 'Z');
    ck_assert_uint_eq(image.reads, reads + (failure != 2));
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(replacement.reads | replacement.writes, 0);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    const dos_unit_t failed = unit;
    dos_work(&unit, &bank);
    ck_assert_mem_eq(&unit, &failed, sizeof(failed));
    ck_assert_int_eq(dos_rel_channel_retry_position(&unit, channel), failure != 2);
    if (failure != 2) {
        finish_pipeline(&unit, &bank, &image);
        ck_assert(state->ready && !state->failed);
        ck_assert_uint_eq(state->rel.number, 154);
        ck_assert_uint_eq(state->rel.record.pointer, 122);
        ck_assert_uint_eq(image.writes, writes + 1u);
        ck_assert_uint_eq(image.bytes[3][2], 'Z');
        ck_assert_mem_eq(unit.status, failed.status, sizeof(unit.status));
    }
}
END_TEST

// Seed INTTS's already-allocated first sector and a native track-one BAM.
static dos_media_ref_t prepare_create_image(dos_unit_t* const unit, dos_media_bank_t* const bank,
                                            rel_image_t* const image, const unsigned int format,
                                            const unsigned int slot) {
    const dos_media_ref_t reference = mount_job_image(bank, image, format, 3, slot, true);
    memset(image->bytes, 0, sizeof(image->bytes));
    const diskimage_type_t type = bank->units[3].slots[slot].io.image.type;
    ck_assert(diskimage_sector_offset(type, 1, format == 0 ? 10 : 5, &image->offsets[1]));
    ck_assert(diskimage_sector_offset(type, 1, 6, &image->offsets[2]));
    ck_assert(diskimage_sector_offset(type, format == 0 ? 18 : 39, 6, &image->offsets[5]));
    ck_assert(diskimage_sector_offset(type, format == 0 ? 18 : 38, 0, &image->offsets[6]));
    dos_bam_t* const bam = &unit->bam[slot];
    *bam = (dos_bam_t){
        .media = reference,
        .buffer = {.track = format == 0 ? 18 : 38, .resident = true},
        .dirty = true,
    };
    bam->buffer.bytes[0] = format == 0 ? 18 : 38;
    bam->buffer.bytes[1] = format == 0 ? 1 : 3;
    bam->buffer.bytes[2] = format == 0 ? 'A' : 'C';
    if (format != 0) {
        bam->buffer.bytes[4] = 1;
        bam->buffer.bytes[5] = 51;
    }
    unsigned int sectors;
    ck_assert(diskimage_track_sectors(type, 1, &sectors));
    uint8_t* const entry = bam->buffer.bytes + (format == 0 ? 4u : 6u);
    entry[0] = sectors - 1u;
    for (unsigned int sector = 1; sector < sectors; ++sector) {
        entry[1u + sector / 8u] |= 1u << (sector % 8u);
    }
    unit->free_blocks[slot] = sectors - 1u;
    unit->free_blocks_valid[slot] = true;
    return reference;
}

// Supply persistent metadata and an SSEND endpoint independent of data links.
static void prepare_close_metadata(dos_unit_t* const unit, const uint8_t channel, rel_image_t* const image) {
    dos_channel_t* const state = &unit->channels[channel];
    state->directory_track = 1;
    state->directory_sector = 6;
    state->directory_entry = DISKIMAGE_DIRECTORY_ENTRY_SIZE;
    state->blocks = 4321;
    image->bytes[0][1] = UINT8_MAX;
    image->bytes[0][4] = 1;
    image->bytes[0][5] = 0;
    memcpy(unit->buffers[state->buffers[2]].bytes, image->bytes[0], DISKIMAGE_SECTOR_SIZE);
    memset(image->bytes[6], 0x5a, DISKIMAGE_SECTOR_SIZE);
    image->bytes[6][state->directory_entry + 2u] = DISKIMAGE_FTYPE_REL | 0x40u;
}

typedef struct {
    uint8_t bytes[DISKIMAGE_D82_SIZE];
    unsigned int reads, writes, flushes;
    unsigned int fail_read, fail_write;
} rel_growth_image_t;

// Supply a complete native image, including allocated but unwritten prefetches.
static bool read_growth(void* const context, const uint32_t offset, void* const output, const size_t length) {
    rel_growth_image_t* const image = context;
    ck_assert_uint_eq(length, DISKIMAGE_SECTOR_SIZE);
    ck_assert_uint_le(offset + length, sizeof(image->bytes));
    ++image->reads;
    const bool failed = image->reads == image->fail_read;
    memcpy(output, image->bytes + offset, failed ? length / 2u : length);
    if (failed) {
        memset((uint8_t*)output + length / 2u, 0xee, length / 2u);
    }
    return !failed;
}

// Retain partially completed backing writes so retry must repair real damage.
static bool write_growth(void* const context, const uint32_t offset, const void* const input, const size_t length) {
    rel_growth_image_t* const image = context;
    ck_assert_uint_eq(length, DISKIMAGE_SECTOR_SIZE);
    ck_assert_uint_le(offset + length, sizeof(image->bytes));
    ++image->writes;
    const bool failed = image->writes == image->fail_write;
    memcpy(image->bytes + offset, input, failed ? length / 2u : length);
    return !failed;
}

// Keep native MAPOUT completion separate from host durability.
static bool flush_growth(void* const context) {
    ++((rel_growth_image_t*)context)->flushes;
    return true;
}

// Complete a finite multi-side operation with bounded map-part traversal.
static void finish_growth(dos_unit_t* const unit, dos_media_bank_t* const bank, rel_growth_image_t* const image) {
    for (unsigned int step = 0; dos_work_pending(unit); ++step) {
        ck_assert_uint_lt(step, 32768);
        const unsigned int callbacks = image->reads + image->writes;
        dos_work(unit, bank);
        ck_assert_uint_le(image->reads + image->writes - callbacks, 5);
    }
}

// Construct valid native maps after INTTS has allocated the first data sector.
static uint8_t prepare_growth(dos_unit_t* const unit, dos_media_bank_t* const bank, rel_growth_image_t* const image,
                              const unsigned int format, const uint8_t size) {
    memset(image, 0, sizeof(*image));
    memset(image->bytes, 0x5a, sizeof(image->bytes));
    dos_media_io_t io = {.flush = flush_growth};
    ck_assert(diskimage_open(&io.image, read_growth, image, sizes[format]));
    io.image.write = write_growth;
    dos_reset(unit, models[format]);
    ck_assert_int_eq(dos_media_set_slot(bank, 3, 1, &io), DOS_MEDIA_OK);
    dos_media_ref_t reference;
    ck_assert_int_eq(dos_media_reference(bank, 3, 1, &reference), DOS_MEDIA_OK);
    const unsigned int tracks = format == 0 ? 35 : format == 1 ? 77 : 154;
    const unsigned int parts = format == 0 ? 1 : (tracks + 49u) / 50u;
    const uint8_t directory = format == 0 ? 18 : 39, bam_track = format == 0 ? 18 : 38;
    uint32_t first_map;
    ck_assert(diskimage_sector_offset(io.image.type, bam_track, 0, &first_map));
    for (unsigned int part = 0; part < parts; ++part) {
        uint32_t offset;
        ck_assert(diskimage_sector_offset(io.image.type, bam_track, part * 3u, &offset));
        uint8_t* const map = image->bytes + offset;
        memset(map, 0, DISKIMAGE_SECTOR_SIZE);
        map[0] = part + 1u < parts ? bam_track : directory;
        map[1] = part + 1u < parts ? (part + 1u) * 3u : 1;
        map[2] = format == 0 ? 'A' : 'C';
        const unsigned int low = format == 0 ? 1 : part * 50u + 1u;
        const unsigned int high = format == 0 ? tracks + 1u : MIN((part + 1u) * 50u + 1u, tracks + 1u);
        if (format != 0) {
            map[4] = low;
            map[5] = high;
        }
        for (unsigned int track = low; track < high; ++track) {
            unsigned int sectors;
            ck_assert(diskimage_track_sectors(io.image.type, track, &sectors));
            uint8_t* const entry = map + (format == 0 ? track * 4u : 6u + (track - low) * 5u);
            for (unsigned int sector = 0; sector < sectors; ++sector) {
                if ((track == 1 && sector == 0) || (track == directory && sector < 2) ||
                    (format != 0 && track == bam_track && sector % 3u == 0 && sector / 3u < parts)) {
                    continue;
                }
                ++entry[0];
                entry[1u + sector / 8u] |= 1u << (sector % 8u);
            }
            if (track != directory) {
                unit->free_blocks[1] += entry[0];
            }
        }
    }
    unit->free_blocks_valid[1] = true;
    unit->bam[1] = (dos_bam_t){
        .media = reference,
        .buffer = {.track = bam_track, .resident = true},
        .dirty = true,
    };
    memcpy(unit->bam[1].buffer.bytes, image->bytes + first_map, DISKIMAGE_SECTOR_SIZE);
    const uint8_t channel = dos_rel_channel_claim(unit, reference, 2);
    dos_rel_channel_create(unit, channel, models[format], 1, 0, size);
    finish_growth(unit, bank, image);
    ck_assert(!unit->channels[channel].failed);
    return channel;
}

// Reach ADDREL through real missing-record lookup, not a forged growth cursor.
static void start_growth(dos_unit_t* const unit, dos_media_bank_t* const bank, rel_growth_image_t* const image,
                         const uint8_t channel, const uint16_t number) {
    ck_assert(dos_rel_channel_position(unit, channel, number, 0));
    finish_growth(unit, bank, image);
    ck_assert_int_eq(unit->channels[channel].rel.record.result, DOS_REL_RECORD_MISSING);
    ck_assert(dos_rel_channel_grow(unit, channel));
}

// OW20 publishes side/super/data/BAM in order, then OROW prepares native bytes.
START_TEST(test_pipeline_create_native_pages) {
    static const uint8_t record_sizes[] = {1, 2, 10, 127, 128, 200, 253, 254, 255};
    static const uint8_t last_bytes[] = {255, 255, 251, 255, 129, 201, 254, 255, 1};
    const unsigned int format = (unsigned int)_i / (2u * count_of(record_sizes));
    const unsigned int variant = (unsigned int)_i / 2u % count_of(record_sizes), slot = (unsigned int)_i % 2u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const dos_media_ref_t reference = prepare_create_image(&unit, &bank, &image, format, slot);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, 2);
    dos_channel_t* const state = &unit.channels[channel];
    dos_rel_channel_create(&unit, channel, models[format], 1, 0, record_sizes[variant]);
    ck_assert(!state->ready && dos_work_pending(&unit));
    ck_assert_uint_eq(image.reads | image.writes | image.flushes, 0);
    ck_assert(!dos_close(&unit, &bank, models[format], 2));
    ck_assert(!dos_finalize(&unit, &bank));
    ck_assert(!dos_rel_channel_close(&unit, channel));
    ck_assert(!dos_rel_channel_position(&unit, channel, 1, 0));
    ck_assert(!dos_rel_channel_begin_write(&unit, channel));
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->ready && !state->failed);
    ck_assert_int_eq(state->rel.record.result, variant == 8 ? DOS_REL_RECORD_MISSING : DOS_REL_RECORD_READY);
    ck_assert_uint_eq(state->rel.number, 1);
    ck_assert_uint_eq(state->rel.record.active, 0);
    ck_assert_uint_eq(state->rel_write, 0);
    ck_assert_uint_eq(image.reads | image.flushes, 0);
    ck_assert_uint_eq(image.writes, format == 2 ? 4 : 3);
    ck_assert_uint_eq(state->rel_root_track, 1);
    ck_assert_uint_eq(state->rel_root_sector, format == 2 ? 6 : format == 0 ? 10 : 5);
    uint8_t expected_side[DISKIMAGE_SECTOR_SIZE] = {0, 17, 0, record_sizes[variant], 1, format == 0 ? 10 : 5};
    expected_side[16] = 1;
    ck_assert_mem_eq(image.bytes[1], expected_side, sizeof(expected_side));
    if (format == 2) {
        uint8_t expected_super[DISKIMAGE_SECTOR_SIZE] = {1, 5, 254, 1, 5};
        ck_assert_mem_eq(image.bytes[2], expected_super, sizeof(expected_super));
        ck_assert_uint_eq(state->rel.record.cached_group, DOS_REL_GROUP_SUPER);
        ck_assert_mem_eq(unit.buffers[state->buffers[2]].bytes, expected_super, sizeof(expected_super));
    } else {
        ck_assert_mem_eq(unit.buffers[state->buffers[2]].bytes, expected_side, sizeof(expected_side));
    }
    ck_assert_uint_eq(image.bytes[0][0], 0);
    ck_assert_uint_eq(image.bytes[0][1], last_bytes[variant]);
    ck_assert_uint_eq(image.bytes[0][2], UINT8_MAX);
    ck_assert_mem_eq(image.bytes[6], unit.bam[slot].buffer.bytes, DISKIMAGE_SECTOR_SIZE);
    ck_assert(!unit.bam[slot].dirty);
    unsigned int sectors;
    ck_assert(diskimage_track_sectors(bank.units[3].slots[slot].io.image.type, 1, &sectors));
    ck_assert_uint_eq(unit.free_blocks[slot], sectors - (format == 2 ? 3u : 2u));
    uint8_t output;
    bool eoi;
    dos_source_t source;
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, &output, 1, &eoi, &source), 1);
    ck_assert_uint_eq(output, variant == 8 ? '\r' : UINT8_MAX);
    ck_assert(eoi);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_POWER_ON);

    // ADDFIL is still a separate boundary. Seed its already-closed REL entry
    // to check that DOS 2.7's no-DYFILE CLOSE still performs the host flush.
    state->directory_track = format == 0 ? 18 : 39;
    state->directory_sector = 6;
    state->directory_entry = DISKIMAGE_DIRECTORY_ENTRY_SIZE;
    uint8_t* const entry = image.bytes[5] + state->directory_entry;
    entry[2] = DISKIMAGE_FTYPE_REL | 0x80u;
    entry[3] = 1;
    entry[21] = state->rel_root_track;
    entry[22] = state->rel_root_sector;
    entry[23] = record_sizes[variant];
    ck_assert(dos_rel_channel_close(&unit, channel));
    finish_pipeline(&unit, &bank, &image);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(image.writes, (format == 2 ? 4u : 3u) + (format == 0));
    ck_assert_uint_eq(image.flushes, 1);
    ck_assert_uint_eq(entry[30], format == 0 ? 2 : 0);
    ck_assert_uint_eq(entry[31], 0);
}
END_TEST

// Failed side/super/data/MAPOUT jobs retain the exact allocation for retry.
START_TEST(test_pipeline_create_failure_retry) {
    const unsigned int format = (unsigned int)_i / 5u, failure = (unsigned int)_i % 5u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const dos_media_ref_t reference = prepare_create_image(&unit, &bank, &image, format, 1);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, 2);
    dos_channel_t* const state = &unit.channels[channel];
    image.fail_write = failure == 3 ? (format == 2 ? 4 : 3) : failure + 1u;
    if (failure == 4) {
        unit.bam[1].buffer.bytes[2] = 'X';
        image.fail_write = 0;
    }
    dos_rel_channel_create(&unit, channel, models[format], 1, 0, 200);
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->failed && !state->ready && state->allocated);
    ck_assert_int_eq(unit.status_code, failure == 4 ? DOS_STATUS_POWER_ON : DOS_STATUS_WRITE_ERROR);
    const dos_unit_t failed = unit;
    dos_work(&unit, &bank);
    ck_assert_mem_eq(&unit, &failed, sizeof(unit));
    ck_assert(!dos_close(&unit, &bank, models[format], 2));
    ck_assert(!dos_finalize(&unit, &bank));
    ck_assert(!dos_rel_channel_retry_close(&unit, channel));
    ck_assert(!dos_rel_channel_retry_position(&unit, channel));
    ck_assert(!dos_rel_channel_retry_write(&unit, channel));
    const unsigned int writes = image.writes;
    unit.bam[1].buffer.bytes[2] = format == 0 ? 'A' : 'C';
    ck_assert(dos_rel_channel_retry_create(&unit, channel));
    finish_pipeline(&unit, &bank, &image);
    ck_assert(state->ready && !state->failed);
    ck_assert_uint_eq(state->rel_root_sector, format == 2 ? 6 : format == 0 ? 10 : 5);
    ck_assert_uint_eq(unit.buffer_use, failed.buffer_use);
    ck_assert_uint_eq(image.writes, (format == 2 ? 4u : 3u) + (failure != 4));
    ck_assert_uint_ge(image.writes, writes);
    ck_assert_mem_eq(unit.status, failed.status, sizeof(unit.status));
    ck_assert_uint_eq(image.flushes, 0);
    ck_assert_uint_eq(image.reads, 0);
    ck_assert_uint_eq(image.bytes[0][1], 201);
}
END_TEST

// Rebinding at each creation boundary never allocates or writes on the new disk.
START_TEST(test_pipeline_create_replaced_media) {
    const unsigned int format = (unsigned int)_i / 4u, variant = (unsigned int)_i % 4u;
    const uint8_t phases[] = {DOS_REL_RECORD_CREATE_SIDE, DOS_REL_RECORD_CREATE_SIDE_WRITE, DOS_REL_RECORD_CREATE_MAP,
                              DOS_REL_RECORD_CREATE_READY};
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const dos_media_ref_t reference = prepare_create_image(&unit, &bank, &image, format, 1);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, 2);
    dos_channel_t* const state = &unit.channels[channel];
    dos_rel_channel_create(&unit, channel, models[format], 1, 0, 200);
    for (unsigned int step = 0; state->rel.record.phase != phases[variant]; ++step) {
        ck_assert_uint_lt(step, 12);
        dos_work(&unit, &bank);
        ck_assert(!state->failed);
    }
    mount_job_image(&bank, &replacement, format, 3, 1, true);
    const unsigned int reads = image.reads, writes = image.writes, flushes = image.flushes;
    const uint16_t use = unit.buffer_use;
    const dos_bam_t bam = unit.bam[1];
    dos_work(&unit, &bank);
    ck_assert(state->failed && !state->ready && state->allocated);
    ck_assert_int_eq(state->rel.record.lookup.media_result, DOS_MEDIA_STALE);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(unit.buffer_use, use);
    ck_assert_mem_eq(&unit.bam[1], &bam, sizeof(bam));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(image.flushes, flushes);
    ck_assert_uint_eq(replacement.reads | replacement.writes | replacement.flushes, 0);
    ck_assert(!dos_work_pending(&unit));
    ck_assert(!dos_rel_channel_retry_create(&unit, channel));
    ck_assert(!dos_close(&unit, &bank, models[format], 2));
    ck_assert(!dos_finalize(&unit, &bank));
}
END_TEST

// Allocate, publish and reposition across data, side and super-group seams.
START_TEST(test_growth_native_pages) {
    const unsigned int format = (unsigned int)_i / 4u, variant = (unsigned int)_i % 4u;
    const uint16_t number = variant == 0 ? 3 : variant == 1 ? 121 : variant == 2 ? 241 : format == 2 ? 721 : 601;
    rel_growth_image_t* const image = malloc(sizeof(*image));
    ck_assert_ptr_nonnull(image);
    dos_media_bank_t bank = {0};
    dos_unit_t unit;
    const uint8_t channel = prepare_growth(&unit, &bank, image, format, variant == 0 ? 127 : 254);
    dos_channel_t* const state = &unit.channels[channel];
    const uint16_t use = unit.buffer_use;
    start_growth(&unit, &bank, image, channel, number);
    ck_assert(!state->ready);
    ck_assert(!dos_close(&unit, &bank, models[format], 2));
    ck_assert(!dos_finalize(&unit, &bank));
    ck_assert(!dos_rel_channel_position(&unit, channel, 1, 0));
    finish_growth(&unit, &bank, image);
    ck_assert(!state->failed && !state->ready);
    ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_READY);
    ck_assert_uint_eq(state->rel.number, number);
    ck_assert_uint_eq(unit.buffer_use, use);
    ck_assert_uint_eq(image->flushes, 0);
    uint8_t byte;
    bool eoi;
    ck_assert(dos_rel_record_peek(&state->rel.record, &bank, unit.buffers[state->buffers[0]].bytes,
                                  unit.buffers[state->buffers[1]].bytes, &byte, &eoi));
    ck_assert_uint_eq(byte, UINT8_MAX);
    ck_assert(eoi);
    ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
    finish_growth(&unit, &bank, image);
    const dos_buffer_t* const active = &unit.buffers[state->buffers[state->rel.record.active]];
    ck_assert_uint_eq(active->bytes[DISKIMAGE_SECTOR_LINK_SIZE], 'Z');
    free(image);
}
END_TEST

// Every retained persistence phase repairs a partial failure without reallocation.
START_TEST(test_growth_partial_failure_retry) {
    static const uint8_t phases[] = {
        DOS_GROW_REL_OLD_WRITE,     DOS_GROW_REL_DATA_WRITE,  DOS_GROW_REL_SIDE_WRITE, DOS_GROW_REL_SIDE_OLD_WRITE,
        DOS_GROW_REL_SIDE_PREVIOUS, DOS_GROW_REL_INDEX_WRITE, DOS_GROW_REL_MAP,        DOS_GROW_REL_SUPER_READ,
        DOS_GROW_REL_SUPER_WRITE,   DOS_GROW_REL_GROUP_READ,
    };
    const unsigned int format = _i < 21 ? (unsigned int)_i / 7u : 2;
    const unsigned int variant = _i < 21 ? (unsigned int)_i % 7u : (unsigned int)_i - 14u;
    rel_growth_image_t *const image = malloc(sizeof(*image)), *const expected = malloc(sizeof(*expected));
    ck_assert_ptr_nonnull(image);
    ck_assert_ptr_nonnull(expected);
    dos_media_bank_t bank = {0}, expected_bank = {0};
    dos_unit_t unit, expected_unit;
    const uint8_t channel = prepare_growth(&unit, &bank, image, format, 254);
    const uint8_t expected_channel = prepare_growth(&expected_unit, &expected_bank, expected, format, 254);
    const uint16_t number = variant >= 7 ? 721 : 241;
    start_growth(&expected_unit, &expected_bank, expected, expected_channel, number);
    finish_growth(&expected_unit, &expected_bank, expected);
    ck_assert(!expected_unit.channels[expected_channel].failed);
    start_growth(&unit, &bank, image, channel, number);
    dos_channel_t* const state = &unit.channels[channel];
    for (unsigned int step = 0; state->closing != phases[variant]; ++step) {
        ck_assert_uint_lt(step, 16384);
        dos_work(&unit, &bank);
        ck_assert(!state->failed);
    }
    const bool reading = variant == 4 || variant == 7 || variant == 9;
    if (reading) {
        image->fail_read = image->reads + 1u;
    } else {
        image->fail_write = image->writes + 1u;
    }
    finish_growth(&unit, &bank, image);
    ck_assert(state->failed && !state->ready && state->allocated);
    ck_assert_int_eq(unit.status_code, reading ? DOS_STATUS_READ_ERROR : DOS_STATUS_WRITE_ERROR);
    const dos_unit_t retained = unit;
    dos_work(&unit, &bank);
    ck_assert_mem_eq(&unit, &retained, sizeof(unit));
    ck_assert(!dos_rel_channel_retry_position(&unit, channel));
    ck_assert(!dos_rel_channel_retry_create(&unit, channel));
    ck_assert(!dos_rel_channel_retry_close(&unit, channel));
    ck_assert(!dos_rel_channel_retry_write(&unit, channel));
    ck_assert(!dos_close(&unit, &bank, models[format], 2));
    ck_assert(!dos_finalize(&unit, &bank));
    ck_assert(dos_rel_channel_retry_grow(&unit, channel));
    finish_growth(&unit, &bank, image);
    ck_assert(!state->failed && !state->ready);
    ck_assert_uint_eq(unit.free_blocks[1], expected_unit.free_blocks[1]);
    ck_assert_uint_eq(unit.buffer_use, expected_unit.buffer_use);
    ck_assert_mem_eq(state->buffers, expected_unit.channels[expected_channel].buffers, sizeof(state->buffers));
    ck_assert_mem_eq(unit.buffers, expected_unit.buffers, sizeof(unit.buffers));
    ck_assert_mem_eq(image->bytes, expected->bytes, sizes[format]);
    ck_assert_uint_eq(image->flushes, 0);
    free(expected);
    free(image);
}
END_TEST

// Capacity rejection publishes 52 before allocating, and retry resumes preflight.
START_TEST(test_growth_capacity_preflight) {
    const unsigned int format = (unsigned int)_i;
    rel_growth_image_t* const image = malloc(sizeof(*image));
    ck_assert_ptr_nonnull(image);
    dos_media_bank_t bank = {0};
    dos_unit_t unit;
    const uint8_t channel = prepare_growth(&unit, &bank, image, format, 127);
    start_growth(&unit, &bank, image, channel, 3);
    const unsigned int available = unit.free_blocks[1], writes = image->writes;
    const dos_bam_t bam = unit.bam[1];
    unit.free_blocks[1] = 0;
    finish_growth(&unit, &bank, image);
    ck_assert(unit.channels[channel].failed && !unit.channels[channel].ready);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_FILE_TOO_LARGE);
    ck_assert_uint_eq(image->writes, writes);
    ck_assert_mem_eq(&unit.bam[1], &bam, sizeof(bam));
    unit.free_blocks[1] = available;
    ck_assert(dos_rel_channel_retry_grow(&unit, channel));
    finish_growth(&unit, &bank, image);
    ck_assert(!unit.channels[channel].failed);
    ck_assert_int_eq(unit.channels[channel].rel.record.result, DOS_REL_RECORD_READY);
    ck_assert_uint_eq(unit.free_blocks[1], available - 1u);
    ck_assert_uint_eq(image->flushes, 0);
    free(image);
}
END_TEST

// DBLBUF must await a failed old READ before replacing its selected page.
START_TEST(test_growth_awaited_page_retry) {
    const unsigned int format = (unsigned int)_i / 2u, variant = (unsigned int)_i % 2u;
    rel_growth_image_t* const image = malloc(sizeof(*image));
    ck_assert_ptr_nonnull(image);
    dos_media_bank_t bank = {0};
    dos_unit_t unit;
    const uint8_t channel = prepare_growth(&unit, &bank, image, format, 127);
    dos_channel_t* const state = &unit.channels[channel];
    start_growth(&unit, &bank, image, channel, variant == 0 ? 3 : 7);
    dos_buffer_t* const other = &unit.buffers[state->buffers[state->rel.record.active ^ 1u]];
    other->track = 1;
    other->sector = 19;
    other->resident = false;
    other->failed = true;
    finish_growth(&unit, &bank, image);
    ck_assert(state->failed);
    ck_assert_uint_eq(state->closing, variant == 0 ? DOS_GROW_REL_LAST_WAIT : DOS_GROW_REL_NEXT_WAIT);
    const unsigned int reads = image->reads;
    image->fail_read = reads + 1u;
    ck_assert(dos_rel_channel_retry_grow(&unit, channel));
    finish_growth(&unit, &bank, image);
    ck_assert(state->failed && !state->ready);
    ck_assert_uint_eq(image->reads, reads + 1u);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert(dos_rel_channel_retry_grow(&unit, channel));
    finish_growth(&unit, &bank, image);
    ck_assert(!state->failed && !state->ready);
    ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_READY);
    ck_assert_uint_eq(image->reads, reads + 2u);
    ck_assert_uint_eq(image->flushes, 0);
    free(image);
}
END_TEST

// Replacement media cannot inherit allocations or publish an interrupted growth.
START_TEST(test_growth_media_replacement) {
    const unsigned int format = (unsigned int)_i / 3u, variant = (unsigned int)_i % 3u;
    const uint8_t phases[] = {DOS_GROW_REL_LINK, DOS_GROW_REL_SIDE_OLD_WRITE, DOS_GROW_REL_MAP};
    rel_growth_image_t* const image = malloc(sizeof(*image));
    ck_assert_ptr_nonnull(image);
    dos_media_bank_t bank = {0};
    dos_unit_t unit;
    const uint8_t channel = prepare_growth(&unit, &bank, image, format, 254);
    start_growth(&unit, &bank, image, channel, 121);
    dos_channel_t* const state = &unit.channels[channel];
    for (unsigned int step = 0; state->closing != phases[variant]; ++step) {
        ck_assert_uint_lt(step, 4096);
        dos_work(&unit, &bank);
        ck_assert(!state->failed);
    }
    const unsigned int reads = image->reads, writes = image->writes;
    const dos_bam_t bam = unit.bam[1];
    const uint16_t use = unit.buffer_use;
    const dos_media_io_t replacement = bank.units[3].slots[1].io;
    ck_assert_int_eq(dos_media_set_slot(&bank, 3, 1, &replacement), DOS_MEDIA_OK);
    const unsigned int flushes = image->flushes;
    finish_growth(&unit, &bank, image);
    ck_assert(state->failed && !state->ready && state->allocated);
    ck_assert_int_eq(state->rel.record.lookup.media_result, DOS_MEDIA_STALE);
    ck_assert_mem_eq(&unit.bam[1], &bam, sizeof(bam));
    ck_assert_uint_eq(unit.buffer_use, use);
    ck_assert_uint_eq(image->reads, reads);
    ck_assert_uint_eq(image->writes, writes);
    ck_assert_uint_eq(image->flushes, flushes);
    ck_assert(!dos_rel_channel_retry_grow(&unit, channel));
    ck_assert(!dos_finalize(&unit, &bank));
    free(image);
}
END_TEST

// CLSREL writes only SCRUB's dirty page, then publishes native SSEND counts.
START_TEST(test_pipeline_rel_close) {
    const unsigned int format = (unsigned int)_i / 6u;
    const unsigned int variant = (unsigned int)_i % 6u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, variant == 4 ? 254 : 0);
    prepare_close_metadata(&unit, channel, &image);
    dos_channel_t* const state = &unit.channels[channel];
    const bool modified = variant >= 1 && variant <= 3;
    if (modified) {
        ck_assert(dos_rel_channel_begin_write(&unit, channel));
        ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
        if (variant == 2) {
            ck_assert(dos_rel_channel_writeback(&unit, &bank, channel, &state->rel_write));
        }
    }
    if (variant == 3) {
        image.bytes[6][state->directory_entry + 2u] |= 0x20u;
    } else if (variant == 5) {
        ck_assert(dos_rel_channel_position(&unit, channel, 916, 0));
        finish_pipeline(&unit, &bank, &image);
        ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_MISSING);
    }
    uint8_t expected[DISKIMAGE_SECTOR_SIZE];
    memcpy(expected, image.bytes[6], sizeof(expected));
    const bool publish = format == 0 || modified;
    if (publish) {
        if (variant != 3) {
            expected[DISKIMAGE_DIRECTORY_ENTRY_SIZE + 2u] = DISKIMAGE_FTYPE_REL | 0x80u;
        }
        expected[DISKIMAGE_DIRECTORY_ENTRY_SIZE + 30u] = format == 2 ? 122 : 121;
        expected[DISKIMAGE_DIRECTORY_ENTRY_SIZE + 31u] = 0;
    }
    const uint8_t outgoing = state->buffers[state->rel.record.active];
    const dos_buffer_t saved = unit.buffers[outgoing];
    const unsigned int reads = image.reads, writes = image.writes, flushes = image.flushes;
    dos_status(&unit, models[format], DOS_STATUS_FILE_EXISTS, 0, 0);
    ck_assert(dos_rel_channel_close(&unit, channel));
    ck_assert(dos_rel_channel_close(&unit, channel));
    ck_assert(!state->ready && dos_work_pending(&unit));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert(!dos_rel_channel_begin_write(&unit, channel));
    ck_assert(!dos_rel_channel_put(&unit, &bank, channel, 'X'));
    ck_assert(!dos_rel_channel_pad(&unit, channel));
    ck_assert(!dos_rel_channel_finish_write(&unit, channel, false));
    ck_assert(!dos_rel_channel_resume_read(&unit, &bank, channel));
    ck_assert(!dos_rel_channel_position(&unit, channel, 1, 0));
    ck_assert(!dos_rel_channel_writeback(&unit, &bank, channel, &state->rel_write));
    ck_assert(!dos_rel_channel_advance_write(&unit, &bank, channel, state->rel_write));
    ck_assert(!dos_close(&unit, &bank, models[format], 2));
    finish_pipeline(&unit, &bank, &image);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), DOS_UNUSED);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(image.reads, reads + (format == 2 ? 2 : 0) + publish);
    ck_assert_uint_eq(image.writes, writes + (modified && variant != 2) + publish);
    ck_assert_uint_eq(image.flushes, flushes + publish);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_FILE_EXISTS);
    ck_assert_mem_eq(image.bytes[6], expected, sizeof(expected));
    ck_assert_mem_eq(unit.buffers[outgoing].bytes, saved.bytes, sizeof(saved.bytes));
    if (modified) {
        ck_assert_uint_eq(image.bytes[3][2], 'Z');
        ck_assert_uint_eq(image.bytes[3][3], 'A');
        ck_assert_uint_eq(image.bytes[3][255], 'A');
    }
}
END_TEST

// Retained close jobs retry without padding, duplicate counts or losing peers.
START_TEST(test_pipeline_rel_close_failure) {
    const unsigned int format = (unsigned int)_i / 18u;
    const unsigned int failure = (unsigned int)_i / 3u % 6u;
    const unsigned int entry = (unsigned int)_i % 3u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    if (failure == 5) {
        image.fail_read = format == 2 ? 4 : 3;
    }
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, 0);
    prepare_close_metadata(&unit, channel, &image);
    dos_channel_t* const state = &unit.channels[channel];
    ck_assert(dos_rel_channel_begin_write(&unit, channel));
    ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
    const uint16_t use = unit.buffer_use;
    const uint8_t active = state->rel.record.active;
    const unsigned int reads = image.reads, writes = image.writes;
    if (failure == 0) {
        image.fail_write = writes + 1u;
    } else if (failure == 1) {
        image.bytes[0][6] = 1;
        image.bytes[0][7] = 1;
        image.bytes[1][1] = UINT8_MAX;
        image.bytes[1][2] = 1;
        memcpy(unit.buffers[state->buffers[2]].bytes, image.bytes[0], DISKIMAGE_SECTOR_SIZE);
        image.fail_read = reads + 1u;
    } else if (failure == 2) {
        image.fail_read = reads + (format == 2 ? 2u : 0u) + 1u;
    } else if (failure == 3) {
        image.fail_write = writes + 2u;
    } else if (failure == 4) {
        image.fail_flush = true;
    }
    if (entry == 2) {
        ck_assert(!dos_finalize(&unit, &bank));
    } else {
        if (entry == 1) {
            ck_assert(!dos_close(&unit, &bank, models[format], 2));
        } else {
            ck_assert(dos_rel_channel_close(&unit, channel));
        }
        finish_pipeline(&unit, &bank, &image);
    }
    ck_assert(state->allocated && state->failed && !state->ready);
    ck_assert_uint_eq(unit.buffer_use, use);
    ck_assert_uint_eq(state->rel.number, 1);
    ck_assert_uint_eq(state->rel.record.active, active ^ (failure != 0));
    ck_assert_int_eq(unit.status_code,
                     failure == 1 || failure == 2 || failure == 5 ? DOS_STATUS_READ_ERROR : DOS_STATUS_WRITE_ERROR);
    const dos_unit_t failed = unit;
    dos_work(&unit, &bank);
    ck_assert_mem_eq(&unit, &failed, sizeof(failed));
    ck_assert(!dos_rel_channel_retry_position(&unit, channel));
    ck_assert(!dos_rel_channel_retry_write(&unit, channel));
    if (failure == 3 || failure == 4) {
        image.bytes[6][5] = 0xe1;
    }
    image.fail_flush = false;
    const unsigned int failed_reads = image.reads, failed_writes = image.writes;
    if (entry == 2) {
        ck_assert(dos_finalize(&unit, &bank));
    } else {
        if (entry == 1) {
            ck_assert(!dos_close(&unit, &bank, models[format], 2));
        } else {
            ck_assert(dos_rel_channel_retry_close(&unit, channel));
        }
        finish_pipeline(&unit, &bank, &image);
    }
    ck_assert_uint_eq(dos_find_channel(&unit, 2), DOS_UNUSED);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(image.bytes[3][2], 'Z');
    ck_assert_uint_eq(image.bytes[3][3], 'A');
    ck_assert_uint_eq(image.bytes[6][DISKIMAGE_DIRECTORY_ENTRY_SIZE + 30u],
                      (format == 2 ? 122 : 121) + (failure == 1 ? 121 : 0));
    if (entry == 1) {
        ck_assert_int_eq(unit.status_code, DOS_STATUS_OK);
    } else {
        ck_assert_mem_eq(unit.status, failed.status, sizeof(unit.status));
    }
    if (failure == 3 || failure == 4) {
        ck_assert_uint_eq(image.bytes[6][5], 0xe1);
    }
    if (failure == 4) {
        ck_assert_uint_eq(image.reads, failed_reads);
        ck_assert_uint_eq(image.writes, failed_writes);
    }
}
END_TEST

// The storage CLOSE entry distinguishes FRECHN, status-retaining SA 1 and ENDCMD.
START_TEST(test_public_rel_close) {
    const unsigned int format = (unsigned int)_i / 8u;
    const unsigned int variant = (unsigned int)_i % 8u;
    const uint8_t requested = variant / 2u == 3 ? IEEE_COMMAND_CHANNEL : variant / 2u;
    const uint8_t secondary = requested == IEEE_COMMAND_CHANNEL ? 2 : requested;
    const bool modified = variant & 1u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, secondary, 0);
    prepare_close_metadata(&unit, channel, &image);
    if (modified) {
        ck_assert(dos_rel_channel_begin_write(&unit, channel));
        ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
    }
    dos_status(&unit, models[format], DOS_STATUS_FILE_EXISTS, 0, 0);
    dos_status_get(&unit, models[format], 2);
    const dos_unit_t before = unit;
    memset(unit.command_bytes, 0xa5, sizeof(unit.command_bytes));
    const unsigned int reads = image.reads, writes = image.writes, flushes = image.flushes;
    ck_assert_int_eq(dos_close(&unit, &bank, models[format], requested), requested == IEEE_LOAD_CHANNEL);
    if (requested != IEEE_LOAD_CHANNEL) {
        ck_assert_int_eq(unit.command, DOS_COMMAND_CLOSE);
        ck_assert(unit.channels[channel].allocated && !unit.channels[channel].ready);
        ck_assert_uint_eq(image.reads, reads);
        ck_assert_uint_eq(image.writes, writes);
        ck_assert(!dos_finalize(&unit, &bank));
        finish_pipeline(&unit, &bank, &image);
        ck_assert_uint_eq(image.writes, writes + modified + (format == 0 || modified));
        ck_assert_uint_eq(image.flushes, flushes + (format == 0 || modified));
    } else {
        ck_assert_uint_eq(image.reads, reads);
        ck_assert_uint_eq(image.writes, writes);
        ck_assert_uint_eq(image.flushes, flushes);
    }
    ck_assert_uint_eq(dos_find_channel(&unit, secondary), DOS_UNUSED);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert(!dos_work_pending(&unit));
    if (requested <= IEEE_SAVE_CHANNEL) {
        ck_assert_mem_eq(unit.status, before.status, sizeof(unit.status));
        ck_assert_uint_eq(unit.status_position, before.status_position);
    } else {
        ck_assert_int_eq(unit.status_code, DOS_STATUS_OK);
        ck_assert_uint_eq(unit.status_position, 0);
    }
    for (unsigned int byte = 0; byte < sizeof(unit.command_bytes); ++byte) {
        ck_assert_uint_eq(unit.command_bytes[byte],
                          requested <= IEEE_SAVE_CHANNEL || byte >= DOS_COMMAND_LIMIT ? 0xa5 : 0);
    }
    if (modified && requested != IEEE_LOAD_CHANNEL) {
        ck_assert_uint_eq(image.bytes[3][2], 'Z');
        ck_assert_uint_eq(image.bytes[3][3], 'A');
    }
}
END_TEST

// CLSALL orders secondaries, not logical indices, and retains failed owners.
START_TEST(test_public_rel_close_all_order) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int failure = (unsigned int)_i % 3u;
    rel_image_t images[2] = {0};
    dos_media_bank_t bank = {0};
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    for (unsigned int owner = 0; owner < count_of(images); ++owner) {
        mount_job_image(&bank, &images[owner], format, 3, owner, true);
    }
    uint8_t channels[2];
    for (unsigned int owner = 0; owner < count_of(channels); ++owner) {
        dos_media_ref_t reference;
        ck_assert_int_eq(dos_media_reference(&bank, 3, owner, &reference), DOS_MEDIA_OK);
        channels[owner] = prepare_job_channel(&unit, &bank, &images[owner], format, reference, owner * 2u, 0);
        prepare_close_metadata(&unit, channels[owner], &images[owner]);
        ck_assert(dos_rel_channel_begin_write(&unit, channels[owner]));
        ck_assert(dos_rel_channel_put(&unit, &bank, channels[owner], 'Z' - owner));
    }
    dos_status(&unit, models[format], DOS_STATUS_FILE_EXISTS, 0, 0);
    const dos_channel_t lower = unit.channels[channels[0]];
    if (failure != 0) {
        images[failure == 1 ? 1 : 0].fail_write = 1;
    }
    ck_assert(!dos_close(&unit, &bank, models[format], IEEE_COMMAND_CHANNEL));
    ck_assert_uint_eq(unit.channels[channels[1]].closing, DOS_CLOSE_REL_SCRUB);
    ck_assert_mem_eq(&unit.channels[channels[0]], &lower, sizeof(lower));
    for (unsigned int step = 0; dos_work_pending(&unit); ++step) {
        ck_assert_uint_lt(step, 40);
        const unsigned int jobs = images[0].reads + images[0].writes + images[1].reads + images[1].writes;
        dos_work(&unit, &bank);
        ck_assert_uint_le(images[0].reads + images[0].writes + images[1].reads + images[1].writes - jobs, 1);
    }
    if (failure != 0) {
        const unsigned int owner = failure == 1 ? 1 : 0;
        ck_assert(unit.channels[channels[owner]].allocated && unit.channels[channels[owner]].failed);
        ck_assert_int_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
        if (failure == 1) {
            ck_assert_mem_eq(&unit.channels[channels[0]], &lower, sizeof(lower));
            ck_assert_uint_eq(images[0].writes, 0);
        } else {
            ck_assert(!unit.channels[channels[1]].allocated);
        }
        const dos_unit_t failed = unit;
        dos_work(&unit, &bank);
        ck_assert_mem_eq(&unit, &failed, sizeof(failed));
        ck_assert(!dos_close(&unit, &bank, models[format], IEEE_COMMAND_CHANNEL));
        for (unsigned int step = 0; dos_work_pending(&unit); ++step) {
            ck_assert_uint_lt(step, 40);
            dos_work(&unit, &bank);
        }
    }
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_OK);
    for (unsigned int owner = 0; owner < count_of(channels); ++owner) {
        ck_assert(!unit.channels[channels[owner]].allocated);
        ck_assert_uint_eq(images[owner].bytes[3][2], 'Z' - owner);
        ck_assert_uint_eq(images[owner].bytes[3][3], 'A');
        ck_assert_uint_eq(images[owner].flushes, 1);
    }
}
END_TEST

// Finalization cannot release a REL while an ordered command can retain it.
START_TEST(test_rel_finalize_retains_command_owner) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, 0);
    prepare_close_metadata(&unit, channel, &image);
    unit.command = DOS_COMMAND_COPY;
    const dos_unit_t owner = unit;
    const unsigned int reads = image.reads, writes = image.writes, flushes = image.flushes;
    ck_assert(!dos_finalize(&unit, &bank));
    ck_assert_mem_eq(&unit, &owner, sizeof(owner));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(image.flushes, flushes);
    unit.command = DOS_COMMAND_IDLE;
    ck_assert(dos_finalize(&unit, &bank));
    ck_assert_uint_eq(unit.buffer_use, 0);
}
END_TEST

// Lifecycle failure releases completed RELs but leaves later owners untouched.
START_TEST(test_rel_finalize_peer_retention) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t images[3] = {0};
    dos_media_bank_t bank = {0};
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    dos_media_ref_t references[2];
    for (unsigned int owner = 0; owner < count_of(references); ++owner) {
        mount_job_image(&bank, &images[owner], format, 3, owner, true);
    }
    for (unsigned int owner = 0; owner < count_of(references); ++owner) {
        ck_assert_int_eq(dos_media_reference(&bank, 3, owner, &references[owner]), DOS_MEDIA_OK);
    }
    uint8_t channels[3];
    for (unsigned int owner = 0; owner < count_of(channels); ++owner) {
        const unsigned int slot = owner & 1u;
        channels[owner] = prepare_job_channel(&unit, &bank, &images[slot], format, references[slot], 2u + owner, 0);
        prepare_close_metadata(&unit, channels[owner], &images[slot]);
        ck_assert(dos_rel_channel_begin_write(&unit, channels[owner]));
        ck_assert(dos_rel_channel_put(&unit, &bank, channels[owner], 'Z' - owner));
    }
    dos_unit_t peer;
    dos_reset(&peer, models[format]);
    const dos_media_ref_t peer_reference = mount_job_image(&bank, &images[2], format, 2, 0, true);
    const uint8_t peer_channel = prepare_job_channel(&peer, &bank, &images[2], format, peer_reference, 2, 0);
    ck_assert(dos_rel_channel_begin_write(&peer, peer_channel));
    ck_assert(dos_rel_channel_put(&peer, &bank, peer_channel, 'W'));
    ck_assert(dos_rel_channel_writeback(&peer, &bank, peer_channel, &peer.channels[peer_channel].rel_write));
    const dos_unit_t saved_peer = peer;
    const dos_channel_t later = unit.channels[channels[2]];
    dos_buffer_t pages[DOS_CHANNEL_BUFFER_COUNT];
    for (unsigned int page = 0; page < count_of(pages); ++page) {
        pages[page] = unit.buffers[later.buffers[page]];
    }
    images[1].fail_flush = true;
    ck_assert(!dos_finalize(&unit, &bank));
    ck_assert(!unit.channels[channels[0]].allocated);
    ck_assert(unit.channels[channels[1]].allocated && unit.channels[channels[1]].failed);
    ck_assert_mem_eq(&unit.channels[channels[2]], &later, sizeof(later));
    for (unsigned int page = 0; page < count_of(pages); ++page) {
        ck_assert_mem_eq(&unit.buffers[later.buffers[page]], &pages[page], sizeof(pages[page]));
    }
    ck_assert_uint_eq(images[2].writes, 1);
    ck_assert_uint_eq(images[2].flushes, 0);
    const unsigned int reads = images[1].reads, writes = images[1].writes;
    char status[DOS_STATUS_LINE_CAPACITY];
    memcpy(status, unit.status, sizeof(status));
    images[1].fail_flush = false;
    ck_assert(dos_finalize(&unit, &bank));
    ck_assert_uint_eq(images[1].reads, reads);
    ck_assert_uint_eq(images[1].writes, writes);
    ck_assert_uint_eq(images[1].flushes, 2);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_mem_eq(unit.status, status, sizeof(status));
    ck_assert_mem_eq(&peer, &saved_peer, sizeof(peer));
    ck_assert_uint_eq(images[2].flushes, 0);
}
END_TEST

// Rebinding at each close persistence phase permanently retires the old owner.
START_TEST(test_pipeline_rel_close_replaced_media) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int phase = (unsigned int)_i % 3u;
    rel_image_t image = {0}, replacement = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference, 2, 0);
    prepare_close_metadata(&unit, channel, &image);
    ck_assert(dos_rel_channel_begin_write(&unit, channel));
    ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
    ck_assert(dos_rel_channel_close(&unit, channel));
    dos_channel_t* const state = &unit.channels[channel];
    const uint16_t use = unit.buffer_use;
    const uint8_t target = phase == 0   ? DOS_CLOSE_REL_SCRUB
                           : phase == 1 ? DOS_CLOSE_REL_DIRECTORY_WRITE
                                        : DOS_CLOSE_REL_FLUSH;
    for (unsigned int step = 0; state->closing != target; ++step) {
        ck_assert_uint_lt(step, 16);
        dos_work(&unit, &bank);
        ck_assert(!state->failed);
    }
    mount_job_image(&bank, &replacement, format, 3, 1, true);
    const unsigned int reads = image.reads, writes = image.writes, flushes = image.flushes;
    dos_work(&unit, &bank);
    ck_assert(state->allocated && state->failed && !state->ready);
    ck_assert_int_eq(state->rel.record.lookup.media_result, DOS_MEDIA_STALE);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(image.flushes, flushes);
    ck_assert_uint_eq(replacement.reads | replacement.writes | replacement.flushes, 0);
    ck_assert_uint_eq(unit.buffer_use, use);
    ck_assert(!dos_rel_channel_retry_close(&unit, channel));
    ck_assert(!dos_work_pending(&unit));
}
END_TEST

// A peer close can commit after failure without a retry restoring stale entries.
START_TEST(test_pipeline_rel_close_peer_directory) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    for (unsigned int file = 0; file < 2; ++file) {
        memset(image.bytes[file], 0, DISKIMAGE_SECTOR_SIZE);
        image.bytes[file][1] = 17;
        image.bytes[file][4] = 1;
        image.bytes[file][5] = file;
        image.bytes[file][16] = 1;
        image.bytes[file][17] = 3u + file;
        image.bytes[3u + file][0] = 0;
        image.bytes[3u + file][1] = UINT8_MAX;
    }
    memset(image.bytes[5], 0, DISKIMAGE_SECTOR_SIZE);
    image.bytes[5][3] = 1;
    image.bytes[5][4] = 1;
    memset(image.bytes[6], 0x5a, DISKIMAGE_SECTOR_SIZE);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    unit.bam[1] = (dos_bam_t){.media = reference, .buffer = {.resident = true}};
    uint8_t channels[2];
    for (unsigned int file = 0; file < count_of(channels); ++file) {
        channels[file] = dos_rel_channel_claim(&unit, reference, 2u + file);
        ck_assert_uint_ne(channels[file], DOS_UNUSED);
        const uint8_t root = format == 2 ? (file == 0 ? 2 : 5) : file;
        dos_rel_channel_prepare(&unit, channels[file], models[format], 1, root, 1, 200, 0);
        finish_pipeline(&unit, &bank, &image);
        dos_channel_t* const state = &unit.channels[channels[file]];
        state->directory_track = 1;
        state->directory_sector = 6;
        state->directory_entry = (file + 1u) * DISKIMAGE_DIRECTORY_ENTRY_SIZE;
        image.bytes[6][state->directory_entry + 2u] = DISKIMAGE_FTYPE_REL;
        ck_assert(dos_rel_channel_begin_write(&unit, channels[file]));
        ck_assert(dos_rel_channel_put(&unit, &bank, channels[file], 'Z' - file));
    }
    image.fail_write = 2;
    ck_assert(dos_rel_channel_close(&unit, channels[0]));
    ck_assert(dos_rel_channel_close(&unit, channels[1]));
    for (unsigned int step = 0; dos_work_pending(&unit); ++step) {
        ck_assert_uint_lt(step, 32);
        const unsigned int jobs = image.reads + image.writes;
        dos_work(&unit, &bank);
        ck_assert_uint_le(image.reads + image.writes - jobs, 1);
    }
    ck_assert(unit.channels[channels[0]].allocated && unit.channels[channels[0]].failed);
    ck_assert_uint_eq(dos_find_channel(&unit, 3), DOS_UNUSED);
    ck_assert_uint_ne(unit.buffer_use, 0);
    ck_assert_uint_eq(image.writes, 4);
    uint8_t peer[DISKIMAGE_DIRECTORY_ENTRY_SIZE];
    memcpy(peer, image.bytes[6] + 2u * DISKIMAGE_DIRECTORY_ENTRY_SIZE, sizeof(peer));
    ck_assert_uint_eq(peer[2], DISKIMAGE_FTYPE_REL | 0x80u);
    ck_assert_uint_eq(peer[30], format == 2 ? 3 : 2);
    ck_assert_uint_eq(peer[31], 0);
    ck_assert(dos_rel_channel_retry_close(&unit, channels[0]));
    finish_pipeline(&unit, &bank, &image);
    ck_assert_mem_eq(image.bytes[6] + 2u * DISKIMAGE_DIRECTORY_ENTRY_SIZE, peer, sizeof(peer));
    ck_assert_uint_eq(image.bytes[6][DISKIMAGE_DIRECTORY_ENTRY_SIZE + 2u], DISKIMAGE_FTYPE_REL | 0x80u);
    ck_assert_uint_eq(image.bytes[6][DISKIMAGE_DIRECTORY_ENTRY_SIZE + 30u], format == 2 ? 3 : 2);
    ck_assert_uint_eq(image.bytes[6][DISKIMAGE_DIRECTORY_ENTRY_SIZE + 31u], 0);
    ck_assert_uint_eq(image.bytes[3][2], 'Z');
    ck_assert_uint_eq(image.bytes[4][2], 'Y');
    ck_assert_uint_eq(image.bytes[3][3], 'A');
    ck_assert_uint_eq(image.bytes[4][3], 'B');
    ck_assert_uint_eq(image.writes, 5);
    ck_assert_uint_eq(image.flushes, 2);
    ck_assert_uint_eq(unit.buffer_use, 0);
}
END_TEST

// Full CLSREL replaces the core fence, while explicit CLOSE 0 remains FRECHN.
START_TEST(test_storage_close_completes_or_shortcuts) {
    const unsigned int format = (unsigned int)_i / 12u;
    const unsigned int variant = (unsigned int)_i % 12u;
    const uint8_t secondaries[] = {IEEE_LOAD_CHANNEL, 2, IEEE_COMMAND_CHANNEL};
    const uint8_t secondary = secondaries[variant / 4u];
    const unsigned int completion = variant % 4u;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    const dos_media_ref_t reference = mount_job_image(&bank, &image, format, 3, 1, true);
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const uint8_t channel = prepare_job_channel(&unit, &bank, &image, format, reference,
                                                secondary == IEEE_COMMAND_CHANNEL ? 2 : secondary, 0);
    prepare_close_metadata(&unit, channel, &image);
    dos_channel_t* const state = &unit.channels[channel];
    ck_assert(dos_rel_channel_begin_write(&unit, channel));
    ck_assert(dos_rel_channel_put(&unit, &bank, channel, 'Z'));
    if (completion != 0) {
        ck_assert(dos_rel_channel_pad(&unit, channel));
        finish_pipeline(&unit, &bank, &image);
    }
    if (completion == 3) {
        ck_assert(dos_rel_channel_writeback(&unit, &bank, channel, &state->rel_write));
    }
    if (completion >= 2) {
        ck_assert(dos_rel_channel_finish_cached(&unit, &bank, channel, false));
    }
    const unsigned int reads = image.reads, writes = image.writes, flushes = image.flushes;
    dos_status(&unit, models[format], DOS_STATUS_FILE_EXISTS, 0, 0);
    ck_assert_int_eq(dos_close(&unit, &bank, models[format], secondary), secondary == IEEE_LOAD_CHANNEL);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(image.flushes, flushes);
    if (secondary != IEEE_LOAD_CHANNEL) {
        ck_assert(state->allocated && !state->ready);
        finish_pipeline(&unit, &bank, &image);
        ck_assert_uint_eq(image.flushes, flushes + 1u);
    }
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(dos_find_channel(&unit, secondary == IEEE_COMMAND_CHANNEL ? 2 : secondary), DOS_UNUSED);
    ck_assert_int_eq(unit.status_code, secondary == IEEE_LOAD_CHANNEL ? DOS_STATUS_FILE_EXISTS : DOS_STATUS_OK);
    ck_assert(dos_close(&unit, &bank, models[format], secondary));
}
END_TEST

// RS=0 remains in native NULBUF without monopolizing the MCU or losing its pages.
START_TEST(test_storage_zero_size_creation_remains_bounded) {
    const unsigned int format = (unsigned int)_i;
    rel_image_t image = {0};
    dos_media_bank_t bank = {0};
    dos_unit_t unit;
    dos_reset(&unit, models[format]);
    const dos_media_ref_t reference = prepare_create_image(&unit, &bank, &image, format, 1);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, IEEE_LOAD_CHANNEL);
    dos_rel_channel_create(&unit, channel, models[format], 1, 0, 0);
    dos_channel_t* const state = &unit.channels[channel];
    for (unsigned int step = 0; state->rel.record.phase != DOS_REL_RECORD_CREATE_DATA; ++step) {
        ck_assert_uint_lt(step, 8);
        dos_work(&unit, &bank);
    }
    dos_work(&unit, &bank);
    const dos_unit_t retained = unit;
    const unsigned int reads = image.reads, writes = image.writes, flushes = image.flushes;
    for (unsigned int pass = 0; pass < DOS_STAGE_CAPACITY; ++pass) {
        ck_assert(dos_work_pending(&unit));
        dos_work(&unit, &bank);
        ck_assert_mem_eq(&unit, &retained, sizeof(unit));
    }
    ck_assert(state->allocated && !state->ready && !state->failed);
    ck_assert_uint_eq(state->rel.record.next, DISKIMAGE_SECTOR_LINK_SIZE);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(image.flushes, flushes);
    ck_assert(!dos_close(&unit, &bank, models[format], IEEE_LOAD_CHANNEL));
    ck_assert(!dos_finalize(&unit, &bank));
    ck_assert_mem_eq(&unit, &retained, sizeof(unit));
}
END_TEST

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
    tcase_add_loop_test(core, test_record_put_and_pad_borrowed_pages, 0, 12);
    tcase_add_loop_test(core, test_record_put_native_wrap_and_deferred_prefetch, 0, 15);
    tcase_add_loop_test(core, test_record_put_requires_prepared_existing_record, 0, 9);
    tcase_add_loop_test(core, test_record_put_generation_retains_dirty_pages, 0, 12);
    tcase_add_loop_test(core, test_record_put_peer_isolation, 0, 3);
    tcase_add_loop_test(core, test_reader_claimed_channel_pool_and_teardown, 0, 3);
    tcase_add_loop_test(core, test_channel_claim_partial_pool_and_logical_exhaustion, 0, 12);
    tcase_add_loop_test(core, test_channel_claim_peer_teardown_and_ordinary_reuse, 0, 6);
    tcase_add_loop_test(core, test_record_finish_write_cached_boundaries, 0, 36);
    tcase_add_loop_test(core, test_record_prepare_cached_completed_window, 0, 18);
    tcase_add_loop_test(core, test_record_cached_completion_generation, 0, 6);
    tcase_add_loop_test(core, test_pipeline_prefix_status_and_missing_get, 0, 3);
    tcase_add_loop_test(core, test_pipeline_pending_refill_and_close_cancel, 0, 6);
    tcase_add_loop_test(core, test_pipeline_generation_and_io_failures, 0, 15);
    tcase_add_loop_test(core, test_pipeline_stale_media_stops_staged_batch, 0, 36);
    tcase_add_loop_test(core, test_pipeline_peer_reposition_and_drive_status, 0, 3);
    tcase_add_loop_test(core, test_pipeline_command_priority_and_job_headers, 0, 9);
    tcase_add_loop_test(core, test_pipeline_retains_incomplete_boundaries, 0, 6);
    tcase_add_loop_test(core, test_output_sources_keep_status_and_ordinary_owners, 0, 3);
    tcase_add_loop_test(core, test_pipeline_position_buffer_reuse, 0, 24);
    tcase_add_loop_test(core, test_initialize_relative_active_job, 0, 12);
    tcase_add_loop_test(core, test_relative_writer_rejects_ordinary_block_editors, 0, 15);
    tcase_add_loop_test(core, test_position_required_prefetch_retry, 0, 18);
    tcase_add_loop_test(core, test_write_cold_bam_is_cooperative, 0, 24);
    tcase_add_loop_test(core, test_write_bam_command_replacement, 0, 12);
    tcase_add_loop_test(core, test_pipeline_position_buffer_failures, 0, 15);
    tcase_add_loop_test(core, test_channel_writeback_saved_page_and_dirty_mask, 0, 18);
    tcase_add_loop_test(core, test_channel_writeback_failures_and_retry, 0, 15);
    tcase_add_loop_test(core, test_channel_writeback_flush_remains_separate, 0, 3);
    tcase_add_loop_test(core, test_channel_writeback_peer_failure_isolation, 0, 3);
    tcase_add_loop_test(core, test_channel_write_handoff_cached_and_refilled, 0, 18);
    tcase_add_loop_test(core, test_channel_write_handoff_matching_written_header, 0, 3);
    tcase_add_loop_test(core, test_channel_write_handoff_blocked_ownership, 0, 15);
    tcase_add_loop_test(core, test_channel_write_handoff_failure_and_retry, 0, 18);
    tcase_add_loop_test(core, test_channel_write_handoff_peer_isolation, 0, 3);
    tcase_add_loop_test(core, test_owned_writer_cooperative_padding, 0, 9);
    tcase_add_loop_test(core, test_owned_writer_failure_retry_and_lifetime, 0, 18);
    tcase_add_loop_test(core, test_owned_writer_peer_and_pool_ownership, 0, 3);
    tcase_add_loop_test(core, test_owned_cached_completion_status_and_cursor, 0, 36);
    tcase_add_loop_test(core, test_owned_cached_completion_boundaries, 0, 12);
    tcase_add_loop_test(core, test_owned_cached_completion_stale_retains_dirty, 0, 6);
    tcase_add_loop_test(core, test_owned_cached_completion_peer_isolation, 0, 3);
    tcase_add_loop_test(core, test_cached_read_return_reenters_write, 0, 9);
    tcase_add_loop_test(core, test_cached_read_return_dirty_boundary, 0, 24);
    tcase_add_loop_test(core, test_cached_read_return_write_failure_retry, 0, 24);
    tcase_add_loop_test(core, test_cached_read_return_lifetime, 0, 12);
    tcase_add_loop_test(core, test_cached_read_return_missing_get, 0, 6);
    tcase_add_loop_test(core, test_cached_read_return_peer_isolation, 0, 3);
    tcase_add_loop_test(core, test_cached_next_retained_write_window, 0, 18);
    tcase_add_loop_test(core, test_owned_nrbuf_dirty_refill, 0, 6);
    tcase_add_loop_test(core, test_owned_nrbuf_read_failure_retry, 0, 18);
    tcase_add_loop_test(core, test_owned_nrbuf_deferred_prefetch_failure, 0, 12);
    tcase_add_loop_test(core, test_eoi_required_prefetch_retry, 0, 3);
    tcase_add_loop_test(core, test_owned_full_eoi_completion, 0, 54);
    tcase_add_loop_test(core, test_owned_full_eoi_write_failure_retry, 0, 9);
    tcase_add_loop_test(core, test_owned_full_eoi_peer_isolation, 0, 3);
    tcase_add_loop_test(core, test_cached_next_window_guards, 0, 9);
    tcase_add_loop_test(core, test_storage_close_completes_or_shortcuts, 0, 36);
    tcase_add_loop_test(core, test_pipeline_position_resident_index, 0, 24);
    tcase_add_loop_test(core, test_pipeline_position_index_failure, 0, 9);
    suite_add_tcase(suite, core);
    tcase_add_loop_test(core, test_pipeline_open_directory_link_and_initial_record, 0, 18);
    tcase_add_loop_test(core, test_pipeline_open_failed_jobs_and_lifetime, 0, 12);
    tcase_add_loop_test(core, test_pipeline_open_peer_isolation, 0, 3);
    tcase_add_loop_test(core, test_pipeline_open_step_order_and_cancel, 0, 9);
    tcase_add_loop_test(core, test_pipeline_open_clean_nrbuf, 0, 9);
    tcase_add_loop_test(core, test_clean_nrbuf_refill_failure_and_cancel, 0, 9);
    tcase_add_loop_test(core, test_clean_nrbuf_terminal_next_pointer, 0, 3);
    tcase_add_loop_test(core, test_clean_nrbuf_resident_terminal_records, 0, 12);
    tcase_add_loop_test(core, test_clean_position_buffer_reuse, 0, 24);
    tcase_add_loop_test(core, test_clean_position_buffer_get_continuation, 0, 18);
    tcase_add_loop_test(core, test_clean_position_buffer_failure_and_cancel, 0, 15);
    tcase_add_loop_test(core, test_clean_position_buffer_required_prefetch_error, 0, 12);
    tcase_add_loop_test(core, test_clean_position_buffer_peer_prefix, 0, 3);
    tcase_add_loop_test(core, test_clean_position_resident_index, 0, 27);
    tcase_add_loop_test(core, test_clean_position_index_failure_and_cancel, 0, 12);
    tcase_add_loop_test(core, test_clean_position_last_group_cache, 0, 4);
    tcase_add_loop_test(core, test_clean_position_wrapped_resident_header, 0, 3);
    tcase_add_loop_test(core, test_clean_position_open_cache_lifetime, 0, 3);
    tcase_add_loop_test(core, test_clean_position_index_peer_prefix, 0, 3);
    tcase_add_loop_test(core, test_clean_position_group_failure_and_empty_table, 0, 4);
    tcase_add_loop_test(core, test_clean_position_terminal_endpoint_wrap, 0, 6);
    tcase_add_loop_test(core, test_pipeline_create_native_pages, 0, 54);
    tcase_add_loop_test(core, test_pipeline_create_failure_retry, 0, 15);
    tcase_add_loop_test(core, test_pipeline_create_replaced_media, 0, 12);
    tcase_add_loop_test(core, test_growth_native_pages, 0, 12);
    tcase_add_loop_test(core, test_growth_partial_failure_retry, 0, 24);
    tcase_add_loop_test(core, test_growth_capacity_preflight, 0, 3);
    tcase_add_loop_test(core, test_growth_awaited_page_retry, 0, 6);
    tcase_add_loop_test(core, test_growth_media_replacement, 0, 9);
    tcase_add_loop_test(core, test_pipeline_rel_close, 0, 18);
    tcase_add_loop_test(core, test_pipeline_rel_close_failure, 0, 54);
    tcase_add_loop_test(core, test_public_rel_close, 0, 24);
    tcase_add_loop_test(core, test_public_rel_close_all_order, 0, 9);
    tcase_add_loop_test(core, test_rel_finalize_retains_command_owner, 0, 3);
    tcase_add_loop_test(core, test_rel_finalize_peer_retention, 0, 3);
    tcase_add_loop_test(core, test_pipeline_rel_close_replaced_media, 0, 9);
    tcase_add_loop_test(core, test_pipeline_rel_close_peer_directory, 0, 3);
    tcase_add_loop_test(core, test_storage_zero_size_creation_remains_bounded, 0, 3);
    return suite;
}

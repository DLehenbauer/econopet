// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "diskimage_test.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d64_fixture.h"
#include "ieee/diskimage.h"

// ----------------------------------------------------------------------------
// Memory-backed read callback
// ----------------------------------------------------------------------------

typedef struct {
    uint8_t* data;
    uint32_t size;
} mem_image_t;

// Reads a bounded byte range from a memory-backed disk image.
static bool mem_read(void* ctx, uint32_t offset, void* buf, size_t len) {
    mem_image_t* img = (mem_image_t*) ctx;
    if (offset + len > img->size) return false;
    memcpy(buf, img->data + offset, len);
    return true;
}

// Writes a bounded byte range to a writable memory-backed disk image.
static bool mem_write(void* ctx, uint32_t offset, const void* buf, size_t len) {
    mem_image_t* img = (mem_image_t*) ctx;
    if (offset + len > img->size) return false;
    memcpy(img->data + offset, buf, len);
    return true;
}

typedef struct {
    mem_image_t image;
    unsigned int reads;
} failing_image_t;

// Serves the first read, then injects an I/O failure on every later read.
static bool fail_after_first_read(void* ctx, uint32_t offset, void* buf, size_t len) {
    failing_image_t* image = (failing_image_t*) ctx;
    image->reads++;
    if (image->reads > 1) return false;
    return mem_read(&image->image, offset, buf, len);
}

// ----------------------------------------------------------------------------
// Synthetic d64 fixture: one PRG file "BASIC" spanning two sectors.
// ----------------------------------------------------------------------------

// Returns the byte offset of a D64 track/sector pair in the synthetic image.
uint32_t diskimage_test_d64_offset(unsigned int track, unsigned int sector) {
    size_t offset;
    const char* const error = d64_fixture_offset(track, sector, &offset);
    ck_assert_msg(error == NULL, "%s", error);
    return (uint32_t) offset;
}

// Builds a D64 fixture with BASIC PRG data and a multi-sector DATA REL file.
uint8_t* diskimage_test_make_d64(void) {
    uint8_t* image = calloc(1, DISKIMAGE_D64_SIZE);
    ck_assert_ptr_nonnull(image);

    d64_fixture_layout_t layout;
    const char* error = d64_fixture_empty(&layout, image, DISKIMAGE_D64_SIZE);
    ck_assert_msg(error == NULL, "%s", error);
    uint8_t basic[264];
    for (unsigned int i = 0; i < 254; ++i) basic[i] = (uint8_t) (i + 2);
    for (unsigned int i = 0; i < 10; ++i) basic[254 + i] = (uint8_t) (0xe2 + i);
    const d64_fixture_sector_t basic_chain[] = {{17, 0}, {17, 5}};
    error = d64_fixture_prg(&layout, image, DISKIMAGE_D64_SIZE, "BASIC", basic,
        sizeof(basic), basic_chain, count_of(basic_chain));
    ck_assert_msg(error == NULL, "%s", error);

    // Entry 1 = REL "DATA" at 16/2, reclen 129. Chain hops 16/2 -> 16/0 ->
    // 15/3 -> 16/5: the track changes 16 -> 15 -> 16 exercise the whole-track
    // buffering in diskchain_build (reload after leaving and returning).
    uint8_t relative[3 * DISKIMAGE_SECTOR_PAYLOAD_SIZE + 50];
    for (size_t i = 0; i < sizeof(relative); ++i)
        relative[i] = (uint8_t) (0xa0 + i / DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    const d64_fixture_sector_t rel_chain[] = {{16, 2}, {16, 0}, {15, 3}, {16, 5}};
    error = d64_fixture_file(&layout, image, DISKIMAGE_D64_SIZE, DISKIMAGE_FTYPE_USR,
        "DATA", relative, sizeof(relative), rel_chain, count_of(rel_chain));
    ck_assert_msg(error == NULL, "%s", error);
    // Deliberate raw REL metadata (no side sectors) for record-chain whitebox tests.
    uint8_t* const relative_entry = image + diskimage_test_d64_offset(18, 1)
        + DISKIMAGE_DIRECTORY_ENTRY_SIZE;
    relative_entry[2] = 0x84;
    relative_entry[DISKIMAGE_DIRECTORY_REL_LENGTH_OFFSET] = 129;

    return image;
}

// Returns the byte offset of a D80 track/sector pair in the synthetic image.
static uint32_t d80_offset(unsigned int track, unsigned int sector) {
    uint32_t offset;
    ck_assert_msg(diskimage_sector_offset(diskimage_type_d80, track, sector, &offset),
        "invalid D80 fixture coordinates: %u/%u", track, sector);
    return offset;
}

// Builds a D80 fixture with a single four-byte BASIC PRG file.
uint8_t* diskimage_test_make_d80(void) {
    uint8_t* image = calloc(1, DISKIMAGE_D80_SIZE);
    ck_assert_ptr_nonnull(image);

    uint8_t* directory = image + d80_offset(39, 1);
    directory[1] = 0xff;
    directory[2] = 0x82;
    directory[3] = 38;
    memset(directory + 5, 0xa0, 16);
    memcpy(directory + 5, "BASIC", 5);

    uint8_t* sector = image + d80_offset(38, 0);
    sector[1] = 5;
    sector[2] = 0x80;
    sector[3] = 0x81;
    sector[4] = 0x82;
    sector[5] = 0x83;
    return image;
}

// Verifies unsupported container sizes do not select a disk-image type.
START_TEST(test_open_rejects_bad_size) {
    // Present a size that is neither a D64 nor a D80 container.
    diskimage_t img;
    mem_image_t mem = { .data = NULL, .size = 12345 };

    // The format detector must reject the image before reading it.
    // No supported fixed-size container matches 12,345 bytes.
    ck_assert(!diskimage_open(&img, mem_read, &mem, mem.size));
}
END_TEST

// Verify every sector against independently specified ROM zones and byte offsets.
START_TEST(test_disk_geometry_matches_rom_zones) {
    static const struct {
        diskimage_type_t type;
        unsigned int first_track;
        unsigned int last_track;
        unsigned int sectors;
        uint32_t offset;
    } zones[] = {
        {diskimage_type_d64, 1, 17, 21, 0},
        {diskimage_type_d64, 18, 24, 19, 91392},
        {diskimage_type_d64, 25, 30, 18, 125440},
        {diskimage_type_d64, 31, 35, 17, 153088},
        {diskimage_type_d80, 1, 39, 29, 0},
        {diskimage_type_d80, 40, 53, 27, 289536},
        {diskimage_type_d80, 54, 64, 25, 386304},
        {diskimage_type_d80, 65, 77, 23, 456704},
    };
    // Keep expected geometry literal so production and fixtures cannot agree on a bug.
    for (size_t zone = 0; zone < count_of(zones); ++zone) {
        for (unsigned int track = zones[zone].first_track; track <= zones[zone].last_track; ++track) {
            unsigned int sectors;
            ck_assert(diskimage_track_sectors(zones[zone].type, track, &sectors));
            ck_assert_uint_eq(sectors, zones[zone].sectors);
            for (unsigned int sector = 0; sector < zones[zone].sectors; ++sector) {
                uint32_t offset;
                ck_assert(diskimage_sector_offset(zones[zone].type, track, sector, &offset));
                const uint32_t expected = zones[zone].offset
                    + ((track - zones[zone].first_track) * zones[zone].sectors + sector) * 256u;
                ck_assert_uint_eq(offset, expected);
            }
            uint32_t offset = UINT32_MAX;
            ck_assert(!diskimage_sector_offset(zones[zone].type, track, zones[zone].sectors, &offset));
            ck_assert_uint_eq(offset, UINT32_MAX);
        }
    }
}
END_TEST

// Invalid types, coordinates, and output pointers reject without changing outputs.
START_TEST(test_disk_geometry_rejects_invalid_inputs) {
    static const struct {
        diskimage_type_t type;
        unsigned int track;
    } invalid[] = {
        {diskimage_type_none, 1}, {diskimage_type_hdd, 1},
        {diskimage_type_d64, 0}, {diskimage_type_d64, 36}, {diskimage_type_d64, UINT_MAX},
        {diskimage_type_d80, 0}, {diskimage_type_d80, 78}, {diskimage_type_d80, UINT_MAX},
    };
    for (size_t i = 0; i < count_of(invalid); ++i) {
        unsigned int sectors = UINT_MAX;
        uint32_t offset = UINT32_MAX;
        ck_assert(!diskimage_track_sectors(invalid[i].type, invalid[i].track, &sectors));
        ck_assert_uint_eq(sectors, UINT_MAX);
        ck_assert(!diskimage_sector_offset(invalid[i].type, invalid[i].track, 0, &offset));
        ck_assert_uint_eq(offset, UINT32_MAX);
    }
    uint32_t offset = UINT32_MAX;
    ck_assert(!diskimage_sector_offset(diskimage_type_d64, 1, UINT_MAX, &offset));
    ck_assert_uint_eq(offset, UINT32_MAX);
    ck_assert(!diskimage_track_sectors(diskimage_type_d64, 1, NULL));
    ck_assert(!diskimage_sector_offset(diskimage_type_d64, 1, 0, NULL));
}
END_TEST

// Shared ordinary fixtures stream exact payload bytes through production parsing,
// including an empty file, sector boundaries, and directory-sector expansion.
START_TEST(test_shared_d64_builder_round_trips_files) {
    static const size_t lengths[] = {0, 1, 253, 254, 255, 508, 509, 6000, 4};
    mem_image_t mem = { .data = malloc(DISKIMAGE_D64_SIZE), .size = DISKIMAGE_D64_SIZE };
    ck_assert_ptr_nonnull(mem.data);
    d64_fixture_layout_t layout;
    const char* error = d64_fixture_empty(&layout, mem.data, mem.size);
    ck_assert_msg(error == NULL, "%s", error);
    diskimage_t img;
    diskimage_entry_t entry;
    ck_assert(diskimage_open(&img, mem_read, &mem, mem.size));
    ck_assert(!diskimage_entry(&img, 0, &entry));
    uint8_t data[6000];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (uint8_t) i;
    for (size_t file = 0; file < count_of(lengths); ++file) {
        char name[16];
        snprintf(name, sizeof(name), "FILE%u", (unsigned int) file);
        error = d64_fixture_prg(&layout, mem.data, mem.size, name, data, lengths[file], NULL, 0);
        ck_assert_msg(error == NULL, "%s", error);
    }
    for (size_t file = 0; file < count_of(lengths); ++file) {
        char name[16];
        snprintf(name, sizeof(name), "FILE%u", (unsigned int) file);
        ck_assert(diskimage_find(&img, name, &entry));
        ck_assert_int_eq(entry.file_type, DISKIMAGE_FTYPE_PRG);
        diskstream_t stream;
        ck_assert(diskstream_open(&stream, &img, entry.start_track, entry.start_sector));
        uint8_t byte;
        bool last;
        for (size_t i = 0; i < lengths[file]; ++i) {
            ck_assert(diskstream_next(&stream, &byte, &last));
            ck_assert_uint_eq(byte, data[i]);
            ck_assert_int_eq(last, i + 1 == lengths[file]);
        }
        ck_assert(!diskstream_next(&stream, &byte, &last));
    }
    ck_assert(!diskimage_entry(&img, count_of(lengths), &entry));
    free(mem.data);
}
END_TEST

// ASCII authoring names encode punctuation before production PETSCII lookup.
START_TEST(test_fixture_punctuation_uses_petscii_lookup) {
    static const char* const ascii_names[] = {"a_b", "a\\b", "a`b", "a{b", "a|b", "a}b", "a~b"};
    static const uint8_t punctuation[] = {0xa4, 0xbf, 0xad, 0xb3, 0xdd, 0xab, 0xb1};
    mem_image_t mem = {.data = malloc(DISKIMAGE_D64_SIZE), .size = DISKIMAGE_D64_SIZE};
    ck_assert_ptr_nonnull(mem.data);
    d64_fixture_layout_t layout;
    const char* error = d64_fixture_empty(&layout, mem.data, mem.size);
    ck_assert_msg(error == NULL, "%s", error);
    diskimage_t img;
    ck_assert(diskimage_open(&img, mem_read, &mem, mem.size));
    for (size_t file = 0; file < count_of(ascii_names); ++file) {
        const uint8_t payload = (uint8_t) file;
        error = d64_fixture_prg(&layout, mem.data, mem.size, ascii_names[file],
            &payload, 1, NULL, 0);
        ck_assert_msg(error == NULL, "%s", error);
        uint8_t encoded[16];
        error = d64_fixture_name(ascii_names[file], encoded);
        ck_assert_msg(error == NULL, "%s", error);
        ck_assert_uint_eq(encoded[0], 'A');
        ck_assert_uint_eq(encoded[1], punctuation[file]);
        ck_assert_uint_eq(encoded[2], 'B');
        char query[4];
        memcpy(query, encoded, sizeof(query) - 1);
        query[sizeof(query) - 1] = '\0';
        diskimage_entry_t entry;
        ck_assert(!diskimage_find(&img, ascii_names[file], &entry));
        ck_assert(diskimage_find(&img, query, &entry));
        ck_assert_str_eq(entry.name, query);
        diskstream_t stream;
        ck_assert(diskstream_open(&stream, &img, entry.start_track, entry.start_sector));
        uint8_t byte;
        bool last;
        ck_assert(diskstream_next(&stream, &byte, &last));
        ck_assert_uint_eq(byte, payload);
        ck_assert(last);
        ck_assert(!diskstream_next(&stream, &byte, &last));
    }
    free(mem.data);
}
END_TEST

// Verifies directory lookup and sequential streaming for the standard D64 fixture.
START_TEST(test_find_and_stream_synthetic_d64) {
    // Open the fixture through the same callback interface used by the driver.
    mem_image_t mem = { .data = diskimage_test_make_d64(), .size = DISKIMAGE_D64_SIZE };
    diskimage_t img;
    ck_assert(diskimage_open(&img, mem_read, &mem, mem.size));
    ck_assert_int_eq(img.type, diskimage_type_d64);

    // Locate BASIC and verify the directory metadata returned to the caller.
    diskimage_entry_t e;
    ck_assert(diskimage_find(&img, "BASIC", &e));
    // The fixture's first directory entry is BASIC and is a PRG file.
    ck_assert_str_eq(e.name, "BASIC");
    ck_assert_int_eq(e.file_type, DISKIMAGE_FTYPE_PRG);

    // Check the supported case-insensitive, drive-prefix, and wildcard forms.
    ck_assert(diskimage_find(&img, "basic", &e));
    ck_assert(diskimage_find(&img, "1:basic", &e));
    ck_assert(diskimage_find(&img, "1.BASIC", &e));
    ck_assert(diskimage_find(&img, "BAS*", &e));
    ck_assert(!diskimage_find(&img, "FORTRAN", &e));

    // Stream the two-sector file and require last only on its final byte.
    diskstream_t st;
    ck_assert(diskstream_open(&st, &img, e.start_track, e.start_sector));

    unsigned int count = 0;
    uint8_t byte = 0;
    bool last = false;
    while (diskstream_next(&st, &byte, &last)) {
        count++;
        // Every byte before the final payload byte must leave EOI clear.
        if (count < 264) ck_assert(!last);
    }
    // The two fixture sectors expose 254 + 10 bytes and finish at 0xEB.
    ck_assert_uint_eq(count, 264);
    ck_assert(last);
    ck_assert_uint_eq(byte, 0xE0 + 11);   // final payload byte

    // Release the generated image after its callback-backed use is complete.
    free(mem.data);
}
END_TEST

// Verifies all eight directory slots and the first successor slot are visited.
START_TEST(test_directory_walks_full_sector_and_successor) {
    const unsigned int entry_count = 9;
    // Replace the fixture directory with a full sector followed by one entry.
    const bool d80 = _i != 0;
    mem_image_t mem = {
        .data = d80 ? diskimage_test_make_d80() : diskimage_test_make_d64(),
        .size = d80 ? DISKIMAGE_D80_SIZE : DISKIMAGE_D64_SIZE,
    };
    const uint8_t track = d80 ? 39 : 18;
    uint8_t* directory = mem.data + (d80 ? d80_offset(track, 1)
                                        : diskimage_test_d64_offset(track, 1));
    memset(directory, 0, 2 * DISKIMAGE_SECTOR_SIZE);
    directory[0] = track;
    directory[1] = 2;
    for (unsigned int slot = 0; slot < entry_count; slot++) {
        uint8_t* entry = directory + slot * DISKIMAGE_DIRECTORY_ENTRY_SIZE;
        entry[2] = 0x80 | DISKIMAGE_FTYPE_PRG;
        entry[3] = 17;
        entry[4] = (uint8_t) slot;
        memset(entry + 5, 0xa0, 16);
        entry[5] = (uint8_t) ('A' + slot);
    }

    // Require each slot in order, then stop at the end of the second sector.
    diskimage_t img;
    diskimage_entry_t entry;
    ck_assert(diskimage_open(&img, mem_read, &mem, mem.size));
    for (unsigned int index = 0; index < entry_count; index++) {
        ck_assert(diskimage_entry(&img, index, &entry));
        const char name[] = { (char) ('A' + index), '\0' };
        ck_assert_str_eq(entry.name, name);
        ck_assert_uint_eq(entry.file_type, DISKIMAGE_FTYPE_PRG);
        ck_assert_uint_eq(entry.start_track, 17);
        ck_assert_uint_eq(entry.start_sector, index);
    }
    ck_assert(!diskimage_entry(&img, entry_count, &entry));
    free(mem.data);
}
END_TEST

// Verifies an I/O failure after a valid sector stops the stream safely.
START_TEST(test_stream_stops_on_read_failure) {
    // Build a fixture whose callback fails while loading the successor sector.
    failing_image_t mem = {
        .image = { .data = diskimage_test_make_d64(), .size = DISKIMAGE_D64_SIZE },
    };
    diskimage_t img;
    diskstream_t stream;
    uint8_t byte = 0;
    bool last = false;

    // Open the first sector and consume its valid payload.
    ck_assert(diskimage_open(&img, fail_after_first_read, &mem, mem.image.size));
    ck_assert(diskstream_open(&stream, &img, 17, 0));
    for (unsigned int index = 0; index < 254; index++) {
        ck_assert(diskstream_next(&stream, &byte, &last));
        // The first sector is valid data and cannot carry EOI.
        ck_assert(!last);
    }
    // The failed second callback must end the stream without inventing EOI data.
    ck_assert(!diskstream_next(&stream, &byte, &last));
    // One successful initial-sector read and one failed successor read occurred.
    ck_assert_uint_eq(mem.reads, 2);

    // Release the generated image.
    free(mem.image.data);
}
END_TEST

// Verifies directory lookup handles the current on-disk 16-byte names and
// skips entries marked deleted.
START_TEST(test_find_handles_16_byte_names_and_skips_deleted_entries) {
    // Open a fixture and replace BASIC's padded name with a full 16-byte name.
    mem_image_t mem = { .data = diskimage_test_make_d64(), .size = DISKIMAGE_D64_SIZE };
    diskimage_t img;
    diskimage_entry_t entry;
    uint8_t* directory = mem.data + diskimage_test_d64_offset(18, 1);
    static const char name[] = "SIXTEENCHARACTER";

    // Confirm case-insensitive lookup returns the untruncated directory name.
    ck_assert_uint_eq(sizeof(name) - 1, 16);
    ck_assert(diskimage_open(&img, mem_read, &mem, mem.size));
    memset(&directory[5], 0xA0, 16);
    memcpy(&directory[5], name, sizeof(name) - 1);
    ck_assert(diskimage_find(&img, "sixteencharacter", &entry));
    // Lookup preserves all 16 bytes after its case-insensitive comparison.
    ck_assert_str_eq(entry.name, name);

    // Mark that entry deleted and ensure lookup no longer exposes it.
    directory[2] = DISKIMAGE_FTYPE_DEL;
    // Deleted entries must be invisible to filename lookup.
    ck_assert(!diskimage_find(&img, name, &entry));

    // Release the generated image.
    free(mem.data);
}
END_TEST

// Verifies malformed directory metadata is rejected without reading beyond
// the image bounds.
START_TEST(test_directory_rejects_out_of_range_link) {
    // Replace the terminal directory link with an invalid D64 track number.
    mem_image_t mem = { .data = diskimage_test_make_d64(), .size = DISKIMAGE_D64_SIZE };
    diskimage_t img;
    diskimage_entry_t entry;
    uint8_t* directory = mem.data + diskimage_test_d64_offset(18, 1);

    // Enumeration and lookup must fail safely instead of reading past the image.
    ck_assert(diskimage_open(&img, mem_read, &mem, mem.size));
    directory[0] = 36;
    directory[1] = 0;
    // Index 8 forces traversal past the first sector to the invalid successor.
    ck_assert(!diskimage_entry(&img, 8, &entry));
    // Lookup likewise fails rather than dereferencing the invalid link.
    ck_assert(!diskimage_find(&img, "MISSING", &entry));

    // Release the generated image.
    free(mem.data);
}
END_TEST

// Verifies the sequential reader preserves exact nonempty file lengths at
// the sector-boundary cases used by DOS images.
START_TEST(test_stream_preserves_short_and_sector_boundary_lengths) {
    // Exercise short files and the one-byte transition beyond a full sector.
    static const unsigned int lengths[] = { 1, 2, 253, 254, 255 };

    for (size_t test = 0; test < count_of(lengths); test++) {
        const unsigned int length = lengths[test];
        mem_image_t mem = { .data = diskimage_test_make_d64(), .size = DISKIMAGE_D64_SIZE };
        diskimage_t img;
        diskimage_entry_t entry;
        diskstream_t stream;
        uint8_t byte = 0;
        bool last = false;
        uint8_t* first = mem.data + diskimage_test_d64_offset(17, 0);
        uint8_t* second = mem.data + diskimage_test_d64_offset(17, 5);

        // Rewrite BASIC's chain to the current exact payload length.
        ck_assert(diskimage_open(&img, mem_read, &mem, mem.size));
        ck_assert(diskimage_find(&img, "BASIC", &entry));
        if (length <= 254) {
            first[0] = 0;
            first[1] = (uint8_t) (length + 1);
        } else {
            first[0] = 17;
            first[1] = 5;
            second[0] = 0;
            second[1] = 2;
        }
        // Read the declared payload and require EOI only at its final byte.
        ck_assert(diskstream_open(&stream, &img, entry.start_track, entry.start_sector));
        for (unsigned int index = 0; index < length; index++) {
            ck_assert(diskstream_next(&stream, &byte, &last));
            // The stream marks precisely the byte at the configured length.
            ck_assert_int_eq(last, index == length - 1);
        }
        // The iterator must end immediately after the exact payload.
        ck_assert(!diskstream_next(&stream, &byte, &last));
        free(mem.data);
    }
}
END_TEST

// Corrupt sequential links must terminate the iterator without looping or
// reading past the disk image. The caller maps this short stream to a DOS
// read error rather than treating it as a normal EOI-terminated file.
START_TEST(test_stream_stops_on_circular_and_out_of_range_links) {
    // Open the normal fixture and locate BASIC's starting sector.
    mem_image_t mem = { .data = diskimage_test_make_d64(), .size = DISKIMAGE_D64_SIZE };
    diskimage_t img;
    diskimage_entry_t entry;
    diskstream_t stream;
    uint8_t byte = 0;
    bool last = false;

    ck_assert(diskimage_open(&img, mem_read, &mem, mem.size));
    ck_assert(diskimage_find(&img, "BASIC", &entry));

    // A self-link is bounded by the image-sector limit instead of looping.
    uint8_t* sector = mem.data + diskimage_test_d64_offset(17, 0);
    sector[0] = 17;
    sector[1] = 0;
    ck_assert(diskstream_open(&stream, &img, entry.start_track, entry.start_sector));
    for (unsigned int index = 0; index < mem.size / 256u * 254u; index++) {
        ck_assert(diskstream_next(&stream, &byte, &last));
        // A circular link must not masquerade as a correctly terminated file.
        ck_assert(!last);
    }
    // The image-sector bound terminates the circular traversal.
    ck_assert(!diskstream_next(&stream, &byte, &last));

    // An out-of-range successor stops after its valid first sector.
    sector[0] = 36;
    sector[1] = 0;
    ck_assert(diskstream_open(&stream, &img, entry.start_track, entry.start_sector));
    for (unsigned int index = 0; index < 254; index++) {
        ck_assert(diskstream_next(&stream, &byte, &last));
        // The first sector remains readable and is not terminal.
        ck_assert(!last);
    }
    // Loading the invalid successor ends the iterator safely.
    ck_assert(!diskstream_next(&stream, &byte, &last));

    // Release the generated image.
    free(mem.data);
}
END_TEST

// Verifies REL chain metadata, cross-sector reads, and end-of-chain bounds.
START_TEST(test_rel_chain_build_and_read) {
    // Open the fixture and obtain its generated DATA REL directory entry.
    mem_image_t mem = { .data = diskimage_test_make_d64(), .size = DISKIMAGE_D64_SIZE };
    diskimage_t img;
    ck_assert(diskimage_open(&img, mem_read, &mem, mem.size));

    diskimage_entry_t e;
    ck_assert(diskimage_find(&img, "DATA", &e));
    ck_assert_int_eq(e.file_type, DISKIMAGE_FTYPE_REL);
    ck_assert_int_eq(e.record_len, 129);

    // Build the random-access chain and verify its cached topology.
    diskchain_t ch;
    ck_assert(diskchain_build(&ch, &img, e.start_track, e.start_sector));
    // The generated four-sector chain has three full sectors and 50 final bytes.
    ck_assert_uint_eq(ch.count, 4);
    ck_assert_uint_eq(ch.last_used, 50);
    ck_assert_uint_eq(diskchain_size(&ch), 3 * 254 + 50);

    // Sector k's payload is filled with 0xA0+k; read across the 0/1 sector
    // boundary (offset 250, 10 bytes: 4 from sector 0, 6 from sector 1).
    uint8_t buf[16];
    ck_assert(diskchain_read(&ch, 250, buf, 10));
    // The request crosses from sector 0's payload into sector 1's payload.
    for (unsigned int i = 0; i < 4; i++) ck_assert_uint_eq(buf[i], 0xA0);
    for (unsigned int i = 4; i < 10; i++) ck_assert_uint_eq(buf[i], 0xA1);

    // Last byte of the chain reads; one past the end fails.
    ck_assert(diskchain_read(&ch, diskchain_size(&ch) - 1, buf, 1));
    // The final byte is present, but no byte exists beyond the logical chain size.
    ck_assert_uint_eq(buf[0], 0xA3);
    ck_assert(!diskchain_read(&ch, diskchain_size(&ch), buf, 1));

    // Release the generated image.
    free(mem.data);
}
END_TEST

// Verifies REL chain construction rejects invalid start coordinates safely.
START_TEST(test_rel_chain_rejects_out_of_range_start) {
    // Corrupt DATA's start track in the directory entry.
    mem_image_t mem = { .data = diskimage_test_make_d64(), .size = DISKIMAGE_D64_SIZE };
    diskimage_t img;
    diskimage_entry_t entry;
    diskchain_t chain;
    uint8_t* directory = mem.data + diskimage_test_d64_offset(18, 1);

    // Lookup still returns the entry, but chain construction must reject it.
    ck_assert(diskimage_open(&img, mem_read, &mem, mem.size));
    directory[DISKIMAGE_DIRECTORY_ENTRY_SIZE + 3] = 36;
    directory[DISKIMAGE_DIRECTORY_ENTRY_SIZE + 4] = 0;
    ck_assert(diskimage_find(&img, "DATA", &entry));
    // Chain construction validates the track before attempting a sector read.
    ck_assert(!diskchain_build(&chain, &img, entry.start_track, entry.start_sector));

    // Release the generated image.
    free(mem.data);
}
END_TEST

// Verifies the EconoPET flat .hdd extension's container and REL-chain behavior.
START_TEST(test_hdd_flat_container) {
    // 100-sector flat hard-disk REL container: record stream of 128-byte
    // halves + $0D pads (the FORMAT.OS/9 layout, see diskimage_type_hdd).
    const uint32_t size = 100u * 258u;
    mem_image_t mem = { malloc(size), size };
    for (uint32_t r = 0; r < 200; r++) {
        memset(mem.data + r * 129, (int) (0x20 + r), 128);
        mem.data[r * 129 + 128] = 0x0D;
    }

    // Reject invalid sizes, then open the aligned flat container for read/write.
    diskimage_t img;
    ck_assert(!diskimage_open_hdd(&img, mem_read, &mem, 100));   // not 258-aligned
    ck_assert(!diskimage_open_hdd(&img, mem_read, &mem, 0));
    // A positive 258-byte multiple selects the flat HDD container type.
    ck_assert(diskimage_open_hdd(&img, mem_read, &mem, size));
    ck_assert_int_eq(img.type, diskimage_type_hdd);
    img.write = mem_write;

    // Verify the synthetic directory and its drive-prefixed name lookup.
    diskimage_entry_t e;
    ck_assert(diskimage_entry(&img, 0, &e));
    // The extension exposes exactly one synthetic REL directory entry.
    ck_assert_str_eq(e.name, "OS9 DRIVE A");
    ck_assert_int_eq(e.file_type, DISKIMAGE_FTYPE_REL);
    ck_assert_int_eq(e.record_len, 129);
    ck_assert(!diskimage_entry(&img, 1, &e));
    ck_assert(diskimage_find(&img, "0:OS9 DRIVE A", &e));
    ck_assert(!diskimage_find(&img, "OS9 DRIVE B", &e));

    // Build the identity-mapped REL chain and check its boundaries and writes.
    static diskchain_t ch;
    ck_assert(diskchain_build(&ch, &img, e.start_track, e.start_sector));
    // Flat mode maps REL offsets directly to image offsets.
    ck_assert(ch.flat);
    ck_assert_uint_eq(diskchain_size(&ch), size);

    uint8_t buf[129], back[129];
    ck_assert(diskchain_read(&ch, 5u * 129u, buf, 129));
    // Record 5 retains its fixture fill byte and trailing carriage return.
    ck_assert_uint_eq(buf[0], 0x25);
    ck_assert_uint_eq(buf[128], 0x0D);
    ck_assert(diskchain_read(&ch, size - 1, buf, 1));
    ck_assert(!diskchain_read(&ch, size - 1, buf, 2));

    memset(buf, 0xAA, 128);
    buf[128] = 0x0D;
    ck_assert(diskchain_write(&ch, 42u * 129u, buf, 129));
    ck_assert(diskchain_read(&ch, 42u * 129u, back, 129));
    // A successful write round-trips, while an overrun write is rejected.
    ck_assert_mem_eq(buf, back, 129);
    ck_assert(!diskchain_write(&ch, size - 10, buf, 20));

    // Release the generated container.
    free(mem.data);
}
END_TEST

// Optional: exercise a real Waterloo language image when provided via env.
START_TEST(test_real_image_if_available) {
    // Skip this optional integration test when no external image is configured.
    const char* path = getenv("ECONOPET_TEST_D80");
    if (path == NULL) return;

    // Load the external D80 image into callback-backed memory.
    FILE* f = fopen(path, "rb");
    ck_assert_ptr_nonnull(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    mem_image_t mem = { .data = malloc(size), .size = (uint32_t) size };
    ck_assert_uint_eq(fread(mem.data, 1, size, f), (size_t) size);
    fclose(f);

    // Find BASIC, stream it to EOF, and require a substantial real payload.
    diskimage_t img;
    ck_assert(diskimage_open(&img, mem_read, &mem, mem.size));

    diskimage_entry_t e;
    ck_assert(diskimage_find(&img, "1:BASIC", &e));
    // The real image must expose BASIC as a PRG through a drive-prefixed name.
    ck_assert_int_eq(e.file_type, DISKIMAGE_FTYPE_PRG);

    diskstream_t st;
    ck_assert(diskstream_open(&st, &img, e.start_track, e.start_sector));

    unsigned int count = 0;
    uint8_t byte;
    bool last = false;
    while (diskstream_next(&st, &byte, &last)) count++;
    // A real BASIC image is substantial and terminates with its final-byte flag.
    ck_assert(last);
    ck_assert_uint_gt(count, 30000);   // BASIC is ~40KB
    printf("real image: BASIC streams %u bytes\n", count);

    // Release the copied external image.
    free(mem.data);
}
END_TEST

// Unit tests for disk geometry, container parsing, and sequential/REL chains
// without an IEEE command stream or FPGA-register mock.
Suite* diskimage_suite(void) {
    // Register geometry and image I/O tests under the core test case.
    Suite* s = suite_create("diskimage");
    TCase* tc = tcase_create("core");
    tcase_add_test(tc, test_open_rejects_bad_size);
    tcase_add_test(tc, test_disk_geometry_matches_rom_zones);
    tcase_add_test(tc, test_disk_geometry_rejects_invalid_inputs);
    tcase_add_test(tc, test_shared_d64_builder_round_trips_files);
    tcase_add_test(tc, test_fixture_punctuation_uses_petscii_lookup);
    tcase_add_test(tc, test_find_and_stream_synthetic_d64);
    tcase_add_loop_test(tc, test_directory_walks_full_sector_and_successor, 0, 2);
    tcase_add_test(tc, test_stream_stops_on_read_failure);
    tcase_add_test(tc, test_find_handles_16_byte_names_and_skips_deleted_entries);
    tcase_add_test(tc, test_directory_rejects_out_of_range_link);
    tcase_add_test(tc, test_stream_preserves_short_and_sector_boundary_lengths);
    tcase_add_test(tc, test_stream_stops_on_circular_and_out_of_range_links);
    tcase_add_test(tc, test_rel_chain_build_and_read);
    tcase_add_test(tc, test_rel_chain_rejects_out_of_range_start);
    tcase_add_test(tc, test_hdd_flat_container);
    tcase_add_test(tc, test_real_image_if_available);
    suite_add_tcase(s, tc);
    return s;
}

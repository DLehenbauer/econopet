// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "diskimage_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    static const unsigned int spt[36] = {
        0, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
        19, 19, 19, 19, 19, 19, 19, 18, 18, 18, 18, 18, 18, 17, 17, 17, 17, 17
    };
    uint32_t sectors = 0;
    for (unsigned int t = 1; t < track; t++) sectors += spt[t];
    return (sectors + sector) * 256u;
}

// Builds a D64 fixture with BASIC PRG data and a multi-sector DATA REL file.
uint8_t* diskimage_test_make_d64(void) {
    uint8_t* image = calloc(1, DISKIMAGE_D64_SIZE);
    ck_assert_ptr_nonnull(image);

    // Directory sector 18/1: entry 0 = PRG "BASIC" at 17/0; end of dir chain.
    uint8_t* dir = image + diskimage_test_d64_offset(18, 1);
    dir[0] = 0;      // no next dir sector
    dir[1] = 0xFF;
    dir[2] = 0x82;   // closed PRG
    dir[3] = 17;     // start track
    dir[4] = 0;      // start sector
    memset(&dir[5], 0xA0, 16);
    memcpy(&dir[5], "BASIC", 5);

    // File chain: 17/0 (full, 254 bytes) -> 17/5 (10 payload bytes).
    uint8_t* s0 = image + diskimage_test_d64_offset(17, 0);
    s0[0] = 17;
    s0[1] = 5;
    for (unsigned int i = 2; i < 256; i++) s0[i] = (uint8_t) i;

    uint8_t* s1 = image + diskimage_test_d64_offset(17, 5);
    s1[0] = 0;
    s1[1] = 11;      // last valid byte offset -> 10 payload bytes (2..11)
    for (unsigned int i = 2; i <= 11; i++) s1[i] = (uint8_t) (0xE0 + i);

    // Entry 1 = REL "DATA" at 16/2, reclen 129. Chain hops 16/2 -> 16/0 ->
    // 15/3 -> 16/5: the track changes 16 -> 15 -> 16 exercise the whole-track
    // buffering in diskchain_build (reload after leaving and returning).
    uint8_t* d1 = &dir[32];
    d1[2] = 0x84;    // closed REL
    d1[3] = 16;
    d1[4] = 2;
    memset(&d1[5], 0xA0, 16);
    memcpy(&d1[5], "DATA", 4);
    d1[23] = 129;    // record length

    static const uint8_t rel_ts[4][2] = { {16, 2}, {16, 0}, {15, 3}, {16, 5} };
    for (unsigned int k = 0; k < 4; k++) {
        uint8_t* s = image + diskimage_test_d64_offset(rel_ts[k][0], rel_ts[k][1]);
        if (k < 3) {
            s[0] = rel_ts[k + 1][0];
            s[1] = rel_ts[k + 1][1];
        } else {
            s[0] = 0;
            s[1] = 51;   // last valid byte offset -> 50 payload bytes
        }
        for (unsigned int i = 2; i < 256; i++) s[i] = (uint8_t) (0xA0 + k);
    }

    return image;
}

// Returns the byte offset of a D80 track/sector pair in the synthetic image.
static uint32_t d80_offset(unsigned int track, unsigned int sector) {
    uint32_t sectors = 0;
    for (unsigned int current_track = 1; current_track < track; current_track++) {
        if (current_track <= 39) sectors += 29;
        else if (current_track <= 53) sectors += 27;
        else if (current_track <= 64) sectors += 25;
        else sectors += 23;
    }
    return (sectors + sector) * 256u;
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

    for (size_t test = 0; test < ARRAY_SIZE(lengths); test++) {
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
    directory[32 + 3] = 36;
    directory[32 + 4] = 0;
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

// Unit tests for diskimage.c only. These use memory callbacks to verify
// container parsing, directory lookup, and sequential/REL chain behavior
// without an IEEE command stream or FPGA-register mock.
Suite* diskimage_suite(void) {
    // Register each diskimage.c unit test under the core test case.
    Suite* s = suite_create("diskimage");
    TCase* tc = tcase_create("core");
    tcase_add_test(tc, test_open_rejects_bad_size);
    tcase_add_test(tc, test_find_and_stream_synthetic_d64);
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

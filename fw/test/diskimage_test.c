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

static bool mem_read(void* ctx, uint32_t offset, void* buf, size_t len) {
    mem_image_t* img = (mem_image_t*) ctx;
    if (offset + len > img->size) return false;
    memcpy(buf, img->data + offset, len);
    return true;
}

static bool mem_write(void* ctx, uint32_t offset, const void* buf, size_t len) {
    mem_image_t* img = (mem_image_t*) ctx;
    if (offset + len > img->size) return false;
    memcpy(img->data + offset, buf, len);
    return true;
}

// ----------------------------------------------------------------------------
// Synthetic d64 fixture: one PRG file "BASIC" spanning two sectors.
// ----------------------------------------------------------------------------

static uint32_t d64_offset(unsigned int track, unsigned int sector) {
    static const unsigned int spt[36] = {
        0, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
        19, 19, 19, 19, 19, 19, 19, 18, 18, 18, 18, 18, 18, 17, 17, 17, 17, 17
    };
    uint32_t sectors = 0;
    for (unsigned int t = 1; t < track; t++) sectors += spt[t];
    return (sectors + sector) * 256u;
}

static mem_image_t make_d64_fixture(void) {
    mem_image_t img;
    img.size = DISKIMAGE_D64_SIZE;
    img.data = calloc(1, img.size);

    // Directory sector 18/1: entry 0 = PRG "BASIC" at 17/0; end of dir chain.
    uint8_t* dir = img.data + d64_offset(18, 1);
    dir[0] = 0;      // no next dir sector
    dir[1] = 0xFF;
    dir[2] = 0x82;   // closed PRG
    dir[3] = 17;     // start track
    dir[4] = 0;      // start sector
    memset(&dir[5], 0xA0, 16);
    memcpy(&dir[5], "BASIC", 5);

    // File chain: 17/0 (full, 254 bytes) -> 17/5 (10 payload bytes).
    uint8_t* s0 = img.data + d64_offset(17, 0);
    s0[0] = 17;
    s0[1] = 5;
    for (unsigned int i = 2; i < 256; i++) s0[i] = (uint8_t) i;

    uint8_t* s1 = img.data + d64_offset(17, 5);
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
        uint8_t* s = img.data + d64_offset(rel_ts[k][0], rel_ts[k][1]);
        if (k < 3) {
            s[0] = rel_ts[k + 1][0];
            s[1] = rel_ts[k + 1][1];
        } else {
            s[0] = 0;
            s[1] = 51;   // last valid byte offset -> 50 payload bytes
        }
        for (unsigned int i = 2; i < 256; i++) s[i] = (uint8_t) (0xA0 + k);
    }

    return img;
}

START_TEST(test_open_rejects_bad_size) {
    diskimage_t img;
    mem_image_t mem = { .data = NULL, .size = 12345 };
    ck_assert(!diskimage_open(&img, mem_read, &mem, mem.size));
}
END_TEST

START_TEST(test_find_and_stream_synthetic_d64) {
    mem_image_t mem = make_d64_fixture();
    diskimage_t img;
    ck_assert(diskimage_open(&img, mem_read, &mem, mem.size));
    ck_assert_int_eq(img.type, diskimage_type_d64);

    diskimage_entry_t e;
    ck_assert(diskimage_find(&img, "BASIC", &e));
    ck_assert_str_eq(e.name, "BASIC");
    ck_assert_int_eq(e.file_type, DISKIMAGE_FTYPE_PRG);

    // Case-insensitive + drive-prefix + wildcard forms all match.
    ck_assert(diskimage_find(&img, "basic", &e));
    ck_assert(diskimage_find(&img, "1:basic", &e));
    ck_assert(diskimage_find(&img, "1.BASIC", &e));
    ck_assert(diskimage_find(&img, "BAS*", &e));
    ck_assert(!diskimage_find(&img, "FORTRAN", &e));

    // Stream: 254 + 10 bytes, with 'last' on the final byte only.
    diskstream_t st;
    ck_assert(diskstream_open(&st, &img, e.start_track, e.start_sector));

    unsigned int count = 0;
    uint8_t byte = 0;
    bool last = false;
    while (diskstream_next(&st, &byte, &last)) {
        count++;
        if (count < 264) ck_assert(!last);
    }
    ck_assert_uint_eq(count, 264);
    ck_assert(last);
    ck_assert_uint_eq(byte, 0xE0 + 11);   // final payload byte

    free(mem.data);
}
END_TEST

START_TEST(test_rel_chain_build_and_read) {
    mem_image_t mem = make_d64_fixture();
    diskimage_t img;
    ck_assert(diskimage_open(&img, mem_read, &mem, mem.size));

    diskimage_entry_t e;
    ck_assert(diskimage_find(&img, "DATA", &e));
    ck_assert_int_eq(e.file_type, DISKIMAGE_FTYPE_REL);
    ck_assert_int_eq(e.record_len, 129);

    diskchain_t ch;
    ck_assert(diskchain_build(&ch, &img, e.start_track, e.start_sector));
    ck_assert_uint_eq(ch.count, 4);
    ck_assert_uint_eq(ch.last_used, 50);
    ck_assert_uint_eq(diskchain_size(&ch), 3 * 254 + 50);

    // Sector k's payload is filled with 0xA0+k; read across the 0/1 sector
    // boundary (offset 250, 10 bytes: 4 from sector 0, 6 from sector 1).
    uint8_t buf[16];
    ck_assert(diskchain_read(&ch, 250, buf, 10));
    for (unsigned int i = 0; i < 4; i++) ck_assert_uint_eq(buf[i], 0xA0);
    for (unsigned int i = 4; i < 10; i++) ck_assert_uint_eq(buf[i], 0xA1);

    // Last byte of the chain reads; one past the end fails.
    ck_assert(diskchain_read(&ch, diskchain_size(&ch) - 1, buf, 1));
    ck_assert_uint_eq(buf[0], 0xA3);
    ck_assert(!diskchain_read(&ch, diskchain_size(&ch), buf, 1));

    free(mem.data);
}
END_TEST

// Optional: exercise a real Waterloo language image when provided via env.
START_TEST(test_real_image_if_available) {
    const char* path = getenv("ECONOPET_TEST_D80");
    if (path == NULL) return;

    FILE* f = fopen(path, "rb");
    ck_assert_ptr_nonnull(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    mem_image_t mem = { .data = malloc(size), .size = (uint32_t) size };
    ck_assert_uint_eq(fread(mem.data, 1, size, f), (size_t) size);
    fclose(f);

    diskimage_t img;
    ck_assert(diskimage_open(&img, mem_read, &mem, mem.size));

    diskimage_entry_t e;
    ck_assert(diskimage_find(&img, "1:BASIC", &e));
    ck_assert_int_eq(e.file_type, DISKIMAGE_FTYPE_PRG);

    diskstream_t st;
    ck_assert(diskstream_open(&st, &img, e.start_track, e.start_sector));

    unsigned int count = 0;
    uint8_t byte;
    bool last = false;
    while (diskstream_next(&st, &byte, &last)) count++;
    ck_assert(last);
    ck_assert_uint_gt(count, 30000);   // BASIC is ~40KB
    printf("real image: BASIC streams %u bytes\n", count);

    free(mem.data);
}
END_TEST

Suite* diskimage_suite(void) {
    Suite* s = suite_create("diskimage");
    TCase* tc = tcase_create("core");
    tcase_add_test(tc, test_open_rejects_bad_size);
    tcase_add_test(tc, test_find_and_stream_synthetic_d64);
    tcase_add_test(tc, test_rel_chain_build_and_read);
    tcase_add_test(tc, test_real_image_if_available);
    suite_add_tcase(s, tc);
    return s;
}

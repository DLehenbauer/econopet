// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "diskimage_test.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "d64_fixture.h"
#include "ieee/diskimage.h"

uint32_t diskimage_test_d64_offset(unsigned int track, unsigned int sector) {
    size_t offset;
    const char* const error = d64_fixture_offset(track, sector, &offset);
    ck_assert_msg(error == NULL, "%s", error);
    return (uint32_t) offset;
}

uint8_t* diskimage_test_make_d64(void) {
    uint8_t* image = calloc(1, DISKIMAGE_D64_SIZE);
    ck_assert_ptr_nonnull(image);
    d64_fixture_layout_t layout;
    const char* error = d64_fixture_empty(&layout, image, DISKIMAGE_D64_SIZE);
    ck_assert_msg(error == NULL, "%s", error);
    uint8_t basic[264];
    for (unsigned int i = 0; i < 254; ++i) basic[i] = (uint8_t) (i + 2);
    for (unsigned int i = 0; i < 10; ++i) basic[254 + i] = (uint8_t) (0xe2 + i);
    const d64_fixture_sector_t chain[] = {{17, 0}, {17, 5}};
    error = d64_fixture_prg(&layout, image, DISKIMAGE_D64_SIZE, "BASIC", basic,
        sizeof(basic), chain, count_of(chain));
    ck_assert_msg(error == NULL, "%s", error);
    return image;
}

uint8_t* diskimage_test_make_d80(void) {
    uint8_t* image = calloc(1, DISKIMAGE_D80_SIZE);
    ck_assert_ptr_nonnull(image);
    const size_t directory = ((39u - 1) * 29 + 1) * DISKIMAGE_SECTOR_SIZE;
    image[directory + 1] = 0xff;
    image[directory + 2] = 0x82;
    image[directory + 3] = 38;
    memset(image + directory + 5, DISKIMAGE_FILENAME_PADDING, DISKIMAGE_FILENAME_SIZE);
    memcpy(image + directory + 5, "BASIC", 5);
    const size_t sector = (38u - 1) * 29 * DISKIMAGE_SECTOR_SIZE;
    image[sector + 1] = 5;
    for (size_t index = 0; index < 4; ++index) image[sector + 2 + index] = 0x80 + index;
    return image;
}

// Exact-size detection must not issue any storage reads.
START_TEST(test_open_recognizes_only_exact_sizes) {
    static const uint32_t sizes[] = {
        0, DISKIMAGE_SECTOR_SIZE, 1024, 12345,
        DISKIMAGE_D64_SIZE - 1, DISKIMAGE_D64_SIZE + 1,
        DISKIMAGE_D80_SIZE - 1, DISKIMAGE_D80_SIZE + 1,
        DISKIMAGE_D82_SIZE - 1, DISKIMAGE_D82_SIZE + 1,
    };
    diskimage_t image;
    for (size_t index = 0; index < count_of(sizes); ++index) {
        ck_assert(!diskimage_open(&image, NULL, NULL, sizes[index]));
        ck_assert_int_eq(image.type, diskimage_type_none);
    }
    ck_assert(diskimage_open(&image, NULL, NULL, DISKIMAGE_D64_SIZE));
    ck_assert_int_eq(image.type, diskimage_type_d64);
    ck_assert(diskimage_open(&image, NULL, NULL, DISKIMAGE_D80_SIZE));
    ck_assert_int_eq(image.type, diskimage_type_d80);
    ck_assert(diskimage_open(&image, NULL, NULL, DISKIMAGE_D82_SIZE));
    ck_assert_int_eq(image.type, diskimage_type_d82);
}
END_TEST

// Literal ROM zone expectations do not reuse the implementation's tables.
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

// Both 8250 sides cover exactly 4,166 sectors without geometry holes.
START_TEST(test_d82_geometry_covers_two_exact_native_sides) {
    uint32_t expected_offset = 0;
    for (unsigned int track = 1; track <= DISKIMAGE_D82_TRACK_COUNT; ++track) {
        const unsigned int side_track = (track - 1) % DISKIMAGE_D80_TRACK_COUNT + 1;
        const unsigned int expected_sectors = side_track <= 39 ? 29
            : side_track <= 53 ? 27 : side_track <= 64 ? 25 : 23;
        unsigned int sectors;
        ck_assert(diskimage_track_sectors(diskimage_type_d82, track, &sectors));
        ck_assert_uint_eq(sectors, expected_sectors);
        for (unsigned int sector = 0; sector < sectors; ++sector) {
            uint32_t offset;
            ck_assert(diskimage_sector_offset(diskimage_type_d82, track, sector, &offset));
            ck_assert_uint_eq(offset, expected_offset);
            expected_offset += DISKIMAGE_SECTOR_SIZE;
        }
        uint32_t unchanged = UINT32_MAX;
        ck_assert(!diskimage_sector_offset(diskimage_type_d82, track, sectors, &unchanged));
        ck_assert_uint_eq(unchanged, UINT32_MAX);
        if (track == DISKIMAGE_D80_TRACK_COUNT)
            ck_assert_uint_eq(expected_offset, DISKIMAGE_D80_SIZE);
    }
    ck_assert_uint_eq(expected_offset, DISKIMAGE_D82_SIZE);
}
END_TEST

// Failed geometry queries preserve outputs, including extreme unsigned inputs.
START_TEST(test_disk_geometry_rejects_invalid_inputs) {
    static const struct {
        diskimage_type_t type;
        unsigned int track;
    } invalid[] = {
        {diskimage_type_none, 1},
        {diskimage_type_d64, 0}, {diskimage_type_d64, 36}, {diskimage_type_d64, UINT_MAX},
        {diskimage_type_d80, 0}, {diskimage_type_d80, 78}, {diskimage_type_d80, UINT_MAX},
        {diskimage_type_d82, 0}, {diskimage_type_d82, 155}, {diskimage_type_d82, UINT_MAX},
    };
    for (size_t index = 0; index < count_of(invalid); ++index) {
        unsigned int sectors = UINT_MAX;
        uint32_t offset = UINT32_MAX;
        ck_assert(!diskimage_track_sectors(invalid[index].type, invalid[index].track, &sectors));
        ck_assert_uint_eq(sectors, UINT_MAX);
        ck_assert(!diskimage_sector_offset(invalid[index].type, invalid[index].track, 0, &offset));
        ck_assert_uint_eq(offset, UINT32_MAX);
    }
    uint32_t offset = UINT32_MAX;
    ck_assert(!diskimage_sector_offset(diskimage_type_d64, 1, UINT_MAX, &offset));
    ck_assert_uint_eq(offset, UINT32_MAX);
    ck_assert(!diskimage_track_sectors(diskimage_type_d64, 1, NULL));
    ck_assert(!diskimage_sector_offset(diskimage_type_d64, 1, 0, NULL));
}
END_TEST

Suite* diskimage_suite(void) {
    Suite* suite = suite_create("diskimage");
    TCase* test_case = tcase_create("geometry");
    tcase_add_test(test_case, test_open_recognizes_only_exact_sizes);
    tcase_add_test(test_case, test_disk_geometry_matches_rom_zones);
    tcase_add_test(test_case, test_d82_geometry_covers_two_exact_native_sides);
    tcase_add_test(test_case, test_disk_geometry_rejects_invalid_inputs);
    suite_add_tcase(suite, test_case);
    return suite;
}

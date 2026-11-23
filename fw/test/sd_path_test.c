// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"

#include <check.h>

#include "ff.h"

// diskio.h requires the FatFs integer types from ff.h.
#include "diskio.h"

#include "sd/sd.h"

#define TEST_SECTOR_SIZE 512u
#define TEST_SECTOR_COUNT 8192u
#define TEST_LONG_COMPONENT_LENGTH 120u
#define FAT_DATE_MONTH_SHIFT 21u
#define FAT_DATE_DAY_SHIFT 16u

static uint8_t disk[TEST_SECTOR_COUNT * TEST_SECTOR_SIZE];
static FATFS filesystem;

// Exposes the in-memory card to the pinned production FatFs implementation.
DSTATUS disk_initialize(BYTE drive) { return drive == 0 ? 0 : STA_NOINIT; }

// The fixture's only card remains present throughout each case.
DSTATUS disk_status(BYTE drive) { return disk_initialize(drive); }

// Reads independently addressable sectors from the fixture card.
DRESULT disk_read(BYTE drive, BYTE* output, LBA_t sector, UINT count) {
    if (drive != 0 || sector >= TEST_SECTOR_COUNT || count > TEST_SECTOR_COUNT - sector) {
        return RES_PARERR;
    }
    memcpy(output, disk + sector * TEST_SECTOR_SIZE, count * TEST_SECTOR_SIZE);
    return RES_OK;
}

// Persists sectors so directory lookup uses real FAT entries and generated short names.
DRESULT disk_write(BYTE drive, const BYTE* input, LBA_t sector, UINT count) {
    if (drive != 0 || sector >= TEST_SECTOR_COUNT || count > TEST_SECTOR_COUNT - sector) {
        return RES_PARERR;
    }
    memcpy(disk + sector * TEST_SECTOR_SIZE, input, count * TEST_SECTOR_SIZE);
    return RES_OK;
}

// Reports fixed card geometry and synchronous persistence to FatFs.
DRESULT disk_ioctl(BYTE drive, BYTE command, void* output) {
    if (drive != 0) {
        return RES_PARERR;
    }
    switch (command) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t*)output = TEST_SECTOR_COUNT;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD*)output = TEST_SECTOR_SIZE;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD*)output = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

// Uses the earliest valid FAT date for deterministic directory metadata.
DWORD get_fattime(void) { return (1u << FAT_DATE_MONTH_SHIFT) | (1u << FAT_DATE_DAY_SHIFT); }

// Creates a real FAT volume with a disk directory and long-name parents.
static void setup(void) {
    memset(disk, 0, sizeof(disk));
    uint8_t work[FF_MAX_SS];
    const MKFS_PARM options = {.fmt = FM_FAT | FM_SFD};
    ck_assert_int_eq(f_mkfs("", &options, work, sizeof(work)), FR_OK);
    ck_assert_int_eq(f_mount(&filesystem, "", 1), FR_OK);
    ck_assert_int_eq(f_mkdir("/disk"), FR_OK);
    ck_assert_int_eq(f_mkdir("/disk/Long Directory"), FR_OK);
    ck_assert_int_eq(f_mkdir("/rom"), FR_OK);
}

// Releases the volume before the next case reformats its card.
static void teardown(void) { ck_assert_int_eq(f_mount(NULL, "", 0), FR_OK); }

// Creates only the backing name needed by these path-resolution contracts.
static void create_file(const char* path) {
    FIL file;
    ck_assert_int_eq(f_open(&file, path, FA_CREATE_NEW | FA_WRITE), FR_OK);
    ck_assert_int_eq(f_close(&file), FR_OK);
}

// Directory aliases, leaf aliases, and trimmed suffixes resolve to the same stored spelling.
START_TEST(test_resolves_fatfs_aliases) {
    const char* const canonical = "/disk/Long Directory/Disk Image.d64";
    create_file(canonical);
    FILINFO parent, leaf;
    ck_assert_int_eq(f_stat("/disk/Long Directory", &parent), FR_OK);
    ck_assert_int_eq(f_stat(canonical, &leaf), FR_OK);
    ck_assert_str_ne(parent.altname, "");
    ck_assert_str_ne(leaf.altname, "");
    char path[SD_PATH_MAX];
    snprintf(path, sizeof(path), "/DISK/%s/%s", parent.altname, leaf.altname);
    ck_assert(sd_resolve_path(path));
    ck_assert_str_eq(path, canonical);
    snprintf(path, sizeof(path), "%s. ", canonical);
    ck_assert(sd_resolve_path(path));
    ck_assert_str_eq(path, canonical);
}
END_TEST

// The longest supported full path expands from an alias without losing its last byte.
START_TEST(test_resolves_maximum_full_path) {
    static const char prefix[] = "/disk/Long Directory/";
    const size_t length = SD_PATH_MAX - sizeof(prefix);
    char name[SD_PATH_MAX];
    memset(name, 'a', length);
    memcpy(name + length - strlen(".d64"), ".d64", sizeof(".d64"));
    char canonical[SD_PATH_MAX];
    memcpy(canonical, prefix, sizeof(prefix) - 1);
    memcpy(canonical + sizeof(prefix) - 1, name, length + 1);
    ck_assert_uint_eq(strlen(canonical), SD_PATH_MAX - 1);
    create_file(canonical);
    FILINFO info;
    ck_assert_int_eq(f_stat(canonical, &info), FR_OK);
    char path[SD_PATH_MAX];
    snprintf(path, sizeof(path), "%s%s", prefix, info.altname);
    ck_assert(sd_resolve_path(path));
    ck_assert_str_eq(path, canonical);
}
END_TEST

// Missing components and malformed CP932 do not become aliases of a valid sibling.
START_TEST(test_failed_resolution_then_valid_lookup) {
    static const char* const invalid[] = {
        "/disk/Missing Directory/image.d64",
        "/disk/Long Directory/missing.d64",
        "/disk/Long Directory/image.d64/child",
        "/disk/\x81",
        "/disk/\x81\x30.d64",
    };
    static const char canonical[] = "/disk/Long Directory/Image.d64";
    create_file(canonical);
    for (size_t index = 0; index < count_of(invalid); ++index) {
        char path[SD_PATH_MAX];
        strcpy(path, invalid[index]);
        ck_assert(!sd_resolve_path(path));
        strcpy(path, "/DISK/long directory/IMAGE.D64");
        ck_assert(sd_resolve_path(path));
        ck_assert_str_eq(path, canonical);
    }
}
END_TEST

// FatFs accepts these spellings, but the slash-delimited host contract must reject them.
START_TEST(test_rejects_alternate_separators_and_terminators) {
    create_file("/rom/outside.d64");
    create_file("/disk/disk.d64");
    const char* const aliases[] = {"/disk/..\\rom\\outside.d64", "/disk\\disk.d64", "/disk/disk.d64\rignored"};
    for (size_t index = 0; index < count_of(aliases); ++index) {
        FILINFO info;
        ck_assert_int_eq(f_stat(aliases[index], &info), FR_OK);
        char path[SD_PATH_MAX];
        strcpy(path, aliases[index]);
        ck_assert(!sd_resolve_path(path));
    }
}
END_TEST

// A CP932 backslash trail is a filename byte, not an alternate directory separator.
START_TEST(test_preserves_cp932_filename_bytes) {
    const char* const canonical = "/disk/\x81\x5c.d64";
    create_file(canonical);
    char path[SD_PATH_MAX];
    strcpy(path, canonical);
    ck_assert(sd_resolve_path(path));
    ck_assert_str_eq(path, canonical);
}
END_TEST

// Short input aliases must not overflow the bounded buffer when their long names expand.
START_TEST(test_rejects_expanded_path_overflow) {
    char name[TEST_LONG_COMPONENT_LENGTH + 1];
    memset(name, 'a', sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    char parent[SD_PATH_MAX];
    snprintf(parent, sizeof(parent), "/disk/%s", name);
    ck_assert_int_eq(f_mkdir(parent), FR_OK);
    FILINFO first;
    ck_assert_int_eq(f_stat(parent, &first), FR_OK);
    char child[2 * SD_PATH_MAX];
    snprintf(child, sizeof(child), "%s/%s", parent, name);
    ck_assert_int_eq(f_mkdir(child), FR_OK);
    FILINFO second;
    ck_assert_int_eq(f_stat(child, &second), FR_OK);
    char filename[sizeof(child) + sizeof("/disk.d64")];
    snprintf(filename, sizeof(filename), "%s/disk.d64", child);
    create_file(filename);
    char path[SD_PATH_MAX];
    snprintf(path, sizeof(path), "/disk/%s/%s/disk.d64", first.altname, second.altname);
    ck_assert(!sd_resolve_path(path));
}
END_TEST

// Runs the production SD resolver against the pinned filesystem, separately from stdio fixtures.
int main(void) {
    Suite* const suite = suite_create("SD paths");
    TCase* const cases = tcase_create("FatFs");
    tcase_add_checked_fixture(cases, setup, teardown);
    tcase_add_test(cases, test_resolves_fatfs_aliases);
    tcase_add_test(cases, test_resolves_maximum_full_path);
    tcase_add_test(cases, test_failed_resolution_then_valid_lookup);
    tcase_add_test(cases, test_rejects_alternate_separators_and_terminators);
    tcase_add_test(cases, test_preserves_cp932_filename_bytes);
    tcase_add_test(cases, test_rejects_expanded_path_overflow);
    suite_add_tcase(suite, cases);
    SRunner* const runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    const int failures = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

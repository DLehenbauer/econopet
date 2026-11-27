// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"

#include <check.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

#include "blockdevice/heap.h"
#include "filesystem/fat.h"

#include "diag/diag.h"
#include "diag/drive.h"
#include "diag/target.h"
#include "drive-test/drive_test_catalog.h"
#include "driver.h"
#include "ieee/diskimage.h"
#include "ieee/diskimage_file.h"
#include "ieee/ieee_drive.h"
#include "menu/menu.h"
#include "menu/menu_browser.h"
#include "pet.h"
#include "sd/sd.h"
#include "sd_diag_mock.h"

#define DRIVE_DIAG_TEST_CARD_BYTES (16u * 1024u * 1024u)

static const char* const scratch_paths[DT_SLOT_COUNT] = {
    "/test/tmp/u8d0.d64",  "/test/tmp/u8d1.d64",  "/test/tmp/u9d0.d80",  "/test/tmp/u9d1.d80",
    "/test/tmp/u10d0.d82", "/test/tmp/u10d1.d82", "/test/tmp/u11d0.d64", "/test/tmp/u11d1.d64",
};

static const diskimage_type_t expected_types[DT_SLOT_COUNT] = {
    diskimage_type_d64, diskimage_type_d64, diskimage_type_d80, diskimage_type_d80,
    diskimage_type_d82, diskimage_type_d82, diskimage_type_d64, diskimage_type_d64,
};

static const uint32_t expected_sizes[DT_SLOT_COUNT] = {
    DISKIMAGE_D64_SIZE, DISKIMAGE_D64_SIZE, DISKIMAGE_D80_SIZE, DISKIMAGE_D80_SIZE,
    DISKIMAGE_D82_SIZE, DISKIMAGE_D82_SIZE, DISKIMAGE_D64_SIZE, DISKIMAGE_D64_SIZE,
};

typedef enum {
    CLEANUP_FAILURE_NONE,
    CLEANUP_FAILURE_OPEN,
    CLEANUP_FAILURE_READ,
    CLEANUP_FAILURE_CLOSE,
    CLEANUP_FAILURE_REMOVE,
    CLEANUP_FAILURE_RMDIR,
    CLEANUP_FAILURE_COUNT,
} cleanup_failure_t;

static blockdevice_t* card;
static filesystem_t* fat;
static int (*real_stat)(filesystem_t* fs, const char* path, struct stat* st);
static int (*real_dir_open)(filesystem_t* fs, fs_dir_t* dir, const char* path);
static int (*real_dir_read)(filesystem_t* fs, fs_dir_t* dir, struct dirent* entry);
static int (*real_dir_close)(filesystem_t* fs, fs_dir_t* dir);
static int (*real_remove)(filesystem_t* fs, const char* path);
static int (*real_rmdir)(filesystem_t* fs, const char* path);

static cleanup_failure_t cleanup_failure;
static bool overlong_entry;
static const char* stat_error_path;
static bool mounted[DT_SLOT_COUNT];
static char mounted_paths[DT_SLOT_COUNT][SD_PATH_MAX];
static bool unmount_succeeds;
static bool scan_close_succeeds;
static bool target_prepare_succeeds;
static bool profile_exists;
static bool profile_loaded;
static int fail_create_slot;
static int fail_mount_slot;
static unsigned storage_wait_calls;
static unsigned cpu_halt_calls;
static unsigned scan_close_calls;
static unsigned unmount_calls;
static unsigned create_calls;
static unsigned mount_calls;
static unsigned target_prepare_calls;
static unsigned successful_removes;
static unsigned remove_calls;
static unsigned rmdir_calls;
static bool target_prepare_saw_mounts;

bool __real_diskimage_create_file(const char* path, diskimage_type_t type);

/** Check whether a path exists on the mounted FAT card as a regular file. */
static bool path_is_file(const char* path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

/** Check whether a path exists on the mounted FAT card as a directory. */
static bool path_is_dir(const char* path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

/** Check whether a path is absent on the mounted FAT card with ENOENT. */
static bool path_is_absent(const char* path) {
    struct stat info;
    return stat(path, &info) != 0 && errno == ENOENT;
}

/** Check whether /test/tmp is absent, accounting for FatFs ENOTDIR when /test is absent. */
static bool scratch_dir_is_absent(void) {
    struct stat info;
    if (path_is_absent("/test")) {
        return stat("/test/tmp", &info) != 0 && errno == ENOTDIR;
    }
    return path_is_absent("/test/tmp");
}

/** Write a one-byte file on the FAT fixture for conflict and purge checks. */
static void write_small_file(const char* path) {
    FILE* const file = fopen(path, "wb");
    ck_assert_ptr_nonnull(file);
    static const uint8_t byte = 0x5au;
    ck_assert_uint_eq(fwrite(&byte, 1, sizeof(byte), file), sizeof(byte));
    ck_assert_int_eq(fclose(file), 0);
}

/** Inject targeted metadata errors while delegating all other lookups to real FatFs. */
static int observe_stat(filesystem_t* fs, const char* path, struct stat* st) {
    if (stat_error_path != NULL && strcmp(path, stat_error_path) == 0) {
        return -EIO;
    }
    return real_stat(fs, path, st);
}

/** Inject directory-open errors while delegating normal opens to real FatFs. */
static int observe_dir_open(filesystem_t* fs, fs_dir_t* dir, const char* path) {
    if (cleanup_failure == CLEANUP_FAILURE_OPEN) {
        return -EIO;
    }
    return real_dir_open(fs, dir, path);
}

/** Inject read errors or overlong names while delegating normal reads to real FatFs. */
static int observe_dir_read(filesystem_t* fs, fs_dir_t* dir, struct dirent* entry) {
    if (cleanup_failure == CLEANUP_FAILURE_READ) {
        return -EIO;
    }
    if (overlong_entry) {
        memset(entry->d_name, 'x', sizeof(entry->d_name) - 1u);
        entry->d_name[sizeof(entry->d_name) - 1u] = '\0';
        entry->d_type = DT_REG;
        return 0;
    }
    return real_dir_read(fs, dir, entry);
}

/** Release the underlying FatFs directory even when simulating a close failure. */
static int observe_dir_close(filesystem_t* fs, fs_dir_t* dir) {
    const int rc = real_dir_close(fs, dir);
    return cleanup_failure == CLEANUP_FAILURE_CLOSE ? -EIO : rc;
}

/** Count all file removals uniformly and verify no handles or mounts remain open. */
static int observe_remove(filesystem_t* fs, const char* path) {
    ck_assert_uint_eq(sd_diag_mock_open_handles(), 0);
    for (size_t slot = 0; slot < count_of(mounted); ++slot) {
        ck_assert(!mounted[slot]);
    }
    ++remove_calls;
    if (cleanup_failure == CLEANUP_FAILURE_REMOVE) {
        return -EIO;
    }
    const int rc = real_remove(fs, path);
    if (rc == 0) {
        ++successful_removes;
    }
    return rc;
}

/** Count all directory removals uniformly and verify no handles remain open. */
static int observe_rmdir(filesystem_t* fs, const char* path) {
    ck_assert_uint_eq(sd_diag_mock_open_handles(), 0);
    ++rmdir_calls;
    if (cleanup_failure == CLEANUP_FAILURE_RMDIR) {
        return -EIO;
    }
    return real_rmdir(fs, path);
}

/** Restore uninstrumented FatFs callbacks before tearing down the root mount. */
static void restore_fat_ops(void) {
    fat->stat = real_stat;
    fat->dir_open = real_dir_open;
    fat->dir_read = real_dir_read;
    fat->dir_close = real_dir_close;
    fat->remove = real_remove;
    fat->rmdir = real_rmdir;
}

/** Reset deletion counters after seeding test-specific filesystem fixtures. */
static void reset_removal_counts(void) {
    successful_removes = 0;
    remove_calls = 0;
    rmdir_calls = 0;
}

/** Format and mount a fresh heap-backed FAT card with /test/drive-test.prg and /test/tmp. */
static void setup(void) {
    memset(mounted, 0, sizeof(mounted));
    memset(mounted_paths, 0, sizeof(mounted_paths));
    cleanup_failure = CLEANUP_FAILURE_NONE;
    overlong_entry = false;
    stat_error_path = NULL;
    unmount_succeeds = true;
    scan_close_succeeds = true;
    target_prepare_succeeds = true;
    profile_exists = true;
    profile_loaded = false;
    fail_create_slot = -1;
    fail_mount_slot = -1;
    storage_wait_calls = 0;
    cpu_halt_calls = 0;
    scan_close_calls = 0;
    unmount_calls = 0;
    create_calls = 0;
    mount_calls = 0;
    target_prepare_calls = 0;
    target_prepare_saw_mounts = false;

    card = blockdevice_heap_create(DRIVE_DIAG_TEST_CARD_BYTES);
    ck_assert_ptr_nonnull(card);
    ck_assert_int_eq(card->init(card), BD_ERROR_OK);
    fat = filesystem_fat_create();
    ck_assert_ptr_nonnull(fat);
    sd_diag_mock_bind(card, fat);
    ck_assert_int_eq(fat->format(fat, card), 0);
    ck_assert(sd_mount_root());

    ck_assert_int_eq(mkdir("/test", 0777), 0);
    write_small_file("/test/drive-test.prg");
    ck_assert_int_eq(mkdir("/test/tmp", 0777), 0);

    real_stat = fat->stat;
    real_dir_open = fat->dir_open;
    real_dir_read = fat->dir_read;
    real_dir_close = fat->dir_close;
    real_remove = fat->remove;
    real_rmdir = fat->rmdir;
    fat->stat = observe_stat;
    fat->dir_open = observe_dir_open;
    fat->dir_read = observe_dir_read;
    fat->dir_close = observe_dir_close;
    fat->remove = observe_remove;
    fat->rmdir = observe_rmdir;
    reset_removal_counts();
}

/** Release the FAT fixture before asserting that the test left no leaked VFS handles. */
static void teardown(void) {
    cleanup_failure = CLEANUP_FAILURE_NONE;
    stat_error_path = NULL;
    overlong_entry = false;
    const size_t leaked = sd_diag_mock_open_handles();
    if (fat != NULL) {
        restore_fat_ops();
    }
    sd_diag_mock_cleanup();
    if (fat != NULL) {
        filesystem_fat_free(fat);
        fat = NULL;
    }
    if (card != NULL) {
        blockdevice_heap_free(card);
        card = NULL;
    }
    ck_assert_uint_eq(leaked, 0);
}

/** Observe storage-idle waits without running hardware service loops. */
void pet_wait_for_storage_idle(void) {
    ++storage_wait_calls;
}

/** Observe CPU halts without writing FPGA registers. */
void set_cpu(cpu_state_t state) {
    ck_assert_int_eq(state, CPU_HALT);
    ++cpu_halt_calls;
}

/** Model closing the active menu scan before drive preparation. */
bool menu_browser_abandon_scan(void) {
    ++scan_close_calls;
    return scan_close_succeeds;
}

/** Model owner-preserving IEEE unmount success or failure. */
bool ieee_drive_unmount_all(void) {
    ++unmount_calls;
    if (!unmount_succeeds) {
        return false;
    }
    memset(mounted, 0, sizeof(mounted));
    memset(mounted_paths, 0, sizeof(mounted_paths));
    return true;
}

/** Delegate blank-image creation to production code unless injecting a slot failure. */
bool __wrap_diskimage_create_file(const char* path, diskimage_type_t type) {
    ck_assert(profile_loaded);
    ck_assert(path_is_dir("/test"));
    ck_assert(path_is_dir("/test/tmp"));
    ck_assert_uint_lt(create_calls, DT_SLOT_COUNT);
    const size_t slot = create_calls++;
    ck_assert_str_eq(path, scratch_paths[slot]);
    ck_assert_int_eq(type, expected_types[slot]);
    if ((int)slot == fail_create_slot) {
        return false;
    }
    return __real_diskimage_create_file(path, type);
}

/** Track successful scratch mounts against real FAT images and inject partial-setup failures. */
dos_media_result_t ieee_drive_mount_scratch(const unsigned int slot, const char* path) {
    ck_assert_uint_lt(slot, DT_SLOT_COUNT);
    ++mount_calls;
    ck_assert_str_eq(path, scratch_paths[slot]);
    if ((int)slot == fail_mount_slot) {
        return DOS_MEDIA_IO_ERROR;
    }
    ck_assert(path_is_file(path));
    mounted[slot] = true;
    snprintf(mounted_paths[slot], sizeof(mounted_paths[slot]), "%s", path);
    return DOS_MEDIA_OK;
}

/** Require scratch purging and unmounting to complete before profile application. */
bool menu_apply_diag_config(const char* id) {
    ck_assert_str_eq(id, "pet-80xx-60hz");
    ck_assert_uint_eq(create_calls, 0);
    ck_assert_uint_eq(mount_calls, 0);
    ck_assert(scratch_dir_is_absent());
    for (size_t slot = 0; slot < count_of(mounted); ++slot) {
        ck_assert(!mounted[slot]);
    }
    profile_loaded = profile_exists;
    return profile_loaded;
}

/** Verify target reset/boot sees the profile and all intended FAT-backed scratch mounts. */
bool diag_target_prepare_pet_prg(const char* prg_path, char* error_out, size_t error_size) {
    ++target_prepare_calls;
    ck_assert(profile_loaded);
    ck_assert_str_eq(prg_path, "/test/drive-test.prg");
    target_prepare_saw_mounts = true;
    for (size_t slot = 0; slot < count_of(mounted); ++slot) {
        target_prepare_saw_mounts = target_prepare_saw_mounts && mounted[slot] &&
                                    path_is_file(scratch_paths[slot]) &&
                                    strcmp(mounted_paths[slot], scratch_paths[slot]) == 0;
    }
    if (!target_prepare_succeeds) {
        snprintf(error_out, error_size, "fixture target preparation failed");
        return false;
    }
    return true;
}

/** Return the mounted path observed by drive_prepare's post-boot invariant. */
const char* ieee_drive_filename(const unsigned int slot) {
    ck_assert_uint_lt(slot, DT_SLOT_COUNT);
    return mounted[slot] ? mounted_paths[slot] : "";
}

/** Fail explicitly if runtime execution hooks are invoked in this setup/teardown suite. */
void ieee_drive_task(void) {
    ck_abort_msg("unexpected ieee_drive_task in drive setup fixture");
}

/** Fail explicitly if per-cycle target execution is invoked in this setup/teardown suite. */
void diag_target_begin_cycle(uint8_t cycle) {
    (void)cycle;
    ck_abort_msg("unexpected diag_target_begin_cycle in drive setup fixture");
}

/** Fail explicitly if target mailbox polling is invoked in this setup/teardown suite. */
void diag_target_await_case(const diag_target_runner_t* runner, size_t check_ordinal) {
    (void)runner;
    (void)check_ordinal;
    ck_abort_msg("unexpected diag_target_await_case in drive setup fixture");
}

/** Prepare and clean up eight correctly sized, mounted scratch containers on real FAT. */
START_TEST(test_drive_prepare_and_cleanup) {
    char error[DIAG_MESSAGE_MAX] = "";

    ck_assert(diag_plan_drive.prepare(error, sizeof(error)));
    ck_assert_uint_eq(storage_wait_calls, 1);
    ck_assert_uint_eq(cpu_halt_calls, 1);
    ck_assert_uint_eq(scan_close_calls, 1);
    ck_assert_uint_eq(unmount_calls, 1);
    ck_assert_uint_eq(create_calls, DT_SLOT_COUNT);
    ck_assert_uint_eq(mount_calls, DT_SLOT_COUNT);
    ck_assert_uint_eq(target_prepare_calls, 1);
    ck_assert(target_prepare_saw_mounts);
    for (size_t slot = 0; slot < count_of(scratch_paths); ++slot) {
        struct stat info;
        ck_assert_int_eq(stat(scratch_paths[slot], &info), 0);
        ck_assert(S_ISREG(info.st_mode));
        ck_assert_uint_eq((uint32_t)info.st_size, expected_sizes[slot]);
        ck_assert(mounted[slot]);
    }

    ck_assert(diag_plan_drive.cleanup(error, sizeof(error)));
    ck_assert_uint_eq(unmount_calls, 2);
    ck_assert_uint_eq(successful_removes, DT_SLOT_COUNT);
    ck_assert(path_is_absent("/test/tmp"));
    ck_assert(path_is_file("/test/drive-test.prg"));
    for (size_t slot = 0; slot < count_of(mounted); ++slot) {
        ck_assert(!mounted[slot]);
    }
}
END_TEST

/** Release created files and earlier mounts after a later slot fails to mount. */
START_TEST(test_drive_prepare_failure_cleans_partial_resources) {
    char error[DIAG_MESSAGE_MAX] = "";
    fail_mount_slot = 3;

    ck_assert(!diag_plan_drive.prepare(error, sizeof(error)));
    ck_assert_ptr_nonnull(strstr(error, "failed to mount"));
    ck_assert_uint_eq(create_calls, DT_SLOT_COUNT);
    ck_assert_uint_eq(mount_calls, 4);
    ck_assert_uint_eq(target_prepare_calls, 0);

    ck_assert(diag_plan_drive.cleanup(error, sizeof(error)));
    ck_assert_uint_eq(unmount_calls, 2);
    ck_assert_uint_eq(successful_removes, DT_SLOT_COUNT);
    ck_assert(path_is_absent("/test/tmp"));
    for (size_t slot = 0; slot < count_of(mounted); ++slot) {
        ck_assert(!mounted[slot]);
    }
}
END_TEST

/** Remove containers created before a later blank-image creation fails. */
START_TEST(test_drive_prepare_creation_failure_cleans_partial_resources) {
    char error[DIAG_MESSAGE_MAX] = "";
    fail_create_slot = 4;

    ck_assert(!diag_plan_drive.prepare(error, sizeof(error)));
    ck_assert_ptr_nonnull(strstr(error, "failed to create"));
    ck_assert_uint_eq(create_calls, 5);
    ck_assert_uint_eq(mount_calls, 0);

    ck_assert(diag_plan_drive.cleanup(error, sizeof(error)));
    ck_assert_uint_eq(successful_removes, 4);
    ck_assert(path_is_absent("/test/tmp"));
}
END_TEST

/** A missing profile must prevent scratch allocation and target boot. */
START_TEST(test_drive_prepare_missing_profile_creates_no_resources) {
    char error[DIAG_MESSAGE_MAX] = "";
    profile_exists = false;

    ck_assert(!diag_plan_drive.prepare(error, sizeof(error)));
    ck_assert_ptr_nonnull(strstr(error, "missing config"));
    ck_assert_uint_eq(create_calls, 0);
    ck_assert_uint_eq(mount_calls, 0);
    ck_assert_uint_eq(target_prepare_calls, 0);
    ck_assert(diag_plan_drive.cleanup(error, sizeof(error)));
    ck_assert_uint_eq(successful_removes, 0);
}
END_TEST

/** Remove all scratch resources when target boot or PRG preparation fails. */
START_TEST(test_drive_prepare_target_failure_cleans_resources) {
    char error[DIAG_MESSAGE_MAX] = "";
    target_prepare_succeeds = false;

    ck_assert(!diag_plan_drive.prepare(error, sizeof(error)));
    ck_assert_ptr_nonnull(strstr(error, "fixture target preparation failed"));
    ck_assert(target_prepare_saw_mounts);
    ck_assert_uint_eq(mount_calls, DT_SLOT_COUNT);

    ck_assert(diag_plan_drive.cleanup(error, sizeof(error)));
    ck_assert_uint_eq(successful_removes, DT_SLOT_COUNT);
    ck_assert(path_is_absent("/test/tmp"));
    for (size_t slot = 0; slot < count_of(mounted); ++slot) {
        ck_assert(!mounted[slot]);
    }
}
END_TEST

/** Keep every backing image intact when the IEEE facade cannot confirm unmount. */
START_TEST(test_drive_cleanup_retains_files_on_unmount_failure) {
    char error[DIAG_MESSAGE_MAX] = "";
    ck_assert(diag_plan_drive.prepare(error, sizeof(error)));
    reset_removal_counts();
    unmount_succeeds = false;

    ck_assert(!diag_plan_drive.cleanup(error, sizeof(error)));
    ck_assert_ptr_nonnull(strstr(error, "scratch files were retained"));
    ck_assert_uint_eq(remove_calls, 0);
    ck_assert_uint_eq(rmdir_calls, 0);
    for (size_t slot = 0; slot < count_of(scratch_paths); ++slot) {
        ck_assert(path_is_file(scratch_paths[slot]));
        ck_assert(mounted[slot]);
    }
}
END_TEST

/** Abort preparation if the active menu scan could not be closed. */
START_TEST(test_drive_prepare_rejects_scan_close_failure) {
    char error[DIAG_MESSAGE_MAX] = "";
    scan_close_succeeds = false;

    ck_assert(!diag_plan_drive.prepare(error, sizeof(error)));
    ck_assert_ptr_nonnull(strstr(error, "could not close the active disk scan"));
    ck_assert_uint_eq(unmount_calls, 0);
    ck_assert_uint_eq(create_calls, 0);
    ck_assert(diag_plan_drive.cleanup(error, sizeof(error)));
}
END_TEST

/** Fresh cards and repeated cleanup must not open or unlink children of an absent directory on FatFs. */
START_TEST(test_drive_missing_scratch_directories) {
    char error[DIAG_MESSAGE_MAX] = "";
    ck_assert_int_eq(rmdir("/test/tmp"), 0);
    if (_i == 0) {
        ck_assert_int_eq(remove("/test/drive-test.prg"), 0);
        ck_assert_int_eq(rmdir("/test"), 0);
    }
    reset_removal_counts();

    ck_assert(diag_plan_drive.cleanup(error, sizeof(error)));
    ck_assert_uint_eq(remove_calls, 0);
    ck_assert_uint_eq(rmdir_calls, 0);
    ck_assert(diag_plan_drive.prepare(error, sizeof(error)));
    ck_assert(target_prepare_saw_mounts);
    ck_assert_uint_eq(remove_calls, 0);
    ck_assert_uint_eq(rmdir_calls, 0);
    ck_assert(diag_plan_drive.cleanup(error, sizeof(error)));
    ck_assert_uint_eq(successful_removes, DT_SLOT_COUNT);
    ck_assert_uint_eq(rmdir_calls, 1);
    ck_assert(diag_plan_drive.cleanup(error, sizeof(error)));
    ck_assert_uint_eq(remove_calls, DT_SLOT_COUNT);
    ck_assert_uint_eq(rmdir_calls, 1);
    if (_i != 0) {
        ck_assert(path_is_file("/test/drive-test.prg"));
    }
}
END_TEST

/** A real non-directory ancestor on FatFs must remain an error, not be mistaken for missing storage. */
START_TEST(test_drive_rejects_scratch_directory_conflicts) {
    char error[DIAG_MESSAGE_MAX] = "";
    ck_assert_int_eq(rmdir("/test/tmp"), 0);
    if (_i == 0) {
        ck_assert_int_eq(remove("/test/drive-test.prg"), 0);
        ck_assert_int_eq(rmdir("/test"), 0);
        write_small_file("/test");
    } else {
        write_small_file("/test/tmp");
    }
    reset_removal_counts();

    ck_assert(!diag_plan_drive.prepare(error, sizeof(error)));
    ck_assert_ptr_nonnull(strstr(error, "not a directory"));
    ck_assert(!diag_plan_drive.cleanup(error, sizeof(error)));
    ck_assert_uint_eq(remove_calls, 0);
    ck_assert_uint_eq(rmdir_calls, 0);
    ck_assert_uint_eq(create_calls, 0);
    ck_assert(path_is_file(_i == 0 ? "/test" : "/test/tmp"));
}
END_TEST

/** Failed metadata reads must not authorize deletion or scratch replacement. */
START_TEST(test_drive_retains_scratch_on_stat_error) {
    char error[DIAG_MESSAGE_MAX] = "";
    write_small_file(scratch_paths[0]);
    reset_removal_counts();
    stat_error_path = _i == 0 ? "/test" : "/test/tmp";

    ck_assert(!diag_plan_drive.prepare(error, sizeof(error)));
    ck_assert_ptr_nonnull(strstr(error, "could not inspect"));
    ck_assert(!diag_plan_drive.cleanup(error, sizeof(error)));
    stat_error_path = NULL;
    ck_assert(path_is_file(scratch_paths[0]));
    ck_assert_uint_eq(remove_calls, 0);
    ck_assert_uint_eq(rmdir_calls, 0);
    ck_assert_uint_eq(create_calls, 0);
}
END_TEST

/** Preparation and teardown must purge unknown files and nested directories while preserving /test assets. */
START_TEST(test_drive_purges_entire_scratch_tree) {
    char error[DIAG_MESSAGE_MAX] = "";
    write_small_file(scratch_paths[0]);
    write_small_file("/test/tmp/obsolete.bin");
    ck_assert_int_eq(mkdir("/test/tmp/old", 0777), 0);
    ck_assert_int_eq(mkdir("/test/tmp/old/nested", 0777), 0);
    write_small_file("/test/tmp/old/nested/unknown.img");
    reset_removal_counts();

    if (_i != 0) {
        ck_assert(diag_plan_drive.prepare(error, sizeof(error)));
        ck_assert(target_prepare_saw_mounts);
    }
    ck_assert(diag_plan_drive.cleanup(error, sizeof(error)));
    ck_assert(path_is_dir("/test"));
    ck_assert(path_is_file("/test/drive-test.prg"));
    ck_assert(path_is_absent("/test/tmp"));
    ck_assert_uint_eq(successful_removes, _i != 0 ? DT_SLOT_COUNT + 3u : 3u);
    ck_assert_uint_eq(remove_calls, successful_removes);
    ck_assert_uint_eq(rmdir_calls, _i != 0 ? 4u : 3u);
}
END_TEST

/** Storage failures must stop preparation, close the scan, and leave cleanup retryable. */
START_TEST(test_drive_scratch_tree_failures) {
    static const char* const expected_errors[] = {
        "", "could not open", "could not read", "could not close", "could not remove", "could not remove",
    };
    char error[DIAG_MESSAGE_MAX] = "";
    write_small_file(scratch_paths[0]);
    reset_removal_counts();
    cleanup_failure = (cleanup_failure_t)_i;

    ck_assert(!diag_plan_drive.prepare(error, sizeof(error)));
    ck_assert_ptr_nonnull(strstr(error, expected_errors[_i]));
    ck_assert_uint_eq(create_calls, 0);
    ck_assert_uint_eq(sd_diag_mock_open_handles(), 0);
    ck_assert(path_is_dir("/test/tmp"));
    ck_assert_int_eq(path_is_file(scratch_paths[0]), cleanup_failure != CLEANUP_FAILURE_RMDIR);

    cleanup_failure = CLEANUP_FAILURE_NONE;
    ck_assert(diag_plan_drive.cleanup(error, sizeof(error)));
    ck_assert(path_is_absent("/test/tmp"));
    ck_assert(path_is_file("/test/drive-test.prg"));
}
END_TEST

/** Reject overlong entries without deleting a truncated path or leaking the directory handle. */
START_TEST(test_drive_rejects_overlong_scratch_path) {
    char error[DIAG_MESSAGE_MAX] = "";
    overlong_entry = true;

    ck_assert(!diag_plan_drive.prepare(error, sizeof(error)));
    ck_assert_ptr_nonnull(strstr(error, "scratch path exceeds"));
    ck_assert_uint_eq(sd_diag_mock_open_handles(), 0);
    ck_assert_uint_eq(remove_calls, 0);
    ck_assert_uint_eq(rmdir_calls, 0);
    ck_assert_uint_eq(create_calls, 0);
}
END_TEST

/** Build the standalone FatFs-backed drive diagnostic setup/teardown suite. */
static Suite* diag_drive_suite(void) {
    Suite* const suite = suite_create("diag-drive");
    TCase* const tests = tcase_create("lifecycle");
    tcase_add_checked_fixture(tests, setup, teardown);
    tcase_add_test(tests, test_drive_prepare_and_cleanup);
    tcase_add_test(tests, test_drive_prepare_creation_failure_cleans_partial_resources);
    tcase_add_test(tests, test_drive_prepare_failure_cleans_partial_resources);
    tcase_add_test(tests, test_drive_prepare_missing_profile_creates_no_resources);
    tcase_add_test(tests, test_drive_prepare_target_failure_cleans_resources);
    tcase_add_test(tests, test_drive_cleanup_retains_files_on_unmount_failure);
    tcase_add_test(tests, test_drive_prepare_rejects_scan_close_failure);
    tcase_add_loop_test(tests, test_drive_missing_scratch_directories, 0, 2);
    tcase_add_loop_test(tests, test_drive_rejects_scratch_directory_conflicts, 0, 2);
    tcase_add_loop_test(tests, test_drive_retains_scratch_on_stat_error, 0, 2);
    tcase_add_loop_test(tests, test_drive_purges_entire_scratch_tree, 0, 2);
    tcase_add_loop_test(tests, test_drive_scratch_tree_failures, CLEANUP_FAILURE_OPEN, CLEANUP_FAILURE_COUNT);
    tcase_add_test(tests, test_drive_rejects_overlong_scratch_path);
    suite_add_tcase(suite, tests);
    return suite;
}

/** Run the standalone drive diagnostic lifecycle suite. */
int main(void) {
    SRunner* const runner = srunner_create(diag_drive_suite());
    srunner_set_fork_status(runner, CK_NOFORK);
    srunner_run_all(runner, CK_NORMAL);
    const int failures = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failures == 0 ? 0 : 1;
}

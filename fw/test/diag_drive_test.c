// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "diag_drive_test.h"

#include <errno.h>
#include <sys/stat.h>

#include "diag/drive.h"
#include "diag/target.h"
#include "drive-test/drive_test_catalog.h"
#include "driver.h"
#include "ieee/diskimage.h"
#include "ieee/diskimage_file.h"
#include "ieee/ieee_drive.h"
#include "menu/menu_browser.h"
#include "pet.h"
#include "sd/sd.h"

static const char* const scratch_paths[DT_SLOT_COUNT] = {
    "/test/tmp/u8d0.d64",  "/test/tmp/u8d1.d64",  "/test/tmp/u9d0.d80",  "/test/tmp/u9d1.d80",
    "/test/tmp/u10d0.d82", "/test/tmp/u10d1.d82", "/test/tmp/u11d0.d64", "/test/tmp/u11d1.d64",
};

static const diskimage_type_t expected_types[DT_SLOT_COUNT] = {
    diskimage_type_d64, diskimage_type_d64, diskimage_type_d80, diskimage_type_d80,
    diskimage_type_d82, diskimage_type_d82, diskimage_type_d64, diskimage_type_d64,
};

typedef struct {
    diskimage_type_t type;
    uint32_t size;
    bool exists;
} scratch_file_t;

static scratch_file_t scratch_files[DT_SLOT_COUNT];
static bool mounted[DT_SLOT_COUNT];
static char mounted_paths[DT_SLOT_COUNT][SD_PATH_MAX];
static bool fixture_enabled;
static bool unmount_succeeds;
static bool scan_close_succeeds;
static bool target_prepare_succeeds;
static bool create_tmp_file_after_rmdir;
static bool tmp_path_is_file;
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
static unsigned rmdir_calls;
static bool target_prepare_saw_mounts;

bool __real_ieee_drive_unmount_all(void);
bool __real_menu_browser_abandon_scan(void);
void __real_pet_wait_for_storage_idle(void);
void __real_set_cpu(cpu_state_t state);
bool __real_diskimage_create_file(const char* path, diskimage_type_t type);
dos_media_result_t __real_ieee_drive_mount_scratch(unsigned int slot, const char* path);
bool __real_diag_target_prepare_pet_prg(const char* config_id, const char* prg_path,
                                        char* error_out, size_t error_size);
const char* __real_ieee_drive_filename(unsigned int slot);
int __real_mkdir(const char* path, mode_t mode);
int __real_stat(const char* path, struct stat* info);
int __real_remove(const char* path);
int __real_rmdir(const char* path);

/** Reset isolated callback observations and enable the fake storage boundaries. */
static void setup(void) {
    memset(scratch_files, 0, sizeof(scratch_files));
    memset(mounted, 0, sizeof(mounted));
    memset(mounted_paths, 0, sizeof(mounted_paths));
    fixture_enabled = true;
    unmount_succeeds = true;
    scan_close_succeeds = true;
    target_prepare_succeeds = true;
    create_tmp_file_after_rmdir = false;
    tmp_path_is_file = false;
    fail_create_slot = -1;
    fail_mount_slot = -1;
    storage_wait_calls = 0;
    cpu_halt_calls = 0;
    scan_close_calls = 0;
    unmount_calls = 0;
    create_calls = 0;
    mount_calls = 0;
    target_prepare_calls = 0;
    successful_removes = 0;
    rmdir_calls = 0;
    target_prepare_saw_mounts = false;
}

/** Disable fake boundaries before the next firmware test runs. */
static void teardown(void) {
    fixture_enabled = false;
}

/** Return the scratch slot associated with a known path, or DT_SLOT_COUNT. */
static size_t scratch_slot(const char* path) {
    for (size_t slot = 0; slot < count_of(scratch_paths); ++slot) {
        if (strcmp(path, scratch_paths[slot]) == 0) {
            return slot;
        }
    }
    return DT_SLOT_COUNT;
}

/** Observe storage-idle waits without running hardware service loops. */
void __wrap_pet_wait_for_storage_idle(void) {
    if (fixture_enabled) {
        ++storage_wait_calls;
        return;
    }
    __real_pet_wait_for_storage_idle();
}

/** Observe CPU halts without writing FPGA registers. */
void __wrap_set_cpu(cpu_state_t state) {
    if (fixture_enabled) {
        ck_assert_int_eq(state, CPU_HALT);
        ++cpu_halt_calls;
        return;
    }
    __real_set_cpu(state);
}

/** Model closing the active menu scan before drive preparation. */
bool __wrap_menu_browser_abandon_scan(void) {
    if (fixture_enabled) {
        ++scan_close_calls;
        return scan_close_succeeds;
    }
    return __real_menu_browser_abandon_scan();
}

/** Model owner-preserving IEEE unmount success or failure. */
bool __wrap_ieee_drive_unmount_all(void) {
    if (!fixture_enabled) {
        return __real_ieee_drive_unmount_all();
    }
    ++unmount_calls;
    if (!unmount_succeeds) {
        return false;
    }
    memset(mounted, 0, sizeof(mounted));
    memset(mounted_paths, 0, sizeof(mounted_paths));
    return true;
}

/** Record each requested blank image and its production geometry size. */
bool __wrap_diskimage_create_file(const char* path, diskimage_type_t type) {
    if (!fixture_enabled) {
        return __real_diskimage_create_file(path, type);
    }
    ck_assert_uint_lt(create_calls, DT_SLOT_COUNT);
    const size_t slot = create_calls++;
    ck_assert_str_eq(path, scratch_paths[slot]);
    if ((int)slot == fail_create_slot) {
        return false;
    }
    scratch_files[slot] = (scratch_file_t){.type = type, .size = diskimage_size(type), .exists = true};
    return true;
}

/** Track successful scratch mounts and inject a selected partial-setup failure. */
dos_media_result_t __wrap_ieee_drive_mount_scratch(const unsigned int slot, const char* path) {
    if (!fixture_enabled) {
        return __real_ieee_drive_mount_scratch(slot, path);
    }
    ck_assert_uint_lt(slot, DT_SLOT_COUNT);
    ++mount_calls;
    ck_assert_str_eq(path, scratch_paths[slot]);
    if ((int)slot == fail_mount_slot) {
        return DOS_MEDIA_IO_ERROR;
    }
    ck_assert(scratch_files[slot].exists);
    mounted[slot] = true;
    snprintf(mounted_paths[slot], sizeof(mounted_paths[slot]), "%s", path);
    return DOS_MEDIA_OK;
}

/** Verify target setup sees all intended scratch mounts before it loads the PRG. */
bool __wrap_diag_target_prepare_pet_prg(const char* config_id, const char* prg_path,
                                        char* error_out, size_t error_size) {
    if (!fixture_enabled) {
        return __real_diag_target_prepare_pet_prg(config_id, prg_path, error_out, error_size);
    }
    ++target_prepare_calls;
    ck_assert_str_eq(config_id, "pet-80xx-60hz");
    ck_assert_str_eq(prg_path, "/test/drive-test.prg");
    target_prepare_saw_mounts = true;
    for (size_t slot = 0; slot < count_of(mounted); ++slot) {
        target_prepare_saw_mounts = target_prepare_saw_mounts && mounted[slot] &&
                                    scratch_files[slot].exists &&
                                    strcmp(mounted_paths[slot], scratch_paths[slot]) == 0;
    }
    if (!target_prepare_succeeds) {
        snprintf(error_out, error_size, "fixture target preparation failed");
        return false;
    }
    return true;
}

/** Return the mounted path observed by drive_prepare's post-configuration invariant. */
const char* __wrap_ieee_drive_filename(const unsigned int slot) {
    if (!fixture_enabled) {
        return __real_ieee_drive_filename(slot);
    }
    ck_assert_uint_lt(slot, DT_SLOT_COUNT);
    return mounted[slot] ? mounted_paths[slot] : "";
}

/** Model the reserved diagnostic directories without touching the host filesystem. */
int __wrap_mkdir(const char* path, const mode_t mode) {
    (void)mode;
    if (fixture_enabled && (strcmp(path, "/test") == 0 || strcmp(path, "/test/tmp") == 0)) {
        if (strcmp(path, "/test/tmp") == 0 && tmp_path_is_file) {
            errno = EEXIST;
            return -1;
        }
        return 0;
    }
    return __real_mkdir(path, mode);
}

/** Expose a regular-file race at /test/tmp to the production directory check. */
int __wrap_stat(const char* path, struct stat* info) {
    if (fixture_enabled && strcmp(path, "/test/tmp") == 0 && tmp_path_is_file) {
        memset(info, 0, sizeof(*info));
        info->st_mode = S_IFREG;
        return 0;
    }
    return __real_stat(path, info);
}

/** Remove registered scratch files from the fake filesystem. */
int __wrap_remove(const char* path) {
    const size_t slot = scratch_slot(path);
    if (fixture_enabled && slot < DT_SLOT_COUNT) {
        if (tmp_path_is_file) {
            errno = ENOTDIR;
            return -1;
        }
        if (!scratch_files[slot].exists) {
            errno = ENOENT;
            return -1;
        }
        scratch_files[slot].exists = false;
        ++successful_removes;
        return 0;
    }
    return __real_remove(path);
}

/** Remove the fake temporary directory only after its scratch entries are gone. */
int __wrap_rmdir(const char* path) {
    if (fixture_enabled && strcmp(path, "/test/tmp") == 0) {
        ++rmdir_calls;
        if (tmp_path_is_file) {
            errno = ENOTDIR;
            return -1;
        }
        for (size_t slot = 0; slot < count_of(scratch_files); ++slot) {
            if (scratch_files[slot].exists) {
                errno = ENOTEMPTY;
                return -1;
            }
        }
        if (create_tmp_file_after_rmdir) {
            tmp_path_is_file = true;
        }
        return 0;
    }
    return __real_rmdir(path);
}

/** Prepare and clean up eight correctly sized, mounted scratch containers. */
START_TEST(test_drive_prepare_and_cleanup) {
    char error[DIAG_MESSAGE_MAX] = "";
    static const uint32_t expected_sizes[DT_SLOT_COUNT] = {
        DISKIMAGE_D64_SIZE, DISKIMAGE_D64_SIZE, DISKIMAGE_D80_SIZE, DISKIMAGE_D80_SIZE,
        DISKIMAGE_D82_SIZE, DISKIMAGE_D82_SIZE, DISKIMAGE_D64_SIZE, DISKIMAGE_D64_SIZE,
    };

    ck_assert(diag_plan_drive.prepare(error, sizeof(error)));
    ck_assert_uint_eq(storage_wait_calls, 1);
    ck_assert_uint_eq(cpu_halt_calls, 1);
    ck_assert_uint_eq(scan_close_calls, 1);
    ck_assert_uint_eq(unmount_calls, 1);
    ck_assert_uint_eq(create_calls, DT_SLOT_COUNT);
    ck_assert_uint_eq(mount_calls, DT_SLOT_COUNT);
    ck_assert_uint_eq(target_prepare_calls, 1);
    ck_assert(target_prepare_saw_mounts);
    for (size_t slot = 0; slot < count_of(scratch_files); ++slot) {
        ck_assert_int_eq(scratch_files[slot].type, expected_types[slot]);
        ck_assert_uint_eq(scratch_files[slot].size, expected_sizes[slot]);
        ck_assert(mounted[slot]);
    }

    ck_assert(diag_plan_drive.cleanup(error, sizeof(error)));
    ck_assert_uint_eq(unmount_calls, 2);
    ck_assert_uint_eq(successful_removes, DT_SLOT_COUNT);
    for (size_t slot = 0; slot < count_of(scratch_files); ++slot) {
        ck_assert(!scratch_files[slot].exists);
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
    for (size_t slot = 0; slot < count_of(scratch_files); ++slot) {
        ck_assert(!scratch_files[slot].exists);
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
    for (size_t slot = 0; slot < 4; ++slot) {
        ck_assert(!scratch_files[slot].exists);
    }
}
END_TEST

/** Remove all scratch resources when target configuration or PRG preparation fails. */
START_TEST(test_drive_prepare_target_failure_cleans_resources) {
    char error[DIAG_MESSAGE_MAX] = "";
    target_prepare_succeeds = false;

    ck_assert(!diag_plan_drive.prepare(error, sizeof(error)));
    ck_assert_ptr_nonnull(strstr(error, "fixture target preparation failed"));
    ck_assert(target_prepare_saw_mounts);
    ck_assert_uint_eq(mount_calls, DT_SLOT_COUNT);

    ck_assert(diag_plan_drive.cleanup(error, sizeof(error)));
    ck_assert_uint_eq(successful_removes, DT_SLOT_COUNT);
    for (size_t slot = 0; slot < count_of(scratch_files); ++slot) {
        ck_assert(!scratch_files[slot].exists);
        ck_assert(!mounted[slot]);
    }
}
END_TEST

/** Keep every backing image intact when the IEEE facade cannot confirm unmount. */
START_TEST(test_drive_cleanup_retains_files_on_unmount_failure) {
    char error[DIAG_MESSAGE_MAX] = "";
    ck_assert(diag_plan_drive.prepare(error, sizeof(error)));
    unmount_succeeds = false;

    ck_assert(!diag_plan_drive.cleanup(error, sizeof(error)));
    ck_assert_ptr_nonnull(strstr(error, "scratch files were retained"));
    ck_assert_uint_eq(successful_removes, 0);
    for (size_t slot = 0; slot < count_of(scratch_files); ++slot) {
        ck_assert(scratch_files[slot].exists);
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

/** Reject a regular-file race at the reserved temporary-directory path. */
START_TEST(test_drive_prepare_rejects_non_directory_tmp_path) {
    char error[DIAG_MESSAGE_MAX] = "";
    create_tmp_file_after_rmdir = true;

    ck_assert(!diag_plan_drive.prepare(error, sizeof(error)));
    ck_assert_ptr_nonnull(strstr(error, "could not create '/test/tmp'"));
    ck_assert_uint_eq(create_calls, 0);
    ck_assert(!diag_plan_drive.cleanup(error, sizeof(error)));
    ck_assert_ptr_nonnull(strstr(error, "could not remove"));
}
END_TEST

/** Register callback lifecycle tests with the fixture-backed host runner. */
Suite* diag_drive_suite(void) {
    Suite* const suite = suite_create("diag-drive");
    TCase* const tests = tcase_create("lifecycle");
    tcase_add_checked_fixture(tests, setup, teardown);
    tcase_add_test(tests, test_drive_prepare_and_cleanup);
    tcase_add_test(tests, test_drive_prepare_creation_failure_cleans_partial_resources);
    tcase_add_test(tests, test_drive_prepare_failure_cleans_partial_resources);
    tcase_add_test(tests, test_drive_prepare_target_failure_cleans_resources);
    tcase_add_test(tests, test_drive_cleanup_retains_files_on_unmount_failure);
    tcase_add_test(tests, test_drive_prepare_rejects_scan_close_failure);
    tcase_add_test(tests, test_drive_prepare_rejects_non_directory_tmp_path);
    suite_add_tcase(suite, tests);
    return suite;
}

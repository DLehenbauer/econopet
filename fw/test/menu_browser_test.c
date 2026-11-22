// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "menu_browser_test.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "diag/log/log.h"
#include "menu/menu_browser.h"
#include "mock.h"
#include "system/system.h"
#include "test_support.h"
#include "ui/cli.h"

#define TEST_IMAGE_COUNT 65u
#define TEST_SERVICE_FILE_COUNT 8u
#define TEST_SERVICE_FILE_BYTES 2048u
#define TEST_SERVICE_STRIDE 137u

char directory_root[64];
bool fail_readdir;
bool fail_next_readdir;
static bool fail_opendir;
bool unknown_entry_types;
unsigned int directory_scans, metadata_reads;
static unsigned int service_count;
static FILE* service_files[TEST_SERVICE_FILE_COUNT];
static size_t service_reads[TEST_SERVICE_FILE_COUNT];
static int last_dir_fd;
static jmp_buf scan_escape;
static const char* const invalid_paths[] = {
    "/disk/..", "/disk/.", "/disk//child", "/disk-other", "/tmp", "/disk/outside/child",
};

DIR* __real_opendir(const char* path);
int __real_lstat(const char* path, struct stat* status);
struct dirent* __real_readdir(DIR* dir);

// Redirects only /disk paths to this test's private temporary directory.
static const char* fixture_path(const char* path, char output[PATH_MAX]) {
    const size_t root_length = strlen(MENU_BROWSER_ROOT);
    if (directory_root[0] == '\0' || strncmp(path, MENU_BROWSER_ROOT, root_length) != 0
        || (path[root_length] != '\0' && path[root_length] != '/')) {
        return path;
    }
    const int length = snprintf(output, PATH_MAX, "%s%s", directory_root, path + root_length);
    ck_assert_int_ge(length, 0);
    ck_assert_int_lt(length, PATH_MAX);
    return output;
}

// Uses real libc directory handles while isolating the firmware's root.
DIR* __wrap_opendir(const char* path) {
    char output[PATH_MAX];
    directory_scans++;
    if (fail_opendir) {
        errno = EACCES;
        return NULL;
    }
    DIR* const dir = __real_opendir(fixture_path(path, output));
    if (dir != NULL) last_dir_fd = dirfd(dir);
    return dir;
}

// Preserves real filesystem type and symlink behavior in browser tests.
int __wrap_lstat(const char* path, struct stat* status) {
    char output[PATH_MAX];
    metadata_reads++;
    return __real_lstat(fixture_path(path, output), status);
}

// Injects read failures or filesystem types that require metadata fallback.
struct dirent* __wrap_readdir(DIR* dir) {
    if (fail_readdir || fail_next_readdir) {
        fail_next_readdir = false;
        errno = EIO;
        return NULL;
    }
    struct dirent* const entry = __real_readdir(dir);
    if (entry != NULL && unknown_entry_types) {
        entry->d_type = DT_UNKNOWN;
    }
    return entry;
}

// Generates independently identifiable bytes for each retained service reader.
static uint8_t service_byte(size_t file, size_t offset) {
    return (uint8_t)(file * 37u + offset * 13u);
}

// Interleaves permitted file I/O without changing the browser's directory membership.
static void service(void) {
    service_count++;
    if (service_files[0] == NULL) return;
    const size_t index = service_count % count_of(service_files);
    const size_t offset = service_count * TEST_SERVICE_STRIDE % TEST_SERVICE_FILE_BYTES;
    ck_assert_int_eq(fseek(service_files[index], (long)offset, SEEK_SET), 0);
    uint8_t byte;
    ck_assert_uint_eq(fread(&byte, 1, 1, service_files[index]), 1);
    ck_assert_uint_eq(byte, service_byte(index, offset));
    ck_assert_int_eq(ftell(service_files[index]), offset + 1);
    service_reads[index]++;
}

// Creates a filename visible to libc directory scans.
static void make_file(const char* relative) {
    char path[PATH_MAX];
    const int length = snprintf(path, sizeof(path), "%s/%s", directory_root, relative);
    ck_assert_int_ge(length, 0);
    ck_assert_uint_lt((size_t) length, sizeof(path));
    FILE* const file = fopen(path, "wb");
    ck_assert_ptr_nonnull(file);
    ck_assert_int_eq(fclose(file), 0);
}

// Initializes only the filesystem fixture, without menu or drive state.
static void setup(void) {
    log_init();
    fail_readdir = fail_next_readdir = fail_opendir = unknown_entry_types = false;
    directory_scans = metadata_reads = service_count = 0;
    last_dir_fd = -1;
    memset(service_files, 0, sizeof(service_files));
    memset(service_reads, 0, sizeof(service_reads));
    snprintf(directory_root, sizeof(directory_root), "/tmp/econopet-browser-XXXXXX");
    ck_assert_ptr_nonnull(mkdtemp(directory_root));
}

// Removes only the named fixture root and its test-created descendants.
static void remove_fixture_directory(const char* path) {
    DIR* const dir = __real_opendir(path);
    ck_assert_ptr_nonnull(dir);
    struct dirent* entry;
    while ((entry = __real_readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[PATH_MAX];
        const int length = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        ck_assert_int_ge(length, 0);
        ck_assert_uint_lt((size_t) length, sizeof(child));
        struct stat status;
        ck_assert_int_eq(__real_lstat(child, &status), 0);
        if (S_ISDIR(status.st_mode)) {
            remove_fixture_directory(child);
        } else {
            ck_assert_int_eq(unlink(child), 0);
        }
    }
    ck_assert_int_eq(closedir(dir), 0);
    ck_assert_int_eq(rmdir(path), 0);
}

// Cleans up the private directory without following host symlinks.
static void teardown(void) {
    ck_assert(menu_browser_abandon_scan());
    for (size_t index = 0; index < count_of(service_files); ++index) {
        if (service_files[index] != NULL)
            ck_assert_int_eq(fclose(service_files[index]), 0);
        service_files[index] = NULL;
    }
    remove_fixture_directory(directory_root);
    directory_root[0] = '\0';
    fail_readdir = fail_next_readdir = fail_opendir = unknown_entry_types = false;
}

/** Terminate a scan as a nonreturning diagnostic does before unmounting storage. */
static void abandon_scan_service(void) {
    ck_assert_int_ge(last_dir_fd, 0);
    ck_assert(menu_browser_abandon_scan());
    errno = 0;
    ck_assert_int_eq(fcntl(last_dir_fd, F_GETFD), -1);
    ck_assert_int_eq(errno, EBADF);
    longjmp(scan_escape, 1);
}

/** An interrupted browser releases its live directory before card retirement. */
START_TEST(test_browser_abandon_scan) {
    make_file("disk.d64");
    menu_browser_t browser = {.path = MENU_BROWSER_ROOT, .service = abandon_scan_service};
    if (setjmp(scan_escape) == 0) {
        menu_browser_refresh(&browser);
        ck_abort_msg("scan service did not abandon its caller");
    }
    browser.service = NULL;
    ck_assert(menu_browser_refresh(&browser));
    ck_assert_uint_eq(browser.count, 1);
}
END_TEST

/** Check the real diagnostic takeover closes the scan before releasing storage. */
static bool prepare_diagnostic_during_scan(void) {
    ck_assert_int_eq(system_mode(), system_operation_diagnostic);
    ck_assert_int_ne(fcntl(last_dir_fd, F_GETFD), -1);
    ck_assert(menu_browser_abandon_scan());
    ck_assert_int_eq(fcntl(last_dir_fd, F_GETFD), -1);
    ck_assert_int_eq(errno, EBADF);
    return true;
}

/** Send an abort key while the diagnostic owns the nested menu call stack. */
static void abort_diagnostic_sleep(uint64_t us) {
    (void)us;
    mock_input_uart_bytes((const uint8_t*)" ", 1);
}

/** Observe reset without ever returning to the interrupted scan. */
static void reset_during_scan(void) {
    longjmp(scan_escape, 1);
}

/** Issue the real CLI command from a browser scan service callback. */
static void start_diagnostic_during_scan(void) {
    const char* command = "test sd\r";
    while (*command != '\0') cli_process_char(*command++);
    ck_abort_msg("diagnostic returned to the interrupted scan");
}

/** A menu command retires an open scan before prompting and resets on abort. */
START_TEST(test_browser_diagnostic_takeover) {
    const system_state_t saved = system_state;
    make_file("disk.d64");
    mock_reset();
    system_reset_guest();
    ck_assert(system_suspend());
    mock_clock_set(0);
    mock_sd_prepare_set_handler(prepare_diagnostic_during_scan);
    mock_sleep_set_handler(abort_diagnostic_sleep);
    mock_system_reset_set_handler(reset_during_scan);
    cli_init();
    menu_browser_t browser = {.path = MENU_BROWSER_ROOT, .service = start_diagnostic_during_scan};
    if (setjmp(scan_escape) == 0) {
        menu_browser_refresh(&browser);
        ck_abort_msg("diagnostic did not reset");
    }
    ck_assert_int_eq(system_mode(), system_operation_diagnostic);
    mock_sd_prepare_set_handler(NULL);
    mock_sleep_set_handler(NULL);
    mock_system_reset_set_handler(NULL);
    mock_clock_restore();
    system_state = saved;
}
END_TEST

// Pages, resolves, and finds full names without a TUI controller.
START_TEST(test_browser_without_ui) {
    for (size_t i = 0; i < TEST_IMAGE_COUNT; i++) {
        char name[32];
        snprintf(name, sizeof(name), "disk%02zu.d64", i);
        make_file(name);
    }
    char folder[PATH_MAX];
    snprintf(folder, sizeof(folder), "%s/folder", directory_root);
    ck_assert_int_eq(mkdir(folder, 0700), 0);
    menu_browser_t browser = {.path = MENU_BROWSER_ROOT, .service = service};
    ck_assert(menu_browser_refresh(&browser));
    ck_assert_uint_eq(browser.count, TEST_IMAGE_COUNT + 1);
    ck_assert(menu_browser_page(&browser, MENU_BROWSER_PAGE_ROWS, MENU_BROWSER_PAGE_ROWS));
    ck_assert_str_eq(browser.entries[0].name, "disk20.d64");
    size_t index;
    ck_assert(menu_browser_find(&browser, "disk64.d64", false, &index));
    char path[SD_PATH_MAX];
    bool directory;
    ck_assert(menu_browser_resolve(&browser, index, path, &directory));
    ck_assert(!directory);
    ck_assert_str_eq(path, "/disk/disk64.d64");
    ck_assert(menu_browser_find(&browser, "folder", true, &index));
    ck_assert(menu_browser_resolve(&browser, index, path, &directory));
    ck_assert(directory);
    snprintf(browser.path, sizeof(browser.path), "%s", path);
    char child[SD_PATH_MAX];
    ck_assert(menu_browser_parent(&browser, child));
    ck_assert_str_eq(child, "folder");
    ck_assert_str_eq(browser.path, MENU_BROWSER_ROOT);
    ck_assert(!menu_browser_parent(&browser, child));
    ck_assert(!menu_browser_find(&browser, "missing.d64", false, &index));
    ck_assert_uint_gt(service_count, TEST_IMAGE_COUNT);
}
END_TEST

// Adjacent overlapping pages scan once in either direction with either metadata source.
START_TEST(test_browser_scan_cost_and_types) {
    unknown_entry_types = _i != 0;
    for (unsigned int index = 0; index < TEST_IMAGE_COUNT; index++) {
        char name[32];
        snprintf(name, sizeof(name), "image%02u.d64", TEST_IMAGE_COUNT - 1 - index);
        make_file(name);
    }
    char folder[PATH_MAX];
    snprintf(folder, sizeof(folder), "%s/folder", directory_root);
    ck_assert_int_eq(mkdir(folder, 0700), 0);
    make_file("ignored.txt");
    char link[PATH_MAX];
    snprintf(link, sizeof(link), "%s/link.d64", directory_root);
    ck_assert_int_eq(symlink("/tmp", link), 0);
    menu_browser_t browser = {.path = MENU_BROWSER_ROOT, .service = service};
    ck_assert(menu_browser_refresh(&browser));
    ck_assert_uint_eq(browser.count, TEST_IMAGE_COUNT + 1);
    ck_assert(menu_browser_page(&browser, 0, MENU_BROWSER_PAGE_ROWS));
    ck_assert_str_eq(browser.entries[0].name, "folder");
    ck_assert(browser.entries[0].directory);
    const size_t stride = MENU_BROWSER_PAGE_ROWS - 1;
    for (size_t page = 1; page <= 3; page++) {
        ck_assert(menu_browser_page(&browser, page * stride, MENU_BROWSER_PAGE_ROWS));
    }

    // Measure a deep backward page, then return forward across the same boundary.
    for (unsigned int direction = 0; direction < 2; direction++) {
        directory_scans = metadata_reads = 0;
        const size_t first = (direction == 0 ? 2 : 3) * stride;
        ck_assert(menu_browser_page(&browser, first, MENU_BROWSER_PAGE_ROWS));
        ck_assert_uint_eq(directory_scans, 1);
        if (unknown_entry_types) {
            ck_assert_uint_gt(metadata_reads, TEST_IMAGE_COUNT);
        } else {
            ck_assert_uint_eq(metadata_reads, 1);
        }
        for (size_t row = 0; row < browser.entry_count; row++) {
            char expected[32];
            snprintf(expected, sizeof(expected), "image%02zu.d64", first + row - 1);
            ck_assert_str_eq(browser.entries[row].name, expected);
        }
        ck_assert(menu_browser_page(&browser, first, MENU_BROWSER_PAGE_ROWS));
        ck_assert_uint_eq(directory_scans, 1);
    }
}
END_TEST

// Nonadjacent pages preserve all sorted identities while servicing independent readers.
START_TEST(test_nonadjacent_pages_with_retained_readers) {
    for (size_t index = 0; index < TEST_IMAGE_COUNT; ++index) {
        char name[32];
        snprintf(name, sizeof(name), "disk%02zu.d64", TEST_IMAGE_COUNT - index - 1);
        make_file(name);
    }
    make_file("ignored.txt");
    for (size_t index = 0; index < count_of(service_files); ++index) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/disk%02zu.d64", directory_root, index);
        FILE* const fixture = fopen(path, "wb");
        ck_assert_ptr_nonnull(fixture);
        uint8_t bytes[TEST_SERVICE_FILE_BYTES];
        for (size_t offset = 0; offset < sizeof(bytes); ++offset)
            bytes[offset] = service_byte(index, offset);
        ck_assert_uint_eq(fwrite(bytes, 1, sizeof(bytes), fixture), sizeof(bytes));
        ck_assert_int_eq(fclose(fixture), 0);
        service_files[index] = fopen(path, "r+b");
        ck_assert_ptr_nonnull(service_files[index]);
        ck_assert_int_eq(setvbuf(service_files[index], NULL, _IONBF, 0), 0);
    }
    menu_browser_t browser = {.path = MENU_BROWSER_ROOT, .service = service};
    ck_assert(menu_browser_refresh(&browser));
    ck_assert_uint_eq(browser.count, TEST_IMAGE_COUNT);
    static const size_t first_rows[] = {0, 63, 4, 42, 21, 64, 0};
    for (size_t page = 0; page < count_of(first_rows); ++page) {
        const size_t first = first_rows[page];
        ck_assert(menu_browser_page(&browser, first, MENU_BROWSER_PAGE_ROWS));
        ck_assert_uint_eq(browser.entry_count, MIN(MENU_BROWSER_PAGE_ROWS, TEST_IMAGE_COUNT - first));
        for (size_t row = 0; row < browser.entry_count; ++row) {
            char expected[32];
            snprintf(expected, sizeof(expected), "disk%02zu.d64", first + row);
            ck_assert_str_eq(browser.entries[row].name, expected);
            ck_assert(!browser.entries[row].directory);
        }
    }
    for (size_t index = 0; index < count_of(service_reads); ++index)
        ck_assert_uint_gt(service_reads[index], 0);
}
END_TEST

// Filtering includes native formats in either case and orders case ties deterministically.
START_TEST(test_browser_formats_and_sorting) {
    make_file("alpha.D64");
    make_file("Alpha.d64");
    make_file("beta.d80");
    make_file("gamma.D82");
    make_file("legacy.hdd");
    make_file("ignored.txt");
    menu_browser_t browser = {.path = MENU_BROWSER_ROOT};
    ck_assert(menu_browser_refresh(&browser));
    ck_assert_uint_eq(browser.count, 4);
    ck_assert(menu_browser_page(&browser, 0, MENU_BROWSER_PAGE_ROWS));
    ck_assert_str_eq(browser.entries[0].name, "Alpha.d64");
    ck_assert_str_eq(browser.entries[1].name, "alpha.D64");
    ck_assert_str_eq(browser.entries[2].name, "beta.d80");
    ck_assert_str_eq(browser.entries[3].name, "gamma.D82");
}
END_TEST

// Full filenames survive paging and resolution, while overlong full paths are skipped.
START_TEST(test_browser_long_names_and_overlong_paths) {
    char long_name[181];
    memset(long_name, 'a', sizeof(long_name) - 5);
    memcpy(long_name + sizeof(long_name) - 5, ".d64", 5);
    make_file(long_name);
    char invalid[256];
    memset(invalid, 'z', sizeof(invalid) - 5);
    memcpy(invalid + sizeof(invalid) - 5, ".d64", 5);
    make_file(invalid);
    menu_browser_t browser = {.path = MENU_BROWSER_ROOT};
    ck_assert(menu_browser_refresh(&browser));
    ck_assert_uint_eq(browser.count, 1);
    ck_assert(menu_browser_page(&browser, 0, MENU_BROWSER_PAGE_ROWS));
    ck_assert_str_eq(browser.error, "Path is too long");
    ck_assert_str_eq(browser.entries[0].name, long_name);
    char path[SD_PATH_MAX];
    bool directory;
    ck_assert(menu_browser_resolve(&browser, 0, path, &directory));
    ck_assert_str_eq(path + strlen(MENU_BROWSER_ROOT) + 1, long_name);
    ck_assert(!directory);
}
END_TEST

// Directory mutation violates the browser's stable-listing contract.
START_TEST(test_browser_directory_change) {
    for (size_t index = 0; index < MENU_BROWSER_PAGE_ROWS + 1; index++) {
        char name[32];
        snprintf(name, sizeof(name), "image%02zu.d64", index);
        make_file(name);
    }
    menu_browser_t browser = {.path = MENU_BROWSER_ROOT};
    ck_assert(menu_browser_refresh(&browser));
    ck_assert(menu_browser_page(&browser, 0, MENU_BROWSER_PAGE_ROWS));
    make_file("new-image.d64");
    test_expect_fatal_message("browser directory count changed");
    test_expect_fatal_callback(teardown);
    menu_browser_page(&browser, MENU_BROWSER_PAGE_ROWS - 1, MENU_BROWSER_PAGE_ROWS);
}
END_TEST

// Cached anchors must retain both their identity and rank in either scan direction.
START_TEST(test_browser_renamed_cursor) {
    for (size_t i = 0; i < TEST_IMAGE_COUNT; i++) {
        char name[32];
        snprintf(name, sizeof(name), "image%02zu.d64", i);
        make_file(name);
    }
    const bool reverse = _i >= 2;
    const bool replace_cursor = _i % 2 != 0;
    menu_browser_t browser = {.path = MENU_BROWSER_ROOT};
    ck_assert(menu_browser_refresh(&browser));
    ck_assert(menu_browser_page(&browser, reverse ? 2 * MENU_BROWSER_PAGE_ROWS : 0,
                                MENU_BROWSER_PAGE_ROWS));
    char source[PATH_MAX], target[PATH_MAX];
    const size_t cursor_index = reverse ? 2 * MENU_BROWSER_PAGE_ROWS : MENU_BROWSER_PAGE_ROWS - 1;
    snprintf(source, sizeof(source), "%s/image%02zu.d64", directory_root,
             replace_cursor ? cursor_index : TEST_IMAGE_COUNT - 1);
    snprintf(target, sizeof(target), "%s/%s", directory_root,
             replace_cursor ? "zzz.d64" : "aaa.d64");
    ck_assert_int_eq(rename(source, target), 0);
    test_expect_fatal_message("browser directory cursor changed");
    test_expect_fatal_callback(teardown);
    const size_t first = reverse ? MENU_BROWSER_PAGE_ROWS + 1 : MENU_BROWSER_PAGE_ROWS;
    menu_browser_page(&browser, first, MENU_BROWSER_PAGE_ROWS);
}
END_TEST

// A search can borrow a full filename from a page that the search itself replaces.
START_TEST(test_browser_find_cached_name) {
    for (size_t i = 0; i < TEST_IMAGE_COUNT; i++) {
        char name[32];
        snprintf(name, sizeof(name), "image%02zu.d64", i);
        make_file(name);
    }
    char long_name[181];
    memset(long_name, 'z', sizeof(long_name) - 5);
    memcpy(long_name + sizeof(long_name) - 5, ".d64", 5);
    make_file(long_name);
    menu_browser_t browser = {.path = MENU_BROWSER_ROOT};
    ck_assert(menu_browser_refresh(&browser));
    const size_t first = _i == 0 ? MENU_BROWSER_PAGE_ROWS : TEST_IMAGE_COUNT;
    ck_assert(menu_browser_page(&browser, first, MENU_BROWSER_PAGE_ROWS));
    size_t index = 0;
    ck_assert(menu_browser_find(&browser, browser.entries[0].name, false, &index));
    ck_assert_uint_eq(index, first);
    char path[SD_PATH_MAX];
    bool directory;
    ck_assert(menu_browser_resolve(&browser, index, path, &directory));
    ck_assert_str_eq(path + strlen(MENU_BROWSER_ROOT) + 1,
                     _i == 0 ? "image21.d64" : long_name);
}
END_TEST

// A cached file replaced by a symlink must not be followed during resolution.
START_TEST(test_browser_replaced_entry) {
    make_file("disk.d64");
    menu_browser_t browser = {.path = MENU_BROWSER_ROOT};
    ck_assert(menu_browser_refresh(&browser));
    ck_assert(menu_browser_page(&browser, 0, MENU_BROWSER_PAGE_ROWS));
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/disk.d64", directory_root);
    ck_assert_int_eq(unlink(path), 0);
    ck_assert_int_eq(symlink("/tmp", path), 0);
    char resolved[SD_PATH_MAX];
    bool directory;
    ck_assert(!menu_browser_resolve(&browser, 0, resolved, &directory));
    ck_assert_str_eq(browser.error, "Entry changed - reopen picker");
}
END_TEST

// Read and open failures remain distinct from a successful empty directory.
START_TEST(test_browser_filesystem_errors) {
    menu_browser_t browser = {.path = MENU_BROWSER_ROOT};
    if (_i == 0) {
        fail_readdir = true;
    } else {
        fail_opendir = true;
    }
    ck_assert(!menu_browser_refresh(&browser));
    ck_assert(!browser.cache_valid);
    ck_assert_uint_eq(browser.count, 0);
    ck_assert_str_eq(browser.error, _i == 0 ? "Cannot read directory" : "Cannot open directory");
    fail_readdir = fail_opendir = false;
    ck_assert(menu_browser_refresh(&browser));
    ck_assert_str_eq(browser.error, "");
}
END_TEST

// Empty storage succeeds, while a missing directory reports an availability error.
START_TEST(test_browser_empty_and_missing) {
    menu_browser_t browser = {.path = MENU_BROWSER_ROOT};
    ck_assert(menu_browser_refresh(&browser));
    ck_assert_uint_eq(browser.count, 0);
    ck_assert(menu_browser_page(&browser, 0, MENU_BROWSER_PAGE_ROWS));
    ck_assert_uint_eq(browser.entry_count, 0);
    ck_assert_str_eq(browser.error, "");
    ck_assert_int_eq(rmdir(directory_root), 0);
    ck_assert(!menu_browser_refresh(&browser));
    ck_assert(!browser.cache_valid);
    ck_assert_str_eq(browser.error, "Directory is unavailable");
    ck_assert_int_eq(mkdir(directory_root, 0700), 0);
}
END_TEST

// Rejects lexical escapes and symlink ancestors before opening any directory.
START_TEST(test_browser_root_confinement) {
    char link[PATH_MAX];
    snprintf(link, sizeof(link), "%s/outside", directory_root);
    ck_assert_int_eq(symlink("/tmp", link), 0);
    menu_browser_t browser = {0};
    snprintf(browser.path, sizeof(browser.path), "%s", invalid_paths[_i]);
    ck_assert(!menu_browser_refresh(&browser));
    ck_assert(!browser.cache_valid);
    ck_assert_uint_eq(directory_scans, 0);
}
END_TEST

// Registers the extracted filesystem contract without menu or mount dependencies.
Suite* menu_browser_suite(void) {
    Suite* const suite = suite_create("menu_browser");
    TCase* const cases = tcase_create("filesystem");
    tcase_add_checked_fixture(cases, setup, teardown);
    tcase_add_test(cases, test_browser_without_ui);
    tcase_add_test(cases, test_browser_abandon_scan);
    tcase_add_test(cases, test_browser_diagnostic_takeover);
    tcase_add_test(cases, test_nonadjacent_pages_with_retained_readers);
    tcase_add_loop_test(cases, test_browser_scan_cost_and_types, 0, 2);
    tcase_add_test(cases, test_browser_formats_and_sorting);
    tcase_add_test(cases, test_browser_long_names_and_overlong_paths);
    tcase_add_loop_test(cases, test_browser_find_cached_name, 0, 2);
    tcase_add_test(cases, test_browser_replaced_entry);
    tcase_add_loop_test(cases, test_browser_filesystem_errors, 0, 2);
    tcase_add_test(cases, test_browser_empty_and_missing);
    tcase_add_loop_test(cases, test_browser_root_confinement, 0, count_of(invalid_paths));
    suite_add_tcase(suite, cases);
    return suite;
}

// Isolates fatal invariant checks and removes fixtures before the expected abort.
Suite* menu_browser_fatal_suite(void) {
    Suite* const suite = suite_create("menu_browser-fatal");
    TCase* const cases = tcase_create("invariants");
    tcase_add_checked_fixture(cases, setup, teardown);
    tcase_add_test_raise_signal(cases, test_browser_directory_change, SIGABRT);
    tcase_add_loop_test_raise_signal(cases, test_browser_renamed_cursor, SIGABRT, 0, 4);
    suite_add_tcase(suite, cases);
    return suite;
}

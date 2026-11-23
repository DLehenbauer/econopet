// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#define _GNU_SOURCE

#include "pch.h"
#include "sd_diag_mock.h"

#include <check.h>
#include <dirent.h>

// pico-vfs defines its own path bound.
#undef PATH_MAX
#include "filesystem/filesystem.h"
#include "filesystem/vfs.h"

#include "sd/sd.h"

#define SD_DIAG_MOCK_HANDLE_LIMIT 16u
#define SD_DIAG_MOCK_FIRST_FD (STDERR_FILENO + 1)
#define SD_DIAG_MOCK_WORKSPACE_BUDGET (40u * 1024u)

typedef struct {
    fs_file_t file;
    char path[SD_PATH_MAX];
    FILE* stream;
    int flags;
    bool open;
} mock_file_t;

struct sd_diag_mock_dir {
    fs_dir_t dir;
    bool open;
};

static blockdevice_t* card;
static filesystem_t* fat;
static bool mounted;
static bool break_stat;
static bool corrupt_image;
static bool corrupt_after_remount;
static size_t reformats;
static size_t mounts;
static size_t peak_handles;
static mock_file_t files[SD_DIAG_MOCK_HANDLE_LIMIT];
static DIR directories[SD_DIAG_MOCK_HANDLE_LIMIT];
static bool capture_workspace;
static void* workspace_allocation;

/** Convert the filesystem's negative errors to POSIX return values. */
static ssize_t remap(ssize_t result) {
    errno = result < 0 ? -result : 0;
    return result < 0 ? -1 : result;
}

/** Reject filesystem operations when no fake root is mounted. */
static bool require_mount(void) {
    if (mounted) return true;
    errno = ENOENT;
    return false;
}

/** Bind a fresh fixture without giving production code a test configuration API. */
void sd_diag_mock_bind(blockdevice_t* device, filesystem_t* filesystem) {
    ck_assert_ptr_nonnull(device);
    ck_assert_ptr_nonnull(filesystem);
    ck_assert_uint_eq(sd_diag_mock_open_handles(), 0);
    ck_assert_ptr_null(workspace_allocation);
    card = device;
    fat = filesystem;
    mounted = false;
    break_stat = false;
    corrupt_image = false;
    corrupt_after_remount = false;
    reformats = 0;
    mounts = 0;
    peak_handles = 0;
}

/** Arm allocation observation just before production preparation allocates workspace. */
void sd_diag_mock_capture_workspace(void) {
    ck_assert_ptr_null(workspace_allocation);
    capture_workspace = true;
}

void* __real_calloc(size_t count, size_t size);

/** Observe only the first allocation after preparation starts. */
void* __wrap_calloc(size_t count, size_t size) {
    if (capture_workspace) {
        ck_assert_msg(size != 0 && count <= SD_DIAG_MOCK_WORKSPACE_BUDGET / size,
                      "SD workspace allocation %zu x %zu exceeds the 40 KiB fixture budget",
                      count, size);
    }
    void* const allocation = __real_calloc(count, size);
    if (capture_workspace) {
        workspace_allocation = allocation;
        capture_workspace = false;
    }
    return allocation;
}

/** Supply the fixture's card through the production SD getter. */
blockdevice_t* __wrap_sd_blockdevice(void) {
    return card;
}

/** Supply the fixture's FAT object through the production SD getter. */
filesystem_t* __wrap_sd_fat_filesystem(void) {
    return fat;
}

/** Model the root mount with the actual portable FAT implementation. */
int fs_mount(const char* path, filesystem_t* filesystem, blockdevice_t* device) {
    ck_assert_str_eq(path, "/");
    ck_assert_ptr_eq(filesystem, fat);
    ck_assert_ptr_eq(device, card);
    ck_assert(!mounted);
    const int result = fat->mount(fat, card, false);
    if (result == 0) {
        mounted = true;
        mounts++;
    }
    return remap(result);
}

/** Require all fake VFS handles to close before unmounting the root. */
int fs_unmount(const char* path) {
    ck_assert_str_eq(path, "/");
    ck_assert(mounted);
    ck_assert_uint_eq(sd_diag_mock_open_handles(), 0);
    const int result = fat->unmount(fat);
    if (result == 0) mounted = false;
    return remap(result);
}

/** Format the same card used by raw and direct-FAT diagnostic cases. */
int fs_reformat(const char* path) {
    if (fs_unmount(path) != 0) return -1;
    if (remap(fat->format(fat, card)) != 0) return -1;
    if (fs_mount(path, fat, card) != 0) return -1;
    ++reformats;
    return 0;
}

/** Preserve the production helper's idempotent root-mount contract. */
bool __wrap_sd_mount_root(void) {
    return mounted || fs_mount("/", fat, card) == 0;
}

/** Preserve the production helper's idempotent root-unmount contract. */
bool __wrap_sd_unmount_root(void) {
    return !mounted || fs_unmount("/") == 0;
}

/** Allocate a fake descriptor or stream handle over a direct FAT file. */
static mock_file_t* open_file(const char* path, int flags) {
    if (!require_mount()) return NULL;
    ck_assert_ptr_nonnull(path);
    ck_assert(path[0] == '/');
    if (strlen(path) >= SD_PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    for (size_t index = 0; index < count_of(files); ++index) {
        mock_file_t* const file = &files[index];
        if (file->open) continue;
        *file = (mock_file_t){ 0 };
        if (remap(fat->file_open(fat, &file->file, path, flags)) != 0) return NULL;
        strcpy(file->path, path);
        file->flags = flags;
        file->open = true;
        const size_t live = sd_diag_mock_open_handles();
        if (live > peak_handles) peak_handles = live;
        return file;
    }
    errno = EMFILE;
    return NULL;
}

/** Resolve a fake POSIX descriptor without touching host OS descriptors. */
static mock_file_t* descriptor_file(int fd) {
    if (fd >= SD_DIAG_MOCK_FIRST_FD &&
        (size_t)(fd - SD_DIAG_MOCK_FIRST_FD) < count_of(files)) {
        mock_file_t* const file = &files[fd - SD_DIAG_MOCK_FIRST_FD];
        if (file->open && file->stream == NULL) return file;
    }
    errno = EBADF;
    return NULL;
}

/** Close a direct FAT handle and retire its fake descriptor or stream. */
static int close_file(mock_file_t* file) {
    const int result = fat->file_close(fat, &file->file);
    file->open = false;
    file->stream = NULL;
    return remap(result);
}

/** Open a production path on the fake card instead of the host filesystem. */
int __wrap_open(const char* path, int flags, ...) {
    mock_file_t* const file = open_file(path, flags);
    return file != NULL ? (int)(file - files) + SD_DIAG_MOCK_FIRST_FD : -1;
}

/** Close a fake POSIX descriptor. */
int __wrap_close(int fd) {
    mock_file_t* const file = descriptor_file(fd);
    return file != NULL ? close_file(file) : -1;
}

/** Read bytes through a fake descriptor. */
ssize_t __wrap_read(int fd, void* buffer, size_t size) {
    mock_file_t* const file = descriptor_file(fd);
    return file != NULL ? remap(fat->file_read(fat, &file->file, buffer, size)) : -1;
}

/** Write bytes through a fake descriptor. */
ssize_t __wrap_write(int fd, const void* buffer, size_t size) {
    mock_file_t* const file = descriptor_file(fd);
    return file != NULL ? remap(fat->file_write(fat, &file->file, buffer, size)) : -1;
}

/** Seek the direct FAT handle owned by a fake descriptor. */
off_t __wrap_lseek(int fd, off_t offset, int whence) {
    mock_file_t* const file = descriptor_file(fd);
    return file != NULL ? remap(fat->file_seek(fat, &file->file, offset, whence)) : -1;
}

/** Read metadata from the fake mounted filesystem. */
int __wrap_stat(const char* path, struct stat* info) {
    return require_mount() ? remap(fat->stat(fat, path, info)) : -1;
}

/** Model the device VFS's path-based FAT descriptor stat. */
int __wrap_fstat(int fd, struct stat* info) {
    mock_file_t* const file = descriptor_file(fd);
    if (file == NULL || __wrap_stat(file->path, info) != 0) return -1;
    if (break_stat) ++info->st_size;
    return 0;
}

/** Create a directory on the fake card. */
int __wrap_mkdir(const char* path, mode_t mode) {
    return require_mount() ? remap(fat->mkdir(fat, path, mode)) : -1;
}

/** Rename a path on the fake card. */
int __wrap_rename(const char* old_path, const char* new_path) {
    return require_mount() ? remap(fat->rename(fat, old_path, new_path)) : -1;
}

/** Remove a file on the fake card. */
int __wrap_unlink(const char* path) {
    return require_mount() ? remap(fat->remove(fat, path)) : -1;
}

/** Remove a directory on the fake card. */
int __wrap_rmdir(const char* path) {
    return require_mount() ? remap(fat->rmdir(fat, path)) : -1;
}

/** Remove a stdio workload file on the fake card. */
int __wrap_remove(const char* path) {
    return __wrap_unlink(path);
}

/** Open a direct FAT directory using the target's opaque DIR interface. */
DIR* opendir(const char* path) {
    if (!require_mount()) return NULL;
    for (size_t index = 0; index < count_of(directories); ++index) {
        DIR* const directory = &directories[index];
        if (directory->open) continue;
        if (remap(fat->dir_open(fat, &directory->dir, path)) != 0) return NULL;
        directory->open = true;
        const size_t live = sd_diag_mock_open_handles();
        if (live > peak_handles) peak_handles = live;
        return directory;
    }
    errno = EMFILE;
    return NULL;
}

/** Return the next target-layout entry without using glibc's dirent type. */
struct dirent* readdir(DIR* directory) {
    ck_assert_ptr_nonnull(directory);
    ck_assert(directory->open);
    const int result = fat->dir_read(fat, &directory->dir, &directory->dir.current);
    if (result != 0) {
        errno = result == -ENOENT ? 0 : -result;
        return NULL;
    }
    return &directory->dir.current;
}

/** Release a fake directory's direct FAT handle. */
int closedir(DIR* directory) {
    ck_assert_ptr_nonnull(directory);
    ck_assert(directory->open);
    const int result = fat->dir_close(fat, &directory->dir);
    directory->open = false;
    return remap(result);
}

/** Read a cookie-backed stdio stream from the fake card. */
static ssize_t cookie_read(void* context, char* buffer, size_t size) {
    mock_file_t* const file = context;
    const ssize_t result = remap(fat->file_read(fat, &file->file, buffer, size));
    // Image verification reopens read-only after retiring the writable population and root.
    if (corrupt_image && (!corrupt_after_remount || (file->flags & O_ACCMODE) == O_RDONLY) &&
        result > 0 && strstr(file->path, "/disk/diag_") != NULL) {
        buffer[0] ^= 1u;
        corrupt_image = false;
    }
    return result;
}

/** Write a cookie-backed stdio stream to the fake card. */
static ssize_t cookie_write(void* context, const char* buffer, size_t size) {
    mock_file_t* const file = context;
    return remap(fat->file_write(fat, &file->file, buffer, size));
}

/** Translate glibc's cookie seek protocol to the fake card's FAT cursor. */
static int cookie_seek(void* context, off64_t* offset, int whence) {
    mock_file_t* const file = context;
    const off_t result = fat->file_seek(fat, &file->file, *offset, whence);
    if (result < 0) return remap(result);
    *offset = result;
    return 0;
}

/** Close the FAT file when libc closes its cookie stream. */
static int cookie_close(void* context) {
    return close_file(context);
}

/** Translate the diagnostic's standard fopen modes to FAT open flags. */
FILE* __wrap_fopen(const char* path, const char* mode) {
    int flags;
    switch (mode[0]) {
        case 'r': flags = O_RDONLY; break;
        case 'w': flags = O_WRONLY | O_CREAT | O_TRUNC; break;
        case 'a': flags = O_WRONLY | O_CREAT | O_APPEND; break;
        default:
            errno = EINVAL;
            return NULL;
    }
    if (strchr(mode, '+') != NULL)
        flags = (flags & ~O_ACCMODE) | O_RDWR;
    mock_file_t* const file = open_file(path, flags);
    if (file == NULL) return NULL;
    const cookie_io_functions_t operations = {
        .read = cookie_read,
        .write = cookie_write,
        .seek = cookie_seek,
        .close = cookie_close,
    };
    file->stream = fopencookie(file, mode, operations);
    if (file->stream == NULL) {
        const int error = errno;
        close_file(file);
        errno = error;
    }
    return file->stream;
}

/** Count all fake VFS resources independently of diagnostic workspace bookkeeping. */
size_t sd_diag_mock_open_handles(void) {
    size_t count = 0;
    for (size_t index = 0; index < count_of(files); ++index)
        count += files[index].open;
    for (size_t index = 0; index < count_of(directories); ++index)
        count += directories[index].open;
    return count;
}

/** Report successful reformats for repeated-cycle validation. */
size_t sd_diag_mock_reformats(void) {
    return reformats;
}

/** Report root construction independently of the diagnostic's bookkeeping. */
size_t sd_diag_mock_mounts(void) {
    return mounts;
}

/** Report the observed maximum live resource population. */
size_t sd_diag_mock_peak_handles(void) {
    return peak_handles;
}

/** Corrupt one image read, optionally delaying until clean root reconstruction. */
void sd_diag_mock_corrupt_image_read(bool after_remount) {
    corrupt_image = true;
    corrupt_after_remount = after_remount;
}

/** Inject an incorrect size at the mocked descriptor-stat boundary. */
void sd_diag_mock_break_stat(void) {
    break_stat = true;
}

/** Retire fake handles and the process-lifetime workspace after a forked test. */
void sd_diag_mock_cleanup(void) {
    for (size_t index = 0; index < count_of(files); ++index) {
        if (!files[index].open) continue;
        if (files[index].stream != NULL)
            ck_assert_int_eq(fclose(files[index].stream), 0);
        else
            ck_assert_int_eq(close_file(&files[index]), 0);
    }
    for (size_t index = 0; index < count_of(directories); ++index)
        if (directories[index].open) ck_assert_int_eq(closedir(&directories[index]), 0);
    if (mounted) ck_assert_int_eq(fs_unmount("/"), 0);
    free(workspace_allocation);
    workspace_allocation = NULL;
    capture_workspace = false;
}

// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

// Must precede pch.h to expose GNU fopencookie().
#define _GNU_SOURCE

#include "pch.h"
#include "test_support.h"

#include <time.h>

#include "fatal.h"
#include "sd/sd.h"

#define MICROSECONDS_PER_SECOND 1000000
#define NANOSECONDS_PER_MICROSECOND 1000
#define MICROSECONDS_PER_MILLISECOND 1000

typedef struct mem_file_s {
    char path[SD_PATH_MAX];
    uint8_t* content;
    size_t size;
    size_t capacity;
    bool writable;
    struct mem_file_s* next;
} mem_file_t;

static mem_file_t* mem_files = NULL;

// Finds a registered file without accessing the host filesystem.
static mem_file_t* find_mem_file(const char* path) {
    for (mem_file_t* file = mem_files; file != NULL; file = file->next) {
        if (strcmp(file->path, path) == 0) {
            return file;
        }
    }
    return NULL;
}

// Registers a read-only text fixture using the binary backing store.
void test_register_file(const char* path, const char* content) {
    test_register_binary_file(path, content, strlen(content), false);
}

// Copies fixture bytes into a uniquely named, optionally writable backing store.
void test_register_binary_file(const char* path, const void* data, size_t size,
                               bool writable) {
    // Check fixture identity before allocating its metadata and content.
    assert(path[0] == '/');
    vet_path_length(strlen(path));
    assert(find_mem_file(path) == NULL);

    mem_file_t* file = malloc(sizeof(mem_file_t));
    strncpy(file->path, path, sizeof(file->path) - 1);
    file->path[sizeof(file->path) - 1] = '\0';

    file->size = size;
    file->capacity = size;
    file->writable = writable;
    file->content = malloc(file->capacity);
    memcpy(file->content, data, file->size);

    // Publish the fully initialized fixture to subsequent open calls.
    file->next = mem_files;
    mem_files = file;
}

// Unlinks and frees a registered fixture once its handles are closed.
void test_unregister_file(const char* path) {
    mem_file_t** pp = &mem_files;
    while (*pp) {
        if (strcmp((*pp)->path, path) == 0) {
            mem_file_t* to_free = *pp;
            *pp = to_free->next;
            free(to_free->content);
            free(to_free);
            return;
        }
        pp = &(*pp)->next;
    }
}

// Frees every registered fixture once its handles are closed.
void test_clear_files(void) {
    while (mem_files) {
        mem_file_t* next = mem_files->next;
        free(mem_files->content);
        free(mem_files);
        mem_files = next;
    }
}

// Opens registered text fixtures for firmware SD-card reads.
FILE* sd_open(const char* path, const char* mode) {
    vet_path_length(strlen(path));
    assert(path[0] == '/');

    mem_file_t* mem_file = find_mem_file(path);
    if (mem_file != NULL) {
        if (strcmp(mode, "r") == 0) {
            return fmemopen(mem_file->content, mem_file->size, "r");
        }
        fprintf(stderr, "FATAL: Write mode not supported for in-memory file '%s'\n", path);
        exit(EXIT_FAILURE);
    }

    fprintf(stderr, "FATAL: File '%s' not registered in test file system.\n", path);
    fprintf(stderr, "Use test_register_file() to register files for testing.\n");
    exit(EXIT_FAILURE);
}

// Reads a cookie-backed fixture and advances its per-handle offset.
static ssize_t test_file_read(void* cookie, char* buffer, size_t size) {
    mem_file_t* file = cookie;
    size_t* pos = (size_t*) (file + 1);
    size_t remaining = file->size - *pos;
    if (size > remaining) size = remaining;
    memcpy(buffer, file->content + *pos, size);
    *pos += size;
    return (ssize_t) size;
}

// Writes within the registered capacity of a mutable fixture.
static ssize_t test_file_write(void* cookie, const char* buffer, size_t size) {
    mem_file_t* file = cookie;
    size_t* pos = (size_t*) (file + 1);
    if (!file->writable || *pos > file->capacity || size > file->capacity - *pos) return -1;
    memcpy(file->content + *pos, buffer, size);
    *pos += size;
    if (*pos > file->size) file->size = *pos;
    return (ssize_t) size;
}

// Resolves stdio seeks within a registered fixture.
static int test_file_seek(void* cookie, off64_t* offset, int whence) {
    mem_file_t* file = cookie;
    size_t* pos = (size_t*) (file + 1);
    int64_t target = *offset;
    if (whence == SEEK_CUR) target += (int64_t) *pos;
    if (whence == SEEK_END) target += (int64_t) file->size;
    if (target < 0 || (uint64_t) target > file->size) return -1;
    *pos = (size_t) target;
    *offset = target;
    return 0;
}

// Releases the handle without removing its registered backing file.
static int test_file_close(void* cookie) {
    free(cookie);
    return 0;
}

extern FILE* __real_fopen(const char* path, const char* mode);

// Wraps registered fixtures, delegating other paths to the real host fopen().
FILE* __wrap_fopen(const char* path, const char* mode) {
    // Delegate host paths and reject updates to read-only fixtures.
    mem_file_t* file = find_mem_file(path);
    if (file == NULL) return __real_fopen(path, mode);
    if (strchr(mode, '+') != NULL && !file->writable) return NULL;

    // Give each handle its own metadata snapshot and cursor.
    mem_file_t* cookie = malloc(sizeof(*cookie) + sizeof(size_t));
    *cookie = *file;
    *(size_t*) (cookie + 1) = 0;
    // Bind stdio operations to the registered backing bytes.
    cookie_io_functions_t io = {
        .read = test_file_read,
        .write = test_file_write,
        .seek = test_file_seek,
        .close = test_file_close,
    };
    return fopencookie(cookie, strchr(mode, '+') != NULL ? "r+" : "r", io);
}

// Returns monotonic host time in the microsecond units expected by Pico code.
uint64_t time_us_64(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * MICROSECONDS_PER_SECOND
        + (uint64_t)ts.tv_nsec / NANOSECONDS_PER_MICROSECOND;
}

// Returns the Pico-compatible representation of host monotonic time.
absolute_time_t get_absolute_time(void) {
    return time_us_64();
}

// Converts a host microsecond timestamp to Pico-compatible milliseconds.
uint32_t to_ms_since_boot(absolute_time_t time) {
    return (uint32_t) (time / MICROSECONDS_PER_MILLISECOND);
}

// Provides the firmware allocator with the existing host allocation assertion.
void* vetted_malloc(size_t size) {
    void* p = malloc(size);
    assert(p != NULL);
    return p;
}

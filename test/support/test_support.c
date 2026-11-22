// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

// Must precede pch.h to expose GNU fopencookie().
#define _GNU_SOURCE

#include "pch.h"
#include "test_support.h"

#include <strings.h>
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
    bool fail_writes;
    size_t open_calls;
    struct mem_file_s* owner;
    struct mem_file_s* next;
} mem_file_t;

static mem_file_t* mem_files = NULL;
static size_t open_cookie_count;
static size_t peak_open_cookie_count;

typedef struct file_alias_s {
    char alias[SD_PATH_MAX];
    char path[SD_PATH_MAX];
    struct file_alias_s* next;
} file_alias_t;

static file_alias_t* file_aliases;

// Reports live stdio borrowers independently of registered backing ownership.
size_t test_open_file_count(void) { return open_cookie_count; }

// Observes candidate overlap independently of the facade's final owner count.
size_t test_peak_open_file_count(void) { return peak_open_cookie_count; }

// Finds a registered file without accessing the host filesystem.
static mem_file_t* find_mem_file(const char* path) {
    for (const file_alias_t* alias = file_aliases; alias != NULL; alias = alias->next) {
        if (strcmp(alias->alias, path) == 0) {
            path = alias->path;
            break;
        }
    }
    for (mem_file_t* file = mem_files; file != NULL; file = file->next) {
        if (strcmp(file->path, path) == 0) {
            return file;
        }
    }
    return NULL;
}

// Supplies filesystem-resolved identities independently of the mount facade's matching.
bool sd_resolve_path(char path[SD_PATH_MAX]) {
    const mem_file_t* file = find_mem_file(path);
    for (const mem_file_t* current = mem_files; file == NULL && current != NULL; current = current->next) {
        if (strcasecmp(current->path, path) == 0) file = current;
    }
    if (file != NULL) {
        strcpy(path, file->path);
        return true;
    }
    char* const resolved = realpath(path, NULL);
    if (resolved == NULL) return false;
    const bool fits = strlen(resolved) < SD_PATH_MAX;
    if (fits) strcpy(path, resolved);
    free(resolved);
    return fits;
}

// Models an alias returned by the backing filesystem, not by the mount algorithm.
void test_register_file_alias(const char* alias, const char* path) {
    assert(find_mem_file(path) != NULL);
    vet_path_length(strlen(alias));
    vet_path_length(strlen(path));
    file_alias_t* const entry = malloc(sizeof(*entry));
    assert(entry != NULL);
    strcpy(entry->alias, alias);
    strcpy(entry->path, path);
    entry->next = file_aliases;
    file_aliases = entry;
}

// Share duplicate detection with fixture owners before they claim a path.
bool test_file_registered(const char* path) { return find_mem_file(path) != NULL; }

// Observes transient duplicate opens even when the candidate is subsequently closed.
size_t test_file_open_calls(const char* path) {
    const mem_file_t* const file = find_mem_file(path);
    assert(file != NULL);
    return file->open_calls;
}

// Registers a read-only text fixture using the binary backing store.
void test_register_file(const char* path, const char* content) {
    test_register_binary_file(path, content, strlen(content), false);
}

// Copies fixture bytes into a uniquely named, optionally writable backing store.
void test_register_binary_file(const char* path, const void* data, size_t size, bool writable) {
    // Check fixture identity before allocating its metadata and content.
    assert(path[0] == '/');
    vet_path_length(strlen(path));
    assert(!test_file_registered(path));

    mem_file_t* file = malloc(sizeof(mem_file_t));
    strncpy(file->path, path, sizeof(file->path) - 1);
    file->path[sizeof(file->path) - 1] = '\0';

    file->size = size;
    file->capacity = size;
    file->writable = writable;
    file->fail_writes = false;
    file->open_calls = 0;
    file->owner = file;
    file->content = malloc(file->capacity);
    memcpy(file->content, data, file->size);

    // Publish the fully initialized fixture to subsequent open calls.
    file->next = mem_files;
    mem_files = file;
}

// Share fault state with already-open cookies without changing their offsets.
void test_fail_file_writes(const char* path, bool fail) {
    mem_file_t* file = find_mem_file(path);
    assert(file != NULL);
    file->fail_writes = fail;
}

// Unlinks and frees a registered fixture once its handles are closed.
void test_unregister_file(const char* path) {
    file_alias_t** alias = &file_aliases;
    while (*alias != NULL) {
        if (strcmp((*alias)->path, path) == 0) {
            file_alias_t* const retired = *alias;
            *alias = retired->next;
            free(retired);
        } else {
            alias = &(*alias)->next;
        }
    }
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
    peak_open_cookie_count = open_cookie_count;
    while (file_aliases != NULL) {
        file_alias_t* const next = file_aliases->next;
        free(file_aliases);
        file_aliases = next;
    }
    while (mem_files) {
        mem_file_t* next = mem_files->next;
        free(mem_files->content);
        free(mem_files);
        mem_files = next;
    }
}

FILE* __wrap_fopen(const char* path, const char* mode);

// Opens tracked independent readers for registered configuration fixtures.
FILE* sd_open(const char* path, const char* mode) {
    vet_path_length(strlen(path));
    assert(path[0] == '/');

    mem_file_t* mem_file = find_mem_file(path);
    if (mem_file != NULL) {
        if (strcmp(mode, "r") == 0) {
            return __wrap_fopen(path, mode);
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
    size_t* pos = (size_t*)(file + 1);
    size_t remaining = file->size - *pos;
    if (size > remaining)
        size = remaining;
    memcpy(buffer, file->content + *pos, size);
    *pos += size;
    return (ssize_t)size;
}

// Writes within the registered capacity of a mutable fixture.
static ssize_t test_file_write(void* cookie, const char* buffer, size_t size) {
    mem_file_t* file = cookie;
    size_t* pos = (size_t*)(file + 1);
    if (!file->writable || file->owner->fail_writes || *pos > file->capacity || size > file->capacity - *pos)
        return -1;
    memcpy(file->content + *pos, buffer, size);
    *pos += size;
    if (*pos > file->size)
        file->size = *pos;
    return (ssize_t)size;
}

// Resolves stdio seeks within a registered fixture.
static int test_file_seek(void* cookie, off64_t* offset, int whence) {
    mem_file_t* file = cookie;
    size_t* pos = (size_t*)(file + 1);
    int64_t target = *offset;
    if (whence == SEEK_CUR)
        target += (int64_t)*pos;
    if (whence == SEEK_END)
        target += (int64_t)file->size;
    if (target < 0 || (uint64_t)target > file->size)
        return -1;
    *pos = (size_t)target;
    *offset = target;
    return 0;
}

// Releases the handle without removing its registered backing file.
static int test_file_close(void* cookie) {
    --open_cookie_count;
    free(cookie);
    return 0;
}

extern FILE* __real_fopen(const char* path, const char* mode);

// Wraps registered fixtures, delegating other paths to the real host fopen().
FILE* __wrap_fopen(const char* path, const char* mode) {
    // Delegate host paths and reject updates to read-only fixtures.
    mem_file_t* file = find_mem_file(path);
    if (file == NULL)
        return __real_fopen(path, mode);
    if (strchr(mode, '+') != NULL && !file->writable)
        return NULL;

    // Give each handle its own metadata snapshot and cursor.
    mem_file_t* cookie = malloc(sizeof(*cookie) + sizeof(size_t));
    *cookie = *file;
    *(size_t*)(cookie + 1) = 0;
    // Bind stdio operations to the registered backing bytes.
    cookie_io_functions_t io = {
        .read = test_file_read,
        .write = test_file_write,
        .seek = test_file_seek,
        .close = test_file_close,
    };
    FILE* const stream = fopencookie(cookie, strchr(mode, '+') != NULL ? "r+" : "r", io);
    if (stream != NULL) {
        ++open_cookie_count;
        if (open_cookie_count > peak_open_cookie_count)
            peak_open_cookie_count = open_cookie_count;
        ++file->open_calls;
    } else {
        free(cookie);
    }
    return stream;
}

// Returns monotonic host time in the microsecond units expected by Pico code.
uint64_t time_us_64(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * MICROSECONDS_PER_SECOND + (uint64_t)ts.tv_nsec / NANOSECONDS_PER_MICROSECOND;
}

// Returns the Pico-compatible representation of host monotonic time.
absolute_time_t get_absolute_time(void) { return time_us_64(); }

// Converts a host microsecond timestamp to Pico-compatible milliseconds.
uint32_t to_ms_since_boot(absolute_time_t time) { return (uint32_t)(time / MICROSECONDS_PER_MILLISECOND); }

// Fail if a host test reaches a real hardware delay without supplying its own mock.
__attribute__((weak)) void sleep_us(uint64_t us) {
    (void)us;
    assert(false && "unexpected sleep_us call in host test");
    abort();
}

// Fail if a host test initializes serial hardware without supplying its own mock.
__attribute__((weak)) bool stdio_init_all(void) {
    assert(false && "unexpected stdio_init_all call in host test");
    abort();
}

// Fail if a host test initializes GPIO without supplying its own mock.
__attribute__((weak)) void gpio_init(unsigned int gpio) {
    (void)gpio;
    assert(false && "unexpected gpio_init call in host test");
    abort();
}

// Fail if a host test configures GPIO direction without supplying its own mock.
__attribute__((weak)) void gpio_set_dir(unsigned int gpio, bool output) {
    (void)gpio;
    (void)output;
    assert(false && "unexpected gpio_set_dir call in host test");
    abort();
}

// Fail if a host test drives GPIO without supplying its own mock.
__attribute__((weak)) void gpio_put(unsigned int gpio, bool value) {
    (void)gpio;
    (void)value;
    assert(false && "unexpected gpio_put call in host test");
    abort();
}

// Provides the firmware allocator with a Debug-only host allocation check.
void* vetted_malloc(size_t size) {
    void* p = malloc(size);
    assert(p != NULL);
    return p;
}

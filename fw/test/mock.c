// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "mock.h"

#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <check.h>

#include "driver.h"
#include "sd/sd.h"
#include "system_state.h"

// ---------------------------------------------------------------------------
// Driver mocks
// ---------------------------------------------------------------------------

uint8_t mock_ram[MOCK_RAM_SIZE];

uint8_t spi_read_at(uint32_t addr) {
    ck_assert_uint_lt(addr, MOCK_RAM_SIZE);
    return mock_ram[addr];
}

void spi_read(uint32_t addr, size_t byteLength, uint8_t* pDest) {
    for (size_t i = 0; i < byteLength; i++) {
        ck_assert_uint_lt(addr + i, MOCK_RAM_SIZE);
        pDest[i] = mock_ram[addr + i];
    }
}

uint8_t spi_write_at(uint32_t addr, uint8_t data) {
    if (addr < MOCK_RAM_SIZE) {
        mock_ram[addr] = data;
    }
    return 0;
}

void spi_write(uint32_t addr, const uint8_t* pSrc, size_t byteLength) {
    for (size_t i = 0; i < byteLength; i++) {
        if (addr + i < MOCK_RAM_SIZE) {
            mock_ram[addr + i] = pSrc[i];
        }
    }
}

uint8_t spi_read_next(void) { return 0; }
uint8_t spi_read_prev(void) { return 0; }

void spi_fill(uint32_t addr, uint8_t byte, size_t byteLength) {
    (void)addr;
    (void)byte;
    (void)byteLength;
}

void set_cpu(bool ready, bool reset, bool nmi) {
    (void)ready;
    (void)reset;
    (void)nmi;
}

// ---------------------------------------------------------------------------
// Breakpoint mocks
// ---------------------------------------------------------------------------

static uint16_t mock_bp_addr;
static bool mock_bp_cleared;

uint16_t bp_hit_addr(void) {
    return mock_bp_addr;
}

void bp_clear_halt(void) {
    mock_bp_cleared = true;
    system_state.bp_halted = false;
}

// ---------------------------------------------------------------------------
// Test fixture helpers
// ---------------------------------------------------------------------------

void mock_reset(void) {
    memset(mock_ram, 0xEA, sizeof(mock_ram));
    system_state.bp_halted = false;
    mock_bp_addr = 0;
    mock_bp_cleared = false;
}

void mock_breakpoint_set_hit_addr(uint16_t addr) {
    mock_bp_addr = addr;
}

bool mock_breakpoint_halt_was_cleared(void) {
    return mock_bp_cleared;
}

// ---------------------------------------------------------------------------
// In-memory file system mocks
// ---------------------------------------------------------------------------

// In-memory file system for testing
typedef struct mem_file_s {
    char path[PATH_MAX];
    char* content;
    size_t size;
    size_t capacity;
    struct mem_file_s* next;
} mem_file_t;

static mem_file_t* mem_files = NULL;

// Find an in-memory file
static mem_file_t* find_mem_file(const char* path) {
    for (mem_file_t* file = mem_files; file != NULL; file = file->next) {
        if (strcmp(file->path, path) == 0) {
            return file;
        }
    }
    return NULL;
}

// Register an in-memory file for testing
void mock_register_file(const char* path, const char* content) {
    assert(path[0] == '/');
    
    // Caller must unregister an existing file before re-registering it.
    assert(find_mem_file(path) == NULL);
    
    mem_file_t* file = malloc(sizeof(mem_file_t));
    strncpy(file->path, path, sizeof(file->path) - 1);
    file->path[sizeof(file->path) - 1] = '\0';
    
    file->size = strlen(content);
    file->capacity = file->size + 1;
    file->content = malloc(file->capacity);
    memcpy(file->content, content, file->size + 1);
    
    file->next = mem_files;
    mem_files = file;
}

// Unregister an in-memory file
void mock_unregister_file(const char* path) {
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

// Clear all registered in-memory files
void mock_clear_files(void) {
    while (mem_files) {
        mem_file_t* next = mem_files->next;
        free(mem_files->content);
        free(mem_files);
        mem_files = next;
    }
}

// Mock implementation of 'sd_open' returns contents of previously registered in-memory files
// using 'mock_register_file()'.
FILE* sd_open(const char* path, const char* mode) {
    assert(path[0] == '/');
    
    // Check if this is an in-memory file
    mem_file_t* mem_file = find_mem_file(path);
    if (mem_file != NULL) {
        // For in-memory files, only support read mode
        if (strcmp(mode, "r") == 0) {
            return fmemopen(mem_file->content, mem_file->size, "r");
        }
        // If write mode requested for in-memory file, fail
        fprintf(stderr, "FATAL: Write mode not supported for in-memory file '%s'\n", path);
        exit(1);
    }
    
    // File not registered - fail with clear error
    fprintf(stderr, "FATAL: File '%s' not registered in mock file system.\n", path);
    fprintf(stderr, "Use mock_register_file() to register files for testing.\n");
    exit(1);
}

// ---------------------------------------------------------------------------
// Display mocks
// ---------------------------------------------------------------------------

void display_window_begin(const void* window) {
    (void)window;
}

void display_window_show(const void* window) {
    (void)window;
    fflush(stdout);
}

void display_task(void) { }

// ---------------------------------------------------------------------------
// Input mocks
// ---------------------------------------------------------------------------

void input_init(void) { }

void input_task(void) { }

int input_getch(void) {
    return EOF;
}

// ---------------------------------------------------------------------------
// ROM, PET, and IEEE mocks
// ---------------------------------------------------------------------------

void roms_refresh_char_rom() { }
void pet_nmi() { }
void ieee_drive_unmount_all() { }

// ---------------------------------------------------------------------------
// Diagnostic mocks
// ---------------------------------------------------------------------------

void test_ram() { }

// ---------------------------------------------------------------------------
// Pico SDK mocks
// ---------------------------------------------------------------------------

uint64_t time_us_64(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

// ---------------------------------------------------------------------------
// System mocks
// ---------------------------------------------------------------------------

// Mock fatal function. Prints the formatted error message to stderr, then calls
// abort().  If the test case expects fatal to be called, use
// `tcase_add_test_raise_signal(tc, test_fn, SIGABRT)` in a forked runner.
void fatal(const char* const format, ...) {
    va_list args;
    va_start(args, format);
    fprintf(stderr, "fatal: ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
    abort();
}

void* vetted_malloc(size_t size) {
    void* p = malloc(size);
    assert(p != NULL);
    return p;
}

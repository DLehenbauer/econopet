#define _GNU_SOURCE

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

#define MOCK_IEEE_BASE (0b01110u << 15)

#define MOCK_IEEE_CTRL     (MOCK_IEEE_BASE + 0)
#define MOCK_IEEE_STATUS   (MOCK_IEEE_BASE + 1)
#define MOCK_IEEE_RX       (MOCK_IEEE_BASE + 2)
#define MOCK_IEEE_TX       (MOCK_IEEE_BASE + 3)
#define MOCK_IEEE_TX_LAST  (MOCK_IEEE_BASE + 4)
#define MOCK_IEEE_TXS      (MOCK_IEEE_BASE + 6)
#define MOCK_IEEE_TXS_LAST (MOCK_IEEE_BASE + 7)

#define MOCK_IEEE_RX_CAPACITY 32
#define MOCK_IEEE_TX_CAPACITY 1024
#define MOCK_IEEE_TXS_CAPACITY 32

typedef struct {
    uint8_t byte;
    bool eoi;
} mock_ieee_tx_byte_t;

static struct {
    uint8_t ctrl;
    struct {
        uint8_t byte;
        bool atn;
    } rx[MOCK_IEEE_RX_CAPACITY];
    size_t rx_head;
    size_t rx_count;
    mock_ieee_tx_byte_t data[MOCK_IEEE_TX_CAPACITY];
    size_t data_count;
    mock_ieee_tx_byte_t status[MOCK_IEEE_TXS_CAPACITY];
    size_t status_count;
} mock_ieee;

static bool mock_ieee_addr(uint32_t addr) {
    return addr >= MOCK_IEEE_BASE && addr < MOCK_IEEE_BASE + 8;
}

static void mock_ieee_flush(bool data_only) {
    mock_ieee.data_count = 0;
    if (!data_only) {
        mock_ieee.rx_head = 0;
        mock_ieee.rx_count = 0;
        mock_ieee.status_count = 0;
    }
}

void mock_ieee_enqueue_rx(bool atn, uint8_t byte) {
    ck_assert_uint_lt(mock_ieee.rx_count, MOCK_IEEE_RX_CAPACITY);
    size_t index = (mock_ieee.rx_head + mock_ieee.rx_count) % MOCK_IEEE_RX_CAPACITY;
    mock_ieee.rx[index].atn = atn;
    mock_ieee.rx[index].byte = byte;
    mock_ieee.rx_count++;
}

size_t mock_ieee_data_count(void) {
    return mock_ieee.data_count;
}

uint8_t mock_ieee_data_byte(size_t index) {
    ck_assert_uint_lt(index, mock_ieee.data_count);
    return mock_ieee.data[index].byte;
}

bool mock_ieee_data_eoi(size_t index) {
    ck_assert_uint_lt(index, mock_ieee.data_count);
    return mock_ieee.data[index].eoi;
}

void mock_ieee_clear_data(void) {
    mock_ieee.data_count = 0;
}

size_t mock_ieee_status_count(void) {
    return mock_ieee.status_count;
}

uint8_t mock_ieee_status_byte(size_t index) {
    ck_assert_uint_lt(index, mock_ieee.status_count);
    return mock_ieee.status[index].byte;
}

bool mock_ieee_status_eoi(size_t index) {
    ck_assert_uint_lt(index, mock_ieee.status_count);
    return mock_ieee.status[index].eoi;
}

void mock_ieee_clear_status(void) {
    mock_ieee.status_count = 0;
}

uint8_t spi_read_at(uint32_t addr) {
    if (mock_ieee_addr(addr)) {
        if (addr == MOCK_IEEE_CTRL) {
            return mock_ieee.data_count <= MOCK_IEEE_TX_CAPACITY - 64 ? 0x02 : 0;
        }
        if (addr == MOCK_IEEE_STATUS) {
            uint8_t status = 0;
            if (mock_ieee.rx_count != 0) {
                status |= 0x01;
                if (mock_ieee.rx[mock_ieee.rx_head].atn) status |= 0x02;
            }
            if (mock_ieee.data_count == MOCK_IEEE_TX_CAPACITY) status |= 0x04;
            if (mock_ieee.data_count == 0) status |= 0x08;
            return status;
        }
        if (addr == MOCK_IEEE_RX) {
            return mock_ieee.rx_count == 0 ? 0 : mock_ieee.rx[mock_ieee.rx_head].byte;
        }
    }
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
    if (mock_ieee_addr(addr)) {
        if (addr == MOCK_IEEE_CTRL) {
            mock_ieee.ctrl = data;
            if (data & 0x02) mock_ieee_flush(false);
            if (data & 0x04) mock_ieee_flush(true);
        } else if (addr == MOCK_IEEE_RX) {
            if (mock_ieee.rx_count != 0) {
                mock_ieee.rx_head = (mock_ieee.rx_head + 1) % MOCK_IEEE_RX_CAPACITY;
                mock_ieee.rx_count--;
            }
        } else if (addr == MOCK_IEEE_TX || addr == MOCK_IEEE_TX_LAST) {
            ck_assert_uint_lt(mock_ieee.data_count, MOCK_IEEE_TX_CAPACITY);
            mock_ieee.data[mock_ieee.data_count++] = (mock_ieee_tx_byte_t) {
                .byte = data,
                .eoi = addr == MOCK_IEEE_TX_LAST,
            };
        } else if (addr == MOCK_IEEE_TXS || addr == MOCK_IEEE_TXS_LAST) {
            ck_assert_uint_lt(mock_ieee.status_count, MOCK_IEEE_TXS_CAPACITY);
            mock_ieee.status[mock_ieee.status_count++] = (mock_ieee_tx_byte_t) {
                .byte = data,
                .eoi = addr == MOCK_IEEE_TXS_LAST,
            };
        }
        return 0;
    }
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

void spi_write_same_block(uint32_t addr, const uint8_t* pSrc, size_t byteLength) {
    for (size_t i = 0; i < byteLength; i++) spi_write_at(addr, pSrc[i]);
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
    memset(&mock_ieee, 0, sizeof(mock_ieee));
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
    uint8_t* content;
    size_t size;
    size_t capacity;
    bool writable;
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
    mock_register_binary_file(path, content, strlen(content), false);
}

void mock_register_binary_file(const char* path, const void* data, size_t size,
                               bool writable) {
    assert(path[0] == '/');

    // Caller must unregister an existing file before re-registering it.
    assert(find_mem_file(path) == NULL);

    mem_file_t* file = malloc(sizeof(mem_file_t));
    strncpy(file->path, path, sizeof(file->path) - 1);
    file->path[sizeof(file->path) - 1] = '\0';

    file->size = size;
    file->capacity = size;
    file->writable = writable;
    file->content = malloc(file->capacity);
    memcpy(file->content, data, file->size);

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

static ssize_t mock_file_read(void* cookie, char* buffer, size_t size) {
    mem_file_t* file = cookie;
    size_t* pos = (size_t*) (file + 1);
    size_t remaining = file->size - *pos;
    if (size > remaining) size = remaining;
    memcpy(buffer, file->content + *pos, size);
    *pos += size;
    return (ssize_t) size;
}

static ssize_t mock_file_write(void* cookie, const char* buffer, size_t size) {
    mem_file_t* file = cookie;
    size_t* pos = (size_t*) (file + 1);
    if (!file->writable || *pos > file->capacity || size > file->capacity - *pos) return -1;
    memcpy(file->content + *pos, buffer, size);
    *pos += size;
    if (*pos > file->size) file->size = *pos;
    return (ssize_t) size;
}

static int mock_file_seek(void* cookie, off64_t* offset, int whence) {
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

static int mock_file_close(void* cookie) {
    free(cookie);
    return 0;
}

extern FILE* __real_fopen(const char* path, const char* mode);

FILE* __wrap_fopen(const char* path, const char* mode) {
    mem_file_t* file = find_mem_file(path);
    if (file == NULL) return __real_fopen(path, mode);
    if (strchr(mode, '+') != NULL && !file->writable) return NULL;

    mem_file_t* cookie = malloc(sizeof(*cookie) + sizeof(size_t));
    *cookie = *file;
    *(size_t*) (cookie + 1) = 0;
    cookie_io_functions_t io = {
        .read = mock_file_read,
        .write = mock_file_write,
        .seek = mock_file_seek,
        .close = mock_file_close,
    };
    return fopencookie(cookie, strchr(mode, '+') != NULL ? "r+" : "r", io);
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

absolute_time_t get_absolute_time(void) {
    return time_us_64();
}

uint32_t to_ms_since_boot(absolute_time_t time) {
    return (uint32_t) (time / 1000u);
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

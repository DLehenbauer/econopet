// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

// Exposes the GNU fopencookie() declaration used by the custom file mock.
#define _GNU_SOURCE

#include "pch.h"
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
#include "fatal.h"
#include "ieee/ieee_registers.h"
#include "sd/sd.h"
#include "system_state.h"

// ---------------------------------------------------------------------------
// Driver mocks
// ---------------------------------------------------------------------------

uint8_t mock_ram[MOCK_RAM_SIZE];

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

// Identifies addresses belonging to the emulated FPGA IEEE register block.
static bool mock_ieee_addr(uint32_t addr) {
    return addr >= ADDR_IEEE && addr < ADDR_IEEE + IEEE_REGISTER_COUNT;
}

// Flushes captured data, optionally clearing receive and status queues too.
static void mock_ieee_flush(bool data_only) {
    mock_ieee.data_count = 0;
    if (!data_only) {
        mock_ieee.rx_head = 0;
        mock_ieee.rx_count = 0;
        mock_ieee.status_count = 0;
    }
}

// Appends one PET byte and ATN tag to the bounded receive FIFO.
void mock_ieee_enqueue_rx(bool atn, uint8_t byte) {
    ck_assert_uint_lt(mock_ieee.rx_count, MOCK_IEEE_RX_CAPACITY);
    size_t index = (mock_ieee.rx_head + mock_ieee.rx_count) % MOCK_IEEE_RX_CAPACITY;
    mock_ieee.rx[index].atn = atn;
    mock_ieee.rx[index].byte = byte;
    mock_ieee.rx_count++;
}

// Returns the captured file-channel byte count.
size_t mock_ieee_data_count(void) {
    return mock_ieee.data_count;
}

// Returns a captured file-channel byte after checking its index.
uint8_t mock_ieee_data_byte(size_t index) {
    ck_assert_uint_lt(index, mock_ieee.data_count);
    return mock_ieee.data[index].byte;
}

// Returns the EOI marker for a captured file-channel byte.
bool mock_ieee_data_eoi(size_t index) {
    ck_assert_uint_lt(index, mock_ieee.data_count);
    return mock_ieee.data[index].eoi;
}

// Models the controller consuming all captured file-channel bytes.
void mock_ieee_clear_data(void) {
    mock_ieee.data_count = 0;
}

// Returns the captured command-channel byte count.
size_t mock_ieee_status_count(void) {
    return mock_ieee.status_count;
}

// Returns a captured command-channel byte after checking its index.
uint8_t mock_ieee_status_byte(size_t index) {
    ck_assert_uint_lt(index, mock_ieee.status_count);
    return mock_ieee.status[index].byte;
}

// Returns the EOI marker for a captured command-channel byte.
bool mock_ieee_status_eoi(size_t index) {
    ck_assert_uint_lt(index, mock_ieee.status_count);
    return mock_ieee.status[index].eoi;
}

// Models the controller consuming all captured command-channel bytes.
void mock_ieee_clear_status(void) {
    mock_ieee.status_count = 0;
}

// Reads an IEEE register without popping RX, or reads the mocked PET RAM.
uint8_t spi_read_at(uint32_t addr) {
    // Synthesize FIFO readiness and peek at the pending receive byte.
    if (mock_ieee_addr(addr)) {
        if (addr == IEEE_REG_CTRL) {
            return mock_ieee.data_count <= MOCK_IEEE_TX_CAPACITY - TX_BURST_CHUNK
                ? IEEE_CTRL_RD_TX_ROOM : 0;
        }
        if (addr == IEEE_REG_STATUS) {
            uint8_t status = 0;
            if (mock_ieee.rx_count != 0) {
                status |= IEEE_ST_RX_AVAIL;
                if (mock_ieee.rx[mock_ieee.rx_head].atn) status |= IEEE_ST_RX_ATN;
            }
            if (mock_ieee.data_count == MOCK_IEEE_TX_CAPACITY) status |= IEEE_ST_TX_FULL;
            if (mock_ieee.data_count == 0) status |= IEEE_ST_TX_EMPTY;
            return status;
        }
        if (addr == IEEE_REG_RX) {
            return mock_ieee.rx_count == 0 ? 0 : mock_ieee.rx[mock_ieee.rx_head].byte;
        }
    }
    // Enforce the address-space bound before accessing ordinary RAM.
    ck_assert_uint_lt(addr, MOCK_RAM_SIZE);
    return mock_ram[addr];
}

// Reads consecutive mocked addresses into the caller's buffer.
void spi_read(uint32_t addr, size_t byteLength, uint8_t* pDest) {
    for (size_t i = 0; i < byteLength; i++) {
        pDest[i] = spi_read_at(addr + i);
    }
}

// Applies IEEE register side effects or writes RAM, returning the mock's zero reply.
uint8_t spi_write_at(uint32_t addr, uint8_t data) {
    // Apply FIFO flush/pop operations and capture outgoing bytes with EOI.
    if (mock_ieee_addr(addr)) {
        if (addr == IEEE_REG_CTRL) {
            mock_ieee.ctrl = data;
            if (data & IEEE_CTRL_FLUSH) mock_ieee_flush(false);
            if (data & IEEE_CTRL_DATA_FLUSH) mock_ieee_flush(true);
        } else if (addr == IEEE_REG_RX) {
            if (mock_ieee.rx_count != 0) {
                mock_ieee.rx_head = (mock_ieee.rx_head + 1) % MOCK_IEEE_RX_CAPACITY;
                mock_ieee.rx_count--;
            }
        } else if (addr == IEEE_REG_TX || addr == IEEE_REG_TX_LAST) {
            ck_assert_uint_lt(mock_ieee.data_count, MOCK_IEEE_TX_CAPACITY);
            mock_ieee.data[mock_ieee.data_count++] = (mock_ieee_tx_byte_t) {
                .byte = data,
                .eoi = addr == IEEE_REG_TX_LAST,
            };
        } else if (addr == IEEE_REG_TXS || addr == IEEE_REG_TXS_LAST) {
            ck_assert_uint_lt(mock_ieee.status_count, MOCK_IEEE_TXS_CAPACITY);
            mock_ieee.status[mock_ieee.status_count++] = (mock_ieee_tx_byte_t) {
                .byte = data,
                .eoi = addr == IEEE_REG_TXS_LAST,
            };
        }
        return 0;
    }
    // Preserve the existing no-op behavior for writes outside mocked RAM.
    if (addr < MOCK_RAM_SIZE) {
        mock_ram[addr] = data;
    }
    return 0;
}

// Writes the caller's bytes to consecutive mocked addresses.
void spi_write(uint32_t addr, const uint8_t* pSrc, size_t byteLength) {
    for (size_t i = 0; i < byteLength; i++) {
        spi_write_at(addr + i, pSrc[i]);
    }
}

// Writes all bytes to one register, as required by FIFO burst transfers.
void spi_write_same_block(uint32_t addr, const uint8_t* pSrc, size_t byteLength) {
    for (size_t i = 0; i < byteLength; i++) spi_write_at(addr, pSrc[i]);
}

// Preserves the legacy zero reply for unused sequential SPI reads.
uint8_t spi_read_next(void) { return 0; }
// Preserves the legacy zero reply for unused reverse SPI reads.
uint8_t spi_read_prev(void) { return 0; }

// Ignores RAM fills in this legacy host mock.
void spi_fill(uint32_t addr, uint8_t byte, size_t byteLength) {
    (void)addr;
    (void)byte;
    (void)byteLength;
}

static cpu_state_t mock_cpu_state;

// Records CPU control flags for subsequent host-side reads.
void set_cpu(cpu_state_t state) {
    mock_cpu_state = state;
}

// Returns the most recently recorded CPU control flags.
cpu_state_t get_cpu(void) {
    return mock_cpu_state;
}

// ---------------------------------------------------------------------------
// Breakpoint mocks
// ---------------------------------------------------------------------------

static uint16_t mock_bp_addr;
static bool mock_bp_cleared;

// Returns the configured breakpoint-hit address.
uint16_t bp_hit_addr(void) {
    return mock_bp_addr;
}

// Records halt acknowledgement and clears the shared halted state.
void bp_clear_halt(void) {
    mock_bp_cleared = true;
    system_state.bp_halted = false;
}

// ---------------------------------------------------------------------------
// Test fixture helpers
// ---------------------------------------------------------------------------

// Restores deterministic RAM contents and clears register/control observations.
void mock_reset(void) {
    // Restore the RAM fixture and empty all emulated IEEE queues.
    memset(mock_ram, 0xEA, sizeof(mock_ram));
    memset(&mock_ieee, 0, sizeof(mock_ieee));
    // Clear breakpoint observations and hold the CPU in reset.
    system_state.bp_halted = false;
    mock_bp_addr = 0;
    mock_bp_cleared = false;
    mock_cpu_state = CPU_RESET;
}

// Configures the address observed by breakpoint handling.
void mock_breakpoint_set_hit_addr(uint16_t addr) {
    mock_bp_addr = addr;
}

// Reports whether breakpoint handling acknowledged a halt.
bool mock_breakpoint_halt_was_cleared(void) {
    return mock_bp_cleared;
}

// ---------------------------------------------------------------------------
// In-memory file system mocks
// ---------------------------------------------------------------------------

// In-memory file system for testing
typedef struct mem_file_s {
    char path[SD_PATH_MAX];
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
    vet_path_length(strlen(path));

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
    vet_path_length(strlen(path));
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

// Accepts a display window without rendering it in host tests.
void display_window_begin(const void* window) {
    (void)window;
}

// Flushes host output instead of presenting a hardware display window.
void display_window_show(const void* window) {
    (void)window;
    fflush(stdout);
}

// Leaves display servicing inactive in host tests.
void display_task(void) { }

// ---------------------------------------------------------------------------
// Input mocks
// ---------------------------------------------------------------------------

// Leaves hardware input initialization inactive in host tests.
void input_init(void) { }

// Leaves hardware input polling inactive in host tests.
void input_task(void) { }

// Reports that no host input character is available.
int input_getch(void) {
    return EOF;
}

// ---------------------------------------------------------------------------
// ROM, PET, and IEEE mocks
// ---------------------------------------------------------------------------

// Leaves character-ROM refresh inactive in host tests.
void roms_refresh_char_rom() { }
// Leaves PET NMI generation inactive in host tests.
void pet_nmi() { }

// ---------------------------------------------------------------------------
// Diagnostic mocks
// ---------------------------------------------------------------------------

// Leaves hardware RAM diagnostics inactive in host tests.
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

static const char* expected_fatal_substring;

void mock_expect_fatal_message(const char* substring) {
    expected_fatal_substring = substring;
}

// Mock fatal function. Prints the formatted error message to stderr, then calls
// abort().  If the test case expects fatal to be called, use
// `tcase_add_test_raise_signal(tc, test_fn, SIGABRT)` in a forked runner.
void fatal(const char* const format, ...) {
    char message[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    if (expected_fatal_substring != NULL &&
        strstr(message, expected_fatal_substring) == NULL) {
        fprintf(
            stderr,
            "fatal message did not contain '%s': %s\n",
            expected_fatal_substring,
            message
        );
        _Exit(EXIT_FAILURE);
    }

    fprintf(stderr, "fatal: %s\n", message);
    abort();
}

void* vetted_malloc(size_t size) {
    void* p = malloc(size);
    assert(p != NULL);
    return p;
}

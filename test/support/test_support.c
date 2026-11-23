// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "test_support.h"

#include <time.h>

#include "hardware/spi.h"
#include "hardware/uart.h"
#include "pico/mutex.h"

#include "fatal.h"
#include "sd/activity.h"
#include "sd/sd.h"

#define MICROSECONDS_PER_SECOND 1000000
#define NANOSECONDS_PER_MICROSECOND 1000
#define MICROSECONDS_PER_MILLISECOND 1000

spi_inst_t test_spi[2] = {{.index = 0}, {.index = 1}};

struct uart_inst {
    unsigned int index;
};
static uart_inst_t test_uart;
uart_inst_t* const uart0 = &test_uart;

// Returns monotonic host time in the microsecond units expected by Pico code.
uint64_t time_us_64(void) {
    struct timespec ts;
    const int result = clock_gettime(CLOCK_MONOTONIC, &ts);
    assert(result == 0 && "host monotonic clock unavailable");
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

// Fail if a host test enables an input pull-up without supplying its own mock.
__attribute__((weak)) void gpio_pull_up(unsigned int gpio) {
    (void)gpio;
    assert(false && "unexpected gpio_pull_up call in host test");
    abort();
}

// Fail if a host test samples GPIO without supplying its own mock.
__attribute__((weak)) bool gpio_get(unsigned int gpio) {
    (void)gpio;
    assert(false && "unexpected gpio_get call in host test");
    abort();
}

// Fail if a host test drives GPIO without supplying its own mock.
__attribute__((weak)) void gpio_put(unsigned int gpio, bool value) {
    (void)gpio;
    (void)value;
    assert(false && "unexpected gpio_put call in host test");
    abort();
}

// Fail if a host test polls UART without supplying a byte source.
__attribute__((weak)) bool uart_is_readable(uart_inst_t* uart) {
    (void)uart;
    assert(false && "unexpected uart_is_readable call in host test");
    abort();
}

// Fail if a host test reads UART without supplying a byte source.
__attribute__((weak)) char uart_getc(uart_inst_t* uart) {
    (void)uart;
    assert(false && "unexpected uart_getc call in host test");
    abort();
}

// Fail unless the fixture links the production activity wrapper or models installation.
__attribute__((weak)) void sd_activity_install(blockdevice_t* device) {
    (void)device;
    assert(false && "unexpected sd_activity_install call in host test");
    abort();
}

// Forward fatal paths for targets using test_fatal.c instead of the hardware renderer.
__attribute__((weak)) void fatal_halt(fatal_error_t error, const char* message) {
    (void)error;
    fatal("%s", message);
}

// Preserve the path contract when the target does not link the production fatal renderer.
__attribute__((weak)) void vet_path_length(size_t length) {
    vet(length < SD_PATH_MAX, "SD path exceeds %u characters (got %zu)", (unsigned int)(SD_PATH_MAX - 1), length);
}

// Provides the firmware allocator with a Debug-only host allocation check.
void* vetted_malloc(size_t size) {
    void* p = malloc(size);
    assert(p != NULL);
    return p;
}

// Initialize ownership tracking for a single-threaded host mutex.
void mutex_init(mutex_t* mutex) {
    assert(mutex != NULL);
    mutex->initialized = true;
    mutex->locked = false;
}

// Reject contention rather than pretending to block in a single-threaded fixture.
void mutex_enter_blocking(mutex_t* mutex) {
    assert(mutex != NULL && mutex->initialized && "host mutex is not initialized");
    assert(!mutex->locked && "contended mutex in single-threaded host fixture");
    mutex->locked = true;
}

// Require every unlock to retire one active owner.
void mutex_exit(mutex_t* mutex) {
    assert(mutex != NULL && mutex->initialized && "host mutex is not initialized");
    assert(mutex->locked && "host mutex unlock without an owner");
    mutex->locked = false;
}

// Initialize nested ownership tracking for a single-threaded host mutex.
void recursive_mutex_init(recursive_mutex_t* mutex) {
    assert(mutex != NULL);
    mutex->initialized = true;
    mutex->depth = 0;
}

// Retain each nested acquisition until its corresponding unlock.
void recursive_mutex_enter_blocking(recursive_mutex_t* mutex) {
    assert(mutex != NULL && mutex->initialized && "host recursive mutex is not initialized");
    assert(mutex->depth < UINT_MAX && "host recursive mutex depth overflow");
    ++mutex->depth;
}

// Reject unmatched unlocks instead of hiding a broken caller's lock balance.
void recursive_mutex_exit(recursive_mutex_t* mutex) {
    assert(mutex != NULL && mutex->initialized && "host recursive mutex is not initialized");
    assert(mutex->depth != 0 && "host recursive mutex unlock without an owner");
    --mutex->depth;
}

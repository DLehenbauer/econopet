// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "mem.h"

#include <inttypes.h>

#include "display/display.h"
#include "driver.h"
#include "hardware_contract.h"
#include "input/pet_keyboard.h"
#include "pet.h"

#define SRAM_ADDR_MIN ECONOPET_WB_RAM_BASE_ADDR
#define SRAM_ADDR_MAX (SRAM_ADDR_MIN + (1u << ECONOPET_RAM_ADDR_WIDTH) - 1u)
#define BRAM_ADDR_MIN ECONOPET_WB_BRAM_BASE_ADDR
#define BRAM_ADDR_MAX (BRAM_ADDR_MIN + (1u << ECONOPET_BRAM_ADDR_WIDTH) - 1u)
#define DATA_BIT_MAX (ECONOPET_DATA_WIDTH - 1)
#define DISPLAY_REFRESH_ADDRESS_MASK 0x3ffu

/** Inclusive address range borrowed by the six March cases. */
typedef struct {
    int32_t addr_min;
    int32_t addr_max;
} mem_region_t;

typedef uint8_t last_read_fn(void);
typedef void march_element_fn(uint32_t addr, uint8_t bit, last_read_fn* last_read);

/** Format one byte portably without relying on newlib's unsupported %b. */
static void format_bits8(uint8_t value, char out[ECONOPET_DATA_WIDTH + 1]) {
    for (unsigned bit = 0; bit < ECONOPET_DATA_WIDTH; ++bit) {
        out[bit] = '0' + ((value >> (DATA_BIT_MAX - bit)) & 1);
    }
    out[ECONOPET_DATA_WIDTH] = '\0';
}

/** Report the first byte mismatch through the diagnostic runner. */
static void check_byte(uint32_t addr, uint8_t actual, uint8_t expected) {
    if (actual == expected) {
        return;
    }
    char actual_bits[ECONOPET_DATA_WIDTH + 1], expected_bits[ECONOPET_DATA_WIDTH + 1];
    format_bits8(actual, actual_bits);
    format_bits8(expected, expected_bits);
    diag_fail("$%05" PRIX32 ": expected $%02X (%s), got $%02X (%s)", addr, (unsigned)expected, expected_bits,
              (unsigned)actual, actual_bits);
}

/** Verify the selected bit before or after its March transition. */
static void check_bit(uint32_t addr, uint8_t actual, uint8_t bit, uint8_t expected) {
    const unsigned actual_bit = (actual >> bit) & 1;
    if (actual_bit == expected) {
        return;
    }
    char bits[ECONOPET_DATA_WIDTH + 1];
    format_bits8(actual, bits);
    diag_fail("$%05" PRIX32 "[%u]: expected %u, got %u (byte $%02X %s)", addr, (unsigned)bit, (unsigned)expected,
              actual_bit, (unsigned)actual, bits);
}

/** Check a bit then flip it for the next March state. */
static uint8_t toggle_bit(uint32_t addr, uint8_t byte, uint8_t bit, uint8_t expected) {
    assert(bit <= DATA_BIT_MAX);
    check_bit(addr, byte, bit, expected);
    return byte ^ (1u << bit);
}

/** Visit every bit upwards, advancing the SPI pipeline only after each byte's last bit. */
static void test_each_bit_ascending(const mem_region_t* region, march_element_fn* fn) {
    spi_read_seek(region->addr_min);
    for (int32_t addr = region->addr_min; addr <= region->addr_max; ++addr) {
        for (uint8_t bit = 0; bit < DATA_BIT_MAX; ++bit) {
            fn(addr, bit, spi_read_same);
        }
        fn(addr, DATA_BIT_MAX, addr != region->addr_max ? spi_read_next : spi_read_same);
        if (((uint32_t)addr & DISPLAY_REFRESH_ADDRESS_MASK) == 0) {
            diag_refresh();
        }
    }
}

/** Visit every bit downwards without queuing a read below the tested range. */
static void test_each_bit_descending(const mem_region_t* region, march_element_fn* fn) {
    spi_read_seek(region->addr_max);
    for (int32_t addr = region->addr_max; addr >= region->addr_min; --addr) {
        for (uint8_t bit = DATA_BIT_MAX; bit >= 1; --bit) {
            fn(addr, bit, spi_read_same);
        }
        fn(addr, 0, addr != region->addr_min ? spi_read_prev : spi_read_same);
        if (((uint32_t)addr & DISPLAY_REFRESH_ADDRESS_MASK) == 0) {
            diag_refresh();
        }
    }
}

/** Read zero, write one, then verify one with a freshly queued post-write read. */
static void r0w1r1(uint32_t addr, uint8_t bit, last_read_fn* last_read) {
    const uint8_t byte = toggle_bit(addr, spi_read_same(), bit, 0);
    spi_write_same(byte);
    // A write replaces the RX latch. Discard it and queue the verification read.
    spi_read_same();
    check_bit(addr, last_read(), bit, 1);
}

/** Read zero and write one, rearming the pipeline after the write. */
static void r0w1(uint32_t addr, uint8_t bit, last_read_fn* last_read) {
    const uint8_t byte = toggle_bit(addr, spi_read_same(), bit, 0);
    spi_write_same(byte);
    (void)last_read();
}

/** Read one and write zero, rearming the pipeline after the write. */
static void r1w0(uint32_t addr, uint8_t bit, last_read_fn* last_read) {
    const uint8_t byte = toggle_bit(addr, spi_read_same(), bit, 1);
    spi_write_same(byte);
    (void)last_read();
}

/** Initialize the range to zero (the first Extended March C- element). */
static void march_step_w0(const void* context) {
    const mem_region_t* const region = context;
    spi_fill(region->addr_min, 0, (size_t)(region->addr_max - region->addr_min + 1));
}

/** Ascending read-zero/write-one/read-one element. */
static void march_step_asc_r0w1r1(const void* context) { test_each_bit_ascending(context, r0w1r1); }

/** Ascending read-one/write-zero element. */
static void march_step_asc_r1w0(const void* context) { test_each_bit_ascending(context, r1w0); }

/** Descending read-zero/write-one element. */
static void march_step_desc_r0w1(const void* context) { test_each_bit_descending(context, r0w1); }

/** Descending read-one/write-zero element. */
static void march_step_desc_r1w0(const void* context) { test_each_bit_descending(context, r1w0); }

/** Verify the final all-zero state without reading beyond the tested range. */
static void march_step_r0(const void* context) {
    const mem_region_t* const region = context;
    spi_read_seek(region->addr_min);
    for (int32_t addr = region->addr_min; addr <= region->addr_max; ++addr) {
        check_byte(addr, addr != region->addr_max ? spi_read_next() : spi_read_same(), 0);
        if (((uint32_t)addr & DISPLAY_REFRESH_ADDRESS_MASK) == 0) {
            diag_refresh();
        }
    }
}

/** Finish storage traffic before destructive SRAM and BRAM testing. */
static void memory_prepare(void) {
    pet_wait_for_storage_idle();
    set_cpu(CPU_HALT);
}

// Extended March C-, retaining the original six elements and per-bit ordering.
// https://booksite.elsevier.com/9780123705976/errata/13~Chapter%2008%20MBIST.pdf
static const diag_case_t march_c_cases[] = {
    {"Any (w0)", march_step_w0},
    {"Asc (r0,w1,r1)", march_step_asc_r0w1r1},
    {"Asc (r1,w0)", march_step_asc_r1w0},
    {"Desc (r0,w1)", march_step_desc_r0w1},
    {"Desc (r1,w0)", march_step_desc_r1w0},
    {"Any (r0)", march_step_r0},
};
static const mem_region_t sram_region = {SRAM_ADDR_MIN, SRAM_ADDR_MAX};
static const mem_region_t bram_region = {BRAM_ADDR_MIN, BRAM_ADDR_MAX};
static const diag_suite_t memory_suites[] = {
    {"SRAM ($00000-$1FFFF)", march_c_cases, count_of(march_c_cases), &sram_region},
    {"BRAM ($68000-$68FFF)", march_c_cases, count_of(march_c_cases), &bram_region},
};

const diag_plan_t diag_plan_memory = {
    .name = "memory",
    .description = "Extended March C- SRAM and BRAM test",
    .confirm = NULL,
    .prepare = memory_prepare,
    .suites = memory_suites,
    .suite_count = count_of(memory_suites),
};

const diag_plan_t diag_plan_bup = {
    .name = "bup",
    .description = "Bring-up diagnostics for a surface-mount-only board",
    .confirm = NULL,
    .prepare = memory_prepare,
    .suites = memory_suites,
    .suite_count = count_of(memory_suites),
};

void bup_boot_check(void) {
    if (pet_keyboard_probe_boot()) {
        return;
    }

    // Report before starting video, so UART remains useful if display startup fails.
    puts("PIA1 unresponsive: starting destructive bup diagnostics");
    fflush(stdout);
    display_init();
    diag_run_plan(&diag_plan_bup);
}

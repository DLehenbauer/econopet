// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "breakpoint.h"

#include "diag/log/log.h"
#include "driver.h"

#define STP_OPCODE 0xDB

typedef struct bp_entry_s {
    uint16_t addr;      // PET address where breakpoint is set
    uint8_t  original;  // Original instruction byte before patching
    bool     active;    // true if STP has been written to SRAM
} bp_entry_t;

static bp_entry_t bp_table[BP_MAX];
static int bp_entry_count = 0;

// Find the table index for 'addr', or -1 if not found.
static int bp_find(uint16_t addr) {
    for (int i = 0; i < bp_entry_count; i++) {
        if (bp_table[i].addr == addr) {
            return i;
        }
    }
    return -1;
}

void bp_init() {
    bp_entry_count = 0;
    memset(bp_table, 0, sizeof(bp_table));
}

bool bp_set(uint16_t addr) {
    if (bp_find(addr) >= 0) {
        log_warn("bp_set: breakpoint already exists at $%04X", addr);
        return false;
    }

    if (bp_entry_count >= BP_MAX) {
        log_warn("bp_set: table full (%d/%d)", bp_entry_count, BP_MAX);
        return false;
    }

    uint8_t original = spi_read_at(addr);

    bp_entry_t* entry = &bp_table[bp_entry_count++];
    entry->addr     = addr;
    entry->original = original;
    entry->active   = true;

    spi_write_at(addr, STP_OPCODE);
    log_info("bp_set: $%04X (was $%02X)", addr, original);
    return true;
}

bool bp_remove(uint16_t addr) {
    int idx = bp_find(addr);
    if (idx < 0) {
        log_warn("bp_remove: no breakpoint at $%04X", addr);
        return false;
    }

    bp_entry_t* entry = &bp_table[idx];

    // Restore original instruction if the breakpoint is currently armed.
    if (entry->active) {
        spi_write_at(addr, entry->original);
    }

    log_info("bp_remove: $%04X (restored $%02X)", addr, entry->original);

    // Compact the table by moving the last entry into the vacated slot.
    bp_entry_count--;
    if (idx < bp_entry_count) {
        bp_table[idx] = bp_table[bp_entry_count];
    }

    return true;
}

void bp_task() {
    if (system_state.bp_halted) {
        uint16_t pc = bp_resume();
        log_info("Breakpoint hit at $%04X", pc);
    }
}

static bool bp_check() {
    return system_state.bp_halted;
}

uint16_t bp_resume() {
    uint16_t pc = bp_hit_addr();
    int idx = bp_find(pc);

    if (idx < 0) {
        // The halt address does not match any table entry. While unexpected, this can happen
        // if the user program contains a $DB opcode, which is the "illegal" `DCP abs,Y` on the
        // original 6502 (see https://masswerk.at/nowgobang/2021/6502-illegal-opcodes.html#DCP).
        log_warn("bp_resume: halt at $%04X not found in table", pc);
        bp_clear_halt();
        return pc;
    }

    bp_entry_t* entry = &bp_table[idx];

    // Restore the original instruction so the CPU can execute it.
    spi_write_at(pc, entry->original);
    entry->active = false;

    log_info("bp_resume: $%04X (restored $%02X)", pc, entry->original);

    // Clear the FPGA halt so the CPU resumes.
    bp_clear_halt();

    // Re-arm the breakpoint after a short delay so the CPU has time to
    // fetch the restored instruction before we patch it again.
    if (idx >= 0) {
        // Re-arm the breakpoint by writing STP again.  The two dummy reads ensure that
        // CPU has had a chance to advance past the breakpoint address before we patch it
        // again.  (The FPGA bus arbiter provides two turns to SPI for each 1 turn to the CPU)
        spi_read_next();
        spi_read_prev();
        spi_write_at(pc, STP_OPCODE);
        bp_table[idx].active = true;
    }

    return pc;
}

int bp_count() {
    return bp_entry_count;
}

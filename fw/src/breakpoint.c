// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "breakpoint.h"

#include "diag/log/log.h"
#include "driver.h"
#include "fatal.h"

#define STP_OPCODE 0xDB
#define NOP_OPCODE 0xEA
#define JMP_OPCODE 0x4C

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

void bp_set(uint16_t addr, bp_callback_t callback, void* context) {
    vet(bp_find(addr) < 0, "bp_set: breakpoint already exists at $%04X", addr);
    vet(bp_entry_count < BP_MAX, "bp_set: table full (%d/%d)", bp_entry_count, BP_MAX);

    // Read 3 bytes to support JMP redirect
    uint8_t orig0 = spi_read_at(addr);
    uint8_t orig1 = spi_read_at(addr + 1);
    uint8_t orig2 = spi_read_at(addr + 2);

    bp_entry_t* entry = &bp_table[bp_entry_count++];
    entry->addr        = addr;
    entry->original[0] = orig0;
    entry->original[1] = orig1;
    entry->original[2] = orig2;
    entry->active      = true;
    entry->callback    = callback;
    entry->context     = context;

    spi_write_at(addr, STP_OPCODE);
    log_info("bp_set: $%04X (was $%02X, callback=%s)", addr, orig0,
             callback ? "yes" : "no");
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
        spi_write_at(addr, entry->original[0]);
    }

    log_info("bp_remove: $%04X (restored $%02X)", addr, entry->original[0]);

    // Compact the table by moving the last entry into the vacated slot.
    bp_entry_count--;
    if (idx < bp_entry_count) {
        bp_table[idx] = bp_table[bp_entry_count];
    }

    return true;
}

void bp_task() {
    if (!system_state.bp_halted) {
        return;
    }

    const uint16_t pc = bp_hit_addr();
    const int idx = bp_find(pc);

    if (idx < 0) {
        // The halt address does not match any table entry. While unexpected, this can
        // happen if the user program contains a $DB opcode, which is the "illegal"
        // `DCP abs,Y` on the original 6502.
        log_warn("bp_task: halt at $%04X not found in table", pc);
        bp_clear_halt();
        return;
    }

    bp_entry_t* entry = &bp_table[idx];
    bp_callback_t callback = entry->callback;
    void* context = entry->context;

    log_info("Breakpoint hit at $%04X", pc);

    // Determine resume address: callback returns it, or default to pc.
    uint16_t target = callback ? callback(pc, context) : pc;
    const int16_t offset = (int16_t)(target - pc);

    // Build the patch bytes. Initialize to NOPs so the skip cases just set length.
    uint8_t patch[3] = { NOP_OPCODE, NOP_OPCODE, NOP_OPCODE };
    int patch_bytes;

    if (offset == 0) {
        // For a zero offset, we can just restore the original opcode.
        patch[0] = entry->original[0];
        patch_bytes = 1;
    } else if (1 <= offset && offset <= 3) {
        // For forward offsets that are less than or equal to the length of a JMP
        // instruction, we can just write NOPs to skip the bytes.
        patch_bytes = offset;
    } else {
        // For other offsets, we write a JMP instruction to redirect execution.
        patch[0] = JMP_OPCODE;
        patch[1] = (uint8_t)(target & 0xFF);
        patch[2] = (uint8_t)(target >> 8);
        patch_bytes = 3;
    }

    log_info("bp_resume_at: $%04X -> $%04X (was %02X %02X %02X, patch %02X %02X %02X)",
             pc, target,
             entry->original[0], entry->original[1], entry->original[2],
             patch[0],
             patch_bytes > 1
                ? patch[1]
                : entry->original[1],
             patch_bytes > 2
                ? patch[2]
                : entry->original[2]);

    spi_write(pc, patch, patch_bytes);
    entry->active = false;

    // Clear the FPGA halt so the CPU resumes.
    bp_clear_halt();

    // Wait for CPU to advance past the patched bytes, then restore originals
    // and re-arm the breakpoint.
    for (int i = 0; i < patch_bytes; i++) {
        // The FPGA bus arbiter services SPI in a 2:1 ratio to the CPU, so each pair
        // of reads allows ensures the CPU executes at least one instruction.
        spi_read_next();
        spi_read_prev();
    }

    // Restore any bytes we patched
    spi_write(pc, entry->original, patch_bytes);

    // Re-arm the breakpoint
    spi_write_at(pc, STP_OPCODE);
    entry->active = true;
}

int bp_count() {
    return bp_entry_count;
}

const bp_entry_t* bp_get(int index) {
    vet_index(index, bp_entry_count);
    return &bp_table[index];
}

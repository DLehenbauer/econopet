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
    vet(bp_entry_count == 0, "bp_init: breakpoint table must be empty");

    // Require that the CPU is currently in reset or halted to prevent a race with
    // the CPU hitting a breakpoint while we are clearing the breakpoint state.
    const cpu_state_t cpu_state = get_cpu();
    vet((cpu_state & CPU_RESET) || !(cpu_state & CPU_READY), "bp_init: CPU must be in reset or halted, got %u", (unsigned int) cpu_state);

    // Ensure the FPGA is not halted due to a previous breakpoint.  This can
    // happen on startup if the MCU is reset while an FPGA programmer is
    // attached, which causes FPGA configuration to be skipped.
    bp_clear_halt();

    bp_entry_count = 0;
    memset(bp_table, 0, sizeof(bp_table));
}

void bp_set(uint16_t addr, bp_callback_t callback, void* context) {
    vet(callback != NULL, "bp_set: callback is required");
    vet(bp_find(addr) < 0, "bp_set: breakpoint already exists at $%04X", addr);
    vet(bp_entry_count < BP_MAX, "bp_set: table full (%d/%d)", bp_entry_count, BP_MAX);

    uint8_t orig = spi_read_at(addr);

    bp_entry_t* entry = &bp_table[bp_entry_count++];
    entry->addr     = addr;
    entry->original = orig;
    entry->active   = true;
    entry->callback = callback;
    entry->context  = context;

    spi_write_at(addr, STP_OPCODE);
    log_info("bp_set: $%04X (was $%02X)", addr, orig);
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

// bp_set() requires a callback even though this synchronous path never invokes
// bp_task(). Keep a callback that would not rearm if normal dispatch is
// accidentally attempted while the temporary probe is installed.
static bp_result_t bp_run_unused(uint16_t pc, void* context) {
    (void)context;
    return (bp_result_t){.pc = pc, .rearm = false};
}

bp_run_result_t bp_run_to(const uint16_t addr, const uint64_t timeout_us) {
    // Refuse to disturb a running CPU or an existing breakpoint halt. Otherwise
    // the probe could consume another owner's hit or leave an in-flight access
    // unsafely paused.
    const cpu_state_t cpu_state = get_cpu();
    vet((cpu_state & CPU_RESET) || !(cpu_state & CPU_READY),
        "bp_run_to: CPU must be reset or halted, got %u", (unsigned int)cpu_state);
    vet(bp_find(addr) < 0, "bp_run_to: breakpoint already exists at $%04X", addr);
    vet((spi_read_at(ECONOPET_WB_STATUS_ADDR) & ECONOPET_REG_STATUS_BP_HALT_MASK) == 0,
        "bp_run_to: resolve existing CPU halt before running");

    // Install a one-shot temporary breakpoint
    bp_set(addr, bp_run_unused, NULL);
    set_cpu(CPU_READY);

    // Poll the FPGA directly instead of bp_task(): this helper owns completion
    // and must not run callback resume/rearm policy for this or any other hit.
    bp_run_result_t result = {.status = bp_run_timeout, .halt_address = 0};

    // Start timing immediately before polling to exclude the variance from
    // breakpoint setup and setting the CPU control register.
    const uint64_t start = time_us_64();
    do {
        system_state.bp_halted =
            (spi_read_at(ECONOPET_WB_STATUS_ADDR) & ECONOPET_REG_STATUS_BP_HALT_MASK) != 0;

        if (system_state.bp_halted) {
            result.halt_address = bp_hit_addr();
            result.status = result.halt_address == addr
                ? bp_run_reached
                : bp_run_stopped_elsewhere;
            break;
        }
    } while (time_us_64() - start < timeout_us);

    // Restore a halted, clean state on every exit path. Holding the CPU before
    // removing the breakpoint prevents the target instruction from executing
    // during cleanup.
    set_cpu(CPU_HALT);
    bp_remove(addr);
    bp_clear_halt();

    return result;
}

void bp_task() {
    if (!system_state.bp_halted) {
        return;
    }

    const uint16_t pc = bp_hit_addr();

    // Capture the fields we need before invoking the callback, since it may add
    // or remove breakpoints (which compacts the table and invalidates pointers).
    bp_callback_t callback;
    void* context;
    uint8_t original[3];
    {
        const int idx = bp_find(pc);

        if (idx < 0) {
            // The halt address does not match any table entry. While unexpected, this can
            // happen if the user program contains a $DB opcode, which is the "illegal"
            // `DCP abs,Y` on the original 6502.
            log_warn("bp_task: halt at $%04X not found in table", pc);
            bp_clear_halt();
            return;
        }
        const bp_entry_t* const bp = &bp_table[idx];
        callback = bp->callback;
        context = bp->context;
        original[0] = bp->original;
    }

    // Build a local copy of the 3 original bytes at pc. The first byte was saved
    // in the table entry (replaced by STP). The next two are still in SRAM.
    spi_read(pc + 1, 2, &original[1]);

    log_info("Breakpoint hit at $%04X", pc);

    // Determine resume address and rearm policy from the callback.
    const bp_result_t result = callback(pc, context);
    const uint16_t target = result.pc;
    const int16_t offset = (int16_t)(target - pc);

    // Build the patch bytes. Initialize to NOPs so the skip cases just set length.
    uint8_t patch[3] = { NOP_OPCODE, NOP_OPCODE, NOP_OPCODE };
    int patch_bytes;

    if (offset == 0) {
        // For a zero offset, we can just restore the original opcode.
        patch[0] = original[0];
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
             original[0], original[1], original[2],
             patch[0],
             patch_bytes > 1 ? patch[1] : original[1],
             patch_bytes > 2 ? patch[2] : original[2]);

    spi_write(pc, patch, patch_bytes);

    // Mark inactive via a fresh lookup (the index may have shifted).
    const int cur_idx = bp_find(pc);
    if (cur_idx >= 0) {
        bp_table[cur_idx].active = false;
    }

    // Clear the FPGA halt so the CPU resumes.
    bp_clear_halt();

    // Wait for CPU to advance past the patched bytes, then restore originals
    // and re-arm the breakpoint.
    for (int i = 0; i < patch_bytes; i++) {
        // The FPGA bus arbiter services SPI in a 2:1 ratio to the CPU, so each pair
        // of reads ensures the CPU executes at least one instruction.
        spi_read_next();
        spi_read_prev();
    }

    // Restore any bytes we patched
    spi_write(pc, original, patch_bytes);

    if (result.rearm) {
        // Re-arm the breakpoint (re-lookup since table may have shifted).
        const int rearm_idx = bp_find(pc);
        if (rearm_idx >= 0) {
            spi_write_at(pc, STP_OPCODE);
            bp_table[rearm_idx].active = true;
        }
    } else {
        // One-shot: remove the breakpoint from the table (bytes already restored).
        bp_remove(pc);
    }
}

int bp_count() {
    return bp_entry_count;
}

const bp_entry_t* bp_get(int index) {
    vet_index(index, bp_entry_count);
    return &bp_table[index];
}

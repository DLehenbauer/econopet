// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Maximum number of simultaneous breakpoints. This is a firmware table limit
// (not a hardware limit). Every breakpoint is a patched byte in SRAM, so the
// only hard limit is the number of addressable locations.
#define BP_MAX 64

// Result returned by a breakpoint callback.
typedef struct {
    uint16_t pc;     // Address where execution should resume
    bool rearm;      // true = re-arm breakpoint, false = remove after resume
} bp_result_t;

// Callback invoked when a breakpoint is hit. The callback receives the address
// where the breakpoint fired and a user-provided context pointer. Returns a
// bp_result_t specifying where to resume and whether to re-arm:
//   - addr == pc: normal resume (restore original opcode)
//   - addr == pc+1..pc+3: skip bytes (write NOPs)
//   - addr == other: redirect (write JMP)
//   - rearm == true: re-arm the breakpoint after resuming
//   - rearm == false: remove the breakpoint (one-shot)
typedef bp_result_t (*bp_callback_t)(uint16_t pc, void* context);

// A single breakpoint table entry.
typedef struct bp_entry_s {
    uint16_t      addr;        // PET address where breakpoint is set
    uint8_t       original;    // Original byte at addr (replaced by STP)
    bool          active;      // true if STP has been written to SRAM
    bp_callback_t callback;    // Callback invoked when breakpoint fires
    void*         context;     // User-provided context passed to callback
} bp_entry_t;

// Initialize the breakpoint subsystem (clears the table).
void bp_init();

// Set a breakpoint at 'addr'. Reads the original byte from SRAM, saves it
// in the table, and writes STP ($DB) to 'addr'. When the breakpoint fires,
// the callback determines where to resume and whether to re-arm.
void bp_set(uint16_t addr, bp_callback_t callback, void* context);

// Remove the breakpoint at 'addr'. Restores the original instruction byte and
// removes the entry from the table. Returns true on success, false if no
// breakpoint exists at 'addr'.
bool bp_remove(uint16_t addr);

// Check for breakpoint hits and handle them. Call this periodically from the
// main loop.
void bp_task();

// Return the number of active breakpoints.
int bp_count();

// Return a pointer to the breakpoint entry at the given index (0-based).
// The index must be in the range [0, bp_count()).
const bp_entry_t* bp_get(int index);

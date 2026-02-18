// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Maximum number of simultaneous breakpoints. This is a firmware table limit
// (not a hardware limit). Every breakpoint is a patched byte in SRAM, so the
// only hard limit is the number of addressable locations.
#define BP_MAX 64

// Callback invoked when a breakpoint is hit. The callback receives the address
// where the breakpoint fired and a user-provided context pointer. Returns the
// address where execution should resume:
//   - Return pc: normal resume (restore original opcode, clear halt, re-arm)
//   - Return pc+1..pc+3: skip bytes (write NOPs, clear halt, restore, re-arm)
//   - Return other address: redirect (write JMP, clear halt, restore, re-arm)
typedef uint16_t (*bp_callback_t)(uint16_t addr, void* context);

// Initialize the breakpoint subsystem (clears the table).
void bp_init();

// Set a breakpoint at 'addr' with an optional callback and context. Reads the
// original bytes from SRAM, saves them in the table, and writes STP ($DB) to
// 'addr'. When the breakpoint fires, the callback determines where to resume.
// If 'callback' is NULL, the breakpoint logs and resumes at 'addr'.
// Returns true on success, false if the table is full or a breakpoint already
// exists at 'addr'.
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

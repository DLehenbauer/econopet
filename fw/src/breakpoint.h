// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Maximum number of simultaneous breakpoints. This is a firmware table limit
// (not a hardware limit). Every breakpoint is a patched byte in SRAM, so the
// only hard limit is the number of addressable locations.
#define BP_MAX 64

// Initialize the breakpoint subsystem (clears the table).
void bp_init();

// Set a breakpoint at 'addr'. Reads the original instruction byte from SRAM,
// saves it in the table, and writes STP ($DB) to 'addr'. Returns true on
// success, false if the table is full or a breakpoint already exists at 'addr'.
bool bp_set(uint16_t addr);

// Remove the breakpoint at 'addr'. Restores the original instruction byte and
// removes the entry from the table. Returns true on success, false if no
// breakpoint exists at 'addr'.
bool bp_remove(uint16_t addr);

// Check for breakpoint hits and handle them. Call this periodically from the
// main loop.
void bp_task();

// Handle a breakpoint hit: restore the original instruction, clear the FPGA
// halt, and return the breakpoint PC. Returns the address of the breakpoint
// that was hit. The breakpoint remains in the table and is re-armed after a
// short delay.
uint16_t bp_resume();

// Return the number of active breakpoints.
int bp_count();

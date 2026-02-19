// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Configuration blob for the virtual tape drive. All ROM-specific addresses
// are encoded in a packed struct that maps directly to a hex string in
// config.yaml. See docs/dev/tape-emulation.md for full details.
typedef struct __attribute__((packed)) {
    // KERNAL post-load fixup address (2 bytes, little-endian).
    // LD210: prints "READY.", sets VARTAB from EAL/EAH, relinks, and
    // enters the BASIC main loop.
    uint16_t ld210;

    // Breakpoint address (2 bytes, little-endian).
    // Set at JSR LD15 (ROM 2/4) or LDA FA (ROM 1), just before the
    // KERNAL dispatches to the device-specific LOAD path. At this
    // point FNLEN, FNADR, and FA are set up in zero page and the
    // stack has no stale KERNAL frames.
    uint16_t bp_addr;

    // Zero page locations (5 bytes).
    // These vary by ROM version. The MCU reads/writes them to access
    // the filename and end-of-load pointer in PET memory.
    uint8_t eal;        // ZP addr of EAL (end address low)
    uint8_t eah;        // ZP addr of EAH (end address high)
    uint8_t fnlen;      // ZP addr of FNLEN (filename length)
    uint8_t devnum;     // ZP addr of DEVNUM (device number)
    uint8_t fnadr;      // ZP addr of FNADR (filename pointer)
} tape_config_t;        // 9 bytes total

#define TAPE_CONFIG_SIZE sizeof(tape_config_t)

// Tape buffer address in PET memory (same across all ROM versions).
#define TAPE_BUFFER 0x027A

// Initialize the virtual tape drive with the config blob parsed from
// config.yaml. If cfg is NULL, the virtual tape is disabled and all
// LOADs fall through to the physical datasette.
void tape_init(const tape_config_t* cfg);

// Remove the tape breakpoint and disable the virtual tape drive.
void tape_deinit(void);

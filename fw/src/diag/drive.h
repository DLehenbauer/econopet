// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "diag/diag.h"

/**
 * Resolved coordinates and optional parameters for one generated check in diag_plan_drive.
 *
 * Reconstructed on demand from (suite_index, case_index) using the shared
 * compact multiplier tables in drive_test_catalog.h.
 */
typedef struct {
    /** Phase enum ordinal (dt_phase_t) identifying the check type. */
    uint8_t phase;
    /** Zero-based virtual drive slot index (0..7). */
    uint8_t slot;
    /** IEEE-488 device number (8..11) for slot. */
    uint8_t device;
    /** Drive number (0 or 1) within device. */
    uint8_t drive;
    /** Disk geometry ID (DT_GEOM_D64, DT_GEOM_D80, or DT_GEOM_D82). */
    uint8_t geom;
    /** True when length holds a byte-length parameter for a boundary check. */
    bool has_length;
    /** Payload byte length for ordinary-file sector-boundary checks. */
    uint16_t length;
    /** 1-based physical track for zone-boundary checks, or 0 when unused. */
    uint8_t track;
    /** 0-based sector on track for zone-boundary checks. */
    uint8_t sector;
} diag_drive_case_t;

/** Automated IEEE-488 virtual drive qualification plan (468 checks across 8 slots). */
extern const diag_plan_t diag_plan_drive;

/**
 * Decode a 0-based (suite_index, case_index) pair into *out using the shared
 * compact drive qualification catalog.
 */
void diag_drive_resolve_case(size_t suite_index, size_t case_index, diag_drive_case_t* out);

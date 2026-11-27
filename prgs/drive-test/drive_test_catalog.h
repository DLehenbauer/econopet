/* SPDX-License-Identifier: CC0-1.0 */
/* https://github.com/dlehenbauer/econopet */

#pragma once

/*
 * Single source of truth for the IEEE-488 virtual drive qualification suite,
 * shared between the 6502 PET target program (CC65) and the RP2040 diagnostic
 * plan (ARM/host GCC).
 *
 * Under __CC65__, only terse PET screen strings and compact multiplier tables
 * are compiled into 6502 RODATA. On the MCU, the same X-macro tables expand
 * human-readable HDMI/UART descriptions and suite case names for all 468 checks
 * without storing an expanded 468-entry table on either side.
 */

#ifndef count_of
/** Return the element count of a fixed-size array on targets without the Pico SDK. */
#define count_of(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define DT_DEVICE_FIRST 8u
#define DT_DEVICE_LAST 11u
#define DT_DRIVES_PER_DEVICE 2u
#define DT_SLOT_COUNT 8u
#define DT_UNIT_COUNT 4u
#define DT_REP_SLOT_COUNT 3u
#define DT_UNIT_ISO_COUNT 4u
#define DT_DRIVE_ISO_PASSES 4u
#define DT_BOUNDARY_LEN_COUNT 8u
#define DT_BOUNDARY_TYPE_COUNT 3u
#define DT_DUP_UNIT_COUNT 3u
#define DT_DUP_STEPS_PER_DIR 4u

#define DT_DOS4040_TRACKS 35u
#define DT_DOS4040_FREE_BLOCKS 664u
#define DT_DOS8050_BAM_PARTS 2u
#define DT_DOS8250_BAM_PARTS 4u
#define DT_DOS8050_TRACK_LIMIT 78u
#define DT_DOS8250_TRACK_LIMIT 155u
#define DT_DOS8050_FREE_BLOCKS 2052u
#define DT_DOS8250_FREE_BLOCKS 4133u

/** Disk container geometry IDs shared by target and MCU drive tables. */
enum {
    /** 35-track single-sided Commodore 4040 D64 image. */
    DT_GEOM_D64 = 0,
    /** 77-track single-sided Commodore 8050 D80 image. */
    DT_GEOM_D80 = 1,
    /** 154-track double-sided Commodore 8250 D82 image. */
    DT_GEOM_D82 = 2,
};

/** Phase identifiers expanded from drive_test_items.inc for each check category. */
typedef enum {
#define DT_PHASE_ITEM(id, terse, full) id,
#include "drive_test_items.inc"
#undef DT_PHASE_ITEM
    DT_PHASE_COUNT
} dt_phase_t;

/** Failure detail identifiers expanded from drive_test_items.inc for mismatch reporting. */
typedef enum {
#define DT_DETAIL_ITEM(id, terse, full) id,
#include "drive_test_items.inc"
#undef DT_DETAIL_ITEM
    DT_ERR_COUNT
} dt_err_t;

/** Static identity and naming attributes for one virtual drive slot (U8..U11, D0..D1). */
typedef struct {
    /** IEEE-488 primary device address (8..11). */
    unsigned char device;
    /** Drive unit number within device (0 or 1). */
    unsigned char drive;
    /** Disk geometry selector (DT_GEOM_D64, DT_GEOM_D80, or DT_GEOM_D82). */
    unsigned char geom;
    /** Slot-unique seed byte preventing cross-slot or cross-drive aliasing on readback. */
    unsigned char signature;
    /** Base filename and formatted disk label (for example "U8D0"). */
    char name[6];
    /** Filename used for 512-byte multi-sector sequential stream checks (for example "L8D0"). */
    char long_name[6];
    /** Two-character DOS format disk ID (for example "80"). */
    char disk_id[3];
} drive_slot_t;

/** Per-geometry DOS layout constants and speed-zone boundary table. */
typedef struct {
    /** Expected free block count reported in the empty directory footer after format. */
    unsigned int free_blocks;
    /** Number of BAM sectors on track 38 (8050/8250) or 1 for 4040's single BAM on track 18. */
    unsigned char bam_parts;
    /** Exclusive upper track bound (last valid 1-based track + 1). */
    unsigned char track_limit;
    /** Non-zero when DOS 2.7 appends ',<drive>' to status strings and uses format '2C'. */
    unsigned char d80_status;
    /** Pointer to {track, last_sector} pairs marking speed-zone and side boundaries. */
    const unsigned char (*zones)[2];
    /** Number of {track, last_sector} entries in zones. */
    unsigned char zone_count;
} drive_geom_t;

/** Last sector of each speed zone boundary on 35-track 4040 D64 media (skipping BAM track 18). */
static const unsigned char dt_d64_zones[][2] = {
    {17, 20}, {19, 18}, {24, 18}, {25, 17}, {30, 17}, {31, 16}, {35, 16},
};

/** Last sector of each speed zone boundary on 77-track 8050 D80 media (skipping BAM track 38). */
static const unsigned char dt_d80_zones[][2] = {
    {39, 28}, {40, 26}, {53, 26}, {54, 24}, {64, 24}, {65, 22}, {77, 22},
};

/** Last sector of each speed zone boundary across both sides of 154-track 8250 D82 media. */
static const unsigned char dt_d82_zones[][2] = {
    {39, 28},  {40, 26},  {53, 26},  {54, 24},  {64, 24},  {65, 22},  {77, 22},  {78, 28},
    {116, 28}, {117, 26}, {130, 26}, {131, 24}, {141, 24}, {142, 22}, {154, 22},
};

#define DT_D64_ZONE_COUNT count_of(dt_d64_zones)
#define DT_D80_ZONE_COUNT count_of(dt_d80_zones)
#define DT_D82_ZONE_COUNT count_of(dt_d82_zones)

/** Geometry descriptors indexed by DT_GEOM_D64, DT_GEOM_D80, and DT_GEOM_D82. */
static const drive_geom_t dt_geoms[] = {
    {DT_DOS4040_FREE_BLOCKS, 1u, DT_DOS4040_TRACKS + 1u, 0u, dt_d64_zones, DT_D64_ZONE_COUNT},
    {DT_DOS8050_FREE_BLOCKS, DT_DOS8050_BAM_PARTS, DT_DOS8050_TRACK_LIMIT, 1u, dt_d80_zones, DT_D80_ZONE_COUNT},
    {DT_DOS8250_FREE_BLOCKS, DT_DOS8250_BAM_PARTS, DT_DOS8250_TRACK_LIMIT, 1u, dt_d82_zones, DT_D82_ZONE_COUNT},
};

/** Eight virtual drive slots spanning devices 8..11, drives 0..1. */
static const drive_slot_t dt_slots[DT_SLOT_COUNT] = {
    {8, 0, DT_GEOM_D64, 0x18, "U8D0", "L8D0", "80"},    {8, 1, DT_GEOM_D64, 0x19, "U8D1", "L8D1", "81"},
    {9, 0, DT_GEOM_D80, 0x29, "U9D0", "L9D0", "90"},    {9, 1, DT_GEOM_D80, 0x28, "U9D1", "L9D1", "91"},
    {10, 0, DT_GEOM_D82, 0x3a, "U10D0", "L10D0", "A0"}, {10, 1, DT_GEOM_D82, 0x3b, "U10D1", "L10D1", "A1"},
    {11, 0, DT_GEOM_D64, 0x4b, "U11D0", "L11D0", "B0"}, {11, 1, DT_GEOM_D64, 0x4a, "U11D1", "L11D1", "B1"},
};

/** Representative drive-0 slot indices covering D64 (slot 0), D80 (slot 2), and D82 (slot 4). */
static const unsigned char dt_rep_slots[DT_REP_SLOT_COUNT] = {0u, 2u, 4u};
/** Alternating slot sequence exercising cross-unit isolation between U8/D0 and U11/D0. */
static const unsigned char dt_unit_iso_slots[DT_UNIT_ISO_COUNT] = {0u, 6u, 0u, 6u};
/** Ordinary-file byte lengths straddling 1-sector (254B), 2-sector (508B), and 0/1-byte EOF edges. */
static const unsigned int dt_boundary_lengths[DT_BOUNDARY_LEN_COUNT] = {253u, 254u, 255u, 256u, 508u, 509u, 1u, 0u};

/* Suite case counts derived directly from the unexpanded domain multipliers. */
#define DT_SMOKE_STEPS_PER_SLOT 17u
#define DT_CONCAT_STEPS_PER_UNIT 11u
#define DT_REL_STEPS_PER_REP 19u
#define DT_MAINT_FIXED_STEPS 35u

#define DT_SUITE_FORMAT_COUNT      DT_SLOT_COUNT
#define DT_SUITE_SMOKE_COUNT       (DT_SLOT_COUNT * DT_SMOKE_STEPS_PER_SLOT)
#define DT_SUITE_ISOLATION_COUNT   (DT_UNIT_ISO_COUNT + DT_UNIT_COUNT * DT_DRIVE_ISO_PASSES)
#define DT_SUITE_CONCAT_COUNT      (DT_UNIT_COUNT * DT_CONCAT_STEPS_PER_UNIT)
#define DT_SUITE_MULTI_SEQ_COUNT   DT_SLOT_COUNT
#define DT_SUITE_REL_COUNT         (DT_REP_SLOT_COUNT * DT_REL_STEPS_PER_REP)
#define DT_SUITE_RAW_BLOCK_COUNT   DT_SLOT_COUNT
#define DT_SUITE_MAINT_D64_COUNT   (DT_MAINT_FIXED_STEPS + 2u * DT_D64_ZONE_COUNT)
#define DT_SUITE_MAINT_D80_COUNT   (DT_MAINT_FIXED_STEPS + 2u * DT_D80_ZONE_COUNT)
#define DT_SUITE_MAINT_D82_COUNT   (DT_MAINT_FIXED_STEPS + 2u * DT_D82_ZONE_COUNT)
#define DT_SUITE_MAINT_COUNT       (DT_SUITE_MAINT_D64_COUNT + DT_SUITE_MAINT_D80_COUNT + DT_SUITE_MAINT_D82_COUNT)
#define DT_SUITE_DUPLICATE_COUNT   (DT_DUP_UNIT_COUNT * DT_DRIVES_PER_DEVICE * DT_DUP_STEPS_PER_DIR)

#define DT_TOTAL_CASES                                                                             \
    (DT_SUITE_FORMAT_COUNT + DT_SUITE_SMOKE_COUNT + DT_SUITE_ISOLATION_COUNT +                     \
     DT_SUITE_CONCAT_COUNT + DT_SUITE_MULTI_SEQ_COUNT + DT_SUITE_REL_COUNT +                       \
     DT_SUITE_RAW_BLOCK_COUNT + DT_SUITE_MAINT_COUNT + DT_SUITE_DUPLICATE_COUNT)

#ifdef __CC65__
/** Terse 40-column PET screen phase mnemonics indexed by dt_phase_t. */
static const char* const dt_phase_pet_names[DT_PHASE_COUNT] = {
#define DT_PHASE_ITEM(id, terse, full) terse,
#include "drive_test_items.inc"
#undef DT_PHASE_ITEM
};

/** Terse 40-column PET screen failure category labels indexed by dt_err_t. */
static const char* const dt_detail_pet_names[DT_ERR_COUNT] = {
#define DT_DETAIL_ITEM(id, terse, full) terse,
#include "drive_test_items.inc"
#undef DT_DETAIL_ITEM
};
#else
/** Human-readable HDMI/UART phase labels indexed by dt_phase_t. */
static const char* const dt_phase_hdmi_names[DT_PHASE_COUNT] = {
#define DT_PHASE_ITEM(id, terse, full) full,
#include "drive_test_items.inc"
#undef DT_PHASE_ITEM
};

/** Human-readable HDMI/UART failure detail labels indexed by dt_err_t. */
static const char* const dt_detail_hdmi_names[DT_ERR_COUNT] = {
#define DT_DETAIL_ITEM(id, terse, full) full,
#include "drive_test_items.inc"
#undef DT_DETAIL_ITEM
};

/** Ordered sequence of 17 smoke phases run per slot in DRIVE_SUITE_SMOKE. */
static const unsigned char dt_smoke_phases[DT_SMOKE_STEPS_PER_SLOT] = {
    DT_PHASE_EMPTY_DIR,    DT_PHASE_FORMAT_BAM,  DT_PHASE_SMOKE_FILE,   DT_PHASE_POP_DIR,
    DT_PHASE_FILE_BAM,     DT_PHASE_FIRST_SECTOR, DT_PHASE_RENAME,      DT_PHASE_COPY,
    DT_PHASE_RENAME_COLL,  DT_PHASE_COPY_COLL,   DT_PHASE_SCRATCH,      DT_PHASE_MISSING_FILE,
    DT_PHASE_UNKNOWN_CMD,  DT_PHASE_DIRECT_BLOCK, DT_PHASE_REL_SMOKE,   DT_PHASE_PRG_STREAM,
    DT_PHASE_USR_STREAM,
};

/** Ordered sequence of 11 cross-drive copy and concatenation phases per dual-drive unit. */
static const unsigned char dt_concat_phases[DT_CONCAT_STEPS_PER_UNIT] = {
    DT_PHASE_CROSS_COPY,      DT_PHASE_CROSS_COPY,     DT_PHASE_CROSS_CONCAT,   DT_PHASE_CONCAT_APPEND,
    DT_PHASE_SEQ_PRG_CONCAT,  DT_PHASE_PRG_SEQ_CONCAT, DT_PHASE_CONCAT_MISSING, DT_PHASE_CONCAT_EXISTS,
    DT_PHASE_PRG_SELF_APPEND, DT_PHASE_CONCAT_SOURCE,  DT_PHASE_CONCAT_PEER,
};

/** Ordered sequence of 19 relative-file phases run on each representative geometry slot. */
static const unsigned char dt_rel_phases[DT_REL_STEPS_PER_REP] = {
    DT_PHASE_REL_2B,         DT_PHASE_REL_129B,      DT_PHASE_REL_254B,      DT_PHASE_REL_LAST_OFF,
    DT_PHASE_REL_ZERO_ALIAS, DT_PHASE_REL_MISSING,   DT_PHASE_REL_HIGH_BYTE, DT_PHASE_REL_POS_OVF,
    DT_PHASE_REL_255_OVF,    DT_PHASE_REL_GROW_SETUP, DT_PHASE_REL_GROW_MISS, DT_PHASE_REL_OFF_GROW,
    DT_PHASE_REL_SEC_CROSS,  DT_PHASE_REL_NEXT_MISS, DT_PHASE_REL_SIDE_OPEN, DT_PHASE_REL_SIDE_MISS,
    DT_PHASE_REL_SIDE_GROW,  DT_PHASE_REL_SIDE_RET,  DT_PHASE_REL_SIDE_NEXT,
};

/** Fixed maintenance command phases preceding zone sweeps in DRIVE_SUITE_MAINT. */
static const unsigned char dt_maint_phases[9] = {
    DT_PHASE_SCRATCH_Q,     DT_PHASE_SCRATCH_STAR, DT_PHASE_BLOCK_ALLOC,
    DT_PHASE_INIT_VALIDATE, DT_PHASE_VALIDATE_FILE, DT_PHASE_QUICK_FORMAT,
    DT_PHASE_EMPTY_DIR,     DT_PHASE_FORMAT_BAM,   DT_PHASE_QUICK_RET,
};
#endif

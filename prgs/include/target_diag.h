// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

/*
 * Shared target-CPU diagnostic mailbox at the top of base SRAM ($7FF0-$7FFF).
 * This is outside the PET ROM's low-memory workspaces (including BASIC 4's
 * DOS command buffer at $0353-$03F9). Target programs must reserve this region
 * in their linker layout, including their software stack. The host bounds PRG
 * loading below it and lowers BASIC MEMSIZ/FRETOP before RUN. Standalone targets
 * must reserve the region themselves before allocating BASIC strings.
 * Explicit offsets keep 6502, 6809, and ARM layouts identical.
 */

/** Base address of the reserved 16-byte target diagnostic mailbox ($7FF0..$7FFF). */
#define TARGET_DIAG_MAILBOX_ADDR      0x7ff0u
/** First mailbox magic byte ('T'), written once the target program initializes the mailbox. */
#define TARGET_DIAG_MAGIC_0           0x54u
/** Second mailbox magic byte ('D'), written once the target program initializes the mailbox. */
#define TARGET_DIAG_MAGIC_1           0x44u

/** Target mailbox state before the target program starts a pass. */
#define TARGET_DIAG_STATE_IDLE        0u
/** Target mailbox state while a diagnostic pass is actively executing checks. */
#define TARGET_DIAG_STATE_RUNNING     1u
/** Target mailbox state after all checks in the current pass succeed. */
#define TARGET_DIAG_STATE_PASSED      2u
/** Target mailbox state after the target halts on the first failing check or timeout. */
#define TARGET_DIAG_STATE_FAILED      3u

/** Host control value (0) indicating standalone execution (for example in VICE) that exits after one pass. */
#define TARGET_DIAG_HOST_STANDALONE   0u
/** Host control value (1) indicating an attached MCU runner that may re-trigger multiple passes. */
#define TARGET_DIAG_HOST_ATTACHED     1u

/** Sentinel phase byte when no check has completed or failed in the active cycle. */
#define TARGET_DIAG_PHASE_NONE        0xffu
/** Sentinel slot byte when the active check is not associated with a specific slot. */
#define TARGET_DIAG_SLOT_NONE         0xffu

/** Byte offset of magic0 ('T'). */
#define TARGET_DIAG_OFF_MAGIC0        0u
/** Byte offset of magic1 ('D'). */
#define TARGET_DIAG_OFF_MAGIC1        1u
/** Byte offset of the host attachment mode (TARGET_DIAG_HOST_*). */
#define TARGET_DIAG_OFF_HOST_CONTROL  2u
/** Byte offset of the execution state (TARGET_DIAG_STATE_*). */
#define TARGET_DIAG_OFF_STATE         3u
/** Byte offset of the completed-check count low byte. */
#define TARGET_DIAG_OFF_COMPLETED_LO  4u
/** Byte offset of the completed-check count high byte. */
#define TARGET_DIAG_OFF_COMPLETED_HI  5u
/** Byte offset of the plan-specific failure detail enum value. */
#define TARGET_DIAG_OFF_DETAIL_ID     6u
/** Byte offset of the expected byte value on failure. */
#define TARGET_DIAG_OFF_EXPECTED      7u
/** Byte offset of the actual byte value (or timeout stage) on failure. */
#define TARGET_DIAG_OFF_ACTUAL        8u
/** Byte offset of the primary auxiliary coordinate (such as track or timeout stage). */
#define TARGET_DIAG_OFF_AUX0          9u
/** Byte offset of the secondary auxiliary coordinate (such as sector or VIA port B state). */
#define TARGET_DIAG_OFF_AUX1         10u
/** Byte offset of the failing byte index low byte. */
#define TARGET_DIAG_OFF_OFFSET_LO    11u
/** Byte offset of the failing byte index high byte. */
#define TARGET_DIAG_OFF_OFFSET_HI    12u
/** Byte offset of the latest check's phase enum ordinal. */
#define TARGET_DIAG_OFF_PHASE        13u
/** Byte offset of the latest check's zero-based slot index. */
#define TARGET_DIAG_OFF_SLOT         14u
/** Byte offset of the odd/even seqlock counter protecting multi-byte mailbox updates. */
#define TARGET_DIAG_OFF_SEQ          15u
/** Total byte size of the target diagnostic mailbox in SRAM. */
#define TARGET_DIAG_MAILBOX_SIZE     16u

/** Compute the physical SRAM address of a TARGET_DIAG_OFF_* mailbox field. */
#define TARGET_DIAG_ADDR(offset)      (TARGET_DIAG_MAILBOX_ADDR + (offset))

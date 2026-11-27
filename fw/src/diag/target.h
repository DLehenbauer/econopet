// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "target_diag.h"

/**
 * Tear-free snapshot of the reserved 16-byte target-CPU diagnostic mailbox.
 *
 * The running target CPU updates this region in PET base SRAM using an odd/even
 * seqlock counter in seq so the MCU can poll progress and failure coordinates
 * over SPI without halting the target CPU.
 */
typedef struct {
    /** First magic byte ('T' when initialized by the target program). */
    uint8_t magic0;
    /** Second magic byte ('D' when initialized by the target program). */
    uint8_t magic1;
    /** Host attachment mode written by the MCU (TARGET_DIAG_HOST_*). */
    uint8_t host_control;
    /** Current target execution state (TARGET_DIAG_STATE_*). */
    uint8_t state;
    /** Monotonically increasing count of checks completed in the active cycle (0..total_cases). */
    uint16_t completed;
    /** Plan-specific failure category ID (valid when state == TARGET_DIAG_STATE_FAILED). */
    uint8_t detail_id;
    /** Expected byte value recorded at the first mismatch. */
    uint8_t expected;
    /** Actual byte value (or timeout stage) recorded at the first mismatch. */
    uint8_t actual;
    /** Plan-specific primary coordinate (such as track or timeout stage). */
    uint8_t aux0;
    /** Plan-specific secondary coordinate (such as sector or raw VIA bus byte). */
    uint8_t aux1;
    /** Zero-based byte offset within the failing transfer or status record. */
    uint16_t offset;
    /** Plan-specific phase ordinal for the most recently completed or failed check. */
    uint8_t phase;
    /** Zero-based slot index for the most recently completed or failed check, or 0xFF. */
    uint8_t slot;
    /** Seqlock counter (odd while the target is mutating the mailbox, even when quiescent). */
    uint8_t seq;
} diag_target_snapshot_t;

/** Target polling policy shared across all cases in a target-CPU diagnostic plan. */
typedef struct {
    /** Exact number of checks the target program must report per cycle. */
    uint16_t total_cases;
    /** Per-case inactivity timeout in microseconds, reset whenever completed advances. */
    uint64_t case_timeout_us;
    /** Optional background service callback invoked on every poll iteration (for example ieee_drive_task). */
    void (*service)(void);
    /**
     * Optional consistency check verifying that snapshot metadata matches the
     * 0-based check_ordinal. Writes a human-readable error into out on mismatch.
     */
    bool (*verify_snapshot)(const diag_target_snapshot_t* snapshot, uint16_t check_ordinal, char* out, size_t out_size);
    /** Optional formatter that converts a TARGET_DIAG_STATE_FAILED snapshot into failure_detail text. */
    void (*format_failure)(const diag_target_snapshot_t* snapshot, char* out, size_t out_size);
} diag_target_runner_t;

/**
 * Read a tear-free 16-byte snapshot of the target diagnostic mailbox from SRAM.
 *
 * Retries bounded back-to-back SPI reads until seq is even and two consecutive
 * bursts match, preventing torn multi-byte fields while the target CPU runs.
 * Returns false on retry exhaustion, leaving out unchanged.
 */
bool diag_target_read_mailbox(diag_target_snapshot_t* out);

/**
 * Boot a PET BASIC 2/4 configuration to READY, load a .prg below the reserved
 * mailbox, lower BASIC's memory ceiling, queue "RUN\r", and leave the CPU halted.
 * The target's linker must keep program data and its software stack below the
 * mailbox as well. Requires the ROM to detect the full 32 KiB base RAM.
 *
 * Writes a descriptive message to error_out[error_size] and returns false if
 * the configuration/ROM is unsupported, BASIC fails to boot, RAM is insufficient,
 * or the .prg cannot load without overlapping the mailbox.
 */
bool diag_target_prepare_pet_prg(const char* config_id, const char* prg_path, char* error_out, size_t error_size);

/**
 * Arm and trigger 1-based target diagnostic iteration 'cycle'.
 *
 * Resets the mailbox's per-cycle completion count and state to RUNNING, releasing
 * the halted CPU on cycle 1 and waking the target's spin loop on later cycles.
 */
void diag_target_begin_cycle(uint8_t cycle);

/**
 * Service hardware and poll the target mailbox until 0-based case_ordinal
 * completes, calling diag_fail() on target failure, mailbox drift, or timeout.
 */
void diag_target_await_case(const diag_target_runner_t* runner, size_t case_ordinal);

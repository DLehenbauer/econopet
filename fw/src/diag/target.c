// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "diag/target.h"

#include "breakpoint.h"
#include "diag/diag.h"
#include "driver.h"
#include "menu/menu.h"
#include "tape.h"

/** Entry point of PET BASIC 4.0's warm READY prompt loop. */
#define PET_BASIC4_READY_ADDR 0xb3ffu

/** Maximum time in microseconds allowed for the PET ROM to reach BASIC READY after reset. */
#define PET_BOOT_TIMEOUT_US UINT64_C(2000000)

/** PET KERNAL zero-page keyboard queue length byte (CHARS). */
#define PET_KEY_COUNT_ADDR 0x009eu

/** PET KERNAL 10-byte keyboard input queue (KEYD). */
#define PET_KEY_BUFFER_ADDR 0x026fu

/**
 * Maximum back-to-back SPI read pairs attempted when sampling the mailbox.
 *
 * Allows ~320us of 31.25 MHz SPI bursts to outlast a ~30us 1 MHz 6502 seqlock update.
 */
#define TARGET_SNAPSHOT_RETRY_LIMIT 64u

/** Decode a consistent mailbox snapshot, leaving *out unchanged if acquisition fails. */
bool diag_target_read_mailbox(diag_target_snapshot_t* const out) {
    uint8_t first[TARGET_DIAG_MAILBOX_SIZE];
    uint8_t second[TARGET_DIAG_MAILBOX_SIZE];
    bool matched = false;

    // Re-read the 16-byte region until the seqlock counter is even and two consecutive SPI bursts match.
    spi_read(TARGET_DIAG_MAILBOX_ADDR, count_of(first), first);
    for (unsigned attempt = 0; attempt < TARGET_SNAPSHOT_RETRY_LIMIT; ++attempt) {
        spi_read(TARGET_DIAG_MAILBOX_ADDR, count_of(second), second);
        if ((second[TARGET_DIAG_OFF_SEQ] & 1u) == 0u && memcmp(first, second, sizeof(first)) == 0) {
            matched = true;
            break;
        }
        memcpy(first, second, sizeof(first));
    }
    if (!matched) {
        return false;
    }

    // Decode the byte-oriented wire layout into host integer fields.
    out->magic0 = second[TARGET_DIAG_OFF_MAGIC0];
    out->magic1 = second[TARGET_DIAG_OFF_MAGIC1];
    out->host_control = second[TARGET_DIAG_OFF_HOST_CONTROL];
    out->state = second[TARGET_DIAG_OFF_STATE];
    out->completed = (uint16_t)second[TARGET_DIAG_OFF_COMPLETED_LO] |
                     ((uint16_t)second[TARGET_DIAG_OFF_COMPLETED_HI] << 8);
    out->detail_id = second[TARGET_DIAG_OFF_DETAIL_ID];
    out->expected = second[TARGET_DIAG_OFF_EXPECTED];
    out->actual = second[TARGET_DIAG_OFF_ACTUAL];
    out->aux0 = second[TARGET_DIAG_OFF_AUX0];
    out->aux1 = second[TARGET_DIAG_OFF_AUX1];
    out->offset = (uint16_t)second[TARGET_DIAG_OFF_OFFSET_LO] |
                  ((uint16_t)second[TARGET_DIAG_OFF_OFFSET_HI] << 8);
    out->phase = second[TARGET_DIAG_OFF_PHASE];
    out->slot = second[TARGET_DIAG_OFF_SLOT];
    out->seq = second[TARGET_DIAG_OFF_SEQ];
    return true;
}

/** Stage a PET diagnostic configuration and .prg in SRAM, halted at READY with "RUN\r" queued. */
bool diag_target_prepare_pet_prg(const char* const config_id, const char* const prg_path,
                                 char* const error_out, const size_t error_size) {
    // Load the requested ROM/model configuration without user mounts or guest-mode takeover.
    if (!menu_apply_diag_config(config_id)) {
        snprintf(error_out, error_size, "missing config '%s' in /config.yaml", config_id);
        return false;
    }

    // Run KERNAL and BASIC cold start to READY ($B3FF) so zero page, PIA/VIA, and MEMSIZ are initialized.
    const bp_run_result_t boot = bp_run_to(PET_BASIC4_READY_ADDR, PET_BOOT_TIMEOUT_US);
    if (boot.status != bp_run_reached) {
        snprintf(error_out, error_size, "PET failed to reach BASIC READY (status %d, PC $%04X)",
                 (int)boot.status, (unsigned)boot.halt_address);
        return false;
    }

    // Stream the target .prg directly into SRAM while the CPU remains halted at READY.
    if (!load_prg(prg_path)) {
        snprintf(error_out, error_size, "failed to load '%s'", prg_path);
        return false;
    }

    // Zero the mailbox and mark HOST_ATTACHED so the target spins for multi-cycle re-triggers instead of exiting.
    uint8_t mailbox[TARGET_DIAG_MAILBOX_SIZE] = {0};
    mailbox[TARGET_DIAG_OFF_HOST_CONTROL] = TARGET_DIAG_HOST_ATTACHED;
    mailbox[TARGET_DIAG_OFF_PHASE] = TARGET_DIAG_PHASE_NONE;
    mailbox[TARGET_DIAG_OFF_SLOT] = TARGET_DIAG_SLOT_NONE;
    spi_write(TARGET_DIAG_MAILBOX_ADDR, mailbox, sizeof(mailbox));

    // Inject "RUN\r" into the KERNAL keyboard buffer so releasing the CPU launches the BASIC stub.
    static const uint8_t run_keys[] = {'R', 'U', 'N', '\r'};
    spi_write(PET_KEY_BUFFER_ADDR, run_keys, sizeof(run_keys));
    spi_write_at(PET_KEY_COUNT_ADDR, sizeof(run_keys));
    return true;
}

/** Reset per-cycle mailbox counters and start or wake the target CPU for 1-based iteration 'cycle'. */
void diag_target_begin_cycle(const uint8_t cycle) {
    // Clear the prior cycle's completion count and coordinates before switching state back to RUNNING.
    spi_write_at(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_LO), 0);
    spi_write_at(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_HI), 0);
    spi_write_at(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_PHASE), TARGET_DIAG_PHASE_NONE);
    spi_write_at(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SLOT), TARGET_DIAG_SLOT_NONE);
    spi_write_at(TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE), TARGET_DIAG_STATE_RUNNING);

    // Release the CPU from its post-READY halt on the first cycle (later cycles wake via STATE_RUNNING).
    if (cycle == 1u) {
        set_cpu(CPU_READY);
    }
}

/** Poll the target mailbox while servicing background tasks until case_ordinal finishes or fails. */
void diag_target_await_case(const diag_target_runner_t* const runner, const size_t case_ordinal) {
    // Initialize the per-case inactivity deadline before entering the polling loop.
    uint64_t deadline_us = time_us_64() + runner->case_timeout_us;
    uint16_t last_completed = 0;
    bool initialized = false;

    while (true) {
        // Service emulated peripherals and refresh the HDMI elapsed-time header once per second.
        if (runner->service != NULL) {
            runner->service();
        }
        diag_refresh();

        // Inspect only a consistent snapshot initialized by the target program.
        diag_target_snapshot_t snap;
        const bool acquired = diag_target_read_mailbox(&snap);
        if (acquired &&
            snap.magic0 == TARGET_DIAG_MAGIC_0 && snap.magic1 == TARGET_DIAG_MAGIC_1) {
            // Extend the inactivity deadline whenever the target initializes or advances completed checks.
            if (!initialized || snap.completed != last_completed) {
                initialized = true;
                last_completed = snap.completed;
                deadline_us = time_us_64() + runner->case_timeout_us;
            }

            // Guard against target check counts overflowing or marking PASSED prematurely.
            if (snap.completed > runner->total_cases ||
                (snap.state == TARGET_DIAG_STATE_PASSED && snap.completed != runner->total_cases)) {
                diag_fail("target check count mismatch (expected %u, got %u)",
                          (unsigned)runner->total_cases, (unsigned)snap.completed);
            }

            // Cross-check the mailbox's latest (phase, slot) coordinates against the host catalog.
            if (runner->verify_snapshot != NULL &&
                (snap.completed > 0u || snap.state == TARGET_DIAG_STATE_FAILED)) {
                const uint16_t check_ordinal = snap.state == TARGET_DIAG_STATE_FAILED
                                                   ? snap.completed
                                                   : (uint16_t)(snap.completed - 1u);
                char drift[DIAG_MESSAGE_MAX];
                if (!runner->verify_snapshot(&snap, check_ordinal, drift, sizeof(drift))) {
                    diag_fail("%s", drift);
                }
            }

            // Return as soon as completed passes case_ordinal (waiting for STATE_PASSED on the final case).
            const bool is_final_case = case_ordinal + 1u >= runner->total_cases;
            if ((!is_final_case && snap.completed > case_ordinal) ||
                (snap.state == TARGET_DIAG_STATE_PASSED && snap.completed == runner->total_cases)) {
                return;
            }

            // Format and report target-detected mismatches immediately when STATE_FAILED is published.
            if (snap.state == TARGET_DIAG_STATE_FAILED) {
                char detail[DIAG_MESSAGE_MAX];
                if (runner->format_failure != NULL) {
                    runner->format_failure(&snap, detail, sizeof(detail));
                } else {
                    snprintf(detail, sizeof(detail),
                             "detail %u at +%u: expected $%02X, got $%02X",
                             (unsigned)snap.detail_id, (unsigned)snap.offset,
                             (unsigned)snap.expected, (unsigned)snap.actual);
                }
                diag_fail("%s", detail);
            }
        }

        // Abort the run if the target stops making progress past case_timeout_us.
        if (time_us_64() >= deadline_us) {
            if (!acquired) {
                diag_fail("target timeout waiting for case %zu (no consistent mailbox snapshot)", case_ordinal + 1u);
            }
            diag_fail("target timeout waiting for case %zu (state %u, completed %u)",
                      case_ordinal + 1u, (unsigned)snap.state, (unsigned)snap.completed);
        }
    }
}

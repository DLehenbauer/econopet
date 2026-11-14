// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "cpu.h"

#include "breakpoint.h"
#include "diag/log/log.h"
#include "driver.h"
#include "fatal.h"

#define PROBE_START 0x0400u
#define PROBE_ROUTINE (PROBE_START + 5u)
#define PROBE_LOOP (PROBE_ROUTINE + 5u)
#define PROBE_RESULT 0x0410u
#define PROBE_MARKER 0xA5u
#define PROBE_SENTINEL 0x00u
#define RESET_VECTOR 0xFFFCu
#define RESET_PULSE_US 4u
#define PROBE_TIMEOUT_US 5000u

#define OPCODE_SEI 0x78u
#define OPCODE_CLD 0xD8u
#define OPCODE_JMP 0x4Cu
#define OPCODE_LDA_IMMEDIATE 0xA9u
#define OPCODE_STA_ABSOLUTE 0x8Du

static bool initialized;
static bool present;

void cpu_init(void) {
    vet(!initialized, "cpu_init: already initialized");
    const cpu_state_t state = get_cpu();
    vet((state & CPU_RESET) || !(state & CPU_READY), "cpu_init: CPU must be reset or halted");

    // Isolate the socket path before installing a stack-independent SRAM probe.
    set_cpu(CPU_RESET);
    set_cpu_type(CPU_PHYS_6502);
    // This stack-independent 6502 probe proves that the socketed processor can
    // fetch instructions from SRAM and write results back over the physical
    // bus.
    //
    // The reset vector enters at $0400, which first establishes known interrupt
    // flags and jumps over the entry stub. The routine then stores the marker
    // in scratch RAM and loops at $040A.
    //
    // Below, we use `bp_run_to()` to detect that the CPU has advanced to the
    // loop, then check the marker to confirm that the preceding RAM write worked.
    const uint8_t program[] = {
        // Establish known flags, then enter the body without using the stack.
        OPCODE_SEI,
        OPCODE_CLD,
        OPCODE_JMP,
        PROBE_ROUTINE & 0xFFu,
        PROBE_ROUTINE >> 8,
        // Publish a value that proves execution reached the probe body and RAM
        // writes worked before fetching the breakpointed loop instruction.
        OPCODE_LDA_IMMEDIATE,
        PROBE_MARKER,
        OPCODE_STA_ABSOLUTE,
        PROBE_RESULT & 0xFFu,
        PROBE_RESULT >> 8,
        OPCODE_JMP,
        PROBE_LOOP & 0xFFu,
        PROBE_LOOP >> 8,
    };

    static_assert(PROBE_START + sizeof(program) <= PROBE_RESULT, "Probe overlaps result");
    static_assert(PROBE_SENTINEL != PROBE_MARKER, "Probe must overwrite the sentinel");

    // Copy program to SRAM and set the reset vector to the entry point.
    spi_write(PROBE_START, program, sizeof(program));
    spi_write_at(PROBE_RESULT, PROBE_SENTINEL);
    spi_write_at(RESET_VECTOR, PROBE_START & 0xFFu);
    spi_write_at(RESET_VECTOR + 1u, PROBE_START >> 8);

    // Allow a complete reset pulse before the temporary run.
    bp_clear_halt();
    sleep_us(RESET_PULSE_US);

    // `bp_run_to()` will resume the CPU and wait for the CPU to reach JMP
    // in the probe loop.
    const bp_run_result_t result = bp_run_to(PROBE_LOOP, PROBE_TIMEOUT_US);

    // We've either reached the probe loop or timed out. Put the CPU back in
    // reset while we interpret the results.
    set_cpu(CPU_RESET);

    if (result.status == bp_run_reached) {
        // CPU fetched the expected instruction. Check that it also wrote
        // the marker to the expected address.
        const uint8_t marker = spi_read_at(PROBE_RESULT);
        vet(marker == PROBE_MARKER,
            "cpu_init: probe marker mismatch ($%02X, expected $%02X)", marker, PROBE_MARKER);

        present = true;
    } else {
        // Neither a timeout nor a halt elsewhere proves socketed execution.
        // Preserve unrelated breakpoint state and fall back to the soft CPU.
        vet(result.status == bp_run_timeout || result.status == bp_run_stopped_elsewhere,
            "cpu_init: unexpected probe result (%d)", result.status);
        present = false;
    }

    set_cpu_type(present ? CPU_PHYS_6502 : CPU_SOFT_6502);
    initialized = true;
    log_info("physical 6502: %s", present ? "present" : "not populated (using soft CPU)");

    // Note: We intentionally leave the CPU held in reset after probing.
}

bool physical_cpu_present(void) {
    vet(initialized, "physical_cpu_present: cpu_init required");
    return present;
}

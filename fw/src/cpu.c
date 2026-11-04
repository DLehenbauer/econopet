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

// Stop at the loop and let cpu_init() validate execution after cleanup.
static bp_result_t probe_complete(uint16_t pc, void* context) {
    set_cpu(CPU_HALT);
    *(bool*)context = true;
    return (bp_result_t){.pc = pc, .rearm = false};
}

void cpu_init(void) {
    vet(!initialized, "cpu_init: already initialized");
    const cpu_state_t state = get_cpu();
    vet((state & CPU_RESET) || !(state & CPU_READY), "cpu_init: CPU must be reset or halted");

    // Isolate the socket path before installing a stack-independent SRAM probe.
    set_cpu(CPU_RESET);
    set_cpu_type(CPU_PHYS_6502);
    const uint8_t program[] = {
        // Enter the routine without relying on stack contents or inherited flags.
        OPCODE_SEI,
        OPCODE_CLD,
        OPCODE_JMP,
        PROBE_ROUTINE & 0xFFu,
        PROBE_ROUTINE >> 8,
        // The store must execute before the final JMP is fetched as a breakpoint.
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
    spi_write(PROBE_START, program, sizeof(program));
    spi_write_at(PROBE_RESULT, PROBE_SENTINEL);
    spi_write_at(RESET_VECTOR, PROBE_START & 0xFFu);
    spi_write_at(RESET_VECTOR + 1u, PROBE_START >> 8);

    // Arm the probe while reset is asserted, discarding any retained FPGA halt.
    bool hit = false;
    bp_clear_halt();
    bp_set(PROBE_LOOP, probe_complete, &hit);

    // Allow a complete reset pulse before waiting for a real instruction fetch.
    sleep_us(RESET_PULSE_US);
    const uint64_t start = time_us_64();
    set_cpu(CPU_READY);
    do {
        system_state.bp_halted = (spi_read_at(ECONOPET_WB_STATUS_ADDR) & ECONOPET_REG_STATUS_BP_HALT_MASK) != 0;
        if (system_state.bp_halted) {
            if (bp_hit_addr() == PROBE_LOOP) {
                bp_task();
            }
            break;
        }
    } while (time_us_64() - start < PROBE_TIMEOUT_US);

    // Timeout or an unrelated halt leaves our patch installed, unlike a valid hit.
    set_cpu(CPU_RESET);
    if (!hit) {
        bp_remove(PROBE_LOOP);
    }
    bp_clear_halt();

    // Keep fatal rendering independent of an armed breakpoint or a running CPU.
    if (hit) {
        const uint8_t marker = spi_read_at(PROBE_RESULT);
        vet(marker == PROBE_MARKER, "cpu_init: probe marker mismatch ($%02X, expected $%02X)", marker, PROBE_MARKER);
    }

    // Leave initialization quiescent so the menu can replace the scratch image.
    present = hit;
    set_cpu_type(present ? CPU_PHYS_6502 : CPU_SOFT_6502);
    initialized = true;
    log_info("physical 6502: %s", present ? "detected" : "not populated (using soft CPU)");
}

bool physical_cpu_present(void) {
    vet(initialized, "physical_cpu_present: cpu_init required");
    return present;
}

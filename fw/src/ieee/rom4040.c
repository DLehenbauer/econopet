// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#define CHIPS_IMPL
#include "rom4040.h"

#include <string.h>

#define RIOT_ATNA 0x01u
#define RIOT_DACO 0x02u
#define RIOT_RFDO 0x04u
#define RIOT_EOIO 0x08u
#define RIOT_DAVO 0x10u
#define RIOT_EOII 0x20u
#define RIOT_DAVI 0x40u
#define RIOT_ATNI 0x80u
#define CONTROLLER_ID 0x0fu
#define JOBS_OFFSET 3u
#define HEADERS_OFFSET 0x21u
#define JOB_EXECUTE 0xd0u
#define TIMER_FLAG 0x80u
#define EDGE_FLAG 0x40u
#define TIMER_POWER_ON_DIVISOR 1024u

// Resolve output pins of a RIOT port, with pullups on input-configured bits.
static uint8_t output_port(const rom4040_t* m, unsigned riot, unsigned port) {
    const uint8_t direction = m->ports[riot][port + 1];
    return (m->ports[riot][port] & direction) | (uint8_t) ~direction;
}

rom4040_bus_t rom4040_output(const rom4040_t* m) {
    const uint8_t control = output_port(m, 1, 0);
    const bool atna = (control & RIOT_ATNA) != 0;
    const bool daco = (control & RIOT_DACO) != 0;
    const bool rfdo = (control & RIOT_RFDO) != 0;
    // Schematic 320806, corroborated by MAME c2040::update_ieee_signals.
    return (rom4040_bus_t) {
        .atn = true,
        .dav = (control & RIOT_DAVO) != 0,
        .eoi = (control & RIOT_EOIO) != 0,
        .nrfd = !(!(!(m->input.atn && atna) && rfdo) || !(m->input.atn || atna)),
        .ndac = !(daco || !(m->input.atn || atna)),
        .dio = output_port(m, 0, 2),
    };
}

// PA7 detection observes the DDR-composed pin, independently on each RIOT.
static void edges(rom4040_t* m) {
    const uint8_t input[] = {
        m->input.dio & rom4040_output(m).dio,
        !m->input.atn ? RIOT_ATNI : 0,
    };
    for (unsigned riot = 0; riot < 2; ++riot) {
        const uint8_t ddr = m->ports[riot][1];
        const bool level = ((m->ports[riot][0] & ddr)
            | (input[riot] & (uint8_t) ~ddr)) & RIOT_ATNI;
        if (level != m->edge_level[riot] && level == m->edge_positive[riot])
            m->edge_pending[riot] = true;
        m->edge_level[riot] = level;
    }
}

void rom4040_init(rom4040_t* m, const uint8_t rom[ROM4040_ROM_SIZE],
    rom4040_job_fn job, void* context) {
    *m = (rom4040_t) {
        .rom = rom, .job = job, .context = context,
        .input = {true, true, true, true, true, 0xff},
        .edge_level = {true, false},
        .timers = {
            {.prescale = TIMER_POWER_ON_DIVISOR, .divider = TIMER_POWER_ON_DIVISOR, .value = 0xff},
            {.prescale = TIMER_POWER_ON_DIVISOR, .divider = TIMER_POWER_ON_DIVISOR, .value = 0xff},
        },
    };
    m->pins = m6502_init(&m->cpu, &(m6502_desc_t) {0});
}

void rom4040_input(rom4040_t* m, rom4040_bus_t input) {
    const bool changed = input.atn != m->input.atn || input.dio != m->input.dio;
    m->input = input;
    if (changed) edges(m);
}

// Read port registers with DDR composition and native ATN edge-flag clearing.
static uint8_t riot_read(rom4040_t* m, unsigned riot, unsigned reg) {
    if (!(reg & 4)) {
        reg &= 3;
        if (reg & 1) return m->ports[riot][reg];
        const rom4040_bus_t out = rom4040_output(m);
        const uint8_t input = riot == 0
            ? (reg == 0 ? m->input.dio & out.dio : 0xff)
            : (reg == 0
                ? ((m->input.eoi && out.eoi) ? RIOT_EOII : 0)
                    | ((m->input.dav && out.dav) ? RIOT_DAVI : 0)
                    | (!m->input.atn ? RIOT_ATNI : 0)
                : ((m->input.ndac && out.ndac) ? 0x40 : 0)
                    | ((m->input.nrfd && out.nrfd) ? 0x80 : 0));
        const uint8_t ddr = m->ports[riot][reg + 1];
        return (m->ports[riot][reg] & ddr) | (input & (uint8_t) ~ddr);
    }
    rom4040_timer_t* const timer = &m->timers[riot];
    if (reg & 1) {
        const uint8_t flag = (m->edge_pending[riot] ? EDGE_FLAG : 0)
            | (timer->pending ? TIMER_FLAG : 0);
        m->edge_pending[riot] = false;
        return flag;
    }
    const uint8_t value = timer->value;
    // A same-cycle underflow wins over a clearing read. After underflow, a timer
    // read restores the selected prescaler using the current free-running value.
    if ((uint32_t) (m->cycles - timer->underflow) != 0) {
        timer->pending = false;
        if (timer->expired) {
            timer->expired = false;
            timer->divider = 1;
        }
    }
    timer->enabled = (reg & 8) != 0;
    return value;
}

// Write native port/DDRs or configure the PA7 edge detector.
static void riot_write(rom4040_t* m, unsigned riot, unsigned reg, uint8_t value) {
    if (!(reg & 4)) {
        m->ports[riot][reg & 3] = value;
        edges(m);
    } else if (!(reg & 0x10)) {
        m->edge_positive[riot] = (reg & 1) != 0;
        m->edge_enabled[riot] = (reg & 2) != 0;
    } else {
        static const uint16_t divisors[] = {1, 8, 64, 1024};
        rom4040_timer_t* const timer = &m->timers[riot];
        timer->prescale = divisors[reg & 3];
        timer->divider = 1;
        timer->value = value;
        timer->pending = timer->expired = false;
        timer->enabled = (reg & 8) != 0;
    }
}

// Timers decrement once immediately after load, then at their selected divisor.
static void timers(rom4040_t* m) {
    for (unsigned riot = 0; riot < 2; ++riot) {
        rom4040_timer_t* const timer = &m->timers[riot];
        if (--timer->divider != 0) continue;
        if (timer->value-- == 0) {
            if (!timer->expired) {
                timer->pending = true;
                timer->underflow = m->cycles;
            }
            timer->expired = true;
        }
        timer->divider = timer->expired ? 1 : timer->prescale;
    }
}

// Submit a native job only after power-on RAM diagnostics have finished.
static void submit(rom4040_t* m, unsigned offset, uint8_t value) {
    if (offset < JOBS_OFFSET || offset >= JOBS_OFFSET + ROM4040_JOB_COUNT
        || !(value & 0x80)) return;
    if (!m->running) {
        // DSKINT sends the 6504 to its wait loop before destructive RAM tests.
        static const uint8_t wait_loop[] = {0x78, 0xa9, 0x00, 0x8d, 0x03, 0x04, 0x4c, 0x04, 0xfc};
        if (offset == JOBS_OFFSET && value == JOB_EXECUTE
            && memcmp(m->shared + 0x100, wait_loop, sizeof(wait_loop)) == 0) {
            m->shared[JOBS_OFFSET] = 0;
            m->bootstrap = true;
        }
        return;
    }
    const unsigned index = offset - JOBS_OFFSET;
    ++m->jobs;
    if (index >= ROM4040_SHARED_SIZE / ROM4040_SECTOR_SIZE - 1) {
        m->fault = ROM4040_FAULT_CONTROLLER_CODE;
        return;
    }
    m->shared[offset] = m->job(m->context, value,
        m->shared + HEADERS_OFFSET + index * 8,
        m->shared + (index + 1) * ROM4040_SECTOR_SIZE);
}

// Map main-CPU aliases (A15 is not decoded, stack aliases the two RIOT RAMs).
static uint8_t memory(rom4040_t* m, uint16_t address, bool read, uint8_t value) {
    const uint16_t a = address & 0x7fff;
    if (a < 0x200) {
        if (!read) m->riot_ram[a & 0xff] = value;
        return m->riot_ram[a & 0xff];
    }
    if (a < 0x1000 && (a & 0x200)) {
        return read ? riot_read(m, (a >> 7) & 1, a & 0x1f)
            : (riot_write(m, (a >> 7) & 1, a & 0x1f, value), value);
    }
    if (a < 0x1000) return value;
    if (a < 0x5000) {
        const unsigned offset = (((a >> 12) - 1) << 10) | (a & 0x3ff);
        if (!read) {
            m->shared[offset] = value;
            // The final descending RAM test clears $4000 last. Publish the
            // controller ID only after that store, never during CMP/EOR tests.
            if (a == 0x4000 && value == 0 && m->bootstrap && !m->running) {
                m->shared[0] = CONTROLLER_ID;
                m->running = true;
            }
            submit(m, offset, value);
        }
        return m->shared[offset];
    }
    return m->rom[a - 0x5000];
}

void rom4040_tick(rom4040_t* m) {
    timers(m);
    if ((m->edge_enabled[1] && m->edge_pending[1])
        || (m->timers[1].enabled && m->timers[1].pending)) m->pins |= M6502_IRQ;
    else m->pins &= ~M6502_IRQ;
    m->pins = m6502_tick(&m->cpu, m->pins);
    const uint8_t value = memory(m, M6502_GET_ADDR(m->pins),
        (m->pins & M6502_RW) != 0, M6502_GET_DATA(m->pins));
    M6502_SET_DATA(m->pins, value);
    ++m->cycles;
}

void rom4040_run(rom4040_t* m, uint32_t cycles) {
    while (cycles-- && m->fault == ROM4040_FAULT_NONE) rom4040_tick(m);
}

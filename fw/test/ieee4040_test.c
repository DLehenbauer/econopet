// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "ieee4040_test.h"

#include <string.h>

#include "ieee/controller4040.h"
#include "ieee/fifo4040.h"

static const uint16_t Code = 0xd800;
static const uint8_t Result = 0x70;
static const unsigned CpuBudget = 64;
static const uint16_t DataPort = 0x0200, DataDdr = 0x0201;
static const uint16_t OutputPort = 0x0202, OutputDdr = 0x0203;
static const uint16_t ControlPort = 0x0280, ControlDdr = 0x0281;
static const uint16_t Riot0 = 0x0200, Riot1 = 0x0280;
static const uint8_t EdgeFlag = 0x40, TimerFlag = 0x80;
static const unsigned HeaderBase = 0x21, JobBase = 3;
static const unsigned FirstBuffer = 0x100, FormatterPrefix = 0x200;
enum {Read = 0x80, Write = 0x90, Verify = 0xa0, Seek = 0xb0, Bump = 0xc0, Format = 0xe0};
enum {Ok = 1, HeaderError = 2, NoSync = 3, DataError = 5, VerifyError = 7, Protected = 8, IdError = 11};
static uint8_t rom[ROM4040_ROM_SIZE];
static rom4040_t machine;
static controller4040_t controller;
static uint8_t media[CONTROLLER4040_DRIVES][ROM4040_SECTOR_SIZE];
static unsigned reads[CONTROLLER4040_DRIVES], writes[CONTROLLER4040_DRIVES];
static bool fail_io;

// Reset every borrowed buffer and production object between same-process Check cases.
static void setup(void) {
    memset(rom, 0, sizeof(rom));
    rom[0x2ffc] = (uint8_t) Code;
    rom[0x2ffd] = Code >> 8;
    rom4040_init(&machine, rom, NULL, NULL);
    memset(&controller, 0, sizeof(controller));
    memset(media, 0, sizeof(media));
    memset(reads, 0, sizeof(reads));
    memset(writes, 0, sizeof(writes));
    fail_io = false;
}

// Restart CPU execution only, retaining the peripheral state being tested.
static void program(const uint8_t* bytes, unsigned size) {
    memcpy(rom + Code - 0xd000, bytes, size);
    machine.pins = m6502_init(&machine.cpu, &(m6502_desc_t) {0});
}

// Stop on the actual CPU write so timer tests can count every subsequent cycle.
static void store(uint16_t address, uint8_t value) {
    const uint8_t code[] = {0xa9, value, 0x8d, (uint8_t) address, address >> 8,
        0x4c, (uint8_t) (Code + 5), Code >> 8};
    program(code, sizeof(code));
    for (unsigned cycle = 0; cycle < CpuBudget; ++cycle) {
        rom4040_tick(&machine);
        ck_assert_int_eq(machine.fault, ROM4040_FAULT_NONE);
        if (!(machine.pins & M6502_RW) && M6502_GET_ADDR(machine.pins) == address) return;
    }
    ck_abort_msg("synthetic 6502 did not write address %04x", address);
}

// Observe the register through a real LDA/STA bus sequence, including read side effects.
static uint8_t sample(uint16_t address) {
    const uint8_t code[] = {0xad, (uint8_t) address, address >> 8,
        0x8d, Result, 0, 0x4c, (uint8_t) (Code + 6), Code >> 8};
    program(code, sizeof(code));
    for (unsigned cycle = 0; cycle < CpuBudget; ++cycle) {
        rom4040_tick(&machine);
        ck_assert_int_eq(machine.fault, ROM4040_FAULT_NONE);
        if (!(machine.pins & M6502_RW) && M6502_GET_ADDR(machine.pins) == Result)
            return machine.riot_ram[Result];
    }
    ck_abort_msg("synthetic 6502 did not read address %04x", address);
    return 0;
}

// Port/DDR mirrors must not be mistaken for timer or edge registers.
START_TEST(port_mirrors_preserve_ddr_latches_and_wired_inputs) {
    const uint16_t bases[] = {Riot0, Riot1};
    for (unsigned riot = 0; riot < 2; ++riot) {
        for (unsigned mirror = 0; mirror < 32; mirror += 8) {
            store(bases[riot] + mirror + 1, 0xf0);
            store(bases[riot] + mirror, 0xa5);
            ck_assert_uint_eq(sample(bases[riot] + 1), 0xf0);
            ck_assert_uint_eq(sample(bases[riot] + mirror + 1), 0xf0);
            ck_assert_uint_eq(sample(bases[riot] + mirror) & 0xf0, 0xa0);
        }
    }
    store(DataDdr, 0);
    store(OutputDdr, 0x0f);
    store(OutputPort, 0xa5);
    ck_assert_uint_eq(sample(DataPort), 0xf5);
    rom4040_bus_t peer = machine.input;
    peer.dio = 0x3f;
    rom4040_input(&machine, peer);
    ck_assert_uint_eq(sample(DataPort | 0x8d60), 0x35);
}
END_TEST

// RIOT0 and RIOT1 flags must survive each other's reads, and only RIOT1 drives IRQ.
START_TEST(pa7_edges_are_independent_ddr_composed_and_mirrored) {
    store(Riot0 + 0x0e, 0);
    store(Riot1 + 0x0f, 0);
    rom4040_bus_t peer = machine.input;
    peer.dio = 0x7f;
    rom4040_input(&machine, peer);
    rom4040_tick(&machine);
    ck_assert(machine.edge_pending[0]);
    ck_assert_uint_eq(machine.pins & M6502_IRQ, 0);
    peer.atn = false;
    rom4040_input(&machine, peer);
    ck_assert_uint_eq(sample(Riot0 + 5) & EdgeFlag, EdgeFlag);
    ck_assert(machine.edge_pending[1]);
    ck_assert_uint_ne(machine.pins & M6502_IRQ, 0);
    ck_assert_uint_eq(sample(Riot1 + 0x1f) & EdgeFlag, EdgeFlag);
    ck_assert_uint_eq(sample(Riot1 + 7) & EdgeFlag, 0);
    store(Riot1 + 0x0c, 0);
    store(ControlPort, 0);
    store(ControlDdr, 0x80);
    ck_assert_uint_eq(sample(Riot1 + 5) & EdgeFlag, EdgeFlag);
    peer.atn = true;
    rom4040_input(&machine, peer);
    peer.atn = false;
    rom4040_input(&machine, peer);
    ck_assert_uint_eq(sample(Riot1 + 5) & EdgeFlag, 0);
}
END_TEST

// RIOT0 PB's wired DIO feedback must generate PA7 edges even without new external input.
START_TEST(pa7_output_feedback_and_reset_do_not_retain_stale_state) {
    store(Riot0 + 4, 0);
    store(OutputPort, 0x7f);
    ck_assert(!machine.edge_pending[0]);
    store(OutputDdr, 0x80);
    ck_assert_uint_eq(sample(Riot0 + 5) & EdgeFlag, EdgeFlag);
    store(Riot0 + 5, 0);
    store(OutputPort, 255);
    ck_assert_uint_eq(sample(Riot0 + 5) & EdgeFlag, EdgeFlag);
    ck_assert_uint_eq(machine.pins & M6502_IRQ, 0);
    store(Riot1 + 7, 0);
    store(Riot1 + 0x1f, 0);
    rom4040_bus_t peer = machine.input;
    peer.atn = false;
    rom4040_input(&machine, peer);
    machine.bootstrap = machine.running = true;
    machine.jobs = 1;
    rom4040_init(&machine, rom, NULL, NULL);
    ck_assert(!machine.running);
    ck_assert(!machine.bootstrap);
    ck_assert_uint_eq(machine.jobs, 0);
    for (unsigned riot = 0; riot < 2; ++riot) {
        ck_assert(!machine.edge_pending[riot]);
        ck_assert(!machine.timers[riot].pending);
        ck_assert(!machine.edge_enabled[riot]);
        ck_assert(!machine.timers[riot].enabled);
    }
    ck_assert_uint_eq(machine.pins & (M6502_IRQ | M6502_NMI), 0);
    ck_assert_uint_eq(rom4040_output(&machine).dio, 255);
}
END_TEST

// Loading zero and every prescaler catches off-by-one underflow and accidental timer rejection.
START_TEST(timer_underflow_masks_mirrors_and_acknowledgement) {
    const unsigned divisors[] = {1, 8, 64, 1024};
    const uint16_t bases[] = {Riot0, Riot1};
    for (unsigned riot = 0; riot < 2; ++riot) {
        for (unsigned mode = 0; mode < 4; ++mode) {
            store(bases[riot] + 0x1c + mode, 2);
            const unsigned boundary = 1 + 2 * divisors[mode];
            for (unsigned cycle = 1; cycle <= boundary; ++cycle) {
                rom4040_tick(&machine);
                ck_assert_int_eq(machine.timers[riot].pending, cycle == boundary);
            }
            ck_assert_uint_eq(machine.timers[riot].value, 255);
            ck_assert_int_eq((machine.pins & M6502_IRQ) != 0, riot == 1);
            ck_assert_uint_eq(sample(bases[riot] + 0x17) & TimerFlag, TimerFlag);
            ck_assert(machine.timers[riot].pending);
            store(bases[riot] + 0x17, 255);
            ck_assert(!machine.timers[riot].enabled);
            ck_assert(!machine.timers[riot].pending);
            sample(bases[riot] + 0x0c);
            ck_assert(machine.timers[riot].enabled);
            sample(bases[riot] + 4);
            ck_assert(!machine.timers[riot].enabled);
            store(bases[riot] + 0x1c + mode, 0);
            ck_assert(!machine.timers[riot].pending);
            rom4040_tick(&machine);
            ck_assert(machine.timers[riot].pending);
            rom4040_tick(&machine);
            ck_assert_uint_eq(machine.timers[riot].value, 254);
            sample(bases[riot] + 0x0c);
            ck_assert(!machine.timers[riot].pending);
            ck_assert(!machine.timers[riot].expired);
        }
        store(bases[riot] + 0x17, 255);
    }
}
END_TEST

// Flags and timer reads acknowledge different sources, including same-cycle timer underflow.
START_TEST(timer_and_edge_sources_have_independent_read_acknowledgement) {
    store(Riot1 + 7, 0);
    store(Riot1 + 0x1f, 0);
    rom4040_bus_t peer = machine.input;
    peer.atn = false;
    rom4040_input(&machine, peer);
    ck_assert_uint_eq(sample(Riot1 + 0x1d), TimerFlag | EdgeFlag);
    ck_assert(!machine.edge_pending[1]);
    ck_assert(machine.timers[1].pending);
    sample(Riot1 + 0x0c);
    ck_assert(!machine.timers[1].pending);

    // Execute a real timer read with underflow coinciding with its data bus phase.
    const uint8_t code[] = {0xad, 0x8c, 2, 0x8d, Result, 0,
        0x4c, (uint8_t) (Code + 6), Code >> 8};
    program(code, sizeof(code));
    for (unsigned cycle = 0; cycle < CpuBudget; ++cycle) {
        // Arrange underflow at the actual bus read, not at a guessed instruction delay.
        if ((machine.pins & M6502_RW) && M6502_GET_ADDR(machine.pins) == Code + 2) {
            machine.timers[1].value = 0;
            machine.timers[1].divider = 1;
            machine.timers[1].pending = machine.timers[1].expired = false;
        }
        rom4040_tick(&machine);
        if ((machine.pins & M6502_RW) && M6502_GET_ADDR(machine.pins) == Riot1 + 0x0c) {
            ck_assert(machine.timers[1].pending);
            ck_assert(machine.timers[1].expired);
            ck_assert_uint_eq(machine.timers[1].underflow, machine.cycles - 1);
            return;
        }
    }
    ck_abort_msg("6502 did not reach coincident timer-read/underflow cycle");
}
END_TEST

// Publishing the controller ID during descending diagnostics corrupts the real ROM's RAM test.
START_TEST(bootstrap_publishes_id_only_after_final_ram_clear) {
    const uint8_t parking[] = {0x78, 0xa9, 0, 0x8d, 3, 4, 0x4c, 4, 0xfc};
    memcpy(machine.shared + FirstBuffer, parking, sizeof(parking));
    store(0x1003, 0xd0);
    ck_assert(machine.bootstrap);
    ck_assert_uint_eq(sample(0x1003), 0);
    store(0x1000, 0x55);
    ck_assert_uint_eq(sample(0x1000), 0x55);
    store(0x4000, 1);
    ck_assert(!machine.running);
    ck_assert_uint_eq(sample(0x1000), 0x55);
    store(0x4000, 0);
    ck_assert(machine.running);
    ck_assert_uint_eq(sample(0x1000), 15);
}
END_TEST

// Supply a deliberately distinct per-drive sector and record writes without needing DOS or ROMs.
static bool sector_io(void* context, uint32_t offset, uint8_t* bytes, bool write) {
    const unsigned drive = *(unsigned*) context;
    ck_assert_uint_lt(drive, CONTROLLER4040_DRIVES);
    ck_assert_uint_le(offset, CONTROLLER4040_IMAGE_SIZE - ROM4040_SECTOR_SIZE);
    if (write) ++writes[drive];
    else ++reads[drive];
    if (fail_io) return false;
    if (write) memcpy(media[drive], bytes, ROM4040_SECTOR_SIZE);
    else memcpy(bytes, media[drive], ROM4040_SECTOR_SIZE);
    return true;
}

// Bind both media callbacks while keeping their backing state independently observable.
static void mount_disks(void) {
    static unsigned drives[] = {0, 1};
    controller.machine = &machine;
    for (unsigned drive = 0; drive < CONTROLLER4040_DRIVES; ++drive) {
        controller.disk[drive] = (controller4040_disk_t) {
            .io = sector_io, .context = &drives[drive], .writable = true,
            .id = {'I', (uint8_t) ('D' + drive)},
        };
        memset(media[drive], 0x55 + drive, sizeof(media[drive]));
    }
}

// Native FED3 mutates header byte 4, including verification and missing-sector errors.
START_TEST(controller_data_jobs_publish_checksum_and_preserve_unrelated_bytes) {
    mount_disks();
    for (unsigned drive = 0; drive < CONTROLLER4040_DRIVES; ++drive) {
        uint8_t header[] = {'I', (uint8_t) ('D' + drive), 18, 0, 0x59, 0x6a, 0x7b, 0x8c};
        uint8_t sector[ROM4040_SECTOR_SIZE];
        ck_assert_uint_eq(controller4040_job(&controller, Read | drive, header, sector), Ok);
        ck_assert_mem_eq(sector, media[drive], sizeof(sector));
        ck_assert_uint_eq(header[4], header[0] ^ header[1] ^ header[2] ^ header[3]);
        ck_assert_uint_eq(header[5], 0x6a);
        ck_assert_uint_eq(header[6], 0x7b);
        ck_assert_uint_eq(header[7], 0x8c);
        header[4] = 0x59;
        ck_assert_uint_eq(controller4040_job(&controller, Verify | drive, header, sector), Ok);
        ck_assert_uint_eq(header[4], header[0] ^ header[1] ^ 18);
        sector[17] ^= 1;
        ck_assert_uint_eq(controller4040_job(&controller, Verify | drive, header, sector), VerifyError);
        ck_assert_uint_eq(controller4040_job(&controller, Write | drive, header, sector), Ok);
        ck_assert_mem_eq(sector, media[drive], sizeof(sector));
        fail_io = true;
        ck_assert_uint_eq(controller4040_job(&controller, Read | drive, header, sector), DataError);
        fail_io = false;
        header[2] = 1;
        header[3] = 21;
        header[4] = 0x59;
        ck_assert_uint_eq(controller4040_job(&controller, Read | drive, header, sector), HeaderError);
        ck_assert_uint_eq(header[4], header[0] ^ header[1] ^ 1 ^ 21);
        header[0] = 'X';
        header[4] = 0x59;
        ck_assert_uint_eq(controller4040_job(&controller, Read | drive, header, sector), IdError);
        ck_assert_uint_eq(header[4], 0x59);
        header[0] = 'I';
        header[2] = 36;
        ck_assert_uint_eq(controller4040_job(&controller, Read | drive, header, sector), NoSync);
        ck_assert_uint_eq(header[4], 0x59);
    }
}
END_TEST

// Native BUMP succeeds without media and never rewrites a caller's track or sector.
START_TEST(controller_bump_seek_and_protected_write_keep_native_header_effects) {
    uint8_t header[] = {'?', '?', 0, 7, 0x59, 0x6a, 0x7b, 0x8c};
    uint8_t sector[ROM4040_SECTOR_SIZE] = {0};
    controller.machine = &machine;
    uint8_t expected[sizeof(header)];
    memcpy(expected, header, sizeof(header));
    for (unsigned drive = 0; drive < CONTROLLER4040_DRIVES; ++drive) {
        ck_assert_uint_eq(controller4040_job(&controller, Bump | drive, header, sector), Ok);
        ck_assert_mem_eq(header, expected, sizeof(header));
        ck_assert_uint_eq(controller4040_job(&controller, Seek | drive, header, sector), NoSync);
    }
    mount_disks();
    ck_assert_uint_eq(controller4040_job(&controller, Seek, header, sector), Ok);
    ck_assert_uint_eq(header[0], 'I');
    ck_assert_uint_eq(header[1], 'D');
    ck_assert_uint_eq(header[2], 1);
    ck_assert_uint_eq(header[3], 1);
    ck_assert_uint_eq(header[4], 'I' ^ 'D' ^ 1 ^ 1);
    controller.disk[0].writable = false;
    header[4] = 0x59;
    memcpy(expected, header, sizeof(header));
    ck_assert_uint_eq(controller4040_job(&controller, Write, header, sector), Protected);
    ck_assert_mem_eq(header, expected, sizeof(header));
    ck_assert_uint_eq(writes[0], 0);
}
END_TEST

// Stock FORMAT publishes its final physical header and changes ID only after success.
START_TEST(controller_formatter_headers_success_protection_and_failure) {
    mount_disks();
    memcpy(machine.shared + FirstBuffer, rom, FormatterPrefix);
    uint8_t header[] = {'N', 'X', 1, 7, 0x59, 0x6a, 0x7b, 0x8c};
    uint8_t sector[ROM4040_SECTOR_SIZE] = {0};
    controller.disk[0].writable = false;
    ck_assert_uint_eq(controller4040_job(&controller, Format, header, sector), Protected);
    ck_assert_uint_eq(header[2], 1);
    ck_assert_uint_eq(header[3], 0);
    ck_assert_uint_eq(header[4], 'N' ^ 'X' ^ 1);
    ck_assert_uint_eq(writes[0], 0);
    ck_assert_uint_eq(controller.disk[0].id[0], 'I');
    controller.disk[0].writable = true;
    fail_io = true;
    ck_assert_uint_eq(controller4040_job(&controller, Format, header, sector), DataError);
    ck_assert_uint_eq(controller.disk[0].id[0], 'I');
    fail_io = false;
    ck_assert_uint_eq(controller4040_job(&controller, Format, header, sector), Ok);
    ck_assert_uint_eq(header[2], 35);
    ck_assert_uint_eq(header[3], 0);
    ck_assert_uint_eq(header[4], 'N' ^ 'X' ^ 35);
    ck_assert_uint_eq(controller.disk[0].id[0], 'N');
    ck_assert_uint_eq(controller.disk[0].id[1], 'X');
    ck_assert_uint_eq(writes[0], 1 + CONTROLLER4040_IMAGE_SIZE / ROM4040_SECTOR_SIZE);
    ck_assert_uint_eq(writes[1], 0);
    for (unsigned i = 0; i < sizeof(media[0]); ++i) ck_assert_uint_eq(media[0][i], 0);
}
END_TEST

// Every CPU mailbox routes its original header and buffer and completes exactly once.
START_TEST(controller_mailbox_slots_and_aliases_do_not_overlap) {
    mount_disks();
    machine.running = true;
    machine.context = &controller;
    machine.job = controller4040_job;
    for (unsigned slot = 0; slot < ROM4040_JOB_COUNT; ++slot) {
        const unsigned drive = slot & 1;
        uint8_t* header = machine.shared + HeaderBase + slot * 8;
        memcpy(header, (uint8_t[]) {'I', (uint8_t) ('D' + drive), 1, 0}, 4);
        memset(machine.shared + (slot + 1) * ROM4040_SECTOR_SIZE, 0,
            ROM4040_SECTOR_SIZE);
        store(0x9003 + slot, Read | drive);
        ck_assert_uint_eq(sample(0x1003 + slot), Ok);
        ck_assert_uint_eq(machine.jobs, slot + 1);
        ck_assert_mem_eq(machine.shared + (slot + 1) * ROM4040_SECTOR_SIZE,
            media[drive], ROM4040_SECTOR_SIZE);
    }
}
END_TEST

// A held or privately sampled TX offer cannot advance GET, including either ATN abort side.
START_TEST(fifo_offer_consumption_and_abort_conservation) {
    store(OutputDdr, 255);
    store(OutputPort, (uint8_t) ~'A');
    store(ControlDdr, 0x1f);
    store(ControlPort, 0x0c);
    fifo4040_t fifo;
    fifo4040_init(&fifo, &machine);
    fifo4040_tick(&fifo);
    uint8_t byte;
    bool eoi;
    ck_assert(fifo4040_transmit(&fifo, &byte, &eoi));
    ck_assert_uint_eq(byte, 'A');
    ck_assert(!eoi);
    for (unsigned pass = 0; pass < 100; ++pass) {
        fifo4040_tick(&fifo);
        ck_assert(!fifo4040_transmit(&fifo, &byte, &eoi));
    }
    ck_assert_uint_eq(fifo.presented, 1);
    ck_assert_uint_eq(fifo.consumed, 0);
    fifo4040_consumed(&fifo, false);
    ck_assert(fifo.peer.nrfd);
    ck_assert_uint_eq(fifo.consumed, 0);
    fifo4040_receive(&fifo, 0x5f, true, false);
    ck_assert_uint_eq(fifo.consumed, 0);
    ck_assert(!fifo.transmitting);
    fifo4040_init(&fifo, &machine);
    fifo4040_tick(&fifo);
    ck_assert(fifo4040_transmit(&fifo, &byte, &eoi));
    fifo4040_consumed(&fifo, true);
    ck_assert(!fifo.peer.nrfd);
    ck_assert_uint_eq(fifo.consumed, 1);
    fifo4040_receive(&fifo, 0x5f, true, false);
    ck_assert_uint_eq(fifo.consumed, 1);
    ck_assert(!fifo.transmitting);
}
END_TEST

// RX metadata stays owned until all three RIOT handshake phases have actually completed.
START_TEST(fifo_listener_preserves_flags_and_does_not_finish_on_queued_data) {
    store(ControlDdr, 0x1f);
    store(ControlPort, 0x1c);
    fifo4040_t fifo;
    fifo4040_init(&fifo, &machine);
    fifo4040_receive(&fifo, 0xa5, true, true);
    ck_assert(!fifo4040_tick(&fifo));
    ck_assert_uint_eq(fifo.receive, FIFO4040_RX_READY);
    store(ControlPort, 0x1f);
    ck_assert(!fifo4040_tick(&fifo));
    ck_assert_uint_eq(fifo.receive, FIFO4040_RX_ACCEPT);
    ck_assert_uint_eq(fifo.peer.dio, (uint8_t) ~0xa5);
    ck_assert(!fifo.peer.dav);
    ck_assert(!fifo.peer.eoi);
    for (unsigned pass = 0; pass < 100; ++pass) {
        ck_assert(!fifo4040_tick(&fifo));
        ck_assert_uint_eq(fifo.receive, FIFO4040_RX_ACCEPT);
        ck_assert_uint_eq(fifo.peer.dio, (uint8_t) ~0xa5);
        ck_assert(!fifo.peer.eoi);
    }
    ck_assert(!fifo4040_tick(&fifo));
    ck_assert_uint_eq(fifo.receive, FIFO4040_RX_ACCEPT);
    // Removing DACO releases NDAC, acknowledging this byte to the synthetic peer.
    store(ControlPort, 0x1d);
    ck_assert(!fifo4040_tick(&fifo));
    ck_assert_uint_eq(fifo.receive, FIFO4040_RX_RELEASE);
    ck_assert(fifo.peer.dav);
    store(ControlPort, 0x1f);
    ck_assert(fifo4040_tick(&fifo));
    ck_assert_uint_eq(fifo.receive, FIFO4040_IDLE);
    ck_assert_uint_eq(fifo.peer.dio, 255);
    ck_assert(fifo.peer.eoi);
    ck_assert(!fifo4040_tick(&fifo));
}
END_TEST

// Keep production regressions in the ordinary CK_NOFORK firmware suite.
Suite* ieee4040_suite(void) {
    Suite* suite = suite_create("IEEE4040");
    TCase* core = tcase_create("Hardware and controller regressions");
    tcase_add_checked_fixture(core, setup, NULL);
    tcase_add_test(core, port_mirrors_preserve_ddr_latches_and_wired_inputs);
    tcase_add_test(core, pa7_edges_are_independent_ddr_composed_and_mirrored);
    tcase_add_test(core, pa7_output_feedback_and_reset_do_not_retain_stale_state);
    tcase_add_test(core, timer_underflow_masks_mirrors_and_acknowledgement);
    tcase_add_test(core, timer_and_edge_sources_have_independent_read_acknowledgement);
    tcase_add_test(core, bootstrap_publishes_id_only_after_final_ram_clear);
    tcase_add_test(core, controller_data_jobs_publish_checksum_and_preserve_unrelated_bytes);
    tcase_add_test(core, controller_bump_seek_and_protected_write_keep_native_header_effects);
    tcase_add_test(core, controller_formatter_headers_success_protection_and_failure);
    tcase_add_test(core, controller_mailbox_slots_and_aliases_do_not_overlap);
    tcase_add_test(core, fifo_offer_consumption_and_abort_conservation);
    tcase_add_test(core, fifo_listener_preserves_flags_and_does_not_finish_on_queued_data);
    suite_add_tcase(suite, core);
    return suite;
}

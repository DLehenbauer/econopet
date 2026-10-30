// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "d64_fixture.h"
#include "ieee/controller4040.h"
#include "ieee/fifo4040.h"
#include "ieee/rom4040.h"

#define BOOT_CYCLES 2000000u
#define HANDSHAKE_BUDGET 100000u

static rom4040_t machine;
static rom4040_bus_t peer = {true, true, true, true, true, 0xff};
static fifo4040_t fifo;
static controller4040_t controller;
static uint8_t image[CONTROLLER4040_IMAGE_SIZE];

// Fail a bounded probe with the exact main-CPU location and hardware fault.
static void require(bool condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "%s (pc=%04x cycles=%u fault=%u)\n", message,
            m6502_pc(&machine.cpu), machine.cycles, machine.fault);
        const rom4040_bus_t out = rom4040_output(&machine);
        fprintf(stderr, "pa=%02x ddr=%02x edge=%u/%u/%u input_atn=%u output nrfd=%u ndac=%u dav=%u\n",
            machine.ports[1][0], machine.ports[1][1], machine.edge_enabled[1],
            machine.edge_positive[1], machine.edge_pending[1], machine.input.atn,
            out.nrfd, out.ndac, out.dav);
        exit(EXIT_FAILURE);
    }
}

// Supply absent-media errors while bringing up the unmodified ROM.
static uint8_t job(void* context, uint8_t command, uint8_t header[8], uint8_t sector[256]) {
    (void) context;
    (void) sector;
    (void) header;
    if ((command & 0xf0) == 0xc0) {
        return 1;
    }
    return 3;
}

// Advance one cycle after applying controller line changes.
static void tick(void) {
    rom4040_input(&machine, peer);
    rom4040_tick(&machine);
    require(machine.fault == ROM4040_FAULT_NONE, "unsupported hardware");
}

// Wait for one physical output level with a deterministic timeout.
static void wait_line(bool dav, bool level) {
    for (unsigned i = 0; i < HANDSHAKE_BUDGET; ++i) {
        const rom4040_bus_t out = rom4040_output(&machine);
        if ((dav ? out.dav : out.ndac) == level) return;
        tick();
    }
    require(false, "handshake timeout");
}

// Perform a controller-to-drive byte transfer without entering DOS routines directly.
static void send(uint8_t byte) {
    peer.dav = true;
    for (unsigned i = 0; ; ++i) {
        const rom4040_bus_t out = rom4040_output(&machine);
        if (out.nrfd && !out.ndac) break;
        require(i < HANDSHAKE_BUDGET, "listener not ready");
        tick();
    }
    peer.dio = (uint8_t) ~byte;
    peer.dav = false;
    wait_line(false, true);
    peer.dav = true;
    wait_line(false, false);
}

// Execute a TALK selection using native ATN interrupts and command decoding.
static void talk_status(void) {
    peer.atn = false;
    peer.nrfd = true;
    peer.ndac = true;
    send(0x48);
    send(0x6f);
    peer.atn = true;
    peer.nrfd = true;
    peer.ndac = false;
}

// Consume one byte at the NRFD transition used by native TLK30/GET.
static uint8_t receive(bool* eoi) {
    wait_line(true, false);
    const rom4040_bus_t out = rom4040_output(&machine);
    const uint8_t byte = (uint8_t) ~out.dio;
    *eoi = !out.eoi;
    peer.nrfd = false;
    for (unsigned i = 0; i < 200; ++i) tick();
    peer.ndac = true;
    wait_line(true, true);
    peer.ndac = false;
    for (unsigned i = 0; i < 200; ++i) tick();
    peer.nrfd = true;
    return byte;
}

// Back controller jobs with a disposable image, without filesystem emulation.
static bool disk_io(void* context, uint32_t offset, uint8_t sector[256], bool write) {
    (void) context;
    require(offset <= sizeof(image) - 256, "sector I/O out of range");
    if (write) memcpy(image + offset, sector, 256);
    else memcpy(sector, image + offset, 256);
    return true;
}

// Run a bounded adapter slice and reject unsupported hardware immediately.
static void fifo_run(unsigned cycles) {
    while (cycles--) {
        fifo4040_tick(&fifo);
        require(machine.fault == ROM4040_FAULT_NONE, "adapter hardware fault");
    }
}

// Pass a byte through synthesized RIOT receive phases in original FIFO order.
static void fifo_send(uint8_t byte, bool atn, bool eoi) {
    fifo4040_receive(&fifo, byte, atn, eoi);
    for (unsigned i = 0; i < HANDSHAKE_BUDGET; ++i) {
        if (fifo4040_tick(&fifo)) return;
        require(machine.fault == ROM4040_FAULT_NONE, "RX hardware fault");
    }
    require(false, "FIFO RX timeout");
}

// Address one logical channel using only IEEE commands.
static void fifo_select(uint8_t primary, uint8_t secondary) {
    fifo_send(primary, true, false);
    fifo_send(secondary, true, false);
    fifo4040_atn_end(&fifo);
    fifo_run(1000);
}

// Interrupt current bus work with native UNLISTEN or UNTALK.
static void fifo_end(uint8_t command) {
    fifo_send(command, true, false);
    fifo4040_atn_end(&fifo);
    fifo_run(10000);
}

// Collect exactly one presented byte and verify that queuing cannot cause GET.
static uint8_t fifo_receive(bool* eoi) {
    uint8_t byte = 0;
    bool ready = false;
    for (unsigned i = 0; i < HANDSHAKE_BUDGET; ++i) {
        if (fifo4040_transmit(&fifo, &byte, eoi)) { ready = true; break; }
        fifo_run(1);
    }
    require(ready, "FIFO TX timeout");
    uint8_t saved[ROM4040_SHARED_SIZE + ROM4040_RIOT_RAM_SIZE];
    memcpy(saved, machine.shared, sizeof(machine.shared));
    memcpy(saved + sizeof(machine.shared), machine.riot_ram, sizeof(machine.riot_ram));
    fifo_run(1000);
    require(memcmp(saved, machine.shared, sizeof(machine.shared)) == 0
        && memcmp(saved + sizeof(machine.shared), machine.riot_ram, sizeof(machine.riot_ram)) == 0,
        "ROM state advanced before actual consumption");
    fifo4040_consumed(&fifo, true);
    for (unsigned i = 0; fifo.acknowledged && i < HANDSHAKE_BUDGET; ++i) fifo_run(1);
    require(!fifo.acknowledged, "ROM did not complete GET");
    return byte;
}

// Read native status text including its wire CR and final EOI.
static void fifo_status(const char* expected) {
    fifo_select(0x48, 0x6f);
    char text[80];
    size_t count = 0;
    bool eoi = false;
    while (!eoi) {
        require(count + 1 < sizeof(text), "FIFO status overflow");
        text[count++] = (char) fifo_receive(&eoi);
    }
    text[count] = 0;
    if (strcmp(text, expected) != 0)
        fprintf(stderr, "status expected=[%s] actual=[%s]\n", expected, text);
    require(strcmp(text, expected) == 0, "native status mismatch");
    fifo_end(0x5f);
}

// Open a file through native LISTEN/OPEN and the filename byte stream.
static void fifo_open(uint8_t channel, const uint8_t* name, size_t length) {
    fifo_select(0x28, 0xf0 | channel);
    for (size_t i = 0; i < length; ++i) fifo_send(name[i], false, i + 1 == length);
    fifo_end(0x3f);
}

// Close a channel with the original ROM's normal teardown path.
static void fifo_close(uint8_t channel) {
    fifo_select(0x28, 0xe0 | channel);
    fifo_end(0x3f);
}

// Execute a DOS command through command-channel PUT and native UNLISTEN.
static void fifo_command(const uint8_t* bytes, size_t count) {
    fifo_select(0x28, 0x6f);
    for (size_t i = 0; i < count; ++i) fifo_send(bytes[i], false, i + 1 == count);
    fifo_end(0x3f);
}

// Exercise the C controller and synthesized FIFO with native filesystem/REL code.
static void adapter_probes(const uint8_t* rom) {
    d64_fixture_layout_t layout;
    uint8_t payload[1000];
    for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t) i;
    require(d64_fixture_empty(&layout, image, sizeof(image)) == NULL, "empty D64 fixture");
    require(d64_fixture_prg(&layout, image, sizeof(image), "PROBE", payload, sizeof(payload),
        NULL, 0) == NULL, "PRG fixture");
    controller = (controller4040_t) {.machine = &machine,
        .disk = {{.io = disk_io, .writable = true, .id = {'0', '0'}}}};
    rom4040_init(&machine, rom, controller4040_job, &controller);
    rom4040_run(&machine, BOOT_CYCLES);
    require(machine.running && !machine.fault, "D64 boot");
    fifo4040_init(&fifo, &machine);
    fifo_status("73,CBM DOS V2,00,00\r");
    fifo_status("00, OK,00,00\r");
    fifo_open(0, (const uint8_t*) "0:PROBE", 7);
    fifo_status("00, OK,00,00\r");
    fifo_select(0x48, 0x60);
    for (size_t i = 0; i < sizeof(payload); ++i) {
        bool eoi;
        require(fifo_receive(&eoi) == payload[i], "PRG byte mismatch");
        require(eoi == (i + 1 == sizeof(payload)), "PRG EOI mismatch");
    }
    fifo_end(0x5f);
    fifo_close(0);
    fifo_open(1, (const uint8_t*) "0:WRITTEN,S,W", 13);
    fifo_select(0x28, 0x61);
    for (size_t i = 0; i < sizeof(payload); ++i)
        fifo_send(payload[i], false, i + 1 == sizeof(payload));
    fifo_end(0x3f);
    fifo_close(1);
    fifo_status("00, OK,00,00\r");
    fifo_open(2, (const uint8_t*) "0:WRITTEN,S,R", 13);
    fifo_select(0x48, 0x62);
    for (size_t i = 0; i < sizeof(payload); ++i) {
        bool eoi;
        require(fifo_receive(&eoi) == payload[i], "write/readback mismatch");
        require(eoi == (i + 1 == sizeof(payload)), "written EOI mismatch");
    }
    fifo_end(0x5f);
    fifo_close(2);
    const uint8_t rel_name[] = {'0', ':', 'R', 'E', 'L', ',', 'L', ',', 17};
    fifo_open(2, rel_name, sizeof(rel_name));
    fifo_status("00, OK,00,00\r");
    fifo_select(0x28, 0x62);
    for (unsigned i = 0; i < 17; ++i) fifo_send((uint8_t) ('A' + i), false, i == 16);
    fifo_end(0x3f);
    fifo_close(2);
    fifo_open(2, (const uint8_t*) "0:REL", 5);
    fifo_status("00, OK,00,00\r");
    fifo_select(0x48, 0x62);
    for (unsigned i = 0; i < 17; ++i) {
        bool eoi;
        require(fifo_receive(&eoi) == 'A' + i, "REL record mismatch");
        require(eoi == (i == 16), "REL EOI mismatch");
    }
    fifo_end(0x5f);
    fifo_close(2);
    // Position a REL record with embedded binary command bytes, not a C parser.
    const uint8_t position[] = {'P', 0x62, 1, 0, 5};
    fifo_open(2, (const uint8_t*) "0:REL", 5);
    fifo_command(position, sizeof(position));
    fifo_select(0x48, 0x62);
    for (unsigned i = 4; i < 17; ++i) {
        bool eoi;
        require(fifo_receive(&eoi) == 'A' + i, "REL position mismatch");
        require(eoi == (i == 16), "positioned REL EOI mismatch");
    }
    fifo_end(0x5f);
    fifo_close(2);
    // Directory construction and EOI are wholly ROM-owned.
    fifo_open(0, (const uint8_t*) "$0", 2);
    fifo_select(0x48, 0x60);
    uint8_t directory[1024];
    size_t length = 0;
    bool eoi = false;
    while (!eoi) {
        require(length < sizeof(directory), "directory overflow");
        directory[length++] = fifo_receive(&eoi);
    }
    require(length > 100 && directory[0] == 1 && directory[1] == 4, "directory BASIC stream");
    fifo_end(0x5f);
    fifo_close(0);
    // Read shared RAM over M-R to establish that no filesystem facade intervenes.
    fifo_open(2, (const uint8_t*) "#", 1);
    const uint8_t memory_read[] = {'M', '-', 'R', 0x00, 0x10, 1};
    fifo_command(memory_read, sizeof(memory_read));
    fifo_select(0x48, 0x6f);
    const uint8_t id = fifo_receive(&eoi);
    require(id == 0x0f && !eoi, "single-byte M-R must retain native non-EOI");
    fifo_end(0x5f);
    fifo_close(2);
    // NEW executes the stock formatter through the explicitly recognized C job.
    fifo_command((const uint8_t*) "N0:FORMATTED,42", 15);
    fifo_status("00, OK,00,00\r");
    uint32_t bam_offset;
    require(controller4040_offset(18, 0, &bam_offset), "BAM offset");
    require(memcmp(image + bam_offset + 0x90, "FORMATTED", 9) == 0, "NEW label mismatch");
    require(image[bam_offset + 0xa2] == '4' && image[bam_offset + 0xa3] == '2', "NEW ID mismatch");
    printf("adapter: status,1000-byte read,write/readback,REL-create/read/position,directory,M-R,NEW passed presented=%u consumed=%u jobs=%u\n",
        fifo.presented, fifo.consumed, machine.jobs);
}

// Run boot, status and idle throughput probes against an external original ROM.
int main(int argc, char** argv) {
    require(argc == 2, "usage: rom4040_probe dos4040.bin");
    uint8_t rom[ROM4040_ROM_SIZE];
    FILE* file = fopen(argv[1], "rb");
    require(file != NULL, "cannot open ROM");
    require(fread(rom, 1, sizeof(rom), file) == sizeof(rom)
        && fgetc(file) == EOF, "ROM must be exactly 12288 bytes");
    require(fclose(file) == 0, "cannot close ROM");
    rom4040_init(&machine, rom, job, NULL);
    const clock_t start = clock();
    rom4040_run(&machine, BOOT_CYCLES);
    require(machine.fault == ROM4040_FAULT_NONE && machine.running, "ROM boot failed");
    printf("state_bytes=%zu native_ram=%u boot_pc=%04x host_cycles_per_second=%.0f\n",
        sizeof(machine), ROM4040_SHARED_SIZE + ROM4040_RIOT_RAM_SIZE,
        m6502_pc(&machine.cpu), BOOT_CYCLES * (double) CLOCKS_PER_SEC / (clock() - start));
    talk_status();
    char status[80];
    size_t count = 0;
    bool eoi = false;
    while (!eoi) {
        require(count + 1 < sizeof(status), "status overflow");
        status[count++] = (char) receive(&eoi);
    }
    status[count] = '\0';
    require(count > 3 && status[0] == '7' && status[1] == '3', "power-on status mismatch");
    printf("power_on_status=%s eoi_at=%zu jobs=%u\n", status, count - 1, machine.jobs);
    adapter_probes(rom);
    return EXIT_SUCCESS;
}

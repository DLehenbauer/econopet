// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "Vieee.h"

extern "C" {
#include "driver.h"
#include "d64_fixture.h"
#include "ieee/ieee_drive.h"
#include "test_support.h"
}

namespace {
Vieee board;
uint64_t clocks;
uint8_t orb = 0xff;

// Reject incomplete bounded handshakes, even with NDEBUG accidentally defined.
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s (FPGA clocks=%llu)\n", message,
            static_cast<unsigned long long>(clocks));
        std::exit(EXIT_FAILURE);
    }
}

// Clock the actual production IEEE module, without a behavioral FIFO model.
void tick(unsigned count = 1) {
    while (count--) {
        board.wb_clock_i = 0;
        board.eval();
        board.wb_clock_i = 1;
        board.eval();
        ++clocks;
    }
}

// Drive one correctly phased PET register transaction through snoop/injection.
uint8_t cpu(unsigned device, uint8_t reg, uint8_t data, bool write = true) {
    board.rs_i = reg;
    board.cpu_data_i = data;
    board.cpu_we_i = write;
    board.cpu_be_i = 1;
    board.cpu_addr_strobe_i = 1;
    tick();
    board.cpu_addr_strobe_i = 0;
    board.pia1_cs_i = device == 1;
    board.pia2_cs_i = device == 2;
    board.via_cs_i = device == 3;
    tick(5);
    const uint8_t result = board.cpu_data_oe ? board.cpu_data_o : 0xff;
    board.cpu_data_strobe_i = 1;
    tick();
    board.cpu_data_strobe_i = 0;
    tick();
    board.cpu_be_i = 0;
    board.pia1_cs_i = board.pia2_cs_i = board.via_cs_i = 0;
    tick(2);
    return result;
}

// Change PET ATN or NRFD via the VIA output latch.
void via(uint8_t bit, bool high) {
    orb = high ? orb | bit : orb & static_cast<uint8_t>(~bit);
    cpu(3, 0, orb);
}

// Poll a composed physical line while cooperatively servicing core-0 firmware.
void wait(uint8_t mask, uint8_t expected) {
    for (unsigned i = 0; i < 1000; ++i) {
        if ((cpu(3, 0, 0, false) & mask) == expected) return;
        ieee_drive_task();
    }
    require(false, "physical IEEE handshake timed out");
}

// Send an ATN command through the actual RX FIFO and synthesized RIOT adapter.
void send(uint8_t byte) {
    cpu(2, 1, 0x3c);
    via(2, true);
    cpu(2, 2, static_cast<uint8_t>(~byte));
    wait(1, 0);
    cpu(2, 3, 0x34);
    wait(1, 1);
    cpu(2, 3, 0x3c);
    wait(1, 0);
    cpu(2, 2, 0xff);
}

// Address native status, then allow its ATN-release token to reach the ROM.
void status_select() {
    via(4, false);
    send(0x48);
    send(0x6f);
    via(4, true);
    for (unsigned i = 0; i < 8; ++i) ieee_drive_task();
}

// Select any ROM channel through the physical transport's primary/secondary path.
void select(uint8_t primary, uint8_t secondary) {
    via(4, false);
    send(primary);
    send(secondary);
    via(4, true);
    for (unsigned i = 0; i < 8; ++i) ieee_drive_task();
}

// End the current native role with an ordered ATN command.
void end(uint8_t command) {
    via(4, false);
    send(command);
    via(4, true);
    for (unsigned i = 0; i < 8; ++i) ieee_drive_task();
}

// Receive one PET byte, independently sampling physical DIO and EOI.
uint8_t receive(bool& eoi) {
    cpu(2, 1, 0x34);
    via(2, true);
    wait(0x80, 0);
    via(2, false);
    const uint8_t data = static_cast<uint8_t>(~cpu(2, 0, 0, false));
    eoi = !(cpu(1, 0, 0, false) & 0x40);
    cpu(2, 1, 0x3c);
    wait(0x80, 0x80);
    cpu(2, 1, 0x34);
    via(2, true);
    return data;
}

// Retain the native power-on line through a queued-but-unread ATN interruption.
void status_probe() {
    status_select();
    cpu(2, 1, 0x34);
    via(2, true);
    wait(0x80, 0);
    for (unsigned i = 0; i < 10; ++i) ieee_drive_task();
    // Interrupt without NRFD assertion. No ROM GET may have occurred.
    via(4, false);
    send(0x5f);
    via(4, true);
    for (unsigned i = 0; i < 8; ++i) ieee_drive_task();
    status_select();
    std::string text;
    bool eoi = false;
    while (!eoi && text.size() < 80) text += static_cast<char>(receive(eoi));
    require(text == "73,CBM DOS V2,00,00\r" && eoi,
        "queued/unread status was consumed or corrupted");
    // The CR's GET must clear status, even with an immediate new ATN selection.
    status_select();
    text.clear();
    eoi = false;
    while (!eoi && text.size() < 80) text += static_cast<char>(receive(eoi));
    require(text == "00, OK,00,00\r" && eoi, "consumed status did not clear");
}

// Verify a multi-sector LOAD, interposed status and a partly consumed FIFO abort.
void load_probe(const std::vector<uint8_t>& payload) {
    select(0x28, 0xf0);
    const std::string name = "0:PROBE";
    for (size_t i = 0; i < name.size(); ++i) {
        cpu(1, 1, i + 1 == name.size() ? 0x34 : 0x3c);
        send(static_cast<uint8_t>(name[i]));
    }
    cpu(1, 1, 0x3c);
    end(0x3f);
    select(0x48, 0x60);
    bool eoi = false;
    for (size_t i = 0; i < payload.size(); ++i) {
        require(receive(eoi) == payload[i], "physical multi-sector LOAD byte mismatch");
        require(eoi == (i + 1 == payload.size()), "physical LOAD EOI mismatch");
        if (i == 127) {
            // GET was applied, but no following byte may be committed by enqueue.
            status_select();
            std::string status;
            bool status_eoi = false;
            while (!status_eoi && status.size() < 80)
                status += static_cast<char>(receive(status_eoi));
            require(status == "00, OK,00,00\r" && status_eoi, "interposed status corrupted");
            select(0x48, 0x60);
        }
    }
    end(0x5f);
    select(0x28, 0xe0);
    end(0x3f);
}
}

// Access Wishbone through the same firmware register API, not a fake FIFO.
extern "C" uint8_t spi_read_at(uint32_t address) {
    board.wbp_addr_i = address;
    board.wbp_we_i = 0;
    board.wbp_cycle_i = board.wbp_strobe_i = board.wbp_sel_i = 1;
    tick();
    const uint8_t value = board.wbp_data_o;
    board.wbp_cycle_i = board.wbp_strobe_i = board.wbp_sel_i = 0;
    tick(2);
    return value;
}

// Write one real Wishbone register, including asynchronous FIFO pop/flush effects.
extern "C" uint8_t spi_write_at(uint32_t address, uint8_t value) {
    board.wbp_addr_i = address;
    board.wbp_data_i = value;
    board.wbp_we_i = 1;
    board.wbp_cycle_i = board.wbp_strobe_i = board.wbp_sel_i = 1;
    tick();
    board.wbp_cycle_i = board.wbp_strobe_i = board.wbp_sel_i = board.wbp_we_i = 0;
    tick(2);
    return 0;
}

// Execute production firmware against the RTL with externally supplied ROM chips.
int main(int argc, char** argv) {
    require(argc == 2, "usage: fifo4040_probe external-rom-directory");
    for (const char* name : {"901468-14.bin", "901468-15.bin", "901468-16.bin"}) {
        std::ifstream input(std::string(argv[1]) + "/" + name, std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        require(bytes.size() == 4096, "missing external ROM chip");
        test_register_binary_file((std::string("/roms/") + name).c_str(),
            bytes.data(), bytes.size(), false);
    }
    std::vector<uint8_t> image(174848);
    std::vector<uint8_t> payload(1000);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i);
    d64_fixture_layout_t layout;
    require(d64_fixture_empty(&layout, image.data(), image.size()) == nullptr, "D64 fixture");
    require(d64_fixture_prg(&layout, image.data(), image.size(), "PROBE",
        payload.data(), payload.size(), nullptr, 0) == nullptr, "PRG fixture");
    test_register_binary_file("/disks/probe.d64", image.data(), image.size(), true);
    board.ifc_i = 1;
    tick(2);
    board.ifc_i = 0;
    board.diag_i = 1;
    ieee_drive_init();
    require(ieee_drive_mount(0, "probe.d64"), "production mount failed");
    cpu(2, 1, 0x3c);
    cpu(2, 3, 0x3c);
    cpu(1, 1, 0x3c);
    status_probe();
    load_probe(payload);
    ieee_drive_unmount_all();
    test_clear_files();
    std::puts("RTL FIFO: unread ATN abort, actual GET, CR/EOI, status reset, "
        "1000-byte LOAD with interposed status passed");
    return EXIT_SUCCESS;
}

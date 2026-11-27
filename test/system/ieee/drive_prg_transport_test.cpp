// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "framework/firmware_test.h"
#include "ieee/fixture.h"
#include "ieee/trace.h"
#include "target_diag.h"

namespace {
using econopet::CpuAddress;
using econopet::SramAddress;
using test_asm::imm;
using test_asm::zp;
using namespace ieee_test;

/** Resolve a linked symbol after reading the CC65 label table once. */
CpuAddress linked_symbol(const std::string& name) {
    static const auto symbols = [] {
        std::ifstream file(ECONOPET_DRIVE_TEST_LABELS);
        if (!file) {
            throw std::runtime_error("cannot open Drive Test linker labels");
        }
        std::map<std::string, CpuAddress> result;
        std::string line;
        while (std::getline(file, line)) {
            std::istringstream fields(line);
            std::string kind, symbol;
            uint32_t address;
            if (fields >> kind >> std::hex >> address >> symbol && kind == "al") {
                result.emplace(symbol, CpuAddress{address});
            }
        }
        return result;
    }();
    const auto entry = symbols.find("." + name);
    if (entry == symbols.end()) {
        throw std::runtime_error("missing Drive Test entry " + name);
    }
    return entry->second;
}

/** Resolve a C entry using CC65's leading underscore convention. */
CpuAddress linked_entry(const std::string& name) { return linked_symbol("_" + name); }

/** Validated load address and contents shared by the two installation adapters. */
struct PrgImage {
    CpuAddress start;
    std::vector<uint8_t> contents;
};

/** Read and validate the compiled base-RAM PRG once for this test process. */
const PrgImage& read_prg() {
    static const auto image = [] {
        std::ifstream file(ECONOPET_DRIVE_TEST_PRG, std::ios::binary);
        if (!file) {
            throw std::runtime_error("cannot open Drive Test PRG");
        }
        const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
        if (bytes.size() < 3) {
            throw std::runtime_error("empty Drive Test PRG");
        }
        const uint16_t start = uint16_t{bytes[0]} | (uint16_t{bytes[1]} << 8);
        if (start != 0x0401 || bytes.size() - 2 > TARGET_DIAG_MAILBOX_ADDR - start) {
            throw std::runtime_error("Drive Test payload is outside base RAM");
        }
        return PrgImage{CpuAddress{start}, {bytes.begin() + 2, bytes.end()}};
    }();
    return image;
}

/** Register the compiled PRG in a ProgramBuilder-owned memory image. */
void install_prg(test_asm::ProgramBuilder<test_asm::Mos6502>& test) {
    const auto& image = read_prg();
    const auto payload = test.data("compiled Drive Test", image.start, image.contents.size());
    test.lower_bank("compiled Drive Test").initialize(payload, image.contents);
}

/** Install the compiled PRG directly and initialize BSS when bypassing CC65 startup. */
void install_prg(System& system) {
    const auto& image = read_prg();
    for (size_t i = 0; i < image.contents.size(); ++i) {
        system.poke(SramAddress{image.start.value() + i}, image.contents[i]);
    }

    // Zero the linked BSS segment below the $7DF0..$7FEF CC65 software stack when bypassing CC65 startup.
    constexpr uint16_t StackBase = TARGET_DIAG_MAILBOX_ADDR - 0x0200u;
    const auto bss = linked_entry("_BSS_RUN__").value();
    const auto bss_size = linked_entry("_BSS_SIZE__").value();
    if (bss < image.start.value() || bss + bss_size > StackBase) {
        throw std::runtime_error("Drive Test BSS is outside resident RAM");
    }
    for (size_t i = 0; i < bss_size; ++i) {
        system.poke(SramAddress{bss + i}, 0);
    }
}
} // namespace

/** Exercise the linked PET-side transport and stream helpers against production firmware and FPGA I/O. */
TEST_F(FirmwareTest, DrivePrgTransportStreamsSequentialFileWithExactEoi) {
    // Mount a 4-byte sequential file on device 8 and install the compiled PRG into the 6502 test harness.
    const std::vector<uint8_t> expected{0x00, 0x42, 0xff, 0x7e};
    firmware.mount(disk_fixture::D64{}.seq("BASIC", expected));
    system.cpu().assert_reset();
    KernalFixture rom(system, test_rom::Set::Rom4);
    auto wire = system.trace_ieee();
    auto test = system.program6502();
    install_prg(test);

    const auto match = test.byte("match");
    const auto init = linked_entry("drive_bus_init");
    const auto listen = linked_entry("drive_listen");
    const auto secondary = linked_entry("drive_secondary");
    const auto send_bytes = linked_entry("drive_send_bytes");
    const auto unlisten = linked_entry("drive_unlisten");
    const auto talk = linked_entry("drive_talk");
    const auto talk_secondary = linked_entry("drive_talk_secondary");
    const auto recv_check = linked_entry("drive_recv_check");
    const auto untalk = linked_entry("drive_untalk");
    const auto payload_buf = linked_entry("payload");
    const auto bad_offset = linked_entry("drive_bad_offset");
    const std::string filename{"0:BASIC,S,R"};

    // Call the linked assembly OPEN, stream send, TALK, and stream receive/EOI verifier routines.
    const auto result =
        test.within(15'000'000).every(1000, ServiceTiming::AfterSlice, firmware.service_adapter()).run([&](auto& p) {
            rom.init_ieee(p);
            for (size_t i = 0; i < filename.size(); ++i) {
                p.lda(imm(static_cast<uint8_t>(filename[i]))).sta(payload_buf + i);
            }
            p.jsr(init)
                .lda(imm(8))
                .sta(zp(KernalWorkspace::Device))
                .jsr(listen)
                .lda(imm(0xf2))
                .jsr(secondary)
                .lda(imm(static_cast<uint8_t>(filename.size())))
                .ldx(imm(0))
                .jsr(send_bytes)
                .jsr(unlisten);
            for (size_t i = 0; i < expected.size(); ++i) {
                p.lda(imm(expected[i])).sta(payload_buf + i);
            }
            p.jsr(talk)
                .lda(imm(0x62))
                .jsr(talk_secondary)
                .lda(imm(static_cast<uint8_t>(expected.size())))
                .sta(bad_offset)
                .lda(imm(0))
                .sta(bad_offset + 1)
                .lda(imm(static_cast<uint8_t>(expected.size())))
                .ldx(imm(0))
                .jsr(recv_check)
                .sta(match)
                .jsr(untalk);
        });

    EXPECT_EQ(result[match], 1);

    // Verify the observed IEEE-488 bus trace matches exact command and final-byte EOI placement.
    std::vector<Record> records{command(Listen(Device{8})), command(Open(Channel{2}))};
    for (size_t i = 0; i < filename.size(); ++i) {
        records.push_back(data(static_cast<uint8_t>(filename[i]), i + 1 == filename.size() ? Eoi::Last : Eoi::More));
    }
    records.push_back(command(Unlisten));
    records.push_back(command(Talk(Device{8})));
    records.push_back(command(Secondary(Channel{2})));
    for (size_t i = 0; i < expected.size(); ++i) {
        records.push_back(ieee_test::data(expected[i], i + 1 == expected.size() ? Eoi::Last : Eoi::More));
    }
    records.push_back(command(Untalk));
    require_sequence(wire.records(), records, "compiled Drive Test transport");
}

/** The linked multi-sector verifier must accept distinct regions and reject a replayed first region. */
TEST_F(FirmwareTest, DrivePrgDetectsReplayedPayloadRegion) {
    constexpr uint8_t Seed = 0x42;
    constexpr size_t RegionSize = 256;
    constexpr size_t Length = 2 * RegionSize;
    std::vector<uint8_t> expected(Length);
    std::vector<uint8_t> replayed(Length);
    for (size_t i = 0; i < RegionSize; ++i) {
        const auto byte = static_cast<uint8_t>(Seed + 13 * i);
        expected[i] = replayed[i] = replayed[i + RegionSize] = byte;
        expected[i + RegionSize] = byte ^ 1;
    }
    firmware.mount(disk_fixture::D64{}.seq("GOOD", expected).seq("REPLAY", replayed));
    system.cpu().assert_reset();
    KernalFixture rom(system, test_rom::Set::Rom4);
    // Reserve a larger caller above the resident PRG for the two complete read sequences.
    auto test = system.program6502(test_asm::Layout::Pet, {.code = test_asm::MemoryRegion{CpuAddress{0x6000}, 0x200}});
    install_prg(test);
    const auto good = test.byte("distinct regions accepted");
    const auto replay = test.byte("replayed region accepted");
    const auto payload_buf = linked_entry("payload");
    const auto bad_offset = linked_entry("drive_bad_offset");
    const auto stack = linked_symbol("sp");
    const auto pushax = linked_symbol("pushax");
    const auto pusha = linked_symbol("pusha");

    const auto result =
        test.within(30'000'000).every(1000, ServiceTiming::AfterSlice, firmware.service_adapter()).run([&](auto& p) {
            rom.init_ieee(p);
            p.jsr(linked_entry("drive_bus_init")).lda(imm(8)).sta(zp(KernalWorkspace::Device));

            for (const bool replay_file : {false, true}) {
                // OPEN through the linked transport, then construct its expected stream using the real C generator.
                p.jsr(linked_entry("drive_listen")).lda(imm(0xf2)).jsr(linked_entry("drive_secondary"));
                for (const uint8_t byte : std::string{replay_file ? "0:REPLAY,S,R" : "0:GOOD,S,R"}) {
                    p.lda(imm(byte)).jsr(linked_entry("drive_ciout"));
                }
                p.jsr(linked_entry("drive_unlisten"));

                // Initialize CC65's software stack and push destination, seed, and length (the final flag uses A).
                p.lda(imm(0)).sta(zp(stack)).lda(imm(0x7e)).sta(zp(stack + 1));
                p.lda(imm(static_cast<uint8_t>(payload_buf.value())))
                    .ldx(imm(static_cast<uint8_t>(payload_buf.value() >> 8)))
                    .jsr(pushax)
                    .lda(imm(Seed))
                    .jsr(pusha)
                    .lda(imm(0))
                    .ldx(imm(Length >> 8))
                    .jsr(pushax)
                    .lda(imm(0))
                    .jsr(linked_entry("drive_fill_payload"));

                // Compare the complete stream and EOI using the same linked verifier as the qualification runner.
                p.jsr(linked_entry("drive_talk"))
                    .lda(imm(0x62))
                    .jsr(linked_entry("drive_talk_secondary"))
                    .lda(imm(0))
                    .sta(bad_offset)
                    .lda(imm(Length >> 8))
                    .sta(bad_offset + 1)
                    .lda(imm(0))
                    .ldx(imm(Length >> 8))
                    .jsr(linked_entry("drive_recv_check"))
                    .sta(replay_file ? replay : good)
                    .jsr(linked_entry("drive_untalk"));
            }
        });

    EXPECT_EQ(result[good], 1);
    EXPECT_EQ(result[replay], 0);
    EXPECT_EQ(system.peek(SramAddress{bad_offset.value()}), 0);
    EXPECT_EQ(system.peek(SramAddress{bad_offset.value() + 1}), 1);
    EXPECT_EQ(system.peek(SramAddress{linked_entry("drive_bad_expected").value()}), Seed ^ 1);
    EXPECT_EQ(system.peek(SramAddress{linked_entry("drive_bad_actual").value()}), Seed);
    EXPECT_EQ(system.peek(SramAddress{linked_entry("drive_bad_eoi").value()}), 0);
}

/** ROM 4 UNTALK must interrupt a talker that has already asserted DAV. */
TEST_F(FirmwareTest, DrivePrgUntalkInterruptsPendingTalkerByte) {
    // Open a 4-byte file, read the first byte, then let the talker assert DAV for byte 2 before UNTALK.
    firmware.mount(disk_fixture::D64{}.seq("BASIC", {0x41, 0x42, 0x43, 0x44}));
    system.cpu().assert_reset();
    KernalFixture rom(system, test_rom::Set::Rom4);
    auto wire = system.trace_ieee();
    auto test = system.program6502();
    install_prg(test);
    const auto received = test.byte("first byte");
    const auto completed = test.byte("untalk completed");
    const auto init = linked_entry("drive_bus_init");
    const auto listen = linked_entry("drive_listen");
    const auto secondary = linked_entry("drive_secondary");
    const auto ciout = linked_entry("drive_ciout");
    const auto unlisten = linked_entry("drive_unlisten");
    const auto talk = linked_entry("drive_talk");
    const auto talk_secondary = linked_entry("drive_talk_secondary");
    const auto acptr = linked_entry("drive_acptr");
    const auto untalk = linked_entry("drive_untalk");
    constexpr CpuAddress ViaPortB{0xe840};
    constexpr uint8_t Nrfd = 0x02;
    constexpr std::array<uint8_t, 2> ReleaseNrfd{0x09, Nrfd};
    constexpr std::array<uint8_t, 5> WaitForDav{0x2c, 0x40, 0xe8, 0x30, 0xfb};

    const auto result =
        test.within(12'000'000).every(1000, ServiceTiming::AfterSlice, firmware.service_adapter()).run([&](auto& p) {
            rom.init_ieee(p);
            p.jsr(init).lda(imm(8)).sta(zp(KernalWorkspace::Device)).jsr(listen).lda(imm(0xf2)).jsr(secondary);
            for (const uint8_t byte : std::string{"0:BASIC,S,R"}) {
                p.lda(imm(byte)).jsr(ciout);
            }
            p.jsr(unlisten).jsr(talk).lda(imm(0x62)).jsr(talk_secondary).jsr(acptr).sta(received);

            // Release NRFD and wait for DAV low while NDAC remains asserted, then invoke drive_untalk.
            p.lda(ViaPortB).raw_bytes(ReleaseNrfd).sta(ViaPortB).raw_bytes(WaitForDav);
            p.jsr(untalk).lda(imm(1)).sta(completed);
        });

    EXPECT_EQ(result[received], 0x41);
    EXPECT_EQ(result[completed], 1);
    EXPECT_NE(std::find(wire.records().begin(), wire.records().end(), command(Untalk)), wire.records().end());
}

/** A missing talker must terminate with a diagnostic and leave the bus released. */
TEST_F(FirmwareTest, DrivePrgTransportTimesOutWithoutTalker) {
    // Install the linked PRG with no virtual drives mounted so drive_acptr stalls waiting for DAV low.
    constexpr uint16_t Mailbox = TARGET_DIAG_MAILBOX_ADDR;
    system.cpu().assert_reset();
    system.load_rom_set(test_rom::Set::Rom4);
    install_prg(system);
    for (unsigned i = 0; i < TARGET_DIAG_MAILBOX_SIZE; ++i) {
        system.poke(SramAddress{Mailbox + i}, 0);
    }

    // Shorten the X/Y inner loop immediates in wait_via so simulation reaches the timeout path quickly.
    const auto wait = linked_entry("drive_test_wait_via");
    ASSERT_EQ(system.peek(SramAddress{wait.value() + 6}), 0xa2);
    ASSERT_EQ(system.peek(SramAddress{wait.value() + 8}), 0xa0);
    system.poke(SramAddress{wait.value() + 7}, 2);
    system.poke(SramAddress{wait.value() + 9}, 2);
    test_asm::Scenario scenario(system);
    const auto code = scenario.reserve("timeout caller", test_asm::RegionKind::Code, CpuAddress{0x0300}, 0x40);
    test_asm::mos6502::Program caller(scenario, code);
    caller.sei()
        .jsr(linked_entry("drive_bus_init"))
        .lda(imm(1))
        .sta(linked_entry("drive_test_wait_budget"))
        .lda(imm(static_cast<uint8_t>(Mailbox)))
        .sta(zp(CpuAddress{0x55}))
        .lda(imm(static_cast<uint8_t>(Mailbox >> 8)))
        .sta(zp(CpuAddress{0x56}))
        .jsr(linked_entry("drive_acptr"));
    caller.install();
    system.cpu(CPU_SOFT_6502).start(code.start());
    system.execution()
        .within(3'000'000)
        .until([&](const System& board) {
            return board.peek(SramAddress{TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE)}) == TARGET_DIAG_STATE_FAILED;
        })
        .run("Drive Test missing talker timeout");

    // Verify DT_ERR_TIMEOUT (1) at stage 4 in the $7FF0 mailbox and confirm all IEEE lines returned high.
    EXPECT_EQ(system.peek(SramAddress{TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC0)}), TARGET_DIAG_MAGIC_0);
    EXPECT_EQ(system.peek(SramAddress{TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC1)}), TARGET_DIAG_MAGIC_1);
    EXPECT_EQ(system.peek(SramAddress{TARGET_DIAG_ADDR(TARGET_DIAG_OFF_DETAIL_ID)}), 1);
    EXPECT_EQ(system.peek(SramAddress{TARGET_DIAG_ADDR(TARGET_DIAG_OFF_ACTUAL)}), 4);
    EXPECT_NE(system.peek(SramAddress{linked_entry("drive_timeout_bus").value()}) & 0x80, 0);
    const auto pins = system.snapshot();
    EXPECT_TRUE(pins.ieee_bus_atn_no);
    EXPECT_TRUE(pins.ieee_bus_dav_no);
    EXPECT_TRUE(pins.ieee_cpu_nrfd_no);
    EXPECT_TRUE(pins.ieee_cpu_ndac_no);
    EXPECT_TRUE(pins.ieee_bus_eoi_no);
}

/** Start the linked program and verify its initial format dispatch. */
TEST_F(FirmwareTest, DrivePrgStartsAndEntersFirstFormat) {
    // Mount eight blank D64/D80/D82 containers matching the qualification slot layout.
    constexpr uint16_t Mailbox = TARGET_DIAG_MAILBOX_ADDR;
    constexpr std::array<size_t, 8> sizes{174848, 174848, 533248, 533248, 1066496, 1066496, 174848, 174848};
    constexpr std::array<const char*, 8> names{"u8d0.d64",  "u8d1.d64",  "u9d0.d80",  "u9d1.d80",
                                               "u10d0.d82", "u10d1.d82", "u11d0.d64", "u11d1.d64"};
    for (size_t i = 0; i < sizes.size(); ++i) {
        firmware.mount(std::vector<uint8_t>(sizes[i], static_cast<uint8_t>(0x31 + i)), names[i],
                       static_cast<unsigned>(i));
    }

    // Initialize MEMSIZ ($8000), run CINT ($E000), and jump into CC65 STARTUP ($040D) until check #1 completes.
    auto wire = system.trace_ieee();
    system.cpu().assert_reset();
    system.load_rom_set(test_rom::Set::Rom4);
    install_prg(system);
    for (unsigned i = 0; i < TARGET_DIAG_MAILBOX_SIZE; ++i) {
        system.poke(SramAddress{Mailbox + i}, 0);
    }
    system.poke(SramAddress{0x34}, 0x00);
    system.poke(SramAddress{0x35}, 0x80);
    constexpr std::array<uint8_t, 6> startup{0x20, 0x00, 0xe0, 0x4c, 0x0d, 0x04};
    for (size_t i = 0; i < startup.size(); ++i) {
        system.poke(SramAddress{0x0300 + i}, startup[i]);
    }
    system.cpu(CPU_SOFT_6502).start(CpuAddress{0x0300});
    system.execution()
        .within(30'000'000)
        .every(8000, ServiceTiming::AfterSlice, firmware.service_adapter())
        .until([&](const System& board) {
            const auto state = board.peek(SramAddress{TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE)});
            const auto completed = board.peek(SramAddress{TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_LO)});
            return state == TARGET_DIAG_STATE_FAILED || (state == TARGET_DIAG_STATE_RUNNING && completed >= 1);
        })
        .run("Drive Test first format");

    // Verify standalone startup clamped MEMSIZ/FRETOP below $7FF0 and published check #1 cleanly.
    EXPECT_EQ(system.peek(SramAddress{0x34}), static_cast<uint8_t>(Mailbox));
    EXPECT_EQ(system.peek(SramAddress{0x35}), static_cast<uint8_t>(Mailbox >> 8));
    EXPECT_EQ(system.peek(SramAddress{0x30}), static_cast<uint8_t>(Mailbox));
    EXPECT_EQ(system.peek(SramAddress{0x31}), static_cast<uint8_t>(Mailbox >> 8));
    EXPECT_EQ(system.peek(SramAddress{TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC0)}), TARGET_DIAG_MAGIC_0);
    EXPECT_EQ(system.peek(SramAddress{TARGET_DIAG_ADDR(TARGET_DIAG_OFF_MAGIC1)}), TARGET_DIAG_MAGIC_1);
    EXPECT_EQ(system.peek(SramAddress{TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE)}), TARGET_DIAG_STATE_RUNNING);
    EXPECT_EQ(system.peek(SramAddress{TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_LO)}), 1);
    EXPECT_EQ(system.peek(SramAddress{TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SEQ)}) & 1u, 0u);

    std::vector<Record> format{command(Listen(Device{8})), command(Open(Channel{15})), command(Unlisten),
                               command(Listen(Device{8})), command(Secondary(Channel{15}))};
    const std::string command_bytes = "N0:U8D0,80";
    for (size_t i = 0; i < command_bytes.size(); ++i) {
        format.push_back(
            data(static_cast<uint8_t>(command_bytes[i]), i + 1 == command_bytes.size() ? Eoi::Last : Eoi::More));
    }
    format.push_back(command(Unlisten));
    ASSERT_GE(wire.records().size(), format.size());
    EXPECT_TRUE(std::equal(format.begin(), format.end(), wire.records().begin()));
}

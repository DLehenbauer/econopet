// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

extern "C" {
#include "ieee/controller4040.h"
#include "ieee/fifo4040.h"
#include "ieee/rom4040.h"
}

namespace {
constexpr uint16_t RomStart = 0xd000;
constexpr uint16_t ResetVector = 0xfffc;
constexpr uint16_t Results = 0x3000;
constexpr uint16_t DataPort = 0x0200, DataDdr = 0x0201;
constexpr uint16_t OutputPort = 0x0202, OutputDdr = 0x0203;
constexpr uint16_t ControlPort = 0x0280, ControlDdr = 0x0281;
constexpr uint16_t DevicePort = 0x0282, DeviceDdr = 0x0283;
constexpr uint16_t EdgePositiveEnabled = 0x0287;
constexpr uint16_t EdgePositiveDisabled = 0x0285;
constexpr uint16_t EdgeFlags = 0x0285;
constexpr uint8_t LdaImmediate = 0xa9, LdaAbsolute = 0xad;
constexpr uint8_t StaAbsolute = 0x8d, JumpAbsolute = 0x4c;
constexpr uint8_t CommandRead = 0x80, CommandWrite = 0x90, CommandVerify = 0xa0;
constexpr uint8_t CommandSeek = 0xb0, CommandBump = 0xc0, CommandJump = 0xd0;
constexpr uint8_t CommandExecute = 0xe0, ControllerDriveError = 13;
constexpr uint8_t ControllerOk = 1, HeaderError = 2, NoSync = 3;
constexpr uint8_t DataError = 5, VerifyError = 7, WriteProtected = 8, IdMismatch = 11;
constexpr unsigned InstructionBudget = 10000;
constexpr uint16_t RiotBases[] = {0x0200, 0x0280};
constexpr uint16_t TimerDivisors[] = {1, 8, 64, 1024};
constexpr uint16_t ControllerId = 0x1000, JobBase = 0x1003;
constexpr uint16_t HeaderBase = 0x1021, FirstBuffer = 0x1100;
constexpr uint16_t LastDiagnosticClear = 0x4000;
constexpr std::array<uint8_t, 9> ControllerWait{
    0x78, 0xa9, 0x00, 0x8d, 0x03, 0x04, 0x4c, 0x04, 0xfc};

// Execute small test programs through the actual CPU bus, never private memory handlers.
class Hardware4040Test : public testing::Test {
protected:
    std::array<uint8_t, ROM4040_ROM_SIZE> rom{};
    rom4040_t machine{};
    std::vector<uint8_t> program;

    // Encode an absolute bus access in the instruction stream.
    void absolute(uint8_t opcode, uint16_t address) {
        program.insert(program.end(), {opcode, static_cast<uint8_t>(address),
            static_cast<uint8_t>(address >> 8)});
    }

    // Write one register using normal 6502 LDA/STA instructions.
    void store(uint16_t address, uint8_t value) {
        program.insert(program.end(), {LdaImmediate, value});
        absolute(StaAbsolute, address);
    }

    // Copy an observed bus value to shared RAM for an independent assertion.
    void sample(uint16_t address, unsigned result) {
        absolute(LdaAbsolute, address);
        absolute(StaAbsolute, Results + result);
    }

    // Install a reset vector and terminal loop without substituting CPU handlers.
    void install() {
        const uint16_t stop = RomStart + program.size();
        absolute(JumpAbsolute, stop);
        std::copy(program.begin(), program.end(), rom.begin());
        rom[ResetVector - RomStart] = static_cast<uint8_t>(RomStart);
        rom[ResetVector - RomStart + 1] = static_cast<uint8_t>(RomStart >> 8);
        rom4040_init(&machine, rom.data(), nullptr, nullptr);
    }

    // Start from RESET and stop at a terminal self-loop with a cycle bound.
    void run() {
        const uint16_t stop = RomStart + program.size();
        install();
        for (unsigned cycle = 0; cycle < InstructionBudget; ++cycle) {
            rom4040_tick(&machine);
            ASSERT_EQ(machine.fault, ROM4040_FAULT_NONE);
            if ((machine.pins & M6502_SYNC) && M6502_GET_ADDR(machine.pins) == stop) return;
        }
        FAIL() << "6502 hardware test did not reach its terminal instruction";
    }

    // Continue a waiting test program after changing only external pin levels.
    void resume(unsigned cycles = 100) {
        rom4040_run(&machine, cycles);
        ASSERT_EQ(machine.fault, ROM4040_FAULT_NONE);
    }

    // Redirect a waiting terminal jump to additional CPU instructions.
    template<class Append>
    void next(Append append) {
        const size_t jump = program.size() - 3;
        const uint16_t waiting = RomStart + jump;
        // A randomized polling gap can stop midway through the jump's operands.
        // Patch only at opcode fetch so the test cannot create a torn jump address.
        unsigned cycles = 0;
        while (!((machine.pins & M6502_SYNC) && M6502_GET_ADDR(machine.pins) == waiting)
               && cycles++ < InstructionBudget)
            rom4040_tick(&machine);
        ASSERT_LT(cycles, InstructionBudget) << "CPU did not return to its waiting instruction";
        const uint16_t target = RomStart + program.size();
        program[jump + 1] = static_cast<uint8_t>(target);
        program[jump + 2] = static_cast<uint8_t>(target >> 8);
        append();
        const uint16_t stop = RomStart + program.size();
        absolute(JumpAbsolute, stop);
        std::copy(program.begin(), program.end(), rom.begin());
        resume();
    }
};

// Schematic 320806: A15, stack and the four 1 KiB shared blocks are aliased.
TEST_F(Hardware4040Test, CpuBusDecodesRamStackAndSharedAliases) {
    store(0x000a, 0x35);
    sample(0x010a, 0);
    sample(0x810a, 1);
    store(0x018a, 0x79);
    sample(0x008a, 2);
    for (unsigned block = 1; block <= 4; ++block) {
        const uint16_t address = block * 0x1000 + 0x2a;
        store(address, static_cast<uint8_t>(block));
        sample(address | 0x8c00, 2 + block);
    }
    run();
    EXPECT_EQ(machine.shared[0x800], 0x35);
    EXPECT_EQ(machine.shared[0x801], 0x35);
    EXPECT_EQ(machine.shared[0x802], 0x79);
    for (unsigned block = 1; block <= 4; ++block)
        EXPECT_EQ(machine.shared[0x802 + block], block);
}

// RIOT input-configured pins release the bus, outputs compose with external DIO.
TEST_F(Hardware4040Test, RiotDdrAndMirrorsComposeWiredData) {
    store(OutputPort, 0xa5);
    sample(DataPort, 0);
    store(OutputDdr, 0x0f);
    sample(DataPort, 1);
    sample(OutputPort | 0x18, 2);
    store(DataDdr, 0xf0);
    store(DataPort, 0x30);
    sample(DataPort | 0x0d60, 3);
    sample(DataDdr | 0x18, 4);
    run();
    EXPECT_EQ(machine.shared[0x800], 0xff);
    EXPECT_EQ(machine.shared[0x801], 0xf5);
    EXPECT_EQ(machine.shared[0x802], 0xf5);
    EXPECT_EQ(machine.shared[0x803], 0x35);
    EXPECT_EQ(machine.shared[0x804], 0xf0);
    EXPECT_EQ(rom4040_output(&machine).dio, 0xf5);
}

// Device straps read device 8, while LED output bits retain their latch values.
TEST_F(Hardware4040Test, RiotDeviceStrapsAndControlInputsUsePhysicalPolarity) {
    store(ControlDdr, 0x1f);
    store(ControlPort, 0x1c);
    store(DeviceDdr, 0x38);
    store(DevicePort, 0x28);
    sample(DevicePort, 0);
    sample(ControlPort, 1);
    run();
    EXPECT_EQ(machine.shared[0x800], 0xe8);
    EXPECT_EQ(machine.shared[0x801], 0x7c);
    auto peer = machine.input;
    peer.atn = peer.dav = peer.eoi = peer.nrfd = peer.ndac = false;
    rom4040_input(&machine, peer);
    const auto out = rom4040_output(&machine);
    EXPECT_FALSE(out.nrfd);
    EXPECT_FALSE(out.ndac);
}

// Native ATNIRQ reads $0287 to clear PA7, while $0285 is its flags mirror.
TEST_F(Hardware4040Test, RiotAtnEdgeLatchesWhileDisabledAndClearsOnFlagRead) {
    store(EdgePositiveDisabled, 0);
    run();
    auto peer = machine.input;
    peer.atn = false;
    rom4040_input(&machine, peer);
    resume();
    EXPECT_EQ(machine.pins & M6502_IRQ, 0u);
    EXPECT_TRUE(machine.edge_pending[1]);
    next([&] {
        store(EdgePositiveEnabled, 0);
        sample(0x0205, 2);
    });
    EXPECT_NE(machine.pins & M6502_IRQ, 0u);
    EXPECT_EQ(machine.shared[0x802], 0);
    next([&] {
        sample(EdgeFlags, 0);
        sample(EdgePositiveEnabled, 1);
    });
    EXPECT_EQ(machine.shared[0x800], 0x40);
    EXPECT_EQ(machine.shared[0x801], 0);
    EXPECT_EQ(machine.pins & M6502_IRQ, 0u);
}

// RIOT0's edge flag is independent and its IRQ output is not wired to the CPU.
TEST_F(Hardware4040Test, RiotZeroEdgeCannotInterruptOrClearRiotOne) {
    store(0x0206, 0);
    run();
    auto peer = machine.input;
    peer.dio = 0x7f;
    rom4040_input(&machine, peer);
    resume();
    EXPECT_TRUE(machine.edge_pending[0]);
    EXPECT_EQ(machine.pins & M6502_IRQ, 0u);
    next([&] { sample(0x021d, 0); });
    EXPECT_EQ(machine.shared[0x800], 0x40);
    EXPECT_FALSE(machine.edge_pending[0]);
}

// Output-configured PA7 transitions also enter the edge detector.
TEST_F(Hardware4040Test, RiotPa7EdgeObservesOutputLatchAndDdr) {
    store(EdgePositiveEnabled, 0);
    store(ControlPort, 0x80);
    store(ControlDdr, 0x80);
    run();
    EXPECT_TRUE(machine.input.atn);
    EXPECT_TRUE(machine.edge_pending[1]);
    resume();
    EXPECT_NE(machine.pins & M6502_IRQ, 0u);
}

// Schematic 320806's discrete gates override software during ATN acknowledgement.
TEST_F(Hardware4040Test, AtnaRfdoDacoResolveEveryHandshakeCombination) {
    for (unsigned bits = 0; bits < 16; ++bits) {
        const bool atn = bits & 8, atna = bits & 1;
        const bool daco = bits & 2, rfdo = bits & 4;
        machine.ports[1][1] = 0xff;
        machine.ports[1][0] = bits & 7;
        machine.input.atn = atn;
        const auto output = rom4040_output(&machine);
        SCOPED_TRACE(bits);
        EXPECT_EQ(output.nrfd, (atn != atna) && rfdo);
        EXPECT_EQ(output.ndac, (atn || atna) && !daco);
    }
}

// MOS 6532: underflow follows 1 + value * divisor clocks, then counts at /1.
TEST_F(Hardware4040Test, RiotTimersUseEveryPrescalerAndNativeUnderflowBoundary) {
    constexpr std::array<unsigned, 4> Divisors{1, 8, 64, 1024};
    constexpr uint8_t Initial = 3;
    for (unsigned index = 0; index < Divisors.size(); ++index) {
        SCOPED_TRACE(Divisors[index]);
        program.clear();
        const uint16_t address = 0x029c + index;
        store(address, Initial);
        install();
        bool loaded = false;
        for (unsigned cycle = 0; cycle < InstructionBudget; ++cycle) {
            rom4040_tick(&machine);
            if (!(machine.pins & M6502_RW) && M6502_GET_ADDR(machine.pins) == address) {
                loaded = true;
                break;
            }
        }
        ASSERT_TRUE(loaded);
        EXPECT_EQ(machine.timers[1].value, Initial);
        const unsigned deadline = 1 + Initial * Divisors[index];
        for (unsigned elapsed = 1; elapsed < deadline; ++elapsed) {
            rom4040_tick(&machine);
            ASSERT_FALSE(machine.timers[1].pending) << elapsed;
        }
        rom4040_tick(&machine);
        EXPECT_TRUE(machine.timers[1].pending);
        EXPECT_EQ(machine.timers[1].value, 0xff);
        EXPECT_NE(machine.pins & M6502_IRQ, 0u);
        resume(1);
        EXPECT_EQ(machine.timers[1].value, 0xfe);
    }
}

// Flags reads acknowledge PA7 only. Timer reads acknowledge timer and select IRQ.
TEST_F(Hardware4040Test, RiotTimerAndEdgeAcknowledgementsAreIndependent) {
    store(EdgePositiveEnabled, 0);
    store(0x029f, 0);
    run();
    auto peer = machine.input;
    peer.atn = false;
    rom4040_input(&machine, peer);
    resume();
    next([&] {
        sample(EdgeFlags, 0);
        sample(EdgeFlags | 0x18, 1);
    });
    EXPECT_EQ(machine.shared[0x800], 0xc0);
    EXPECT_EQ(machine.shared[0x801], 0x80);
    EXPECT_TRUE(machine.timers[1].pending);
    EXPECT_FALSE(machine.edge_pending[1]);
    EXPECT_NE(machine.pins & M6502_IRQ, 0u);
    next([&] { sample(0x0284, 2); });
    EXPECT_FALSE(machine.timers[1].enabled);
    EXPECT_FALSE(machine.timers[1].pending);
    EXPECT_EQ(machine.pins & M6502_IRQ, 0u);
    EXPECT_FALSE(machine.timers[0].enabled);
}

// RIOT0 timers still count and latch, but cannot drive the main CPU IRQ pin.
TEST_F(Hardware4040Test, RiotZeroTimerIsNotConnectedToCpuIrq) {
    store(0x021f, 0);
    run();
    resume();
    EXPECT_TRUE(machine.timers[0].pending);
    EXPECT_TRUE(machine.timers[0].enabled);
    EXPECT_EQ(machine.pins & M6502_IRQ, 0u);
    next([&] {
        sample(0x0205, 0);
        sample(0x020c, 1);
    });
    EXPECT_EQ(machine.shared[0x800], 0x80);
    EXPECT_FALSE(machine.timers[0].pending);
    EXPECT_TRUE(machine.timers[0].enabled);
    EXPECT_EQ(machine.pins & M6502_IRQ, 0u);
}

// A timer read on its underflow bus cycle must not erase the newly raised IRQ.
TEST_F(Hardware4040Test, RiotTimerUnderflowWinsOverSameCycleRead) {
    store(0x029c, 3);
    sample(0x028c, 0);
    run();
    EXPECT_EQ(machine.shared[0x800], 0xff);
    EXPECT_TRUE(machine.timers[1].pending);
    EXPECT_NE(machine.pins & M6502_IRQ, 0u);
}

// Reload discards the old timer event but preserves PA7, and expiry can occur again.
TEST_F(Hardware4040Test, TimerReloadRearmsAfterUnderflowWithoutAcknowledgingEdge) {
    for (const auto base : RiotBases) {
        program.clear();
        store(base + 7, 0);
        store(base + 0x1f, 0);
        run();
        auto peer = machine.input;
        if (base == RiotBases[0]) {
            peer.dio = 0x7f;
            rom4040_input(&machine, peer);
            peer.dio = 0xff;
        } else peer.atn = false;
        rom4040_input(&machine, peer);
        resume();
        const unsigned riot = base == RiotBases[0] ? 0 : 1;
        next([&] { store(base + 0x17, 1); });
        EXPECT_FALSE(machine.timers[riot].pending);
        EXPECT_FALSE(machine.timers[riot].expired);
        EXPECT_FALSE(machine.timers[riot].enabled);
        EXPECT_TRUE(machine.edge_pending[riot]);
        resume(TimerDivisors[3]);
        EXPECT_TRUE(machine.timers[riot].pending);
        EXPECT_TRUE(machine.timers[riot].expired);
        // A flag read clears only PA7. Repeated reads must not restart the timer.
        next([&] { sample(base + 5, 0); });
        EXPECT_EQ(machine.shared[0x800], 0xc0);
        EXPECT_FALSE(machine.edge_pending[riot]);
        EXPECT_TRUE(machine.timers[riot].expired);
        EXPECT_TRUE(machine.timers[riot].pending);
    }
}

// Changing RIOT0 PB's DIO contribution can cause a PA7 edge even without external activity.
TEST_F(Hardware4040Test, DdrAndDataOutputFeedbackBothReachRiotZeroEdgeDetector) {
    store(0x0204, 0);
    store(OutputPort, 0x7f);
    run();
    EXPECT_FALSE(machine.edge_pending[0]);
    next([&] { store(OutputDdr, 0x80); });
    EXPECT_TRUE(machine.edge_pending[0]);
    next([&] {
        sample(0x0205, 0);
        store(0x0205, 0);
        store(OutputPort, 0xff);
    });
    EXPECT_EQ(machine.shared[0x800], 0x40);
    EXPECT_TRUE(machine.edge_pending[0]);
    EXPECT_EQ(machine.pins & M6502_IRQ, 0u);
}

// Cold reset removes pending protocol state instead of inheriting stale IRQs or jobs.
TEST_F(Hardware4040Test, ResetClearsBothRiotsJobsAndControllerHandshakeState) {
    store(ControlDdr, 0xff);
    store(ControlPort, 0);
    store(EdgePositiveEnabled, 0);
    store(0x029f, 0);
    run();
    machine.bootstrap = machine.running = true;
    std::fill_n(machine.shared, ROM4040_SHARED_SIZE, 0xff);
    std::fill_n(machine.riot_ram, ROM4040_RIOT_RAM_SIZE, 0xff);
    rom4040_init(&machine, rom.data(), nullptr, nullptr);
    EXPECT_FALSE(machine.running);
    EXPECT_FALSE(machine.bootstrap);
    EXPECT_EQ(machine.cycles, 0u);
    EXPECT_EQ(machine.jobs, 0u);
    EXPECT_TRUE(std::all_of(std::begin(machine.shared), std::end(machine.shared),
        [](uint8_t byte) { return byte == 0; }));
    EXPECT_TRUE(std::all_of(std::begin(machine.riot_ram), std::end(machine.riot_ram),
        [](uint8_t byte) { return byte == 0; }));
    for (unsigned riot = 0; riot < 2; ++riot) {
        EXPECT_FALSE(machine.edge_enabled[riot]);
        EXPECT_FALSE(machine.edge_pending[riot]);
        EXPECT_FALSE(machine.timers[riot].enabled);
        EXPECT_FALSE(machine.timers[riot].pending);
        EXPECT_EQ(machine.ports[riot][1], 0);
        EXPECT_EQ(machine.ports[riot][3], 0);
    }
    const auto output = rom4040_output(&machine);
    EXPECT_TRUE(output.dav);
    EXPECT_TRUE(output.eoi);
    EXPECT_EQ(output.dio, 0xff);
    EXPECT_EQ(machine.pins & (M6502_IRQ | M6502_NMI), 0u);
}

// Fifteen native job slots map headers and full buffers without overlapping RAM.
TEST_F(Hardware4040Test, CpuStoresRouteEveryJobHeaderAndBufferExactlyOnce) {
    struct Request {
        uint8_t command, id, first, last;
    };
    std::vector<Request> requests;
    for (unsigned index = 0; index < ROM4040_JOB_COUNT; ++index) {
        const unsigned offset = (index + 1) * ROM4040_SECTOR_SIZE;
        const uint16_t buffer = ((offset / 0x400 + 1) << 12) | (offset & 0x3ff);
        store(0x1021 + index * 8, index);
        store(buffer, 0x30 + index);
        store(buffer + ROM4040_SECTOR_SIZE - 1, 0x70 + index);
        store(0x1003 + index, CommandRead | (index & 1));
    }
    install();
    machine.running = true;
    machine.context = &requests;
    machine.job = [](void* context, uint8_t command, uint8_t* header, uint8_t* sector) -> uint8_t {
        auto& calls = *static_cast<std::vector<Request>*>(context);
        calls.push_back({command, header[0], sector[0], sector[ROM4040_SECTOR_SIZE - 1]});
        return ControllerOk;
    };
    resume(InstructionBudget);
    ASSERT_EQ(requests.size(), ROM4040_JOB_COUNT);
    for (size_t index = 0; index < requests.size(); ++index) {
        SCOPED_TRACE(index);
        EXPECT_EQ(requests[index].command, CommandRead | (index & 1));
        EXPECT_EQ(requests[index].id, index);
        EXPECT_EQ(requests[index].first, 0x30 + index);
        EXPECT_EQ(requests[index].last, 0x70 + index);
        EXPECT_EQ(machine.shared[3 + index], ControllerOk);
    }
}

// Exercise all port-register mirrors through CPU instructions, not private handlers.
TEST_F(Hardware4040Test, EveryRiotPortMirrorRetainsIndependentLatchAndDirection) {
    for (const auto base : RiotBases) {
        for (unsigned mirror = 0; mirror <= 0x8d60; ++mirror) {
        if (mirror & ~0x8d60u) continue;
            for (const unsigned local : {0u, 8u, 16u, 24u}) {
                SCOPED_TRACE(base | mirror | local);
                program.clear();
                for (unsigned reg = 0; reg < 4; ++reg)
                    store(base | mirror | local | reg, 0xff);
                store(base | mirror | local, 0xa5);
                store(base | mirror | local | 2, 0x5a);
                for (unsigned reg = 0; reg < 4; ++reg) sample(base + reg, reg);
                run();
                EXPECT_EQ(machine.shared[0x800], 0xa5);
                EXPECT_EQ(machine.shared[0x801], 0xff);
                EXPECT_EQ(machine.shared[0x802], 0x5a);
                EXPECT_EQ(machine.shared[0x803], 0xff);
                EXPECT_EQ(machine.ports[(base == RiotBases[0]) ? 1 : 0][1], 0);
            }
        }
    }
}

// External address mirrors apply to all 32 registers, including timers and edge controls.
TEST_F(Hardware4040Test, EveryRiotRegisterExternalMirrorHasIdenticalBusSideEffects) {
    for (const auto base : RiotBases) {
        for (unsigned reg = 0; reg < 32; ++reg) {
            std::array<uint8_t, 4> expected_ports{};
            rom4040_timer_t expected_timer{};
            bool expected_positive = false, expected_enabled = false;
            uint8_t expected_read = 0;
            for (unsigned mirror = 0; mirror <= 0x8d60; ++mirror) {
                if (mirror & ~0x8d60u) continue;
                program.clear();
                store(base | reg | mirror, 0x55);
                sample(base | reg | mirror, 0);
                run();
                const unsigned riot = base == RiotBases[0] ? 0 : 1;
                if (mirror == 0) {
                    std::copy_n(machine.ports[riot], 4, expected_ports.begin());
                    expected_timer = machine.timers[riot];
                    expected_positive = machine.edge_positive[riot];
                    expected_enabled = machine.edge_enabled[riot];
                    expected_read = machine.shared[0x800];
                } else {
                    ASSERT_TRUE(std::equal(expected_ports.begin(), expected_ports.end(),
                        machine.ports[riot])) << base << " " << reg << " " << mirror;
                    ASSERT_EQ(machine.timers[riot].prescale, expected_timer.prescale);
                    ASSERT_EQ(machine.timers[riot].divider, expected_timer.divider);
                    ASSERT_EQ(machine.timers[riot].value, expected_timer.value);
                    ASSERT_EQ(machine.timers[riot].enabled, expected_timer.enabled);
                    ASSERT_EQ(machine.timers[riot].pending, expected_timer.pending);
                    ASSERT_EQ(machine.edge_positive[riot], expected_positive);
                    ASSERT_EQ(machine.edge_enabled[riot], expected_enabled);
                    ASSERT_EQ(machine.shared[0x800], expected_read);
                }
            }
        }
    }
}

// Decode every RAM byte and every hardware alias without allocating mirrored storage.
TEST_F(Hardware4040Test, EveryRamByteAndAliasSharesStorageAndRomWritesAreIgnored) {
    for (unsigned address = 0; address < 0x10000; ++address) {
        const unsigned decoded = address & 0x7fff;
        if (decoded >= 0x200 && decoded < 0x1000) continue;
        program.clear();
        store(address, 0x5a);
        absolute(LdaAbsolute, address);
        absolute(StaAbsolute, 0x0070);
        install();
        const auto saved_rom = rom;
        resume(30);
        if (decoded < 0x200) {
            ASSERT_EQ(machine.riot_ram[decoded & 0xff], 0x5a) << address;
        } else if (decoded < 0x5000) {
            const unsigned offset = ((decoded / 0x1000 - 1) * 0x400) | (decoded & 0x3ff);
            ASSERT_EQ(machine.shared[offset], 0x5a) << address;
        } else {
            ASSERT_EQ(machine.riot_ram[0x70], saved_rom[decoded - 0x5000]) << address;
            ASSERT_EQ(rom, saved_rom) << address;
        }
    }
}

// Each DDR/latch byte pair must select the latch on outputs and physical pins on inputs.
TEST_F(Hardware4040Test, EveryPortDdrAndLatchByteComposesWithPhysicalInputs) {
    for (const auto base : RiotBases) {
        for (unsigned port = 0; port <= 2; port += 2) {
            for (unsigned ddr = 0; ddr < 256; ++ddr) {
                for (unsigned latch = 0; latch < 256; ++latch) {
                    program.clear();
                    store(base + port, latch);
                    store(base + port + 1, ddr);
                    sample(base + port, 0);
                    install();
                    auto peer = machine.input;
                    peer.dio = static_cast<uint8_t>(~latch);
                    peer.atn = peer.dav = peer.eoi = peer.nrfd = peer.ndac = false;
                    rom4040_input(&machine, peer);
                    resume(100);
                    // RIOT0 PB has pullups. RIOT1 inputs are all asserted here,
                    // with inverted ATN, device-8 straps and no LED input pullups.
                    const uint8_t input = base == RiotBases[0]
                        ? (port == 0 ? static_cast<uint8_t>(~latch) : 0xff)
                        : (port == 0 ? 0x80 : 0);
                    const uint8_t expected = (latch & ddr) | (input & ~ddr);
                    ASSERT_EQ(machine.shared[0x800], expected)
                        << "base=" << base << " port=" << port
                        << " ddr=" << ddr << " latch=" << latch;
                    ASSERT_EQ(machine.fault, ROM4040_FAULT_NONE);
                }
            }
        }
    }
}

// Both data-direction paths resolve every external DIO byte without changing the latch.
TEST_F(Hardware4040Test, EveryExternalDataByteIsWiredAndWithDriveOutput) {
    for (unsigned drive = 0; drive < 256; ++drive) {
        program.clear();
        store(OutputPort, drive);
        store(OutputDdr, 0xff);
        const uint16_t read = RomStart + program.size();
        sample(DataPort, 0);
        install();
        resume();
        rom[program.size() - 2] = static_cast<uint8_t>(read);
        rom[program.size() - 1] = static_cast<uint8_t>(read >> 8);
        for (unsigned external = 0; external < 256; ++external) {
            auto peer = machine.input;
            peer.dio = external;
            rom4040_input(&machine, peer);
            // Reuse a bounded read program at the terminal loop.
            resume(20);
            ASSERT_EQ(machine.shared[0x800], drive & external)
                << "drive=" << drive << " external=" << external;
            ASSERT_EQ(machine.ports[0][2], drive);
        }
    }
}

// All external control combinations must compose with every software output combination.
TEST_F(Hardware4040Test, EveryControlInputAndOutputCombinationResolvesAtCpuPins) {
    for (unsigned output = 0; output < 32; ++output) {
        for (unsigned input = 0; input < 32; ++input) {
            program.clear();
            store(ControlDdr, 0x1f);
            store(ControlPort, output);
            sample(ControlPort, 0);
            sample(DevicePort, 1);
            install();
            auto peer = machine.input;
            peer.atn = input & 1;
            peer.dav = input & 2;
            peer.eoi = input & 4;
            peer.nrfd = input & 8;
            peer.ndac = input & 16;
            rom4040_input(&machine, peer);
            resume(100);
            const bool atna = output & 1, daco = output & 2, rfdo = output & 4;
            const bool nrfd = (peer.atn != atna) && rfdo;
            const bool ndac = (peer.atn || atna) && !daco;
            const uint8_t pa = output | ((!peer.atn) ? 0x80 : 0)
                | ((peer.dav && (output & 16)) ? 0x40 : 0)
                | ((peer.eoi && (output & 8)) ? 0x20 : 0);
            const uint8_t pb = ((peer.nrfd && nrfd) ? 0x80 : 0)
                | ((peer.ndac && ndac) ? 0x40 : 0);
            ASSERT_EQ(machine.shared[0x800], pa) << output << " " << input;
            ASSERT_EQ(machine.shared[0x801], pb) << output << " " << input;
        }
    }
}

// PA7 senses both polarities even while masked, across every edge-control mirror.
TEST_F(Hardware4040Test, EveryEdgeControlMirrorPolarityMaskAndHeldLevel) {
    for (const auto base : RiotBases) {
        for (unsigned reg = 4; reg < 16; ++reg) {
            if (!(reg & 4)) continue;
            program.clear();
            store(base + reg, 0xff);
            run();
            auto peer = machine.input;
            // First establish the opposite level and clear any transition flag.
            const bool positive = reg & 1;
            if (base == RiotBases[0]) peer.dio = positive ? 0x7f : 0xff;
            else peer.atn = positive;
            rom4040_input(&machine, peer);
            next([&] { sample(base + 5, 0); });
            if (base == RiotBases[0]) peer.dio ^= 0x80;
            else peer.atn = !peer.atn;
            rom4040_input(&machine, peer);
            resume();
            EXPECT_TRUE(machine.edge_pending[base == RiotBases[0] ? 0 : 1]);
            EXPECT_EQ((machine.pins & M6502_IRQ) != 0,
                base == RiotBases[1] && (reg & 2));
            next([&] {
                sample(base + 0x1f, 0);
                sample(base + 0x0d, 1);
            });
            EXPECT_EQ(machine.shared[0x800], 0x40);
            EXPECT_EQ(machine.shared[0x801], 0);
            // Re-presenting a held level is not another edge.
            rom4040_input(&machine, peer);
            resume();
            EXPECT_FALSE(machine.edge_pending[base == RiotBases[0] ? 0 : 1]);
        }
    }
}

// Loads cover every register, initial byte and both IRQ masks on both RIOTs.
TEST_F(Hardware4040Test, EveryTimerLoadValuePrescalerAndIrqMaskHasExactBoundary) {
    for (const auto base : RiotBases) {
        for (unsigned reg = 0x14; reg <= 0x1f; ++reg) {
            if (!(reg & 4)) continue;
            for (unsigned initial = 0; initial < 256; ++initial) {
                program.clear();
                store(base + reg, initial);
                install();
                bool loaded = false;
                for (unsigned cycle = 0; cycle < InstructionBudget; ++cycle) {
                    rom4040_tick(&machine);
                    if (!(machine.pins & M6502_RW)
                        && M6502_GET_ADDR(machine.pins) == base + reg) {
                        loaded = true;
                        break;
                    }
                }
                ASSERT_TRUE(loaded);
                const unsigned riot = base == RiotBases[0] ? 0 : 1;
                ASSERT_EQ(machine.timers[riot].value, initial);
                const unsigned deadline = 1 + initial * TimerDivisors[reg & 3];
                for (unsigned elapsed = 1; elapsed < deadline; ++elapsed) {
                    rom4040_tick(&machine);
                    if (machine.timers[riot].pending)
                        FAIL() << "early underflow base=" << base << " reg=" << reg
                               << " initial=" << initial << " elapsed=" << elapsed;
                }
                rom4040_tick(&machine);
                ASSERT_EQ(machine.timers[riot].value, 0xff);
                ASSERT_TRUE(machine.timers[riot].pending);
                ASSERT_EQ((machine.pins & M6502_IRQ) != 0, riot == 1 && (reg & 8));
            }
        }
    }
}

// All flag/timer read mirrors distinguish acknowledgement and interrupt-enable selection.
TEST_F(Hardware4040Test, EveryTimerAndFlagsReadMirrorHasIndependentSideEffects) {
    for (const auto base : RiotBases) {
        for (unsigned reg = 4; reg < 32; ++reg) {
            if (!(reg & 4)) continue;
            program.clear();
            store(base + 7, 0);
            store(base + 0x1f, 0);
            run();
            auto peer = machine.input;
            if (base == RiotBases[0]) {
                peer.dio = 0x7f;
                rom4040_input(&machine, peer);
                peer.dio = 0xff;
            } else peer.atn = false;
            rom4040_input(&machine, peer);
            resume();
            const unsigned riot = base == RiotBases[0] ? 0 : 1;
            ASSERT_TRUE(machine.timers[riot].pending);
            ASSERT_TRUE(machine.edge_pending[riot]);
            next([&] { sample(base + reg, 0); });
            if (reg & 1) {
                EXPECT_EQ(machine.shared[0x800], 0xc0);
                EXPECT_FALSE(machine.edge_pending[riot]);
                EXPECT_TRUE(machine.timers[riot].pending);
                EXPECT_TRUE(machine.timers[riot].enabled);
            } else {
                EXPECT_TRUE(machine.edge_pending[riot]);
                EXPECT_FALSE(machine.timers[riot].pending);
                EXPECT_EQ(machine.timers[riot].enabled, (reg & 8) != 0);
                EXPECT_FALSE(machine.timers[riot].expired);
                EXPECT_EQ(machine.timers[riot].prescale, TimerDivisors[3]);
            }
        }
    }
}

// The actual CPU vectors through IRQ on ATN and returns through RTI after acknowledgement.
TEST_F(Hardware4040Test, AtnIrqVectorsThroughCpuAndDoesNotRetriggerAfterAcknowledgement) {
    constexpr uint16_t Handler = 0xd800;
    constexpr uint16_t Marker = Results + 1;
    program.push_back(0x58); // CLI allows the real 6502 interrupt sequence.
    store(EdgePositiveEnabled, 0);
    install();
    const std::array<uint8_t, 10> handler{
        LdaAbsolute, 0x87, 0x02, LdaImmediate, 0x5a,
        StaAbsolute, static_cast<uint8_t>(Marker), static_cast<uint8_t>(Marker >> 8),
        0x40, 0xea};
    std::copy(handler.begin(), handler.end(), rom.begin() + Handler - RomStart);
    rom[0x2ffe] = static_cast<uint8_t>(Handler);
    rom[0x2fff] = static_cast<uint8_t>(Handler >> 8);
    resume();
    auto peer = machine.input;
    peer.atn = false;
    rom4040_input(&machine, peer);
    resume();
    EXPECT_EQ(machine.shared[0x801], 0x5a);
    EXPECT_FALSE(machine.edge_pending[1]);
    EXPECT_EQ(machine.pins & M6502_IRQ, 0u);
    EXPECT_EQ(machine.pins & M6502_NMI, 0u);
}

// Only the exact startup payload in slot zero may park the native controller.
TEST_F(Hardware4040Test, BootstrapAcknowledgementAndControllerIdFollowDiagnosticOrdering) {
    for (unsigned mutation = 0; mutation <= ControllerWait.size(); ++mutation) {
        program.clear();
        for (size_t byte = 0; byte < ControllerWait.size(); ++byte)
            store(FirstBuffer + byte, ControllerWait[byte] ^ (byte == mutation ? 1 : 0));
        store(JobBase, CommandJump);
        sample(JobBase, 0);
        sample(ControllerId, 1);
        store(LastDiagnosticClear, 0);
        sample(ControllerId, 2);
        run();
        const bool valid = mutation == ControllerWait.size();
        EXPECT_EQ(machine.shared[0x800], valid ? 0 : CommandJump);
        EXPECT_EQ(machine.shared[0x801], 0);
        EXPECT_EQ(machine.shared[0x802], valid ? 0x0f : 0);
        EXPECT_EQ(machine.bootstrap, valid);
        EXPECT_EQ(machine.running, valid);
        EXPECT_EQ(machine.jobs, 0u);
    }
    for (unsigned slot = 1; slot < ROM4040_JOB_COUNT; ++slot) {
        program.clear();
        for (size_t byte = 0; byte < ControllerWait.size(); ++byte)
            store(FirstBuffer + byte, ControllerWait[byte]);
        store(JobBase + slot, CommandJump);
        store(LastDiagnosticClear, 0);
        run();
        EXPECT_FALSE(machine.bootstrap);
        EXPECT_FALSE(machine.running);
    }
}

// A bootstrap-looking request must have the exact command and final clear must write zero.
TEST_F(Hardware4040Test, BootstrapRejectsEveryOtherCommandAndPrematureControllerIdPublication) {
    for (unsigned command = 0; command < 256; ++command) {
        program.clear();
        for (size_t byte = 0; byte < ControllerWait.size(); ++byte)
            store(FirstBuffer + byte, ControllerWait[byte]);
        store(LastDiagnosticClear, 0);
        sample(ControllerId, 0);
        store(JobBase, command);
        sample(ControllerId, 1);
        store(LastDiagnosticClear, 1);
        sample(ControllerId, 2);
        store(LastDiagnosticClear, 0);
        sample(ControllerId, 3);
        run();
        EXPECT_EQ(machine.shared[0x800], 0);
        EXPECT_EQ(machine.shared[0x801], 0);
        EXPECT_EQ(machine.shared[0x802], 0);
        EXPECT_EQ(machine.shared[0x803], command == CommandJump ? 0x0f : 0);
        EXPECT_EQ(machine.running, command == CommandJump);
    }
}

// Mirrored job stores publish to the same physical mailbox and never duplicate on reads.
TEST_F(Hardware4040Test, EveryJobSlotMirrorSubmitsOnceAndReadsOnlyObserveCompletion) {
    for (unsigned slot = 0; slot < ROM4040_JOB_COUNT; ++slot) {
        for (unsigned mirror = 0; mirror <= 0x8c00; ++mirror) {
            if (mirror & ~0x8c00u) continue;
            program.clear();
            store((JobBase + slot) | mirror, CommandRead);
            sample(JobBase + slot, 0);
            sample((JobBase + slot) | mirror, 1);
            install();
            unsigned calls = 0;
            machine.running = true;
            machine.context = &calls;
            machine.job = [](void* context, uint8_t, uint8_t*, uint8_t*) -> uint8_t {
                ++*static_cast<unsigned*>(context);
                return ControllerOk;
            };
            resume();
            ASSERT_EQ(calls, 1u);
            ASSERT_EQ(machine.shared[0x800], ControllerOk);
            ASSERT_EQ(machine.shared[0x801], ControllerOk);
            ASSERT_EQ(machine.shared[3 + slot], ControllerOk);
        }
    }
}

// Pending values publish once per store, and synchronous completion never asserts IRQ/NMI.
TEST_F(Hardware4040Test, EveryJobByteAndSlotPublishesFullBuffersAndExactResultsWithoutInterrupts) {
    for (unsigned slot = 0; slot < ROM4040_JOB_COUNT; ++slot) {
        for (unsigned command = 0; command < 256; ++command) {
            program.clear();
            store(JobBase + slot, command);
            absolute(LdaAbsolute, JobBase + slot);
            absolute(StaAbsolute, 0x0070);
            install();
            machine.running = true;
            const unsigned offset = (slot + 1) * ROM4040_SECTOR_SIZE;
            for (unsigned byte = 0; byte < ROM4040_SECTOR_SIZE; ++byte)
                machine.shared[offset + byte] = static_cast<uint8_t>(byte ^ slot);
            for (unsigned byte = 0; byte < 8; ++byte)
                machine.shared[0x21 + slot * 8 + byte] = byte + slot;
            struct Call {
                unsigned count = 0, slot;
                uint8_t command;
                rom4040_t* machine;
            } call{0, slot, static_cast<uint8_t>(command), &machine};
            machine.context = &call;
            machine.job = [](void* context, uint8_t value, uint8_t* header, uint8_t* sector) -> uint8_t {
                auto& call = *static_cast<Call*>(context);
                EXPECT_EQ(value, call.command);
                EXPECT_EQ(header, call.machine->shared + 0x21 + call.slot * 8);
                EXPECT_EQ(sector, call.machine->shared + (call.slot + 1) * ROM4040_SECTOR_SIZE);
                for (unsigned byte = 0; byte < ROM4040_SECTOR_SIZE; ++byte)
                    EXPECT_EQ(sector[byte], static_cast<uint8_t>(byte ^ call.slot));
                for (unsigned byte = 0; byte < 8; ++byte) EXPECT_EQ(header[byte], byte + call.slot);
                std::fill_n(sector, ROM4040_SECTOR_SIZE, 0x5a);
                header[7] = 0xa5;
                ++call.count;
                return call.command & 0x7f;
            };
            resume();
            ASSERT_EQ(call.count, command >= 0x80 ? 1u : 0u);
            ASSERT_EQ(machine.riot_ram[0x70], command & 0x7f);
            ASSERT_EQ(machine.jobs, call.count);
            ASSERT_EQ(machine.pins & (M6502_IRQ | M6502_NMI), 0u);
            if (call.count) {
                ASSERT_TRUE(std::all_of(machine.shared + offset,
                    machine.shared + offset + ROM4040_SECTOR_SIZE,
                    [](uint8_t byte) { return byte == 0x5a; }));
                ASSERT_EQ(machine.shared[0x21 + slot * 8 + 7], 0xa5);
            }
        }
    }
}

// Every wire byte/ATN/EOI combination obeys all three listener phases and backpressure.
TEST_F(Hardware4040Test, FifoReceiveAllBytesAndMetadataHoldUntilCompleteHandshake) {
    for (unsigned value = 0; value < 256; ++value) {
        for (unsigned flags = 0; flags < 4; ++flags) {
            const bool atn = flags & 1, eoi = flags & 2;
            program.clear();
            store(ControlDdr, 0x1f);
            // Initially hold NRFD low, NDAC low.
            store(ControlPort, 0x1a | (atn ? 1 : 0));
            run();
            fifo4040_t fifo;
            fifo4040_init(&fifo, &machine);
            fifo4040_receive(&fifo, value, atn, eoi);
            for (unsigned wait = 0; wait < 20; ++wait) {
                ASSERT_FALSE(fifo4040_tick(&fifo));
                ASSERT_EQ(fifo.receive, FIFO4040_RX_READY);
                ASSERT_TRUE(fifo.peer.dav);
            }
            // The listener's RFDO release permits the peer to present data/DAV.
            next([&] { store(ControlPort, 0x1e | (atn ? 1 : 0)); });
            ASSERT_FALSE(fifo4040_tick(&fifo));
            ASSERT_EQ(fifo.receive, FIFO4040_RX_ACCEPT);
            ASSERT_EQ(fifo.peer.dio, static_cast<uint8_t>(~value));
            ASSERT_EQ(fifo.peer.eoi, !eoi);
            ASSERT_EQ(fifo.peer.atn, !atn);
            ASSERT_FALSE(fifo.peer.dav);
            for (unsigned wait = 0; wait < 20; ++wait) {
                ASSERT_FALSE(fifo4040_tick(&fifo));
                ASSERT_EQ(fifo.receive, FIFO4040_RX_ACCEPT);
            }
            // DACO low releases NDAC, allowing the sender to release DAV.
            next([&] { store(ControlPort, 0x18 | (atn ? 1 : 0)); });
            ASSERT_FALSE(fifo4040_tick(&fifo));
            ASSERT_EQ(fifo.receive, FIFO4040_RX_RELEASE);
            ASSERT_TRUE(fifo.peer.dav);
            for (unsigned wait = 0; wait < 20; ++wait) {
                ASSERT_FALSE(fifo4040_tick(&fifo));
                ASSERT_EQ(fifo.receive, FIFO4040_RX_RELEASE);
            }
            // Reasserting NDAC completes exactly once, with idle data/EOI.
            next([&] { store(ControlPort, 0x1a | (atn ? 1 : 0)); });
            ASSERT_TRUE(fifo4040_tick(&fifo));
            ASSERT_EQ(fifo.receive, FIFO4040_IDLE);
            ASSERT_EQ(fifo.peer.dio, 0xff);
            ASSERT_TRUE(fifo.peer.eoi);
            ASSERT_FALSE(fifo4040_tick(&fifo));
        }
    }
}

// Every output byte and EOI polarity is offered once, then held until GET or abort.
TEST_F(Hardware4040Test, FifoTransmitAllBytesEoiAndTerminationModesDoNotSpeculateGet) {
    for (unsigned value = 0; value < 256; ++value) {
        for (unsigned flags = 0; flags < 4; ++flags) {
            const bool eoi = flags & 1, get = flags & 2;
            program.clear();
            store(OutputDdr, 0xff);
            store(OutputPort, static_cast<uint8_t>(~value));
            store(ControlDdr, 0x1f);
            store(ControlPort, 4 | (eoi ? 0 : 8));
            run();
            fifo4040_t fifo;
            fifo4040_init(&fifo, &machine);
            ASSERT_FALSE(fifo4040_tick(&fifo));
            ASSERT_EQ(fifo.presented, 1u);
            uint8_t byte = 0;
            bool last = false;
            ASSERT_TRUE(fifo4040_transmit(&fifo, &byte, &last));
            ASSERT_EQ(byte, value);
            ASSERT_EQ(last, eoi);
            for (unsigned wait = 0; wait < 20; ++wait) {
                ASSERT_FALSE(fifo4040_transmit(&fifo, &byte, &last));
                ASSERT_FALSE(fifo4040_tick(&fifo));
                ASSERT_TRUE(fifo.peer.nrfd);
                ASSERT_FALSE(fifo.acknowledged);
                ASSERT_EQ(fifo.consumed, 0u);
                ASSERT_EQ(fifo.presented, 1u);
            }
            fifo4040_consumed(&fifo, get);
            ASSERT_EQ(fifo.peer.nrfd, !get);
            ASSERT_TRUE(fifo.peer.ndac);
            ASSERT_EQ(fifo.consumed, get ? 1u : 0u);
            ASSERT_FALSE(fifo4040_tick(&fifo));
            ASSERT_TRUE(fifo.transmitting);
            // The peer cannot begin another byte until the ROM releases DAV.
            next([&] { store(ControlPort, 0x1c); });
            ASSERT_FALSE(fifo4040_tick(&fifo));
            ASSERT_FALSE(fifo.transmitting);
            ASSERT_FALSE(fifo.acknowledged);
            ASSERT_TRUE(fifo.peer.nrfd);
            ASSERT_FALSE(fifo.peer.ndac);
            fifo4040_atn_end(&fifo);
            ASSERT_TRUE(fifo.peer.atn);
        }
    }
}

// An ATN selection replaces any source phase without manufacturing another GET.
TEST_F(Hardware4040Test, FifoAtnOverridesEverySourcePhaseAndRetainsConsumptionCount) {
    for (unsigned phase = 0; phase < 4; ++phase) {
        program.clear();
        store(ControlDdr, 0x1f);
        store(ControlPort, 4);
        run();
        fifo4040_t fifo;
        fifo4040_init(&fifo, &machine);
        fifo4040_tick(&fifo);
        if (phase) {
            uint8_t byte;
            bool eoi;
            ASSERT_TRUE(fifo4040_transmit(&fifo, &byte, &eoi));
        }
        if (phase >= 2) fifo4040_consumed(&fifo, phase == 3);
        fifo4040_receive(&fifo, 0x3f, true, false);
        EXPECT_FALSE(fifo.transmitting);
        EXPECT_FALSE(fifo.offered);
        EXPECT_FALSE(fifo.acknowledged);
        EXPECT_TRUE(fifo.peer.nrfd);
        EXPECT_TRUE(fifo.peer.ndac);
        EXPECT_FALSE(fifo.peer.atn);
        EXPECT_EQ(fifo.consumed, phase == 3 ? 1u : 0u);
        EXPECT_EQ(fifo.receive, FIFO4040_RX_READY);
    }
}

// Back controller requests with one independently tracked raw sector.
class Controller4040Test : public testing::Test {
protected:
    rom4040_t machine{};
    controller4040_t controller{.machine = &machine};
    std::array<uint8_t, 8> header{'I', 'D', 18, 0};
    std::array<uint8_t, ROM4040_SECTOR_SIZE> sector{}, media{};
    uint32_t last_offset = 0;
    unsigned reads = 0, writes = 0;
    bool fail_io = false;

    // Record exact disk access and emulate an explicit read or write failure.
    static bool io(void* context, uint32_t offset, uint8_t* bytes, bool write) {
        auto& self = *static_cast<Controller4040Test*>(context);
        self.last_offset = offset;
        if (write) ++self.writes;
        else ++self.reads;
        if (self.fail_io) return false;
        if (write) std::copy_n(bytes, self.media.size(), self.media.begin());
        else std::copy(self.media.begin(), self.media.end(), bytes);
        return true;
    }

    // Attach drive zero without touching filesystem or IEEE routines.
    void SetUp() override {
        controller.disk[0] = {.io = io, .context = this, .writable = true, .id = {'I', 'D'}};
        for (size_t i = 0; i < media.size(); ++i) media[i] = static_cast<uint8_t>(i);
    }

    // Enter the exact callback used by shared-memory job submission.
    uint8_t job(uint8_t command) {
        return controller4040_job(&controller, command, header.data(), sector.data());
    }
};

// Geometry follows the 4040's four track zones and rejects every boundary overrun.
TEST_F(Controller4040Test, GeometryCoversEveryD64SectorExactlyOnce) {
    uint32_t expected = 0, offset = 0;
    for (unsigned track = 1; track <= 35; ++track) {
        const unsigned count = track <= 17 ? 21 : track <= 24 ? 19 : track <= 30 ? 18 : 17;
        for (unsigned sector = 0; sector < count; ++sector) {
            ASSERT_TRUE(controller4040_offset(track, sector, &offset));
            EXPECT_EQ(offset, expected);
            expected += ROM4040_SECTOR_SIZE;
        }
        EXPECT_FALSE(controller4040_offset(track, count, &offset));
    }
    EXPECT_EQ(expected, CONTROLLER4040_IMAGE_SIZE);
    EXPECT_FALSE(controller4040_offset(0, 0, &offset));
    EXPECT_FALSE(controller4040_offset(36, 0, &offset));
}

// Native READ/WRITE/VERIFY exchange one buffer and publish the physical header checksum.
TEST_F(Controller4040Test, RawReadWriteVerifyAndFailureResults) {
    auto saved_header = header;
    saved_header[4] = header[0] ^ header[1] ^ header[2] ^ header[3];
    EXPECT_EQ(job(CommandRead), ControllerOk);
    EXPECT_EQ(sector, media);
    EXPECT_EQ(last_offset, 357u * ROM4040_SECTOR_SIZE);
    EXPECT_EQ(job(CommandVerify), ControllerOk);
    sector[73] ^= 0xff;
    EXPECT_EQ(job(CommandVerify), VerifyError);
    EXPECT_EQ(job(CommandWrite), ControllerOk);
    EXPECT_EQ(sector, media);
    EXPECT_EQ(header, saved_header);
    fail_io = true;
    for (const auto command : {CommandRead, CommandWrite, CommandVerify})
        EXPECT_EQ(job(command), DataError);
    EXPECT_EQ(machine.fault, ROM4040_FAULT_NONE);
}

// Failures never write media, and drive selection is the job's low bit.
TEST_F(Controller4040Test, IdProtectionCoordinatesAndMissingDriveHaveNativeResults) {
    EXPECT_EQ(job(CommandRead | 1), NoSync);
    header[0] ^= 1;
    EXPECT_EQ(job(CommandRead), IdMismatch);
    header[0] ^= 1;
    controller.disk[0].writable = false;
    EXPECT_EQ(job(CommandWrite), WriteProtected);
    header[2] = 36;
    EXPECT_EQ(job(CommandRead), NoSync);
    EXPECT_EQ(reads, 0u);
    EXPECT_EQ(writes, 0u);
    EXPECT_EQ(machine.fault, ROM4040_FAULT_NONE);
}

// Native SEEK supplies a physical header and BUMP preserves the caller's header.
TEST_F(Controller4040Test, SeekBumpAndUnsupportedControllerExecution) {
    header = {};
    EXPECT_EQ(job(CommandSeek), ControllerOk);
    EXPECT_EQ(header[0], 'I');
    EXPECT_EQ(header[1], 'D');
    EXPECT_EQ(header[3], 1);
    EXPECT_EQ(job(CommandBump), ControllerOk);
    EXPECT_EQ(header[2], 1);
    EXPECT_EQ(reads + writes, 0u);
    job(CommandJump);
    EXPECT_TRUE(controller.unsupported_code);
    EXPECT_EQ(machine.fault, ROM4040_FAULT_CONTROLLER_CODE);
}

// The low job bit routes drive 1 to its own media and ID, not drive 0's state.
TEST_F(Controller4040Test, TwoDrivesHaveIndependentIdentityAndWriteProtection) {
    controller.disk[1] = {.io = io, .context = this, .writable = false, .id = {'X', 'Y'}};
    EXPECT_EQ(job(CommandRead | 1), IdMismatch);
    header[0] = 'X';
    header[1] = 'Y';
    EXPECT_EQ(job(CommandRead | 1), ControllerOk);
    EXPECT_EQ(sector, media);
    EXPECT_EQ(job(CommandWrite | 1), WriteProtected);
    EXPECT_EQ(job(CommandRead), IdMismatch);
    EXPECT_EQ(writes, 0u);
}

// Stock formatter recognition gates all media changes and propagates failed writes.
TEST_F(Controller4040Test, FormatterIsBoundedProtectedAndRejectsChangedPayload) {
    constexpr uint8_t Execute = 0xe0;
    std::array<uint8_t, ROM4040_ROM_SIZE> rom{};
    for (size_t i = 0; i < 0x200; ++i) rom[i] = static_cast<uint8_t>(i);
    machine.rom = rom.data();
    std::copy_n(rom.begin(), 0x200, machine.shared + 0x100);
    EXPECT_EQ(job(Execute), ControllerOk);
    EXPECT_EQ(writes, CONTROLLER4040_IMAGE_SIZE / ROM4040_SECTOR_SIZE);
    EXPECT_EQ(last_offset, CONTROLLER4040_IMAGE_SIZE - ROM4040_SECTOR_SIZE);
    EXPECT_TRUE(std::all_of(media.begin(), media.end(), [](uint8_t byte) { return byte == 0; }));
    writes = 0;
    controller.disk[0].writable = false;
    EXPECT_EQ(job(Execute), WriteProtected);
    EXPECT_EQ(writes, 0u);
    controller.disk[0].writable = true;
    fail_io = true;
    EXPECT_EQ(job(Execute), DataError);
    EXPECT_EQ(writes, 1u);
    machine.shared[0x173] ^= 1;
    job(Execute);
    EXPECT_EQ(writes, 1u);
    EXPECT_EQ(machine.fault, ROM4040_FAULT_CONTROLLER_CODE);
}

// Every possible command byte must either implement a known operation or fault explicitly.
TEST_F(Controller4040Test, EveryCommandByteOnBothDrivesHasAnExplicitDisposition) {
    controller.disk[1] = controller.disk[0];
    std::array<uint8_t, ROM4040_ROM_SIZE> rom{};
    machine.rom = rom.data();
    for (unsigned command = 0; command < 256; ++command) {
        SCOPED_TRACE(command);
        machine.fault = ROM4040_FAULT_NONE;
        controller.unsupported_code = false;
        header = {'I', 'D', 18, 0};
        sector = media;
        const unsigned operation = command & 0xfe;
        const bool known = operation == CommandRead || operation == CommandWrite
            || operation == CommandVerify || operation == CommandSeek
            || operation == CommandBump || operation == CommandExecute;
        const auto result = job(command);
        EXPECT_EQ(result, known ? ControllerOk : ControllerDriveError);
        EXPECT_EQ(machine.fault, known ? ROM4040_FAULT_NONE : ROM4040_FAULT_CONTROLLER_CODE);
        EXPECT_EQ(controller.unsupported_code, !known);
    }
}

// Both bytes of the ID gate sector access, while non-data jobs handle absent media.
TEST_F(Controller4040Test, EveryWrongIdByteAndMissingMediaOperationAvoidsSectorIo) {
    for (unsigned drive = 0; drive < CONTROLLER4040_DRIVES; ++drive) {
        controller.disk[drive] = controller.disk[0];
        for (const uint8_t operation : {CommandRead, CommandWrite, CommandVerify}) {
            for (unsigned byte = 0; byte < 2; ++byte) {
                for (unsigned value = 0; value < 256; ++value) {
                    if (value == controller.disk[drive].id[byte]) continue;
                    header = {'I', 'D', 18, 0};
                    header[byte] = value;
                    ASSERT_EQ(job(operation | drive), IdMismatch);
                }
            }
        }
        EXPECT_EQ(reads, 0u);
        EXPECT_EQ(writes, 0u);
    }
    controller.disk[0] = {};
    controller.disk[1] = {};
    for (const uint8_t operation :
            {CommandRead, CommandWrite, CommandVerify, CommandSeek, CommandBump, CommandExecute}) {
        for (unsigned drive = 0; drive < CONTROLLER4040_DRIVES; ++drive)
            EXPECT_EQ(job(operation | drive), operation == CommandBump ? ControllerOk : NoSync);
    }
}

// Protection applies only to writes, and non-data jobs mutate only documented header bytes.
TEST_F(Controller4040Test, ReadOnlyMediaAllowsReadVerifySeekBumpAndPreservesUnrelatedHeaderBytes) {
    controller.disk[0].writable = false;
    controller.disk[1] = controller.disk[0];
    for (unsigned drive = 0; drive < CONTROLLER4040_DRIVES; ++drive) {
        header = {'I', 'D', 18, 0, 0x55, 0xaa, 0x33, 0xcc};
        auto original = header;
        original[4] = 'I' ^ 'D' ^ 18;
        EXPECT_EQ(job(CommandRead | drive), ControllerOk);
        EXPECT_EQ(job(CommandVerify | drive), ControllerOk);
        EXPECT_EQ(header, original);
        EXPECT_EQ(job(CommandWrite | drive), WriteProtected);
        EXPECT_EQ(header, original);
        header = {'?', '?', 18, 0xaa, 0x33, 0xcc, 0x5a, 0xa5};
        auto expected = header;
        expected[0] = 'I';
        expected[1] = 'D';
        expected[3] = 1;
        expected[4] = 'I' ^ 'D' ^ 18 ^ 1;
        EXPECT_EQ(job(CommandSeek | drive), ControllerOk);
        EXPECT_EQ(header, expected);
        EXPECT_EQ(job(CommandBump | drive), ControllerOk);
        EXPECT_EQ(header, expected);
        EXPECT_EQ(writes, 0u);
    }
}

// No single-byte corruption of the recognized formatter can write or change disk identity.
TEST_F(Controller4040Test, EveryFormatterPayloadByteIsCheckedBeforeTouchingEitherDrive) {
    std::array<uint8_t, ROM4040_ROM_SIZE> rom{};
    machine.rom = rom.data();
    controller.disk[1] = controller.disk[0];
    for (unsigned drive = 0; drive < CONTROLLER4040_DRIVES; ++drive) {
        for (unsigned byte = 0; byte < 0x200; ++byte) {
            machine.shared[0x100 + byte] = 1;
            EXPECT_EQ(job(CommandExecute | drive), ControllerDriveError);
            EXPECT_EQ(machine.fault, ROM4040_FAULT_CONTROLLER_CODE);
            EXPECT_TRUE(controller.unsupported_code);
            EXPECT_EQ(writes, 0u);
            EXPECT_EQ(controller.disk[drive].id[0], 'I');
            EXPECT_EQ(controller.disk[drive].id[1], 'D');
            machine.shared[0x100 + byte] = 0;
        }
    }
}
// Format visits each sector once in order, and stops at every possible failed write.
TEST_F(Controller4040Test, FormatterChecksEverySectorFailureAndDoesNotPublishNewIdOnFailure) {
    struct Sink {
        unsigned writes = 0, fail_at = 0;
    } sink;
    std::array<uint8_t, ROM4040_ROM_SIZE> rom{};
    machine.rom = rom.data();
    constexpr unsigned SectorCount = CONTROLLER4040_IMAGE_SIZE / ROM4040_SECTOR_SIZE;
    for (unsigned drive = 0; drive < CONTROLLER4040_DRIVES; ++drive) {
        controller.disk[drive] = {.io = [](void* context, uint32_t offset,
                uint8_t* bytes, bool write) -> bool {
            auto& sink = *static_cast<Sink*>(context);
            EXPECT_TRUE(write);
            EXPECT_EQ(offset, sink.writes * ROM4040_SECTOR_SIZE);
            EXPECT_TRUE(std::all_of(bytes, bytes + ROM4040_SECTOR_SIZE,
                [](uint8_t byte) { return byte == 0; }));
            return sink.writes++ != sink.fail_at;
        }, .context = &sink, .writable = true, .id = {'O', 'L'}};
        for (unsigned failed = 0; failed < SectorCount; ++failed) {
            sink = {0, failed};
            controller.disk[drive].id[0] = 'O';
            controller.disk[drive].id[1] = 'L';
            EXPECT_EQ(job(CommandExecute | drive), DataError);
            EXPECT_EQ(sink.writes, failed + 1);
            EXPECT_EQ(controller.disk[drive].id[0], 'O');
            EXPECT_EQ(controller.disk[drive].id[1], 'L');
            EXPECT_EQ(machine.fault, ROM4040_FAULT_NONE);
        }
    }
}

// Controller requests enter through CPU stores and complete into that same mailbox.
class ControllerMailbox4040Test : public Hardware4040Test {
protected:
    struct Disk {
        std::vector<uint8_t> bytes = std::vector<uint8_t>(CONTROLLER4040_IMAGE_SIZE);
        unsigned reads = 0, writes = 0;
        bool fail = false;
    };
    std::array<Disk, CONTROLLER4040_DRIVES> disks;
    controller4040_t controller{};

    // Independent image backing checks buffer bounds and records all controller accesses.
    static bool io(void* context, uint32_t offset, uint8_t* bytes, bool write) {
        auto& disk = *static_cast<Disk*>(context);
        EXPECT_LE(offset + ROM4040_SECTOR_SIZE, disk.bytes.size());
        if (offset > disk.bytes.size() - ROM4040_SECTOR_SIZE) return false;
        if (write) ++disk.writes;
        else ++disk.reads;
        if (disk.fail) return false;
        if (write) std::copy_n(bytes, ROM4040_SECTOR_SIZE, disk.bytes.begin() + offset);
        else std::copy_n(disk.bytes.begin() + offset, ROM4040_SECTOR_SIZE, bytes);
        return true;
    }

    // Install a single submission, retaining image state across fresh CPU/controller boots.
    void submit(uint8_t command, uint8_t track, uint8_t sector,
                bool writable = true, bool present = true, bool correct_id = true) {
        program.clear();
        store(HeaderBase, correct_id ? 'I' : 'X');
        store(HeaderBase + 1, 'D');
        store(HeaderBase + 2, track);
        store(HeaderBase + 3, sector);
        store(JobBase, command);
        absolute(LdaAbsolute, JobBase);
        absolute(StaAbsolute, 0x0070);
        install();
        controller = {.machine = &machine};
        const unsigned drive = command & 1;
        if (present)
            controller.disk[drive] = {.io = io, .context = &disks[drive],
                .writable = writable, .id = {'I', 'D'}};
        machine.running = true;
        machine.job = controller4040_job;
        machine.context = &controller;
        for (unsigned byte = 0; byte < ROM4040_SECTOR_SIZE; ++byte)
            machine.shared[0x100 + byte] = (command & 0xfe) == CommandRead
                ? 0x33 : static_cast<uint8_t>(byte ^ (0xa5 + drive));
        resume(100);
    }
};

// READ/WRITE/VERIFY cover all 683 sectors on each independent drive via the CPU mailbox.
TEST_F(ControllerMailbox4040Test, EverySectorOnBothDrivesTransfersExactDataThroughCpuMailboxes) {
    for (unsigned drive = 0; drive < CONTROLLER4040_DRIVES; ++drive) {
        unsigned linear = 0;
        for (unsigned track = 1; track <= 35; ++track) {
            const unsigned count = track <= 17 ? 21 : track <= 24 ? 19 : track <= 30 ? 18 : 17;
            for (unsigned sector = 0; sector < count; ++sector, ++linear) {
                SCOPED_TRACE(drive);
                SCOPED_TRACE(linear);
                submit(CommandWrite | drive, track, sector);
                ASSERT_EQ(machine.riot_ram[0x70], ControllerOk);
                for (unsigned byte = 0; byte < ROM4040_SECTOR_SIZE; ++byte)
                    ASSERT_EQ(disks[drive].bytes[linear * ROM4040_SECTOR_SIZE + byte],
                        static_cast<uint8_t>(byte ^ (0xa5 + drive)));
                submit(CommandRead | drive, track, sector);
                ASSERT_EQ(machine.riot_ram[0x70], ControllerOk);
                for (unsigned byte = 0; byte < ROM4040_SECTOR_SIZE; ++byte)
                    ASSERT_EQ(machine.shared[0x100 + byte], static_cast<uint8_t>(byte ^ (0xa5 + drive)));
                submit(CommandVerify | drive, track, sector);
                ASSERT_EQ(machine.riot_ram[0x70], ControllerOk);
                ASSERT_EQ(machine.jobs, 1u);
                ASSERT_EQ(machine.pins & (M6502_IRQ | M6502_NMI), 0u);
            }
        }
        EXPECT_EQ(disks[drive].writes, 683u);
        EXPECT_EQ(disks[drive].reads, 1366u);
        EXPECT_TRUE(std::all_of(disks[1 - drive].bytes.begin(), disks[1 - drive].bytes.end(),
            [drive](uint8_t byte) { return drive != 0 || byte == 0; }));
    }
}

// Header errors, ID, missing media, protection and failed I/O complete without interrupts.
TEST_F(ControllerMailbox4040Test, BothDrivesPublishEverySupportedErrorAndNonDataJobThroughCpuBus) {
    for (unsigned drive = 0; drive < CONTROLLER4040_DRIVES; ++drive) {
        for (const uint8_t operation : {CommandRead, CommandWrite, CommandVerify}) {
            submit(operation | drive, 18, 0, true, false);
            EXPECT_EQ(machine.riot_ram[0x70], NoSync);
            submit(operation | drive, 18, 0, true, true, false);
            EXPECT_EQ(machine.riot_ram[0x70], IdMismatch);
            for (unsigned track = 0; track < 256; ++track) {
                const unsigned count = track == 0 || track > 35 ? 0
                    : track <= 17 ? 21 : track <= 24 ? 19 : track <= 30 ? 18 : 17;
                for (unsigned sector = count; sector < 256; ++sector) {
                    submit(operation | drive, track, sector);
                    ASSERT_EQ(machine.riot_ram[0x70], track > 35 ? NoSync : HeaderError)
                        << "drive=" << drive << " track=" << track << " sector=" << sector;
                }
            }
            disks[drive].fail = true;
            submit(operation | drive, 18, 0);
            EXPECT_EQ(machine.riot_ram[0x70], DataError);
            disks[drive].fail = false;
        }
        submit(CommandWrite | drive, 18, 0, false);
        EXPECT_EQ(machine.riot_ram[0x70], WriteProtected);
        submit(CommandVerify | drive, 18, 0);
        EXPECT_EQ(machine.riot_ram[0x70], VerifyError);
        submit(CommandSeek | drive, 0, 0);
        EXPECT_EQ(machine.riot_ram[0x70], ControllerOk);
        EXPECT_EQ(machine.shared[0x21], 'I');
        EXPECT_EQ(machine.shared[0x22], 'D');
        EXPECT_EQ(machine.shared[0x24], 1);
        submit(CommandBump | drive, 0, 0);
        EXPECT_EQ(machine.riot_ram[0x70], ControllerOk);
        EXPECT_EQ(machine.shared[0x23], 0);
        EXPECT_EQ(machine.pins & (M6502_IRQ | M6502_NMI), 0u);
    }
}

// Keep one machine alive across randomized slots, drives, failures and polling gaps.
TEST_F(ControllerMailbox4040Test, SeededMailboxHistoriesConserveMemoryMediaAndCompletionOwnership) {
    constexpr unsigned Requests = 64, MaxPollingGap = 512;
    constexpr unsigned TrackSectors = 21, ShortTracks = 17;
    constexpr uint8_t IdleJob = 0x20;
    constexpr std::array<uint8_t, 3> Operations{CommandRead, CommandWrite, CommandVerify};
    enum FailureMode { None, Absent, Protected, BadId, IoError, ModeCount };
    for (const unsigned seed : {1u, 0x4040u, 0xc0ffeeu}) {
        SCOPED_TRACE("mailbox history seed=" + std::to_string(seed));
        std::mt19937 random(seed);
        std::array<std::vector<uint8_t>, CONTROLLER4040_DRIVES> expected_media;
        for (unsigned drive = 0; drive < CONTROLLER4040_DRIVES; ++drive) {
            std::fill(disks[drive].bytes.begin(), disks[drive].bytes.end(), 0);
            disks[drive].fail = false;
            expected_media[drive] = disks[drive].bytes;
        }
        program.clear();
        run();
        controller = {.machine = &machine};
        machine.running = true;
        machine.job = controller4040_job;
        machine.context = &controller;
        for (unsigned request = 0; request < Requests; ++request) {
            SCOPED_TRACE("request=" + std::to_string(request));
            const unsigned drive = random() % CONTROLLER4040_DRIVES;
            const unsigned slot = random() % ROM4040_JOB_COUNT;
            const uint8_t operation = Operations[random() % Operations.size()];
            const unsigned track = 1 + random() % ShortTracks, sector = random() % TrackSectors;
            const unsigned offset = ((track - 1) * TrackSectors + sector) * ROM4040_SECTOR_SIZE;
            const unsigned buffer = (slot + 1) * ROM4040_SECTOR_SIZE, header = 0x21 + slot * 8;
            const auto failure = static_cast<FailureMode>(random() % ModeCount);
            for (unsigned device = 0; device < CONTROLLER4040_DRIVES; ++device) {
                disks[device].fail = failure == IoError && device == drive;
                controller.disk[device] = {.io = io, .context = &disks[device],
                    .writable = failure != Protected, .id = {'I', 'D'}};
            }
            if (failure == Absent) controller.disk[drive] = {};
            std::fill_n(machine.shared + 3, ROM4040_JOB_COUNT, IdleJob);
            for (unsigned byte = 0; byte < ROM4040_SECTOR_SIZE; ++byte)
                machine.shared[buffer + byte] = operation == CommandVerify
                    ? expected_media[drive][offset + byte] : static_cast<uint8_t>(random());
            const bool mismatch = operation == CommandVerify && (random() & 1);
            if (mismatch) machine.shared[buffer] ^= 1;
            const uint8_t result = failure == Absent ? NoSync
                : failure == BadId ? IdMismatch
                : failure == Protected && operation == CommandWrite ? WriteProtected
                : failure == IoError ? DataError
                : mismatch ? VerifyError : ControllerOk;
            std::array<uint8_t, ROM4040_SHARED_SIZE> expected;
            std::copy_n(machine.shared, expected.size(), expected.begin());
            expected[header] = failure == BadId ? 'X' : 'I';
            expected[header + 1] = 'D';
            expected[header + 2] = track;
            expected[header + 3] = sector;
            expected[3 + slot] = result;
            if (failure != Absent && failure != BadId
                && !(failure == Protected && operation == CommandWrite))
                expected[header + 4] = expected[header] ^ expected[header + 1]
                    ^ expected[header + 2] ^ expected[header + 3];
            if (result == ControllerOk && operation == CommandRead)
                std::copy_n(expected_media[drive].begin() + offset, ROM4040_SECTOR_SIZE,
                    expected.begin() + buffer);
            if (result == ControllerOk && operation == CommandWrite)
                std::copy_n(expected.begin() + buffer, ROM4040_SECTOR_SIZE,
                    expected_media[drive].begin() + offset);
            next([&] {
                store(HeaderBase + slot * 8, expected[header]);
                store(HeaderBase + slot * 8 + 1, 'D');
                store(HeaderBase + slot * 8 + 2, track);
                store(HeaderBase + slot * 8 + 3, sector);
                store(JobBase + slot, operation | drive);
                absolute(LdaAbsolute, JobBase + slot);
                absolute(StaAbsolute, 0x0070);
            });
            resume(1 + random() % MaxPollingGap);
            ASSERT_EQ(machine.riot_ram[0x70], result);
            ASSERT_EQ(machine.jobs, request + 1);
            ASSERT_EQ(machine.pins & (M6502_IRQ | M6502_NMI), 0u);
            ASSERT_TRUE(std::equal(expected.begin(), expected.end(), machine.shared));
            for (unsigned device = 0; device < CONTROLLER4040_DRIVES; ++device)
                ASSERT_EQ(disks[device].bytes, expected_media[device]);
        }
    }
}
} // namespace

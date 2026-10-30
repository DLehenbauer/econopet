// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

extern "C" {
#include "ieee/rom4040.h"
}

namespace {
constexpr uint16_t Code = 0xd000, Result = 0x0070, Riot = 0x0200;
constexpr unsigned AccessCycles = 40, WaitCycles = 1000000;

// Execute reference transactions through the production CPU bus rather than private RIOT APIs.
class Replay {
public:
    // Install a synthetic program with no DOS dependency or hardware substitutions.
    Replay() {
        rom_[0x2ffc] = static_cast<uint8_t>(Code);
        rom_[0x2ffd] = static_cast<uint8_t>(Code >> 8);
    }

    // Restart only CPU execution, preserving peripheral state between transactions.
    uint8_t access(bool write, unsigned reg, unsigned value) {
        if (reg >= 32 || value >= 256) throw std::runtime_error("invalid reference transaction");
        const uint16_t address = Riot + reg;
        size_t used = 0;
        if (write) {
            rom_[used++] = 0xa9;
            rom_[used++] = value;
            rom_[used++] = 0x8d;
        } else rom_[used++] = 0xad;
        rom_[used++] = static_cast<uint8_t>(address);
        rom_[used++] = static_cast<uint8_t>(address >> 8);
        if (!write) {
            rom_[used++] = 0x8d;
            rom_[used++] = static_cast<uint8_t>(Result);
            rom_[used++] = static_cast<uint8_t>(Result >> 8);
        }
        const uint16_t stop = Code + used;
        rom_[used++] = 0x4c;
        rom_[used++] = static_cast<uint8_t>(stop);
        rom_[used++] = static_cast<uint8_t>(stop >> 8);
        if (!initialized_) {
            rom4040_init(&machine_, rom_.data(), nullptr, nullptr);
            initialized_ = true;
        } else {
            const m6502_desc_t description{};
            machine_.pins = m6502_init(&machine_.cpu, &description);
        }
        rom4040_run(&machine_, AccessCycles);
        if (machine_.fault != ROM4040_FAULT_NONE) throw std::runtime_error("RIOT replay fault");
        return machine_.riot_ram[Result];
    }

    // Allow expiration without comparing elapsed time or free-running timer phase.
    void wait() {
        if (!initialized_) throw std::runtime_error("reference wait precedes initialization");
        rom4040_run(&machine_, WaitCycles);
        if (machine_.fault != ROM4040_FAULT_NONE) throw std::runtime_error("RIOT wait fault");
    }

private:
    std::array<uint8_t, ROM4040_ROM_SIZE> rom_{};
    rom4040_t machine_{};
    bool initialized_ = false;
};
}

// Reject every mismatch with its source line, keeping independent reference failure explicit.
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: riot_reference observations.txt");
        std::ifstream input(argv[1]);
        if (!input) throw std::runtime_error("cannot open independent RIOT observations");
        Replay replay;
        std::string record;
        unsigned line = 0, reads = 0, comparisons = 0;
        bool complete = false;
        while (std::getline(input, record)) {
            ++line;
            if (complete) throw std::runtime_error("trailing reference observations");
            std::istringstream fields(record);
            std::string operation, extra;
            fields >> operation;
            if (operation == "DONE") {
                if (fields >> extra) throw std::runtime_error("invalid completion record");
                complete = true;
                continue;
            }
            if (operation == "WAIT") {
                if (fields >> extra) throw std::runtime_error("invalid wait record");
                replay.wait();
                continue;
            }
            unsigned reg, value, mask;
            if (operation == "W") {
                if (!(fields >> reg >> value) || fields >> extra)
                    throw std::runtime_error("invalid write record");
                replay.access(true, reg, value);
            } else if (operation == "R") {
                if (!(fields >> reg >> value >> mask) || value >= 256 || mask >= 256
                    || fields >> extra) throw std::runtime_error("invalid read record");
                const unsigned actual = replay.access(false, reg, 0);
                if ((actual & mask) != (value & mask))
                    throw std::runtime_error("independent RIOT mismatch line "
                        + std::to_string(line) + " register " + std::to_string(reg)
                        + " actual " + std::to_string(actual) + " expected "
                        + std::to_string(value) + " mask " + std::to_string(mask));
                ++reads;
                if (mask != 0) ++comparisons;
            } else throw std::runtime_error("unknown independent RIOT record");
        }
        if (!input.eof() || !complete || comparisons == 0)
            throw std::runtime_error("incomplete reference observations");
        std::cout << "independent RIOT masked comparisons matched: " << comparisons
            << " (read transactions: " << reads << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

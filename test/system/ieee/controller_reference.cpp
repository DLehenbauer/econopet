// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include "ieee/controller4040.h"
}

namespace {
constexpr uint16_t Code = 0xd800, JobBase = 0x1003;
constexpr unsigned AccessCycles = 64;

// Require an exact-size external image before using it as a controller backend.
std::vector<uint8_t> load(const std::filesystem::path& path, size_t size) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open controller input: " + path.string());
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
    if (input.bad() || bytes.size() != size) throw std::runtime_error("controller input size mismatch");
    return bytes;
}

// Fail explicitly on short or failed persisted media output.
void save(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream output;
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.open(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    output.close();
}

// Decode complete byte strings without allowing partial or out-of-range values.
std::vector<uint8_t> unhex(const std::string& hex, size_t size) {
    if (hex.size() != size * 2 || hex.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::runtime_error("invalid controller byte string");
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.size(); i += 2)
        bytes.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return bytes;
}

// Serialize every header and buffer byte into a stable, comparable record.
std::string hex(const uint8_t* bytes, size_t size) {
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (size_t i = 0; i < size; ++i) result << std::setw(2) << unsigned(bytes[i]);
    return result.str();
}

// Own both independent images and retain the same production controller across jobs.
class Replay {
public:
    // Load only manifest-verified inputs already staged by the Python runner.
    Replay(const std::filesystem::path& root) {
        for (unsigned drive = 0; drive < CONTROLLER4040_DRIVES; ++drive)
            media_[drive] = load(root / ("initial" + std::to_string(drive) + ".d64"),
                CONTROLLER4040_IMAGE_SIZE);
        constexpr std::array<const char*, 3> Parts{"901468-14.bin", "901468-15.bin", "901468-16.bin"};
        for (size_t part = 0; part < Parts.size(); ++part) {
            const auto bytes = load(root / Parts[part], ROM4040_ROM_SIZE / Parts.size());
            std::copy(bytes.begin(), bytes.end(), rom_.begin() + part * bytes.size());
        }
        rom4040_init(&machine_, rom_.data(), controller4040_job, &controller_);
        machine_.running = true;
        controller_.machine = &machine_;
        std::ifstream mode(root / "mode.txt");
        unsigned writable[2], present[2];
        std::string extra;
        if (!(mode >> writable[0] >> writable[1] >> present[0] >> present[1]) || mode >> extra)
            throw std::runtime_error("invalid reference media mode");
        for (unsigned drive = 0; drive < CONTROLLER4040_DRIVES; ++drive)
            if (present[drive])
                controller_.disk[drive] = {.io = io, .context = &media_[drive],
                    .writable = writable[drive] != 0, .id = {'I', 'D'}};
    }

    // Submit the job byte using actual CPU instructions and normal mailbox completion.
    std::string job(const std::string& record) {
        std::istringstream input(record);
        std::string name, header_text, buffer_text, extra;
        unsigned slot, command;
        if (!(input >> name >> slot >> command >> header_text >> buffer_text) || input >> extra
            || slot >= ROM4040_JOB_COUNT || command < 128 || command > 255)
            throw std::runtime_error("invalid controller request");
        const auto header = unhex(header_text, 8);
        const auto buffer = unhex(buffer_text, ROM4040_SECTOR_SIZE);
        std::copy(header.begin(), header.end(), machine_.shared + 0x21 + slot * 8);
        std::copy(buffer.begin(), buffer.end(), machine_.shared + (slot + 1) * ROM4040_SECTOR_SIZE);
        if ((command & 0xfe) == 0xe0)
            std::copy_n(rom_.begin(), 0x300, machine_.shared + 0x100);
        const uint16_t address = JobBase + slot;
        constexpr uint8_t LdaImmediate = 0xa9, StaAbsolute = 0x8d, JumpAbsolute = 0x4c;
        const std::array<uint8_t, 8> code{LdaImmediate, static_cast<uint8_t>(command),
            StaAbsolute, static_cast<uint8_t>(address), static_cast<uint8_t>(address >> 8),
            JumpAbsolute, static_cast<uint8_t>(Code + 5), static_cast<uint8_t>(Code >> 8)};
        std::copy(code.begin(), code.end(), rom_.begin() + Code - 0xd000);
        rom_[0x2ffc] = static_cast<uint8_t>(Code);
        rom_[0x2ffd] = static_cast<uint8_t>(Code >> 8);
        const m6502_desc_t description{};
        machine_.pins = m6502_init(&machine_.cpu, &description);
        const auto jobs = machine_.jobs;
        rom4040_run(&machine_, AccessCycles);
        if (machine_.fault != ROM4040_FAULT_NONE || machine_.jobs != jobs + 1)
            throw std::runtime_error("controller submission fault: " + name);
        return name + " " + std::to_string(slot) + " "
            + std::to_string(machine_.shared[3 + slot]) + " "
            + hex(machine_.shared + 0x21 + slot * 8, 8) + " "
            + hex(machine_.shared + (slot + 1) * ROM4040_SECTOR_SIZE, ROM4040_SECTOR_SIZE);
    }

    // Persist both media for byte-exact comparison with MAME's GCR-decoded images.
    void finish(const std::filesystem::path& root) {
        for (unsigned drive = 0; drive < CONTROLLER4040_DRIVES; ++drive)
            save(root / ("production" + std::to_string(drive) + ".d64"), media_[drive]);
    }

private:
    // Access a bounded sector without substituting native controller completion behavior.
    static bool io(void* context, uint32_t offset, uint8_t* bytes, bool write) {
        auto& media = *static_cast<std::vector<uint8_t>*>(context);
        if (offset > media.size() - ROM4040_SECTOR_SIZE)
            throw std::runtime_error("controller media access out of bounds");
        if (write) std::copy_n(bytes, ROM4040_SECTOR_SIZE, media.begin() + offset);
        else std::copy_n(media.begin() + offset, ROM4040_SECTOR_SIZE, bytes);
        return true;
    }

    std::array<std::vector<uint8_t>, CONTROLLER4040_DRIVES> media_;
    std::array<uint8_t, ROM4040_ROM_SIZE> rom_{};
    rom4040_t machine_{};
    controller4040_t controller_{};
};
}

// Retain replay observations and media, and fail loudly on malformed or empty input.
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: controller_reference artifact-directory");
        const std::filesystem::path root(argv[1]);
        Replay replay(root);
        std::ifstream input(root / "requests.txt");
        if (!input) throw std::runtime_error("cannot open controller requests");
        std::ofstream output;
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output.open(root / "production.txt");
        std::string record;
        unsigned count = 0;
        while (std::getline(input, record)) {
            if (++count > 10000) throw std::runtime_error("oversized controller requests");
            output << replay.job(record) << '\n';
        }
        if (!input.eof() || count == 0) throw std::runtime_error("incomplete controller requests");
        output << "DONE\n";
        output.close();
        replay.finish(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

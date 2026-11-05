// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "framework/system_test.h"

namespace {
using namespace econopet;
using test_asm::imm;
constexpr CpuAddress Scanner{0x7672}, Translator{0x76d1};
constexpr CpuAddress CurrentRow{0x9327}, CurrentBit{0x9328};
constexpr Cycles ScanBudget{100000};
constexpr uint8_t Released = 0xff;
constexpr size_t SectorSize = 256, DirectoryTrack = 39, DirectorySector = 1;
constexpr size_t WaterlooHeaderSize = 6, LoadAddress = 0x6de1;

struct Key {
    unsigned row, bit;
};

// Locate one D80 sector independently of the media patcher.
size_t sector_offset(unsigned track, unsigned sector) {
    auto sectors = [](unsigned t) { return t <= 39 ? 29u : t <= 53 ? 27u : t <= 64 ? 25u : 23u; };
    if (track == 0 || track > 77 || sector >= sectors(track))
        throw std::runtime_error("invalid OS-9 disk sector");
    size_t offset = sector;
    for (unsigned t = 1; t < track; ++t) offset += sectors(t);
    return offset * SectorSize;
}

// Extract the loader from the generated disk without assuming contiguous sectors.
std::vector<uint8_t> loader() {
    const char* media = std::getenv("ECONOPET_MEDIA_DIR");
    if (!media || !*media) throw std::runtime_error("ECONOPET_MEDIA_DIR is not set");
    const auto path = std::filesystem::path{media} / "disks/superpet/os9/os9-systemdisk-n.d80";
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("missing graphics OS-9 disk: " + path.string());
    const std::vector<uint8_t> image{std::istreambuf_iterator<char>{input}, {}};
    if (image.size() != 533248) throw std::runtime_error("invalid OS-9 D80 size");
    const size_t entry = sector_offset(DirectoryTrack, DirectorySector) + 32;
    if (image.at(entry + 2) != 0x82 || image.at(entry + 5) != 'o'
        || image.at(entry + 6) != 's' || image.at(entry + 7) != '9')
        throw std::runtime_error("unexpected OS-9 loader directory entry");
    unsigned track = image.at(entry + 3), sector = image.at(entry + 4);
    std::vector<uint8_t> result;
    for (unsigned count = 0; track != 0 && count < 77 * 29; ++count) {
        const size_t offset = sector_offset(track, sector);
        const auto next_track = image.at(offset), next_sector = image.at(offset + 1);
        if (next_track == 0 && next_sector == 0) throw std::runtime_error("invalid final OS-9 sector");
        const size_t length = next_track ? SectorSize - 2 : next_sector - 1;
        result.insert(result.end(), image.begin() + offset + 2, image.begin() + offset + 2 + length);
        track = next_track;
        sector = next_sector;
    }
    if (track != 0 || result.size() != 4581) throw std::runtime_error("invalid OS-9 loader chain");
    return result;
}

class Os9Keyboard : public SystemTest {
  protected:
    // Install the real disk loader in its Waterloo load address.
    void SetUp() override {
        system.load_rom_set(System::RomSet::Waterloo);
        system.set_keyboard(pet_keyboard_model_graphics);
        matrix_.fill(Released);
        const auto program = loader();
        for (size_t i = WaterlooHeaderSize; i < program.size() - WaterlooHeaderSize; ++i)
            system.poke(SramAddress{LoadAddress + i - WaterlooHeaderSize}, program[i]);
    }

    // Exercise the native scanner and translate its captured raw matrix position.
    uint16_t scan(std::initializer_list<Key> keys, bool translate = true) {
        std::array<uint8_t, 10> next;
        next.fill(Released);
        for (const auto key : keys) next[key.row] &= ~(1u << key.bit);
        for (size_t row = 0; row < next.size(); ++row)
            if (next[row] != matrix_[row])
                system.spi().write(WishboneAddress{ECONOPET_WB_KBD_BASE_ADDR + row}, next[row]);
        matrix_ = next;
        auto test = system.program6809();
        const auto character = test.byte("character");
        const auto raw_row = test.byte("raw row"), raw_bit = test.byte("raw bit");
        constexpr std::array<uint8_t, 2> transfer_a_to_dp{0x1f, 0x8b};
        constexpr std::array<uint8_t, 3> load_b_current_bit{0xf6, 0x93, 0x28};
        const auto result = test.within(ScanBudget).run([&](auto& p) {
            p.lda(imm(0)).sta(pet::Pia1ControlA).sta(pet::Pia1ControlB).sta(pet::Pia1PortB);
            p.lda(imm(0x0f)).sta(pet::Pia1PortA);
            p.lda(imm(0x3c)).sta(pet::Pia1ControlA).sta(pet::Pia1ControlB);
            p.lda(imm(0xff)).sta(CpuAddress{0x9325}).sta(CpuAddress{0x9326})
                .sta(CurrentRow).sta(CurrentBit);
            p.lda(imm(0x93)).raw_bytes(transfer_a_to_dp).jsr(Scanner);
            p.sta(raw_row.address()).stb(raw_bit.address());
            if (translate)
                p.lda(CurrentRow).raw_bytes(load_b_current_bit).jsr(Translator).stb(character.address());
        });
        return translate ? result[character] : (result[raw_row] << 8) | result[raw_bit];
    }

  private:
    std::array<uint8_t, 10> matrix_;
};

// Test physical positions through the PIA and the patched 6809 instructions.
TEST_F(Os9Keyboard, GraphicsKeysAndModifiersUseNativeOs9Codes) {
    constexpr Key left_shift{8, 0}, right_shift{8, 5}, control{9, 0};
    EXPECT_EQ(scan({{4, 0}}), 'a');
    EXPECT_EQ(scan({{4, 0}, left_shift}), 'A');
    EXPECT_EQ(scan({{4, 0}, right_shift}), 'A');
    EXPECT_EQ(scan({{4, 0}, control}), 1);
    EXPECT_EQ(scan({{4, 0}, left_shift, control}), 1);
    EXPECT_EQ(scan({{6, 0}}), 'z');
    EXPECT_EQ(scan({{6, 6}}), '1');
    EXPECT_EQ(scan({{0, 0}}), '!');
    EXPECT_EQ(scan({{0, 0}, left_shift}), '1');
    EXPECT_EQ(scan({{9, 7}}), '=');
    EXPECT_EQ(scan({{6, 6}, right_shift}), 0x81);
    EXPECT_EQ(scan({}, false), 0xffff);
    EXPECT_EQ(scan({left_shift}, false), 0xffff);
    EXPECT_EQ(scan({right_shift}, false), 0xffff);
    EXPECT_EQ(scan({control}, false), 0xffff);
}
} // namespace

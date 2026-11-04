// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <array>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "framework/system_test.h"

extern "C" {
#include "config/config.h"
#include "test_support.h"
}

namespace {
using namespace econopet;
using test_asm::imm;
constexpr CpuAddress Scan{0xdef3}, Translate{0xde80}, CharacterMode{0x0128};
constexpr uint32_t TableStart = 0xdf40, TableEnd = 0xe084;
constexpr size_t TableSize = 81, MatrixColumns = 10, MatrixBits = 8;
constexpr Cycles ScanBudget{100000};
constexpr uint8_t Released = 0xff, ShiftOffset = TableSize;
constexpr char ConfigPath[] = "/waterloo-keyboard-config.yaml";

// Coordinates use KSCAN and KIN from docs/dev/PET/keyboard.md.
struct Key {
    unsigned column, bit;
};
constexpr Key GraphicsLeftShift{8, 0}, GraphicsRightShift{8, 5};

// Compare graphics keys with their native business-key translations, not patch bytes.
struct EquivalentKey {
    Key graphics, business;
    bool swapped = false;
};
constexpr std::array EquivalentKeys{
    EquivalentKey{{4, 0}, {3, 0}},       // A
    EquivalentKey{{2, 2}, {5, 2}},       // T
    EquivalentKey{{6, 3}, {8, 3}},       // M
    EquivalentKey{{7, 0}, {8, 1}},       // X
    EquivalentKey{{6, 0}, {7, 0}},       // Z (formerly a business Shift position)
    EquivalentKey{{6, 6}, {8, 7}},       // Keypad 1 (formerly a business Shift position)
    EquivalentKey{{8, 6}, {7, 4}},       // Keypad 0
    EquivalentKey{{4, 7}, {3, 7}},       // Keypad 6
    EquivalentKey{{2, 6}, {1, 4}},       // Keypad 7
    EquivalentKey{{2, 7}, {1, 7}},       // Keypad 9
    EquivalentKey{{9, 6}, {6, 4}},       // Keypad decimal point
    EquivalentKey{{6, 5}, {3, 4}},       // Return
    EquivalentKey{{9, 4}, {9, 4}},       // Stop
    EquivalentKey{{0, 6}, {8, 4}},       // Home
    EquivalentKey{{0, 7}, {0, 5}},       // Right
    EquivalentKey{{1, 6}, {5, 4}},       // Down
    EquivalentKey{{1, 7}, {4, 7}},       // Delete
    EquivalentKey{{1, 5}, {4, 0}},       // USB-only Tab position in the existing graphics map
    EquivalentKey{{9, 5}, {7, 6}},       // USB-only Repeat position
    EquivalentKey{{9, 0}, {8, 0}},       // RVS remains unused by Waterloo
    EquivalentKey{{3, 5}, {9, 7}},       // Unpopulated position
    EquivalentKey{{8, 1}, {3, 6}},       // @
    EquivalentKey{{9, 1}, {5, 6}},       // [
    EquivalentKey{{8, 2}, {2, 4}},       // ]
    EquivalentKey{{1, 3}, {4, 4}},       // Backslash
    EquivalentKey{{2, 5}, {1, 5}},       // Up arrow
    EquivalentKey{{0, 5}, {9, 0}},       // Back arrow
    EquivalentKey{{8, 7}, {0, 3}},       // -
    EquivalentKey{{3, 7}, {8, 6}},       // /
    EquivalentKey{{9, 7}, {0, 3}, true}, // = (reverse the native -/= pair)
    EquivalentKey{{7, 7}, {2, 6}, true}, // +
    EquivalentKey{{5, 7}, {9, 5}, true}, // *
    EquivalentKey{{9, 3}, {7, 3}, true}, // <
    EquivalentKey{{8, 4}, {6, 3}, true}, // >
    EquivalentKey{{7, 4}, {8, 6}, true}, // ?
    EquivalentKey{{0, 0}, {1, 0}, true}, // !
    EquivalentKey{{1, 0}, {0, 0}, true}, // "
    EquivalentKey{{1, 2}, {1, 2}, true}, // '
};

// Load exact revision-12 media and execute only the selected config's patch callbacks.
class WaterlooKeyboard : public SystemTest, public testing::WithParamInterface<const char*> {
  protected:
    // Register the shipped configuration without replacing media files or firmware state.
    void SetUp() override {
        system.load_rom_set(System::RomSet::Waterloo);
        std::ifstream file(ECONOPET_SDCARD_CONFIG);
        ASSERT_TRUE(file);
        const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        test_register_file(ConfigPath, text.c_str());
        registered_ = true;
        matrix_.fill(Released);
        original_ = rom_bytes();
    }

    // Remove only this fixture's registered file and preserve board failure diagnostics.
    void TearDown() override {
        if (registered_)
            test_unregister_file(ConfigPath);
        SystemTest::TearDown();
    }

    // Snapshot the ROM window so untouched instructions, tables and vectors can be checked.
    std::vector<uint8_t> rom_bytes() const {
        std::vector<uint8_t> bytes;
        for (uint32_t address = 0xa000; address < 0x10000; ++address)
            bytes.push_back(system.peek(SramAddress{address}));
        return bytes;
    }

    // Find SuperPET by ID so configuration reordering cannot silently test another model.
    void apply(pet_keyboard_model_t keyboard) {
        struct Selection {
            int current = 0, selected = CONFIG_ENUMERATE;
            const char* id;
        } selection{.id = GetParam()};
        const config_sink_t enumerate{
            .context = &selection,
            .on_exit_config =
                [](void* context, const char* id, const char*) {
                    auto& found = *static_cast<Selection*>(context);
                    if (std::string{id} == found.id)
                        found.selected = found.current;
                    ++found.current;
                },
        };
        parse_config_file(ConfigPath, &enumerate, CONFIG_ENUMERATE);
        ASSERT_GE(selection.selected, 0);

        system_state_t state{};
        state.pet_keyboard_model = keyboard;
        const setup_sink_t setup{
            .context = this,
            .on_patch =
                [](void* context, uint32_t address, const binary_t* patch) {
                    auto& fixture = *static_cast<WaterlooKeyboard*>(context);
                    ASSERT_GE(address, TableStart);
                    ASSERT_LE(address + patch->size, TableEnd);
                    for (size_t i = 0; i < patch->size; ++i)
                        fixture.system.poke(SramAddress{address + i}, patch->data[i]);
                    fixture.patched_bytes_ += patch->size;
                },
            .system_state = &state,
        };
        const config_sink_t sink{.setup = &setup};
        parse_config_file(ConfigPath, &sink, selection.selected);
        system.set_keyboard(keyboard);
    }

    // Feed the real keyboard peripheral through SPI, then run the unchanged 6809 scanner.
    std::array<uint8_t, 2> scan(std::initializer_list<Key> keys, uint8_t mode) {
        std::array<uint8_t, MatrixColumns> next;
        next.fill(Released);
        for (const auto key : keys)
            next[key.column] &= ~(1u << key.bit);
        for (size_t column = 0; column < next.size(); ++column)
            if (next[column] != matrix_[column])
                system.spi().write(WishboneAddress{ECONOPET_WB_KBD_BASE_ADDR + column}, next[column]);
        matrix_ = next;

        auto test = system.program6809();
        const auto index = test.byte("scan index"), character = test.byte("translated character");
        const auto result = test.within(ScanBudget).run([&](auto& p) {
            // Select keyboard outputs on PIA1 A and input bits on PIA1 B.
            p.lda(imm(0)).sta(pet::Pia1ControlA).sta(pet::Pia1ControlB).sta(pet::Pia1PortB);
            p.lda(imm(0x0f)).sta(pet::Pia1PortA);
            p.lda(imm(0x3c)).sta(pet::Pia1ControlA).sta(pet::Pia1ControlB);
            p.lda(imm(mode)).sta(CharacterMode);
            p.jsr(Scan).stb(index.address()).jsr(Translate).stb(character.address());
        });
        return {result[index], result[character]};
    }

    // Read a known physical business key's original translation in either character mode.
    uint8_t native(Key key, uint8_t mode, bool shifted) const {
        const uint32_t base = mode == Released ? 0xdfe2 : TableStart;
        const size_t index = MatrixColumns * MatrixBits - key.column * MatrixBits - key.bit;
        return original_[base - 0xa000 + index + (shifted ? ShiftOffset : 0)];
    }

    std::vector<uint8_t> original_;
    size_t patched_bytes_ = 0;

  private:
    bool registered_ = false;
    std::array<uint8_t, MatrixColumns> matrix_;
};

// Business keyboards retain all original ROM bytes and native scan positions.
TEST_P(WaterlooKeyboard, BusinessConfigurationLeavesRomsUnchanged) {
    apply(pet_keyboard_model_business);
    EXPECT_EQ(patched_bytes_, 0u);
    EXPECT_EQ(rom_bytes(), original_);
    EXPECT_EQ(scan({{3, 0}}, 0)[1], 'a');
    EXPECT_EQ(scan({{3, 0}, {6, 0}}, 0)[1], 'A');
}

// Both Shift keys and character modes preserve native key meanings at graphics positions.
TEST_P(WaterlooKeyboard, GraphicsConfigurationRelocatesNativeTranslations) {
    apply(pet_keyboard_model_graphics);
    EXPECT_EQ(patched_bytes_, 4 * TableSize);
    const auto patched = rom_bytes();
    for (size_t i = 0; i < patched.size(); ++i)
        if (i + 0xa000 < TableStart || i + 0xa000 >= TableEnd)
            ASSERT_EQ(patched[i], original_[i]) << std::hex << i + 0xa000;

    for (const uint8_t mode : {uint8_t{0}, Released}) {
        for (const auto& key : EquivalentKeys) {
            SCOPED_TRACE(testing::Message() << "KSCAN=" << key.graphics.column << " KIN=" << key.graphics.bit
                                            << " mode=" << unsigned(mode));
            const auto plain = scan({key.graphics}, mode);
            EXPECT_EQ(plain[1], native(key.business, mode, key.swapped));
            EXPECT_EQ(plain[0], MatrixColumns * MatrixBits - key.graphics.column * MatrixBits - key.graphics.bit);
            for (const auto shift : {GraphicsLeftShift, GraphicsRightShift}) {
                const auto shifted = scan({key.graphics, shift}, mode);
                EXPECT_EQ(shifted[1], native(key.business, mode, !key.swapped));
                EXPECT_EQ(shifted[0], plain[0] + ShiftOffset);
            }
        }
    }
}

// Zero sentinels keep idle and modifier-only scans silent, including both Shifts together.
TEST_P(WaterlooKeyboard, GraphicsShiftOnlyAndReleaseProduceNoCharacter) {
    apply(pet_keyboard_model_graphics);
    for (const uint8_t mode : {uint8_t{0}, Released}) {
        EXPECT_EQ(scan({}, mode), (std::array<uint8_t, 2>{0, 0}));
        EXPECT_EQ(scan({GraphicsLeftShift}, mode), (std::array<uint8_t, 2>{ShiftOffset, 0}));
        EXPECT_EQ(scan({GraphicsRightShift}, mode), (std::array<uint8_t, 2>{ShiftOffset, 0}));
        EXPECT_EQ(scan({GraphicsLeftShift, GraphicsRightShift}, mode), (std::array<uint8_t, 2>{ShiftOffset, 0}));
        EXPECT_EQ(scan({{4, 0}, GraphicsLeftShift, GraphicsRightShift}, mode)[1], 'A');
        EXPECT_EQ(scan({}, mode), (std::array<uint8_t, 2>{0, 0}));
    }
}

INSTANTIATE_TEST_SUITE_P(VideoStandards, WaterlooKeyboard,
                         testing::Values("superpet-6809-50hz", "superpet-6809-60hz"));
} // namespace

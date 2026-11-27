// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "framework/firmware_test.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

extern "C" {
#include "breakpoint.h"
#include "sd/sd.h"
#include "tape.h"
#include "test_support.h"
}

using econopet::SramAddress;

namespace {

/** Byte length of PET Cassette #1 tape buffer ($027A..$0339) saved/restored by tape.c. */
constexpr size_t TapeBufferBytes = 192;

/** Captured breakpoint registration slot used to invoke tape.c callbacks directly in tests. */
struct FakeBreakpoint {
    /** 6502 address where the breakpoint was armed. */
    uint16_t address{};
    /** Registered callback invoked when execution reaches address. */
    bp_callback_t callback{};
    /** Opaque context pointer passed to callback. */
    void* context{};
};

std::array<FakeBreakpoint, 2> fake_breakpoints{};
bool fake_directory_active{};
bool fake_directory_emitted{};
struct dirent fake_directory_entry{};

/** Look up an active fake breakpoint slot by 6502 address, or return nullptr. */
FakeBreakpoint* find_breakpoint(const uint16_t address) {
    for (auto& entry : fake_breakpoints) {
        if (entry.callback != nullptr && entry.address == address) {
            return &entry;
        }
    }
    return nullptr;
}

/** Return the synthetic DIR* handle representing the intercepted /prg/ directory. */
DIR* fake_directory_handle() { return reinterpret_cast<DIR*>(&fake_directory_active); }

/** RAII guard that tears down tape.c state, temporary breakpoints, and registered test files. */
class TapeCallbackCleanup {
  public:
    /** Bind cleanup to path and the LD210 return_breakpoint address. */
    explicit TapeCallbackCleanup(const char* path, const uint16_t return_breakpoint)
        : path_(path), return_breakpoint_(return_breakpoint) {}

    /** Deinitialize virtual tape, clear any lingering return breakpoint, and unregister path_. */
    ~TapeCallbackCleanup() {
        tape_deinit();
        if (find_breakpoint(return_breakpoint_) != nullptr) {
            bp_remove(return_breakpoint_);
        }
        if (registered_) {
            test_unregister_file(path_);
        }
    }

    /** Register image in the host filesystem shim at path_ and arm automatic unregistration. */
    void register_file(const std::vector<uint8_t>& image) {
        test_register_binary_file(path_, image.data(), image.size(), false);
        registered_ = true;
    }

  private:
    const char* path_;
    uint16_t return_breakpoint_;
    bool registered_{};
};

} // namespace

extern "C" DIR* __real_opendir(const char* path);
extern "C" struct dirent* __real_readdir(DIR* directory);
extern "C" int __real_closedir(DIR* directory);

extern "C" {

/** Capture the production tape callback in fake_breakpoints[] at address. */
void bp_set(const uint16_t address, const bp_callback_t callback, void* context) {
    // Reject null or duplicate registrations and store the callback in the first free slot.
    if (callback == nullptr || find_breakpoint(address) != nullptr) {
        std::abort();
    }
    for (auto& entry : fake_breakpoints) {
        if (entry.callback == nullptr) {
            entry = {address, callback, context};
            return;
        }
    }
    std::abort();
}

/** Retire one captured breakpoint callback at address. */
bool bp_remove(const uint16_t address) {
    FakeBreakpoint* const entry = find_breakpoint(address);
    if (entry == nullptr) {
        return false;
    }
    *entry = {};
    return true;
}

/** Fail explicitly if tape.c unexpectedly requests synthetic directory free-block space. */
uint64_t sd_free_bytes(void) {
    std::fputs("unexpected directory-listing request in tape file-load test\n", stderr);
    std::abort();
}

/** Intercept opendir("/prg/") so case-insensitive PRG lookup finds drive-test.prg in memory. */
DIR* __wrap_opendir(const char* path) {
    if (std::strcmp(path, "/prg/") == 0) {
        fake_directory_active = true;
        fake_directory_emitted = false;
        return fake_directory_handle();
    }
    return __real_opendir(path);
}

/** Yield a single synthetic "drive-test.prg" entry when iterating fake_directory_handle(). */
struct dirent* __wrap_readdir(DIR* directory) {
    if (directory != fake_directory_handle()) {
        return __real_readdir(directory);
    }
    if (!fake_directory_active || fake_directory_emitted) {
        return nullptr;
    }
    fake_directory_emitted = true;
    std::memset(&fake_directory_entry, 0, sizeof(fake_directory_entry));
    std::strcpy(fake_directory_entry.d_name, "drive-test.prg");
    return &fake_directory_entry;
}

/** Close fake_directory_handle(), forwarding all other handles to __real_closedir(). */
int __wrap_closedir(DIR* directory) {
    if (directory != fake_directory_handle()) {
        return __real_closedir(directory);
    }
    fake_directory_active = false;
    return 0;
}

} // extern "C"

/** Execute the production tape callback and verify its load and LD210 restore behavior. */
TEST_F(FirmwareTest, DrivePrgLoadsThroughProductionTapeCallback) {
    constexpr char PrgPath[] = "/prg/drive-test.prg";
    constexpr uint16_t LoadBreakpoint = 0x03a0;
    constexpr uint16_t ReturnBreakpoint = 0x03b0;
    constexpr uint16_t FilenameAddress = 0x0380;
    constexpr uint8_t EndAddressLow = 0xc9;
    constexpr uint8_t EndAddressHigh = 0xca;
    constexpr uint8_t FilenameLength = 0xb7;
    constexpr uint8_t DeviceNumber = 0xba;
    constexpr uint8_t FilenamePointer = 0xbb;

    // Load the built drive-test.prg binary and register it at /prg/drive-test.prg.
    std::ifstream input(ECONOPET_DRIVE_TEST_PRG, std::ios::binary);
    ASSERT_TRUE(input);
    const std::vector<uint8_t> image{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    ASSERT_GT(image.size(), 2u);

    TapeCallbackCleanup cleanup(PrgPath, ReturnBreakpoint);
    cleanup.register_file(image);
    system.cpu().assert_reset();
    system.load_rom_set(test_rom::Set::Rom4);

    // Initialize tape.c with synthetic zero-page and breakpoint addresses, then populate KERNAL LOAD parameters.
    tape_config_t config{};
    config.ld210 = ReturnBreakpoint;
    config.bp_addr = LoadBreakpoint;
    config.eal = EndAddressLow;
    config.eah = EndAddressHigh;
    config.fnlen = FilenameLength;
    config.devnum = DeviceNumber;
    config.fnadr = FilenamePointer;
    tape_init(&config);

    system.poke(SramAddress{DeviceNumber}, 1);
    system.poke(SramAddress{FilenameLength}, 10);
    system.poke(SramAddress{FilenamePointer}, static_cast<uint8_t>(FilenameAddress));
    system.poke(SramAddress{FilenamePointer + 1}, static_cast<uint8_t>(FilenameAddress >> 8));
    constexpr std::string_view Name{"DRIVE-TEST"};
    for (size_t i = 0; i < Name.size(); ++i) {
        system.poke(SramAddress{static_cast<uint16_t>(FilenameAddress + i)}, static_cast<uint8_t>(Name[i]));
    }

    // Seed TAPE_BUFFER ($027A) with known bytes so LD210 restoration can be verified afterward.
    std::array<uint8_t, TapeBufferBytes> original_tape_buffer{};
    for (size_t i = 0; i < original_tape_buffer.size(); ++i) {
        original_tape_buffer[i] = static_cast<uint8_t>(i ^ 0xa5);
        system.poke(SramAddress{static_cast<uint16_t>(TAPE_BUFFER + i)}, original_tape_buffer[i]);
    }

    ASSERT_EQ(system.peek(SramAddress{FilenamePointer}), static_cast<uint8_t>(FilenameAddress));
    ASSERT_EQ(system.peek(SramAddress{FilenamePointer + 1}), static_cast<uint8_t>(FilenameAddress >> 8));
    for (size_t i = 0; i < Name.size(); ++i) {
        ASSERT_EQ(system.peek(SramAddress{static_cast<uint16_t>(FilenameAddress + i)}), static_cast<uint8_t>(Name[i]));
    }

    // Fire the LOAD breakpoint callback and confirm the PRG payload, EAL/EAH, and TAPE_BUFFER trampoline.
    FakeBreakpoint* const load = find_breakpoint(LoadBreakpoint);
    ASSERT_NE(load, nullptr);
    const bp_result_t loaded = load->callback(LoadBreakpoint, load->context);
    ASSERT_EQ(loaded.pc, TAPE_BUFFER);
    EXPECT_TRUE(loaded.rearm);

    const uint16_t load_address = static_cast<uint16_t>(image[0] | (image[1] << 8));
    const uint16_t end_address = static_cast<uint16_t>(load_address + image.size() - 2);
    EXPECT_EQ(system.peek(SramAddress{EndAddressLow}), static_cast<uint8_t>(end_address));
    EXPECT_EQ(system.peek(SramAddress{EndAddressHigh}), static_cast<uint8_t>(end_address >> 8));
    for (size_t i = 2; i < image.size(); ++i) {
        EXPECT_EQ(system.peek(SramAddress{static_cast<uint16_t>(load_address + i - 2)}), image[i])
            << "payload offset " << i - 2;
    }
    EXPECT_EQ(system.peek(SramAddress{TAPE_BUFFER}), 0xa2);
    EXPECT_EQ(system.peek(SramAddress{TAPE_BUFFER + 13}), 0x4c);
    EXPECT_EQ(system.peek(SramAddress{TAPE_BUFFER + 14}), static_cast<uint8_t>(ReturnBreakpoint));
    EXPECT_EQ(system.peek(SramAddress{TAPE_BUFFER + 15}), static_cast<uint8_t>(ReturnBreakpoint >> 8));

    // Fire the LD210 return breakpoint callback and confirm all 192 TAPE_BUFFER bytes are restored.
    FakeBreakpoint* const returning = find_breakpoint(ReturnBreakpoint);
    ASSERT_NE(returning, nullptr);
    const bp_result_t restored = returning->callback(ReturnBreakpoint, returning->context);
    EXPECT_EQ(restored.pc, ReturnBreakpoint);
    EXPECT_FALSE(restored.rearm);
    for (size_t i = 0; i < original_tape_buffer.size(); ++i) {
        EXPECT_EQ(system.peek(SramAddress{static_cast<uint16_t>(TAPE_BUFFER + i)}), original_tape_buffer[i])
            << "buffer offset " << i;
    }
    bp_remove(ReturnBreakpoint);
}

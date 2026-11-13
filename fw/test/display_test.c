// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "display_test.h"

#include "mock.h"
#include "system_state.h"

#define TEST_VIDEO_RAM_ADDRESS 0x8000u
#define TEST_VIDEO_RAM_UNIT_BYTES 1024u
#define TEST_VIDEO_COLOR_OFFSET 0x800u

void __real_display_task(void);

static const struct {
    pet_video_ram_mask_t mask;
    unsigned int transfers;
    struct {
        size_t offset, length;
    } ranges[2];
} display_ram_cases[] = {
    {pet_video_ram_mask_1kb, 1, {{0, TEST_VIDEO_RAM_UNIT_BYTES}}},
    {pet_video_ram_mask_2kb, 1, {{0, 2u * TEST_VIDEO_RAM_UNIT_BYTES}}},
    {pet_video_ram_mask_colourpet,
     2,
     {{0, TEST_VIDEO_RAM_UNIT_BYTES}, {TEST_VIDEO_COLOR_OFFSET, TEST_VIDEO_RAM_UNIT_BYTES}}},
    {pet_video_ram_mask_4kb, 1, {{0, 4u * TEST_VIDEO_RAM_UNIT_BYTES}}},
};

/** Exercise each video RAM layout in both transfer directions. */
START_TEST(test_display_ram) {
    const system_state_t saved_state = system_state;
    mock_reset();
    const size_t test_case = (size_t)_i / 2;
    const bool reading = (_i & 1) == 0;
    system_state.video_source = reading ? video_source_pet : video_source_firmware;
    system_state.terminal_session = terminal_session_cli;
    system_state_set_video_ram_mask(&system_state, display_ram_cases[test_case].mask);

    // Distinct payloads expose misplaced transfers and writes into inactive ranges.
    for (size_t offset = 0; offset < PET_MAX_VIDEO_RAM_BYTES; ++offset) {
        const uint8_t payload = (offset < TEST_VIDEO_COLOR_OFFSET ? 'A' : 'a') + offset % 16;
        mock_ram[TEST_VIDEO_RAM_ADDRESS + offset] = reading ? payload : UINT8_MAX;
        system_state.video_char_buffer[offset] = reading ? UINT8_MAX : payload;
    }

    __real_display_task();
    ck_assert_uint_eq(mock_spi_read_count(), reading ? display_ram_cases[test_case].transfers : 0);
    ck_assert_uint_eq(mock_spi_write_count(), reading ? 0 : display_ram_cases[test_case].transfers);

    for (size_t offset = 0; offset < PET_MAX_VIDEO_RAM_BYTES; ++offset) {
        bool active = false;
        for (size_t range = 0; range < count_of(display_ram_cases[test_case].ranges); ++range) {
            const size_t start = display_ram_cases[test_case].ranges[range].offset;
            const size_t length = display_ram_cases[test_case].ranges[range].length;
            active |= offset >= start && offset - start < length;
        }
        const uint8_t payload = (offset < TEST_VIDEO_COLOR_OFFSET ? 'A' : 'a') + offset % 16;
        const uint8_t actual =
            reading ? system_state.video_char_buffer[offset] : mock_ram[TEST_VIDEO_RAM_ADDRESS + offset];
        ck_assert_msg(actual == (active ? payload : UINT8_MAX),
                      "case %zu (%s), video offset 0x%zx: got 0x%02x",
                      test_case, reading ? "read" : "write", offset, actual);
    }
    system_state = saved_state;
}
END_TEST

Suite* display_suite(void) {
    Suite* const suite = suite_create("display");
    TCase* const synchronization = tcase_create("video RAM synchronization");
    tcase_add_loop_test(synchronization, test_display_ram, 0, 2 * count_of(display_ram_cases));
    suite_add_tcase(suite, synchronization);
    return suite;
}

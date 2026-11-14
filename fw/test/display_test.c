// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "display_test.h"

#include "display/display.h"
#include "display/window.h"
#include "mock.h"
#include "system/state.h"

void __real_display_task(void);

static const struct {
    pet_video_ram_mask_t mask;
    unsigned int transfers;
    struct {
        size_t offset, length;
    } ranges[2];
} display_ram_cases[] = {
    {pet_video_ram_mask_1kb, 1, {{0, VIDEO_RAM_UNIT_BYTES}}},
    {pet_video_ram_mask_2kb, 1, {{0, 2u * VIDEO_RAM_UNIT_BYTES}}},
    {pet_video_ram_mask_colourpet,
     2,
     {{0, VIDEO_RAM_UNIT_BYTES}, {VIDEO_COLOR_OFFSET, VIDEO_RAM_UNIT_BYTES}}},
    {pet_video_ram_mask_4kb, 1, {{0, 4u * VIDEO_RAM_UNIT_BYTES}}},
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
        const uint8_t payload = (offset < VIDEO_COLOR_OFFSET ? 'A' : 'a') + offset % 16;
        mock_ram[VIDEO_RAM_ADDRESS + offset] = reading ? payload : UINT8_MAX;
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
        const uint8_t payload = (offset < VIDEO_COLOR_OFFSET ? 'A' : 'a') + offset % 16;
        const uint8_t actual =
            reading ? system_state.video_char_buffer[offset] : mock_ram[VIDEO_RAM_ADDRESS + offset];
        ck_assert_msg(actual == (active ? payload : UINT8_MAX),
                      "case %zu (%s), video offset 0x%zx: got 0x%02x",
                      test_case, reading ? "read" : "write", offset, actual);
    }
    system_state = saved_state;
}
END_TEST

/** Clear every character without changing attributes, configuration, or ownership. */
START_TEST(test_display_clear_preserves_display_state) {
    const system_state_t saved_state = system_state;
    mock_reset();
    system_state.video_source = (_i & 1) ? video_source_firmware : video_source_pet;
    system_state.terminal_session =
        (_i & 2) ? terminal_session_remote : terminal_session_cli;
    system_state.pet_display_columns = pet_display_columns_80;
    system_state.video_text_mode = video_text_mode_lower;
    system_state_set_video_ram_mask(&system_state, pet_video_ram_mask_4kb);
    set_cpu(CPU_HALT);
    memset(system_state.video_char_buffer, 0xa5, sizeof(system_state.video_char_buffer));
    memset(mock_ram + VIDEO_RAM_ADDRESS, 0x5a, sizeof(system_state.video_char_buffer));

    const video_source_t source = system_state.video_source;
    const terminal_session_t terminal_session = system_state.terminal_session;
    const pet_display_columns_t columns = system_state.pet_display_columns;
    const pet_video_ram_mask_t mask = system_state.video_ram_mask;
    const video_text_mode_t text_mode = system_state.video_text_mode;
    const cpu_state_t cpu = get_cpu();
    char* terminal_output = NULL;
    size_t terminal_output_size = 0;
    FILE* const saved_stdout = stdout;
    stdout = open_memstream(&terminal_output, &terminal_output_size);
    ck_assert_ptr_nonnull(stdout);

    display_clear();
    ck_assert_int_eq(fflush(stdout), 0);
    ck_assert_int_eq(fclose(stdout), 0);
    stdout = saved_stdout;

    ck_assert_uint_eq(mock_spi_read_count(), 0);
    ck_assert_uint_eq(mock_spi_write_count(), 1);
    ck_assert(terminal_session == terminal_session_remote
        ? terminal_output_size > 0 : terminal_output_size == 0);
    for (size_t offset = 0; offset < VIDEO_PLANE_CAPACITY; ++offset) {
        ck_assert_uint_eq(system_state.video_char_buffer[offset], CH_SPACE);
        ck_assert_uint_eq(mock_ram[VIDEO_RAM_ADDRESS + offset], CH_SPACE);
    }
    for (size_t offset = VIDEO_PLANE_CAPACITY;
         offset < sizeof(system_state.video_char_buffer); ++offset) {
        ck_assert_uint_eq(system_state.video_char_buffer[offset], 0xa5);
        ck_assert_uint_eq(mock_ram[VIDEO_RAM_ADDRESS + offset], 0x5a);
    }
    ck_assert_int_eq(system_state.video_source, source);
    ck_assert_int_eq(system_state.terminal_session, terminal_session);
    ck_assert_int_eq(system_state.pet_display_columns, columns);
    ck_assert_int_eq(system_state.video_ram_mask, mask);
    ck_assert_int_eq(system_state.video_text_mode, text_mode);
    ck_assert_int_eq(get_cpu(), cpu);
    free(terminal_output);
    system_state = saved_state;
}
END_TEST

Suite* display_suite(void) {
    Suite* const suite = suite_create("display");
    TCase* const synchronization = tcase_create("video RAM synchronization");
    tcase_add_loop_test(synchronization, test_display_ram, 0, 2 * count_of(display_ram_cases));
    tcase_add_loop_test(synchronization, test_display_clear_preserves_display_state, 0, 4);
    suite_add_tcase(suite, synchronization);
    return suite;
}

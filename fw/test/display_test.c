// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "display_test.h"

#include "display/char_encoding.h"
#include "display/display.h"
#include "display/dvi/crtc.h"
#include "display/window.h"
#include "mock.h"
#include "roms/roms.h"
#include "system/state.h"
#include "ui/cli.h"
#include "ui/console.h"

void __real_display_task(void);
void __real_display_term_begin(void);
void __real_display_term_end(void);
void __real_display_term_refresh(void);

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

/** Local display initialization must not access the FPGA in either column mode. */
START_TEST(test_display_init_local) {
    const system_state_t saved_state = system_state;
    mock_reset();
    const pet_display_columns_t columns = _i ? pet_display_columns_80 : pet_display_columns_40;
    const uint8_t attr = DISPLAY_ATTR(DISPLAY_COLOR_BLUE, DISPLAY_COLOR_WHITE);
    memset(system_state.video_char_buffer, 0xa5, sizeof(system_state.video_char_buffer));
    const window_t window = display_init_local(columns, attr);
    ck_assert_uint_eq(mock_spi_access_count(), 0);
    ck_assert_uint_eq(window.width, columns);
    ck_assert_uint_eq(window.height, DISPLAY_ROWS);
    ck_assert_ptr_eq(window.colors, system_state.video_char_buffer + VIDEO_COLOR_OFFSET);
    ck_assert_mem_eq(roms_get_char_rom(video_text_mode_lower), rom_chars_e800 + 0x400, 0x400);
    for (size_t i = 0; i < VIDEO_PLANE_CAPACITY; ++i)
        ck_assert_uint_eq(system_state.video_char_buffer[i], VROM_SPACE);
    for (size_t i = 0; i < VIDEO_COLOR_CAPACITY; ++i)
        ck_assert_uint_eq(system_state.video_char_buffer[VIDEO_COLOR_OFFSET + i], attr);
    dvi_display_geometry_t geometry;
    crtc_calculate_geometry(system_state.pet_crtc_registers, columns, 720, 240, 8, 2, &geometry);
    ck_assert_uint_eq(geometry.chars_per_row, columns);
    ck_assert_uint_eq(geometry.rows, DISPLAY_ROWS);
    ck_assert_uint_eq(geometry.vram_start, 0);
    ck_assert_uint_eq(geometry.invert_mask, 0);
    system_state = saved_state;
}
END_TEST

/** Terminal mirroring uses the active width for every row. */
START_TEST(test_terminal_width) {
    const system_state_t saved_state = system_state;
    system_state.pet_display_columns = _i ? pet_display_columns_80 : pet_display_columns_40;
    memset(system_state.video_char_buffer, VROM_SPACE, VIDEO_PLANE_CAPACITY);
    char* output = NULL;
    size_t size = 0;
    FILE* const saved_stdout = stdout;
    stdout = open_memstream(&output, &size);
    ck_assert_ptr_nonnull(stdout);
    __real_display_term_refresh();
    ck_assert_int_eq(fclose(stdout), 0);
    stdout = saved_stdout;
    ck_assert_uint_eq(size, strlen("\e[H") + DISPLAY_ROWS * (system_state.pet_display_columns + 2));
    for (unsigned row = 0; row < DISPLAY_ROWS; ++row) {
        const size_t end = strlen("\e[H") + row * (system_state.pet_display_columns + 2) +
                           system_state.pet_display_columns;
        ck_assert_mem_eq(output + end, "\r\n", 2);
    }
    free(output);
    system_state = saved_state;
}
END_TEST

/** Startup and remote exit disable local echo, with clearing confined to startup. */
START_TEST(test_terminal_cli_recovery) {
    const system_state_t saved_state = system_state;
    mock_reset();
    char* output = NULL;
    size_t size = 0;
    FILE* const saved_stdout = stdout;
    stdout = open_memstream(&output, &size);
    ck_assert_ptr_nonnull(stdout);

    // Exercise both startup after an interrupted remote session and a normal remote exit.
    system_state.terminal_session = terminal_session_remote;
    if (_i == 0) cli_init();
    else __real_display_term_end();
    ck_assert_int_eq(fclose(stdout), 0);
    stdout = saved_stdout;

    ck_assert_ptr_null(strstr(output, "\e[3J"));
    ck_assert_ptr_null(strstr(output, "\ec"));
    ck_assert_ptr_null(strstr(output, "\e[12l"));
    if (_i == 0) {
        const char* const recovery = "\e[?1049l\e[?25h\e[?6l\e[r\e[m\e[?7h\e(B\x0f" "\e[12h";
        const char* const clear_and_banner = "\e[2J\e[H\r\nEconoPET Firmware";
        ck_assert_uint_ge(size, strlen(recovery) + strlen(clear_and_banner));
        ck_assert_mem_eq(output, recovery, strlen(recovery));
        ck_assert_mem_eq(output + strlen(recovery), clear_and_banner, strlen(clear_and_banner));
        ck_assert_ptr_null(strstr(output + strlen(recovery) + strlen(clear_and_banner), "\e[2J"));
        ck_assert_int_eq(system_state.terminal_session, terminal_session_cli);
        ck_assert_ptr_nonnull(strstr(output + strlen(recovery), "econopet> "));
    } else {
        ck_assert_str_eq(output, "\e[?1049l\e[?25h\e[m\e[?7h\e(B\x0f" "\e[12h");
    }
    free(output);
    system_state = saved_state;
}
END_TEST

/** Repeated prompts and late-connection Enter disable local echo without cursor movement. */
START_TEST(test_terminal_prompt_normalization) {
    const system_state_t saved_state = system_state;
    mock_reset();
    char* output = NULL;
    size_t size = 0;
    FILE* const saved_stdout = stdout;
    stdout = open_memstream(&output, &size);
    ck_assert_ptr_nonnull(stdout);
    cli_init();
    const size_t startup_size = size;
    for (unsigned prompt = 0; prompt < 2; ++prompt) {
        if (_i == 0) console_prompt();
        else cli_process_char('\r');
    }
    ck_assert_int_eq(fclose(stdout), 0);
    stdout = saved_stdout;

    const char* const prompt = "\e[?25h\e[m\e[?7h\e(B\x0f" "\e[12heconopet> ";
    const size_t prefix_size = _i == 0 ? 0 : strlen("\r\n");
    const size_t prompt_size = prefix_size + strlen(prompt);
    ck_assert_uint_eq(size - startup_size, 2 * prompt_size);
    for (unsigned index = 0; index < 2; ++index) {
        const char* const actual = output + startup_size + index * prompt_size;
        if (prefix_size != 0) ck_assert_mem_eq(actual, "\r\n", prefix_size);
        ck_assert_mem_eq(actual + prefix_size, prompt, strlen(prompt));
    }
    free(output);
    system_state = saved_state;
}
END_TEST

/** Remote mode disables local echo, clears only the alternate screen, and hides the cursor. */
START_TEST(test_terminal_remote_begin) {
    char* output = NULL;
    size_t size = 0;
    FILE* const saved_stdout = stdout;
    stdout = open_memstream(&output, &size);
    ck_assert_ptr_nonnull(stdout);
    __real_display_term_begin();
    ck_assert_int_eq(fclose(stdout), 0);
    stdout = saved_stdout;
    ck_assert_str_eq(output, "\e[?1049h\e[12h\e[?25l\e[m\e[2J\e[H");
    free(output);
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
        ck_assert_uint_eq(system_state.video_char_buffer[offset], VROM_SPACE);
        ck_assert_uint_eq(mock_ram[VIDEO_RAM_ADDRESS + offset], VROM_SPACE);
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
    tcase_add_loop_test(synchronization, test_display_init_local, 0, 2);
    tcase_add_loop_test(synchronization, test_terminal_width, 0, 2);
    tcase_add_loop_test(synchronization, test_terminal_cli_recovery, 0, 2);
    tcase_add_loop_test(synchronization, test_terminal_prompt_normalization, 0, 2);
    tcase_add_test(synchronization, test_terminal_remote_begin);
    suite_add_tcase(suite, synchronization);
    return suite;
}

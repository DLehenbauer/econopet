// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "display_test.h"

#include <unistd.h>

#include "display/display.h"
#include "system_state.h"

static const unsigned int screen_rows = 25;
static FILE* output;
static int saved_stdout;
static system_state_t saved_state;

// Capture terminal output and preserve shared state for the non-forking runner.
static void setup(void) {
    saved_state = system_state;
    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    ck_assert_int_ge(saved_stdout, 0);
    output = tmpfile();
    ck_assert_ptr_nonnull(output);
    ck_assert_int_ge(dup2(fileno(output), STDOUT_FILENO), 0);
}

// Restore stdout before the runner reports results.
static void teardown(void) {
    fflush(stdout);
    ck_assert_int_ge(dup2(saved_stdout, STDOUT_FILENO), 0);
    close(saved_stdout);
    fclose(output);
    system_state = saved_state;
}

// Read captured bytes without depending on the terminal's display width.
static void read_output(char* buffer, size_t capacity) {
    fflush(stdout);
    rewind(output);
    const size_t length = fread(buffer, 1, capacity - 1, output);
    ck_assert_int_eq(fgetc(output), EOF);
    buffer[length] = '\0';
}

// Both PET widths must render every row once without wrapping or line feeds.
START_TEST(test_remote_geometry) {
    const pet_display_columns_t columns = _i == 0
        ? pet_display_columns_40 : pet_display_columns_80;
    system_state.pet_display_columns = columns;
    for (unsigned int row = 0; row < screen_rows; ++row) {
        memset(system_state.video_char_buffer + row * columns, 'A' + row, columns);
    }
    display_term_refresh();

    char buffer[4096];
    read_output(buffer, sizeof(buffer));
    const char* cursor = buffer;
    ck_assert_int_eq(strncmp(cursor, "\e[m", 3), 0);
    cursor += 3;
    for (unsigned int row = 0; row < screen_rows; ++row) {
        char position[32];
        const int length = snprintf(position, sizeof(position), "\e[%u;1H", row + 1);
        ck_assert_int_eq(strncmp(cursor, position, length), 0);
        cursor += length;
        for (unsigned int col = 0; col < (unsigned int)columns; ++col) {
            ck_assert_int_eq(*cursor++, 'A' + row);
        }
    }
    ck_assert_int_eq(*cursor, '\0');
}
END_TEST

// Remote mode suppresses autowrap and restores it when returning to the CLI.
START_TEST(test_remote_wrap_mode) {
    display_term_begin();
    display_term_end();

    char buffer[256];
    read_output(buffer, sizeof(buffer));
    const char* const alternate = strstr(buffer, "\e[?1049h");
    const char* const wrap_off = strstr(buffer, "\e[?7l");
    const char* const wrap_on = strstr(buffer, "\e[?7h");
    const char* const exit = strstr(buffer, "\e[?1049l");
    ck_assert_ptr_nonnull(alternate);
    ck_assert_ptr_nonnull(wrap_off);
    ck_assert_ptr_nonnull(wrap_on);
    ck_assert_ptr_nonnull(exit);
    ck_assert(alternate < wrap_off);
    ck_assert(wrap_off < wrap_on);
    ck_assert(wrap_on < exit);
}
END_TEST

// Window rendering shares row positioning and resets inherited reverse video.
START_TEST(test_window_render) {
    uint8_t characters[] = { 0xc1, 'B', 'C', 0xc4 };
    const window_t window = window_create(characters, 2, 2);
    display_window_show(&window);
    display_window_show(&window);

    char buffer[256];
    read_output(buffer, sizeof(buffer));
    const char* const frame = "\e[m\e[1;1H\e[7mA\e[mB\e[2;1HC\e[7mD";
    ck_assert_uint_eq(strlen(buffer), 2 * strlen(frame));
    ck_assert_int_eq(strncmp(buffer, frame, strlen(frame)), 0);
    ck_assert_str_eq(buffer + strlen(frame), frame);
}
END_TEST

// Register terminal regressions in the firmware test runner.
Suite* display_suite(void) {
    Suite* const suite = suite_create("display");
    TCase* const test_case = tcase_create("terminal");
    tcase_add_checked_fixture(test_case, setup, teardown);
    tcase_add_loop_test(test_case, test_remote_geometry, 0, 2);
    tcase_add_test(test_case, test_remote_wrap_mode);
    tcase_add_test(test_case, test_window_render);
    suite_add_tcase(suite, test_case);
    return suite;
}

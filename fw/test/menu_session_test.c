// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "menu_session_test.h"

#include "diag/log/log.h"
#include "driver.h"
#include "input/input.h"
#include "input/input_controls.h"
#include "input/input_routing.h"
#include "input/pet_keyboard.h"
#include "menu/menu_screen.h"
#include "menu/menu_session.h"
#include "menu/menu_ui.h"
#include "mock.h"
#include "sd/sd.h"
#include "system_state.h"
#include "tape.h"
#include "usb/keyscan.h"

extern void __real_display_task(void);
extern void __real_menu_enter(bool is_boot);

static bool tape_change_expected;
static unsigned tape_init_count, tape_deinit_count, reset_count;

/** Accept only the explicitly selected configuration's disabled-tape setup. */
void tape_init(const tape_config_t* config) {
    ck_assert(tape_change_expected);
    ck_assert_ptr_null(config);
    ++tape_init_count;
}

/** Record authorized destructive teardown, rejecting any Back/reset teardown. */
void tape_deinit(void) {
    ck_assert(tape_change_expected);
    ++tape_deinit_count;
}

/** Bulk SD/keymap operations are outside these lifecycle fixtures. */
void sd_read_file(const char* path, sd_read_callback_t callback, void* context, size_t max_bytes) {
    (void)path;
    (void)callback;
    (void)context;
    (void)max_bytes;
    ck_abort_msg("unexpected SD bulk read in menu lifecycle fixture");
}

/** Count actual reset assertions without replacing the production reset lifecycle. */
static void observe_reset(cpu_state_t state) {
    if (state & CPU_RESET) {
        ++reset_count;
    }
}

static unsigned exit_step;
static unsigned release_guard_services;
static menu_exit_t desired_exit;

/** Request root Back/reset, or activate Change Config and the already-selected entry. */
static tui_action_t request_exit(void) {
    if (!pet_keyboard_ready()) {
        ++release_guard_services;
        return tui_action_none;
    }
    if (desired_exit == MENU_EXIT_RESUME) {
        return tui_action_back;
    }
    if (desired_exit == MENU_EXIT_LOAD_CONFIG && exit_step++ < 2) {
        return tui_action_down;
    }
    return tui_action_activate;
}

/** Initialize a nondefault guest, including output latches hidden behind direction selection. */
static void prepare_guest(void) {
    mock_reset();
    mock_clock_set(0);
    set_cpu(CPU_HALT);
    pet_io_write(0xe811, 0x38);
    pet_io_write(0xe810, 0xaf);
    pet_io_write(0xe813, 0x30);
    pet_io_write(0xe812, 0xff);
    pet_io_write(0xe811, 0x3c);
    pet_io_write(0xe810, 0x39);
    pet_io_write(0xe813, 0x34);
    pet_io_write(0xe812, 0xa5);
    pet_io_write(0xe84c, 0xcc);
}

START_TEST(test_back_restores_only_borrowed_bytes_and_readable_pia_state) {
    prepare_guest();
    uint8_t original[PET_MAX_VIDEO_RAM_BYTES], font[1u << ECONOPET_BRAM_ADDR_WIDTH];
    for (size_t index = 0; index < sizeof(original); ++index) {
        original[index] = (uint8_t)(index * 17 + 3);
    }
    for (size_t index = 0; index < sizeof(font); ++index) {
        font[index] = (uint8_t)(index * 7 + 1);
    }
    spi_write(MENU_SCREEN_ADDRESS, original, sizeof(original));
    spi_write(ECONOPET_WB_BRAM_BASE_ADDR, font, sizeof(font));
    memcpy(system_state.video_char_buffer, original, sizeof(original));
    system_state.video_source = video_source_pet;
    system_state.pet_display_columns = pet_display_columns_80;
    system_state_set_video_ram_mask(&system_state, pet_video_ram_mask_4kb);
    set_cpu(_i & 1 ? CPU_HALT : CPU_READY);
    system_state.bp_halted = _i == 2;
    const cpu_state_t cpu = get_cpu();
    const uint8_t audio = _i & 1 ? 0 : ECONOPET_REG_AUDIO_ENABLE_MASK;
    spi_write_at(ECONOPET_WB_AUDIO_ADDR, audio);

    ck_assert(menu_session_begin());
    ck_assert(!menu_session_begin());
    ck_assert(cpu_suspended());
    ck_assert_uint_eq(get_cpu(), cpu);
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_AUDIO_ADDR), 0);
    menu_screen_clear();
    __real_display_task();
    for (size_t index = MENU_SCREEN_BYTES; index < sizeof(original); ++index) {
        ck_assert_uint_eq(spi_read_at(MENU_SCREEN_ADDRESS + index), original[index]);
    }

    menu_session_restore(true);
    ck_assert(!menu_session_active());
    ck_assert(!cpu_suspended());
    ck_assert_uint_eq(get_cpu(), cpu);
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_AUDIO_ADDR), audio);
    ck_assert_uint_eq(system_state.bp_halted, _i == 2);
    for (size_t index = 0; index < sizeof(original); ++index) {
        ck_assert_uint_eq(spi_read_at(MENU_SCREEN_ADDRESS + index), original[index]);
        ck_assert_uint_eq(system_state.video_char_buffer[index], original[index]);
    }
    for (size_t index = 0; index < sizeof(font); ++index) {
        ck_assert_uint_eq(spi_read_at(ECONOPET_WB_BRAM_BASE_ADDR + index), font[index]);
    }
    set_cpu(CPU_HALT);
    ck_assert_uint_eq(pet_io_read(0xe811), 0x3c);
    ck_assert_uint_eq(pet_io_read(0xe813), 0x34);
    ck_assert_uint_eq(pet_io_read(0xe810) & 0xaf, 0x39 & 0xaf);
    ck_assert_uint_eq(pet_io_read(0xe812), 0xa5);
    ck_assert_uint_eq(pet_io_read(0xe84c), 0xcc);
}
END_TEST

START_TEST(test_direct_scanner_publishes_complete_matrix_without_stale_snoop_overwrite) {
    prepare_guest();
    uint8_t matrix[KEY_COL_COUNT];
    memset(matrix, 0xff, sizeof(matrix));
    matrix[3] = 0xf7;
    mock_input_pet_snapshot(matrix);
    set_cpu(CPU_READY);
    ck_assert(menu_session_begin());
    for (unsigned step = 0; step < 29; ++step) {
        pet_keyboard_task();
        mock_clock_advance(200);
    }
    ck_assert(!pet_keyboard_ready());
    pet_keyboard_task();
    ck_assert(pet_keyboard_ready());
    ck_assert_mem_eq(pet_key_matrix, matrix, sizeof(matrix));
    memset(matrix, 0xff, sizeof(matrix));
    mock_input_pet_snapshot(matrix);
    sync_state();
    ck_assert_uint_eq(pet_key_matrix[3], 0xf7);
    ck_assert(!pet_keyboard_released());
    menu_session_restore(false);
    ck_assert(cpu_suspended());
    menu_session_abandon();
    ck_assert(!menu_session_active());
    ck_assert_uint_eq(get_cpu(), CPU_RESET);
}
END_TEST

START_TEST(test_release_requires_a_fresh_stable_round_even_when_noise_looks_empty) {
    prepare_guest();
    pet_keyboard_begin_boot();
    for (unsigned step = 0; step < 30; ++step) {
        pet_keyboard_task();
        mock_clock_advance(200);
    }
    ck_assert(pet_keyboard_released());
    pet_keyboard_wait_release_begin();
    ck_assert(!pet_keyboard_released());
    pet_keyboard_task();
    uint8_t matrix[KEY_COL_COUNT];
    memset(matrix, 0xff, sizeof(matrix));
    for (unsigned sample = 0; sample < 3; ++sample) {
        matrix[0] = sample & 1 ? 0xff : 0xfe;
        mock_input_pet_snapshot(matrix);
        mock_clock_advance(200);
        pet_keyboard_task();
    }
    memset(matrix, 0xff, sizeof(matrix));
    mock_input_pet_snapshot(matrix);
    for (unsigned step = 0; step < 27; ++step) {
        pet_keyboard_task();
        mock_clock_advance(200);
    }
    ck_assert(pet_keyboard_ready());
    ck_assert(!pet_keyboard_released());
    for (unsigned step = 0; step < 30; ++step) {
        pet_keyboard_task();
        mock_clock_advance(200);
    }
    ck_assert(pet_keyboard_released());
    pet_keyboard_end();
}
END_TEST

START_TEST(test_runtime_owner_distinguishes_back_reset_and_same_configuration_activation) {
    log_init();
    prepare_guest();
    test_unregister_file("/config.yaml");
    test_register_file("/config.yaml", "configs:\n"
                                       "  - name: Guest\n"
                                       "    setup:\n"
                                       "      - action: set\n"
                                       "        columns: 40\n"
                                       "        cpu: 6502\n");
    const menu_exit_t exits[] = {MENU_EXIT_RESUME, MENU_EXIT_RESET, MENU_EXIT_LOAD_CONFIG};
    desired_exit = exits[_i];
    exit_step = release_guard_services = reset_count = tape_init_count = tape_deinit_count = 0;
    tape_change_expected = desired_exit == MENU_EXIT_LOAD_CONFIG;
    mock_allow_configuration_model_writes(tape_change_expected);
    system_state.video_source = video_source_pet;
    system_state.pet_display_columns = pet_display_columns_80;
    set_cpu(CPU_READY);
    spi_write_at(ECONOPET_WB_AUDIO_ADDR, 0);
    mock_cpu_set_state_observer(observe_reset);
    mock_input_set_reader(request_exit);
    __real_menu_enter(false);
    mock_input_set_reader(NULL);
    mock_cpu_set_state_observer(NULL);
    ck_assert(!menu_session_active());
    ck_assert(!pet_keyboard_active());
    ck_assert(!cpu_suspended());
    ck_assert_uint_gt(release_guard_services, 0);
    mock_clock_advance(INPUT_LONG_PRESS_US + 1);
    input_task();
    ck_assert_int_eq(input_routing_pending_request().action, input_system_none);
    ck_assert_uint_eq(get_cpu(), CPU_READY);
    ck_assert_uint_eq(mock_pet_nmi_count(), 0);
    if (desired_exit == MENU_EXIT_RESUME) {
        ck_assert_uint_eq(reset_count, 0);
        ck_assert_uint_eq(system_state.pet_display_columns, pet_display_columns_80);
    } else {
        ck_assert_uint_gt(reset_count, 0);
    }
    ck_assert_uint_eq(tape_init_count, tape_change_expected ? 1 : 0);
    ck_assert_uint_eq(tape_deinit_count, tape_change_expected ? 1 : 0);
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_AUDIO_ADDR), tape_change_expected ? ECONOPET_REG_AUDIO_ENABLE_MASK : 0);

    // A genuinely new physical chord must still request the menu after handoff.
    uint8_t matrix[KEY_COL_COUNT];
    memset(matrix, UINT8_MAX, sizeof(matrix));
    mock_input_pet_snapshot(matrix);
    input_task();
    const bool business = system_state.pet_keyboard_model == pet_keyboard_model_business;
    const key_event_t left = business ? PET_KEY_LSHIFT_B : PET_KEY_LSHIFT_N;
    const key_event_t right = business ? PET_KEY_RSHIFT_B : PET_KEY_RSHIFT_N;
    matrix[PET_KEY_COL(left)] &= (uint8_t)~(1u << PET_KEY_ROW(left));
    matrix[PET_KEY_COL(right)] &= (uint8_t)~(1u << PET_KEY_ROW(right));
    mock_input_pet_snapshot(matrix);
    input_task();
    mock_clock_advance(INPUT_LONG_PRESS_US + 1);
    input_task();
    ck_assert_int_eq(input_routing_take_request().action, input_system_request_menu);
    memset(matrix, UINT8_MAX, sizeof(matrix));
    mock_input_pet_snapshot(matrix);
    input_task();
    test_unregister_file("/config.yaml");
    tape_change_expected = false;
    mock_allow_configuration_model_writes(false);
}
END_TEST

START_TEST(test_failed_hold_acquisition_releases_ownership_and_restores_saved_audio) {
    prepare_guest();
    set_cpu(CPU_READY);
    spi_write_at(ECONOPET_WB_AUDIO_ADDR, ECONOPET_REG_AUDIO_ENABLE_MASK);
    mock_cpu_fail_next_suspend();
    ck_assert(!menu_session_begin());
    ck_assert(!menu_session_active());
    ck_assert(!cpu_suspended());
    ck_assert(!pet_keyboard_active());
    ck_assert_uint_eq(get_cpu(), CPU_READY);
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_AUDIO_ADDR), ECONOPET_REG_AUDIO_ENABLE_MASK);
}
END_TEST

Suite* menu_session_suite(void) {
    Suite* const suite = suite_create("menu_session");
    TCase* const test = tcase_create("borrowing");
    tcase_add_loop_test(test, test_back_restores_only_borrowed_bytes_and_readable_pia_state, 0, 3);
    tcase_add_test(test, test_direct_scanner_publishes_complete_matrix_without_stale_snoop_overwrite);
    tcase_add_test(test, test_release_requires_a_fresh_stable_round_even_when_noise_looks_empty);
    tcase_add_loop_test(test, test_runtime_owner_distinguishes_back_reset_and_same_configuration_activation, 0, 3);
    tcase_add_test(test, test_failed_hold_acquisition_releases_ownership_and_restores_saved_audio);
    suite_add_tcase(suite, test);
    return suite;
}

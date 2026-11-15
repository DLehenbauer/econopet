// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "menu_session_test.h"

#include "diag/log/log.h"
#include "display/display.h"
#include "driver.h"
#include "diskimage_test.h"
#include "ieee/ieee_drive.h"
#include "input/input.h"
#include "input/input_controls.h"
#include "input/input_routing.h"
#include "input/pet_keyboard.h"
#include "menu/menu.h"
#include "menu/menu_config.h"
#include "menu/menu_ui.h"
#include "mock.h"
#include "roms/roms.h"
#include "sd/sd.h"
#include "system/state.h"
#include "system/system.h"
#include "tape.h"
#include "usb/keyscan.h"

extern void __real_display_task(void);
extern void __real_menu_enter(void);
static bool tape_change_expected;
static unsigned tape_init_count, tape_deinit_count, reset_count;

static void setup(void) {
    log_init();
    test_clear_files();
}

static void teardown(void) {
    test_clear_files();
}

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

/** Read fixture-backed keymaps through the production callback contract. */
void sd_read_file(const char* path, sd_read_callback_t callback, void* context, size_t max_bytes) {
    FILE* const file = sd_open(path, "r");
    ck_assert_ptr_nonnull(file);
    uint8_t buffer[256];
    size_t offset = 0;
    size_t length;
    while (offset < max_bytes &&
           (length = fread(buffer, 1, MIN(sizeof(buffer), max_bytes - offset), file)) != 0) {
        callback(offset, buffer, length, context);
        offset += length;
    }
    ck_assert_uint_eq(ferror(file), 0);
    ck_assert_int_eq(fclose(file), 0);
}

/** Count actual reset assertions without replacing the production reset lifecycle. */
static void observe_reset(cpu_state_t state) {
    if (state & CPU_RESET) {
        ++reset_count;
    }
}

static unsigned exit_step;
static unsigned release_guard_services;
static unsigned exit_requests;
static menu_exit_t desired_exit;
static unsigned boot_rom_completions;

/** Request root Back/reset, or activate Change Config and the already-selected entry. */
static tui_action_t request_exit(void) {
    ++exit_requests;
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
    ieee_drive_init();
    system_reset_guest();
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

/** Complete each diagnostic-ROM phase and publish the charset handoff. */
static void complete_boot_rom(void) {
    ++boot_rom_completions;
    spi_write_at(MENU_ROM_COMPLETION_ADDRESS, MENU_ROM_COMPLETION_VALUE);
    input_routing_request((input_system_request_t){
        .source = INPUT_SOURCE_BUTTON,
        .action = input_system_request_reset,
    });
}

/** Select the first prepared configuration after the boot menu has rendered. */
static tui_action_t select_boot_configuration(void) {
    return pet_keyboard_ready() ? tui_action_activate : tui_action_none;
}

/** One-time startup discovers the physical model and fallback USB keymap. */
START_TEST(test_menu_init_discovers_model_and_default_keymap) {
    prepare_guest();
    mock_set_pet_model(pet_keyboard_model_business, pet_video_type_crtc);
    uint8_t keymap[sizeof(system_state.usb_keymap_data)];
    memset(keymap, 0xff, sizeof(keymap));
    keymap[0] = 0x12;
    test_register_binary_file("/ukm/us.bin", keymap, sizeof(keymap), false);

    menu_init();

    ck_assert_int_eq(system_state.pet_keyboard_model, pet_keyboard_model_business);
    ck_assert_int_eq(system_state.pet_video_type, pet_video_type_crtc);
    ck_assert_uint_eq(system_state.usb_keymap_data[0][0][0].row, 2);
    ck_assert_uint_eq(system_state.usb_keymap_data[0][0][0].col, 1);
}
END_TEST

/** Boot owner bypasses diagnostics only for a resolved default configuration. */
START_TEST(test_boot_owner_prepares_rom_only_without_default) {
    prepare_guest();
    tape_change_expected = true;
    test_register_file("/config.yaml", _i == 0
        ? "default: selected\nconfigs:\n"
          "  - id: selected\n    name: Selected\n    setup:\n"
          "      - action: set\n        columns: 40\n        cpu: 6502\n"
        : "configs:\n"
          "  - id: selected\n    name: Selected\n    setup:\n"
          "      - action: set\n        columns: 40\n        cpu: 6502\n");
    boot_rom_completions = 0;
    mock_cpu_set_run_callback(complete_boot_rom);
    mock_input_set_reader(select_boot_configuration);
    mock_clock_restore();

    menu_boot();

    mock_input_set_reader(NULL);
    mock_cpu_set_run_callback(NULL);
    tape_change_expected = false;
    if (_i == 0) {
        ck_assert_uint_eq(boot_rom_completions, 0);
    } else {
        ck_assert_uint_gt(boot_rom_completions, 0);
    }
    ck_assert_uint_eq(mock_display_task_count(), _i == 0 ? 0 : 1);
    ck_assert_int_eq(system_state.video_source, video_source_pet);
    ck_assert_int_eq(get_cpu(), CPU_READY);
}
END_TEST

START_TEST(test_runtime_owner_distinguishes_back_reset_and_same_configuration_activation) {
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
    exit_requests = 0;
    tape_change_expected = desired_exit == MENU_EXIT_LOAD_CONFIG;
    system_state.video_source = video_source_pet;
    system_state.pet_display_columns = pet_display_columns_80;
    set_cpu(CPU_READY);
    spi_write_at(ECONOPET_WB_AUDIO_ADDR, 0);
    mock_cpu_set_state_observer(observe_reset);
    mock_input_set_reader(request_exit);
    uint8_t* const image = diskimage_test_make_d64();
    test_register_binary_file("/disks/live.d64", image, DISKIMAGE_D64_SIZE, false);
    free(image);
    ck_assert_int_eq(ieee_drive_try_mount(0, "live.d64"), DOS_MEDIA_OK);
    __real_menu_enter();
    mock_input_set_reader(NULL);
    mock_cpu_set_state_observer(NULL);
    ck_assert(!pet_keyboard_active());
    ck_assert_uint_gt(exit_requests, 0);
    ck_assert_uint_gt(release_guard_services, 0);
    mock_clock_advance(INPUT_LONG_PRESS_US + 1);
    input_task();
    ck_assert_int_eq(input_routing_pending_request().action, input_system_none);
    ck_assert_uint_eq(get_cpu(), CPU_READY);
    ck_assert_int_eq(system_state.video_source, video_source_pet);
    if (desired_exit == MENU_EXIT_RESUME) {
        ck_assert_uint_eq(reset_count, 0);
        ck_assert_uint_eq(system_state.pet_display_columns, pet_display_columns_80);
        ck_assert_str_eq(ieee_drive_filename(0), "live.d64");
    } else {
        ck_assert_uint_gt(reset_count, 0);
        ck_assert_str_eq(ieee_drive_filename(0),
            desired_exit == MENU_EXIT_LOAD_CONFIG ? "" : "live.d64");
    }
    ck_assert_uint_eq(tape_init_count, tape_change_expected ? 1 : 0);
    ck_assert_uint_eq(tape_deinit_count, tape_change_expected ? 1 : 0);
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_AUDIO_ADDR), tape_change_expected ? ECONOPET_REG_AUDIO_ENABLE_MASK : 0);

    // A genuinely new physical chord must still request the menu after handoff.
    uint8_t matrix[ECONOPET_KBD_COL_COUNT];
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
    ck_assert(ieee_drive_unmount_all());
    tape_change_expected = false;
}
END_TEST

Suite* menu_session_suite(void) {
    Suite* const suite = suite_create("menu_session");
    TCase* const test = tcase_create("borrowing");
    tcase_add_checked_fixture(test, setup, teardown);
    tcase_add_test(test, test_menu_init_discovers_model_and_default_keymap);
    tcase_add_loop_test(test, test_boot_owner_prepares_rom_only_without_default, 0, 2);
    tcase_add_loop_test(test, test_runtime_owner_distinguishes_back_reset_and_same_configuration_activation, 0, 3);
    suite_add_tcase(suite, test);
    return suite;
}

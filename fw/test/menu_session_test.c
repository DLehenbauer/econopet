// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "menu_session_test.h"

#include <signal.h>

#include "breakpoint.h"
#include "cpu.h"
#include "diag/diag.h"
#include "diag/log/log.h"
#include "diag/target.h"
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
#include "usb/keymap.h"
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

/** Request root Back/reset, or open Change Config and activate the selected entry. */
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

/** One-time startup discovers the physical model and restores the embedded USB keymap. */
START_TEST(test_menu_init_discovers_model_and_embedded_keymap) {
    prepare_guest();
    mock_set_pet_model(pet_keyboard_model_business, pet_video_type_crtc);
    memset(system_state.usb_keymap_data, 0, sizeof(system_state.usb_keymap_data));
    menu_init();

    ck_assert_int_eq(system_state.pet_keyboard_model, pet_keyboard_model_business);
    ck_assert_int_eq(system_state.pet_video_type, pet_video_type_crtc);
    ck_assert_uint_eq(system_state.usb_keymap_data[0][0][0x2a].row, 7);
    ck_assert_uint_eq(system_state.usb_keymap_data[0][0][0x2a].col, 4);
}
END_TEST

/** Boot owner bypasses diagnostics only for a resolved default configuration. */
START_TEST(test_boot_owner_prepares_rom_only_without_default) {
    prepare_guest();
    tape_change_expected = true;
    test_register_file("/config.yaml", _i == 0
        ? "format-version: 1\ndefault: selected\nconfigs:\n"
          "  - id: selected\n    name: Selected\n    setup:\n"
          "      - action: set\n        columns: 40\n        cpu: 6502\n"
        : "format-version: 1\nconfigs:\n"
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
    test_register_file("/config.yaml", "format-version: 1\nconfigs:\n"
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
    test_register_binary_file("/disk/live.d64", image, DISKIMAGE_D64_SIZE, false);
    free(image);
    ck_assert_int_eq(ieee_drive_try_mount(0, "live.d64", NULL), DOS_MEDIA_OK);
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
        ck_assert_str_eq(ieee_drive_filename(0), "live.d64");
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

/** Configuration activation restores the embedded keymap, while Resume and Reset preserve it. */
START_TEST(test_configuration_restores_embedded_keymap_between_sessions) {
    prepare_guest();
    system_state_t expected = {0};
    usb_keymap_reset(&expected);
    test_register_file("/config.yaml",
        "format-version: 1\ndefault: custom\nconfigs:\n"
        "  - id: custom\n    name: Custom\n    setup:\n"
        "      - action: set\n        cpu: 6502\n"
        "      - action: set\n        cpu: 6502\n        columns: 40\n");
    tape_change_expected = true;
    menu_init();
    menu_boot();
    ck_assert_mem_eq(system_state.usb_keymap_data, expected.usb_keymap_data,
                     sizeof(expected.usb_keymap_data));

    tape_change_expected = false;
    const menu_exit_t nondestructive_exits[] = {MENU_EXIT_RESUME, MENU_EXIT_RESET};
    mock_input_set_reader(request_exit);
    for (size_t i = 0; i < count_of(nondestructive_exits); ++i) {
        desired_exit = nondestructive_exits[i];
        __real_menu_enter();
        ck_assert_mem_eq(system_state.usb_keymap_data, expected.usb_keymap_data,
                         sizeof(expected.usb_keymap_data));
    }

    test_unregister_file("/config.yaml");
    test_register_file("/config.yaml", _i == 0
        ? "format-version: 1\nconfigs:\n  - name: Default\n    setup:\n"
          "      - action: set\n        cpu: 6502\n        columns: 40\n"
        : "format-version: 1\nconfigs:\n  - name: Default\n    setup: []\n");
    tape_change_expected = true;
    desired_exit = MENU_EXIT_LOAD_CONFIG;
    exit_step = 0;
    __real_menu_enter();
    mock_input_set_reader(NULL);
    tape_change_expected = false;
    ck_assert_mem_eq(system_state.usb_keymap_data, expected.usb_keymap_data,
                     sizeof(expected.usb_keymap_data));
}
END_TEST

/** Advance simulated time during cpu_init() so the absent-physical-CPU probe times out cleanly. */
static void advance_cpu_init_timeout(void) {
    mock_clock_advance(1000u);
}

/** Initialize the one-time CPU probe with no physical 6502 in an isolated test process. */
static void prepare_diag_cpu(void) {
    prepare_guest();
    bp_init();
    set_cpu(CPU_RESET);
    mock_cpu_set_run_callback(advance_cpu_init_timeout);
    cpu_init();
    mock_cpu_set_run_callback(NULL);
}

/** menu_apply_diag_config skips mount: actions, forces stock 6502 + tape off, and keeps diagnostic mode. */
START_TEST(test_diag_config_preserves_mode_and_ignores_mounts) {
    // Initialize breakpoint and CPU probing with no physical 6502 present.
    prepare_diag_cpu();

    // Pre-mount a diagnostic scratch disk and select a conflicting 6809 SuperPET state.
    uint8_t* const scratch = diskimage_test_make_d64();
    test_register_binary_file("/test/tmp/u8d0.d64", scratch, DISKIMAGE_D64_SIZE, true);
    free(scratch);
    ck_assert_int_eq(ieee_drive_mount_scratch(0, "/test/tmp/u8d0.d64"), DOS_MEDIA_OK);

    set_cpu_type_machine(CPU_SOFT_6809, true);
    reset_count = tape_init_count = tape_deinit_count = 0;
    tape_change_expected = true;
    mock_cpu_set_state_observer(observe_reset);

    // Apply a YAML configuration containing virtual tape, 6809 SuperPET options, and a user disk mount.
    test_register_file("/config.yaml",
        "format-version: 1\n"
        "configs:\n"
        "  - id: 8032\n"
        "    name: Custom 8032\n"
        "    setup:\n"
        "      - action: set\n"
        "        columns: 80\n"
        "        tape: \"2ef415f4c9cad1d4da\"\n"
        "        cpu: 6809\n"
        "        machine: superpet\n"
        "      - action: mount\n"
        "        device: 8\n"
        "        drive: 0\n"
        "        file: missing-user-disk.d64\n");
    system_takeover();
    ck_assert(menu_apply_diag_config("8032"));
    mock_cpu_set_state_observer(NULL);
    tape_change_expected = false;

    // Confirm diagnostic mode, halted soft 6502, disabled tape, and preserved scratch mount.
    ck_assert_int_eq(system_mode(), system_operation_diagnostic);
    ck_assert_uint_eq(reset_count, 0);
    ck_assert_uint_eq(tape_deinit_count, 1);
    ck_assert_uint_eq(tape_init_count, 1);
    ck_assert_int_eq(get_cpu(), CPU_HALT);
    ck_assert_int_eq(spi_read_at(ECONOPET_WB_CPU_SEL_ADDR), CPU_SOFT_6502);
    ck_assert_str_eq(ieee_drive_filename(0), "/test/tmp/u8d0.d64");
}
END_TEST

/** A required-ROM fatal leaves the CPU halted without resetting or allocating media. */
static void check_missing_rom_fatal(void) {
    ck_assert_int_eq(system_mode(), system_operation_diagnostic);
    ck_assert_int_eq(get_cpu(), CPU_HALT);
    ck_assert_uint_eq(reset_count, 0);
    ck_assert_uint_eq(tape_init_count, 0);
    ck_assert_str_eq(ieee_drive_filename(0), "");
}

/** Preserve required-ROM fatal behavior through the real configuration load path. */
START_TEST(test_diag_config_missing_rom_is_fatal) {
    prepare_diag_cpu();

    test_register_file("/rom/missing.bin", "placeholder");
    test_fail_file_opens("/rom/missing.bin", true);
    test_register_file("/config.yaml",
        "format-version: 1\n"
        "configs:\n"
        "  - id: custom\n"
        "    name: Custom\n"
        "    setup:\n"
        "      - action: load\n"
        "        files:\n"
        "          - file: /rom/missing.bin\n"
        "            address: 0xb000\n"
        "      - action: set\n"
        "        columns: 40\n");

    tape_change_expected = true;
    system_takeover();
    reset_count = tape_init_count = 0;
    mock_cpu_set_state_observer(observe_reset);
    test_expect_fatal_message("file not found '/rom/missing.bin'");
    test_expect_fatal_callback(check_missing_rom_fatal);
    menu_apply_diag_config("custom");
    ck_abort_msg("missing required ROM did not halt");
}
END_TEST

/** A valid PRG decodes its address little-endian and publishes the exclusive BASIC end. */
START_TEST(test_load_prg_valid_and_ram_boundary) {
    prepare_guest();
    const uint16_t start = _i == 0 ? 0x1234u : VIDEO_RAM_ADDRESS - 3u;
    const uint8_t prg[] = {(uint8_t)start, (uint8_t)(start >> 8), 0x55u, 0xaau, 0x42u};
    test_register_binary_file("/test/valid.prg", prg, sizeof(prg), false);
    ck_assert(load_prg("/test/valid.prg"));
    ck_assert_mem_eq(mock_ram + start, prg + 2, sizeof(prg) - 2u);
    const uint16_t end = start + sizeof(prg) - 2u;
    for (uint16_t addr = 0x2au; addr <= 0x2fu; addr += 2u) {
        ck_assert_uint_eq(mock_ram[addr], (uint8_t)end);
        ck_assert_uint_eq(mock_ram[addr + 1u], end >> 8);
    }
    ck_assert_uint_eq(mock_ram[0xc9u], (uint8_t)end);
    ck_assert_uint_eq(mock_ram[0xcau], end >> 8);
    ck_assert_uint_eq(test_open_file_count(), 0u);
}
END_TEST

/** Rejected PRGs do not publish BASIC end pointers or overwrite video RAM. */
START_TEST(test_load_prg_rejects_invalid_files) {
    prepare_guest();
    const uint8_t prg[] = {0xffu, 0x7fu, 0x55u, 0xaau};
    uint8_t invalid[sizeof(prg)];
    memcpy(invalid, prg, sizeof(prg));
    const size_t sizes[] = {0u, 1u, 2u, sizeof(prg), sizeof(prg)};
    if (_i == 4) {
        invalid[0] = 0u;
        invalid[1] = 0x80u;
    }
    memset(mock_ram + 0x2au, 0x5au, 6u);
    memset(mock_ram + 0xc9u, 0x5au, 2u);
    mock_ram[VIDEO_RAM_ADDRESS] = 0x5au;
    test_register_binary_file("/test/invalid.prg", invalid, sizes[_i], false);
    ck_assert(!load_prg("/test/invalid.prg"));
    for (uint16_t addr = 0x2au; addr <= 0x2fu; ++addr) {
        ck_assert_uint_eq(mock_ram[addr], 0x5au);
    }
    ck_assert_uint_eq(mock_ram[0xc9u], 0x5au);
    ck_assert_uint_eq(mock_ram[0xcau], 0x5au);
    ck_assert_uint_eq(mock_ram[VIDEO_RAM_ADDRESS], 0x5au);
    ck_assert_uint_eq(test_open_file_count(), 0u);
}
END_TEST

/** A caller-reserved SRAM ceiling protects the mailbox without rejecting the last available byte. */
START_TEST(test_load_prg_reserved_ceiling) {
    prepare_guest();
    const uint16_t start = TARGET_DIAG_MAILBOX_ADDR - 1u;
    const uint8_t prg[] = {(uint8_t)start, (uint8_t)(start >> 8), 0x55u, 0xaau};
    memset(mock_ram + TARGET_DIAG_MAILBOX_ADDR, 0x5au, TARGET_DIAG_MAILBOX_SIZE);
    memset(mock_ram + 0x2au, 0x5au, 6u);
    test_register_binary_file("/test/bounded.prg", prg, _i == 0 ? 3u : 4u, false);
    ck_assert_int_eq(load_prg_bounded("/test/bounded.prg", TARGET_DIAG_MAILBOX_ADDR), _i == 0);
    for (uint16_t addr = TARGET_DIAG_MAILBOX_ADDR;
         addr < TARGET_DIAG_MAILBOX_ADDR + TARGET_DIAG_MAILBOX_SIZE; ++addr) {
        ck_assert_uint_eq(mock_ram[addr], 0x5au);
    }
    if (_i != 0) {
        for (uint16_t addr = 0x2au; addr <= 0x2fu; ++addr) {
            ck_assert_uint_eq(mock_ram[addr], 0x5au);
        }
    }
    ck_assert_uint_eq(test_open_file_count(), 0u);
}
END_TEST

static uint16_t target_ready_address;
static bool target_boot_timeout;
static bool target_small_ram;
static unsigned target_ready_releases;

/** Model ROM startup reaching the expected READY entry, or taking longer than its budget. */
static void reach_target_ready(void) {
    if (bp_count() == 0) return;
    ck_assert_uint_eq(bp_get(0)->addr, target_ready_address);
    if (target_boot_timeout) {
        mock_clock_advance(2000000u);
        return;
    }
    mock_ram[0x34u] = 0u;
    mock_ram[0x35u] = target_small_ram ? 0x40u : 0x80u;
    mock_ram[0x30u] = 0u;
    mock_ram[0x31u] = 0x80u;
    mock_ram[0x32u] = 0x55u;
    mock_ram[0x33u] = 0xaau;
    mock_breakpoint_set_hit_addr(target_ready_address);
    system_state.bp_halted = true;
}

/** Count CPU releases after staging, requiring the first-cycle mailbox to be armed first. */
static void observe_target_release(cpu_state_t state) {
    if (state != CPU_READY) return;
    ++target_ready_releases;
    ck_assert_uint_eq(mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE)], TARGET_DIAG_STATE_RUNNING);
    ck_assert_uint_eq(mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_HOST_CONTROL)], TARGET_DIAG_HOST_ATTACHED);
}

/** Staging selects the loaded ROM, reserves its mailbox, queues RUN, and distinguishes cycle retriggers. */
START_TEST(test_target_prepare_and_cycles) {
    prepare_diag_cpu();
    tape_change_expected = true;
    target_ready_address = _i == 1 ? 0xc389u : 0xb3ffu;
    target_boot_timeout = _i == 3;
    target_small_ram = _i == 5;
    // These entry bytes come from media ROMs 901465-01 and 901465-23 (also 901465-19).
    const char* const signature = _i == 1 ? "460da997a0c2201c" : "a912a0b3201dbb20";
    char config[512];
    snprintf(config, sizeof(config),
        "format-version: 1\nconfigs:\n  - id: custom\n    name: Custom\n    setup:\n"
        "      - action: set\n        columns: 40\n        cpu: 6502\n"
        "      - action: patch\n        address: %u\n        hex: \"%s\"\n",
        target_ready_address, _i == 2 ? "0000000000000000" : signature);
    test_register_file("/config.yaml", config);
    const uint16_t load_address = _i == 6 ? TARGET_DIAG_MAILBOX_ADDR : 0x0401u;
    const uint8_t prg[] = {(uint8_t)load_address, (uint8_t)(load_address >> 8), 0x00u, 0x00u, 0x42u};
    if (_i != 4) test_register_binary_file("/test/target.prg", prg, sizeof(prg), false);
    mock_cpu_set_run_callback(reach_target_ready);
    system_takeover();
    reset_count = 0;
    mock_cpu_set_state_observer(observe_reset);
    ck_assert(menu_apply_diag_config("custom"));
    ck_assert_uint_eq(reset_count, 0);

    // Mount only after profile loading, then exercise target reset with live scratch media.
    uint8_t* const scratch = diskimage_test_make_d64();
    test_register_binary_file("/test/tmp/u8d0.d64", scratch, DISKIMAGE_D64_SIZE, true);
    free(scratch);
    ck_assert_int_eq(ieee_drive_mount_scratch(0, "/test/tmp/u8d0.d64"), DOS_MEDIA_OK);
    char error[DIAG_MESSAGE_MAX] = {0};
    const bool prepared = diag_target_prepare_pet_prg("/test/target.prg", error, sizeof(error));
    mock_cpu_set_state_observer(NULL);
    mock_cpu_set_run_callback(NULL);
    tape_change_expected = false;
    ck_assert_uint_gt(reset_count, 0);
    ck_assert_str_eq(ieee_drive_filename(0), "/test/tmp/u8d0.d64");
    ck_assert_int_eq(get_cpu(), CPU_HALT);
    ck_assert_uint_eq(bp_count(), 0u);
    ck_assert(!system_state.bp_halted);
    ck_assert_int_eq(system_mode(), system_operation_diagnostic);
    if (_i >= 2) {
        ck_assert(!prepared);
        ck_assert_ptr_nonnull(strstr(error, _i == 2 ? "unsupported" : _i == 3 ? "READY" :
                                           _i == 5 ? "requires 32 KiB" : "failed to load"));
        ck_assert_uint_ne(mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_HOST_CONTROL)], TARGET_DIAG_HOST_ATTACHED);
        return;
    }
    ck_assert(prepared);
    ck_assert_mem_eq(mock_ram + 0x0401u, prg + 2, sizeof(prg) - 2u);
    ck_assert_uint_eq(mock_ram[0x009eu], 4u);
    ck_assert_mem_eq(mock_ram + 0x026fu, "RUN\r", 4u);
    ck_assert_uint_eq(mock_ram[0x34u] | ((uint16_t)mock_ram[0x35u] << 8), TARGET_DIAG_MAILBOX_ADDR);
    ck_assert_uint_eq(mock_ram[0x30u] | ((uint16_t)mock_ram[0x31u] << 8), TARGET_DIAG_MAILBOX_ADDR);
    ck_assert_uint_eq(mock_ram[0x32u], 0x55u);
    ck_assert_uint_eq(mock_ram[0x33u], 0xaau);
    ck_assert_uint_eq(mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_HOST_CONTROL)], TARGET_DIAG_HOST_ATTACHED);

    // Replay SENDP's TBUFF,X stores for CONCAT with two maximum-length filenames.
    // KERNAL_PET_4.0_REC/dos.write builds 57 bytes at $0353, through the old mailbox.
    static const char concat[] = "C0:ABCDEFGHIJKLMNOP=0:ABCDEFGHIJKLMNOP,0:QRSTUVWXYZABCDEF";
    memcpy(mock_ram + 0x0353u, concat, sizeof(concat) - 1u);
    diag_target_snapshot_t snap;
    ck_assert(diag_target_read_mailbox(&snap));
    ck_assert_uint_eq(snap.host_control, TARGET_DIAG_HOST_ATTACHED);
    ck_assert_uint_eq(snap.state, TARGET_DIAG_STATE_IDLE);
    ck_assert_uint_eq(snap.completed, 0u);

    target_ready_releases = 0u;
    mock_cpu_set_state_observer(observe_target_release);
    diag_target_begin_cycle(1u);
    ck_assert_uint_eq(target_ready_releases, 1u);
    ck_assert_int_eq(get_cpu(), CPU_READY);
    // A completed target is spinning while the host resets counters and publishes RUNNING last.
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_STATE)] = TARGET_DIAG_STATE_PASSED;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_LO)] = 0xffu;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_COMPLETED_HI)] = 1u;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_PHASE)] = 2u;
    mock_ram[TARGET_DIAG_ADDR(TARGET_DIAG_OFF_SLOT)] = 3u;
    diag_target_begin_cycle(2u);
    mock_cpu_set_state_observer(NULL);
    ck_assert_uint_eq(target_ready_releases, 1u);
    ck_assert(diag_target_read_mailbox(&snap));
    ck_assert_uint_eq(snap.completed, 0u);
    ck_assert_uint_eq(snap.phase, TARGET_DIAG_PHASE_NONE);
    ck_assert_uint_eq(snap.slot, TARGET_DIAG_SLOT_NONE);
    ck_assert_uint_eq(snap.state, TARGET_DIAG_STATE_RUNNING);
    ck_assert_uint_eq(snap.host_control, TARGET_DIAG_HOST_ATTACHED);
}
END_TEST

Suite* menu_session_suite(void) {
    Suite* const suite = suite_create("menu_session");
    TCase* const test = tcase_create("borrowing");
    tcase_add_checked_fixture(test, setup, teardown);
    tcase_add_test(test, test_menu_init_discovers_model_and_embedded_keymap);
    tcase_add_loop_test(test, test_boot_owner_prepares_rom_only_without_default, 0, 2);
    tcase_add_loop_test(test, test_runtime_owner_distinguishes_back_reset_and_same_configuration_activation, 0, 3);
    tcase_add_loop_test(test, test_configuration_restores_embedded_keymap_between_sessions, 0, 2);
    tcase_add_loop_test(test, test_load_prg_valid_and_ram_boundary, 0, 2);
    tcase_add_loop_test(test, test_load_prg_rejects_invalid_files, 0, 5);
    tcase_add_loop_test(test, test_load_prg_reserved_ceiling, 0, 2);
    suite_add_tcase(suite, test);
    return suite;
}

/** Build the isolated fork-runner suite for menu session tests that call one-time cpu_init(). */
Suite* menu_session_fatal_suite(void) {
    Suite* const suite = suite_create("menu_session_fatal");
    TCase* const test = tcase_create("isolated");
    tcase_add_checked_fixture(test, setup, teardown);
    tcase_add_test(test, test_diag_config_preserves_mode_and_ignores_mounts);
    tcase_add_test_raise_signal(test, test_diag_config_missing_rom_is_fatal, SIGABRT);
    tcase_add_loop_test(test, test_target_prepare_and_cycles, 0, 7);
    suite_add_tcase(suite, test);
    return suite;
}

// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "system_session_test.h"

#include <setjmp.h>

#include "diag/log/log.h"
#include "display/display.h"
#include "diskimage_test.h"
#include "driver.h"
#include "ieee/diskimage.h"
#include "ieee/ieee_drive.h"
#include "ieee/ieee_protocol.h"
#include "input/input_uart.h"
#include "input/pet_keyboard.h"
#include "mock.h"
#include "menu/menu_config.h"
#include "system/state.h"
#include "system/system.h"
#include "ui/cli.h"

extern void __real_display_task(void);

static jmp_buf terminal_exit;
static unsigned terminal_action;

static void setup(void) {
    log_init();
    mock_reset();
    ieee_drive_init();
}

static void teardown(void) {
    ck_assert(ieee_drive_unmount_all());
    test_clear_files();
}

static void observe_terminal_action(void) {
    ++terminal_action;
    ck_assert_int_eq(system_mode(), system_operation_diagnostic);
    ck_assert(!pet_keyboard_active());
    longjmp(terminal_exit, 1);
}

/** Feed a complete CLI command synchronously. */
static void send_cli(const char* line) {
    while (*line) input_uart_process((uint8_t)*line++);
}

static void expect_terminal_action(const char* command) {
    terminal_action = 0;
    if (setjmp(terminal_exit) == 0) {
        send_cli(command);
        ck_abort_msg("terminal command returned");
    }
    ck_assert_uint_eq(terminal_action, 1);
}

/** Initialize a guest with nondefault borrowed PIA/VIA state. */
static void prepare_guest(void) {
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

/** Drain bounded production drive work. */
static void service_drive(void) {
    for (unsigned pass = 0; pass < 8; ++pass) ieee_drive_task();
}

/** Open a LOAD channel through the public IEEE facade. */
static void open_load_channel(const char* name) {
    mock_ieee_enqueue_rx(true, IEEE_CMD_LISTEN(8));
    mock_ieee_enqueue_rx(true, IEEE_CMD_OPEN(IEEE_LOAD_CHANNEL));
    for (size_t index = 0; index < strlen(name); ++index)
        mock_ieee_enqueue_rx_eoi(false, (uint8_t)name[index], name[index + 1] == '\0');
    mock_ieee_enqueue_rx(true, IEEE_CMD_UNLISTEN);
    mock_ieee_enqueue_atn_end();
    service_drive();
}

/** Borrow and restore bounded physical and renderer-owned state. */
START_TEST(test_suspend_restores_borrowed_resources_and_control) {
    prepare_guest();
    uint8_t screen[PET_MAX_VIDEO_RAM_BYTES], font[1u << ECONOPET_BRAM_ADDR_WIDTH];
    for (size_t i = 0; i < sizeof(screen); ++i) screen[i] = (uint8_t)(i * 17 + 3);
    for (size_t i = 0; i < sizeof(font); ++i) font[i] = (uint8_t)(i * 7 + 1);
    spi_write(VIDEO_RAM_ADDRESS, screen, sizeof(screen));
    spi_write(ECONOPET_WB_BRAM_BASE_ADDR, font, sizeof(font));
    memcpy(system_state.video_char_buffer, screen, sizeof(screen));
    system_state.video_source = video_source_pet;
    system_state.pet_display_columns = pet_display_columns_80;
    system_state_set_video_ram_mask(&system_state, pet_video_ram_mask_4kb);
    set_cpu(_i & 1 ? CPU_HALT : CPU_READY);
    system_state.bp_halted = _i == 2;
    const cpu_state_t cpu = get_cpu();
    const uint8_t audio = _i & 1 ? 0 : ECONOPET_REG_AUDIO_ENABLE_MASK;
    spi_write_at(ECONOPET_WB_AUDIO_ADDR, audio);

    ck_assert(system_suspend());
    ck_assert_int_eq(system_mode(), system_operation_console);
    ck_assert(!system_suspend());
    ck_assert_uint_eq(get_cpu(), cpu & ~CPU_READY);
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_AUDIO_ADDR), 0);
    __real_display_task();
    system_restore(true);

    ck_assert_int_eq(system_mode(), system_operation_guest);
    ck_assert_uint_eq(get_cpu(), cpu);
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_AUDIO_ADDR), audio);
    ck_assert_uint_eq(system_state.bp_halted, _i == 2);
    for (size_t i = 0; i < sizeof(screen); ++i) {
        ck_assert_uint_eq(spi_read_at(VIDEO_RAM_ADDRESS + i), screen[i]);
        ck_assert_uint_eq(system_state.video_char_buffer[i], screen[i]);
    }
    for (size_t i = 0; i < sizeof(font); ++i)
        ck_assert_uint_eq(spi_read_at(ECONOPET_WB_BRAM_BASE_ADDR + i), font[i]);
    set_cpu(CPU_HALT);
    ck_assert_uint_eq(pet_io_read(0xe811), 0x3c);
    ck_assert_uint_eq(pet_io_read(0xe813), 0x34);
    ck_assert_uint_eq(pet_io_read(0xe810) & 0xaf, 0x39 & 0xaf);
    ck_assert_uint_eq(pet_io_read(0xe812), 0xa5);
    ck_assert_uint_eq(pet_io_read(0xe84c), 0xcc);
}
END_TEST

/** CLI reset and diagnostic commands irreversibly take diagnostic ownership. */
START_TEST(test_terminal_actions_take_over_without_restoring_console) {
    const char* const commands[] = {"test\r", "reset\r"};
    for (unsigned phase = 0; phase < count_of(commands); ++phase) {
        prepare_guest();
        cli_init();
        set_cpu(CPU_READY);
        ck_assert(system_suspend());
        uint8_t matrix[ECONOPET_KBD_COL_COUNT];
        memset(matrix, 0xff, sizeof(matrix));
        mock_input_pet_snapshot(matrix);
        if (phase != 0) {
            for (unsigned step = 0; step < 3 * ECONOPET_KBD_COL_COUNT; ++step) {
                pet_keyboard_task();
                mock_clock_advance(200);
            }
            ck_assert(pet_keyboard_ready());
            pet_keyboard_wait_release_begin();
        }
        if (phase == 0) mock_ram_test_set_handler(observe_terminal_action);
        else mock_system_reset_set_handler(observe_terminal_action);
        expect_terminal_action(commands[phase]);
        mock_ram_test_set_handler(NULL);
        mock_system_reset_set_handler(NULL);
        ck_assert_int_eq(system_mode(), system_operation_diagnostic);
    }
}
END_TEST

/** Restoring without resume leaves CPU control halted for destructive teardown. */
START_TEST(test_restore_without_resume_retains_halt) {
    prepare_guest();
    set_cpu(CPU_READY);
    ck_assert(system_suspend());
    system_restore(false);
    ck_assert_int_eq(system_mode(), system_operation_console);
    ck_assert(!(get_cpu() & CPU_READY));
    ck_assert(!system_suspend());
}
END_TEST

/** Restoring and loading configuration preserves staged GET counters until teardown. */
START_TEST(test_configuration_preserves_staged_get_until_unmount) {
    prepare_guest();
    uint8_t* const image = diskimage_test_make_d64();
    test_register_binary_file("/disks/old.d64", image, DISKIMAGE_D64_SIZE, false);
    free(image);
    ck_assert_int_eq(ieee_drive_try_mount(0, "old.d64"), DOS_MEDIA_OK);
    const bool status_source = _i != 0;
    if (!status_source) open_load_channel("BASIC,PRG,R");
    mock_ieee_enqueue_rx(true, IEEE_CMD_TALK(8));
    mock_ieee_enqueue_rx(true,
        IEEE_CMD_SECONDARY(status_source ? IEEE_COMMAND_CHANNEL : IEEE_LOAD_CHANNEL));
    mock_ieee_enqueue_atn_end();
    service_drive();

    if (status_source) mock_ieee_consume_status(1);
    else mock_ieee_consume_data(1);
    service_drive();
    const uint32_t counter =
        status_source ? ECONOPET_WB_IEEE_TXS_ADDR : ECONOPET_WB_IEEE_TX_ADDR;
    ck_assert_uint_eq(spi_read_at(counter), 1);
    ck_assert_uint_gt(
        status_source ? mock_ieee_status_count() : mock_ieee_data_count(), 0);

    set_cpu(CPU_READY);
    ck_assert(system_suspend());
    system_restore(false);
    test_register_file("/config.yaml", "configs:\n  - name: replacement\n");
    const setup_sink_t sink = {0};
    load_config(&sink, 0);

    ck_assert_int_eq(get_cpu(), CPU_HALT);
    ck_assert_str_eq(ieee_drive_filename(0), "");
    ck_assert_uint_eq(spi_read_at(counter), 0);
    ck_assert_uint_eq(mock_ieee_data_count(), 0);
    ck_assert_uint_eq(mock_ieee_status_count(), 0);
}
END_TEST

Suite* system_session_suite(void) {
    Suite* const suite = suite_create("system_session");
    TCase* const test = tcase_create("ownership");
    tcase_add_checked_fixture(test, setup, teardown);
    tcase_add_loop_test(test, test_suspend_restores_borrowed_resources_and_control, 0, 3);
    tcase_add_test(test, test_terminal_actions_take_over_without_restoring_console);
    tcase_add_test(test, test_restore_without_resume_retains_halt);
    tcase_add_loop_test(test, test_configuration_preserves_staged_get_until_unmount, 0, 2);
    suite_add_tcase(suite, test);
    return suite;
}

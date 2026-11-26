// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "system_session_test.h"

#include <setjmp.h>

#include "diag/log/log.h"
#include "display/char_encoding.h"
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

/** Inspect real diagnostic initialization before escaping its terminal loop. */
static void observe_diagnostic(diag_session_t* session) {
    ck_assert_ptr_eq(session->plan, diag_find_plan("memory"));
    ck_assert_int_eq(get_cpu(), CPU_HALT);
    ck_assert_int_eq(system_state.pet_display_columns, pet_display_columns_80);
    observe_terminal_action();
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

/** Guest reset clears both 80-column character halves and initializes DVI attributes. */
START_TEST(test_guest_reset_clears_full_video_planes) {
    system_state.pet_display_columns = pet_display_columns_80;
    memset(system_state.video_char_buffer, 0xa5, sizeof(system_state.video_char_buffer));

    system_reset_guest();

    for (size_t i = 0; i < VIDEO_PLANE_CAPACITY; ++i)
        ck_assert_uint_eq(system_state.video_char_buffer[i], VROM_SPACE);
    for (size_t i = 0; i < VIDEO_COLOR_CAPACITY; ++i)
        ck_assert_uint_eq(system_state.video_char_buffer[VIDEO_COLOR_OFFSET + i],
            VIDEO_DEFAULT_ATTRIBUTE);
}
END_TEST

/** CLI reset and diagnostic commands irreversibly take diagnostic ownership. */
START_TEST(test_terminal_actions_take_over_without_restoring_console) {
    const char* const commands[] = {"test memory\r", "reset\r"};
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
        if (phase == 0) mock_diag_loop_set_handler(observe_diagnostic);
        else mock_system_reset_set_handler(observe_terminal_action);
        expect_terminal_action(commands[phase]);
        mock_diag_loop_set_handler(NULL);
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

/** Configuration keeps mounted media and its staged GET until explicit unmount. */
START_TEST(test_configuration_preserves_mounted_media_until_unmount) {
    prepare_guest();
    uint8_t* const image = diskimage_test_make_d64();
    test_register_binary_file("/disk/old.d64", image, DISKIMAGE_D64_SIZE, false);
    free(image);
    ck_assert_int_eq(ieee_drive_try_mount(0, "old.d64", NULL), DOS_MEDIA_OK);
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
    const size_t data_count = mock_ieee_data_count();
    const size_t status_count = mock_ieee_status_count();
    const size_t queued = status_source ? status_count : data_count;
    uint8_t bytes[MOCK_IEEE_TX_CAPACITY];
    bool eoi[MOCK_IEEE_TX_CAPACITY];
    ck_assert_uint_le(queued, count_of(bytes));
    for (size_t i = 0; i < queued; ++i) {
        bytes[i] = status_source ? mock_ieee_status_byte(i) : mock_ieee_data_byte(i);
        eoi[i] = status_source ? mock_ieee_status_eoi(i) : mock_ieee_data_eoi(i);
    }

    set_cpu(CPU_READY);
    ck_assert(system_suspend());
    system_restore(false);
    ck_assert_uint_eq(spi_read_at(counter), 1);
    ck_assert_uint_eq(mock_ieee_data_count(), data_count);
    ck_assert_uint_eq(mock_ieee_status_count(), status_count);
    for (size_t i = 0; i < queued; ++i) {
        ck_assert_uint_eq(status_source ? mock_ieee_status_byte(i) : mock_ieee_data_byte(i), bytes[i]);
        ck_assert_int_eq(status_source ? mock_ieee_status_eoi(i) : mock_ieee_data_eoi(i), eoi[i]);
    }
    test_register_file("/config.yaml", "format-version: 1\nconfigs:\n  - name: replacement\n");
    const setup_sink_t sink = {0};
    load_config(&sink, 0);

    ck_assert_int_eq(get_cpu(), CPU_HALT);
    ck_assert_str_eq(ieee_drive_filename(0), "old.d64");
    ck_assert_uint_eq(spi_read_at(counter), 1);
    ck_assert_uint_eq(mock_ieee_data_count(), data_count);
    ck_assert_uint_eq(mock_ieee_status_count(), status_count);

    ck_assert(ieee_drive_unmount_all());
    ck_assert_str_eq(ieee_drive_filename(0), "");
    ck_assert_uint_eq(spi_read_at(counter), 0);
    ck_assert_uint_eq(mock_ieee_data_count(), 0);
    ck_assert_uint_eq(mock_ieee_status_count(), 0);
}
END_TEST

/** Park VIA/PIA2 IEEE pins when virtual drives are mounted and restore snooped guest state on unmount. */
START_TEST(test_ieee_io_isolation_across_reset_and_menu_mounts) {
    uint8_t* const image = diskimage_test_make_d64();
    test_register_binary_file("/disk/iso.d64", image, DISKIMAGE_D64_SIZE, false);
    free(image);

    // Reset with mounted media parks VIA DDRB at 0x00 and PIA2 directions as inputs.
    ck_assert_int_eq(ieee_drive_try_mount(0, "iso.d64", NULL), DOS_MEDIA_OK);
    system_reset_guest();
    set_cpu(CPU_HALT);
    ck_assert_uint_eq(pet_io_read(0xe842), 0x00u);
    ck_assert_uint_eq(pet_io_read(0xe821), 0x04u);
    ck_assert_uint_eq(pet_io_read(0xe823), 0x04u);
    pet_io_write(0xe821, 0x00u);
    ck_assert_uint_eq(pet_io_read(0xe820), 0x00u);
    pet_io_write(0xe821, 0x04u);
    pet_io_write(0xe823, 0x00u);
    ck_assert_uint_eq(pet_io_read(0xe822), 0x00u);
    pet_io_write(0xe823, 0x04u);

    // Unmounting before the guest writes PIA2/VIA restores hardware 0x00 reset latches (not 0xFF).
    set_cpu(CPU_READY);
    ck_assert(system_suspend());
    ck_assert(ieee_drive_unmount_all());
    system_restore(true);
    set_cpu(CPU_HALT);
    ck_assert_uint_eq(pet_io_read(0xe842), 0x00u);
    pet_io_write(0xe842, 0xffu);
    ck_assert_uint_eq(pet_io_read(0xe840), 0x00u);
    pet_io_write(0xe823, 0x04u);
    pet_io_write(0xe823, 0x00u);
    pet_io_write(0xe822, 0xffu);
    pet_io_write(0xe823, 0x04u);
    ck_assert_uint_eq(pet_io_read(0xe822), 0x00u);

    // Reset again with mounted media before simulating KERNAL CINT + guest IEEE/user-port writes.
    ck_assert_int_eq(ieee_drive_try_mount(0, "iso.d64", NULL), DOS_MEDIA_OK);
    system_reset_guest();
    set_cpu(CPU_HALT);

    // Simulate KERNAL CINT + guest IEEE/user-port writes snooped by ieee.sv while isolated.
    mock_cpu_io_write(0xe821, 0x00u);
    mock_cpu_io_write(0xe820, 0x5au);
    mock_cpu_io_write(0xe821, 0x3cu);
    mock_cpu_io_write(0xe820, 0xc3u);
    mock_cpu_io_write(0xe823, 0x00u);
    mock_cpu_io_write(0xe822, 0xffu);
    mock_cpu_io_write(0xe823, 0x34u);
    mock_cpu_io_write(0xe822, 0xa5u);
    mock_cpu_io_write(0xe840, 0x14u);
    mock_cpu_io_write(0xe842, 0x9eu);

    // Lazy system_task() synchronization forwards non-IEEE VIA DDRB bits (0x98) before any menu entry.
    set_cpu(CPU_READY);
    system_task();
    set_cpu(CPU_HALT);
    ck_assert_uint_eq(pet_io_read(0xe842), 0x98u);

    // Unmounting all virtual drives in the menu restores exact snooped guest PIA2/VIA state.
    set_cpu(CPU_READY);
    ck_assert(system_suspend());
    ck_assert(ieee_drive_unmount_all());
    system_restore(true);
    set_cpu(CPU_HALT);
    ck_assert_uint_eq(pet_io_read(0xe840), (uint8_t)((0xffu & ~0x9eu) | 0x14u));
    ck_assert_uint_eq(pet_io_read(0xe842), 0x9eu);
    ck_assert_uint_eq(pet_io_read(0xe821), 0x3cu);
    ck_assert_uint_eq(pet_io_read(0xe820), (uint8_t)((0xffu & ~0x5au) | (0xc3u & 0x5au)));
    pet_io_write(0xe821, 0x00u);
    ck_assert_uint_eq(pet_io_read(0xe820), 0x5au);
    pet_io_write(0xe821, 0x3cu);
    ck_assert_uint_eq(pet_io_read(0xe823), 0x34u);
    ck_assert_uint_eq(pet_io_read(0xe822), 0xa5u);
    pet_io_write(0xe823, 0x00u);
    ck_assert_uint_eq(pet_io_read(0xe822), 0xffu);
    pet_io_write(0xe823, 0x34u);

    // Hot-mounting a virtual drive in the menu parks VIA PB2:1 and PIA2 while preserving
    // guest non-IEEE VIA DDRB bits (e.g. PB7), PIA1, and active-low VIA cassette outputs.
    prepare_guest();
    mock_cpu_io_write(0xe840, 0x86u);
    mock_cpu_io_write(0xe842, 0x9eu);
    set_cpu(CPU_READY);
    ck_assert(system_suspend());
    ck_assert_int_eq(ieee_drive_try_mount(0, "iso.d64", NULL), DOS_MEDIA_OK);
    system_restore(true);
    set_cpu(CPU_HALT);
    ck_assert_uint_eq(pet_io_read(0xe840) & 0x18u, 0x00u);
    ck_assert_uint_eq(pet_io_read(0xe842), 0x98u);
    ck_assert_uint_eq(pet_io_read(0xe821), 0x04u);
    ck_assert_uint_eq(pet_io_read(0xe823), 0x04u);
    ck_assert_uint_eq(pet_io_read(0xe811), 0x3cu);
    ck_assert_uint_eq(pet_io_read(0xe813), 0x34u);
    ck_assert_uint_eq(pet_io_read(0xe84c), 0xccu);

    // Waterloo IEEEInit uses DDRB/ORB = $06. Neither cold reset + system_task()
    // nor menu hot-mount may turn its input-configured cassette pins into low-driving outputs.
    system_reset_guest();
    mock_cpu_io_write(0xe842, 0x06u);
    mock_cpu_io_write(0xe840, 0x06u);
    system_task();
    set_cpu(CPU_HALT);
    ck_assert_uint_eq(pet_io_read(0xe842), 0x00u);
    ck_assert_uint_eq(pet_io_read(0xe840) & 0x18u, 0x18u);

    set_cpu(CPU_READY);
    ck_assert(system_suspend());
    ck_assert(ieee_drive_unmount_all());
    system_restore(true);
    mock_cpu_io_write(0xe840, 0x06u);
    mock_cpu_io_write(0xe842, 0x06u);
    set_cpu(CPU_READY);
    ck_assert(system_suspend());
    ck_assert_int_eq(ieee_drive_try_mount(0, "iso.d64", NULL), DOS_MEDIA_OK);
    system_restore(true);
    set_cpu(CPU_HALT);
    ck_assert_uint_eq(pet_io_read(0xe842), 0x00u);
    ck_assert_uint_eq(pet_io_read(0xe840) & 0x18u, 0x18u);
}
END_TEST

Suite* system_session_suite(void) {
    Suite* const suite = suite_create("system_session");
    TCase* const test = tcase_create("ownership");
    tcase_add_checked_fixture(test, setup, teardown);
    tcase_add_loop_test(test, test_suspend_restores_borrowed_resources_and_control, 0, 3);
    tcase_add_test(test, test_guest_reset_clears_full_video_planes);
    tcase_add_test(test, test_terminal_actions_take_over_without_restoring_console);
    tcase_add_test(test, test_restore_without_resume_retains_halt);
    tcase_add_loop_test(test, test_configuration_preserves_mounted_media_until_unmount, 0, 2);
    tcase_add_test(test, test_ieee_io_isolation_across_reset_and_menu_mounts);
    suite_add_tcase(suite, test);
    return suite;
}

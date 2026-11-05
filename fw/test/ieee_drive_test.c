// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "ieee_drive_test.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "diag/log/log.h"
#include "d64_fixture.h"
#include "diskimage_test.h"
#include "driver.h"
#include "fatal.h"
#include "ieee/diskimage.h"
#include "ieee/ieee_drive.h"
#include "ieee/ieee_protocol.h"
#include "menu/menu_config.h"
#include "mock.h"
#include "sd/sd.h"
#include "system_state.h"

#define TEST_UNIT_COUNT 4u
#define TEST_SLOTS_PER_UNIT 2u
#define TEST_FORMAT_COUNT 3u
#define TEST_SERVICE_PASSES 8u
#define TEST_COMMAND_LOG_LIMIT 8u

typedef struct {
    unsigned int copies;
    unsigned int options;
    unsigned int mounts;
} config_actions_t;

// Counts actual machine-copy actions in the selected setup.
static void copy_action(void* context, uint32_t source, uint32_t destination, uint32_t length) {
    (void)source;
    (void)destination;
    (void)length;
    ++((config_actions_t*)context)->copies;
}

// Counts option changes independently of parsing a candidate configuration.
static void options_action(void* context, options_t* options) {
    (void)options;
    ++((config_actions_t*)context)->options;
}

// Observes sequential parser callbacks and uses the public drive's type validation.
static void config_mount_action(void* context, uint32_t device, uint32_t drive, const char* filename) {
    ++((config_actions_t*)context)->mounts;
    vet(ieee_drive_mount((device - 8) * 2 + drive, filename), "configured disk could not mount");
}

// Starts each facade case with isolated register and filesystem observations.
static void setup(void) {
    mock_reset();
    log_init();
    ieee_drive_init();
}

// Settles pending work before retiring the borrowed mock file storage.
static void teardown(void) {
    for (unsigned int pass = 0; pass < TEST_SERVICE_PASSES; ++pass)
        ieee_drive_task();
    ck_assert(ieee_drive_unmount_all());
    test_clear_files();
}

// Exercises the public mount API with a copied, ordinary PRG fixture.
static void mount_image(const unsigned int slot, const unsigned int format, const char* name) {
    static const size_t sizes[] = {DISKIMAGE_D64_SIZE, DISKIMAGE_D80_SIZE, DISKIMAGE_D82_SIZE};
    ck_assert_uint_lt(format, count_of(sizes));
    uint8_t* image = format == 0 ? diskimage_test_make_d64() : diskimage_test_make_d80();
    if (format == 2) {
        uint8_t* const expanded = realloc(image, sizes[format]);
        ck_assert_ptr_nonnull(expanded);
        image = expanded;
        memset(image + DISKIMAGE_D80_SIZE, 0, DISKIMAGE_D80_SIZE);
    }
    char path[SD_PATH_MAX];
    sd_make_path(path, SD_DIR_DISKS, name);
    test_register_binary_file(path, image, sizes[format], false);
    free(image);
    ck_assert(ieee_drive_mount(slot, name));
}

// Supplies ordered ATN commands followed by their release checkpoint.
static void commands(const uint8_t first, const uint8_t second) {
    mock_ieee_enqueue_rx(true, first);
    mock_ieee_enqueue_rx(true, second);
    mock_ieee_enqueue_atn_end();
}

// Advances bounded cooperative jobs as well as ordered mock RX work.
static void service(void) {
    for (unsigned int pass = 0; pass < TEST_SERVICE_PASSES; ++pass)
        ieee_drive_task();
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_STATUS_ADDR) & ECONOPET_IEEE_ST_RX_AVAIL_MASK, 0);
}

// Requires exact status bytes and CR/EOI before modeling actual GET consumption.
static void status(const unsigned int device, const char* expected) {
    commands(IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_COMMAND_CHANNEL));
    service();
    const size_t length = strlen(expected);
    ck_assert_uint_eq(mock_ieee_status_count(), length);
    for (size_t index = 0; index < length; ++index) {
        ck_assert_uint_eq(mock_ieee_status_byte(index), (uint8_t)expected[index]);
        ck_assert_int_eq(mock_ieee_status_eoi(index), index + 1 == length);
    }
    mock_ieee_clear_status();
    commands(IEEE_CMD_UNTALK, IEEE_CMD_UNLISTEN);
    service();
}

// Requires a retained diagnostic without printing logs.
static void expect_status_log(const log_level_t expected_level, const char* expected) {
    log_iterator_t iterator;
    log_iter_init(&iterator, LOG_LEVEL_DEBUG);
    log_level_t level;
    const log_entry_t* entry;
    while ((entry = log_iter_next(&iterator, &level)) != NULL)
        if (level == expected_level && strstr(entry->message, expected) != NULL)
            return;
    ck_abort_msg("missing IEEE diagnostic: %s", expected);
}

// Records mounts, reset readback and service without changing fabric presence.
START_TEST(test_presence_diagnostics_observe_mount_reset_and_service) {
    mount_image(0, 0, "diagnostic.d64");
    expect_status_log(LOG_LEVEL_INFO, "IEEE mount u0 d0 rw0 diagnostic.d64");
    ieee_drive_reset();
    expect_status_log(LOG_LEVEL_INFO, "IEEE reset mask01 ctrl03 st08");

    service();
    expect_status_log(LOG_LEVEL_INFO, "IEEE service mask01 st08");
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR) & ECONOPET_IEEE_CTRL_RD_ENABLE_MASK,
                      ECONOPET_IEEE_CTRL_RD_ENABLE_MASK);
}
END_TEST

// Keeps only the first command exchange and rearms its budget on PET reset.
START_TEST(test_command_diagnostics_are_bounded_and_rearmed) {
    mount_image(0, 0, "diagnostic.d64");
    for (unsigned int pass = 0; pass < TEST_COMMAND_LOG_LIMIT; ++pass) {
        commands(IEEE_CMD_UNTALK, IEEE_CMD_UNLISTEN);
        service();
    }
    unsigned int count = 0;
    log_iterator_t iterator;
    log_iter_init(&iterator, LOG_LEVEL_INFO);
    log_level_t level;
    const log_entry_t* entry;
    while ((entry = log_iter_next(&iterator, &level)) != NULL) {
        if (strstr(entry->message, "IEEE cmd ") != NULL) {
            ++count;
        }
    }
    ck_assert_uint_eq(count, TEST_COMMAND_LOG_LIMIT);

    ieee_drive_reset();
    commands(IEEE_CMD_TALK(8), IEEE_CMD_SECONDARY(IEEE_COMMAND_CHANNEL));
    service();
    expect_status_log(LOG_LEVEL_INFO, "IEEE cmd 48 mask01 addr4 talk4");
    expect_status_log(LOG_LEVEL_INFO, "IEEE cmd 6f mask01 addr0 talk0");
    expect_status_log(LOG_LEVEL_INFO, "IEEE u0 status n20 e1 t0d abs");
}
END_TEST

// Distinguishes publication, partial GET progress and CR consumption in the log.
START_TEST(test_status_diagnostics_observe_partial_and_complete_gets) {
    mount_image(0, 0, "diagnostic.d64");
    commands(IEEE_CMD_TALK(8), IEEE_CMD_SECONDARY(IEEE_COMMAND_CHANNEL));
    service();
    expect_status_log(LOG_LEVEL_INFO, "IEEE u0 status n20 e1 t0d abs");
    expect_status_log(LOG_LEVEL_DEBUG, "IEEE u0 put p0 n20 e1 t0d [73,CBM DOS V2,00,00]");

    // Model the board's early stop without pretending the remaining CR was read.
    mock_ieee_consume_status(8);
    service();
    expect_status_log(LOG_LEVEL_DEBUG, "IEEE u0 get p0 +8 q12 c8");
    ck_assert_uint_eq(mock_ieee_status_count(), 12);
    status(8, "OS V2,00,00\r");
    expect_status_log(LOG_LEVEL_INFO, "IEEE u0 status complete n20");
    status(8, "00, OK,00,00\r");
    expect_status_log(LOG_LEVEL_INFO, "IEEE u0 status n13 e1 t0d abs");
    expect_status_log(LOG_LEVEL_INFO, "IEEE u0 status complete n13");

    // Preserve the early CLOSE cursor even after the native status is reset.
    commands(IEEE_CMD_TALK(8), IEEE_CMD_SECONDARY(IEEE_COMMAND_CHANNEL));
    service();
    mock_ieee_consume_status(8);
    commands(IEEE_CMD_LISTEN(8), IEEE_CMD_CLOSE(IEEE_COMMAND_CHANNEL));
    service();
    expect_status_log(LOG_LEVEL_INFO, "IEEE u0 close status p8/13");
}
END_TEST

// Schedules an ordinary OPEN through the facade, never a direct DOS call.
static void open_channel(const unsigned int device, const unsigned int channel, const char* name) {
    commands(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(channel));
    for (size_t index = 0; index < strlen(name); ++index)
        mock_ieee_enqueue_rx_eoi(false, (uint8_t)name[index], name[index + 1] == '\0');
    mock_ieee_enqueue_rx(true, IEEE_CMD_UNLISTEN);
    mock_ieee_enqueue_atn_end();
    service();
}

// Preserve the existing LOAD-specific facade cases.
static void open_file(const unsigned int device, const char* name) { open_channel(device, IEEE_LOAD_CHANNEL, name); }

// Persist ordinary bytes through decoded LISTEN/RX, never direct DOS access.
static void write_payload(const unsigned int count) {
    commands(IEEE_CMD_LISTEN(8), IEEE_CMD_SECONDARY(2));
    for (unsigned int index = 0; index < count; ++index)
        mock_ieee_enqueue_rx_eoi(false, (uint8_t)index, index + 1u == count);
    mock_ieee_enqueue_rx(true, IEEE_CMD_UNLISTEN);
    mock_ieee_enqueue_atn_end();
    service();
}

// Install a real writable filesystem rather than a zero-filled geometry fixture.
static void mount_writable(void) {
    uint8_t* const image = malloc(DISKIMAGE_D64_SIZE);
    ck_assert_ptr_nonnull(image);
    d64_fixture_layout_t layout;
    ck_assert_ptr_null(d64_fixture_empty(&layout, image, DISKIMAGE_D64_SIZE));
    test_register_binary_file("/disks/writable.d64", image, DISKIMAGE_D64_SIZE, true);
    free(image);
    ck_assert(ieee_drive_mount(0, "writable.d64"));
}

// Failed CLOSE/mount/unmount/reset retain the FILE and write cursor until retry.
START_TEST(test_failed_write_survives_lifecycle_and_remounts_persisted_bytes) {
    mount_writable();
    mount_image(2, 1, "peer.d80");
    open_channel(8, 2, "0:FILE,S,W");
    write_payload(22);
    test_fail_file_writes("/disks/writable.d64", true);
    commands(IEEE_CMD_LISTEN(8), IEEE_CMD_CLOSE(2));
    service();
    status(8, "25, WRITE ERROR,17,00\r");
    ck_assert(!ieee_drive_mount(0, "writable.d64"));
    ck_assert(!ieee_drive_unmount_all());
    ieee_drive_reset();
    status(8, "25, WRITE ERROR,17,00\r");
    status(9, "73,CBM DOS V2.7,00,00,0\r");
    test_fail_file_writes("/disks/writable.d64", false);
    commands(IEEE_CMD_LISTEN(8), IEEE_CMD_CLOSE(2));
    service();
    status(8, "00, OK,00,00\r");
    ck_assert(ieee_drive_unmount_all());
    ck_assert(ieee_drive_mount(0, "writable.d64"));
    open_channel(8, 2, "0:FILE,S,R");
    commands(IEEE_CMD_TALK(8), IEEE_CMD_SECONDARY(2));
    service();
    ck_assert_uint_eq(mock_ieee_data_count(), 22);
    for (unsigned int index = 0; index < 22; ++index) {
        ck_assert_uint_eq(mock_ieee_data_byte(index), index);
        ck_assert_int_eq(mock_ieee_data_eoi(index), index == 21);
    }
    mock_ieee_clear_data();
    commands(IEEE_CMD_UNTALK, IEEE_CMD_UNLISTEN);
    service();
}
END_TEST

// The sole engine exposes ROM-derived startup/status on every model and slot.
START_TEST(test_sole_engine_reads_all_models_units_and_slots) {
    const unsigned int format = _i % TEST_FORMAT_COUNT;
    const unsigned int slot = _i / TEST_FORMAT_COUNT;
    const unsigned int device = 8 + slot / TEST_SLOTS_PER_UNIT;
    mount_image(slot, format, "drive.img");
    status(device, format == 0 ? "73,CBM DOS V2,00,00\r" : "73,CBM DOS V2.7,00,00,0\r");
    status(device, format == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r");
    open_file(device, slot % TEST_SLOTS_PER_UNIT == 0 ? "0:BASIC,PRG,R" : "1:BASIC,PRG,R");
    commands(IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_LOAD_CHANNEL));
    service();

    // Consume each bounded staging window instead of treating publication as GET.
    const size_t length = format == 0 ? 264 : 4;
    size_t consumed = 0;
    while (consumed < length) {
        const size_t count = mock_ieee_data_count();
        ck_assert_uint_gt(count, 0);
        ck_assert_uint_le(count, ECONOPET_IEEE_TX_BURST_CHUNK);
        ck_assert_uint_le(consumed + count, length);
        for (size_t index = 0; index < count; ++index) {
            const size_t position = consumed + index;
            const uint8_t expected = format != 0      ? 0x80 + position
                                     : position < 254 ? position + 2
                                                      : 0xe2 + position - 254;
            ck_assert_uint_eq(mock_ieee_data_byte(index), expected);
            ck_assert_int_eq(mock_ieee_data_eoi(index), position + 1 == length);
        }
        consumed += count;
        mock_ieee_clear_data();
        service();
    }
    ck_assert_uint_eq(mock_ieee_data_count(), 0);
}
END_TEST

// Rejection retains the old media rather than consulting another implementation.
START_TEST(test_invalid_pair_and_image_preserve_mounted_unit) {
    mount_image(0, 0, "old.d64");
    mount_image(2, 1, "next.d80");
    ck_assert(!ieee_drive_mount(1, "next.d80"));
    const uint8_t invalid[DISKIMAGE_SECTOR_SIZE] = {0};
    test_register_binary_file("/disks/invalid.img", invalid, sizeof(invalid), false);
    ck_assert(!ieee_drive_mount(0, "invalid.img"));
    status(8, "73,CBM DOS V2,00,00\r");
    open_file(8, "BASIC,PRG,R");
    status(8, "00, OK,00,00\r");
}
END_TEST

// OPEN/OPREAD checks explicit REL type and applies SA 0's LOAD-mode default.
START_TEST(test_native_rel_type_checks_and_mode_defaults) {
    mount_image(0, 0, "read-only.d64");
    open_file(8, "BASIC,L");
    status(8, "64, FILE TYPE MISMATCH,00,00\r");
    open_file(8, "B?SIC");
    status(8, "00, OK,00,00\r");
    open_file(8, "BASIC,PRG,W");
    // Native SA 0 forces LOAD mode even when the name contains a W suffix.
    status(8, "00, OK,00,00\r");
    open_file(8, "BASIC,PRG,R");
    status(8, "00, OK,00,00\r");
}
END_TEST

// Arms the expected mount diagnostic only after the preceding setup actions ran.
static void expect_mount_failure_after_settings(void* context, uint32_t source, uint32_t destination, uint32_t length) {
    config_actions_t* const actions = context;
    copy_action(context, source, destination, length);
    ck_assert_int_eq(get_cpu(), CPU_HALT);
    ck_assert_uint_eq(actions->options, 1);
    ck_assert_uint_eq(actions->copies, 1);
    test_expect_fatal_message("configured disk could not mount");
}

// Supplies one manual selection. A menu retry would violate the fatal policy.
static int select_invalid_config(void) { return '\r'; }

// Direct, automatic and manual loading fail fatally after applying machine settings.
START_TEST(test_configuration_mount_failure_is_fatal_after_settings) {
    mount_image(0, 0, "old.d64");
    mount_image(2, 1, "next.d80");
    config_actions_t actions = {0};
    system_state_t state = {0};
    const setup_sink_t sink = {
        .context = &actions,
        .on_copy = expect_mount_failure_after_settings,
        .on_set_options = options_action,
        .on_mount = config_mount_action,
        .system_state = &state,
    };
    char yaml[768];
    const int length = snprintf(yaml, sizeof(yaml),
                                "default: invalid\n"
                                "configs:\n"
                                "  - id: invalid\n"
                                "    name: Invalid\n"
                                "    setup:\n"
                                "      - action: set\n"
                                "        columns: 80\n"
                                "      - action: copy\n"
                                "        source: 4096\n"
                                "        destination: 8192\n"
                                "        length: 256\n"
                                "      - action: mount\n"
                                "        device: 8\n"
                                "        drive: 0\n"
                                "        file: old.d64\n"
                                "      - action: mount\n"
                                "        device: 8\n"
                                "        drive: 1\n"
                                "        file: %s\n",
                                _i < 3 ? "next.d80" : "missing.d64");
    ck_assert_int_gt(length, 0);
    ck_assert_uint_lt((size_t)length, sizeof(yaml));
    test_register_file("/config.yaml", yaml);
    // Any earlier fatal error must fail the diagnostic check rather than pass on SIGABRT.
    test_expect_fatal_message("configuration setup actions did not finish");
    set_cpu(CPU_READY);
    if (_i % 3 == 0) {
        load_config(&sink, 0);
    } else {
        uint8_t buffer[40];
        const window_t window = window_create(buffer, 40, 1);
        mock_input_set_reader(select_invalid_config);
        menu_config_show(&window, &sink, _i % 3 == 1);
    }
    ck_abort_msg("invalid configuration returned instead of reporting a fatal error");
}
END_TEST

// Unmounting first allows either new-drive order to replace an old complete pair.
START_TEST(test_configuration_unmounts_then_mounts_selected_disks) {
    mount_image(0, 0, "old.d64");
    mount_image(1, 0, "old-partner.d64");
    mount_image(2, 1, "next.d80");
    config_actions_t actions = {0};
    system_state_t state = {0};
    const setup_sink_t sink = {
        .context = &actions,
        .on_copy = copy_action,
        .on_set_options = options_action,
        .on_mount = config_mount_action,
        .system_state = &state,
    };
    char yaml[768];
    const int length = snprintf(yaml, sizeof(yaml),
                                "configs:\n"
                                "  - name: replace\n"
                                "    setup:\n"
                                "      - action: copy\n"
                                "        source: 4096\n"
                                "        destination: 8192\n"
                                "        length: 256\n"
                                "      - action: mount\n"
                                "        device: 8\n"
                                "        drive: %d\n"
                                "        file: next.d80\n"
                                "      - action: mount\n"
                                "        device: 8\n"
                                "        drive: %d\n"
                                "        file: next.d80\n"
                                "      - action: set\n"
                                "        columns: 80\n",
                                _i, 1 - _i);
    ck_assert_int_gt(length, 0);
    ck_assert_uint_lt((size_t)length, sizeof(yaml));
    test_register_file("/config.yaml", yaml);
    set_cpu(CPU_READY);
    load_config(&sink, 0);
    ck_assert_int_eq(get_cpu(), CPU_HALT);
    ck_assert_uint_eq(actions.copies, 1);
    ck_assert_uint_eq(actions.options, 1);
    ck_assert_uint_eq(actions.mounts, 2);
    status(8, "73,CBM DOS V2.7,00,00,0\r");
    open_file(8, "1:BASIC,PRG,R");
    status(8, "00, OK,00,00,1\r");
    commands(IEEE_CMD_TALK(9), IEEE_CMD_SECONDARY(IEEE_COMMAND_CHANNEL));
    service();
    ck_assert_uint_eq(mock_ieee_status_count(), 0);
    commands(IEEE_CMD_UNTALK, IEEE_CMD_UNLISTEN);
    service();
}
END_TEST

// Every unit's mounted media enables emulation, and removing the last disables it.
START_TEST(test_unit_presence_controls_enabled_readback) {
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR) & ECONOPET_IEEE_CTRL_RD_ENABLE_MASK, 0);
    mount_image((unsigned int)_i * TEST_SLOTS_PER_UNIT, 0, "presence.d64");
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR) & ECONOPET_IEEE_CTRL_RD_ENABLE_MASK,
                      ECONOPET_IEEE_CTRL_RD_ENABLE_MASK);
    ck_assert(ieee_drive_unmount_all());
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR) & ECONOPET_IEEE_CTRL_RD_ENABLE_MASK, 0);
}
END_TEST

// The last valid full SD path mounts without truncation.
START_TEST(test_mount_path_boundary) {
    char name[SD_PATH_MAX];
    const size_t length = SD_PATH_MAX - 1 - strlen(sd_dir_prefix(SD_DIR_DISKS));
    memset(name, 'a', length);
    name[length] = '\0';
    mount_image(0, 0, name);
}
END_TEST

// The public mount boundary rejects overlong paths before opening any file.
START_TEST(test_mount_rejects_overlong_path) {
    char name[SD_PATH_MAX];
    const size_t length = SD_PATH_MAX - strlen(sd_dir_prefix(SD_DIR_DISKS));
    memset(name, 'a', length);
    name[length] = '\0';
    test_expect_fatal_message("SD path exceeds 255 characters (got 256)");
    ieee_drive_mount(0, name);
}
END_TEST

Suite* ieee_drive_suite(void) {
    Suite* suite = suite_create("ieee_drive");
    TCase* test_case = tcase_create("replacement");
    tcase_add_checked_fixture(test_case, setup, teardown);
    tcase_add_loop_test(test_case, test_sole_engine_reads_all_models_units_and_slots, 0,
                        TEST_UNIT_COUNT * TEST_SLOTS_PER_UNIT * TEST_FORMAT_COUNT);
    tcase_add_test(test_case, test_status_diagnostics_observe_partial_and_complete_gets);
    tcase_add_test(test_case, test_presence_diagnostics_observe_mount_reset_and_service);
    tcase_add_test(test_case, test_command_diagnostics_are_bounded_and_rearmed);
    tcase_add_test(test_case, test_invalid_pair_and_image_preserve_mounted_unit);
    tcase_add_test(test_case, test_native_rel_type_checks_and_mode_defaults);
    tcase_add_test(test_case, test_failed_write_survives_lifecycle_and_remounts_persisted_bytes);
    tcase_add_loop_test(test_case, test_configuration_unmounts_then_mounts_selected_disks, 0, 2);
    tcase_add_test(test_case, test_mount_path_boundary);
    tcase_add_loop_test(test_case, test_unit_presence_controls_enabled_readback, 0, TEST_UNIT_COUNT);
    suite_add_tcase(suite, test_case);
    return suite;
}

Suite* ieee_drive_fatal_suite(void) {
    Suite* suite = suite_create("ieee_drive-fatal");
    TCase* test_case = tcase_create("paths");
    tcase_add_checked_fixture(test_case, setup, teardown);
    tcase_add_test_raise_signal(test_case, test_mount_rejects_overlong_path, SIGABRT);
    tcase_add_loop_test_raise_signal(test_case, test_configuration_mount_failure_is_fatal_after_settings, SIGABRT, 0,
                                     6);
    suite_add_tcase(suite, test_case);
    return suite;
}

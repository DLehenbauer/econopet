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
#include "mock.h"
#include "sd/sd.h"
#include "system/state.h"

#define TEST_UNIT_COUNT 4u
#define TEST_SLOTS_PER_UNIT 2u
#define TEST_FORMAT_COUNT 3u
#define TEST_SERVICE_PASSES 8u
#define TEST_COMMAND_LOG_LIMIT 8u
#define TEST_FORMAT_SERVICE_PASSES (DISKIMAGE_D64_SIZE / DISKIMAGE_SECTOR_SIZE + TEST_SERVICE_PASSES)

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
    ck_assert_int_eq(ieee_drive_try_mount(slot, name, NULL), DOS_MEDIA_OK);
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
static void write_payload(const unsigned int device, const unsigned int count) {
    commands(IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(2));
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
    ck_assert_int_eq(ieee_drive_try_mount(0, "writable.d64", NULL), DOS_MEDIA_OK);
}

// Failed CLOSE/mount/unmount/reset retain the FILE and write cursor until retry.
START_TEST(test_failed_write_survives_lifecycle_and_remounts_persisted_bytes) {
    mount_writable();
    mount_image(1, 0, "partner.d64");
    mount_image(2, 1, "peer.d80");
    open_channel(8, 2, "0:FILE,S,W");
    write_payload(8, 22);
    test_fail_file_writes("/disks/writable.d64", true);
    commands(IEEE_CMD_LISTEN(8), IEEE_CMD_CLOSE(2));
    service();
    status(8, "25, WRITE ERROR,17,00\r");
    ck_assert_int_eq(ieee_drive_try_mount(0, "writable.d64", NULL), DOS_MEDIA_IO_ERROR);
    const size_t open_files = test_open_file_count();
    // Neither replacing the dirty target nor ejecting it as a partner may lose it.
    for (unsigned int slot = 0; slot < TEST_SLOTS_PER_UNIT; ++slot) {
        ck_assert_int_eq(ieee_drive_try_mount(slot, "peer.d80", NULL), DOS_MEDIA_IO_ERROR);
        ck_assert_str_eq(ieee_drive_filename(0), "writable.d64");
        ck_assert_str_eq(ieee_drive_filename(1), "partner.d64");
        ck_assert_uint_eq(test_open_file_count(), open_files);
    }
    ck_assert_str_eq(ieee_drive_filename(0), "writable.d64");
    ck_assert(!ieee_drive_unmount_all());
    ck_assert_str_eq(ieee_drive_filename(0), "writable.d64");
    ieee_drive_reset();
    status(8, "25, WRITE ERROR,17,00\r");
    status(9, "73,CBM DOS V2.7,00,00,0\r");
    test_fail_file_writes("/disks/writable.d64", false);
    commands(IEEE_CMD_LISTEN(8), IEEE_CMD_CLOSE(2));
    service();
    status(8, "00, OK,00,00\r");
    ck_assert(ieee_drive_unmount_all());
    ck_assert_int_eq(ieee_drive_try_mount(0, "writable.d64", NULL), DOS_MEDIA_OK);
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

// Unsupported candidates retain the old media and its native reader behavior.
START_TEST(test_invalid_image_preserves_mounted_unit) {
    mount_image(0, 0, "old.d64");
    mount_image(2, 1, "next.d80");
    ck_assert_str_eq(ieee_drive_filename(0), "old.d64");
    ck_assert_str_eq(ieee_drive_filename(1), "");
    const uint8_t invalid[DISKIMAGE_SECTOR_SIZE] = {0};
    test_register_binary_file("/disks/invalid.img", invalid, sizeof(invalid), false);
    ck_assert_int_eq(ieee_drive_try_mount(0, "invalid.img", NULL), DOS_MEDIA_UNSUPPORTED);
    ck_assert_str_eq(ieee_drive_filename(0), "old.d64");
    status(8, "73,CBM DOS V2,00,00\r");
    open_file(8, "BASIC,PRG,R");
    status(8, "00, OK,00,00\r");
}
END_TEST

// A queued bus command postpones replacement and partner ejection as one operation.
START_TEST(test_busy_mount_retains_pair_until_retry) {
    mount_image(0, 0, "old.d64");
    mount_image(1, 0, "partner.d64");
    mount_image(2, 1, "next.d80");
    const size_t open_files = test_open_file_count();
    mock_ieee_enqueue_rx(true, IEEE_CMD_UNLISTEN);
    mock_ieee_enqueue_atn_end();
    ck_assert_int_eq(ieee_drive_try_mount(_i, "next.d80", NULL), DOS_MEDIA_BUSY);
    ck_assert_str_eq(ieee_drive_filename(0), "old.d64");
    ck_assert_str_eq(ieee_drive_filename(1), "partner.d64");
    ck_assert_uint_eq(test_open_file_count(), open_files);
    service();
    ck_assert_int_eq(ieee_drive_try_mount(_i, "next.d80", NULL), DOS_MEDIA_OK);
    ck_assert_str_eq(ieee_drive_filename(_i), "next.d80");
    ck_assert_str_eq(ieee_drive_filename(_i ^ 1), "");
    ck_assert_str_eq(ieee_drive_filename(2), "");
    ck_assert_uint_eq(test_open_file_count(), open_files - 2);
}
END_TEST

// Published names own their storage and follow replacement, eject, reset, and teardown.
START_TEST(test_mounted_filenames_follow_live_bindings) {
    char name[] = "live.d64";
    mount_image(0, _i, name);
    mount_image(2, 0, "peer.d64");
    name[0] = 'x';
    ck_assert_str_eq(ieee_drive_filename(0), "live.d64");
    const size_t open_files = test_open_file_count();
    ieee_drive_mount_outcome_t outcome;
    ck_assert_int_eq(ieee_drive_try_mount(0, ieee_drive_filename(0), &outcome), DOS_MEDIA_OK);
    ck_assert_int_eq(outcome.moved_from_slot, -1);
    ck_assert_int_eq(outcome.ejected_partner, diskimage_type_none);
    ck_assert_str_eq(ieee_drive_filename(0), "live.d64");
    ck_assert_uint_eq(test_open_file_count(), open_files);
    ieee_drive_reset();
    ck_assert_str_eq(ieee_drive_filename(0), "live.d64");
    ck_assert_int_eq(ieee_drive_try_mount(0, NULL, NULL), DOS_MEDIA_OK);
    ck_assert_str_eq(ieee_drive_filename(0), "");
    ck_assert_str_eq(ieee_drive_filename(2), "peer.d64");
    ck_assert(ieee_drive_unmount_all());
    ck_assert_str_eq(ieee_drive_filename(2), "");
    mount_image(2, 0, "aborted.d64");
    ieee_drive_abort();
    ck_assert_str_eq(ieee_drive_filename(2), "");
}
END_TEST

// Missing or unsupported candidates neither leak handles nor replace the published name.
START_TEST(test_mount_failure_preserves_filename_and_closes_candidate) {
    mount_image(0, 0, "live.d64");
    mount_image(1, 0, "partner.d64");
    const uint8_t invalid[DISKIMAGE_SECTOR_SIZE] = {0};
    test_register_binary_file("/disks/invalid.d82", invalid, sizeof(invalid), false);
    const size_t open_files = test_open_file_count();
    ck_assert_int_eq(ieee_drive_try_mount(0, _i == 0 ? "missing.d64" : "invalid.d82", NULL),
                     _i == 0 ? DOS_MEDIA_IO_ERROR : DOS_MEDIA_UNSUPPORTED);
    ck_assert_str_eq(ieee_drive_filename(0), "live.d64");
    ck_assert_str_eq(ieee_drive_filename(1), "partner.d64");
    ck_assert_uint_eq(test_open_file_count(), open_files);
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

// Every ordered format pair preserves compatible partners or atomically ejects them.
START_TEST(test_mount_pair_policy) {
    const unsigned int slot = (unsigned int)_i % TEST_SLOTS_PER_UNIT;
    const unsigned int formats = (unsigned int)_i / TEST_SLOTS_PER_UNIT;
    const unsigned int existing = formats / TEST_FORMAT_COUNT;
    const unsigned int replacement = formats % TEST_FORMAT_COUNT;
    mount_image(0, existing, "old0.img");
    mount_image(1, existing, "old1.img");
    mount_image(2, replacement, "candidate.img");
    const size_t open_files = test_open_file_count();

    ieee_drive_mount_outcome_t outcome;
    ck_assert_int_eq(ieee_drive_try_mount(slot, "candidate.img", &outcome), DOS_MEDIA_OK);
    ck_assert_int_eq(outcome.moved_from_slot, 2);
    ck_assert_int_eq(outcome.ejected_partner,
                     existing == replacement ? diskimage_type_none : diskimage_type_d64 + existing);
    ck_assert_str_eq(ieee_drive_filename(slot), "candidate.img");
    ck_assert_str_eq(ieee_drive_filename(slot ^ 1u),
                     existing == replacement ? (slot == 0 ? "old1.img" : "old0.img") : "");
    ck_assert_str_eq(ieee_drive_filename(2), "");
    ck_assert_uint_eq(test_open_file_count(), open_files - 1 - (existing != replacement));

    // Removing the new disk must not eject a compatible partner or another unit.
    ck_assert_int_eq(ieee_drive_try_mount(slot, NULL, &outcome), DOS_MEDIA_OK);
    ck_assert_int_eq(outcome.moved_from_slot, -1);
    ck_assert_int_eq(outcome.ejected_partner, diskimage_type_none);
    ck_assert_str_eq(ieee_drive_filename(slot ^ 1u),
                     existing == replacement ? (slot == 0 ? "old1.img" : "old0.img") : "");
    ck_assert_str_eq(ieee_drive_filename(2), "");
}
END_TEST

// Moving either slot's image preserves both compatible partners and unrelated units.
START_TEST(test_moving_image_keeps_one_mount) {
    const unsigned int source = _i % 2 == 0 ? 6 : 4;
    const unsigned int destination = 7;
    const unsigned int format = (unsigned int)_i / 2;
    mount_image(source, format, "moving.img");
    if (source != (destination ^ 1u))
        mount_image(source ^ 1u, format, "source-partner.img");
    mount_image(destination, format, "old.img");
    mount_image(0, format, "unrelated.img");
    const size_t open_files = test_open_file_count();

    ck_assert_int_eq(ieee_drive_try_mount(destination, ieee_drive_filename(source), NULL), DOS_MEDIA_OK);
    ck_assert_str_eq(ieee_drive_filename(source), "");
    ck_assert_str_eq(ieee_drive_filename(destination), "moving.img");
    ck_assert_str_eq(ieee_drive_filename(0), "unrelated.img");
    if (source != (destination ^ 1u))
        ck_assert_str_eq(ieee_drive_filename(source ^ 1u), "source-partner.img");
    ck_assert_uint_eq(test_open_file_count(), open_files - 1);
    status(11, format == 0 ? "73,CBM DOS V2,00,00\r" : "73,CBM DOS V2.7,00,00,0\r");
    open_file(11, "1:BASIC,PRG,R");
    status(11, format == 0 ? "00, OK,00,00\r" : "00, OK,00,00,1\r");
}
END_TEST

// Canonical paths identify one mount, stay inside /disks, and reuse storage across aliases.
START_TEST(test_mounted_image_path_aliases) {
    uint8_t* const image = diskimage_test_make_d64();
    test_register_binary_file("/disks/folder/disk.d64", image, DISKIMAGE_D64_SIZE, false);
    free(image);
    ieee_drive_mount_outcome_t outcome;
    ck_assert_int_eq(ieee_drive_try_mount(0, "./folder//child/../disk.d64", &outcome), DOS_MEDIA_OK);
    ck_assert_str_eq(ieee_drive_filename(0), "folder/disk.d64");
    ck_assert_int_eq(outcome.moved_from_slot, -1);
    ck_assert_int_eq(outcome.ejected_partner, diskimage_type_none);
    mount_image(1, 0, "other.d64");
    const char* const aliases[] = {"./FOLDER//child/../DISK.d64", "../disks/folder/disk.d64"};
    for (size_t index = 0; index < count_of(aliases); ++index) {
        ck_assert_int_eq(ieee_drive_try_mount(2, aliases[index], &outcome), DOS_MEDIA_OK);
        ck_assert_int_eq(outcome.moved_from_slot, index == 0 ? 0 : -1);
        ck_assert_int_eq(outcome.ejected_partner, diskimage_type_none);
        ck_assert_str_eq(ieee_drive_filename(0), "");
    }
    ck_assert_str_eq(ieee_drive_filename(2), "folder/disk.d64");
    ck_assert_int_eq(ieee_drive_try_mount(3, "../disks/folder/disk.d64", &outcome), DOS_MEDIA_OK);
    ck_assert_int_eq(outcome.moved_from_slot, 2);
    ck_assert_int_eq(outcome.ejected_partner, diskimage_type_none);
    ck_assert_str_eq(ieee_drive_filename(2), "");
    ck_assert_str_eq(ieee_drive_filename(3), "folder/disk.d64");
    const char* const invalid[] = {"../roms/disk.d64", "../disks-elsewhere/disk.d64", "..", ".", "folder/.."};
    for (size_t index = 0; index < count_of(invalid); ++index) {
        ck_assert_int_eq(ieee_drive_try_mount(0, invalid[index], &outcome), DOS_MEDIA_INVALID);
        ck_assert_int_eq(outcome.moved_from_slot, 2);
        ck_assert_int_eq(outcome.ejected_partner, diskimage_type_none);
        ck_assert_str_eq(ieee_drive_filename(0), "");
        ck_assert_str_eq(ieee_drive_filename(3), "folder/disk.d64");
    }
    ck_assert_str_eq(ieee_drive_filename(1), "other.d64");
    ck_assert_uint_eq(test_file_open_calls("/disks/folder/disk.d64"), 1);
}
END_TEST

// Filesystem aliases must move one owner, including directory and leaf short names.
START_TEST(test_filesystem_aliases_reuse_image_owner) {
    mount_writable();
    const char* const aliases[] = {
        "/disks/writable.d64.",
        "/disks/writable.d64 ",
        "/disks/WRITAB~1.D64",
        "/disks/ALIAS~1/WRITAB~1.D64",
    };
    for (size_t index = 0; index < count_of(aliases); ++index) {
        test_register_file_alias(aliases[index], "/disks/writable.d64");
        ieee_drive_mount_outcome_t outcome;
        ck_assert_int_eq(ieee_drive_try_mount(index + 1, aliases[index] + strlen("/disks/"), &outcome), DOS_MEDIA_OK);
        ck_assert_int_eq(outcome.moved_from_slot, index);
        ck_assert_str_eq(ieee_drive_filename(index), "");
        ck_assert_str_eq(ieee_drive_filename(index + 1), "writable.d64");
        ck_assert_uint_eq(test_open_file_count(), 1);
        ck_assert_uint_eq(test_file_open_calls("/disks/writable.d64"), 1);
    }
}
END_TEST

// Distinct CP932 characters can contain ASCII upper/lowercase trail bytes.
START_TEST(test_cp932_image_names_remain_distinct) {
    mount_image(0, 0, "\x81\x41.d64");
    mount_image(1, 0, "\x81\x61.d64");
    ck_assert_str_eq(ieee_drive_filename(0), "\x81\x41.d64");
    ck_assert_str_eq(ieee_drive_filename(1), "\x81\x61.d64");
    ck_assert_uint_eq(test_open_file_count(), 2);
}
END_TEST

// Backend path spellings must not turn a lexically contained request into outside storage.
START_TEST(test_filesystem_alias_outside_disks_is_rejected) {
    mount_writable();
    uint8_t* const image = diskimage_test_make_d64();
    test_register_binary_file("/roms/outside.d64", image, DISKIMAGE_D64_SIZE, true);
    free(image);
    test_register_file_alias("/disks/..\\roms\\outside.d64", "/roms/outside.d64");
    ieee_drive_mount_outcome_t outcome = {.moved_from_slot = 7};
    ck_assert_int_ne(ieee_drive_try_mount(0, "..\\roms\\outside.d64", &outcome), DOS_MEDIA_OK);
    ck_assert_int_eq(outcome.moved_from_slot, 7);
    ck_assert_str_eq(ieee_drive_filename(0), "writable.d64");
    ck_assert_uint_eq(test_file_open_calls("/roms/outside.d64"), 0);
    ck_assert_uint_eq(test_open_file_count(), 1);
}
END_TEST

// Full-drive moves and same-slot remounts must reuse the sole writable storage handle.
START_TEST(test_writable_moves_reuse_storage_handle) {
    mount_writable();
    for (unsigned int slot = 1; slot < TEST_UNIT_COUNT * TEST_SLOTS_PER_UNIT; ++slot) {
        char name[32];
        snprintf(name, sizeof(name), "drive%u.d64", slot);
        mount_image(slot, 0, name);
    }
    const size_t open_files = test_open_file_count();
    const unsigned int destinations[] = {0, 1, 6};
    unsigned int source = 0;
    for (unsigned int index = 0; index < count_of(destinations); ++index) {
        const unsigned int destination = destinations[index];
        ck_assert_int_eq(ieee_drive_try_mount(destination, ieee_drive_filename(source), NULL), DOS_MEDIA_OK);
        ck_assert_uint_eq(test_file_open_calls("/disks/writable.d64"), 1);
        ck_assert_str_eq(ieee_drive_filename(destination), "writable.d64");
        if (source != destination) ck_assert_str_eq(ieee_drive_filename(source), "");
        ck_assert_uint_eq(test_open_file_count(), open_files - index);
        source = destination;
    }
}
END_TEST

// Pending work in a later unit rejects moves and teardown before earlier status or writes change.
START_TEST(test_busy_lifecycle_preserves_earlier_unit) {
    mount_writable();
    open_channel(8, 2, "0:FILE,S,W");
    write_payload(8, 22);
    uint8_t* const image = diskimage_test_make_d64();
    test_register_binary_file("/disks/busy.d64", image, DISKIMAGE_D64_SIZE, true);
    free(image);
    ck_assert_int_eq(ieee_drive_try_mount(2, "busy.d64", NULL), DOS_MEDIA_OK);

    // Leave an unconsumed suffix in the earlier unit's status FIFO.
    commands(IEEE_CMD_TALK(8), IEEE_CMD_SECONDARY(IEEE_COMMAND_CHANNEL));
    ieee_drive_task();
    mock_ieee_consume_status(3);
    commands(IEEE_CMD_UNTALK, IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    const size_t remaining = mock_ieee_status_count();
    ck_assert_uint_gt(remaining, 0);
    const uint8_t first = mock_ieee_status_byte(0);

    // Dispatch the format on the last RX entry, leaving DOS busy but transport idle.
    commands(IEEE_CMD_LISTEN(9), IEEE_CMD_SECONDARY(IEEE_COMMAND_CHANNEL));
    const char* const format = "N:BUSY,ID";
    for (size_t index = 0; index < strlen(format); ++index)
        mock_ieee_enqueue_rx_eoi(false, (uint8_t)format[index], format[index + 1] == '\0');
    mock_ieee_enqueue_rx(true, IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_STATUS_ADDR) & ECONOPET_IEEE_ST_RX_AVAIL_MASK, 0);
    ck_assert_int_eq(ieee_drive_try_mount(2, NULL, NULL), DOS_MEDIA_BUSY);
    test_fail_file_writes("/disks/writable.d64", true);
    const dos_media_result_t moved = _i == 0                    ? ieee_drive_try_mount(2, "writable.d64", NULL)
                                     : ieee_drive_unmount_all() ? DOS_MEDIA_OK
                                                                : DOS_MEDIA_BUSY;
    test_fail_file_writes("/disks/writable.d64", false);
    const size_t retained = mock_ieee_status_count();
    const uint8_t retained_first = retained != 0 ? mock_ieee_status_byte(0) : 0;
    ck_assert_str_eq(ieee_drive_filename(0), "writable.d64");
    ck_assert_str_eq(ieee_drive_filename(2), "busy.d64");

    // Complete the other unit's format before the fixture retires its storage.
    dos_media_result_t result = DOS_MEDIA_BUSY;
    for (unsigned int pass = 0; pass < TEST_FORMAT_SERVICE_PASSES && result == DOS_MEDIA_BUSY; ++pass) {
        ieee_drive_task();
        result = ieee_drive_try_mount(2, NULL, NULL);
    }
    ck_assert_int_eq(result, DOS_MEDIA_OK);
    ck_assert_int_eq(moved, DOS_MEDIA_BUSY);
    ck_assert_uint_eq(retained, remaining);
    ck_assert_uint_eq(retained_first, first);
    status(8, " OK,00,00\r");
}
END_TEST

// A failed source writeback retains every binding, in either unit-finalization order.
START_TEST(test_failed_image_move_preserves_source_until_retry) {
    const unsigned int source = _i == 0 ? 0 : 2;
    const unsigned int destination = source ^ 2u;
    const unsigned int device = 8 + source / TEST_SLOTS_PER_UNIT;
    mount_writable();
    if (source != 0)
        ck_assert_int_eq(ieee_drive_try_mount(source, "writable.d64", NULL), DOS_MEDIA_OK);
    mount_image(source ^ 1u, 0, "source-partner.d64");
    mount_image(destination, 1, "destination.d80");
    mount_image(destination ^ 1u, 1, "destination-partner.d80");
    open_channel(device, 2, "0:FILE,S,W");
    write_payload(device, 22);
    test_fail_file_writes("/disks/writable.d64", true);
    const size_t open_files = test_open_file_count();
    ieee_drive_mount_outcome_t outcome = {
        .moved_from_slot = 7,
        .ejected_partner = diskimage_type_d82,
    };
    ck_assert_int_eq(ieee_drive_try_mount(destination, "writable.d64", &outcome), DOS_MEDIA_IO_ERROR);
    ck_assert_int_eq(outcome.moved_from_slot, 7);
    ck_assert_int_eq(outcome.ejected_partner, diskimage_type_d82);
    ck_assert_str_eq(ieee_drive_filename(source), "writable.d64");
    ck_assert_str_eq(ieee_drive_filename(source ^ 1u), "source-partner.d64");
    ck_assert_str_eq(ieee_drive_filename(destination), "destination.d80");
    ck_assert_str_eq(ieee_drive_filename(destination ^ 1u), "destination-partner.d80");
    ck_assert_uint_eq(test_open_file_count(), open_files);

    test_fail_file_writes("/disks/writable.d64", false);
    ck_assert_int_eq(ieee_drive_try_mount(destination, "writable.d64", &outcome), DOS_MEDIA_OK);
    ck_assert_int_eq(outcome.moved_from_slot, source);
    ck_assert_int_eq(outcome.ejected_partner, diskimage_type_d80);
    ck_assert_str_eq(ieee_drive_filename(source), "");
    ck_assert_str_eq(ieee_drive_filename(source ^ 1u), "source-partner.d64");
    ck_assert_str_eq(ieee_drive_filename(destination), "writable.d64");
    ck_assert_str_eq(ieee_drive_filename(destination ^ 1u), "");
    ck_assert_uint_eq(test_open_file_count(), open_files - 2);
    const unsigned int target_device = 8 + destination / TEST_SLOTS_PER_UNIT;
    open_channel(target_device, 2, "0:FILE,S,R");
    commands(IEEE_CMD_TALK(target_device), IEEE_CMD_SECONDARY(2));
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
    ieee_drive_try_mount(0, name, NULL);
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
    tcase_add_test(test_case, test_invalid_image_preserves_mounted_unit);
    tcase_add_loop_test(test_case, test_mount_pair_policy, 0,
                       TEST_SLOTS_PER_UNIT * TEST_FORMAT_COUNT * TEST_FORMAT_COUNT);
    tcase_add_loop_test(test_case, test_moving_image_keeps_one_mount, 0, 2 * TEST_FORMAT_COUNT);
    tcase_add_test(test_case, test_mounted_image_path_aliases);
    tcase_add_test(test_case, test_filesystem_aliases_reuse_image_owner);
    tcase_add_test(test_case, test_cp932_image_names_remain_distinct);
    tcase_add_test(test_case, test_filesystem_alias_outside_disks_is_rejected);
    tcase_add_loop_test(test_case, test_failed_image_move_preserves_source_until_retry, 0, 2);
    tcase_add_loop_test(test_case, test_busy_mount_retains_pair_until_retry, 0, TEST_SLOTS_PER_UNIT);
    tcase_add_loop_test(test_case, test_mounted_filenames_follow_live_bindings, 0, TEST_FORMAT_COUNT);
    tcase_add_loop_test(test_case, test_mount_failure_preserves_filename_and_closes_candidate, 0, 2);
    tcase_add_test(test_case, test_native_rel_type_checks_and_mode_defaults);
    tcase_add_test(test_case, test_failed_write_survives_lifecycle_and_remounts_persisted_bytes);
    tcase_add_test(test_case, test_mount_path_boundary);
    tcase_add_loop_test(test_case, test_unit_presence_controls_enabled_readback, 0, TEST_UNIT_COUNT);
    tcase_add_test(test_case, test_writable_moves_reuse_storage_handle);
    tcase_add_loop_test(test_case, test_busy_lifecycle_preserves_earlier_unit, 0, 2);
    suite_add_tcase(suite, test_case);
    return suite;
}

Suite* ieee_drive_fatal_suite(void) {
    Suite* suite = suite_create("ieee_drive-fatal");
    TCase* test_case = tcase_create("paths");
    tcase_add_checked_fixture(test_case, setup, teardown);
    tcase_add_test_raise_signal(test_case, test_mount_rejects_overlong_path, SIGABRT);
    suite_add_tcase(suite, test_case);
    return suite;
}

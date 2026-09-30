// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

// Host-level contract tests for ieee_drive.c. The tests inject PET IEEE-488
// command/data bytes into the FPGA-register mock and assert the FIFO bytes and
// EOI markers the firmware sends back. Disk images are generated in memory so
// the suite does not depend on the SD-card filesystem.

#include "pch.h"
#include "ieee_drive_test.h"

#include <stdlib.h>
#include <string.h>

#include "diskimage_test.h"
#include "ieee/diskimage.h"
#include "ieee/ieee_drive.h"
#include "ieee/ieee_protocol.h"
#include "mock.h"

#define IEEE_FIRST_DEVICE 8u
#define IEEE_DEVICE_COUNT 4u
#define IEEE_DRIVES_PER_DEVICE 2u

// ---------------------------------------------------------------------------
// IEEE command-stream helpers
// ---------------------------------------------------------------------------

// Queues ATN-tagged IEEE command bytes in their caller-supplied order.
static void enqueue_commands(const uint8_t* commands, size_t count) {
    for (size_t index = 0; index < count; index++) {
        mock_ieee_enqueue_rx(true, commands[index]);
    }
}

// Enqueues one or more ATN-tagged command bytes. Listener data (filenames,
// channel-15 command payloads, and file writes) is enqueued separately.
#define enqueue_command(...) do { \
    const uint8_t commands[] = { __VA_ARGS__ }; \
    enqueue_commands(commands, ARRAY_SIZE(commands)); \
} while (0)

// Queues an untagged PETSCII filename or command payload.
static void enqueue_name(const char* name) {
    for (const char* c = name; *c != '\0'; c++) mock_ieee_enqueue_rx(false, (uint8_t) *c);
}

// Asserts the status FIFO contains a complete dual-drive DOS status record.
static void assert_status(unsigned int expected_code, unsigned int expected_drive) {
    char line[40];
    const size_t length = mock_ieee_status_count();
    unsigned int code, track, sector, drive;
    char message[24];
    int consumed = 0;

    ck_assert_uint_lt(length, sizeof(line));
    for (size_t index = 0; index < length; index++) {
        line[index] = (char) mock_ieee_status_byte(index);
        ck_assert_int_eq(mock_ieee_status_eoi(index), index == length - 1);
    }
    line[length] = '\0';

    ck_assert_int_eq(sscanf(line, "%2u,%23[^,],%2u,%2u,%u\r%n",
                            &code, message, &track, &sector, &drive, &consumed), 5);
    ck_assert_int_eq(consumed, length);
    ck_assert_uint_eq(code, expected_code);
    ck_assert_uint_eq(track, 0);
    ck_assert_uint_eq(sector, 0);
    ck_assert_uint_eq(drive, expected_drive);
}

// Converts external device/drive coordinates to the production mount slot.
static unsigned int device_slot(unsigned int device, unsigned int drive) {
    ck_assert_uint_ge(device, IEEE_FIRST_DEVICE);
    ck_assert_uint_lt(device, IEEE_FIRST_DEVICE + IEEE_DEVICE_COUNT);
    ck_assert_uint_lt(drive, IEEE_DRIVES_PER_DEVICE);
    return (device - IEEE_FIRST_DEVICE) * IEEE_DRIVES_PER_DEVICE + drive;
}

// Registers an image with the mock filesystem and mounts it in one drive slot.
static void mount_image(unsigned int device, unsigned int drive, const char* filename,
                        uint8_t* image, size_t image_size, bool writable) {
    char path[64];
    snprintf(path, sizeof(path), "/disks/%s", filename);
    mock_register_binary_file(path, image, image_size, writable);
    free(image);
    ck_assert(ieee_drive_mount(device_slot(device, drive), filename));
}

// Mounts a generated D64 fixture with the requested mock write permission.
static void mount_d64(unsigned int device, unsigned int drive, const char* filename,
                      bool writable) {
    mount_image(device, drive, filename, diskimage_test_make_d64(), DISKIMAGE_D64_SIZE,
                writable);
}

// Mounts a generated D80 fixture with the requested mock write permission.
static void mount_d80(unsigned int device, unsigned int drive, const char* filename,
                      bool writable) {
    mount_image(device, drive, filename, diskimage_test_make_d80(), DISKIMAGE_D80_SIZE,
                writable);
}

// Use with a loop index that enumerates device/drive slots from 0 through 7:
// 8.0, 8.1, 9.0, 9.1, 10.0, 10.1, 11.0, 11.1.
static unsigned int loop_device(int loop_index) {
    ck_assert_int_ge(loop_index, 0);
    ck_assert_int_lt(loop_index, IEEE_DEVICE_COUNT * IEEE_DRIVES_PER_DEVICE);
    return IEEE_FIRST_DEVICE + (unsigned int) loop_index / IEEE_DRIVES_PER_DEVICE;
}

// Use with the same device/drive slot index passed to loop_device().
static unsigned int loop_drive(int loop_index) {
    ck_assert_int_ge(loop_index, 0);
    ck_assert_int_lt(loop_index, IEEE_DEVICE_COUNT * IEEE_DRIVES_PER_DEVICE);
    return (unsigned int) loop_index % IEEE_DRIVES_PER_DEVICE;
}

// Use with a loop index that enumerates devices only from 0 through 3:
// 8, 9, 10, 11.
static unsigned int loop_device_only(int loop_index) {
    ck_assert_int_ge(loop_index, 0);
    ck_assert_int_lt(loop_index, IEEE_DEVICE_COUNT);
    return IEEE_FIRST_DEVICE + (unsigned int) loop_index;
}

// Queues an optional drive-1 prefix followed by a filename for an OPEN command.
static void enqueue_filename_for_drive(unsigned int drive, const char* filename) {
    ck_assert_uint_lt(drive, IEEE_DRIVES_PER_DEVICE);
    if (drive == 1) {
        mock_ieee_enqueue_rx(false, '1');
        mock_ieee_enqueue_rx(false, ':');
    }
    enqueue_name(filename);
}

// Resets the register/filesystem mock and initializes the IEEE drive under test.
static void setup(void) {
    mock_reset();
    ieee_drive_init();
}

// Unmounts images and clears mock files so every test starts isolated.
static void teardown(void) {
    ieee_drive_unmount_all();
    mock_clear_files();
}

// ---------------------------------------------------------------------------
// Sequential files and status channel
// ---------------------------------------------------------------------------

// Verifies a standard D64 PRG read over the firmware/FPGA FIFO boundary.
START_TEST(test_d64_mount_open_status_and_stream) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Mount the shared D64 fixture in the current device and drive slot.
    mount_d64(device, drive, "drive.d64", true);

    // Associate channel 0 with BASIC before UNLISTEN resolves the name.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(0));
    enqueue_filename_for_drive(drive, "BASIC,PRG");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    // Read the completed OPEN status through the unit's command channel.
    enqueue_command(IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(15));
    ieee_drive_task();
    assert_status(0, drive);

    // Readdress the file channel and inspect the complete sector-chain stream.
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(0));
    ieee_drive_task();

    // OPEN must return DOS success before the file channel is addressed.
    ck_assert_uint_eq(mock_ieee_data_count(), 264);
    for (size_t index = 0; index < 254; index++) {
        // The first sector contributes 254 bytes without EOI.
        ck_assert_uint_eq(mock_ieee_data_byte(index), index + 2);
        ck_assert(!mock_ieee_data_eoi(index));
    }
    for (size_t index = 0; index < 10; index++) {
        // The final sector contributes 10 bytes and EOI belongs to its tail.
        ck_assert_uint_eq(mock_ieee_data_byte(254 + index), 0xe2 + index);
        ck_assert_int_eq(mock_ieee_data_eoi(254 + index), index == 9);
    }
}
END_TEST

// Verifies replacing an existing mount in one slot serves the new image.
START_TEST(test_replacing_mount_uses_new_image) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);
    // Build a replacement image whose first BASIC byte is distinguishable.
    uint8_t* replacement = diskimage_test_make_d64();
    replacement[diskimage_test_d64_offset(17, 0) + 2] = 0xa5;

    // Mount an initial image, then replace that same external drive slot.
    mount_d64(device, drive, "first.d64", true);
    mount_image(device, drive, "replacement.d64", replacement, DISKIMAGE_D64_SIZE, true);

    // OPEN and TALK BASIC through the replacement mount.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(0));
    enqueue_filename_for_drive(drive, "BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(0));
    ieee_drive_task();

    // The stream length and EOI remain valid, while byte zero proves replacement.
    ck_assert_uint_eq(mock_ieee_data_count(), 264);
    ck_assert_uint_eq(mock_ieee_data_byte(0), 0xa5);
    ck_assert(mock_ieee_data_eoi(263));
}
END_TEST

// Verifies the post-EOF continuation response expected by the current drive.
START_TEST(test_read_past_eof_returns_eoied_carriage_return) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Mount the source image and issue the initial complete file read.
    mount_d64(device, drive, "drive.d64", true);

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(0));
    enqueue_filename_for_drive(drive, "BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(0));
    ieee_drive_task();
    // The initial stream must contain the complete two-sector fixture.
    ck_assert_uint_eq(mock_ieee_data_count(), 264);

    // Model the controller consuming the first EOF-terminated response.
    mock_ieee_clear_data();

    // Resume the same channel after EOF and inspect the continuation reply.
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(0));
    ieee_drive_task();
    // A post-EOF TALK returns only a carriage return marked EOI.
    ck_assert_uint_eq(mock_ieee_data_count(), 1);
    ck_assert_uint_eq(mock_ieee_data_byte(0), '\r');
    ck_assert(mock_ieee_data_eoi(0));
}
END_TEST

// Verifies that a newly mounted unit exposes its power-on status before I/O.
START_TEST(test_power_on_status_is_served_before_any_operation) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Mount without opening a file, leaving the unit in power-on state.
    mount_d64(device, drive, "drive.d64", true);

    // Read channel 15 and verify its initial informational status.
    enqueue_command(IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(15));
    ieee_drive_task();
    // Newly mounted units report the firmware power-on identification.
    assert_status(73, 0);
}
END_TEST

// Verifies that reading a status line acknowledges and clears it to status 00.
START_TEST(test_status_read_resets_to_ok) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Read the power-on status once.
    mount_d64(device, drive, "drive.d64", true);

    enqueue_command(IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(15));
    ieee_drive_task();
    // The first read observes status 73.
    assert_status(73, 0);

    // Consume the mock response, then readdress channel 15.
    mock_ieee_clear_status();

    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(15));
    ieee_drive_task();
    // Status reads acknowledge the line, leaving the next read at status 00.
    assert_status(0, 0);
}
END_TEST

// Verifies a failed filename OPEN is reported through channel 15 as status 62.
START_TEST(test_missing_file_reports_status_62) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Mount a fixture whose directory has no MISSING entry.
    mount_d64(device, drive, "drive.d64", true);

    // Attempt the OPEN, then read its resulting error from channel 15.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(0));
    enqueue_filename_for_drive(drive, "MISSING");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(15));
    ieee_drive_task();
    // The failed directory lookup must become DOS error 62 on channel 15.
    assert_status(62, drive);
}
END_TEST

// ---------------------------------------------------------------------------
// Addressing and command-channel behavior
// ---------------------------------------------------------------------------

// Verifies channel-15 status is isolated between every pair of adjacent devices.
START_TEST(test_status_is_isolated_per_unit) {
    const unsigned int source_device = loop_device_only(_i);
    const unsigned int target_device = loop_device_only((_i + 1) % IEEE_DEVICE_COUNT);

    // Give adjacent devices independent mounted images.
    mount_d64(source_device, 0, "source.d64", true);
    mount_d64(target_device, 0, "target.d64", true);

    // Fail an OPEN on one device, but ask its neighbor for status.
    enqueue_command(IEEE_CMD_LISTEN(source_device), IEEE_CMD_OPEN(0));
    enqueue_name("MISSING");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(target_device), IEEE_CMD_SECONDARY(15));
    ieee_drive_task();
    // The untouched neighbor retains its independent power-on status.
    assert_status(73, 0);
}
END_TEST

// Verifies a drive prefix routes a filename to drive 1 within every device.
START_TEST(test_drive_prefix_selects_second_drive_in_unit) {
    const unsigned int device = loop_device_only(_i);

    // Build two disks and give drive 1 a payload byte unique to this test.
    uint8_t* drive_zero = diskimage_test_make_d64();
    uint8_t* drive_one = diskimage_test_make_d64();
    drive_one[diskimage_test_d64_offset(17, 0) + 2] = 0xa5;
    mount_image(device, 0, "zero.d64", drive_zero, DISKIMAGE_D64_SIZE, true);
    mount_image(device, 1, "one.d64", drive_one, DISKIMAGE_D64_SIZE, true);

    // Use the 1: filename prefix and confirm the stream came from drive 1.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(0));
    enqueue_name("1:BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(0));
    ieee_drive_task();

    // Drive 1 has the unique first byte while preserving the normal stream shape.
    ck_assert_uint_eq(mock_ieee_data_count(), 264);
    ck_assert_uint_eq(mock_ieee_data_byte(0), 0xa5);
    ck_assert(mock_ieee_data_eoi(263));
}
END_TEST

// Verifies CLOSE stops a sequential stream and allows the channel to be reopened.
START_TEST(test_close_then_reopen_streams_file) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Open BASIC once so CLOSE has an active sequential channel to terminate.
    mount_d64(device, drive, "drive.d64", true);

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(0));
    enqueue_filename_for_drive(drive, "BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_CLOSE(0), IEEE_CMD_UNLISTEN,
                    IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(0));
    ieee_drive_task();
    // TALK after CLOSE cannot resume the discarded stream.
    ck_assert_uint_eq(mock_ieee_data_count(), 0);

    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(0));
    enqueue_filename_for_drive(drive, "BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(0));
    ieee_drive_task();

    // Reopening the channel recreates the full BASIC stream from its start.
    ck_assert_uint_eq(mock_ieee_data_count(), 264);
    ck_assert_uint_eq(mock_ieee_data_byte(0), 2);
    ck_assert(mock_ieee_data_eoi(263));
}
END_TEST

// Verifies media unmount invalidates channels backed by the removed image.
START_TEST(test_unmount_clears_open_sequential_channel) {
    // Exhaust BASIC so stale state would answer a later TALK with its EOF reply.
    mount_d64(IEEE_FIRST_DEVICE, 0, "first.d64", true);
    enqueue_command(IEEE_CMD_LISTEN(IEEE_FIRST_DEVICE), IEEE_CMD_OPEN(0));
    enqueue_name("BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(IEEE_FIRST_DEVICE),
                    IEEE_CMD_SECONDARY(0));
    ieee_drive_task();
    ck_assert_uint_eq(mock_ieee_data_count(), 264);

    mock_ieee_clear_data();
    ieee_drive_unmount_all();
    mount_d64(IEEE_FIRST_DEVICE, 0, "second.d64", true);

    // Remounting media must not resurrect a channel opened on the old image.
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_TALK(IEEE_FIRST_DEVICE),
                    IEEE_CMD_SECONDARY(0));
    ieee_drive_task();
    ck_assert_uint_eq(mock_ieee_data_count(), 0);
}
END_TEST

// Verifies unmount discards REL channels that reference the removed image.
START_TEST(test_unmount_clears_open_relative_channel) {
    mount_d64(IEEE_FIRST_DEVICE, 0, "first.d64", true);
    enqueue_command(IEEE_CMD_LISTEN(IEEE_FIRST_DEVICE), IEEE_CMD_OPEN(2));
    enqueue_name("DATA,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    ieee_drive_unmount_all();
    mount_d64(IEEE_FIRST_DEVICE, 0, "second.d64", true);

    // TALK alone cannot reuse the REL chain cached for the previous image.
    enqueue_command(IEEE_CMD_TALK(IEEE_FIRST_DEVICE), IEEE_CMD_SECONDARY(2));
    ieee_drive_task();
    ck_assert_uint_eq(mock_ieee_data_count(), 0);
}
END_TEST

// Verifies commands directed at unimplemented primary addresses are ignored.
START_TEST(test_non_target_addresses_are_ignored) {
    static const unsigned int non_target_devices[] = { 0, 7, 12, 31 };

    mount_d64(IEEE_FIRST_DEVICE, 0, "drive.d64", true);

    // Exercise addresses below, above, and outside the five-bit device range.
    for (size_t index = 0; index < ARRAY_SIZE(non_target_devices); index++) {
        const unsigned int device = non_target_devices[index];
        enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(0));
        enqueue_name("BASIC");
        enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(0));
        ieee_drive_task();
    }

    // No unowned address may create data or command-channel output.
    ck_assert_uint_eq(mock_ieee_data_count(), 0);
    ck_assert_uint_eq(mock_ieee_status_count(), 0);
}
END_TEST

// Verifies secondary addresses with the same low nibble select the same channel.
START_TEST(test_secondary_address_alias_selects_data_channel) {
    mount_d64(IEEE_FIRST_DEVICE, 0, "drive.d64", true);

    // Address BASIC on channel 0, then TALK with secondary address 16.
    enqueue_command(IEEE_CMD_LISTEN(IEEE_FIRST_DEVICE), IEEE_CMD_OPEN(0));
    enqueue_name("BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(IEEE_FIRST_DEVICE),
                    IEEE_CMD_SECONDARY(16));
    ieee_drive_task();

    // The low-nibble alias of 16 must select channel 0's full stream.
    ck_assert_uint_eq(mock_ieee_data_count(), 264);
    ck_assert_uint_eq(mock_ieee_data_byte(0), 2);
    ck_assert(mock_ieee_data_eoi(263));
}
END_TEST

// Verifies a channel-15 I command clears a previously generated error status.
START_TEST(test_initialize_command_clears_error_status) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Establish a FILE NOT FOUND status through an unsuccessful OPEN.
    mount_d64(device, drive, "drive.d64", true);

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(0));
    enqueue_filename_for_drive(drive, "MISSING");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(15));

    // Send I to the command channel, then read the cleared status.
    mock_ieee_enqueue_rx(false, 'I');
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(15));
    ieee_drive_task();
    // I clears the pending file-not-found error to DOS status 00.
    assert_status(0, drive);
}
END_TEST

// Verifies an unreadable successor in a sequential chain reports status 23.
START_TEST(test_corrupt_sequential_chain_reports_read_error) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);
    // Corrupt BASIC's successor link after a valid first sector.
    uint8_t* image = diskimage_test_make_d64();
    uint8_t* sector = image + diskimage_test_d64_offset(17, 0);
    sector[0] = 36;
    sector[1] = 0;
    mount_image(device, drive, "corrupt.d64", image, DISKIMAGE_D64_SIZE, true);

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(0));
    enqueue_filename_for_drive(drive, "BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(0));
    ieee_drive_task();

    // Valid prefix bytes are sent, followed by an EOI'd carriage-return fallback.
    ck_assert_uint_eq(mock_ieee_data_count(), 255);
    for (size_t index = 0; index < 254; index++) {
        ck_assert_uint_eq(mock_ieee_data_byte(index), index + 2);
        ck_assert(!mock_ieee_data_eoi(index));
    }
    ck_assert_uint_eq(mock_ieee_data_byte(254), '\r');
    ck_assert(mock_ieee_data_eoi(254));

    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(15));
    ieee_drive_task();
    // The incomplete chain records DOS read error 23 on channel 15.
    assert_status(23, drive);
}
END_TEST

// Verifies a channel-15 V command clears a previously generated error status.
START_TEST(test_validate_command_clears_error_status) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    mount_d64(device, drive, "drive.d64", true);

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(0));
    enqueue_filename_for_drive(drive, "MISSING");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(15));

    mock_ieee_enqueue_rx(false, 'V');
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(15));
    ieee_drive_task();
    // V clears the pending file-not-found error to DOS status 00.
    assert_status(0, drive);
}
END_TEST

// ---------------------------------------------------------------------------
// Relative files
// ---------------------------------------------------------------------------

// Verifies REL positioning and a record read that crosses a sector boundary.
START_TEST(test_relative_file_position_and_read) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Mount the fixture and associate its DATA REL file with channel 2.
    mount_d64(device, drive, "rel.d64", true);

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(2));
    enqueue_filename_for_drive(drive, "DATA,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    // Position channel 2 at record 2: P, secondary channel, low, high.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(15));
    mock_ieee_enqueue_rx(false, 'P');
    mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(2));
    mock_ieee_enqueue_rx(false, 2);
    mock_ieee_enqueue_rx(false, 0);
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(2));
    ieee_drive_task();

    // Record 2 crosses sectors in the fixture, so verify both byte runs and EOI.
    ck_assert_uint_eq(mock_ieee_data_count(), 129);
    for (size_t index = 0; index < 125; index++) {
        ck_assert_uint_eq(mock_ieee_data_byte(index), 0xa0);
        ck_assert(!mock_ieee_data_eoi(index));
    }
    for (size_t index = 125; index < 129; index++) {
        ck_assert_uint_eq(mock_ieee_data_byte(index), 0xa1);
        ck_assert_int_eq(mock_ieee_data_eoi(index), index == 128);
    }
}
END_TEST

// Verifies P's one-based record offset selects the expected suffix.
START_TEST(test_relative_position_within_record) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Open the generated REL file on channel 2.
    mount_d64(device, drive, "rel.d64", true);

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(2));
    enqueue_filename_for_drive(drive, "DATA,L");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(15));
    mock_ieee_enqueue_rx(false, 'P');
    mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(2));
    mock_ieee_enqueue_rx(false, 2);
    mock_ieee_enqueue_rx(false, 0);
    // P selects record 2 at one-based offset 125, one byte before its boundary.
    mock_ieee_enqueue_rx(false, 125);
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(2));
    ieee_drive_task();

    // One byte remains in the first sector, followed by four from the next.
    ck_assert_uint_eq(mock_ieee_data_count(), 5);
    ck_assert_uint_eq(mock_ieee_data_byte(0), 0xa0);
    for (size_t index = 1; index < 5; index++) {
        ck_assert_uint_eq(mock_ieee_data_byte(index), 0xa1);
        ck_assert_int_eq(mock_ieee_data_eoi(index), index == 4);
    }
}
END_TEST

// Verifies a REL position beyond the fixture's final record reports status 50.
START_TEST(test_relative_position_past_end_reports_50) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Open the REL file and select channel 15 for a position command.
    mount_d64(device, drive, "rel.d64", true);

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(2));
    enqueue_filename_for_drive(drive, "DATA,L");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(15));

    // Select record 7, beyond the generated chain's usable records.
    mock_ieee_enqueue_rx(false, 'P');
    mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(2));
    mock_ieee_enqueue_rx(false, 7);
    mock_ieee_enqueue_rx(false, 0);
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(15));
    ieee_drive_task();

    // The command channel exposes the missing-record DOS status.
    // The command channel exposes the missing-record DOS status.
    assert_status(50, drive);
}
END_TEST

// Verifies REL writes surface the correct status when the mounted image is read-only.
START_TEST(test_relative_write_to_read_only_image_reports_26) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Mount the REL fixture as read-only and open DATA on channel 2.
    mount_d64(device, drive, "rel.d64", false);

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(2));
    enqueue_filename_for_drive(drive, "DATA,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    // Write one byte and UNLISTEN, which commits the record write attempt.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(2));
    mock_ieee_enqueue_rx(false, 'Z');
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(15));
    ieee_drive_task();

    // The command channel must report write protection instead of success.
    // A failed commit must report write protection rather than status 00.
    assert_status(26, drive);
}
END_TEST

// Verifies a writable REL mount updates an existing record in place.
START_TEST(test_relative_write_updates_existing_record) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Mount writable media and open DATA on its REL channel.
    mount_d64(device, drive, "rel.d64", true);

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(2));
    enqueue_filename_for_drive(drive, "DATA,L");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(2));
    // LISTEN two replacement bytes and commit them at UNLISTEN.
    mock_ieee_enqueue_rx(false, 'O');
    mock_ieee_enqueue_rx(false, 'K');
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(15));
    // Reposition to record 1, then TALK the modified channel back.
    mock_ieee_enqueue_rx(false, 'P');
    mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(2));
    mock_ieee_enqueue_rx(false, 1);
    mock_ieee_enqueue_rx(false, 0);
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(2));
    ieee_drive_task();

    // Trailing zero-fill trims the record to the exact two written bytes.
    ck_assert_uint_eq(mock_ieee_data_count(), 2);
    ck_assert_uint_eq(mock_ieee_data_byte(0), 'O');
    ck_assert(!mock_ieee_data_eoi(0));
    ck_assert_uint_eq(mock_ieee_data_byte(1), 'K');
    ck_assert(mock_ieee_data_eoi(1));
}
END_TEST

// Verifies generated REL record lengths across the supported 1..254 range.
START_TEST(test_relative_record_lengths_stream_exactly) {
    static const unsigned int lengths[] = { 1, 2, 128, 129, 254 };
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Rebuild DATA at representative legal record lengths, including both bounds.
    for (size_t test = 0; test < ARRAY_SIZE(lengths); test++) {
        const unsigned int length = lengths[test];
        char filename[32];
        uint8_t* image = diskimage_test_make_d64();
        uint8_t* entry = image + diskimage_test_d64_offset(18, 1) + 32;
        entry[23] = (uint8_t) length;
        snprintf(filename, sizeof(filename), "rel-%u.d64", length);
        mount_image(device, drive, filename, image, DISKIMAGE_D64_SIZE, true);

        // Open and TALK record 1 for the current fixture variant.
        enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(2));
        enqueue_filename_for_drive(drive, "DATA,L");
        enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(2));
        ieee_drive_task();

        // The returned byte count and EOI placement must equal the record length.
        ck_assert_uint_eq(mock_ieee_data_count(), length);
        for (size_t index = 0; index < length; index++) {
            ck_assert_uint_eq(mock_ieee_data_byte(index), 0xa0);
            ck_assert_int_eq(mock_ieee_data_eoi(index), index == length - 1);
        }
        // Discard this variant's response before mounting the next image.
        mock_ieee_clear_data();
    }
}
END_TEST

// Verifies an empty REL record is skipped when serving the next available record.
START_TEST(test_empty_relative_record_is_transparent) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);
    // Zero record 1 of DATA while leaving record 2 intact in the fixture.
    uint8_t* image = diskimage_test_make_d64();
    uint8_t* sector = image + diskimage_test_d64_offset(16, 2);
    memset(sector + 2, 0, 129);
    mount_image(device, drive, "rel.d64", image, DISKIMAGE_D64_SIZE, true);

    // Open and TALK DATA from its initial, now-empty record.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(2));
    enqueue_filename_for_drive(drive, "DATA,L");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(2));
    ieee_drive_task();

    // Serving skips the empty record and returns record 2 across its boundary.
    ck_assert_uint_eq(mock_ieee_data_count(), 129);
    for (size_t index = 0; index < 125; index++) {
        ck_assert_uint_eq(mock_ieee_data_byte(index), 0xa0);
        ck_assert(!mock_ieee_data_eoi(index));
    }
    for (size_t index = 125; index < 129; index++) {
        ck_assert_uint_eq(mock_ieee_data_byte(index), 0xa1);
        ck_assert_int_eq(mock_ieee_data_eoi(index), index == 128);
    }
}
END_TEST

// Verifies REL channels with the same secondary address remain isolated by unit.
START_TEST(test_relative_channels_are_isolated_between_units) {
    const unsigned int source_device = loop_device_only(_i);
    const unsigned int target_device = loop_device_only((_i + 1) % IEEE_DEVICE_COUNT);

    // Open DATA on the same secondary channel in adjacent units.
    mount_d64(source_device, 0, "source.d64", true);
    mount_d64(target_device, 0, "target.d64", true);

    // Position only the source unit at record 2.
    enqueue_command(IEEE_CMD_LISTEN(source_device), IEEE_CMD_OPEN(2));
    enqueue_name("DATA,L");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(target_device), IEEE_CMD_OPEN(2));
    enqueue_name("DATA,L");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(source_device), IEEE_CMD_SECONDARY(15));
    mock_ieee_enqueue_rx(false, 'P');
    mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(2));
    mock_ieee_enqueue_rx(false, 2);
    mock_ieee_enqueue_rx(false, 0);
    // TALK the untouched target unit, which must remain at record 1.
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(target_device), IEEE_CMD_SECONDARY(2));
    ieee_drive_task();

    // Record 1 is entirely 0xA0 and therefore proves no source-unit leakage.
    ck_assert_uint_eq(mock_ieee_data_count(), 129);
    for (size_t index = 0; index < 129; index++) {
        ck_assert_uint_eq(mock_ieee_data_byte(index), 0xa0);
        ck_assert_int_eq(mock_ieee_data_eoi(index), index == 128);
    }
}
END_TEST

// Verifies a REL write beyond its record length reports status 51.
START_TEST(test_relative_write_past_record_reports_51) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Open writable DATA and begin collecting a record write on channel 2.
    mount_d64(device, drive, "rel.d64", true);

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(2));
    enqueue_filename_for_drive(drive, "DATA,L");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(2));
    ieee_drive_task();

    // Send one byte beyond the generated 129-byte record, draining mock RX in batches.
    for (unsigned int index = 0; index < 130; index++) {
        mock_ieee_enqueue_rx(false, (uint8_t) index);
        if (index % 31 == 30) ieee_drive_task();
    }
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(15));
    ieee_drive_task();

    // The excess byte must surface as DOS record-overflow status 51.
    assert_status(51, drive);
}
END_TEST

// Verifies a flat OS-9 .hdd mount exposes its synthetic REL file over IEEE.
START_TEST(test_hdd_mount_position_and_read) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);
    // Create one flat 258-byte HDD record pair with an observable byte pattern.
    uint8_t* image = calloc(1, 258);
    ck_assert_ptr_nonnull(image);
    for (size_t index = 0; index < 258; index++) image[index] = (uint8_t) index;
    mount_image(device, drive, "drive.hdd", image, 258, true);

    // Open the extension's synthetic REL entry and position it at record 2.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(2));
    enqueue_filename_for_drive(drive, "OS9 DRIVE A,L");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(15));
    mock_ieee_enqueue_rx(false, 'P');
    mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(2));
    mock_ieee_enqueue_rx(false, 2);
    mock_ieee_enqueue_rx(false, 0);
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(2));
    ieee_drive_task();

    // Record 2 maps directly to image bytes 129 through 257 with final EOI.
    ck_assert_uint_eq(mock_ieee_data_count(), 129);
    for (size_t index = 0; index < 129; index++) {
        ck_assert_uint_eq(mock_ieee_data_byte(index), (uint8_t) (129 + index));
        ck_assert_int_eq(mock_ieee_data_eoi(index), index == 128);
    }
}
END_TEST

// Verifies D80 container mounting and sequential payload transfer.
START_TEST(test_d80_mount_open_and_stream) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Mount the 8050-style D80 fixture.
    mount_d80(device, drive, "drive.d80", true);

    // Open BASIC and request its data stream through channel 0.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(0));
    enqueue_filename_for_drive(drive, "BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(0));
    ieee_drive_task();

    // Compare the fixture payload and EOI placement byte-for-byte.
    ck_assert_uint_eq(mock_ieee_data_count(), 4);
    for (size_t index = 0; index < 4; index++) {
        ck_assert_uint_eq(mock_ieee_data_byte(index), 0x80 + index);
        ck_assert_int_eq(mock_ieee_data_eoi(index), index == 3);
    }
}
END_TEST

// Host-level IEEE protocol tests. These mount images through ieee_drive, inject
// LISTEN/TALK/OPEN commands through the FPGA-register mock, and assert the
// bus-visible data, status, and EOI response.
//
// (Note that Disk-container semantics belong in diskimage_test.c)
Suite* ieee_drive_suite(void) {
    const int FOR_EACH_DEVICE_AND_DRIVE = IEEE_DEVICE_COUNT * IEEE_DRIVES_PER_DEVICE;
    const int FOR_EACH_DEVICE = IEEE_DEVICE_COUNT;

    Suite* suite = suite_create("ieee_drive");
    TCase* test_case = tcase_create("d64");
    tcase_add_checked_fixture(test_case, setup, teardown);
    tcase_add_loop_test(test_case, test_d64_mount_open_status_and_stream,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_replacing_mount_uses_new_image,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_read_past_eof_returns_eoied_carriage_return,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_power_on_status_is_served_before_any_operation,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_status_read_resets_to_ok,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_missing_file_reports_status_62,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_status_is_isolated_per_unit,
                        0, FOR_EACH_DEVICE);
    tcase_add_loop_test(test_case, test_drive_prefix_selects_second_drive_in_unit,
                        0, FOR_EACH_DEVICE);
    tcase_add_loop_test(test_case, test_close_then_reopen_streams_file,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_test(test_case, test_unmount_clears_open_sequential_channel);
    tcase_add_test(test_case, test_unmount_clears_open_relative_channel);
    tcase_add_test(test_case, test_non_target_addresses_are_ignored);
    tcase_add_test(test_case, test_secondary_address_alias_selects_data_channel);
    tcase_add_loop_test(test_case, test_corrupt_sequential_chain_reports_read_error,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_initialize_command_clears_error_status,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_validate_command_clears_error_status,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_relative_file_position_and_read,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_relative_position_within_record,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_relative_position_past_end_reports_50,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_relative_write_to_read_only_image_reports_26,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_relative_write_updates_existing_record,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_relative_record_lengths_stream_exactly,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_empty_relative_record_is_transparent,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_relative_channels_are_isolated_between_units,
                        0, FOR_EACH_DEVICE);
    tcase_add_loop_test(test_case, test_relative_write_past_record_reports_51,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_hdd_mount_position_and_read,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_d80_mount_open_and_stream,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    suite_add_tcase(suite, test_case);
    return suite;
}
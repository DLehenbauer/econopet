// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

// Host-level contract tests for ieee_drive.c. The tests inject PET IEEE-488
// command/data bytes into the FPGA-register mock and assert the FIFO bytes and
// EOI markers the firmware sends back. Disk images are generated in memory so
// the suite does not depend on the SD-card filesystem.

#include "pch.h"
#include "ieee_drive_test.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "driver.h"
#include "diskimage_test.h"
#include "fatal.h"
#include "ieee/diskimage.h"
#include "ieee/ieee_drive.h"
#include "ieee/ieee_protocol.h"
#include "mock.h"
#include "sd/sd.h"

#define IEEE_FIRST_DEVICE 8
#define IEEE_DEVICE_COUNT 4
#define IEEE_DRIVES_PER_DEVICE 2

enum ieee_drive {
    IEEE_DRIVE_FIRST = 0,
    IEEE_DRIVE_SECOND = 1,
};

enum ieee_channel {
    IEEE_CHANNEL_LOAD = 0,
    IEEE_CHANNEL_REL = 2,
    IEEE_CHANNEL_COMMAND = IEEE_CMD_CHANNEL_MASK,
    IEEE_CHANNEL_LOAD_ALIAS = IEEE_CMD_CHANNEL_MASK + 1,
};

#define TEST_RX_DATA_BATCH_SIZE (MOCK_IEEE_RX_CAPACITY - 1)

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
    enqueue_commands(commands, count_of(commands)); \
} while (0)

// Queues an untagged PETSCII filename or command payload.
static void enqueue_name(const char* name) {
    for (const char* c = name; *c != '\0'; c++) mock_ieee_enqueue_rx(false, (uint8_t) *c);
}

// Asserts the status FIFO holds exactly one expected DOS line with EOI on CR.
static void assert_status(const char* expected) {
    const size_t length = strlen(expected);
    ck_assert_uint_lt(length, IEEE_STATUS_LINE_CAPACITY);
    ck_assert_uint_eq(mock_ieee_status_count(), length);
    for (size_t index = 0; index < length; index++) {
        ck_assert_uint_eq(mock_ieee_status_byte(index), (uint8_t) expected[index]);
        ck_assert_int_eq(mock_ieee_status_eoi(index), index == length - 1);
    }
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
    // Transfer fixture bytes to the registered filesystem before releasing them.
    char path[SD_PATH_MAX];
    sd_make_path(path, SD_DIR_DISKS, filename);
    test_register_binary_file(path, image, image_size, writable);
    free(image);
    // Mount through the public drive API using the corresponding slot.
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

// Returns the external device for a device/drive loop index.
static unsigned int loop_device(int loop_index) {
    ck_assert_int_ge(loop_index, 0);
    ck_assert_int_lt(loop_index, IEEE_DEVICE_COUNT * IEEE_DRIVES_PER_DEVICE);
    return IEEE_FIRST_DEVICE + (unsigned int) loop_index / IEEE_DRIVES_PER_DEVICE;
}

// Returns the local drive for the same loop index passed to loop_device().
static unsigned int loop_drive(int loop_index) {
    ck_assert_int_ge(loop_index, 0);
    ck_assert_int_lt(loop_index, IEEE_DEVICE_COUNT * IEEE_DRIVES_PER_DEVICE);
    return (unsigned int) loop_index % IEEE_DRIVES_PER_DEVICE;
}

// Returns the external device for a device-only loop index.
static unsigned int loop_device_only(int loop_index) {
    ck_assert_int_ge(loop_index, 0);
    ck_assert_int_lt(loop_index, IEEE_DEVICE_COUNT);
    return IEEE_FIRST_DEVICE + (unsigned int) loop_index;
}

// Queues an optional drive-1 prefix followed by a filename for an OPEN command.
static void enqueue_filename_for_drive(unsigned int drive, const char* filename) {
    ck_assert_uint_lt(drive, IEEE_DRIVES_PER_DEVICE);
    if (drive == IEEE_DRIVE_SECOND) {
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
    test_clear_files();
}

// Checks enable readback independently of the data FIFO's burst-room threshold.
START_TEST(test_mock_ctrl_readback) {
    const size_t burst_limit = MOCK_IEEE_TX_CAPACITY - ECONOPET_IEEE_TX_BURST_CHUNK;
    const uint8_t enabled_room =
        ECONOPET_IEEE_CTRL_RD_ENABLE_MASK | ECONOPET_IEEE_CTRL_RD_TX_ROOM_MASK;

    // Enable and disable while the empty FIFO has room for a whole burst.
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR), ECONOPET_IEEE_CTRL_RD_TX_ROOM_MASK);
    spi_write_at(ECONOPET_WB_IEEE_CTRL_ADDR, ECONOPET_IEEE_CTRL_ENABLE_MASK);
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR), enabled_room);
    spi_write_at(ECONOPET_WB_IEEE_CTRL_ADDR, 0);
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR), ECONOPET_IEEE_CTRL_RD_TX_ROOM_MASK);
    spi_write_at(ECONOPET_WB_IEEE_CTRL_ADDR, ECONOPET_IEEE_CTRL_ENABLE_MASK);

    // The last complete burst fits at the threshold, but not one byte beyond it.
    for (size_t index = 0; index < burst_limit; index++) {
        spi_write_at(ECONOPET_WB_IEEE_TX_ADDR, 0);
    }
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR), enabled_room);
    spi_write_at(ECONOPET_WB_IEEE_TX_ADDR, 0);
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR), ECONOPET_IEEE_CTRL_RD_ENABLE_MASK);
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR), ECONOPET_IEEE_CTRL_RD_ENABLE_MASK);

    // Disabling must not manufacture room, and draining must not re-enable.
    spi_write_at(ECONOPET_WB_IEEE_CTRL_ADDR, 0);
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR), 0);
    mock_ieee_clear_data();
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR), ECONOPET_IEEE_CTRL_RD_TX_ROOM_MASK);
}
END_TEST

// Checks both flush commands and ensures their write-only bits never leak into reads.
START_TEST(test_mock_ctrl_flush_readback) {
    const uint8_t flush_commands[] = {
        ECONOPET_IEEE_CTRL_FLUSH_MASK,
        ECONOPET_IEEE_CTRL_DATA_FLUSH_MASK,
    };
    const uint8_t enable_flags[] = { 0, ECONOPET_IEEE_CTRL_ENABLE_MASK };
    const size_t beyond_burst_limit = MOCK_IEEE_TX_CAPACITY - ECONOPET_IEEE_TX_BURST_CHUNK + 1;

    for (size_t flush = 0; flush < count_of(flush_commands); flush++) {
        for (size_t enable = 0; enable < count_of(enable_flags); enable++) {
            const uint8_t enabled = enable_flags[enable] ? ECONOPET_IEEE_CTRL_RD_ENABLE_MASK : 0;
            const bool flush_all = flush_commands[flush] == ECONOPET_IEEE_CTRL_FLUSH_MASK;

            // Populate every FIFO before replacing enable and issuing the flush.
            mock_reset();
            spi_write_at(ECONOPET_WB_IEEE_CTRL_ADDR, ECONOPET_IEEE_CTRL_ENABLE_MASK);
            mock_ieee_enqueue_rx(false, 0);
            spi_write_at(ECONOPET_WB_IEEE_TX_ADDR, 0);
            spi_write_at(ECONOPET_WB_IEEE_TXS_ADDR, 0);
            spi_write_at(ECONOPET_WB_IEEE_CTRL_ADDR, enable_flags[enable] | flush_commands[flush]);
            ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR),
                enabled | ECONOPET_IEEE_CTRL_RD_TX_ROOM_MASK);
            ck_assert_uint_eq(mock_ieee_data_count(), 0);
            ck_assert_uint_eq(mock_ieee_status_count(), flush_all ? 0 : 1);
            ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_STATUS_ADDR) & ECONOPET_IEEE_ST_RX_AVAIL_MASK,
                flush_all ? 0 : ECONOPET_IEEE_ST_RX_AVAIL_MASK);

            // Refilling removes room even when the stored FLUSH bit shares its encoding.
            for (size_t index = 0; index < beyond_burst_limit; index++) {
                spi_write_at(ECONOPET_WB_IEEE_TX_ADDR, 0);
            }
            ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR), enabled);
            ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR), enabled);
        }
    }
}
END_TEST

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
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
    enqueue_filename_for_drive(drive, "BASIC,PRG");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    // Read the completed OPEN status through the unit's command channel.
    enqueue_command(IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();
    assert_status("00, OK,00,00\r");

    // Readdress the file channel and inspect the complete sector-chain stream.
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_LOAD));
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
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
    enqueue_filename_for_drive(drive, "BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_LOAD));
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

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
    enqueue_filename_for_drive(drive, "BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_LOAD));
    ieee_drive_task();
    // The initial stream must contain the complete two-sector fixture.
    ck_assert_uint_eq(mock_ieee_data_count(), 264);

    // Model the controller consuming the first EOF-terminated response.
    mock_ieee_clear_data();

    // Resume the same channel after EOF and inspect the continuation reply.
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_LOAD));
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
    enqueue_command(IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();
    // Newly mounted units report the firmware power-on identification.
    assert_status("73,CBM DOS V2,00,00\r");
}
END_TEST

// Verifies a firmware-initiated reset flushes fabric and DOS state without
// ejecting mounted media.
START_TEST(test_reset_preserves_mount_and_restores_power_on_status) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Simulate media mounted before the PET is reset. The final OPEN verifies
    // that reset clears DOS state without ejecting this image.
    mount_d64(device, drive, "drive.d64", true);

    // Leave the emulated drive in non-power-on state. The missing-file OPEN
    // creates both completed protocol activity and a pending DOS error that
    // must not survive reset.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
    enqueue_name("MISSING");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    // Queue the pending error response so reset must discard both firmware
    // state and bytes already written to the fabric.
    enqueue_command(IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();
    ck_assert_uint_gt(mock_ieee_status_count(), 0);

    // Reset both DOS state and captured fabric responses while retaining media.
    ieee_drive_reset();
    ck_assert_uint_eq(mock_ieee_data_count(), 0);
    ck_assert_uint_eq(mock_ieee_status_count(), 0);

    // Reading channel 15 vets that the pre-reset FILE NOT FOUND status was
    // discarded and replaced by the exact 4040 power-on response.
    enqueue_command(IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();
    assert_status("73,CBM DOS V2,00,00\r");
    mock_ieee_clear_status();

    // Reopen a known file from the original image. Its complete payload and
    // final EOI prove that reset preserved the mount and restored usable
    // protocol state.
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
    enqueue_filename_for_drive(drive, "BASIC,PRG");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_LOAD));
    ieee_drive_task();
    ck_assert_uint_eq(mock_ieee_data_count(), 264);
    ck_assert_uint_eq(mock_ieee_data_byte(0), 2);
    ck_assert(mock_ieee_data_eoi(263));
}
END_TEST

// Verifies I/V use an explicit 0/1 drive and otherwise retain the ROM's
// last-drive default. Mixed image types make the selected formatter visible.
START_TEST(test_initialize_and_validate_commands_select_status_drive) {
    const unsigned int device = loop_device_only(_i);

    // Give each local drive a distinct status formatter.
    mount_d64(device, IEEE_DRIVE_FIRST, "drive0.d64", true);
    mount_d80(device, IEEE_DRIVE_SECOND, "drive1.d80", true);

    // Initialize drive 1 explicitly, then validate using the retained default.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    enqueue_name("I1");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();
    assert_status("00, OK,00,00,1\r");
    mock_ieee_clear_status();

    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    enqueue_name("V");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();
    assert_status("00, OK,00,00,1\r");
    mock_ieee_clear_status();

    // Switch explicitly to drive 0 and check the new retained default.
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    enqueue_name("I0");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();
    assert_status("00, OK,00,00\r");
    mock_ieee_clear_status();

    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    enqueue_name("V");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();
    assert_status("00, OK,00,00\r");
}
END_TEST

// Verifies that reading a status line acknowledges and clears it to status 00.
START_TEST(test_status_read_resets_to_ok) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Read the power-on status once.
    mount_d64(device, drive, "drive.d64", true);

    enqueue_command(IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();
    // The first read observes status 73.
    assert_status("73,CBM DOS V2,00,00\r");

    // Consume the mock response, then readdress channel 15.
    mock_ieee_clear_status();

    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();
    // Status reads acknowledge the line, leaving the next read at status 00.
    assert_status("00, OK,00,00\r");
}
END_TEST

// Verifies a failed filename OPEN is reported through channel 15 as status 62.
START_TEST(test_missing_file_reports_status_62) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Mount a fixture whose directory has no MISSING entry.
    mount_d64(device, drive, "drive.d64", true);

    // Attempt the OPEN, then read its resulting error from channel 15.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
    enqueue_filename_for_drive(drive, "MISSING");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();
    // The failed directory lookup must become DOS error 62 on channel 15.
    assert_status("62,FILE NOT FOUND,00,00\r");
}
END_TEST

// Verifies a dual-D80 unit reports the drive that produced each status.
START_TEST(test_d80_pair_status_reports_selected_drive) {
    const unsigned int device = loop_device_only(_i);

    // Simulate a complete dual-drive 8050 unit so both drive numbers share the
    // same DOS 2.7 model and status channel.
    mount_d80(device, IEEE_DRIVE_FIRST, "zero.d80", true);
    mount_d80(device, IEEE_DRIVE_SECOND, "one.d80", true);

    for (unsigned int drive = 0; drive < IEEE_DRIVES_PER_DEVICE; drive++) {
        // Select each drive with its filename prefix and deliberately fail an
        // OPEN. This associates the resulting FILE NOT FOUND status with the
        // drive that performed the directory lookup.
        enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
        enqueue_filename_for_drive(drive, "MISSING");
        enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device),
                        IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
        ieee_drive_task();

        // Vet the exact DOS 2.7 status shape, including the originating drive
        // number in the fifth field.
        char expected[IEEE_STATUS_LINE_CAPACITY];
        snprintf(expected, sizeof(expected), "62,FILE NOT FOUND,00,00,%u\r", drive);
        assert_status(expected);

        // Consume the response and leave TALK mode so the other drive begins
        // from an independent protocol exchange.
        mock_ieee_clear_status();
        enqueue_command(IEEE_CMD_UNTALK);
    }
}
END_TEST

// Verifies a mixed unit formats status according to the originating drive.
START_TEST(test_mixed_models_follow_selected_drive) {
    const unsigned int device = loop_device_only(_i / IEEE_DRIVES_PER_DEVICE);
    const unsigned int d80_drive = (unsigned int) _i % IEEE_DRIVES_PER_DEVICE;
    const unsigned int d64_drive = d80_drive ^ IEEE_DRIVE_SECOND;

    // Simulate both mixed-drive orders. Each loop case swaps which local drive
    // contains D80 media, ensuring model selection is not tied to one slot.
    mount_d80(device, d80_drive, "drive.d80", true);
    mount_d64(device, d64_drive, "drive.d64", true);

    // Read status before selecting a drive. This vets the hybrid unit's
    // deterministic power-on rule: drive 0 chooses the initial DOS model.
    enqueue_command(IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();
    assert_status(d80_drive == IEEE_DRIVE_FIRST
        ? "73,CBM DOS V2.7,00,00,0\r"
        : "73,CBM DOS V2,00,00\r");
    mock_ieee_clear_status();

    for (unsigned int drive = 0; drive < IEEE_DRIVES_PER_DEVICE; drive++) {
        // Address each local drive and fail an OPEN. The pending status now has
        // an unambiguous originating drive whose mounted media selects the
        // emulated DOS model.
        enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
        enqueue_filename_for_drive(drive, "MISSING");
        enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device),
                        IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
        ieee_drive_task();

        if (drive == d80_drive) {
            // The D80 side must use DOS 2.7's five-field status and report its
            // local drive number.
            char expected[IEEE_STATUS_LINE_CAPACITY];
            snprintf(expected, sizeof(expected), "62,FILE NOT FOUND,00,00,%u\r",
                     drive);
            assert_status(expected);
        } else {
            // The D64 side must use the 4040 four-field status even though its
            // sibling drive in the same unit contains D80 media.
            assert_status("62,FILE NOT FOUND,00,00\r");
        }

        // Discard this response so the next drive's assertion cannot pass on
        // stale channel-15 bytes.
        mock_ieee_clear_status();
    }
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
    mount_d64(source_device, IEEE_DRIVE_FIRST, "source.d64", true);
    mount_d64(target_device, IEEE_DRIVE_FIRST, "target.d64", true);

    // Fail an OPEN on one device, but ask its neighbor for status.
    enqueue_command(IEEE_CMD_LISTEN(source_device), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
    enqueue_name("MISSING");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(target_device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();
    // The untouched neighbor retains its independent power-on status.
    assert_status("73,CBM DOS V2,00,00\r");
}
END_TEST

// Verifies a drive prefix routes a filename to drive 1 within every device.
START_TEST(test_drive_prefix_selects_second_drive_in_unit) {
    const unsigned int device = loop_device_only(_i);

    // Build two disks and give drive 1 a payload byte unique to this test.
    uint8_t* drive_zero = diskimage_test_make_d64();
    uint8_t* drive_one = diskimage_test_make_d64();
    drive_one[diskimage_test_d64_offset(17, 0) + 2] = 0xa5;
    mount_image(device, IEEE_DRIVE_FIRST, "zero.d64", drive_zero, DISKIMAGE_D64_SIZE, true);
    mount_image(device, IEEE_DRIVE_SECOND, "one.d64", drive_one, DISKIMAGE_D64_SIZE, true);

    // Use the 1: filename prefix and confirm the stream came from drive 1.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
    enqueue_name("1:BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_LOAD));
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

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
    enqueue_filename_for_drive(drive, "BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    // Close the active channel and attempt to resume its discarded stream.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_CLOSE(IEEE_CHANNEL_LOAD), IEEE_CMD_UNLISTEN,
                    IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_LOAD));
    ieee_drive_task();
    // TALK after CLOSE cannot resume the discarded stream.
    ck_assert_uint_eq(mock_ieee_data_count(), 0);

    // Reopen the same file channel from its original position.
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
    enqueue_filename_for_drive(drive, "BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_LOAD));
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
    mount_d64(IEEE_FIRST_DEVICE, IEEE_DRIVE_FIRST, "first.d64", true);
    enqueue_command(IEEE_CMD_LISTEN(IEEE_FIRST_DEVICE), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
    enqueue_name("BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(IEEE_FIRST_DEVICE),
                    IEEE_CMD_SECONDARY(IEEE_CHANNEL_LOAD));
    ieee_drive_task();
    ck_assert_uint_eq(mock_ieee_data_count(), 264);

    // Consume the old response, unmount its media, and mount a fresh image.
    mock_ieee_clear_data();
    ieee_drive_unmount_all();
    mount_d64(IEEE_FIRST_DEVICE, IEEE_DRIVE_FIRST, "second.d64", true);

    // Remounting media must not resurrect a channel opened on the old image.
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_TALK(IEEE_FIRST_DEVICE),
                    IEEE_CMD_SECONDARY(IEEE_CHANNEL_LOAD));
    ieee_drive_task();
    ck_assert_uint_eq(mock_ieee_data_count(), 0);
}
END_TEST

// Verifies unmount discards REL channels that reference the removed image.
START_TEST(test_unmount_clears_open_relative_channel) {
    // Open a REL channel backed by the original image.
    mount_d64(IEEE_FIRST_DEVICE, IEEE_DRIVE_FIRST, "first.d64", true);
    enqueue_command(IEEE_CMD_LISTEN(IEEE_FIRST_DEVICE), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_name("DATA,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    // Replace the mounted image without reopening the old channel.
    ieee_drive_unmount_all();
    mount_d64(IEEE_FIRST_DEVICE, IEEE_DRIVE_FIRST, "second.d64", true);

    // TALK alone cannot reuse the REL chain cached for the previous image.
    enqueue_command(IEEE_CMD_TALK(IEEE_FIRST_DEVICE), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    ieee_drive_task();
    ck_assert_uint_eq(mock_ieee_data_count(), 0);
}
END_TEST

// Verifies commands directed at unimplemented primary addresses are ignored.
START_TEST(test_non_target_addresses_are_ignored) {
    static const unsigned int non_target_devices[] = {
        0, IEEE_FIRST_DEVICE - 1, IEEE_FIRST_DEVICE + IEEE_DEVICE_COUNT,
        IEEE_CMD_ADDRESS_MASK,
    };

    mount_d64(IEEE_FIRST_DEVICE, IEEE_DRIVE_FIRST, "drive.d64", true);

    // Exercise addresses outside the implemented range, including address 31.
    for (size_t index = 0; index < count_of(non_target_devices); index++) {
        const unsigned int device = non_target_devices[index];
        enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
        enqueue_name("BASIC");
        enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_LOAD));
        ieee_drive_task();
    }

    // No unowned address may create data or command-channel output.
    ck_assert_uint_eq(mock_ieee_data_count(), 0);
    ck_assert_uint_eq(mock_ieee_status_count(), 0);
}
END_TEST

// Verifies secondary addresses with the same low nibble select the same channel.
START_TEST(test_secondary_address_alias_selects_data_channel) {
    mount_d64(IEEE_FIRST_DEVICE, IEEE_DRIVE_FIRST, "drive.d64", true);

    // Address BASIC on channel 0, then TALK with secondary address 16.
    enqueue_command(IEEE_CMD_LISTEN(IEEE_FIRST_DEVICE), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
    enqueue_name("BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(IEEE_FIRST_DEVICE),
                    IEEE_CMD_SECONDARY(IEEE_CHANNEL_LOAD_ALIAS));
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

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
    enqueue_filename_for_drive(drive, "MISSING");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));

    // Send I to the command channel, then read the cleared status.
    mock_ieee_enqueue_rx(false, 'I');
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();
    // I clears the pending file-not-found error to DOS status 00.
    assert_status("00, OK,00,00\r");
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

    // Request the corrupted file through its normal sequential channel.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
    enqueue_filename_for_drive(drive, "BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_LOAD));
    ieee_drive_task();

    // Valid prefix bytes are sent, followed by an EOI'd carriage-return fallback.
    ck_assert_uint_eq(mock_ieee_data_count(), 255);
    for (size_t index = 0; index < 254; index++) {
        ck_assert_uint_eq(mock_ieee_data_byte(index), index + 2);
        ck_assert(!mock_ieee_data_eoi(index));
    }
    ck_assert_uint_eq(mock_ieee_data_byte(254), '\r');
    ck_assert(mock_ieee_data_eoi(254));

    // Read command-channel status after the partial data response.
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();
    // The incomplete chain records DOS read error 23 on channel 15.
    assert_status("23,READ ERROR,00,00\r");
}
END_TEST

// Verifies a channel-15 V command clears a previously generated error status.
START_TEST(test_validate_command_clears_error_status) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Generate an error by opening a missing file on mounted media.
    mount_d64(device, drive, "drive.d64", true);

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
    enqueue_filename_for_drive(drive, "MISSING");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));

    // Validate the drive and read the resulting cleared status.
    mock_ieee_enqueue_rx(false, 'V');
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();
    // V clears the pending file-not-found error to DOS status 00.
    assert_status("00, OK,00,00\r");
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

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "DATA,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    // Position channel 2 at record 2: P, secondary channel, low, high.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    mock_ieee_enqueue_rx(false, 'P');
    mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    mock_ieee_enqueue_rx(false, 2);
    mock_ieee_enqueue_rx(false, 0);
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
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

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "DATA,L");
    // Position record 2 at its sector-boundary suffix.
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    mock_ieee_enqueue_rx(false, 'P');
    mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    mock_ieee_enqueue_rx(false, 2);
    mock_ieee_enqueue_rx(false, 0);
    // P selects record 2 at one-based offset 125, one byte before its boundary.
    mock_ieee_enqueue_rx(false, 125);
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
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

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "DATA,L");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));

    // Select record 7, beyond the generated chain's usable records.
    mock_ieee_enqueue_rx(false, 'P');
    mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    mock_ieee_enqueue_rx(false, 7);
    mock_ieee_enqueue_rx(false, 0);
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();

    // The command channel exposes the missing-record DOS status.
    assert_status("50,RECORD NOT PRESENT,00,00\r");
}
END_TEST

// Verifies REL writes surface the correct status when the mounted image is read-only.
START_TEST(test_relative_write_to_read_only_image_reports_26) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Mount the REL fixture as read-only and open DATA on channel 2.
    mount_d64(device, drive, "rel.d64", false);

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "DATA,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    // Write one byte and UNLISTEN, which commits the record write attempt.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    mock_ieee_enqueue_rx(false, 'Z');
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();

    // A failed commit must report write protection rather than status 00.
    assert_status("26,WRITE PROTECT ON,00,00\r");
}
END_TEST

// Verifies a writable REL mount updates an existing record in place.
START_TEST(test_relative_write_updates_existing_record) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Mount writable media and open DATA on its REL channel.
    mount_d64(device, drive, "rel.d64", true);

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "DATA,L");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    // LISTEN two replacement bytes and commit them at UNLISTEN.
    mock_ieee_enqueue_rx(false, 'O');
    mock_ieee_enqueue_rx(false, 'K');
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    // Reposition to record 1, then TALK the modified channel back.
    mock_ieee_enqueue_rx(false, 'P');
    mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    mock_ieee_enqueue_rx(false, 1);
    mock_ieee_enqueue_rx(false, 0);
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
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
    static const unsigned int lengths[] = {
        DISKIMAGE_REL_MIN_RECORD_LENGTH, 2, 128, 129, DISKIMAGE_REL_MAX_RECORD_LENGTH,
    };
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Rebuild DATA at representative legal record lengths, including both bounds.
    for (size_t test = 0; test < count_of(lengths); test++) {
        const unsigned int length = lengths[test];
        char filename[SD_PATH_MAX];
        uint8_t* image = diskimage_test_make_d64();
        uint8_t* entry = image + diskimage_test_d64_offset(18, 1) + DISKIMAGE_DIRECTORY_ENTRY_SIZE;
        entry[DISKIMAGE_DIRECTORY_REL_LENGTH_OFFSET] = (uint8_t) length;
        snprintf(filename, sizeof(filename), "rel-%u.d64", length);
        mount_image(device, drive, filename, image, DISKIMAGE_D64_SIZE, true);

        // Open and TALK record 1 for the current fixture variant.
        enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
        enqueue_filename_for_drive(drive, "DATA,L");
        enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
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
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "DATA,L");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
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
    mount_d64(source_device, IEEE_DRIVE_FIRST, "source.d64", true);
    mount_d64(target_device, IEEE_DRIVE_FIRST, "target.d64", true);

    // Position only the source unit at record 2.
    enqueue_command(IEEE_CMD_LISTEN(source_device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_name("DATA,L");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(target_device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_name("DATA,L");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(source_device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    mock_ieee_enqueue_rx(false, 'P');
    mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    mock_ieee_enqueue_rx(false, 2);
    mock_ieee_enqueue_rx(false, 0);
    // TALK the untouched target unit, which must remain at record 1.
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(target_device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
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

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "DATA,L");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    ieee_drive_task();

    // Send one byte beyond the generated 129-byte record, draining mock RX in batches.
    for (unsigned int index = 0; index < 130; index++) {
        mock_ieee_enqueue_rx(false, (uint8_t) index);
        if (index % TEST_RX_DATA_BATCH_SIZE == TEST_RX_DATA_BATCH_SIZE - 1) ieee_drive_task();
    }
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();

    // The excess byte must surface as DOS record-overflow status 51.
    assert_status("51,OVERFLOW IN RECORD,00,00\r");
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
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "OS9 DRIVE A,L");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    mock_ieee_enqueue_rx(false, 'P');
    mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    mock_ieee_enqueue_rx(false, 2);
    mock_ieee_enqueue_rx(false, 0);
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
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

    // The 8050 DOS ROM reports its own power-on banner and drive field.
    enqueue_command(IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();
    assert_status("73,CBM DOS V2.7,00,00,0\r");
    mock_ieee_clear_status();

    // Open BASIC and request its data stream through channel 0.
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
    enqueue_filename_for_drive(drive, "BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_LOAD));
    ieee_drive_task();

    // Compare the fixture payload and EOI placement byte-for-byte.
    ck_assert_uint_eq(mock_ieee_data_count(), 4);
    for (size_t index = 0; index < 4; index++) {
        ck_assert_uint_eq(mock_ieee_data_byte(index), 0x80 + index);
        ck_assert_int_eq(mock_ieee_data_eoi(index), index == 3);
    }
}
END_TEST

// Checks shared format facts independently of the code that consumes them.
START_TEST(test_shared_format_constants) {
    ck_assert_uint_eq(DISKIMAGE_SECTOR_SIZE, 256);
    ck_assert_uint_eq(DISKIMAGE_DIRECTORY_ENTRIES_PER_SECTOR, 8);
    ck_assert_uint_eq(DISKIMAGE_REL_MIN_RECORD_LENGTH, 1);
    ck_assert_uint_eq(DISKIMAGE_REL_MAX_RECORD_LENGTH, 254);
    ck_assert_uint_eq(DISKIMAGE_DIRECTORY_ENTRY_SIZE, 32);
    ck_assert_uint_eq(DISKIMAGE_DIRECTORY_REL_LENGTH_OFFSET, 23);
    ck_assert_uint_eq(IEEE_STATUS_TEXT_MAX, 36);
    ck_assert_uint_eq(IEEE_STATUS_LINE_CAPACITY, 38);
    const char longest_status[] = "66,ILLEGAL TRACK OR SECTOR,255,255,1\r";
    ck_assert_uint_eq(sizeof(longest_status), IEEE_STATUS_LINE_CAPACITY);
}
END_TEST

// Mounts the longest full SD path without truncating its filename.
START_TEST(test_mount_path_boundary) {
    char filename[SD_PATH_MAX];
    const size_t length = SD_PATH_MAX - 1 - strlen(sd_dir_prefix(SD_DIR_DISKS));
    memset(filename, 'a', length);
    filename[length] = '\0';
    mount_d64(IEEE_FIRST_DEVICE, IEEE_DRIVE_FIRST, filename, true);

}
END_TEST

// Verifies an overlong mount is fatal instead of reopening a truncated filename.
START_TEST(test_mount_rejects_overlong_path) {
    char filename[SD_PATH_MAX];
    const size_t length = SD_PATH_MAX - 1 - strlen(sd_dir_prefix(SD_DIR_DISKS));
    memset(filename, 'a', length);
    filename[length] = '\0';
    mount_d64(IEEE_FIRST_DEVICE, IEEE_DRIVE_FIRST, filename, true);

    filename[length] = 'b';
    filename[length + 1] = '\0';
    test_expect_fatal_message("SD path exceeds 255 characters (got 256)");
    ieee_drive_mount(device_slot(IEEE_FIRST_DEVICE, IEEE_DRIVE_FIRST), filename);
}
END_TEST

// Builds host protocol tests for bus-visible data, status, and EOI responses.
Suite* ieee_drive_suite(void) {
    const int FOR_EACH_DEVICE_AND_DRIVE = IEEE_DEVICE_COUNT * IEEE_DRIVES_PER_DEVICE;
    const int FOR_EACH_DEVICE = IEEE_DEVICE_COUNT;
    const int FOR_EACH_DEVICE_AND_MODEL_ORDER =
        IEEE_DEVICE_COUNT * IEEE_DRIVES_PER_DEVICE;

    // Share isolated mount/register fixtures across all protocol cases.
    Suite* suite = suite_create("ieee_drive");
    TCase* test_case = tcase_create("d64");
    tcase_add_checked_fixture(test_case, setup, teardown);
    tcase_add_test(test_case, test_mock_ctrl_readback);
    tcase_add_test(test_case, test_mock_ctrl_flush_readback);
    tcase_add_test(test_case, test_shared_format_constants);
    tcase_add_test(test_case, test_mount_path_boundary);
    // Cover sequential transfers, status, addressing, and unit isolation.
    tcase_add_loop_test(test_case, test_d64_mount_open_status_and_stream,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_replacing_mount_uses_new_image,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_read_past_eof_returns_eoied_carriage_return,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_power_on_status_is_served_before_any_operation,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_reset_preserves_mount_and_restores_power_on_status,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_status_read_resets_to_ok,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_missing_file_reports_status_62,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_d80_pair_status_reports_selected_drive,
                        0, FOR_EACH_DEVICE);
    tcase_add_loop_test(test_case, test_mixed_models_follow_selected_drive,
                        0, FOR_EACH_DEVICE_AND_MODEL_ORDER);
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
    tcase_add_loop_test(test_case, test_initialize_and_validate_commands_select_status_drive,
                        0, IEEE_DEVICE_COUNT);
    // Cover REL records, mutable/read-only media, and the HDD extension.
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

// Builds forked mount tests whose invalid paths must abort.
Suite* ieee_drive_fatal_suite(void) {
    Suite* suite = suite_create("ieee_drive-fatal");
    TCase* test_case = tcase_create("paths");
    tcase_add_checked_fixture(test_case, setup, teardown);
    tcase_add_test_raise_signal(test_case, test_mount_rejects_overlong_path, SIGABRT);
    suite_add_tcase(suite, test_case);
    return suite;
}
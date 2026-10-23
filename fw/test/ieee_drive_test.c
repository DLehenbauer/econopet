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

#include "diag/log/log.h"
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
    IEEE_CHANNEL_DATA = 2,
    IEEE_CHANNEL_REL = IEEE_CHANNEL_DATA,
    IEEE_CHANNEL_COMMAND = IEEE_CMD_CHANNEL_MASK,
    IEEE_CHANNEL_LOAD_ALIAS = IEEE_CMD_CHANNEL_MASK + 1,
};

#define TEST_RX_DATA_BATCH_SIZE (MOCK_IEEE_RX_CAPACITY - 1)
#define TEST_REL_RECORD_COUNT 3u
#define TEST_REL_WRAPPED_RECORD_COUNT 5u
#define TEST_COMMAND_RECORD_COUNT 45u
#define TEST_COMMAND_RECORD_LENGTH 2u
static const uint8_t relative_eoi_lengths[] = {2, 129, DISKIMAGE_REL_MAX_RECORD_LENGTH};
enum close_write_pattern {
    close_no_writes,
    close_lower_written,
    close_upper_written,
    close_both_written,
    close_lower_reopened,
    close_lower_rearmed,
    close_write_pattern_count,
};

// ---------------------------------------------------------------------------
// IEEE command-stream helpers
// ---------------------------------------------------------------------------

// Queue command phases, releasing ATN after UNLISTEN/UNTALK as KERNAL does.
static void enqueue_commands(const uint8_t* commands, size_t count) {
    for (size_t index = 0; index < count; index++) {
        mock_ieee_enqueue_rx(true, commands[index]);
        if (commands[index] == IEEE_CMD_UNLISTEN || commands[index] == IEEE_CMD_UNTALK)
            mock_ieee_enqueue_atn_end();
    }
    if (count != 0 && commands[count - 1] != IEEE_CMD_UNLISTEN
        && commands[count - 1] != IEEE_CMD_UNTALK) mock_ieee_enqueue_atn_end();
}

// Enqueues one or more ATN-tagged command bytes. Listener data (filenames,
// channel-15 command payloads, and file writes) is enqueued separately.
#define enqueue_command(...) do { \
    const uint8_t commands[] = { __VA_ARGS__ }; \
    enqueue_commands(commands, count_of(commands)); \
} while (0)

// Queues a PETSCII filename or command with EOI only on its final byte.
static void enqueue_name(const char* name) {
    for (const char* c = name; *c != '\0'; c++)
        mock_ieee_enqueue_rx_eoi(false, (uint8_t) *c, c[1] == '\0');
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

// Initializes logging and the mock/drive state without relying on earlier suites.
static void setup(void) {
    mock_reset();
    log_init();
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

// RX-head EOI is retained until pop and never inferred from a following UNLISTEN.
START_TEST(test_mock_rx_eoi_readback) {
    const uint8_t room = ECONOPET_IEEE_CTRL_RD_TX_ROOM_MASK;
    mock_ieee_enqueue_rx_eoi(false, 0, true);
    mock_ieee_enqueue_rx(true, IEEE_CMD_UNLISTEN);
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR),
        room | ECONOPET_IEEE_CTRL_RD_RX_EOI_MASK);
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_RX_ADDR), 0);
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR),
        room | ECONOPET_IEEE_CTRL_RD_RX_EOI_MASK);
    spi_write_at(ECONOPET_WB_IEEE_RX_ADDR, 0);
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR), room);
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_RX_ADDR), IEEE_CMD_UNLISTEN);
    spi_write_at(ECONOPET_WB_IEEE_RX_ADDR, 0);
    ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_CTRL_ADDR), room);
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

static const diskimage_type_t native_image_types[] = {
    diskimage_type_d64, diskimage_type_d80
};
static const uint8_t native_file_types[] = {
    DISKIMAGE_FTYPE_SEQ, DISKIMAGE_FTYPE_PRG, DISKIMAGE_FTYPE_USR
};

// Native OPEN compares raw filename bytes before decoding names for display.
START_TEST(test_native_open_matches_raw_filename_bytes) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const bool d80 = type == diskimage_type_d80;
    static const struct {
        const char* name;
        size_t length;
        const char* pattern;
        bool matches;
    } cases[] = {
        {"BaSIC", 5, "BAS?C", false},
        {"BaSIC", 5, "B?SIC", true},
        {"A\0B", 3, "A?B", true},
        {"A\0B", 3, "A", false},
        {"ABCDEFGHIJKLMNOP", 16, "?????????????????", true},
        {"ABCDEFGHIJKLMNOP", 16, "ABCDEFGHIJKLMNOPX", true},
        {"ABCDEFGHIJKLMNOP", 16, "ABCDEFGHIJKLMNOX?", false},
        {"ABCDEFGHIJKLMNO", 15, "?????????????????", false},
    };
    const uint8_t payload[] = {0xa5, 0x5a};
    const size_t filename_offset = 5, filename_capacity = 16;
    for (size_t variant = 0; variant < count_of(cases); ++variant) {
        // Store names with counted bytes so an embedded zero survives the fixture.
        uint8_t* const image = d80 ? diskimage_test_make_d80() : diskimage_test_make_d64();
        uint32_t directory, data;
        ck_assert(diskimage_sector_offset(type, d80 ? 39 : 18, 1, &directory));
        ck_assert(diskimage_sector_offset(type, d80 ? 38 : 17, 0, &data));
        memset(image + directory + filename_offset, 0xa0, filename_capacity);
        memcpy(image + directory + filename_offset, cases[variant].name, cases[variant].length);
        image[data] = 0;
        image[data + 1] = DISKIMAGE_SECTOR_LINK_SIZE + sizeof(payload) - 1;
        memcpy(image + data + DISKIMAGE_SECTOR_LINK_SIZE, payload, sizeof(payload));
        char filename[32];
        snprintf(filename, sizeof(filename), "raw-name-%zu.%s", variant, d80 ? "d80" : "d64");
        mount_image(device, drive, filename, image,
            d80 ? DISKIMAGE_D80_SIZE : DISKIMAGE_D64_SIZE, true);

        // Exercise OPEN and TALK through firmware, not just the image-level matcher.
        enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_DATA));
        enqueue_filename_for_drive(drive, cases[variant].pattern);
        enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device),
            IEEE_CMD_SECONDARY(IEEE_CHANNEL_DATA));
        ieee_drive_task();
        ck_assert_uint_eq(mock_ieee_data_count(), cases[variant].matches ? sizeof(payload) : 0);
        if (cases[variant].matches) {
            for (size_t index = 0; index < sizeof(payload); ++index) {
                ck_assert_uint_eq(mock_ieee_data_byte(index), payload[index]);
                ck_assert_int_eq(mock_ieee_data_eoi(index), index + 1 == sizeof(payload));
            }
        }
        mock_ieee_clear_data();

        // A literal mismatch must report the selected model's exact not-found status.
        enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_TALK(device),
            IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
        ieee_drive_task();
        char expected[IEEE_STATUS_LINE_CAPACITY];
        snprintf(expected, sizeof(expected), "%s,00,00%s\r",
            cases[variant].matches ? "00, OK" : "62, FILE NOT FOUND",
            d80 ? (drive == IEEE_DRIVE_FIRST ? ",0" : ",1") : "");
        assert_status(expected);
        mock_ieee_clear_status();
    }
}
END_TEST

// Empty-drive fallback remains available for ordinary native images.
START_TEST(test_empty_drive_falls_back_to_native_peer) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const bool d80 = native_image_types[_i % count_of(native_image_types)] == diskimage_type_d80;
    if (d80) {
        mount_d80(device, drive ^ 1u, "peer.d80", true);
    } else {
        mount_d64(device, drive ^ 1u, "peer.d64", true);
    }

    // Explicitly request the empty drive and verify the native peer's complete file.
    char filename[32];
    snprintf(filename, sizeof(filename), "%u:B?SIC", drive);
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
    enqueue_name(filename);
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device),
        IEEE_CMD_SECONDARY(IEEE_CHANNEL_LOAD));
    ieee_drive_task();
    const size_t length = d80 ? 4 : 264;
    ck_assert_uint_eq(mock_ieee_data_count(), length);
    for (size_t index = 0; index < length; ++index) {
        const uint8_t expected = d80 ? (uint8_t) (0x80 + index)
            : index < DISKIMAGE_SECTOR_PAYLOAD_SIZE ? (uint8_t) (index + 2)
            : (uint8_t) (0xe2 + index - DISKIMAGE_SECTOR_PAYLOAD_SIZE);
        ck_assert_uint_eq(mock_ieee_data_byte(index), expected);
        ck_assert_int_eq(mock_ieee_data_eoi(index), index + 1 == length);
    }
}
END_TEST

// Replaces BASIC's first sector with a short ordinary file for each native model.
static void mount_short_native_file(size_t case_index, const uint8_t* payload, size_t length) {
    ck_assert_uint_lt(case_index, count_of(native_image_types) * count_of(native_file_types));
    ck_assert_uint_ge(length, 1);
    ck_assert_uint_le(length, DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    const diskimage_type_t type = native_image_types[case_index / count_of(native_file_types)];
    const bool d80 = type == diskimage_type_d80;
    uint8_t* const image = d80 ? diskimage_test_make_d80() : diskimage_test_make_d64();
    uint32_t directory_offset;
    uint32_t data_offset;
    ck_assert(diskimage_sector_offset(type, d80 ? 39 : 18, 1, &directory_offset));
    ck_assert(diskimage_sector_offset(type, d80 ? 38 : 17, 0, &data_offset));

    // Retain the closed-file flag, but vary SEQ/PRG/USR independently of channel 2.
    const uint8_t closed_file_flag = 0x80;
    image[directory_offset + 2] =
        closed_file_flag | native_file_types[case_index % count_of(native_file_types)];
    image[data_offset] = 0;
    image[data_offset + 1] = (uint8_t) (DISKIMAGE_SECTOR_LINK_SIZE + length - 1);
    memcpy(image + data_offset + DISKIMAGE_SECTOR_LINK_SIZE, payload, length);
    mount_image(IEEE_FIRST_DEVICE, IEEE_DRIVE_FIRST, d80 ? "short.d80" : "short.d64",
        image, d80 ? DISKIMAGE_D80_SIZE : DISKIMAGE_D64_SIZE, true);
}

// Preserves OR30's lost initial EOF, including buffer-header leakage and later EOF.
START_TEST(test_native_one_byte_file_reaches_eof_after_four_bytes) {
    const uint8_t payload[] = {0x41};
    const uint8_t expected[] = {0x41, 0x00, 0x02, 0x41};
    const uint8_t data_channel = IEEE_CHANNEL_DATA;
    mount_short_native_file(_i, payload, sizeof(payload));

    // Every OPEN must repeat the native behavior, even after a completed stream.
    for (size_t attempt = 0; attempt < 2; ++attempt) {
        enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(IEEE_FIRST_DEVICE),
            IEEE_CMD_OPEN(data_channel));
        enqueue_name("BASIC");
        enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(IEEE_FIRST_DEVICE),
            IEEE_CMD_SECONDARY(data_channel));
        ieee_drive_task();

        // Compare literal native bytes and EOI rather than the image reader's EOF.
        ck_assert_uint_eq(mock_ieee_data_count(), sizeof(expected));
        for (size_t i = 0; i < sizeof(expected); ++i) {
            ck_assert_uint_eq(mock_ieee_data_byte(i), expected[i]);
            ck_assert_int_eq(mock_ieee_data_eoi(i), i + 1 == sizeof(expected));
        }
        ieee_drive_task();
        ck_assert_uint_eq(mock_ieee_data_count(), sizeof(expected));
    }
}
END_TEST

// Verifies two stored bytes avoid the initial-EOF overwrite for every native type.
START_TEST(test_native_two_byte_file_keeps_normal_eof) {
    const uint8_t payload[] = {0x41, 0x42};
    const uint8_t data_channel = IEEE_CHANNEL_DATA;
    mount_short_native_file(_i, payload, sizeof(payload));

    // OPEN an ordinary read channel, then let production firmware fill its FIFO.
    enqueue_command(IEEE_CMD_LISTEN(IEEE_FIRST_DEVICE), IEEE_CMD_OPEN(data_channel));
    enqueue_name("BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(IEEE_FIRST_DEVICE),
        IEEE_CMD_SECONDARY(data_channel));
    ieee_drive_task();

    // The second byte ends the file with neither header leakage nor duplication.
    ck_assert_uint_eq(mock_ieee_data_count(), sizeof(payload));
    for (size_t i = 0; i < sizeof(payload); ++i) {
        ck_assert_uint_eq(mock_ieee_data_byte(i), payload[i]);
        ck_assert_int_eq(mock_ieee_data_eoi(i), i + 1 == sizeof(payload));
    }
}
END_TEST

// Keeps the native initial-EOF bug independent of REL reads on any device.
START_TEST(test_native_one_byte_eof_survives_interleaved_relative_read) {
    const unsigned int native_case_count =
        count_of(native_image_types) * count_of(native_file_types);
    const unsigned int rel_device = loop_device_only(_i / native_case_count);
    const uint8_t ordinary_channel = IEEE_CHANNEL_DATA + 1;
    const uint8_t payload[] = {0x41};
    const uint8_t expected[] = {0x41, 0x00, 0x02, 0x41};
    const size_t record_length = 129;

    // Give the ordinary stream and REL record separate files and channels.
    mount_short_native_file(_i % native_case_count, payload, sizeof(payload));
    mount_d64(rel_device, IEEE_DRIVE_SECOND, "relative.d64", true);
    enqueue_command(IEEE_CMD_LISTEN(rel_device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(IEEE_DRIVE_SECOND, "DATA");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    // Leave the ordinary file open but unread while servicing the REL channel.
    enqueue_command(IEEE_CMD_LISTEN(IEEE_FIRST_DEVICE), IEEE_CMD_OPEN(ordinary_channel));
    enqueue_name("BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    enqueue_command(IEEE_CMD_TALK(rel_device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    ieee_drive_task();

    // Verify a real record was served, then model the controller draining it.
    ck_assert_uint_eq(mock_ieee_data_count(), record_length);
    for (size_t i = 0; i < record_length; ++i) {
        ck_assert_uint_eq(mock_ieee_data_byte(i), 0xa0);
        ck_assert_int_eq(mock_ieee_data_eoi(i), i + 1 == record_length);
    }
    mock_ieee_clear_data();

    // The unrelated read must not suppress the ordinary file's four-byte EOF.
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_TALK(IEEE_FIRST_DEVICE),
        IEEE_CMD_SECONDARY(ordinary_channel));
    ieee_drive_task();
    ck_assert_uint_eq(mock_ieee_data_count(), sizeof(expected));
    for (size_t i = 0; i < sizeof(expected); ++i) {
        ck_assert_uint_eq(mock_ieee_data_byte(i), expected[i]);
        ck_assert_int_eq(mock_ieee_data_eoi(i), i + 1 == sizeof(expected));
    }
    ieee_drive_task();
    ck_assert_uint_eq(mock_ieee_data_count(), sizeof(expected));
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

// Verifies native ordinary channels remain not ready after their final EOI.
START_TEST(test_read_past_eof_keeps_channel_not_ready) {
    const unsigned int device = loop_device(_i / count_of(native_image_types));
    const unsigned int drive = loop_drive(_i / count_of(native_image_types));
    const bool d80 = native_image_types[_i % count_of(native_image_types)]
        == diskimage_type_d80;

    // Mount the source image and issue the initial complete file read.
    if (d80) mount_d80(device, drive, "drive.d80", true);
    else mount_d64(device, drive, "drive.d64", true);

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
    enqueue_filename_for_drive(drive, "BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_LOAD));
    ieee_drive_task();
    // The initial stream ends with the fixture's real final byte and EOI.
    const size_t length = d80 ? 4 : 264;
    ck_assert_uint_eq(mock_ieee_data_count(), length);
    ck_assert(mock_ieee_data_eoi(length - 1));

    // Model the controller consuming the first EOF-terminated response.
    mock_ieee_clear_data();

    // Resume the same channel after EOF and inspect the continuation reply.
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_LOAD));
    ieee_drive_task();
    // A post-EOF TALK cannot create data or change clean status.
    ck_assert_uint_eq(mock_ieee_data_count(), 0);
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_TALK(device),
        IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();
    char expected[IEEE_STATUS_LINE_CAPACITY];
    const char* const suffix = d80 ? (drive == IEEE_DRIVE_FIRST ? ",0" : ",1") : "";
    snprintf(expected, sizeof(expected), "00, OK,00,00%s\r", suffix);
    assert_status(expected);
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
    mock_ieee_consume_status(3);

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

    // Text commands use CMDSIZ, ignoring a digit beyond a penultimate CR.
    const uint8_t entries[] = {
        IEEE_CMD_OPEN(IEEE_CHANNEL_COMMAND), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND),
    };
    for (size_t entry = 0; entry < count_of(entries); ++entry) {
        const char* commands[] = {"I\r0", "V\r0"};
        for (size_t command = 0; command < count_of(commands); ++command) {
            enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), entries[entry]);
            enqueue_name(commands[command]);
            enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device),
                IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
            ieee_drive_task();
            assert_status("00, OK,00,00,1\r");
            mock_ieee_clear_status();
        }
    }

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
    assert_status("62, FILE NOT FOUND,00,00\r");
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
        snprintf(expected, sizeof(expected), "62, FILE NOT FOUND,00,00,%u\r", drive);
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
            snprintf(expected, sizeof(expected), "62, FILE NOT FOUND,00,00,%u\r",
                     drive);
            assert_status(expected);
        } else {
            // The D64 side must use the 4040 four-field status even though its
            // sibling drive in the same unit contains D80 media.
            assert_status("62, FILE NOT FOUND,00,00\r");
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

// Verifies UNTALK pauses top-up without exhausting or closing a readable channel.
START_TEST(test_untalk_pauses_readable_sequential_channel) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);

    // Open without TALK so backpressure can be established before streaming.
    mount_d64(device, drive, "paused.d64", true);
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
    enqueue_filename_for_drive(drive, "BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    // Leave one burst free, keeping the stream readable after its first top-up.
    const size_t blocked = MOCK_IEEE_TX_CAPACITY - ECONOPET_IEEE_TX_BURST_CHUNK;
    for (size_t index = 0; index < blocked; ++index)
        spi_write_at(ECONOPET_WB_IEEE_TX_ADDR, 0);
    enqueue_command(IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_LOAD));
    ieee_drive_task();
    ck_assert_uint_eq(mock_ieee_data_count(), MOCK_IEEE_TX_CAPACITY);
    for (size_t index = 0; index < ECONOPET_IEEE_TX_BURST_CHUNK; ++index) {
        ck_assert_uint_eq(mock_ieee_data_byte(blocked + index), index + 2);
        ck_assert(!mock_ieee_data_eoi(blocked + index));
    }

    // Drain the mock FIFO, then prove UNTALK prevents unsolicited top-up.
    mock_ieee_clear_data();
    enqueue_command(IEEE_CMD_UNTALK);
    ieee_drive_task();
    ck_assert_uint_eq(mock_ieee_data_count(), 0);
    ieee_drive_task();
    ck_assert_uint_eq(mock_ieee_data_count(), 0);

    // Readdressing resumes at the saved cursor, not at the beginning or EOF.
    enqueue_command(IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_LOAD));
    ieee_drive_task();
    const size_t remaining_first_sector = DISKIMAGE_SECTOR_PAYLOAD_SIZE - ECONOPET_IEEE_TX_BURST_CHUNK;
    const size_t final_sector_bytes = 10;
    ck_assert_uint_eq(mock_ieee_data_count(), remaining_first_sector + final_sector_bytes);
    for (size_t index = 0; index < remaining_first_sector; ++index) {
        ck_assert_uint_eq(mock_ieee_data_byte(index), index + ECONOPET_IEEE_TX_BURST_CHUNK + 2);
        ck_assert(!mock_ieee_data_eoi(index));
    }
    for (size_t index = 0; index < final_sector_bytes; ++index) {
        ck_assert_uint_eq(mock_ieee_data_byte(remaining_first_sector + index), 0xe2 + index);
        ck_assert_int_eq(mock_ieee_data_eoi(remaining_first_sector + index),
            index + 1 == final_sector_bytes);
    }
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
    mock_ieee_enqueue_rx_eoi(false, 'I', true);
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();
    // I clears the pending file-not-found error to DOS status 00.
    assert_status("00, OK,00,00\r");
}
END_TEST

// Verifies an unreadable successor in a sequential chain reports status 23.
START_TEST(test_corrupt_sequential_chain_reports_read_error) {
    const unsigned int device = loop_device(_i / count_of(native_image_types));
    const unsigned int drive = loop_drive(_i / count_of(native_image_types));
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const bool d80 = type == diskimage_type_d80;
    // Corrupt BASIC's successor link after a valid first sector.
    uint8_t* image = d80 ? diskimage_test_make_d80() : diskimage_test_make_d64();
    uint32_t first_sector;
    const unsigned int first_track = d80 ? 38 : 17;
    ck_assert(diskimage_sector_offset(type, first_track, 0, &first_sector));
    uint8_t* sector = image + first_sector;
    sector[0] = (d80 ? DISKIMAGE_D80_TRACK_COUNT : DISKIMAGE_D64_TRACK_COUNT) + 1;
    sector[1] = 0;
    for (size_t index = 0; index < DISKIMAGE_SECTOR_PAYLOAD_SIZE; ++index)
        sector[DISKIMAGE_SECTOR_LINK_SIZE + index] = (uint8_t) (index + 2);
    mount_image(device, drive, d80 ? "corrupt.d80" : "corrupt.d64", image,
        d80 ? DISKIMAGE_D80_SIZE : DISKIMAGE_D64_SIZE, true);

    // Request the corrupted file through its normal sequential channel.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_LOAD));
    enqueue_filename_for_drive(drive, "BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_LOAD));
    ieee_drive_task();

    // Valid prefix bytes survive without a fabricated byte or EOI on failure.
    ck_assert_uint_eq(mock_ieee_data_count(), DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    for (size_t index = 0; index < DISKIMAGE_SECTOR_PAYLOAD_SIZE; index++) {
        ck_assert_uint_eq(mock_ieee_data_byte(index), index + 2);
        ck_assert(!mock_ieee_data_eoi(index));
    }
    mock_ieee_clear_data();

    // Read command-channel status after the partial data response.
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();
    // The incomplete chain records DOS read error 23 on channel 15.
    char expected[IEEE_STATUS_LINE_CAPACITY];
    const char* const suffix = d80 ? (drive == IEEE_DRIVE_FIRST ? ",0" : ",1") : "";
    snprintf(expected, sizeof(expected), "23,READ ERROR,00,00%s\r", suffix);
    assert_status(expected);

    // Readdressing a released channel must not retry or regenerate its error.
    mock_ieee_clear_status();
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_TALK(device),
        IEEE_CMD_SECONDARY(IEEE_CHANNEL_LOAD));
    ieee_drive_task();
    ck_assert_uint_eq(mock_ieee_data_count(), 0);
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_TALK(device),
        IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();
    snprintf(expected, sizeof(expected), "00, OK,00,00%s\r", suffix);
    assert_status(expected);
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
    mock_ieee_enqueue_rx_eoi(false, 'V', true);
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
    mock_ieee_enqueue_rx_eoi(false, 0, true);
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
    mock_ieee_enqueue_rx_eoi(false, 125, true);
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
    mock_ieee_enqueue_rx_eoi(false, 0, true);
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();

    // The command channel exposes the missing-record DOS status.
    assert_status("50,RECORD NOT PRESENT,00,00\r");
}
END_TEST

// Verifies REL writes surface the correct status when the mounted image is read-only.
START_TEST(test_relative_write_to_read_only_image_reports_26) {
    const unsigned int device = loop_device(_i / count_of(native_image_types));
    const unsigned int drive = loop_drive(_i / count_of(native_image_types));
    const bool d80 = native_image_types[_i % count_of(native_image_types)] == diskimage_type_d80;

    // Decorate D80's BASIC entry as a two-record REL file before mounting read-only.
    if (d80) {
        uint8_t* const image = diskimage_test_make_d80();
        uint32_t directory;
        ck_assert(diskimage_sector_offset(diskimage_type_d80, 39, 1, &directory));
        image[directory + 2] = 0x80 | DISKIMAGE_FTYPE_REL;
        image[directory + DISKIMAGE_DIRECTORY_REL_LENGTH_OFFSET] = TEST_COMMAND_RECORD_LENGTH;
        uint8_t* const writable = malloc(DISKIMAGE_D80_SIZE);
        ck_assert_ptr_nonnull(writable);
        memcpy(writable, image, DISKIMAGE_D80_SIZE);
        mount_image(device, drive ^ 1u, "writable.d80", writable, DISKIMAGE_D80_SIZE, true);
        mount_image(device, drive, "rel.d80", image, DISKIMAGE_D80_SIZE, false);
    } else {
        mount_d64(device, drive ^ 1u, "writable.d64", true);
        mount_d64(device, drive, "rel.d64", false);
    }

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, d80 ? "BASIC,L" : "DATA,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    // EOI must surface the write failure without waiting for UNLISTEN.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    mock_ieee_enqueue_rx_eoi(false, 'Z', true);
    ieee_drive_task();
    enqueue_command(IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();

    // A failed commit must report write protection rather than status 00.
    char expected[IEEE_STATUS_LINE_CAPACITY];
    snprintf(expected, sizeof(expected), "26,WRITE PROTECT ON,00,00%s\r",
        d80 ? (drive == 0 ? ",0" : ",1") : "");
    assert_status(expected);
    mock_ieee_clear_status();

    // A failed write leaves the original record readable after a fresh P.
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device),
        IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    mock_ieee_enqueue_rx(false, 'P');
    mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    mock_ieee_enqueue_rx(false, 1);
    mock_ieee_enqueue_rx_eoi(false, 0, true);
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device),
        IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    ieee_drive_task();
    const unsigned int length = d80 ? TEST_COMMAND_RECORD_LENGTH : 129;
    ck_assert_uint_eq(mock_ieee_data_count(), length);
    for (unsigned int index = 0; index < length; ++index) {
        ck_assert_uint_eq(mock_ieee_data_byte(index), d80 ? 0x80 + index : 0xa0);
        ck_assert_int_eq(mock_ieee_data_eoi(index), index + 1 == length);
    }
    mock_ieee_clear_data();

    // Reuse the same logical channel on writable media without resetting DOS.
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive ^ 1u, d80 ? "BASIC,L" : "DATA,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    mock_ieee_enqueue_rx_eoi(false, 'Z', true);
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(device),
        IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    mock_ieee_enqueue_rx(false, 'P');
    mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    mock_ieee_enqueue_rx(false, 1);
    mock_ieee_enqueue_rx_eoi(false, 0, true);
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device),
        IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    ieee_drive_task();
    ck_assert_uint_eq(mock_ieee_data_count(), 1);
    ck_assert_uint_eq(mock_ieee_data_byte(0), 'Z');
    ck_assert(mock_ieee_data_eoi(0));
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
    mock_ieee_enqueue_rx_eoi(false, 0, true);
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

// Mounts a bounded native REL data chain, without claiming side-sector validity.
static void mount_native_relative_records(unsigned int device, unsigned int drive,
        diskimage_type_t type, const char* filename, uint8_t record_length,
        const uint8_t* payload, size_t length) {
    const bool d80 = type == diskimage_type_d80;
    const size_t max_payload = TEST_REL_RECORD_COUNT * DISKIMAGE_REL_MAX_RECORD_LENGTH;
    ck_assert_uint_gt(length, 0);
    ck_assert_uint_le(length, max_payload);
    uint8_t* image = d80 ? diskimage_test_make_d80() : diskimage_test_make_d64();
    const unsigned int data_track = d80 ? 38 : 17;
    uint32_t directory;
    ck_assert(diskimage_sector_offset(type, d80 ? 39 : 18, 1, &directory));
    uint8_t* entry = image + directory;
    const size_t file_type_offset = 2, start_track_offset = 3, start_sector_offset = 4;
    const uint8_t closed_relative_file = 0x80 | DISKIMAGE_FTYPE_REL;
    entry[file_type_offset] = closed_relative_file;
    entry[start_track_offset] = (uint8_t) data_track;
    entry[start_sector_offset] = 0;
    entry[DISKIMAGE_DIRECTORY_REL_LENGTH_OFFSET] = record_length;

    // Encode consecutive data sectors and the exact final-sector payload length.
    size_t used = 0;
    for (unsigned int sector = 0; used < length; ++sector) {
        uint32_t offset;
        ck_assert(diskimage_sector_offset(type, data_track, sector, &offset));
        const size_t count = length - used < DISKIMAGE_SECTOR_PAYLOAD_SIZE
            ? length - used : DISKIMAGE_SECTOR_PAYLOAD_SIZE;
        const bool last = used + count == length;
        image[offset] = last ? 0 : (uint8_t) data_track;
        image[offset + 1] = (uint8_t) (last ? count + 1 : sector + 1);
        memcpy(image + offset + DISKIMAGE_SECTOR_LINK_SIZE, payload + used, count);
        used += count;
    }
    mount_image(device, drive, filename, image,
        d80 ? DISKIMAGE_D80_SIZE : DISKIMAGE_D64_SIZE, true);
}

// Verifies native empty-record NUL/EOI, advancement, trimming and repositioning.
START_TEST(test_native_empty_relative_records_are_not_skipped) {
    static const uint8_t lengths[] = {
        DISKIMAGE_REL_MIN_RECORD_LENGTH, 2, 128, 129, DISKIMAGE_REL_MAX_RECORD_LENGTH,
    };
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const uint8_t first_marker = 0xa5, middle_marker = 0x5a;

    // Cross both native models with both record bounds and sector crossings.
    for (size_t variant = 0; variant < count_of(lengths); ++variant) {
        const uint8_t length = lengths[variant];
        uint8_t payload[TEST_REL_RECORD_COUNT * DISKIMAGE_REL_MAX_RECORD_LENGTH] = {0};
        payload[2 * length] = first_marker;
        const size_t last = length > 2 ? length / 2 : 0;
        if (last > 0) payload[2 * length + last] = middle_marker;
        char filename[SD_PATH_MAX];
        snprintf(filename, sizeof(filename), "empty-rel-%u.%s", length,
            type == diskimage_type_d80 ? "d80" : "d64");
        mount_native_relative_records(device, drive, type, filename, length,
            payload, TEST_REL_RECORD_COUNT * length);
        enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
        enqueue_filename_for_drive(drive, "BASIC,L");
        enqueue_command(IEEE_CMD_UNLISTEN);
        ieee_drive_task();

        // Each empty record returns its own first NUL with EOI.
        for (size_t record = 0; record < TEST_REL_RECORD_COUNT; ++record) {
            enqueue_command(IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
            ieee_drive_task();
            const size_t expected = record < 2 ? 1 : last + 1;
            ck_assert_uint_eq(mock_ieee_data_count(), expected);
            for (size_t index = 0; index < expected; ++index) {
                ck_assert_uint_eq(mock_ieee_data_byte(index), payload[record * length + index]);
                ck_assert_int_eq(mock_ieee_data_eoi(index), index + 1 == expected);
            }
            mock_ieee_clear_data();
            enqueue_command(IEEE_CMD_UNTALK);
            ieee_drive_task();
        }

        // P's last-byte offset into an empty record retains a NUL, not record three.
        enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
        mock_ieee_enqueue_rx(false, 'P');
        mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
        mock_ieee_enqueue_rx(false, 2);
        mock_ieee_enqueue_rx(false, 0);
        mock_ieee_enqueue_rx_eoi(false, length, true);
        enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device),
            IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
        ieee_drive_task();
        ck_assert_uint_eq(mock_ieee_data_count(), 1);
        ck_assert_uint_eq(mock_ieee_data_byte(0), 0);
        ck_assert(mock_ieee_data_eoi(0));
        mock_ieee_clear_data();
    }
}
END_TEST

// Selects a REL read channel, processing commands through the production task.
static void relative_talk(unsigned int device, unsigned int channel) {
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(channel));
    ieee_drive_task();
}

// Executes a binary P command without consuming any pending data.
static void relative_position(unsigned int device, unsigned int channel,
        uint16_t record, uint8_t offset) {
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device),
        IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    mock_ieee_enqueue_rx(false, 'P');
    mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(channel));
    mock_ieee_enqueue_rx(false, (uint8_t) record);
    mock_ieee_enqueue_rx(false, (uint8_t) (record >> 8));
    mock_ieee_enqueue_rx_eoi(false, offset, true);
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
}

// Checks persisted native payload bytes independently of the REL read cursor.
static void assert_eoi_written_records(const char* filename, diskimage_type_t type,
        uint8_t length, unsigned int completed) {
    char path[SD_PATH_MAX];
    sd_make_path(path, SD_DIR_DISKS, filename);
    FILE* const file = fopen(path, "rb");
    ck_assert_ptr_nonnull(file);
    const unsigned int track = type == diskimage_type_d80 ? 38 : 17;
    for (unsigned int record = 0; record < TEST_REL_RECORD_COUNT; ++record) {
        for (unsigned int index = 0; index < length; ++index) {
            const unsigned int position = record * length + index;
            uint32_t offset;
            ck_assert(diskimage_sector_offset(type, track,
                position / DISKIMAGE_SECTOR_PAYLOAD_SIZE, &offset));
            offset += DISKIMAGE_SECTOR_LINK_SIZE + position % DISKIMAGE_SECTOR_PAYLOAD_SIZE;
            ck_assert_int_eq(fseek(file, (long) offset, SEEK_SET), 0);
            uint8_t byte;
            ck_assert_uint_eq(fread(&byte, 1, 1, file), 1);
            const uint8_t expected = record >= completed ? 0xa0
                : record == 0 ? (index == 0 ? 0xa0 : index == 1 ? 0xa5 : 0)
                : index == 0 ? 0xb5 : index == 1 ? 0xb6 : 0;
            ck_assert_uint_eq(byte, expected);
        }
    }
    ck_assert_int_eq(fclose(file), 0);
}

// Each EOI completes a REL record without UNLISTEN or corrupting its successor.
START_TEST(test_relative_eoi_completes_each_record) {
    const unsigned int coordinates = (unsigned int) _i / count_of(relative_eoi_lengths);
    const unsigned int device = loop_device((int) (coordinates / count_of(native_image_types)));
    const unsigned int drive = loop_drive((int) (coordinates / count_of(native_image_types)));
    const diskimage_type_t type = native_image_types[coordinates % count_of(native_image_types)];
    const uint8_t length = relative_eoi_lengths[_i % count_of(relative_eoi_lengths)];
    uint8_t payload[TEST_REL_RECORD_COUNT * DISKIMAGE_REL_MAX_RECORD_LENGTH];
    memset(payload, 0xa0, sizeof(payload));
    const char* const filename = "relative-eoi.img";
    mount_native_relative_records(device, drive, type, filename, length,
        payload, TEST_REL_RECORD_COUNT * length);
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    relative_position(device, IEEE_CHANNEL_REL, 1, 2);
    relative_position(device, IEEE_CHANNEL_REL + 1, 1, 1);

    // Keep the same listener selection across both EOI-terminated records.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    mock_ieee_enqueue_rx_eoi(false, 0xa5, true);
    ieee_drive_task();
    assert_eoi_written_records(filename, type, length, 1);
    mock_ieee_enqueue_rx(false, 0xb5);
    ieee_drive_task();
    assert_eoi_written_records(filename, type, length, 1);
    mock_ieee_enqueue_rx_eoi(false, 0xb6, true);
    ieee_drive_task();
    assert_eoi_written_records(filename, type, length, 2);

    // No further byte means UNLISTEN must not write an empty third record.
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    assert_eoi_written_records(filename, type, length, 2);
    // WR45 clears an earlier error on DOS 2.7. DOS 2 WR51 leaves it intact.
    relative_talk(device, IEEE_CHANNEL_COMMAND);
    char expected_status[IEEE_STATUS_LINE_CAPACITY];
    snprintf(expected_status, sizeof(expected_status), "%s,00,00%s\r",
        type == diskimage_type_d80 ? "00, OK" : "70,NO CHANNEL",
        type == diskimage_type_d80 ? (drive == 0 ? ",0" : ",1") : "");
    assert_status(expected_status);
    mock_ieee_clear_status();
    relative_position(device, IEEE_CHANNEL_REL, 1, 1);
    relative_talk(device, IEEE_CHANNEL_REL);
    ck_assert_uint_eq(mock_ieee_data_count(), 2);
    ck_assert_uint_eq(mock_ieee_data_byte(0), 0xa0);
    ck_assert(!mock_ieee_data_eoi(0));
    ck_assert_uint_eq(mock_ieee_data_byte(1), 0xa5);
    ck_assert(mock_ieee_data_eoi(1));
    mock_ieee_clear_data();
    relative_position(device, IEEE_CHANNEL_REL, 2, 1);
    relative_talk(device, IEEE_CHANNEL_REL);
    ck_assert_uint_eq(mock_ieee_data_count(), 2);
    ck_assert_uint_eq(mock_ieee_data_byte(0), 0xb5);
    ck_assert(!mock_ieee_data_eoi(0));
    ck_assert_uint_eq(mock_ieee_data_byte(1), 0xb6);
    ck_assert(mock_ieee_data_eoi(1));
}
END_TEST

// OPEN 15 and secondary 15 must execute the same counted binary P command.
START_TEST(test_command_open_positions_relative_records) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const uint8_t record_length = TEST_COMMAND_RECORD_LENGTH;
    const uint8_t records[] = {2, '\r', ','};
    uint8_t payload[TEST_COMMAND_RECORD_COUNT * TEST_COMMAND_RECORD_LENGTH];
    for (size_t index = 0; index < sizeof(payload); ++index)
        payload[index] = (uint8_t) (index + 1);
    mount_native_relative_records(device, drive, type, "command-open.img",
        record_length, payload, sizeof(payload));
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    // Include zero, CR and comma as binary record bytes, not string delimiters.
    const uint8_t command_entries[] = {
        IEEE_CMD_OPEN(IEEE_CHANNEL_COMMAND), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND),
    };
    for (size_t entry = 0; entry < count_of(command_entries); ++entry) {
        for (size_t variant = 0; variant < count_of(records); ++variant) {
            // Split service across the payload so collection must survive task calls.
            enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), command_entries[entry]);
            mock_ieee_enqueue_rx(false, 'P');
            mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
            mock_ieee_enqueue_rx(false, records[variant]);
            ieee_drive_task();
            mock_ieee_enqueue_rx(false, 0);
            mock_ieee_enqueue_rx_eoi(false, 1, true);
            enqueue_command(IEEE_CMD_UNLISTEN);
            ieee_drive_task();

            // RECORD selects this file's drive and completes with native OK status.
            relative_talk(device, IEEE_CHANNEL_COMMAND);
            char expected[IEEE_STATUS_LINE_CAPACITY];
            snprintf(expected, sizeof(expected), "00, OK,00,00%s\r",
                type == diskimage_type_d80 ? (drive == 0 ? ",0" : ",1") : "");
            assert_status(expected);
            mock_ieee_clear_status();

            // The selected record must be read, not the previous/default record.
            relative_talk(device, IEEE_CHANNEL_REL);
            ck_assert_uint_eq(mock_ieee_data_count(), record_length);
            const size_t offset = (records[variant] - 1) * record_length;
            for (size_t index = 0; index < record_length; ++index) {
                ck_assert_uint_eq(mock_ieee_data_byte(index), payload[offset + index]);
                ck_assert_int_eq(mock_ieee_data_eoi(index), index + 1 == record_length);
            }
            mock_ieee_clear_data();
        }
    }
}
END_TEST

// Missing and sequential channels have distinct RECORD errors and permit recovery.
START_TEST(test_command_channel_errors_and_recovery) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const uint8_t payload[] = {1, 2, 3, 4, 5, 6};
    const unsigned int sequential_channel = IEEE_CHANNEL_REL + 1;
    const unsigned int missing_channel = sequential_channel + 1;
    mount_native_relative_records(device, drive, type, "command-errors.img",
        TEST_COMMAND_RECORD_LENGTH, payload, sizeof(payload));
    if (type == diskimage_type_d80)
        mount_d80(device, drive ^ 1u, "sequential.d80", true);
    else
        mount_d64(device, drive ^ 1u, "sequential.d64", true);
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(sequential_channel));
    enqueue_filename_for_drive(drive ^ 1u, "BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    const uint8_t entries[] = {
        IEEE_CMD_OPEN(IEEE_CHANNEL_COMMAND), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND),
    };
    const unsigned int channels[] = {missing_channel, sequential_channel, IEEE_CHANNEL_COMMAND};
    const char* const errors[] = {"70,NO CHANNEL", "64, FILE TYPE MISMATCH", "64, FILE TYPE MISMATCH"};
    const char* const suffix = type == diskimage_type_d80 ? (drive == 0 ? ",0" : ",1") : "";
    for (size_t entry = 0; entry < count_of(entries); ++entry) {
        for (size_t variant = 0; variant < count_of(channels); ++variant) {
            // Establish the last selected drive without reading or advancing REL data.
            relative_position(device, IEEE_CHANNEL_REL, 1, 1);
            enqueue_command(IEEE_CMD_LISTEN(device), entries[entry]);
            mock_ieee_enqueue_rx(false, 'P');
            mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(channels[variant]));
            mock_ieee_enqueue_rx(false, 2);
            mock_ieee_enqueue_rx(false, 0);
            mock_ieee_enqueue_rx_eoi(false, 1, true);
            enqueue_command(IEEE_CMD_UNLISTEN);
            ieee_drive_task();
            relative_talk(device, IEEE_CHANNEL_COMMAND);
            char expected[IEEE_STATUS_LINE_CAPACITY];
            snprintf(expected, sizeof(expected), "%s,00,00%s\r", errors[variant], suffix);
            assert_status(expected);
            mock_ieee_clear_status();

            // A rejected P must leave the valid REL channel at its original record.
            relative_talk(device, IEEE_CHANNEL_REL);
            ck_assert_uint_eq(mock_ieee_data_count(), TEST_COMMAND_RECORD_LENGTH);
            ck_assert_uint_eq(mock_ieee_data_byte(0), payload[0]);
            ck_assert_uint_eq(mock_ieee_data_byte(1), payload[1]);
            ck_assert(!mock_ieee_data_eoi(0));
            ck_assert(mock_ieee_data_eoi(1));
            mock_ieee_clear_data();

            // Recover through the same entry path, not just a fresh drive reset.
            enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), entries[entry]);
            mock_ieee_enqueue_rx(false, 'P');
            mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
            mock_ieee_enqueue_rx(false, 2);
            mock_ieee_enqueue_rx(false, 0);
            mock_ieee_enqueue_rx_eoi(false, 1, true);
            enqueue_command(IEEE_CMD_UNLISTEN);
            ieee_drive_task();
            relative_talk(device, IEEE_CHANNEL_COMMAND);
            snprintf(expected, sizeof(expected), "00, OK,00,00%s\r", suffix);
            assert_status(expected);
            mock_ieee_clear_status();
            relative_talk(device, IEEE_CHANNEL_REL);
            ck_assert_uint_eq(mock_ieee_data_count(), TEST_COMMAND_RECORD_LENGTH);
            ck_assert_uint_eq(mock_ieee_data_byte(0), payload[2]);
            ck_assert_uint_eq(mock_ieee_data_byte(1), payload[3]);
            ck_assert(mock_ieee_data_eoi(1));
            mock_ieee_clear_data();
        }
    }
}
END_TEST

// UNLISTEN cannot complete P, whereas EOI completes it without an UNLISTEN.
START_TEST(test_command_completion_requires_eoi) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const uint8_t payload[] = {1, 2, 3, 4, 5, 6, 7, 8};
    mount_native_relative_records(device, drive, type, "command-eoi.img",
        TEST_COMMAND_RECORD_LENGTH, payload, sizeof(payload));
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    const uint8_t entries[] = {
        IEEE_CMD_OPEN(IEEE_CHANNEL_COMMAND), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND),
    };
    for (size_t entry = 0; entry < count_of(entries); ++entry) {
        relative_position(device, IEEE_CHANNEL_REL, 1, 1);
        enqueue_command(IEEE_CMD_LISTEN(device), entries[entry]);
        mock_ieee_enqueue_rx(false, 'P');
        mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
        mock_ieee_enqueue_rx(false, 3);
        ieee_drive_task();
        mock_ieee_enqueue_rx(false, 0);
        mock_ieee_enqueue_rx(false, 1);
        enqueue_command(IEEE_CMD_UNLISTEN);
        ieee_drive_task();

        // These exact P bytes lack final EOI and must not change the record.
        relative_talk(device, IEEE_CHANNEL_REL);
        ck_assert_uint_eq(mock_ieee_data_count(), TEST_COMMAND_RECORD_LENGTH);
        ck_assert_uint_eq(mock_ieee_data_byte(0), payload[0]);
        ck_assert_uint_eq(mock_ieee_data_byte(1), payload[1]);
        mock_ieee_clear_data();

        // Another unit's completed command must not consume this unit's prefix.
        const unsigned int peer = IEEE_FIRST_DEVICE
            + (device - IEEE_FIRST_DEVICE + 1) % IEEE_DEVICE_COUNT;
        enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(peer), entries[entry]);
        mock_ieee_enqueue_rx(false, 'P');
        mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
        mock_ieee_enqueue_rx(false, 1);
        mock_ieee_enqueue_rx_eoi(false, 0, true);
        enqueue_command(IEEE_CMD_UNLISTEN);
        ieee_drive_task();

        // EOI only marks pending work. Immediate TALK serves existing CHNDAT first.
        enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), entries[entry]);
        mock_ieee_enqueue_rx_eoi(false, '\r', true);
        ieee_drive_task();
        relative_talk(device, IEEE_CHANNEL_REL);
        ck_assert_uint_eq(mock_ieee_data_count(), TEST_COMMAND_RECORD_LENGTH);
        ck_assert_uint_eq(mock_ieee_data_byte(0), payload[2]);
        ck_assert_uint_eq(mock_ieee_data_byte(1), payload[3]);
        ck_assert(!mock_ieee_data_eoi(0));
        ck_assert(mock_ieee_data_eoi(1));
        mock_ieee_clear_data();

        // UNTALK reaches IDLE and dispatches the retained P selecting record three.
        enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_UNLISTEN);
        relative_talk(device, IEEE_CHANNEL_REL);
        ck_assert_uint_eq(mock_ieee_data_count(), TEST_COMMAND_RECORD_LENGTH);
        ck_assert_uint_eq(mock_ieee_data_byte(0), payload[4]);
        ck_assert_uint_eq(mock_ieee_data_byte(1), payload[5]);
        ck_assert(mock_ieee_data_eoi(1));
        mock_ieee_clear_data();
        enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_UNLISTEN);
        relative_talk(device, IEEE_CHANNEL_REL);
        ck_assert_uint_eq(mock_ieee_data_byte(0), payload[6]);
        ck_assert_uint_eq(mock_ieee_data_byte(1), payload[7]);
        ck_assert(mock_ieee_data_eoi(1));
        mock_ieee_clear_data();
    }
}
END_TEST

// Ordinary OPEN consumes its unit's shared input, but not a peer's partial command.
START_TEST(test_open_consumes_shared_command_input) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const unsigned int peer = IEEE_FIRST_DEVICE
        + (device - IEEE_FIRST_DEVICE + 1) % IEEE_DEVICE_COUNT;
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const uint8_t payload[] = {1, 2, 3, 4, 5, 6};
    mount_native_relative_records(device, drive, type, "shared-input.img",
        TEST_COMMAND_RECORD_LENGTH, payload, sizeof(payload));
    const uint8_t entries[] = {
        IEEE_CMD_OPEN(IEEE_CHANNEL_COMMAND), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND),
    };
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    for (size_t entry = 0; entry < count_of(entries); ++entry) {
        for (unsigned int same_unit = 0; same_unit < 2; ++same_unit) {
            enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), entries[entry]);
            mock_ieee_enqueue_rx(false, 'P');
            mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
            mock_ieee_enqueue_rx(false, 3);
            mock_ieee_enqueue_rx(false, 0);
            mock_ieee_enqueue_rx(false, 1);
            enqueue_command(IEEE_CMD_UNLISTEN);
            ieee_drive_task();

            // Even a failed ordinary OPEN consumes only its own collected input.
            enqueue_command(IEEE_CMD_LISTEN(same_unit ? device : peer),
                IEEE_CMD_OPEN(IEEE_CHANNEL_REL + 1));
            enqueue_name("MISSING");
            enqueue_command(IEEE_CMD_UNLISTEN);
            ieee_drive_task();
            enqueue_command(IEEE_CMD_LISTEN(device), entries[entry]);
            if (same_unit) {
                mock_ieee_enqueue_rx(false, 'P');
                mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
                mock_ieee_enqueue_rx(false, 2);
                mock_ieee_enqueue_rx(false, 0);
                mock_ieee_enqueue_rx_eoi(false, 1, true);
            } else {
                mock_ieee_enqueue_rx_eoi(false, '\r', true);
            }
            enqueue_command(IEEE_CMD_UNLISTEN);
            ieee_drive_task();
            relative_talk(device, IEEE_CHANNEL_REL);
            const size_t offset = same_unit ? 2 : 4;
            ck_assert_uint_eq(mock_ieee_data_count(), TEST_COMMAND_RECORD_LENGTH);
            ck_assert_uint_eq(mock_ieee_data_byte(0), payload[offset]);
            ck_assert_uint_eq(mock_ieee_data_byte(1), payload[offset + 1]);
            ck_assert(!mock_ieee_data_eoi(0));
            ck_assert(mock_ieee_data_eoi(1));
            mock_ieee_clear_data();
        }
    }
}
END_TEST

// Incomplete ordinary OPEN input survives UNLISTEN and completes only on EOI.
START_TEST(test_open_input_requires_eoi) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const uint8_t payload[] = {1, 2, 3, 4};
    mount_native_relative_records(device, drive, type, "open-eoi.img",
        TEST_COMMAND_RECORD_LENGTH, payload, sizeof(payload));
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    mock_ieee_enqueue_rx(false, (uint8_t) ('0' + drive));
    mock_ieee_enqueue_rx(false, ':');
    mock_ieee_enqueue_rx(false, 'B');
    mock_ieee_enqueue_rx(false, 'A');
    mock_ieee_enqueue_rx(false, 'S');
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_name("IC,L");
    ieee_drive_task();

    // The filename is pending until UNLISTEN returns this peripheral to IDLE.
    ck_assert_uint_eq(mock_ieee_data_count(), 0);
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    relative_talk(device, IEEE_CHANNEL_REL);
    ck_assert_uint_eq(mock_ieee_data_count(), TEST_COMMAND_RECORD_LENGTH);
    ck_assert_uint_eq(mock_ieee_data_byte(0), payload[0]);
    ck_assert_uint_eq(mock_ieee_data_byte(1), payload[1]);
    ck_assert(!mock_ieee_data_eoi(0));
    ck_assert(mock_ieee_data_eoi(1));
}
END_TEST

// LISTEN is cumulative across peripherals, while TALK clears only its own listener.
START_TEST(test_virtual_units_retain_independent_listeners) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const unsigned int peer = IEEE_FIRST_DEVICE
        + (device - IEEE_FIRST_DEVICE + 1) % IEEE_DEVICE_COUNT;
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const uint8_t payload[] = {1, 2};
    mount_native_relative_records(device, drive, type, "listener-roles.img",
        TEST_COMMAND_RECORD_LENGTH, payload, sizeof(payload));
    for (unsigned int talk_same_unit = 0; talk_same_unit < 2; ++talk_same_unit) {
        relative_position(device, IEEE_CHANNEL_COMMAND, 1, 1);
        relative_position(peer, IEEE_CHANNEL_COMMAND, 1, 1);
        enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device),
            IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND), IEEE_CMD_LISTEN(peer),
            IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
        if (talk_same_unit) enqueue_command(IEEE_CMD_TALK(device));
        mock_ieee_enqueue_rx(false, 'P');
        mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
        mock_ieee_enqueue_rx(false, 1);
        mock_ieee_enqueue_rx(false, 0);
        mock_ieee_enqueue_rx_eoi(false, 1, true);
        enqueue_command(IEEE_CMD_UNLISTEN);
        ieee_drive_task();
        relative_talk(device, IEEE_CHANNEL_COMMAND);
        char expected[IEEE_STATUS_LINE_CAPACITY];
        snprintf(expected, sizeof(expected), "%s%s",
            talk_same_unit ? "64, FILE TYPE MISMATCH,00,00" : "70,NO CHANNEL,00,00",
            type == diskimage_type_d80 ? ",0\r" : "\r");
        assert_status(expected);
        mock_ieee_clear_status();
        relative_talk(peer, IEEE_CHANNEL_COMMAND);
        assert_status("70,NO CHANNEL,00,00\r");
        mock_ieee_clear_status();
    }

    // A peer LISTEN retains the writer, but its own LISTEN restores default SA 0.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    for (unsigned int same_unit = 0; same_unit < 2; ++same_unit) {
        relative_position(device, IEEE_CHANNEL_REL, 1, 1);
        enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
        mock_ieee_enqueue_rx(false, 'X');
        enqueue_command(IEEE_CMD_LISTEN(same_unit ? device : peer));
        mock_ieee_enqueue_rx(false, 'Y');
        enqueue_command(IEEE_CMD_UNLISTEN);
        ieee_drive_task();
        relative_position(device, IEEE_CHANNEL_REL, 1, 1);
        relative_talk(device, IEEE_CHANNEL_REL);
        const size_t length = same_unit ? 1 : TEST_COMMAND_RECORD_LENGTH;
        ck_assert_uint_eq(mock_ieee_data_count(), length);
        ck_assert_uint_eq(mock_ieee_data_byte(0), 'X');
        if (!same_unit) ck_assert_uint_eq(mock_ieee_data_byte(1), 'Y');
        ck_assert(mock_ieee_data_eoi(length - 1));
        mock_ieee_clear_data();
    }
}
END_TEST

// EOI marks pending work but does not split one continuous LISTEN into commands.
START_TEST(test_command_dispatch_waits_for_idle) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const uint8_t payload[] = {1, 2, 3, 4, 5, 6};
    mount_native_relative_records(device, drive, type, "pending-command.img",
        TEST_COMMAND_RECORD_LENGTH, payload, sizeof(payload));
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    const uint8_t entries[] = {
        IEEE_CMD_OPEN(IEEE_CHANNEL_COMMAND), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND),
    };
    for (size_t entry = 0; entry < count_of(entries); ++entry) {
        enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), entries[entry]);
        for (uint8_t record = 2; record <= 3; ++record) {
            mock_ieee_enqueue_rx(false, 'P');
            mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
            mock_ieee_enqueue_rx(false, record);
            mock_ieee_enqueue_rx(false, 0);
            mock_ieee_enqueue_rx_eoi(false, 1, true);
            ieee_drive_task();
        }
        enqueue_command(IEEE_CMD_UNLISTEN);
        ieee_drive_task();
        relative_talk(device, IEEE_CHANNEL_REL);
        ck_assert_uint_eq(mock_ieee_data_count(), TEST_COMMAND_RECORD_LENGTH);
        ck_assert_uint_eq(mock_ieee_data_byte(0), payload[2]);
        ck_assert_uint_eq(mock_ieee_data_byte(1), payload[3]);
        ck_assert(mock_ieee_data_eoi(1));
        mock_ieee_clear_data();
    }
}
END_TEST

// NOTLK returns to IDLE without clearing the peripheral's TALK role.
START_TEST(test_pending_command_dispatch_after_unopened_talk) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const uint8_t payload[] = {1, 2, 3, 4, 5, 6, 7, 8};
    mount_native_relative_records(device, drive, type, "notlk.img",
        TEST_COMMAND_RECORD_LENGTH, payload, sizeof(payload));
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    const uint8_t entries[] = {
        IEEE_CMD_OPEN(IEEE_CHANNEL_COMMAND), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND),
    };
    enum { unopened_channel, closed_channel, default_channel };
    for (size_t entry = 0; entry < count_of(entries); ++entry) {
        for (unsigned int selection = unopened_channel; selection <= default_channel; ++selection) {
            if (selection == closed_channel) {
                enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL + 1));
                enqueue_filename_for_drive(drive, "BASIC,L");
                enqueue_command(IEEE_CMD_UNLISTEN);
                ieee_drive_task();
                enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_CLOSE(IEEE_CHANNEL_REL + 1),
                    IEEE_CMD_UNLISTEN);
                ieee_drive_task();
            }
            relative_position(device, IEEE_CHANNEL_REL, 1, 1);
            enqueue_command(IEEE_CMD_LISTEN(device), entries[entry]);
            const uint8_t command[] = {'P', IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL), 3, 0, 1};
            for (size_t index = 0; index < sizeof(command); ++index)
                mock_ieee_enqueue_rx_eoi(false, command[index], index + 1 == sizeof(command));
            ieee_drive_task();
            // Only the final ATN release may dispatch. Do not use the UNLISTEN helper.
            mock_ieee_enqueue_rx(true, IEEE_CMD_UNLISTEN);
            mock_ieee_enqueue_rx(true, IEEE_CMD_TALK(device));
            if (selection != default_channel)
                mock_ieee_enqueue_rx(true, IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL + 1));
            mock_ieee_enqueue_atn_end();
            ieee_drive_task();
            // Reselect without UNTALK, which would otherwise hide the missing dispatch.
            enqueue_command(IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
            ieee_drive_task();
            ck_assert_uint_eq(mock_ieee_data_count(), TEST_COMMAND_RECORD_LENGTH);
            ck_assert_uint_eq(mock_ieee_data_byte(0), payload[4]);
            ck_assert_uint_eq(mock_ieee_data_byte(1), payload[5]);
            ck_assert(!mock_ieee_data_eoi(0));
            ck_assert(mock_ieee_data_eoi(1));
            mock_ieee_clear_data();
            mock_ieee_enqueue_atn_end();
            ieee_drive_task();
            enqueue_command(IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
            ieee_drive_task();
            ck_assert_uint_eq(mock_ieee_data_count(), TEST_COMMAND_RECORD_LENGTH);
            ck_assert_uint_eq(mock_ieee_data_byte(0), payload[6]);
            ck_assert_uint_eq(mock_ieee_data_byte(1), payload[7]);
            ck_assert(mock_ieee_data_eoi(1));
            mock_ieee_clear_data();
        }
    }
}
END_TEST

// A consumed SEQ EOF is NOTRDY, unlike an EOF still queued for its final GET.
START_TEST(test_pending_command_dispatch_after_sequential_eof) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const uint8_t payload[] = {1, 2, 3, 4, 5, 6};
    const unsigned int sequential = IEEE_CHANNEL_REL + 1;
    mount_native_relative_records(device, drive, type, "eof-idle.img",
        TEST_COMMAND_RECORD_LENGTH, payload, sizeof(payload));
    if (type == diskimage_type_d80) mount_d80(device, drive ^ 1u, "plain.d80", true);
    else mount_d64(device, drive ^ 1u, "plain.d64", true);
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    const uint8_t entries[] = {
        IEEE_CMD_OPEN(IEEE_CHANNEL_COMMAND), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND),
    };
    for (size_t entry = 0; entry < count_of(entries); ++entry) {
        for (unsigned int consumed = 0; consumed <= 1; ++consumed) {
            relative_position(device, IEEE_CHANNEL_REL, 1, 1);
            enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(sequential));
            enqueue_filename_for_drive(drive ^ 1u, "BASIC");
            enqueue_command(IEEE_CMD_UNLISTEN);
            ieee_drive_task();
            relative_talk(device, sequential);
            const size_t length = mock_ieee_data_count();
            ck_assert_uint_gt(length, 0);
            ck_assert(mock_ieee_data_eoi(length - 1));
            if (consumed) mock_ieee_clear_data();
            enqueue_command(IEEE_CMD_LISTEN(device), entries[entry]);
            const uint8_t command[] = {'P', IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL), 3, 0, 1};
            for (size_t index = 0; index < sizeof(command); ++index)
                mock_ieee_enqueue_rx_eoi(false, command[index], index + 1 == sizeof(command));
            mock_ieee_enqueue_rx(true, IEEE_CMD_UNLISTEN);
            mock_ieee_enqueue_rx(true, IEEE_CMD_TALK(device));
            mock_ieee_enqueue_rx(true, IEEE_CMD_SECONDARY(sequential));
            mock_ieee_enqueue_atn_end();
            ieee_drive_task();
            if (!consumed) {
                ck_assert_uint_eq(mock_ieee_data_count(), length);
                mock_ieee_clear_data();
                ieee_drive_task();
            }
            enqueue_command(IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
            ieee_drive_task();
            ck_assert_uint_eq(mock_ieee_data_count(), TEST_COMMAND_RECORD_LENGTH);
            ck_assert_uint_eq(mock_ieee_data_byte(0), payload[4]);
            ck_assert_uint_eq(mock_ieee_data_byte(1), payload[5]);
            ck_assert(!mock_ieee_data_eoi(0));
            ck_assert(mock_ieee_data_eoi(1));
            mock_ieee_clear_data();
        }
    }
}
END_TEST

// Native unread termination dispatches without advancing either source GET count.
START_TEST(test_pending_command_dispatch_after_talk_without_get) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const uint8_t payload[] = {1, 2, 3, 4, 5, 6};
    mount_native_relative_records(device, drive, type, "no-get.img",
        TEST_COMMAND_RECORD_LENGTH, payload, sizeof(payload));
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    const uint8_t entries[] = {
        IEEE_CMD_OPEN(IEEE_CHANNEL_COMMAND), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND),
    };
    const uint8_t sources[] = {IEEE_CHANNEL_REL, IEEE_CHANNEL_COMMAND};
    for (size_t entry = 0; entry < count_of(entries); ++entry) {
        for (size_t source = 0; source < count_of(sources); ++source) {
            relative_position(device, IEEE_CHANNEL_REL, 1, 1);
            enqueue_command(IEEE_CMD_LISTEN(device), entries[entry]);
            const uint8_t command[] = {'P', IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL), 3, 0, 1};
            for (size_t index = 0; index < sizeof(command); ++index)
                mock_ieee_enqueue_rx_eoi(false, command[index], index + 1 == sizeof(command));
            mock_ieee_enqueue_rx(true, IEEE_CMD_UNLISTEN);
            mock_ieee_enqueue_rx(true, IEEE_CMD_TALK(device));
            mock_ieee_enqueue_rx(true, IEEE_CMD_SECONDARY(sources[source]));
            mock_ieee_enqueue_atn_end();
            ieee_drive_task();
            const uint8_t data_get = spi_read_at(ECONOPET_WB_IEEE_TX_ADDR);
            const uint8_t status_get = spi_read_at(ECONOPET_WB_IEEE_TXS_ADDR);
            mock_ieee_enqueue_talk_event(
                ((device - IEEE_FIRST_DEVICE) << ECONOPET_IEEE_TX_OWNER_UNIT_SHIFT)
                    | sources[source], 0);
            ieee_drive_task();
            ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_TX_ADDR), data_get);
            ck_assert_uint_eq(spi_read_at(ECONOPET_WB_IEEE_TXS_ADDR), status_get);
            enqueue_command(IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
            ieee_drive_task();
            ck_assert_uint_eq(mock_ieee_data_count(), TEST_COMMAND_RECORD_LENGTH);
            ck_assert_uint_eq(mock_ieee_data_byte(0), payload[4]);
            ck_assert_uint_eq(mock_ieee_data_byte(1), payload[5]);
            ck_assert(!mock_ieee_data_eoi(0));
            ck_assert(mock_ieee_data_eoi(1));
            mock_ieee_clear_data();
        }
    }
}
END_TEST

// LSN21 returns only after DAV releases. LSN25 on SA1 remains an active listener.
START_TEST(test_pending_command_dispatch_after_unopened_listen) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const uint8_t payload[] = {1, 2, 3, 4, 5, 6, 7, 8};
    mount_native_relative_records(device, drive, type, "listener-idle.img",
        TEST_COMMAND_RECORD_LENGTH, payload, sizeof(payload));
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    const uint8_t entries[] = {
        IEEE_CMD_OPEN(IEEE_CHANNEL_COMMAND), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND),
    };
    const uint8_t listeners[] = {IEEE_CHANNEL_REL + 1, IEEE_SAVE_CHANNEL};
    for (size_t entry = 0; entry < count_of(entries); ++entry) {
        for (size_t listener = 0; listener < count_of(listeners); ++listener) {
            for (unsigned int released = 0; released <= 1; ++released) {
                relative_position(device, IEEE_CHANNEL_REL, 1, 1);
                enqueue_command(IEEE_CMD_LISTEN(device), entries[entry]);
                const uint8_t command[] = {'P', IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL), 3, 0, 1};
                for (size_t index = 0; index < sizeof(command); ++index)
                    mock_ieee_enqueue_rx_eoi(false, command[index], index + 1 == sizeof(command));
                mock_ieee_enqueue_rx(true, IEEE_CMD_UNLISTEN);
                mock_ieee_enqueue_rx(true, IEEE_CMD_LISTEN(device));
                mock_ieee_enqueue_rx(true, IEEE_CMD_SECONDARY(listeners[listener]));
                mock_ieee_enqueue_atn_end();
                // Draining a data byte alone must not imply that DAV has released.
                mock_ieee_enqueue_rx(false, 0x55);
                ieee_drive_task();
                if (released) {
                    mock_ieee_enqueue_listen_end((uint8_t) (1u << (device - IEEE_FIRST_DEVICE)));
                    ieee_drive_task();
                }
                enqueue_command(IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
                ieee_drive_task();
                const bool dispatched = released && listeners[listener] != IEEE_SAVE_CHANNEL;
                const size_t offset = dispatched ? 4 : 0;
                ck_assert_uint_eq(mock_ieee_data_count(), TEST_COMMAND_RECORD_LENGTH);
                ck_assert_uint_eq(mock_ieee_data_byte(0), payload[offset]);
                ck_assert_uint_eq(mock_ieee_data_byte(1), payload[offset + 1]);
                ck_assert(!mock_ieee_data_eoi(0));
                ck_assert(mock_ieee_data_eoi(1));
                mock_ieee_clear_data();
                enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_UNLISTEN);
                ieee_drive_task();
                relative_talk(device, IEEE_CHANNEL_REL);
                const size_t next = dispatched ? 6 : 4;
                ck_assert_uint_eq(mock_ieee_data_count(), TEST_COMMAND_RECORD_LENGTH);
                ck_assert_uint_eq(mock_ieee_data_byte(0), payload[next]);
                ck_assert_uint_eq(mock_ieee_data_byte(1), payload[next + 1]);
                ck_assert(!mock_ieee_data_eoi(0));
                ck_assert(mock_ieee_data_eoi(1));
                mock_ieee_clear_data();
            }
        }
    }
}
END_TEST

// Deferred peer lookup may reuse TX after the selected source has already bound it.
START_TEST(test_deferred_peer_open_preserves_selected_relative_response) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int peer = IEEE_FIRST_DEVICE
        + (device - IEEE_FIRST_DEVICE + 1) % IEEE_DEVICE_COUNT;
    const unsigned int drive = loop_drive((int) coordinates);
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const uint8_t payload[] = {1, 2, 3, 4};
    const uint8_t peer_payload[] = {0xa1, 0xa2, 0xa3, 0xa4};
    mount_native_relative_records(device, drive, type, "deferred-source.img",
        TEST_COMMAND_RECORD_LENGTH, payload, sizeof(payload));
    mount_native_relative_records(peer, drive, type, "deferred-peer.img",
        TEST_COMMAND_RECORD_LENGTH, peer_payload, sizeof(peer_payload));
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    const char* const filenames[] = {"MISSING", "BASIC,L"};
    for (size_t filename = 0; filename < count_of(filenames); ++filename) {
        relative_position(device, IEEE_CHANNEL_REL, 1, 1);
        enqueue_command(IEEE_CMD_LISTEN(peer), IEEE_CMD_OPEN(IEEE_CHANNEL_REL + 1));
        enqueue_filename_for_drive(drive, filenames[filename]);
        mock_ieee_enqueue_rx(true, IEEE_CMD_UNLISTEN);
        mock_ieee_enqueue_rx(true, IEEE_CMD_TALK(device));
        mock_ieee_enqueue_rx(true, IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
        mock_ieee_enqueue_atn_end();
        ieee_drive_task();
        ck_assert_uint_eq(mock_ieee_data_count(), TEST_COMMAND_RECORD_LENGTH);
        ck_assert_uint_eq(mock_ieee_data_byte(0), payload[0]);
        ck_assert_uint_eq(mock_ieee_data_byte(1), payload[1]);
        ck_assert(!mock_ieee_data_eoi(0));
        ck_assert(mock_ieee_data_eoi(1));
        mock_ieee_clear_data();
        relative_talk(device, IEEE_CHANNEL_REL);
        ck_assert_uint_eq(mock_ieee_data_count(), TEST_COMMAND_RECORD_LENGTH);
        ck_assert_uint_eq(mock_ieee_data_byte(0), payload[2]);
        ck_assert_uint_eq(mock_ieee_data_byte(1), payload[3]);
        ck_assert(!mock_ieee_data_eoi(0));
        ck_assert(mock_ieee_data_eoi(1));
        mock_ieee_clear_data();
    }
}
END_TEST

// Native receive saturation and CMDSET's 58-byte limit are distinct contracts.
START_TEST(test_command_length_boundaries_and_recovery) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const uint8_t payload[] = {1, 2, 3, 4, 5, 6};
    mount_native_relative_records(device, drive, type, "command-limits.img",
        TEST_COMMAND_RECORD_LENGTH, payload, sizeof(payload));
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    const uint8_t entries[] = {
        IEEE_CMD_OPEN(IEEE_CHANNEL_COMMAND), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND),
    };
    const size_t native_limit = 58;
    const size_t receive_capacity = type == diskimage_type_d80 ? 59 : 41;
    const size_t lengths[] = {40, 41, 42, 57, 58, 59, 60};
    for (size_t entry = 0; entry < count_of(entries); ++entry) {
        for (size_t test = 0; test < count_of(lengths); ++test) {
            const size_t length = lengths[test];
            for (size_t trailing = 0; trailing <= 3; ++trailing) {
                relative_position(device, IEEE_CHANNEL_REL, 1, 1);
                enqueue_command(IEEE_CMD_LISTEN(device), entries[entry]);
                for (size_t index = 0; index < length; ++index) {
                    const uint8_t position[] = {'P', IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL), 2, 0, 1};
                    const uint8_t byte = trailing != 0 && index == length - trailing
                        ? '\r' : index < sizeof(position) ? position[index] : 'X';
                    mock_ieee_enqueue_rx_eoi(false, byte, index + 1 == length);
                    if (index % TEST_COMMAND_RECORD_LENGTH == 0) ieee_drive_task();
                }
                enqueue_command(IEEE_CMD_UNLISTEN);
                ieee_drive_task();
                relative_talk(device, IEEE_CHANNEL_COMMAND);
                size_t retained = length < receive_capacity ? length : receive_capacity;
                if (trailing != 0 && length - trailing == retained - 1) --retained;
                else if (trailing != 0 && length - trailing == retained - 2) retained -= 2;
                const bool overlong = retained > native_limit;
                char expected[IEEE_STATUS_LINE_CAPACITY];
                snprintf(expected, sizeof(expected), "%s%s\r",
                    overlong ? "32,SYNTAX ERROR,00,00" : "00, OK,00,00",
                    type == diskimage_type_d80 ? (drive ? ",1" : ",0") : "");
                assert_status(expected);
                mock_ieee_clear_status();
                relative_talk(device, IEEE_CHANNEL_REL);
                ck_assert_uint_eq(mock_ieee_data_count(), TEST_COMMAND_RECORD_LENGTH);
                ck_assert_uint_eq(mock_ieee_data_byte(0), payload[overlong ? 0 : 2]);
                ck_assert_uint_eq(mock_ieee_data_byte(1), payload[overlong ? 1 : 3]);
                ck_assert(!mock_ieee_data_eoi(0));
                ck_assert(mock_ieee_data_eoi(1));
                mock_ieee_clear_data();
                relative_position(device, IEEE_CHANNEL_REL, 3, 1);
                relative_talk(device, IEEE_CHANNEL_REL);
                ck_assert_uint_eq(mock_ieee_data_byte(0), payload[4]);
                mock_ieee_clear_data();
            }
        }
    }
}
END_TEST

// CR-valued binary fields remain readable after CMDSET adjusts the parsing size.
START_TEST(test_binary_position_retains_cr_fields) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const uint8_t record_length = 13;
    uint8_t payload[3 * 13];
    for (size_t index = 0; index < sizeof(payload); ++index) payload[index] = (uint8_t) (index + 1);
    mount_native_relative_records(device, drive, type, "binary-cr.img",
        record_length, payload, sizeof(payload));
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    const uint8_t entries[] = {
        IEEE_CMD_OPEN(IEEE_CHANNEL_COMMAND), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND),
    };
    for (size_t entry = 0; entry < count_of(entries); ++entry) {
        const uint8_t commands[][5] = {
            {'P', IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL), 2, 0, '\r'},
            {'P', IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL), 2, '\r', 1},
            {'P', IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL), 2, '\r', '\r'},
        };
        for (size_t command = 0; command < count_of(commands); ++command) {
            enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), entries[entry]);
            for (size_t index = 0; index < sizeof(commands[command]); ++index)
                mock_ieee_enqueue_rx_eoi(false, commands[command][index],
                    index + 1 == sizeof(commands[command]));
            enqueue_command(IEEE_CMD_UNLISTEN);
            ieee_drive_task();
            relative_talk(device, IEEE_CHANNEL_COMMAND);
            char expected[IEEE_STATUS_LINE_CAPACITY];
            snprintf(expected, sizeof(expected), "%s%s\r",
                command == 0 ? "00, OK,00,00" : "50,RECORD NOT PRESENT,00,00",
                type == diskimage_type_d80 ? (drive ? ",1" : ",0") : "");
            assert_status(expected);
            mock_ieee_clear_status();
            if (command == 0) {
                relative_talk(device, IEEE_CHANNEL_REL);
                ck_assert_uint_eq(mock_ieee_data_count(), 1);
                ck_assert_uint_eq(mock_ieee_data_byte(0), payload[2 * record_length - 1]);
                ck_assert(mock_ieee_data_eoi(0));
                mock_ieee_clear_data();
            }
        }
    }
}
END_TEST

// A media-model replacement cannot change the shape of an already-read response.
START_TEST(test_partial_status_retains_format_across_model_change) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const uint8_t payload[] = {1, 2};
    mount_native_relative_records(device, drive, type, "old-model.img",
        TEST_COMMAND_RECORD_LENGTH, payload, sizeof(payload));
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    relative_position(device, IEEE_CHANNEL_REL + 1, 1, 1);
    relative_talk(device, IEEE_CHANNEL_COMMAND);
    char error[IEEE_STATUS_LINE_CAPACITY];
    snprintf(error, sizeof(error), "70,NO CHANNEL,00,00%s\r",
        type == diskimage_type_d80 ? (drive ? ",1" : ",0") : "");
    assert_status(error);
    const size_t prefix = strlen(error) - 1;
    mock_ieee_consume_status(prefix);
    const bool next_d80 = type == diskimage_type_d64;
    mount_image(device, drive, "new-model.img",
        next_d80 ? diskimage_test_make_d80() : diskimage_test_make_d64(),
        next_d80 ? DISKIMAGE_D80_SIZE : DISKIMAGE_D64_SIZE, true);
    relative_talk(device, IEEE_CHANNEL_COMMAND);
    assert_status("\r");
    mock_ieee_clear_status();
    relative_talk(device, IEEE_CHANNEL_COMMAND);
    char ok[IEEE_STATUS_LINE_CAPACITY];
    snprintf(ok, sizeof(ok), "00, OK,00,00%s\r", next_d80 ? (drive ? ",1" : ",0") : "");
    assert_status(ok);
}
END_TEST

// A selection accepted during TXS filling must keep its truncated suffix paused.
START_TEST(test_status_selection_interrupts_publication) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const uint8_t payload[] = {1, 2};
    mount_native_relative_records(device, drive, type, "status-publication.img",
        TEST_COMMAND_RECORD_LENGTH, payload, sizeof(payload));
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    char error[IEEE_STATUS_LINE_CAPACITY];
    snprintf(error, sizeof(error), "70,NO CHANNEL,00,00%s\r",
        type == diskimage_type_d80 ? (drive ? ",1" : ",0") : "");
    const size_t prefixes[] = {0, 3};
    for (size_t test = 0; test < count_of(prefixes); ++test) {
        const size_t prefix = prefixes[test];
        for (size_t discarded = 1; discarded <= strlen(error) - prefix; ++discarded) {
            relative_position(device, IEEE_CHANNEL_REL + 1, 1, 1);
            if (prefix != 0) {
                relative_talk(device, IEEE_CHANNEL_COMMAND);
                mock_ieee_consume_status(prefix);
            }
            mock_ieee_repeat_status_after(discarded);
            relative_talk(device, IEEE_CHANNEL_COMMAND);
            ck_assert_uint_eq(mock_ieee_status_count(), strlen(error) - prefix - discarded);
            ck_assert(mock_ieee_tx_paused());
            ieee_drive_task();
            assert_status(error + prefix);
            mock_ieee_clear_status();
            relative_talk(device, IEEE_CHANNEL_COMMAND);
            assert_status(type == diskimage_type_d80
                ? (drive ? "00, OK,00,00,1\r" : "00, OK,00,00,0\r")
                : "00, OK,00,00\r");
            mock_ieee_clear_status();
        }
    }
}
END_TEST

// Status selection/transport discard cannot acknowledge the terminal CR.
START_TEST(test_status_progress_survives_reselection) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const uint8_t payload[] = {1, 2};
    mount_native_relative_records(device, drive, type, "status-progress.img",
        TEST_COMMAND_RECORD_LENGTH, payload, sizeof(payload));
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    char error[IEEE_STATUS_LINE_CAPACITY], ok[IEEE_STATUS_LINE_CAPACITY];
    const char* const suffix = type == diskimage_type_d80 ? (drive ? ",1" : ",0") : "";
    snprintf(error, sizeof(error), "70,NO CHANNEL,00,00%s\r", suffix);
    snprintf(ok, sizeof(ok), "00, OK,00,00%s\r", suffix);
    const size_t prefixes[] = {0, 3, strlen(error) - 1};
    // Repetition also crosses the modulo-256 status GET counter boundary.
    const size_t repetitions = 12;
    for (size_t iteration = 0; iteration < repetitions; ++iteration) {
        const size_t prefix = iteration % count_of(prefixes);
        relative_position(device, IEEE_CHANNEL_REL + 1, 1, 1);
        relative_talk(device, IEEE_CHANNEL_COMMAND);
        assert_status(error);
        mock_ieee_consume_status(prefixes[prefix]);
        mock_ieee_discard_status();
        relative_talk(device, IEEE_CHANNEL_COMMAND);
        assert_status(error + prefixes[prefix]);
        mock_ieee_clear_status();
        relative_talk(device, IEEE_CHANNEL_COMMAND);
        assert_status(ok);
        mock_ieee_clear_status();
    }

    // Finishing an older response must not acknowledge a newer error generation.
    relative_position(device, IEEE_CHANNEL_REL + 1, 1, 1);
    relative_talk(device, IEEE_CHANNEL_COMMAND);
    assert_status(error);
    mock_ieee_consume_status(3);
    relative_position(device, IEEE_CHANNEL_REL, 99, 1);
    mock_ieee_clear_status();
    relative_talk(device, IEEE_CHANNEL_COMMAND);
    char missing[IEEE_STATUS_LINE_CAPACITY];
    snprintf(missing, sizeof(missing), "50,RECORD NOT PRESENT,00,00%s\r", suffix);
    assert_status(missing);

    // Remount discards transport, not the already fetched status prefix.
    mock_ieee_consume_status(3);
    ck_assert(ieee_drive_mount(device_slot(device, drive), "status-progress.img"));
    relative_talk(device, IEEE_CHANNEL_COMMAND);
    assert_status(missing + 3);
    mock_ieee_clear_status();
}
END_TEST

// Retains unconsumed bytes across retalk, status, another channel, and FIFO discard.
START_TEST(test_relative_continuation_tracks_prefetch_not_queueing) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const uint8_t reclen = 129;
    uint8_t payload[TEST_REL_RECORD_COUNT * 129] = {0};
    for (size_t i = reclen; i < sizeof(payload); ++i) payload[i] = (uint8_t) (i | 1);
    mount_native_relative_records(device, drive, type, "continuation.img",
        reclen, payload, sizeof(payload));
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    // Merely queueing the empty record must never advance to record two.
    relative_talk(device, IEEE_CHANNEL_REL);
    relative_talk(device, IEEE_CHANNEL_REL);
    ck_assert_uint_eq(mock_ieee_data_count(), 1);
    ck_assert_uint_eq(mock_ieee_data_byte(0), 0);
    ck_assert(mock_ieee_data_eoi(0));

    // OPEN flushes hardware data, but the original channel retains its NUL.
    const unsigned int peer_channel = IEEE_CHANNEL_REL + 1;
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(peer_channel));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    relative_talk(device, peer_channel);
    mock_ieee_clear_data();
    relative_talk(device, peer_channel);
    mock_ieee_consume_data(5);
    relative_talk(device, IEEE_CHANNEL_REL);
    ck_assert_uint_eq(mock_ieee_data_count(), 1);
    ck_assert_uint_eq(mock_ieee_data_byte(0), 0);
    ck_assert(mock_ieee_data_eoi(0));

    // Real prefetch advances the original channel, including an interrupted suffix.
    mock_ieee_clear_data();
    relative_talk(device, IEEE_CHANNEL_REL);
    mock_ieee_consume_data(7);
    relative_talk(device, IEEE_CHANNEL_COMMAND);
    mock_ieee_clear_status();
    relative_talk(device, IEEE_CHANNEL_REL);
    ck_assert_uint_eq(mock_ieee_data_count(), reclen - 7);
    for (size_t i = 0; i < reclen - 7; ++i) {
        ck_assert_uint_eq(mock_ieee_data_byte(i), payload[reclen + 7 + i]);
        ck_assert_int_eq(mock_ieee_data_eoi(i), i + 1 == reclen - 7);
    }
    relative_talk(device, peer_channel);
    ck_assert_uint_eq(mock_ieee_data_count(), reclen - 5);
    for (size_t i = 0; i < reclen - 5; ++i)
        ck_assert_uint_eq(mock_ieee_data_byte(i), payload[reclen + 5 + i]);

    // P deliberately replaces only its channel's parked response.
    relative_position(device, IEEE_CHANNEL_REL, 1, 1);
    relative_talk(device, IEEE_CHANNEL_REL);
    ck_assert_uint_eq(mock_ieee_data_count(), 1);
    ck_assert_uint_eq(mock_ieee_data_byte(0), 0);

    // Modulo-counter wrap is unambiguous because only one bounded record is queued.
    for (unsigned int lap = 0; lap < 3; ++lap) {
        relative_position(device, IEEE_CHANNEL_REL, 2, 1);
        relative_talk(device, IEEE_CHANNEL_REL);
        mock_ieee_consume_data(reclen - 1);
        relative_talk(device, peer_channel);
        relative_talk(device, IEEE_CHANNEL_REL);
        ck_assert_uint_eq(mock_ieee_data_count(), 1);
        ck_assert_uint_eq(mock_ieee_data_byte(0), payload[2 * reclen - 1]);
        ck_assert(mock_ieee_data_eoi(0));
        mock_ieee_clear_data();
    }

    // GET of the empty record's EOI positions subsequent LISTEN at record two.
    relative_position(device, IEEE_CHANNEL_REL, 1, 1);
    relative_talk(device, IEEE_CHANNEL_REL);
    mock_ieee_clear_data();
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    mock_ieee_enqueue_rx(false, 'Z');
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    relative_position(device, IEEE_CHANNEL_REL, 2, 1);
    relative_talk(device, IEEE_CHANNEL_REL);
    ck_assert_uint_eq(mock_ieee_data_count(), 1);
    ck_assert_uint_eq(mock_ieee_data_byte(0), 'Z');
    ck_assert(mock_ieee_data_eoi(0));

    // CLOSE and reset invalidate pending data instead of resurrecting old batches.
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device),
        IEEE_CMD_CLOSE(IEEE_CHANNEL_REL), IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    relative_talk(device, IEEE_CHANNEL_REL);
    ck_assert_uint_eq(mock_ieee_data_count(), 0);
    // The supported MCU reset invalidates a live, partially synchronized batch
    // before the fabric resets its nonzero progress counter.
    relative_talk(device, peer_channel);
    mock_ieee_consume_data(7);
    ieee_drive_task();
    ieee_drive_reset();
    relative_talk(device, peer_channel);
    ck_assert_uint_eq(mock_ieee_data_count(), 0);
}
END_TEST

// Read or write EOI saves the successor before a peer changes its backing data.
START_TEST(test_relative_eoi_retains_prepared_next_record) {
    const bool write_eoi = _i % 2 != 0;
    const unsigned int model_coordinates = (unsigned int) _i / 2;
    const unsigned int coordinates = model_coordinates / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const diskimage_type_t type = native_image_types[model_coordinates % count_of(native_image_types)];
    const uint8_t reclen = 129;
    const unsigned int peer_channel = IEEE_CHANNEL_REL + 1;
    uint8_t payload[TEST_REL_RECORD_COUNT * 129] = {0};
    payload[reclen] = 0xa5;
    mount_native_relative_records(device, drive, type, "eoi-prefetch.img",
        reclen, payload, sizeof(payload));
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(peer_channel));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    // Both RDREL and WRTREL reach RD40/NXTREC and save the successor's CHNDAT.
    if (write_eoi) {
        enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
        mock_ieee_enqueue_rx_eoi(false, 0x80, true);
        ieee_drive_task();
    } else {
        relative_talk(device, IEEE_CHANNEL_REL);
        mock_ieee_clear_data();
        ieee_drive_task();
    }

    // Replace record two through the peer after its original data was prepared.
    relative_position(device, peer_channel, 2, 1);
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(peer_channel));
    mock_ieee_enqueue_rx(false, 'Z');
    mock_ieee_enqueue_rx(false, 'Y');
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    relative_talk(device, IEEE_CHANNEL_REL);
    ck_assert_uint_eq(mock_ieee_data_count(), 1);
    ck_assert_uint_eq(mock_ieee_data_byte(0), 0xa5);
    ck_assert(mock_ieee_data_eoi(0));

    // Explicit P replaces that prepared byte and exposes the actual peer write.
    relative_position(device, IEEE_CHANNEL_REL, 2, 1);
    relative_talk(device, IEEE_CHANNEL_REL);
    ck_assert_uint_eq(mock_ieee_data_count(), 2);
    ck_assert_uint_eq(mock_ieee_data_byte(0), 'Z');
    ck_assert(!mock_ieee_data_eoi(0));
    ck_assert_uint_eq(mock_ieee_data_byte(1), 'Y');
    ck_assert(mock_ieee_data_eoi(1));
}
END_TEST

// CLOSE 15 releases every data channel on its unit, not a selected peer source.
START_TEST(test_close_command_channel_releases_unit_channels) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const unsigned int peer = IEEE_FIRST_DEVICE
        + (device - IEEE_FIRST_DEVICE + 1) % IEEE_DEVICE_COUNT;
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const unsigned int second_channel = IEEE_CHANNEL_REL + 1;
    const unsigned int sequential_channel = second_channel + 1;
    const uint8_t payload[] = {0xa0, 0xa1, 0xa5, 0, 0xcc, 0xdd};
    const char* const filename = "close-all.img";
    mount_native_relative_records(device, drive, type, filename, 2, payload, sizeof(payload));
    ck_assert(ieee_drive_mount(device_slot(peer, drive), filename));
    if (type == diskimage_type_d80) mount_d80(device, drive ^ 1u, "close-seq.d80", true);
    else mount_d64(device, drive ^ 1u, "close-seq.d64", true);

    // Allocate two REL channels and an ordinary channel across the unit's drives.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(second_channel));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(sequential_channel));
    enqueue_filename_for_drive(drive ^ 1u, "BASIC");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    enqueue_command(IEEE_CMD_LISTEN(peer), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    mock_ieee_enqueue_rx(false, 'Z');
    mock_ieee_enqueue_rx_eoi(false, 'Y', true);
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    // Keep the peer's selected response partly unread across CLOSE 15.
    relative_position(device, sequential_channel + 1, 1, 1);
    relative_talk(peer, IEEE_CHANNEL_REL);
    ck_assert_uint_eq(mock_ieee_data_count(), 2);
    mock_ieee_consume_data(1);
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_CLOSE(IEEE_CHANNEL_COMMAND),
        IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    ck_assert_uint_eq(mock_ieee_data_count(), 1);
    ck_assert_uint_eq(mock_ieee_data_byte(0), 'Y');
    ck_assert(mock_ieee_data_eoi(0));
    mock_ieee_clear_data();
    relative_talk(device, IEEE_CHANNEL_COMMAND);
    char completed_status[IEEE_STATUS_LINE_CAPACITY];
    snprintf(completed_status, sizeof(completed_status), "00, OK,00,00%s\r",
        type == diskimage_type_d80 ? (drive == 0 ? ",0" : ",1") : "");
    assert_status(completed_status);
    mock_ieee_clear_status();

    // Missing REL and SEQ allocations all report NO CHANNEL, not type mismatch.
    for (unsigned int channel = IEEE_CHANNEL_REL; channel <= sequential_channel; ++channel) {
        relative_position(device, channel, 1, 1);
        relative_talk(device, IEEE_CHANNEL_COMMAND);
        char expected[IEEE_STATUS_LINE_CAPACITY];
        snprintf(expected, sizeof(expected), "70,NO CHANNEL,00,00%s\r",
            type == diskimage_type_d80 ? (drive == 0 ? ",0" : ",1") : "");
        assert_status(expected);
        mock_ieee_clear_status();
        relative_talk(device, channel);
        ck_assert_uint_eq(mock_ieee_data_count(), 0);
    }

    // Channel reuse exposes the completed write, never a stale prepared successor.
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    relative_talk(device, IEEE_CHANNEL_REL);
    ck_assert_uint_eq(mock_ieee_data_count(), 2);
    ck_assert_uint_eq(mock_ieee_data_byte(0), 'Z');
    ck_assert(!mock_ieee_data_eoi(0));
    ck_assert_uint_eq(mock_ieee_data_byte(1), 'Y');
    ck_assert(mock_ieee_data_eoi(1));
}
END_TEST

// CLSALL selects the last directory-updating REL drive, not the old status drive.
START_TEST(test_close_command_channel_selects_mixed_drive) {
    const unsigned int pattern = (unsigned int) _i % close_write_pattern_count;
    const unsigned int model_coordinates = (unsigned int) _i / close_write_pattern_count;
    const unsigned int coordinates = model_coordinates / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int lower_drive = loop_drive((int) coordinates);
    const unsigned int upper_drive = lower_drive ^ 1u;
    const diskimage_type_t type = native_image_types[model_coordinates % count_of(native_image_types)];
    const unsigned int lower = IEEE_CHANNEL_REL, upper = lower + 1;
    const uint8_t payload[] = {0xa0, 0xa1, 0xa5, 0, 0xcc, 0xdd};
    const bool lower_written = pattern == close_lower_written
        || pattern == close_both_written || pattern == close_lower_reopened;
    const bool upper_written = pattern == close_upper_written || pattern == close_both_written
        || pattern == close_lower_reopened || pattern == close_lower_rearmed;
    mount_native_relative_records(device, lower_drive, type, "close-lower.img",
        TEST_COMMAND_RECORD_LENGTH, payload, sizeof(payload));
    mount_native_relative_records(device, upper_drive, type, "close-upper.img",
        TEST_COMMAND_RECORD_LENGTH, payload, sizeof(payload));

    // A lower SA closes last, regardless of drive number, OPEN order or write order.
    for (unsigned int channel = lower; channel <= upper; ++channel) {
        enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(channel));
        enqueue_filename_for_drive(channel == lower ? lower_drive : upper_drive, "BASIC,L");
        enqueue_command(IEEE_CMD_UNLISTEN);
        ieee_drive_task();
    }
    for (unsigned int channel = lower; channel <= upper; ++channel) {
        if (!(channel == lower ? lower_written : upper_written)) continue;
        enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(channel));
        mock_ieee_enqueue_rx_eoi(false, (uint8_t) channel, true);
        enqueue_command(IEEE_CMD_UNLISTEN);
        ieee_drive_task();
    }
    for (unsigned int channel = lower; channel <= upper; ++channel)
        relative_position(device, channel, 2, 1);
    if (pattern == close_lower_reopened) {
        // Reusing a written allocation must clear its previous native DYFILE state.
        enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_CLOSE(lower), IEEE_CMD_UNLISTEN);
        ieee_drive_task();
        relative_talk(device, IEEE_CHANNEL_COMMAND);
        char closed_status[IEEE_STATUS_LINE_CAPACITY];
        snprintf(closed_status, sizeof(closed_status), "00, OK,00,00%s\r",
            type == diskimage_type_d80 ? (lower_drive == 0 ? ",0" : ",1") : "");
        assert_status(closed_status);
        mock_ieee_clear_status();
        enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(lower));
        enqueue_filename_for_drive(lower_drive, "BASIC,L");
        enqueue_command(IEEE_CMD_UNLISTEN);
        ieee_drive_task();
    } else if (pattern == close_lower_rearmed) {
        // Merely arming an unwritten channel is not a native RELPUT/SDIRTY.
        enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(lower), IEEE_CMD_UNLISTEN);
        ieee_drive_task();
    }
    const unsigned int selected_drive = type == diskimage_type_d64
        || (lower_written && pattern != close_lower_reopened) ? lower_drive : upper_drive;
    const unsigned int prior_drive = pattern == close_no_writes ? selected_drive : selected_drive ^ 1u;

    // Select a conflicting status drive without allocating another data channel.
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(upper + 1));
    enqueue_filename_for_drive(prior_drive, "MISSING,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    relative_talk(device, IEEE_CHANNEL_COMMAND);
    char expected[IEEE_STATUS_LINE_CAPACITY];
    snprintf(expected, sizeof(expected), "62, FILE NOT FOUND,00,00%s\r",
        type == diskimage_type_d80 ? (prior_drive == 0 ? ",0" : ",1") : "");
    assert_status(expected);
    mock_ieee_clear_status();

    // An unwritten DOS 2.7 close retains DRVNUM, including an already empty unit.
    for (unsigned int repetition = 0; repetition < 2; ++repetition) {
        enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device),
            IEEE_CMD_CLOSE(IEEE_CHANNEL_COMMAND), IEEE_CMD_UNLISTEN);
        ieee_drive_task();
        relative_talk(device, IEEE_CHANNEL_COMMAND);
        snprintf(expected, sizeof(expected), "00, OK,00,00%s\r",
            type == diskimage_type_d80 ? (selected_drive == 0 ? ",0" : ",1") : "");
        assert_status(expected);
        mock_ieee_clear_status();
    }
}
END_TEST

// Wrapped native reads retain their physical cursor through consecutive writes.
START_TEST(test_relative_wrapped_read_write_continuation) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const uint8_t reclen = 129;
    const uint32_t first_write = DISKIMAGE_SECTOR_PAYLOAD_SIZE + reclen;
    const uint32_t second_write = first_write + reclen;
    uint8_t payload[TEST_REL_WRAPPED_RECORD_COUNT * 129] = {0};
    payload[0] = 0xa5;
    payload[reclen] = 0x5a;
    payload[DISKIMAGE_SECTOR_PAYLOAD_SIZE] = 0x7b;
    payload[2 * reclen] = 0xcc;
    payload[second_write] = 0x66;
    const char* const filename = "wrapped-write.img";
    mount_native_relative_records(device, drive, type, filename,
        reclen, payload, sizeof(payload));
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    // P into the zero suffix reaches EOI at physical payload offset 254.
    relative_position(device, IEEE_CHANNEL_REL, 1, reclen);
    relative_talk(device, IEEE_CHANNEL_REL);
    ck_assert_uint_eq(mock_ieee_data_count(), DISKIMAGE_SECTOR_PAYLOAD_SIZE - reclen + 2);
    ck_assert_uint_eq(mock_ieee_data_byte(mock_ieee_data_count() - 1), 0x7b);
    ck_assert(mock_ieee_data_eoi(mock_ieee_data_count() - 1));
    mock_ieee_clear_data();

    // Native NXTREC selects 383, then WR40 advances physically to 512.
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    mock_ieee_enqueue_rx(false, 'W');
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    mock_ieee_enqueue_rx(false, 'X');
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    payload[first_write] = 'W';
    memset(payload + first_write + 1, 0, reclen - 1);
    payload[second_write] = 'X';
    memset(payload + second_write + 1, 0, reclen - 1);

    // Compare the entire backing payload, including protected earlier records.
    char path[SD_PATH_MAX];
    sd_make_path(path, SD_DIR_DISKS, filename);
    FILE* file = sd_open(path, "r");
    ck_assert_ptr_nonnull(file);
    const unsigned int track = type == diskimage_type_d80 ? 38 : 17;
    for (size_t used = 0; used < sizeof(payload);) {
        const size_t count = MIN(sizeof(payload) - used, DISKIMAGE_SECTOR_PAYLOAD_SIZE);
        uint32_t offset;
        ck_assert(diskimage_sector_offset(type, track,
            used / DISKIMAGE_SECTOR_PAYLOAD_SIZE, &offset));
        ck_assert_int_eq(fseek(file, offset + DISKIMAGE_SECTOR_LINK_SIZE, SEEK_SET), 0);
        uint8_t actual[DISKIMAGE_SECTOR_PAYLOAD_SIZE];
        ck_assert_uint_eq(fread(actual, 1, count, file), count);
        ck_assert_mem_eq(actual, payload + used, count);
        used += count;
    }
    ck_assert_int_eq(fclose(file), 0);
}
END_TEST

// Checks P aliases, the first invalid offset, and missing-record error precedence.
START_TEST(test_relative_position_native_boundaries) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const uint8_t reclen = 129;
    uint8_t payload[TEST_REL_RECORD_COUNT * 129];
    memset(payload, 0xa5, sizeof(payload));
    mount_native_relative_records(device, drive, type, "position.img",
        reclen, payload, sizeof(payload));
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    // Native R30/R40 normalize 0/1 and reject the first invalid encoded offset.
    const uint8_t offsets[] = {0, 1, reclen, reclen + 1, UINT8_MAX};
    for (size_t variant = 0; variant < count_of(offsets); ++variant) {
        const uint8_t offset = offsets[variant];
        relative_position(device, IEEE_CHANNEL_REL, 1, offset);
        relative_talk(device, IEEE_CHANNEL_COMMAND);
        char expected[IEEE_STATUS_LINE_CAPACITY];
        const bool overflow = offset > reclen;
        snprintf(expected, sizeof(expected), "%s,00,00%s\r",
            overflow ? "51,OVERFLOW IN RECORD" : "00, OK",
            type == diskimage_type_d80 ? (drive == 0 ? ",0" : ",1") : "");
        assert_status(expected);
        mock_ieee_clear_status();
        relative_talk(device, IEEE_CHANNEL_REL);
        ck_assert_uint_eq(mock_ieee_data_count(), offset == reclen ? 1 : reclen);
        ck_assert_uint_eq(mock_ieee_data_byte(0), 0xa5);
    }
    relative_position(device, IEEE_CHANNEL_REL, TEST_REL_RECORD_COUNT + 1, reclen + 1);
    relative_talk(device, IEEE_CHANNEL_COMMAND);
    char expected[IEEE_STATUS_LINE_CAPACITY];
    snprintf(expected, sizeof(expected), "50,RECORD NOT PRESENT,00,00%s\r",
        type == diskimage_type_d80 ? (drive == 0 ? ",0" : ",1") : "");
    assert_status(expected);
}
END_TEST

// Native zero-suffix P compares modular buffer pointers, including crossed buffers.
START_TEST(test_relative_position_zero_suffix_uses_physical_last_character) {
    const unsigned int coordinates = (unsigned int) _i / count_of(native_image_types);
    const unsigned int device = loop_device((int) coordinates);
    const unsigned int drive = loop_drive((int) coordinates);
    const diskimage_type_t type = native_image_types[_i % count_of(native_image_types)];
    const uint8_t reclen = 129;
    uint8_t payload[TEST_REL_RECORD_COUNT * 129] = {0};
    payload[0] = 0xa5;
    payload[reclen] = 0x5a;
    payload[DISKIMAGE_SECTOR_PAYLOAD_SIZE] = 0x7b;
    mount_native_relative_records(device, drive, type, "suffix.img",
        reclen, payload, sizeof(payload));
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();

    // LSTCHR=$02 lies behind P's $82, so RDREL reaches $02 in the next sector.
    relative_position(device, IEEE_CHANNEL_REL, 1, reclen);
    relative_talk(device, IEEE_CHANNEL_REL);
    const size_t count = DISKIMAGE_SECTOR_PAYLOAD_SIZE - (reclen - 1) + 1;
    ck_assert_uint_eq(mock_ieee_data_count(), count);
    for (size_t index = 0; index < count; ++index) {
        ck_assert_uint_eq(mock_ieee_data_byte(index), payload[reclen - 1 + index]);
        ck_assert_int_eq(mock_ieee_data_eoi(index), index + 1 == count);
    }

    // With the selected pointer already in the successor buffer, SETLST's
    // empty search falls back to that selected pointer instead of searching back.
    payload[DISKIMAGE_SECTOR_PAYLOAD_SIZE] = 0;
    mount_native_relative_records(device, drive, type, "crossed-suffix.img",
        reclen, payload, sizeof(payload));
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    relative_position(device, IEEE_CHANNEL_REL, 2, 126);
    relative_talk(device, IEEE_CHANNEL_REL);
    ck_assert_uint_eq(mock_ieee_data_count(), 1);
    ck_assert_uint_eq(mock_ieee_data_byte(0), 0);
    ck_assert(mock_ieee_data_eoi(0));
}
END_TEST

// Keeps transparent empty records isolated to the existing flat HDD extension.
START_TEST(test_hdd_empty_relative_record_remains_transparent) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);
    const size_t record_length = 129;
    const uint8_t marker = 0x7b;

    // Keep the first flat record empty and make every byte in record two visible.
    uint8_t* image = calloc(2, record_length);
    ck_assert_ptr_nonnull(image);
    memset(image + record_length, marker, record_length);
    mount_image(device, drive, "empty.hdd", image, 2 * record_length, true);
    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "OS9 DRIVE A,L");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device),
        IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    ieee_drive_task();

    // The extension's legacy skip behavior must not acquire native NUL responses.
    ck_assert_uint_eq(mock_ieee_data_count(), record_length);
    for (size_t index = 0; index < record_length; ++index) {
        ck_assert_uint_eq(mock_ieee_data_byte(index), marker);
        ck_assert_int_eq(mock_ieee_data_eoi(index), index + 1 == record_length);
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
    ieee_drive_task();
    enqueue_name("DATA,L");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(source_device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    mock_ieee_enqueue_rx(false, 'P');
    mock_ieee_enqueue_rx(false, IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    mock_ieee_enqueue_rx(false, 2);
    mock_ieee_enqueue_rx_eoi(false, 0, true);
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
    const unsigned int device = loop_device(_i / count_of(native_image_types));
    const unsigned int drive = loop_drive(_i / count_of(native_image_types));
    const bool d80 = native_image_types[_i % count_of(native_image_types)] == diskimage_type_d80;
    const uint8_t record_length = 129;
    uint8_t payload[TEST_REL_RECORD_COUNT * DISKIMAGE_REL_MAX_RECORD_LENGTH];
    memset(payload, 0xa0, sizeof(payload));

    // Open a bounded native REL file with room for the overflow successor.
    mount_native_relative_records(device, drive,
        d80 ? diskimage_type_d80 : diskimage_type_d64, "overflow.img",
        record_length, payload, 2 * record_length);

    enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
    enqueue_filename_for_drive(drive, "BASIC,L");
    enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    ieee_drive_task();

    // Send one byte beyond the generated 129-byte record, draining mock RX in batches.
    for (unsigned int index = 0; index <= record_length; index++) {
        mock_ieee_enqueue_rx_eoi(false, (uint8_t) index, index == record_length);
        if (index % TEST_RX_DATA_BATCH_SIZE == TEST_RX_DATA_BATCH_SIZE - 1) ieee_drive_task();
    }
    ieee_drive_task();
    enqueue_command(IEEE_CMD_TALK(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_COMMAND));
    ieee_drive_task();

    // The excess byte must surface as DOS record-overflow status 51.
    char expected[IEEE_STATUS_LINE_CAPACITY];
    snprintf(expected, sizeof(expected), "51,OVERFLOW IN RECORD,00,00%s\r",
        d80 ? (drive == 0 ? ",0" : ",1") : "");
    assert_status(expected);
    mock_ieee_clear_status();

    // CMDERR retains the channel. P recovers its valid, completed prefix.
    relative_position(device, IEEE_CHANNEL_REL, 1, 1);
    relative_talk(device, IEEE_CHANNEL_REL);
    ck_assert_uint_eq(mock_ieee_data_count(), record_length);
    for (unsigned int index = 0; index < record_length; ++index) {
        ck_assert_uint_eq(mock_ieee_data_byte(index), index);
        ck_assert_int_eq(mock_ieee_data_eoi(index), index + 1 == record_length);
    }
    mock_ieee_clear_data();

    // A later valid write must work without a reopen, reset or stale overflow.
    enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
    mock_ieee_enqueue_rx_eoi(false, 0xe7, true);
    enqueue_command(IEEE_CMD_UNLISTEN);
    ieee_drive_task();
    relative_position(device, IEEE_CHANNEL_REL, 2, 1);
    relative_talk(device, IEEE_CHANNEL_REL);
    ck_assert_uint_eq(mock_ieee_data_count(), 1);
    ck_assert_uint_eq(mock_ieee_data_byte(0), 0xe7);
    ck_assert(mock_ieee_data_eoi(0));
}
END_TEST

// HDD aliases must never open or modify the peer of an empty requested drive.
START_TEST(test_hdd_aliases_preserve_drive_isolation) {
    const unsigned int device = loop_device(_i);
    const unsigned int drive = loop_drive(_i);
    const size_t record_length = 129, image_size = 2 * record_length;
    const uint8_t marker = 0x5a, write_marker = 0xe7;
    uint8_t* const image = malloc(image_size);
    ck_assert_ptr_nonnull(image);
    memset(image, marker, image_size);
    mount_image(device, drive ^ 1u, "peer.hdd", image, image_size, true);
    static const char* const aliases[] = {
        "OS9 DRIVE A,L", "OS? DRIVE A,L", "OS9 DRI?E A,L",
        "*,L", "?*,L", "os? drive a,L",
    };
    for (size_t variant = 0; variant < count_of(aliases); ++variant) {
        // A failed OPEN must also discard a previous REL channel on this secondary.
        char filename[32];
        snprintf(filename, sizeof(filename), "%u:%s", drive, aliases[variant]);
        enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
        enqueue_name(filename);
        enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device),
            IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
        ieee_drive_task();
        ck_assert_uint_eq(mock_ieee_data_count(), 0);
        relative_talk(device, IEEE_CHANNEL_COMMAND);
        char expected[IEEE_STATUS_LINE_CAPACITY];
        snprintf(expected, sizeof(expected), "62, FILE NOT FOUND,00,00,%u\r", drive);
        assert_status(expected);
        mock_ieee_clear_status();

        // LISTEN after the rejected OPEN must not gain write access to the peer.
        enqueue_command(IEEE_CMD_UNTALK, IEEE_CMD_LISTEN(device),
            IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
        mock_ieee_enqueue_rx(false, write_marker);
        enqueue_command(IEEE_CMD_UNLISTEN);
        ieee_drive_task();

        // The same alias still works when the mounted drive is explicitly selected.
        snprintf(filename, sizeof(filename), "%u:%s", drive ^ 1u, aliases[variant]);
        enqueue_command(IEEE_CMD_LISTEN(device), IEEE_CMD_OPEN(IEEE_CHANNEL_REL));
        enqueue_name(filename);
        enqueue_command(IEEE_CMD_UNLISTEN, IEEE_CMD_TALK(device),
            IEEE_CMD_SECONDARY(IEEE_CHANNEL_REL));
        ieee_drive_task();
        ck_assert_uint_eq(mock_ieee_data_count(), record_length);
        for (size_t index = 0; index < record_length; ++index) {
            ck_assert_uint_eq(mock_ieee_data_byte(index), marker);
            ck_assert_int_eq(mock_ieee_data_eoi(index), index + 1 == record_length);
        }
        mock_ieee_clear_data();
    }

    // Inspect the complete backing image, including records not read by the control.
    char path[SD_PATH_MAX];
    sd_make_path(path, SD_DIR_DISKS, "peer.hdd");
    FILE* const file = fopen(path, "rb");
    ck_assert_ptr_nonnull(file);
    uint8_t actual[258];
    ck_assert_uint_eq(fread(actual, 1, sizeof(actual), file), image_size);
    for (size_t index = 0; index < sizeof(actual); ++index)
        ck_assert_uint_eq(actual[index], marker);
    ck_assert_int_eq(fclose(file), 0);
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
    mock_ieee_enqueue_rx_eoi(false, 0, true);
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
    const int FOR_EACH_DEVICE_DRIVE_AND_MODEL =
        FOR_EACH_DEVICE_AND_DRIVE * count_of(native_image_types);

    // Share isolated mount/register fixtures across all protocol cases.
    Suite* suite = suite_create("ieee_drive");
    TCase* test_case = tcase_create("d64");
    tcase_add_checked_fixture(test_case, setup, teardown);
    tcase_add_test(test_case, test_mock_ctrl_readback);
    tcase_add_test(test_case, test_mock_rx_eoi_readback);
    tcase_add_test(test_case, test_mock_ctrl_flush_readback);
    tcase_add_test(test_case, test_shared_format_constants);
    tcase_add_test(test_case, test_mount_path_boundary);
    // Cover sequential transfers, status, addressing, and unit isolation.
    tcase_add_loop_test(test_case, test_d64_mount_open_status_and_stream,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_native_open_matches_raw_filename_bytes,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_empty_drive_falls_back_to_native_peer,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_native_one_byte_file_reaches_eof_after_four_bytes,
                        0, count_of(native_image_types) * count_of(native_file_types));
    tcase_add_loop_test(test_case, test_native_two_byte_file_keeps_normal_eof,
                        0, count_of(native_image_types) * count_of(native_file_types));
    tcase_add_loop_test(test_case, test_native_one_byte_eof_survives_interleaved_relative_read,
                        0, count_of(native_image_types) * count_of(native_file_types)
                            * IEEE_DEVICE_COUNT);
    tcase_add_loop_test(test_case, test_replacing_mount_uses_new_image,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_read_past_eof_keeps_channel_not_ready,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
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
    tcase_add_loop_test(test_case, test_untalk_pauses_readable_sequential_channel,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_close_then_reopen_streams_file,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_test(test_case, test_unmount_clears_open_sequential_channel);
    tcase_add_test(test_case, test_unmount_clears_open_relative_channel);
    tcase_add_test(test_case, test_non_target_addresses_are_ignored);
    tcase_add_test(test_case, test_secondary_address_alias_selects_data_channel);
    tcase_add_loop_test(test_case, test_corrupt_sequential_chain_reports_read_error,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_initialize_command_clears_error_status,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_validate_command_clears_error_status,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_initialize_and_validate_commands_select_status_drive,
                        0, IEEE_DEVICE_COUNT);
    // Cover REL records, mutable/read-only media, and the HDD extension.
    tcase_add_loop_test(test_case, test_relative_file_position_and_read,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_command_open_positions_relative_records,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_relative_eoi_completes_each_record,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL * count_of(relative_eoi_lengths));
    tcase_add_loop_test(test_case, test_command_channel_errors_and_recovery,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_command_completion_requires_eoi,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_open_consumes_shared_command_input,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_open_input_requires_eoi,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_virtual_units_retain_independent_listeners,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_command_dispatch_waits_for_idle,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_pending_command_dispatch_after_unopened_talk,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_pending_command_dispatch_after_sequential_eof,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_pending_command_dispatch_after_talk_without_get,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_pending_command_dispatch_after_unopened_listen,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_deferred_peer_open_preserves_selected_relative_response,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_command_length_boundaries_and_recovery,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_status_progress_survives_reselection,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_status_selection_interrupts_publication,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_binary_position_retains_cr_fields,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_partial_status_retains_format_across_model_change,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_relative_position_within_record,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_relative_position_past_end_reports_50,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_relative_write_to_read_only_image_reports_26,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_relative_write_updates_existing_record,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_relative_record_lengths_stream_exactly,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_native_empty_relative_records_are_not_skipped,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_relative_continuation_tracks_prefetch_not_queueing,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_relative_eoi_retains_prepared_next_record,
                        0, 2 * FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_close_command_channel_releases_unit_channels,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_close_command_channel_selects_mixed_drive,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL * close_write_pattern_count);
    tcase_add_loop_test(test_case, test_relative_wrapped_read_write_continuation,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_relative_position_native_boundaries,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_relative_position_zero_suffix_uses_physical_last_character,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_hdd_empty_relative_record_remains_transparent,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_relative_channels_are_isolated_between_units,
                        0, FOR_EACH_DEVICE);
    tcase_add_loop_test(test_case, test_relative_write_past_record_reports_51,
                        0, FOR_EACH_DEVICE_DRIVE_AND_MODEL);
    tcase_add_loop_test(test_case, test_hdd_mount_position_and_read,
                        0, FOR_EACH_DEVICE_AND_DRIVE);
    tcase_add_loop_test(test_case, test_hdd_aliases_preserve_drive_isolation,
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
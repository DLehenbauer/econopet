// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"

#include "dos_test.h"

#include "diskimage_test.h"
#include "ieee/dos.h"
#include "ieee/ieee_protocol.h"

typedef struct {
    uint8_t* bytes;
    uint32_t size;
    unsigned int reads;
    unsigned int writes;
    unsigned int flushes;
    unsigned int fail_write;
    unsigned int fail_read;
    bool fail_flush;
} image_t;

static bool read_image(void* context, uint32_t offset, void* output, size_t length);

// Injects a partial sector write at a selected persistence boundary.
static bool write_image(void* const context, const uint32_t offset, const void* const input, const size_t length) {
    image_t* const image = context;
    if (offset > image->size || length > image->size - offset) {
        return false;
    }
    if (++image->writes == image->fail_write) {
        memcpy(image->bytes + offset, input, length / 2u);
        return false;
    }
    memcpy(image->bytes + offset, input, length);
    return true;
}

// Keeps flush failures observable until the test explicitly permits recovery.
static bool flush_image(void* const context) {
    image_t* const image = context;
    ++image->flushes;
    return !image->fail_flush;
}

// Resolves independently asserted fixture coordinates.
static uint8_t* sector_bytes(image_t* const image, const diskimage_type_t type, const unsigned int track,
                             const unsigned int sector) {
    uint32_t offset;
    ck_assert(diskimage_sector_offset(type, track, sector, &offset));
    return image->bytes + offset;
}

// Initializes native BAM ranges and reserves only format metadata, without DOS helpers.
static void mount_blank(dos_media_bank_t* const bank, image_t* const image, const unsigned int format) {
    const diskimage_type_t type = format == 0   ? diskimage_type_d64
                                  : format == 1 ? diskimage_type_d80
                                                : diskimage_type_d82;
    const unsigned int tracks = format == 0 ? 35u : format == 1 ? 77u : 154u;
    const unsigned int parts = format == 0 ? 1u : format == 1 ? 2u : 4u;
    *image = (image_t){.size = format == 0   ? DISKIMAGE_D64_SIZE
                               : format == 1 ? DISKIMAGE_D80_SIZE
                                             : DISKIMAGE_D82_SIZE};
    image->bytes = calloc(1, image->size);
    ck_assert_ptr_nonnull(image->bytes);
    const unsigned int directory = format == 0 ? 18u : 39u;
    uint8_t* const header = sector_bytes(image, type, directory, 0);
    header[0] = format == 0 ? 18 : 38;
    header[1] = format == 0 ? 1 : 0;
    header[2] = format == 0 ? 'A' : 'C';
    const unsigned int name = format == 0 ? 144u : 6u;
    memset(header + name, 0xa0, 27);
    memcpy(header + name, "M3 FIXTURE", 10);
    memcpy(header + name + 18u, "01", 2);
    header[name + 21u] = '2';
    header[name + 22u] = format == 0 ? 'A' : 'C';
    sector_bytes(image, type, directory, 1)[1] = 0xff;
    for (unsigned int part = 0; part < parts; ++part) {
        uint8_t* const bam = sector_bytes(image, type, format == 0 ? 18u : 38u, part * 3u);
        const unsigned int low = part * 50u + 1u;
        const unsigned int high = format == 0 ? 36u : (low + 50u < tracks + 1u ? low + 50u : tracks + 1u);
        if (format != 0) {
            bam[0] = part + 1u < parts ? 38 : 39;
            bam[1] = part + 1u < parts ? (part + 1u) * 3u : 1;
            bam[2] = 'C';
            bam[4] = low;
            bam[5] = high;
        }
        for (unsigned int track = low; track < high; ++track) {
            uint8_t* const entry = bam + (format == 0 ? track * 4u : 6u + (track - low) * 5u);
            unsigned int sectors;
            ck_assert(diskimage_track_sectors(type, track, &sectors));
            for (unsigned int sector = 0; sector < sectors; ++sector) {
                const bool reserved = (track == directory && sector < 2u) ||
                                      (format != 0 && track == 38u && sector % 3u == 0 && sector / 3u < parts);
                if (!reserved) {
                    ++entry[0];
                    entry[1u + sector / 8u] |= (uint8_t)(1u << (sector % 8u));
                }
            }
        }
    }
    dos_media_io_t io = {.flush = flush_image};
    ck_assert(diskimage_open(&io.image, read_image, image, image->size));
    io.image.write = write_image;
    ck_assert_int_eq(dos_media_set_slot(bank, 0, 0, &io), DOS_MEDIA_OK);
}

// Reads a sector from an independently constructed native-format fixture.
static bool read_image(void* const context, const uint32_t offset, void* const output, const size_t length) {
    image_t* const image = context;
    ++image->reads;
    if (image->reads == image->fail_read) {
        memset(output, 0xaa, length / 2u);
        return false;
    }
    if (offset > image->size || length > image->size - offset) {
        return false;
    }
    memcpy(output, image->bytes + offset, length);
    return true;
}

// Mounts each selected raw model, placing D82's data on the second side.
static void mount_image(dos_media_bank_t* const bank, image_t* const image, const unsigned int format) {
    *image = (image_t){0};
    image->bytes = format == 0 ? diskimage_test_make_d64() : diskimage_test_make_d80();
    image->reads = 0;
    image->size = format == 0 ? DISKIMAGE_D64_SIZE : DISKIMAGE_D80_SIZE;
    if (format == 2) {
        image->bytes = realloc(image->bytes, DISKIMAGE_D82_SIZE);
        ck_assert_ptr_nonnull(image->bytes);
        memset(image->bytes + image->size, 0, image->size);
        uint32_t directory, data;
        ck_assert(diskimage_sector_offset(diskimage_type_d80, 39, 1, &directory));
        ck_assert(diskimage_sector_offset(diskimage_type_d80, 38, 0, &data));
        memcpy(image->bytes + DISKIMAGE_D80_SIZE + data, image->bytes + data, DISKIMAGE_SECTOR_SIZE);
        image->bytes[directory + 3] += DISKIMAGE_D80_TRACK_COUNT;
        image->size = DISKIMAGE_D82_SIZE;
    }
    dos_media_io_t io = {0};
    ck_assert(diskimage_open(&io.image, read_image, image, image->size));
    ck_assert_int_eq(dos_media_set_slot(bank, 0, 0, &io), DOS_MEDIA_OK);
}

// Finishes cooperative DOS work with an explicit corruption stop bound.
static void finish_open(dos_unit_t* const unit, dos_media_bank_t* const bank) {
    unsigned int steps = 0;
    while (dos_work_pending(unit)) {
        ck_assert_uint_lt(steps++, 3u * (DISKIMAGE_D82_SIZE / DISKIMAGE_SECTOR_SIZE) + 128u);
        dos_work(unit, bank);
    }
}

// Opens a bounded native request through the same cooperative work entry point.
static uint8_t open_name(dos_unit_t* const unit, dos_media_bank_t* const bank, const uint8_t secondary,
                         const char* const name) {
    dos_open(unit, bank, 0, secondary, (const uint8_t*)name, strlen(name));
    finish_open(unit, bank);
    return dos_find_channel(unit, secondary);
}

// Consumes and independently checks every byte and final EOI, including native singleton EOF.
static void expect_bytes(dos_unit_t* const unit, dos_media_bank_t* const bank, const uint8_t channel,
                         const unsigned int count, const unsigned int first) {
    unsigned int consumed = 0;
    const unsigned int expected = count <= 1u ? 4u : count;
    bool eoi = false;
    while (!eoi) {
        uint8_t bytes[DOS_STAGE_CAPACITY], again[DOS_STAGE_CAPACITY];
        bool repeated_eoi;
        const unsigned int length = dos_stage(unit, bank, channel, bytes, sizeof(bytes), &eoi);
        ck_assert_uint_gt(length, 0);
        ck_assert_uint_eq(dos_stage(unit, bank, channel, again, sizeof(again), &repeated_eoi), length);
        ck_assert_mem_eq(bytes, again, length);
        ck_assert_int_eq(eoi, repeated_eoi);
        for (unsigned int index = 0; index < length; ++index) {
            const unsigned int offset = consumed + index;
            const uint8_t byte = count > 1u     ? first + offset
                                 : offset == 1u ? 0
                                 : offset == 2u ? 2
                                 : count == 0   ? '\r'
                                                : first;
            ck_assert_uint_eq(bytes[index], byte);
        }
        dos_get(unit, bank, channel, length);
        consumed += length;
        ck_assert_uint_le(consumed, expected);
    }
    ck_assert_uint_eq(consumed, expected);
    uint8_t byte;
    ck_assert_uint_eq(dos_stage(unit, bank, channel, &byte, 1, &eoi), 0);
}

// Read a bounded directory while checking preview reversibility and terminal EOI.
static unsigned int directory_bytes(dos_unit_t* const unit, dos_media_bank_t* const bank, const uint8_t channel,
                                    uint8_t* const output, const unsigned int capacity) {
    unsigned int total = 0;
    bool eoi = false;
    for (unsigned int attempts = 0; !eoi && attempts < 128u; ++attempts) {
        uint8_t bytes[DOS_STAGE_CAPACITY];
        const unsigned int count = dos_stage(unit, bank, channel, bytes, sizeof(bytes), &eoi);
        ck_assert_uint_le(total + count, capacity);
        memcpy(output + total, bytes, count);
        total += count;
        dos_get(unit, bank, channel, count);
    }
    ck_assert(eoi);
    return total;
}

// Publish independent ordinary-file sectors and their directory/BAM metadata.
static void fixture_file(image_t* const image, const unsigned int format, const unsigned int slot,
                         const char* const name, const uint8_t file_type, const unsigned int first,
                         const unsigned int count) {
    const diskimage_type_t type = format == 0   ? diskimage_type_d64
                                  : format == 1 ? diskimage_type_d80
                                                : diskimage_type_d82;
    const unsigned int directory = format == 0 ? 18u : 39u;
    const unsigned int blocks = count == 0 ? 1u : (count + 253u) / 254u;
    ck_assert_uint_le(blocks, 2);
    uint8_t* const entry = sector_bytes(image, type, directory, 1) + slot * 32u;
    entry[2] = 0x80u | file_type;
    entry[3] = 1;
    entry[4] = slot * 2u;
    memset(entry + 5, 0xa0, 16);
    memcpy(entry + 5, name, strlen(name));
    entry[30] = blocks;
    uint8_t* const bam = sector_bytes(image, type, format == 0 ? 18u : 38u, 0) + (format == 0 ? 4u : 6u);
    for (unsigned int block = 0; block < blocks; ++block) {
        const unsigned int sector = slot * 2u + block;
        uint8_t* const data = sector_bytes(image, type, 1, sector);
        const unsigned int remaining = count > block * 254u ? count - block * 254u : 1u;
        const unsigned int payload = remaining < 254u ? remaining : 254u;
        data[0] = block + 1u < blocks ? 1u : 0u;
        data[1] = block + 1u < blocks ? sector + 1u : payload + 1u;
        for (unsigned int offset = 0; offset < payload; ++offset) {
            data[2u + offset] = count == 0 ? '\r' : first + block * 254u + offset;
        }
        --bam[0];
        bam[1u + sector / 8u] &= (uint8_t)~(1u << (sector % 8u));
    }
}

// OP60 takes every record-length byte literally after the first CKTM selects L.
START_TEST(test_open_parse_binary_record_size) {
    const uint8_t size = (uint8_t)_i;
    for (unsigned int prefix = 0; prefix < 2; ++prefix) {
        uint8_t command[DOS_COMMAND_RECEIVE_CAPACITY] = {0};
        const char* const name = prefix ? "0:NAME,L," : "NAME,L,";
        const unsigned int length = (unsigned int)strlen(name);
        memcpy(command, name, length);
        command[length] = size;
        dos_open_work_t work = {0};
        uint8_t drive = 1;
        // CMDSET hides one trailing CR without erasing the binary parameter.
        const bool parsed = dos_open_parse(command, length + (size != '\r'), &drive, &work);
        ck_assert_int_eq(parsed, prefix != 0 || size != ':');
        if (!parsed) {
            continue;
        }
        ck_assert_uint_eq(drive, prefix ? 0 : 1);
        ck_assert_uint_eq(work.type, DISKIMAGE_FTYPE_REL);
        ck_assert_uint_eq(work.record_size, size);
        ck_assert(work.record_parameter);
        ck_assert_int_eq(work.access, DOS_ACCESS_READ);
        ck_assert_mem_eq(work.name, "NAME\xa0\xa0\xa0\xa0\xa0\xa0\xa0\xa0\xa0\xa0\xa0\xa0", sizeof(work.name));
    }
}
END_TEST

// Preserve parameter order, empty fields and bounded retained command tails.
START_TEST(test_open_parse_rel_parameter_order_and_tail) {
    static const struct {
        const char* name;
        uint8_t type;
        dos_access_t access;
        bool record_parameter;
        uint8_t size;
    } cases[] = {
        {"0:NAME,L,", DISKIMAGE_FTYPE_REL, DOS_ACCESS_READ, true, 'Z'},
        {"NAME,L", DISKIMAGE_FTYPE_REL, DOS_ACCESS_READ, false, 0},
        {"NAME,W,L", DISKIMAGE_FTYPE_REL, DOS_ACCESS_WRITE, false, 0},
        {"NAME,L,W", DISKIMAGE_FTYPE_REL, DOS_ACCESS_READ, true, 'W'},
        {"NAME,,W", 0, DOS_ACCESS_WRITE, false, 0},
        {"NAME,S,L,10", DISKIMAGE_FTYPE_REL, DOS_ACCESS_READ, false, 0},
        {"NAME,L\x8d,W", DISKIMAGE_FTYPE_REL, DOS_ACCESS_READ, false, 0},
        {"NAME,S\x8d,W", DISKIMAGE_FTYPE_SEQ, DOS_ACCESS_READ, false, 0},
    };
    const unsigned int length = (unsigned int)strlen(cases[_i].name);
    uint8_t command[DOS_COMMAND_RECEIVE_CAPACITY];
    memset(command, 'Z', sizeof(command));
    memcpy(command, cases[_i].name, length);
    dos_open_work_t work = {0};
    uint8_t drive = 0;
    ck_assert(dos_open_parse(command, length, &drive, &work));
    ck_assert_uint_eq(work.type, cases[_i].type);
    ck_assert_int_eq(work.access, cases[_i].access);
    ck_assert_int_eq(work.record_parameter, cases[_i].record_parameter);
    ck_assert_uint_eq(work.record_size, cases[_i].size);
}
END_TEST

// Partial GET retains status until its final CR is actually consumed.
START_TEST(test_geterc_retains_partial_status_and_consumes_cr) {
    const dos_model_t model = _i == 0 ? DOS_MODEL_4040 : DOS_MODEL_8050;
    const char* const power = _i == 0 ? "73,CBM DOS V2,00,00\r" : "73,CBM DOS V2.7,00,00,0\r";
    dos_unit_t unit;
    dos_reset(&unit, model);
    uint8_t bytes[DOS_STATUS_LINE_CAPACITY];
    bool eoi;
    ck_assert_uint_eq(dos_status_stage(&unit, bytes, sizeof(bytes), &eoi), strlen(power));
    ck_assert_mem_eq(bytes, power, strlen(power));
    ck_assert(eoi);
    dos_status_get(&unit, model, 3);
    ck_assert_uint_eq(dos_status_stage(&unit, bytes, sizeof(bytes), &eoi), strlen(power) - 3);
    ck_assert_mem_eq(bytes, power + 3, strlen(power) - 3);
    dos_status_get(&unit, model, (unsigned int)strlen(power) - 3);
    ck_assert_str_eq(unit.status, _i == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r");
}
END_TEST

// A deferred lookup must not issue I/O against a replacement image.
START_TEST(test_open_job_rejects_remounted_media_before_io) {
    image_t old, replacement;
    dos_media_bank_t bank = {0};
    mount_image(&bank, &old, (unsigned int)_i);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t name[] = "0:BASIC,PRG,R";
    dos_open(&unit, &bank, 0, IEEE_LOAD_CHANNEL, name, sizeof(name) - 1);
    ck_assert(unit.open.pending);
    mount_image(&bank, &replacement, (unsigned int)_i);
    dos_work(&unit, &bank);
    ck_assert(!unit.open.pending);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(old.reads + replacement.reads, 0);
    ck_assert_uint_eq(dos_find_channel(&unit, IEEE_LOAD_CHANNEL), DOS_UNUSED);

    // The transport's unit reset admits a new OPEN against the current binding.
    dos_reset(&unit, bank.units[0].model);
    dos_open(&unit, &bank, 0, IEEE_LOAD_CHANNEL, name, sizeof(name) - 1);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(old.reads, 0);
    ck_assert_uint_eq(replacement.reads, _i == 0 ? 3 : 2);
    ck_assert_uint_ne(dos_find_channel(&unit, IEEE_LOAD_CHANNEL), DOS_UNUSED);
    free(old.bytes);
    free(replacement.bytes);
}
END_TEST

// Unmatched directory links yield with their scratch ownership retained.
START_TEST(test_open_job_yields_between_directory_sectors) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_image(&bank, &image, 0);
    uint32_t first, second;
    ck_assert(diskimage_sector_offset(diskimage_type_d64, 18, 1, &first));
    ck_assert(diskimage_sector_offset(diskimage_type_d64, 18, 2, &second));
    memcpy(image.bytes + second, image.bytes + first, DISKIMAGE_SECTOR_SIZE);
    memset(image.bytes + first, 0, DISKIMAGE_SECTOR_SIZE);
    image.bytes[first] = 18;
    image.bytes[first + 1] = 2;
    dos_unit_t unit;
    dos_reset(&unit, DOS_MODEL_4040);
    const uint8_t name[] = "0:BASIC";
    dos_open(&unit, &bank, 0, IEEE_LOAD_CHANNEL, name, sizeof(name) - 1);
    ck_assert_uint_eq(image.reads, 0);
    ck_assert(unit.open.pending);
    dos_work(&unit, &bank);
    ck_assert_uint_eq(image.reads, 1);
    ck_assert(unit.open.pending);
    ck_assert_uint_eq(dos_find_channel(&unit, IEEE_LOAD_CHANNEL), DOS_UNUSED);
    dos_work(&unit, &bank);
    ck_assert_uint_eq(image.reads, 4);
    ck_assert(!unit.open.pending);
    ck_assert_uint_ne(dos_find_channel(&unit, IEEE_LOAD_CHANNEL), DOS_UNUSED);
    dos_reset(&unit, DOS_MODEL_4040);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// Repeated preview preserves bytes and cursors across a data-sector seam.
START_TEST(test_opnrch_staging_is_reversible_across_sector_links) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_image(&bank, &image, (unsigned int)_i);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t name[] = "0:BASIC,PRG";
    dos_open(&unit, &bank, 0, IEEE_LOAD_CHANNEL, name, sizeof(name) - 1);
    finish_open(&unit, &bank);
    const uint8_t channel = dos_find_channel(&unit, IEEE_LOAD_CHANNEL);
    ck_assert_uint_ne(channel, DOS_UNUSED);
    ck_assert_uint_eq(unit.buffer_use, (1u << 9) | (1u << 8));
    const unsigned int expected_size = _i == 0 ? 264 : 4;
    uint8_t first[64], second[64];
    bool eoi, repeated_eoi;
    const unsigned int first_size = dos_stage(&unit, &bank, channel, first, sizeof(first), &eoi);
    ck_assert_uint_eq(dos_stage(&unit, &bank, channel, second, sizeof(second), &repeated_eoi), first_size);
    ck_assert_mem_eq(first, second, first_size);
    ck_assert_int_eq(eoi, repeated_eoi);
    unsigned int received = 0;
    while (received < expected_size) {
        const unsigned int length = dos_stage(&unit, &bank, channel, first, sizeof(first), &eoi);
        ck_assert_uint_gt(length, 0);
        for (unsigned int index = 0; index < length; ++index) {
            const unsigned int position = received + index;
            const uint8_t expected = _i != 0 ? 0x80 + position : position < 254 ? position + 2 : 0xe2 + position - 254;
            ck_assert_uint_eq(first[index], expected);
        }
        ck_assert_int_eq(eoi, received + length == expected_size);
        dos_get(&unit, &bank, channel, length);
        received += length;
    }
    ck_assert(!unit.channels[channel].ready);
    ck_assert_uint_eq(dos_stage(&unit, &bank, channel, first, sizeof(first), &eoi), 0);
    dos_close(&unit, bank.units[0].model, IEEE_LOAD_CHANNEL);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// Shared finite channels/pages exhaust predictably and become reusable on CLOSE.
START_TEST(test_getrch_fndlnx_and_frechn_own_finite_resources) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_image(&bank, &image, (unsigned int)_i);
    const dos_media_io_t other = bank.units[0].slots[0].io;
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &other), DOS_MEDIA_OK);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    // OPNIRD retains its own two buffers until ENDCMD, allowing five files.
    for (uint8_t index = 0; index < DOS_FILE_CHANNEL_COUNT - 1u; ++index) {
        const uint8_t name[] = "0:BASIC";
        uint8_t selected[sizeof(name)];
        memcpy(selected, name, sizeof(name));
        selected[0] += index & 1;
        dos_open(&unit, &bank, 0, index + 2, selected, sizeof(selected) - 1);
        finish_open(&unit, &bank);
        ck_assert_uint_eq(dos_find_channel(&unit, index + 2), index);
        ck_assert_uint_eq(unit.channels[index].media.slot, index & 1);
        ck_assert_uint_eq(unit.channels[index].buffers[0], DOS_BUFFER_COUNT - 3 - 2 * index);
        ck_assert_uint_eq(unit.channels[index].buffers[1], DOS_BUFFER_COUNT - 4 - 2 * index);
    }
    ck_assert_uint_eq(unit.buffer_use, (1u << (DOS_BUFFER_COUNT - 2u)) - 1);
    const uint8_t name[] = "BASIC";
    dos_open(&unit, &bank, 0, 8, name, sizeof(name) - 1);
    finish_open(&unit, &bank);
    ck_assert_str_eq(unit.status, model == DOS_MODEL_4040 ? "70,NO CHANNEL,00,00\r" : "70,NO CHANNEL,00,00,0\r");
    ck_assert_uint_eq(dos_find_channel(&unit, 8), DOS_UNUSED);
    dos_close(&unit, model, 4);
    dos_open(&unit, &bank, 0, 8, name, sizeof(name) - 1);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(dos_find_channel(&unit, 8), 2);
    dos_close(&unit, model, IEEE_COMMAND_CHANNEL);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// Absent media and unsupported writes fail without leaking resources.
START_TEST(test_empty_slot_and_staged_features_fail_explicitly) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_image(&bank, &image, 0);
    dos_unit_t unit;
    dos_reset(&unit, DOS_MODEL_4040);
    const uint8_t absent[] = "1:BASIC";
    dos_open(&unit, &bank, 0, 2, absent, sizeof(absent) - 1);
    ck_assert_str_eq(unit.status, "74,DRIVE NOT READY,00,00\r");
    const uint8_t write[] = "0:NEW,SEQ,W";
    dos_open(&unit, &bank, 0, 2, write, sizeof(write) - 1);
    finish_open(&unit, &bank);
    ck_assert_str_eq(unit.status, "30,SYNTAX ERROR,00,00\r");
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// Check literal BASIC rows, partial consumption and final listing EOI.
START_TEST(test_formatted_directory_retains_exact_rows_and_final_eoi) {
    const unsigned int format = (unsigned int)_i;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    fixture_file(&image, format, 0, "FILE", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    const uint8_t channel = open_name(&unit, &bank, 0, "$0:F*");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    uint8_t expected[96] = {1, 4, 1, 1, 0, 0};
    memcpy(expected + 6, "\x12\"M3 FIXTURE      \" 01 2A", 25);
    expected[30] = format == 0 ? 'A' : 'C';
    expected[32] = expected[33] = 1;
    expected[34] = 1;
    memcpy(expected + 36, "   \"FILE\"             SEQ  ", 27);
    expected[64] = expected[65] = 1;
    const unsigned int available = format == 0 ? 663 : format == 1 ? 2051 : 4132;
    expected[66] = (uint8_t)available;
    expected[67] = (uint8_t)(available >> 8u);
    memset(expected + 68, ' ', 25);
    memcpy(expected + 68, "BLOCKS FREE.", 12);
    unsigned int position = 0;
    bool eoi = false;
    for (unsigned int attempts = 0; !eoi && attempts < 32u; ++attempts) {
        uint8_t bytes[DOS_STAGE_CAPACITY], repeated[DOS_STAGE_CAPACITY];
        bool repeated_eoi;
        const unsigned int length = dos_stage(&unit, &bank, channel, bytes, sizeof(bytes), &eoi);
        if (length == 0) {
            continue;
        }
        ck_assert_uint_eq(dos_stage(&unit, &bank, channel, repeated, sizeof(repeated), &repeated_eoi), length);
        ck_assert_mem_eq(bytes, repeated, length);
        ck_assert_uint_le(position + length, sizeof(expected));
        ck_assert_mem_eq(bytes, expected + position, length);
        ck_assert_int_eq(eoi, position + length == sizeof(expected));
        dos_get(&unit, &bank, channel, 3);
        ck_assert_uint_eq(dos_stage(&unit, &bank, channel, repeated, sizeof(repeated), &repeated_eoi), length - 3u);
        ck_assert_mem_eq(repeated, bytes + 3, length - 3u);
        dos_get(&unit, &bank, channel, length - 3u);
        position += length;
    }
    ck_assert_uint_eq(position, sizeof(expected));
    ck_assert(eoi);
    ck_assert(dos_close(&unit, model, 0));
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// Apply listing patterns/types separately, while raw channels retain the disk chain.
START_TEST(test_directory_type_filters_patterns_and_raw_chain) {
    const unsigned int format = (unsigned int)_i;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    fixture_file(&image, format, 0, "FILE", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    fixture_file(&image, format, 1, "USER", DISKIMAGE_FTYPE_USR, 0x60, 22);
    uint8_t bytes[6u * DISKIMAGE_SECTOR_PAYLOAD_SIZE];
    uint8_t channel = open_name(&unit, &bank, 0, "$0:*=U");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, bytes, sizeof(bytes)), 96);
    ck_assert_mem_eq(bytes + 36, "   \"USER\"             USR  ", 27);
    ck_assert(dos_close(&unit, model, 0));
    channel = open_name(&unit, &bank, 0, "$0:FILE,0:USER");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, bytes, sizeof(bytes)), 128);
    ck_assert_mem_eq(bytes + 36, "   \"FILE\"             SEQ  ", 27);
    ck_assert_mem_eq(bytes + 68, "   \"USER\"             USR  ", 27);
    ck_assert(dos_close(&unit, model, 0));

    channel = open_name(&unit, &bank, 2, "$0");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    const unsigned int sectors = format == 0 ? 2u : format == 1 ? 4u : 6u;
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, bytes, sizeof(bytes)),
                      sectors * DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    for (unsigned int index = 0; index < sectors; ++index) {
        const bool bam = format != 0 && index != 0 && index != sectors - 1u;
        const unsigned int track = bam ? 38u : format == 0 ? 18u : 39u;
        const unsigned int sector = bam ? (index - 1u) * 3u : index == 0 ? 0u : 1u;
        ck_assert_mem_eq(bytes + index * DISKIMAGE_SECTOR_PAYLOAD_SIZE,
                         sector_bytes(&image, type, track, sector) + DISKIMAGE_SECTOR_LINK_SIZE,
                         DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    }
    free(image.bytes);
}
END_TEST

// A failed alternate read becomes an error only when its page is selected.
START_TEST(test_open_prefetch_failure_waits_for_actual_buffer_selection) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    fixture_file(&image, format, 0, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x20, 255);
    image.fail_read = image.reads + 3;
    const uint8_t reader = open_name(&unit, &bank, 2, "0:SOURCE,S,R");
    ck_assert_uint_ne(reader, DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    const unsigned int reads = image.reads;
    unsigned int consumed = 0;
    while (consumed < DISKIMAGE_SECTOR_PAYLOAD_SIZE) {
        uint8_t bytes[DOS_STAGE_CAPACITY];
        bool eoi;
        const unsigned int length = dos_stage(&unit, &bank, reader, bytes, sizeof(bytes), &eoi);
        ck_assert_uint_gt(length, 0);
        ck_assert(!eoi);
        for (unsigned int offset = 0; offset < length; ++offset) {
            ck_assert_uint_eq(bytes[offset], (uint8_t)(0x20 + consumed + offset));
        }
        dos_get(&unit, &bank, reader, length);
        consumed += length;
    }
    uint8_t byte;
    bool eoi;
    ck_assert_uint_eq(dos_stage(&unit, &bank, reader, &byte, 1, &eoi), 0);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert(dos_close(&unit, model, 2));
    image.fail_read = 0;
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:SOURCE,S,R"), DOS_UNUSED);
    free(image.bytes);
}
END_TEST

// Check payload boundaries and the native initial singleton EOF sequence.
START_TEST(test_read_payload_boundaries) {
    static const unsigned int lengths[] = {0, 1, 2, 254, 255, 508};
    const unsigned int format = (unsigned int)_i / count_of(lengths);
    const unsigned int length = lengths[(unsigned int)_i % count_of(lengths)];
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "PAYLOAD", DISKIMAGE_FTYPE_SEQ, 0x40, length);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, 2, "0:PAYLOAD,S,R");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    expect_bytes(&unit, &bank, channel, length, 0x40);
    ck_assert_uint_eq(image.writes, 0);
    ck_assert(dos_close(&unit, bank.units[0].model, 2));
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// Exercise wildcard lookup and an omitted drive's fallback to its peer.
START_TEST(test_read_wildcards_and_peer_lookup) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "PAYLOAD", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    const dos_media_io_t io = bank.units[0].slots[0].io;
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, NULL), DOS_MEDIA_OK);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &io), DOS_MEDIA_OK);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, 2, "P?Y*,S,R");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    ck_assert_uint_eq(unit.selected_drive, 1);
    expect_bytes(&unit, &bank, channel, 22, 0x40);
    ck_assert(dos_close(&unit, bank.units[0].model, 2));
    ck_assert_uint_eq(open_name(&unit, &bank, 2, "1:PAYLOAD,U,R"), DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_TYPE_MISMATCH);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(image.writes, 0);
    free(image.bytes);
}
END_TEST

// Cached free-block totals must follow the newly bound image's identity.
START_TEST(test_directory_free_count_after_media_rebind) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t first, second;
    mount_blank(&bank, &first, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    uint8_t before[96], after[96];
    uint8_t channel = open_name(&unit, &bank, 0, "$0");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, before, sizeof(before)), 64);
    ck_assert(dos_close(&unit, bank.units[0].model, 0));
    mount_blank(&bank, &second, format);
    fixture_file(&second, format, 0, "NEW", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    channel = open_name(&unit, &bank, 0, "$0");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    if (format != 0) {
        // Fail after publishing the first new BAM part, then retry the listing.
        second.fail_read = second.reads + 4u;
        bool eoi;
        for (unsigned int attempt = 0; unit.channels[channel].ready && attempt < 32u; ++attempt) {
            uint8_t bytes[DOS_STAGE_CAPACITY];
            const unsigned int count = dos_stage(&unit, &bank, channel, bytes, sizeof(bytes), &eoi);
            dos_get(&unit, &bank, channel, count);
        }
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
        ck_assert(dos_close(&unit, bank.units[0].model, 0));
        second.fail_read = 0;
        channel = open_name(&unit, &bank, 0, "$0");
        ck_assert_uint_ne(channel, DOS_UNUSED);
    }
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, after, sizeof(after)), 96);
    const unsigned int old_free = before[34] | ((unsigned int)before[35] << 8u);
    const unsigned int new_free = after[66] | ((unsigned int)after[67] << 8u);
    ck_assert_uint_eq(new_free + 1u, old_free);
    ck_assert(dos_close(&unit, bank.units[0].model, 0));
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// Register only the directly callable read/status foundation.
Suite* dos_suite(void) {
    Suite* const suite = suite_create("dos");
    TCase* const core = tcase_create("read/status");
    tcase_add_loop_test(core, test_read_payload_boundaries, 0, 18);
    tcase_add_loop_test(core, test_read_wildcards_and_peer_lookup, 0, 3);
    tcase_add_loop_test(core, test_directory_free_count_after_media_rebind, 0, 3);
    tcase_add_loop_test(core, test_open_parse_binary_record_size, 0, 256);
    tcase_add_loop_test(core, test_open_parse_rel_parameter_order_and_tail, 0, 8);
    tcase_add_loop_test(core, test_geterc_retains_partial_status_and_consumes_cr, 0, 2);
    tcase_add_loop_test(core, test_open_job_rejects_remounted_media_before_io, 0, 3);
    tcase_add_test(core, test_open_job_yields_between_directory_sectors);
    tcase_add_loop_test(core, test_opnrch_staging_is_reversible_across_sector_links, 0, 3);
    tcase_add_loop_test(core, test_getrch_fndlnx_and_frechn_own_finite_resources, 0, 3);
    tcase_add_test(core, test_empty_slot_and_staged_features_fail_explicitly);
    tcase_add_loop_test(core, test_formatted_directory_retains_exact_rows_and_final_eoi, 0, 3);
    tcase_add_loop_test(core, test_directory_type_filters_patterns_and_raw_chain, 0, 3);
    tcase_add_loop_test(core, test_open_prefetch_failure_waits_for_actual_buffer_selection, 0, 3);
    suite_add_tcase(suite, core);
    return suite;
}

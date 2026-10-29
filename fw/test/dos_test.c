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

// Mounts an independently backed peer without replacing the first slot.
static void mount_peer(dos_media_bank_t* const bank, image_t* const image, const unsigned int format) {
    dos_media_bank_t peer = {0};
    mount_blank(&peer, image, format);
    ck_assert_int_eq(dos_media_set_slot(bank, 0, 1, &peer.units[0].slots[0].io), DOS_MEDIA_OK);
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
    unsigned int sectors;
    ck_assert(diskimage_track_sectors(type, 1, &sectors));
    ck_assert_uint_le(slot * 2u + blocks, sectors);
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

// CLRCB retires logical command RAM on success and error, not during pending parsing.
START_TEST(test_open_completion_retires_command_bytes) {
    static const char* const requests[] = {"F,S,R", "F,S,W", "M,S,R", "$0"};
    static const dos_status_code_t results[] = {
        DOS_STATUS_OK,
        DOS_STATUS_SYNTAX,
        DOS_STATUS_NOT_FOUND,
        DOS_STATUS_OK,
    };
    const unsigned int format = (unsigned int)_i / count_of(requests);
    const unsigned int request = (unsigned int)_i % count_of(requests);
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "F", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    fixture_file(&image, format, 1, "G", DISKIMAGE_FTYPE_PRG, 0x60, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    unit.command_bytes[DOS_COMMAND_LIMIT] = 'Z';
    const unsigned int length = (unsigned int)strlen(requests[request]);
    dos_open(&unit, &bank, 0, request == 3 ? 0 : 2, (const uint8_t*)requests[request], length);
    if (dos_work_pending(&unit)) {
        ck_assert_mem_eq(unit.command_bytes, requests[request], length);
    }
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, results[request]);
    uint8_t retired[DOS_COMMAND_RECEIVE_CAPACITY];
    memcpy(retired, unit.command_bytes, sizeof(retired));

    // An empty type field must not inherit the preceding command's parameter.
    const uint8_t channel = open_name(&unit, &bank, 3, "G,");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    expect_bytes(&unit, &bank, channel, 22, 0x60);
    const uint8_t cleared[DOS_COMMAND_LIMIT] = {0};
    ck_assert_mem_eq(retired, cleared, sizeof(cleared));
    ck_assert_uint_eq(retired[DOS_COMMAND_LIMIT], 'Z');
    free(image.bytes);
}
END_TEST

// CMDSET trims CR or CR-plus-one before checking its 58-byte logical limit.
START_TEST(test_open_command_limit_follows_cr_trimming) {
    static const struct {
        unsigned int characters;
        const char* suffix;
        bool accepted;
    } cases[] = {
        {58, "", true},    {58, "\r", true},   {57, "\rX", true}, {59, "", false},
        {59, "\r", false}, {58, "\rX", false}, {57, "\r", true},
    };
    const unsigned int format = (unsigned int)_i / count_of(cases);
    const unsigned int index = (unsigned int)_i % count_of(cases);
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "NNNNNNNNNNNNNNNN", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    uint8_t request[DOS_COMMAND_RECEIVE_CAPACITY + 2u];
    memset(request, 'N', sizeof(request));
    const unsigned int suffix = (unsigned int)strlen(cases[index].suffix);
    memcpy(request + cases[index].characters, cases[index].suffix, suffix);
    dos_open(&unit, &bank, 0, 2, request, cases[index].characters + suffix);
    finish_open(&unit, &bank);
    if (cases[index].accepted) {
        const uint8_t channel = dos_find_channel(&unit, 2);
        ck_assert_uint_ne(channel, DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        expect_bytes(&unit, &bank, channel, 22, 0x20);
    } else {
        ck_assert_uint_eq(dos_find_channel(&unit, 2), DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_COMMAND_TOO_LONG);
        ck_assert_uint_eq(unit.buffer_use, 0);
    }
    free(image.bytes);
}
END_TEST

// ALLDRS starts from LSTDRV even after a failed explicit OPEN changes DRVNUM.
START_TEST(test_open_defaults_to_last_successful_drive) {
    static const char* const requests[] = {"SAME", ":SAME", "X:SAME", "$"};
    const unsigned int format = (unsigned int)_i / count_of(requests);
    const unsigned int request = (unsigned int)_i % count_of(requests);
    dos_media_bank_t bank = {0};
    image_t first, second;
    mount_blank(&bank, &first, format);
    mount_peer(&bank, &second, format);
    fixture_file(&first, format, 0, "SAME", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    fixture_file(&second, format, 0, "SAME", DISKIMAGE_FTYPE_SEQ, 0x60, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:SAME"), DOS_UNUSED);
    ck_assert_uint_eq(open_name(&unit, &bank, 3, "1:MISSING"), DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NOT_FOUND);
    ck_assert_uint_eq(unit.selected_drive, 1);
    ck_assert_uint_eq(unit.last_drive, 0);
    const uint8_t channel = open_name(&unit, &bank, request == 3 ? 0 : 3, requests[request]);
    ck_assert_uint_ne(channel, DOS_UNUSED);
    ck_assert_uint_eq(unit.channels[channel].media.slot, 0);
    if (request != 3) {
        expect_bytes(&unit, &bank, channel, 22, 0x20);
    }
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// Replacing a raw directory channel is not a successful command before its read.
START_TEST(test_failed_raw_directory_replacement_preserves_last_drive) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t first, second;
    mount_blank(&bank, &first, format);
    mount_peer(&bank, &second, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "$0"), DOS_UNUSED);
    ck_assert_uint_eq(unit.last_drive, 0);
    second.fail_read = second.reads + 1u;
    ck_assert_uint_eq(open_name(&unit, &bank, 2, "$1"), DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_uint_eq(unit.selected_drive, 1);
    ck_assert_uint_eq(unit.last_drive, 0);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// SR20 treats a nonzero closed DEL byte as occupied, for reads and exact rows.
START_TEST(test_closed_del_entry_is_readable_and_listed) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "FILE", DISKIMAGE_FTYPE_DEL, 0x20, 22);
    fixture_file(&image, format, 1, "SEQ", DISKIMAGE_FTYPE_SEQ, 0x60, 22);
    fixture_file(&image, format, 2, "GHOST", DISKIMAGE_FTYPE_SEQ, 0x80, 22);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    sector_bytes(&image, type, format == 0 ? 18u : 39u, 1)[2u * DISKIMAGE_DIRECTORY_ENTRY_SIZE + 2u] = 0;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    ck_assert_uint_eq(open_name(&unit, &bank, 2, "0:GHOST"), DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NOT_FOUND);
    uint8_t channel = open_name(&unit, &bank, 2, "0:FILE");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    expect_bytes(&unit, &bank, channel, 22, 0x20);
    ck_assert(dos_close(&unit, bank.units[0].model, 2));
    channel = open_name(&unit, &bank, 0, "$0:F*");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    uint8_t bytes[128];
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, bytes, sizeof(bytes)), 96);
    uint8_t row[32] = {1, 1, 1, 0};
    memcpy(row + 4, "   \"FILE\"             DEL  ", 27);
    ck_assert_mem_eq(bytes + 32, row, sizeof(row));
    ck_assert(dos_close(&unit, bank.units[0].model, 0));
    channel = open_name(&unit, &bank, 0, "$0:*=S");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, bytes, sizeof(bytes)), 96);
    ck_assert_mem_eq(bytes + 36, "   \"SEQ\"              SEQ  ", 27);
    ck_assert(dos_close(&unit, bank.units[0].model, 0));
    channel = open_name(&unit, &bank, 0, "$0:*");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, bytes, sizeof(bytes)), 128);
    ck_assert_mem_eq(bytes + 32, row, sizeof(row));
    ck_assert_mem_eq(bytes + 68, "   \"SEQ\"              SEQ  ", 27);
    free(image.bytes);
}
END_TEST

// Later ordinary/raw read failures survive repeated and partially consumed previews.
START_TEST(test_later_sector_failure_retains_nonempty_preview_prefix) {
    const unsigned int format = (unsigned int)_i % 3u;
    const bool raw = _i >= 3;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    if (!raw) {
        fixture_file(&image, format, 0, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x20, 512);
    }
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const char* const request = raw ? "$0" : "0:SOURCE,S,R";
    const uint8_t channel = open_name(&unit, &bank, 2, request);
    ck_assert_uint_ne(channel, DOS_UNUSED);
    image.fail_read = image.reads + 1u;
    unsigned int consumed = 0;
    uint8_t bytes[DOS_STAGE_CAPACITY], repeated[DOS_STAGE_CAPACITY];
    bool eoi;
    while (consumed < (raw ? 192u : 448u)) {
        ck_assert_uint_eq(dos_stage(&unit, &bank, channel, bytes, sizeof(bytes), &eoi), sizeof(bytes));
        ck_assert(!eoi);
        dos_get(&unit, &bank, channel, sizeof(bytes));
        consumed += sizeof(bytes);
    }
    const unsigned int prefix = raw ? 62u : 60u;
    ck_assert_uint_eq(dos_stage(&unit, &bank, channel, bytes, sizeof(bytes), &eoi), prefix);
    ck_assert(!eoi);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    const unsigned int reads = image.reads;
    ck_assert_uint_eq(dos_stage(&unit, &bank, channel, repeated, sizeof(repeated), &eoi), prefix);
    ck_assert_mem_eq(bytes, repeated, prefix);
    ck_assert_uint_eq(image.reads, reads);
    dos_get(&unit, &bank, channel, 7);
    ck_assert_uint_eq(dos_stage(&unit, &bank, channel, repeated, sizeof(repeated), &eoi), prefix - 7u);
    ck_assert_mem_eq(repeated, bytes + 7, prefix - 7u);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    dos_get(&unit, &bank, channel, prefix - 7u);
    ck_assert_uint_eq(dos_stage(&unit, &bank, channel, bytes, sizeof(bytes), &eoi), 0);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert(!unit.channels[channel].ready);
    ck_assert(dos_close(&unit, bank.units[0].model, 2));
    ck_assert_uint_ne(open_name(&unit, &bank, 2, request), DOS_UNUSED);
    if (raw) {
        uint8_t chain[6u * DISKIMAGE_SECTOR_PAYLOAD_SIZE];
        ck_assert_uint_eq(directory_bytes(&unit, &bank, dos_find_channel(&unit, 2), chain, sizeof(chain)),
                          (format == 0   ? 2u
                           : format == 1 ? 4u
                                         : 6u) *
                              DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    } else {
        expect_bytes(&unit, &bank, dos_find_channel(&unit, 2), 512, 0x20);
    }
    free(image.bytes);
}
END_TEST

// Deferred read errors identify the failing channel, not the most recent OPEN.
START_TEST(test_interleaved_read_failure_reports_channel_drive) {
    const unsigned int format = (unsigned int)_i / 2u;
    const unsigned int failing = (unsigned int)_i % 2u;
    dos_media_bank_t bank = {0};
    image_t images[2];
    mount_blank(&bank, &images[0], format);
    mount_peer(&bank, &images[1], format);
    for (unsigned int slot = 0; slot < count_of(images); ++slot) {
        fixture_file(&images[slot], format, 0, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x20, 255);
    }
    images[failing].fail_read = images[failing].reads + 3u;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t reader = open_name(&unit, &bank, 2, failing ? "1:SOURCE,S,R" : "0:SOURCE,S,R");
    ck_assert_uint_ne(reader, DOS_UNUSED);
    const uint8_t other = open_name(&unit, &bank, 3, failing ? "0:SOURCE,S,R" : "1:SOURCE,S,R");
    ck_assert_uint_ne(other, DOS_UNUSED);
    ck_assert_uint_eq(unit.selected_drive, failing ^ 1u);
    uint8_t bytes[DOS_STAGE_CAPACITY];
    bool eoi;
    unsigned int remaining = DISKIMAGE_SECTOR_PAYLOAD_SIZE;
    while (remaining != 0) {
        const unsigned int capacity = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
        ck_assert_uint_eq(dos_stage(&unit, &bank, reader, bytes, capacity, &eoi), capacity);
        dos_get(&unit, &bank, reader, capacity);
        remaining -= capacity;
    }
    ck_assert_uint_eq(dos_stage(&unit, &bank, reader, bytes, sizeof(bytes), &eoi), 0);
    ck_assert_str_eq(unit.status, format == 0 ? "23,READ ERROR,00,00\r"
                                  : failing   ? "23,READ ERROR,00,00,1\r"
                                              : "23,READ ERROR,00,00,0\r");
    ck_assert_uint_eq(unit.last_drive, failing ^ 1u);
    expect_bytes(&unit, &bank, other, 255, 0x20);
    free(images[0].bytes);
    free(images[1].bytes);
}
END_TEST

// A resumed OPEN reselects its own drive after an older channel reports an error.
START_TEST(test_pending_open_reselects_drive_after_read_error) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t first, second;
    mount_blank(&bank, &first, format);
    mount_peer(&bank, &second, format);
    fixture_file(&first, format, 0, "GOOD", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    fixture_file(&second, format, 0, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x60, 255);
    second.fail_read = second.reads + 3u;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t reader = open_name(&unit, &bank, 2, "1:SOURCE,S,R");
    ck_assert_uint_ne(reader, DOS_UNUSED);
    uint8_t bytes[DOS_STAGE_CAPACITY];
    bool eoi;
    unsigned int remaining = DISKIMAGE_SECTOR_PAYLOAD_SIZE;
    while (remaining != 0) {
        const unsigned int capacity = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
        ck_assert_uint_eq(dos_stage(&unit, &bank, reader, bytes, capacity, &eoi), capacity);
        dos_get(&unit, &bank, reader, capacity);
        remaining -= capacity;
    }
    const uint8_t request[] = "0:GOOD,S,R";
    dos_open(&unit, &bank, 0, 3, request, sizeof(request) - 1u);
    ck_assert(dos_work_pending(&unit));
    ck_assert_uint_eq(dos_stage(&unit, &bank, reader, bytes, sizeof(bytes), &eoi), 0);
    ck_assert_uint_eq(unit.selected_drive, 1);
    ck_assert_mem_eq(unit.command_bytes, request, sizeof(request) - 1u);
    finish_open(&unit, &bank);
    const uint8_t channel = dos_find_channel(&unit, 3);
    ck_assert_uint_ne(channel, DOS_UNUSED);
    ck_assert_uint_eq(unit.channels[channel].media.slot, 0);
    ck_assert_uint_eq(unit.selected_drive, 0);
    ck_assert_uint_eq(unit.last_drive, 0);
    expect_bytes(&unit, &bank, channel, 22, 0x20);
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// Malformed BAM ranges retain the directory channel's drive in DOS 2.7 status.
START_TEST(test_directory_bam_error_reports_channel_drive) {
    const unsigned int format = (unsigned int)_i + 1u;
    dos_media_bank_t bank = {0};
    image_t first, second;
    mount_blank(&bank, &first, format);
    mount_peer(&bank, &second, format);
    fixture_file(&second, format, 0, "GOOD", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    sector_bytes(&first, type, 38, 0)[4] = 0;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t listing = open_name(&unit, &bank, 0, "$0");
    ck_assert_uint_ne(listing, DOS_UNUSED);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "1:GOOD,S,R"), DOS_UNUSED);
    for (unsigned int attempt = 0; unit.channels[listing].ready && attempt < 32u; ++attempt) {
        uint8_t bytes[DOS_STAGE_CAPACITY];
        bool eoi;
        const unsigned int length = dos_stage(&unit, &bank, listing, bytes, sizeof(bytes), &eoi);
        dos_get(&unit, &bank, listing, length);
    }
    ck_assert(!unit.channels[listing].ready);
    ck_assert_str_eq(unit.status, "71,DIR ERROR,01,00,0\r");
    ck_assert_uint_eq(unit.last_drive, 1);
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// FNDLMT ends the name at '=', but PARSE still finds later type/mode fields.
START_TEST(test_open_name_ends_at_equals_before_parameters) {
    static const char* const requests[] = {"0:FILE=IGNORED,S,R", "0:FILE=IGNORED,U,R", "FILE=IGNORED",
                                           "0:FILE=IGNORED\x8d,W"};
    const unsigned int format = (unsigned int)_i / count_of(requests);
    const unsigned int scenario = (unsigned int)_i % count_of(requests);
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "FILE", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, 2, requests[scenario]);
    if (scenario == 1) {
        ck_assert_uint_eq(channel, DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_TYPE_MISMATCH);
    } else {
        ck_assert_uint_ne(channel, DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        expect_bytes(&unit, &bank, channel, 22, 0x40);
    }
    free(image.bytes);
}
END_TEST

// CKTM's D clears an earlier type, while a later P overrides D.
START_TEST(test_open_d_resets_type_in_parameter_order) {
    static const char* const requests[] = {"0:FILE,P,D", "0:FILE,D,P", "0:FILE,U,D"};
    const unsigned int format = (unsigned int)_i / count_of(requests);
    const unsigned int scenario = (unsigned int)_i % count_of(requests);
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "FILE", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, 2, requests[scenario]);
    if (scenario == 1) {
        ck_assert_uint_eq(channel, DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_TYPE_MISMATCH);
    } else {
        ck_assert_uint_ne(channel, DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        expect_bytes(&unit, &bank, channel, 22, 0x40);
    }
    free(image.bytes);
}
END_TEST

// FS1SET recognizes R for REL, while the OPEN-only L leaves listing types unrestricted.
START_TEST(test_directory_relative_filter_uses_r_not_l) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool relative = (unsigned int)_i % 2u == 0;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "DATA", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    fixture_file(&image, format, 1, "RECORDS", DISKIMAGE_FTYPE_REL, 0x60, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, relative ? "$0:*=R" : "$0:*=L");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    uint8_t bytes[128];
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, bytes, sizeof(bytes)), relative ? 96 : 128);
    if (!relative) {
        ck_assert_mem_eq(bytes + 36, "   \"DATA\"             SEQ  ", 27);
    }
    ck_assert_mem_eq(bytes + (relative ? 36 : 68), "   \"RECORDS\"          REL  ", 27);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    free(image.bytes);
}
END_TEST

// OP0415 starts at drive zero and FF30 skips non-PRG matches before peer fallback.
START_TEST(test_initial_load_star_resets_drive_and_filters_programs) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int scenario = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0};
    image_t first, second;
    mount_blank(&bank, &first, format);
    mount_peer(&bank, &second, format);
    fixture_file(&first, format, 0, "DATA", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    if (scenario != 2) {
        fixture_file(&first, format, 1, "PROGRAM", DISKIMAGE_FTYPE_PRG, 0x60, 22);
    }
    fixture_file(&second, format, 0, "PEER", DISKIMAGE_FTYPE_PRG, 0x80, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    if (scenario == 0) {
        ck_assert_uint_ne(open_name(&unit, &bank, 2, "1:PEER,P,R"), DOS_UNUSED);
        ck_assert_uint_eq(unit.last_drive, 1);
        ck_assert_uint_eq(unit.last_program_track, 0);
    }
    const uint8_t channel = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, "*");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.channels[channel].media.slot, scenario == 2 ? 1 : 0);
    ck_assert_uint_eq(unit.last_drive, scenario == 2 ? 1 : 0);
    expect_bytes(&unit, &bank, channel, 22, scenario == 2 ? 0x80 : 0x60);
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// OP0415 resets both drive variables even when PRSCLN rejects the request.
START_TEST(test_failed_initial_load_star_resets_drive_before_parsing) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t first, second;
    mount_blank(&bank, &first, format);
    mount_peer(&bank, &second, format);
    fixture_file(&first, format, 0, "FILE", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    fixture_file(&second, format, 0, "OTHER", DISKIMAGE_FTYPE_SEQ, 0x80, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "1:OTHER,S,R"), DOS_UNUSED);
    ck_assert_uint_eq(open_name(&unit, &bank, IEEE_LOAD_CHANNEL, "*,0:INVALID"), DOS_UNUSED);
    ck_assert_str_eq(unit.status, format == 0 ? "30,SYNTAX ERROR,00,00\r" : "30,SYNTAX ERROR,00,00,0\r");
    ck_assert_uint_eq(unit.selected_drive, 0);
    ck_assert_uint_eq(unit.last_drive, 0);
    const uint8_t channel = open_name(&unit, &bank, 3, "FILE,S,R");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    expect_bytes(&unit, &bank, channel, 22, 0x40);
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// OPEN's retained-program shortcut precedes filename and parameter parsing.
START_TEST(test_retained_load_star_bypasses_ignored_parameters) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool relative = (unsigned int)_i % 2u == 0;
    dos_media_bank_t bank = {0};
    image_t first, second;
    mount_blank(&bank, &first, format);
    mount_peer(&bank, &second, format);
    fixture_file(&first, format, 0, "PROGRAM", DISKIMAGE_FTYPE_PRG, 0x60, 22);
    fixture_file(&second, format, 0, "OTHER", DISKIMAGE_FTYPE_SEQ, 0x80, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    ck_assert_uint_ne(open_name(&unit, &bank, IEEE_LOAD_CHANNEL, "0:PROGRAM,P,R"), DOS_UNUSED);
    ck_assert(dos_close(&unit, bank.units[0].model, IEEE_LOAD_CHANNEL));
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "1:OTHER,S,R"), DOS_UNUSED);
    const uint8_t channel = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, relative ? "*,L,10" : "*,0:INVALID");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.channels[channel].media.slot, 0);
    expect_bytes(&unit, &bank, channel, 22, 0x60);
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// Bound cached self-links and multi-sector cycles, including cycles that emit rows.
START_TEST(test_directory_cycles_publish_bounded_read_error) {
    const unsigned int format = (unsigned int)_i / 4u;
    const unsigned int scenario = (unsigned int)_i % 4u;
    dos_media_bank_t bank = {0};
    image_t first, second;
    mount_blank(&bank, &first, format);
    mount_peer(&bank, &second, format);
    fixture_file(&first, format, 0, "OTHER", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    if (scenario == 1 || scenario == 3) {
        fixture_file(&second, format, 0, scenario == 1 ? "OTHER" : "MATCH", DISKIMAGE_FTYPE_SEQ, 0x60, 22);
    }
    const diskimage_type_t type = bank.units[0].slots[1].io.image.type;
    const unsigned int track = format == 0 ? 18u : 39u;
    uint8_t* const directory = sector_bytes(&second, type, track, 1);
    directory[0] = track;
    directory[1] = scenario == 2 ? 2 : 1;
    if (scenario == 2) {
        uint8_t* const next = sector_bytes(&second, type, track, 2);
        next[0] = track;
        next[1] = 1;
    }
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, "$1:MATCH*");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:OTHER,S,R"), DOS_UNUSED);
    const unsigned int limit = second.size / DISKIMAGE_SECTOR_SIZE;
    for (unsigned int attempt = 0; unit.channels[channel].ready && attempt < 3u * (limit + 1u); ++attempt) {
        uint8_t bytes[DOS_STAGE_CAPACITY];
        bool eoi;
        const unsigned int count = dos_stage(&unit, &bank, channel, bytes, sizeof(bytes), &eoi);
        ck_assert(!eoi);
        dos_get(&unit, &bank, channel, count);
    }
    ck_assert(!unit.channels[channel].ready);
    const unsigned int sector = scenario == 2 && limit % 2u != 0 ? 2 : 1;
    char expected[DOS_STATUS_LINE_CAPACITY];
    snprintf(expected, sizeof(expected), format == 0 ? "23,READ ERROR,%02u,%02u\r" : "23,READ ERROR,%02u,%02u,1\r",
             track, sector);
    ck_assert_str_eq(unit.status, expected);
    ck_assert_uint_eq(unit.last_drive, 0);
    ck_assert(dos_close(&unit, bank.units[0].model, IEEE_COMMAND_CHANNEL));
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// Empty previews cannot prepare rows, do I/O, or retire ordinary/raw readers.
START_TEST(test_zero_capacity_preview_preserves_reader_and_directory_state) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int kind = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "FILE", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, kind == 2 ? IEEE_LOAD_CHANNEL : 2, kind == 0 ? "0:FILE" : "$0");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    dos_unit_t before;
    memcpy(&before, &unit, sizeof(unit));
    unsigned int reads = image.reads;
    bool eoi = true;
    uint8_t bytes[8], repeated[5];
    ck_assert_uint_eq(dos_stage(&unit, &bank, channel, bytes, 0, &eoi), 0);
    ck_assert(!eoi);
    ck_assert_mem_eq(&unit, &before, sizeof(unit));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(dos_stage(&unit, &bank, channel, bytes, sizeof(bytes), &eoi), sizeof(bytes));
    if (kind == 0) {
        ck_assert_mem_eq(bytes, "\x40\x41\x42\x43\x44\x45\x46\x47", sizeof(bytes));
    } else if (kind == 1) {
        const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
        ck_assert_mem_eq(bytes, sector_bytes(&image, type, format == 0 ? 18 : 39, 0) + 2, sizeof(bytes));
    } else {
        ck_assert_mem_eq(bytes, "\x01\x04\x01\x01\x00\x00\x12\"", sizeof(bytes));
    }
    dos_get(&unit, &bank, channel, 3);
    memcpy(&before, &unit, sizeof(unit));
    reads = image.reads;
    eoi = true;
    ck_assert_uint_eq(dos_stage(&unit, &bank, channel, repeated, 0, &eoi), 0);
    ck_assert(!eoi);
    ck_assert_mem_eq(&unit, &before, sizeof(unit));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(dos_stage(&unit, &bank, channel, repeated, sizeof(repeated), &eoi), sizeof(repeated));
    ck_assert_mem_eq(repeated, bytes + 3, sizeof(repeated));
    ck_assert(!eoi);
    free(image.bytes);
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
    tcase_add_loop_test(core, test_open_completion_retires_command_bytes, 0, 12);
    tcase_add_loop_test(core, test_open_command_limit_follows_cr_trimming, 0, 21);
    tcase_add_loop_test(core, test_open_defaults_to_last_successful_drive, 0, 12);
    tcase_add_loop_test(core, test_failed_raw_directory_replacement_preserves_last_drive, 0, 3);
    tcase_add_loop_test(core, test_closed_del_entry_is_readable_and_listed, 0, 3);
    tcase_add_loop_test(core, test_later_sector_failure_retains_nonempty_preview_prefix, 0, 6);
    tcase_add_loop_test(core, test_interleaved_read_failure_reports_channel_drive, 0, 6);
    tcase_add_loop_test(core, test_pending_open_reselects_drive_after_read_error, 0, 3);
    tcase_add_loop_test(core, test_directory_bam_error_reports_channel_drive, 0, 2);
    tcase_add_loop_test(core, test_open_name_ends_at_equals_before_parameters, 0, 12);
    tcase_add_loop_test(core, test_open_d_resets_type_in_parameter_order, 0, 9);
    tcase_add_loop_test(core, test_directory_relative_filter_uses_r_not_l, 0, 6);
    tcase_add_loop_test(core, test_initial_load_star_resets_drive_and_filters_programs, 0, 9);
    tcase_add_loop_test(core, test_failed_initial_load_star_resets_drive_before_parsing, 0, 3);
    tcase_add_loop_test(core, test_retained_load_star_bypasses_ignored_parameters, 0, 6);
    tcase_add_loop_test(core, test_directory_cycles_publish_bounded_read_error, 0, 12);
    tcase_add_loop_test(core, test_zero_capacity_preview_preserves_reader_and_directory_state, 0, 9);
    suite_add_tcase(suite, core);
    return suite;
}

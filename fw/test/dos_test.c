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
    dos_close(&unit, &bank, bank.units[0].model, IEEE_LOAD_CHANNEL);
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
    dos_close(&unit, &bank, model, 4);
    dos_open(&unit, &bank, 0, 8, name, sizeof(name) - 1);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(dos_find_channel(&unit, 8), 2);
    dos_close(&unit, &bank, model, IEEE_COMMAND_CHANNEL);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// Absent media and read-only writes fail without leaking resources.
START_TEST(test_empty_slot_and_read_only_writes_fail_explicitly) {
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
    ck_assert_str_eq(unit.status, "26, WRITE PROTECT ON,00,00\r");
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
    ck_assert(dos_close(&unit, &bank, model, 0));
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
    ck_assert(dos_close(&unit, &bank, model, 0));
    channel = open_name(&unit, &bank, 0, "$0:FILE,0:USER");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, bytes, sizeof(bytes)), 128);
    ck_assert_mem_eq(bytes + 36, "   \"FILE\"             SEQ  ", 27);
    ck_assert_mem_eq(bytes + 68, "   \"USER\"             USR  ", 27);
    ck_assert(dos_close(&unit, &bank, model, 0));

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
    ck_assert(dos_close(&unit, &bank, model, 2));
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
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
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
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
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
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 0));
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
        ck_assert(dos_close(&unit, &bank, bank.units[0].model, 0));
        second.fail_read = 0;
        channel = open_name(&unit, &bank, 0, "$0");
        ck_assert_uint_ne(channel, DOS_UNUSED);
    }
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, after, sizeof(after)), 96);
    const unsigned int old_free = before[34] | ((unsigned int)before[35] << 8u);
    const unsigned int new_free = after[66] | ((unsigned int)after[67] << 8u);
    ck_assert_uint_eq(new_free + 1u, old_free);
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 0));
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// CLRCB retires logical command RAM on success and error, not during pending parsing.
START_TEST(test_open_completion_retires_command_bytes) {
    static const char* const requests[] = {"F,S,R", "F,S,W", "M,S,R", "$0"};
    static const dos_status_code_t results[] = {
        DOS_STATUS_OK,
        DOS_STATUS_FILE_EXISTS,
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
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    channel = open_name(&unit, &bank, 0, "$0:F*");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    uint8_t bytes[128];
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, bytes, sizeof(bytes)), 96);
    uint8_t row[32] = {1, 1, 1, 0};
    memcpy(row + 4, "   \"FILE\"             DEL  ", 27);
    ck_assert_mem_eq(bytes + 32, row, sizeof(row));
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 0));
    channel = open_name(&unit, &bank, 0, "$0:*=S");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, bytes, sizeof(bytes)), 96);
    ck_assert_mem_eq(bytes + 36, "   \"SEQ\"              SEQ  ", 27);
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 0));
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
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
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
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, IEEE_LOAD_CHANNEL));
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
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, IEEE_COMMAND_CHANNEL));
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

// DOS 2.7 CMDER3 retires the failing secondary, not another reader's owner.
START_TEST(test_failed_reopen_releases_only_dos27_ordinary_owner) {
    const unsigned int format = (unsigned int)_i / 5u;
    const unsigned int scenario = (unsigned int)_i % 5u;
    dos_media_bank_t bank = {0};
    image_t first, second;
    mount_blank(&bank, &first, format);
    mount_peer(&bank, &second, format);
    const uint8_t reserved_type = DISKIMAGE_FTYPE_REL + 1u;
    fixture_file(&first, format, 0, "FILE", scenario == 4 ? reserved_type : DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    fixture_file(&second, format, 0, "PEER", DISKIMAGE_FTYPE_SEQ, 0x80, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t owner = open_name(&unit, &bank, 2, "0:FILE");
    const uint8_t peer = open_name(&unit, &bank, 3, "1:PEER");
    ck_assert_uint_ne(owner, DOS_UNUSED);
    ck_assert_uint_ne(peer, DOS_UNUSED);
    const uint16_t owned_pages = (1u << unit.channels[owner].buffers[0]) | (1u << unit.channels[owner].buffers[1]);
    const uint16_t before = unit.buffer_use;
    char oversized[DOS_COMMAND_RECEIVE_CAPACITY + 1u];
    memset(oversized, 'X', sizeof(oversized));
    if (scenario == 2) {
        dos_open(&unit, &bank, 0, 2, (const uint8_t*)oversized, sizeof(oversized));
    } else {
        if (scenario == 3) {
            first.fail_read = first.reads + 1u;
        }
        open_name(&unit, &bank, 2, scenario == 1 ? "0:FILE,U,R" : "0:MISSING");
    }
    const dos_status_code_t expected = scenario == 1   ? DOS_STATUS_TYPE_MISMATCH
                                       : scenario == 2 ? DOS_STATUS_COMMAND_TOO_LONG
                                       : scenario == 3 ? DOS_STATUS_READ_ERROR
                                                       : DOS_STATUS_NOT_FOUND;
    ck_assert_uint_eq(unit.status_code, expected);
    ck_assert_uint_eq(unit.last_drive, 1);
    const bool retained = format == 0 || scenario == 4;
    ck_assert_uint_eq(dos_find_channel(&unit, 2), retained ? owner : DOS_UNUSED);
    ck_assert_uint_eq(unit.buffer_use, retained ? before : before & ~owned_pages);
    ck_assert_uint_eq(dos_find_channel(&unit, 3), peer);
    expect_bytes(&unit, &bank, peer, 22, 0x80);
    if (retained) {
        expect_bytes(&unit, &bank, owner, 22, 0x40);
    }
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// A listing's REL selector is not a REL channel exempt from TLERR/FRECHN.
START_TEST(test_failed_reopen_releases_dos27_listing_reservations) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "RELATIVE", DISKIMAGE_FTYPE_REL, 0x40, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t listing = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, "$0:*=R");
    ck_assert_uint_ne(listing, DOS_UNUSED);
    ck_assert_uint_ne(unit.directory_internal, 0);
    open_name(&unit, &bank, IEEE_LOAD_CHANNEL, "0:MISSING");
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NOT_FOUND);
    ck_assert_uint_eq(dos_find_channel(&unit, IEEE_LOAD_CHANNEL), format == 0 ? listing : DOS_UNUSED);
    if (format == 0) {
        uint8_t bytes[96];
        ck_assert_uint_eq(directory_bytes(&unit, &bank, listing, bytes, sizeof(bytes)), sizeof(bytes));
        ck_assert_mem_eq(bytes + 40, "RELATIVE", 8);
    } else {
        ck_assert_uint_eq(unit.directory_internal, 0);
        ck_assert_uint_eq(unit.buffer_use, 0);
    }
    free(image.bytes);
}
END_TEST

// COMPAR consumes literal $A0 until FNDLMT, then tests directory padding.
START_TEST(test_open_preserves_explicit_pattern_length) {
    const unsigned int format = (unsigned int)_i / 5u;
    const unsigned int scenario = (unsigned int)_i % 5u;
    static const char* const requests[] = {
        "0:A\xa0X", "0:A\xa0", "0:A\xa0*", "0:A?X", "0:ABCDEFGHIJKLMNOPQ",
    };
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "A\xa0Y", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    fixture_file(&image, format, 1, "A\xa0X", DISKIMAGE_FTYPE_SEQ, 0x60, 22);
    fixture_file(&image, format, 2, "A", DISKIMAGE_FTYPE_SEQ, 0x80, 22);
    fixture_file(&image, format, 3, "ABCDEFGHIJKLMNOP", DISKIMAGE_FTYPE_SEQ, 0xa0, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, 2, requests[scenario]);
    if (scenario == 3) {
        ck_assert_uint_eq(channel, DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_NOT_FOUND);
    } else {
        ck_assert_uint_ne(channel, DOS_UNUSED);
        const unsigned int first = scenario == 0 ? 0x60 : scenario == 1 ? 0x80 : scenario == 2 ? 0x40 : 0xa0;
        expect_bytes(&unit, &bank, channel, 22, first);
    }
    free(image.bytes);
}
END_TEST

// Listing records retain independent lengths, including subsequent patterns.
START_TEST(test_directory_preserves_explicit_pattern_lengths) {
    const unsigned int format = (unsigned int)_i / 5u;
    const unsigned int scenario = (unsigned int)_i % 5u;
    static const char* const requests[] = {
        "$0:A\xa0X", "$0:A\xa0", "$0:A\xa0*", "$0:A?X", "$0:A\xa0X,0:ABCDEFGHIJKLMNOPQ",
    };
    static const unsigned int rows[] = {1, 1, 3, 0, 2};
    static const uint8_t blocks[][3] = {{2}, {3}, {1, 2, 3}, {0}, {2, 4}};
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    static const char* const names[] = {"A\xa0Y", "A\xa0X", "A", "ABCDEFGHIJKLMNOP"};
    for (unsigned int entry = 0; entry < count_of(names); ++entry) {
        fixture_file(&image, format, entry, names[entry], DISKIMAGE_FTYPE_SEQ, 0x40, 22);
        // Distinguish matches whose formatted names all end at their first $A0.
        sector_bytes(&image, bank.units[0].slots[0].io.image.type, format == 0 ? 18 : 39,
                     1)[entry * DISKIMAGE_DIRECTORY_ENTRY_SIZE + 30u] = entry + 1u;
    }
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, requests[scenario]);
    ck_assert_uint_ne(channel, DOS_UNUSED);
    uint8_t bytes[160];
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, bytes, sizeof(bytes)), 64u + rows[scenario] * 32u);
    for (unsigned int row = 0; row < rows[scenario]; ++row) {
        ck_assert_uint_eq(bytes[34u + row * 32u], blocks[scenario][row]);
    }
    free(image.bytes);
}
END_TEST

// Initial INITDR releases drive-zero GETACT/LSTJOB owners before parsing.
START_TEST(test_initial_load_star_releases_active_drive_owners) {
    const unsigned int format = (unsigned int)_i / 5u;
    const unsigned int scenario = (unsigned int)_i % 5u;
    dos_media_bank_t bank = {0};
    image_t first, second;
    mount_blank(&bank, &first, format);
    mount_peer(&bank, &second, format);
    fixture_file(&first, format, 0, "DATA", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    fixture_file(&second, format, 0, "PEER", DISKIMAGE_FTYPE_SEQ, 0x80, 22);
    if (scenario == 0 || scenario == 4) {
        fixture_file(&first, format, 1, "PROGRAM", DISKIMAGE_FTYPE_PRG, 0x60, 22);
    }
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    if (scenario == 4) {
        ck_assert_uint_ne(open_name(&unit, &bank, IEEE_LOAD_CHANNEL, "0:PROGRAM,P,R"), DOS_UNUSED);
        ck_assert(dos_close(&unit, &bank, bank.units[0].model, IEEE_LOAD_CHANNEL));
    }
    const uint8_t owner = open_name(&unit, &bank, 2, "0:DATA");
    const uint8_t peer = open_name(&unit, &bank, 3, "1:PEER");
    ck_assert_uint_ne(owner, DOS_UNUSED);
    ck_assert_uint_ne(peer, DOS_UNUSED);
    if (scenario == 3) {
        // Oppose active-page jobs to media slots and inactive-page jobs.
        unit.buffer_drives |= (uint16_t)(1u << unit.channels[owner].buffers[unit.channels[owner].active]);
        unit.buffer_drives &= (uint16_t)~(1u << unit.channels[peer].buffers[unit.channels[peer].active]);
    }
    const uint8_t channel = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, scenario >= 2 ? "*,0:INVALID" : "*");
    ck_assert_uint_eq(unit.status_code, scenario == 0 || scenario == 4 ? DOS_STATUS_OK
                                        : scenario == 1                ? DOS_STATUS_NOT_FOUND
                                                                       : DOS_STATUS_SYNTAX);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), scenario == 3 || scenario == 4 ? owner : DOS_UNUSED);
    ck_assert_uint_eq(dos_find_channel(&unit, 3), scenario == 3 ? DOS_UNUSED : peer);
    if (scenario != 3) {
        expect_bytes(&unit, &bank, peer, 22, 0x80);
    }
    if (scenario == 3 || scenario == 4) {
        expect_bytes(&unit, &bank, owner, 22, 0x40);
    }
    if (scenario == 0 || scenario == 4) {
        ck_assert_uint_ne(channel, DOS_UNUSED);
        expect_bytes(&unit, &bank, channel, 22, 0x60);
    } else {
        ck_assert_uint_eq(channel, DOS_UNUSED);
        ck_assert_uint_eq(unit.buffer_use, (1u << unit.channels[scenario == 3 ? owner : peer].buffers[0]) |
                                               (1u << unit.channels[scenario == 3 ? owner : peer].buffers[1]));
    }
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// Only exact single-drive formatted LOAD initializes and releases 4040 owners.
START_TEST(test_directory_load_initializes_only_4040_shorthand) {
    const unsigned int format = (unsigned int)_i / 4u;
    const unsigned int scenario = (unsigned int)_i % 4u;
    const unsigned int selected = scenario & 1u;
    dos_media_bank_t bank = {0};
    image_t first, second;
    mount_blank(&bank, &first, format);
    mount_peer(&bank, &second, format);
    fixture_file(&first, format, 0, "FILE", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    fixture_file(&second, format, 0, "FILE", DISKIMAGE_FTYPE_SEQ, 0x80, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    for (unsigned int secondary = 2; secondary < 6; ++secondary) {
        ck_assert_uint_ne(open_name(&unit, &bank, secondary, selected ? "1:FILE" : "0:FILE"), DOS_UNUSED);
    }
    const uint8_t peer = open_name(&unit, &bank, 6, selected ? "0:FILE" : "1:FILE");
    ck_assert_uint_ne(peer, DOS_UNUSED);
    const uint8_t channel = open_name(&unit, &bank, scenario == 3 ? 7 : IEEE_LOAD_CHANNEL,
                                      scenario == 2 ? "$0:*"
                                      : selected    ? "$1"
                                                    : "$0");
    if (format == 0 && scenario < 2) {
        ck_assert_uint_ne(channel, DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        for (unsigned int secondary = 2; secondary < 6; ++secondary) {
            ck_assert_uint_eq(dos_find_channel(&unit, secondary), DOS_UNUSED);
        }
        uint8_t bytes[96];
        ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, bytes, sizeof(bytes)), sizeof(bytes));
        ck_assert_uint_eq(bytes[4], selected);
    } else if (scenario != 3) {
        ck_assert_uint_eq(channel, DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
        for (unsigned int secondary = 2; secondary < 6; ++secondary) {
            ck_assert_uint_ne(dos_find_channel(&unit, secondary), DOS_UNUSED);
        }
    } else {
        ck_assert_uint_ne(channel, DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        for (unsigned int secondary = 2; secondary < 6; ++secondary) {
            ck_assert_uint_ne(dos_find_channel(&unit, secondary), DOS_UNUSED);
        }
    }
    ck_assert_uint_eq(dos_find_channel(&unit, 6), peer);
    expect_bytes(&unit, &bank, peer, 22, selected ? 0x40 : 0x80);
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// Numeric-leading patterns do not address a drive unless followed by a colon.
START_TEST(test_directory_numeric_wildcard_uses_available_drive) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool peer_only = (unsigned int)_i % 2u != 0;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, peer_only ? "0FILE" : "1FILE", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    if (peer_only) {
        const dos_media_io_t io = bank.units[0].slots[0].io;
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &io), DOS_MEDIA_OK);
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, NULL), DOS_MEDIA_OK);
    }
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, peer_only ? "$0*" : "$1*");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    uint8_t bytes[96];
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, bytes, sizeof(bytes)), sizeof(bytes));
    ck_assert_mem_eq(bytes + 40, peer_only ? "0FILE" : "1FILE", 5);
    free(image.bytes);
}
END_TEST

// Exact requests retire once, while unresolved and wildcard requests keep searching.
START_TEST(test_directory_exact_patterns_complete_independently) {
    const unsigned int format = (unsigned int)_i / 4u;
    const unsigned int scenario = (unsigned int)_i % 4u;
    static const char* const requests[] = {"$FILE", "$FILE,OTHER", "$F?LE", "$FILE=U"};
    dos_media_bank_t bank = {0};
    image_t first, second;
    mount_blank(&bank, &first, format);
    mount_peer(&bank, &second, format);
    fixture_file(&first, format, 0, "FILE", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    fixture_file(&first, format, 1, "FILE", DISKIMAGE_FTYPE_SEQ, 0x60, 22);
    fixture_file(&second, format, 0, "FILE", DISKIMAGE_FTYPE_SEQ, 0x80, 22);
    fixture_file(&second, format, 1, "OTHER", DISKIMAGE_FTYPE_USR, 0xa0, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, requests[scenario]);
    ck_assert_uint_ne(channel, DOS_UNUSED);
    uint8_t bytes[256];
    const unsigned int length = directory_bytes(&unit, &bank, channel, bytes, sizeof(bytes));
    ck_assert_uint_eq(length, scenario == 0 ? 96 : scenario == 1 ? 192 : scenario == 2 ? 224 : 64);
    if (scenario != 3) {
        ck_assert_mem_eq(bytes + 40, "FILE", 4);
    }
    if (scenario == 1) {
        ck_assert_mem_eq(bytes + 136, "OTHER", 5);
    }
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// ALLDRS retains SETDRV's previous selection for bare/nonnumeric prefixes.
START_TEST(test_directory_mixed_prefixes_inherit_previous_drive) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int scenario = (unsigned int)_i % 3u;
    static const char* const requests[] = {
        "$0:ALPHA,1:BETA,:GAMMA",
        "$0:ALPHA,1:BETA,X:GAMMA",
        "$0:ALPHA,1:BETA,GAMMA",
    };
    dos_media_bank_t bank = {0};
    image_t first, second;
    mount_blank(&bank, &first, format);
    mount_peer(&bank, &second, format);
    fixture_file(&first, format, 0, "ALPHA", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    fixture_file(&first, format, 1, "GAMMA", DISKIMAGE_FTYPE_SEQ, 0x60, 22);
    fixture_file(&second, format, 0, "BETA", DISKIMAGE_FTYPE_SEQ, 0x80, 22);
    fixture_file(&second, format, 1, "GAMMA", DISKIMAGE_FTYPE_USR, 0xa0, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, requests[scenario]);
    ck_assert_uint_ne(channel, DOS_UNUSED);
    uint8_t bytes[256];
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, bytes, sizeof(bytes)), 224);
    // SCHTBL prefers the default drive when only one default is represented.
    ck_assert_mem_eq(bytes + (scenario == 2 ? 168 : 40), "ALPHA", 5);
    ck_assert_mem_eq(bytes + (scenario == 2 ? 40 : 136), "BETA", 4);
    ck_assert_mem_eq(bytes + (scenario == 2 ? 72 : 168), "GAMMA", 5);
    ck_assert_mem_eq(bytes + (scenario == 2 ? 90 : 186), "USR", 3);
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// DOS 2.7 OPTSCH admits the full drive set, not just the first prefix.
START_TEST(test_directory_missing_first_drive_falls_back_only_when_requested) {
    const unsigned int format = 1u + (unsigned int)_i / 3u;
    const unsigned int scenario = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "B", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, IEEE_LOAD_CHANNEL,
                                      scenario == 0   ? "$1:A,0:B"
                                      : scenario == 1 ? "$1"
                                                      : "$1:A");
    if (scenario == 0) {
        ck_assert_uint_ne(channel, DOS_UNUSED);
        uint8_t bytes[96];
        ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, bytes, sizeof(bytes)), sizeof(bytes));
        ck_assert_mem_eq(bytes + 40, "B", 1);
    } else {
        ck_assert_uint_eq(channel, DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_NOT_READY);
    }
    free(image.bytes);
}
END_TEST

// CLDCHN follows either active page even when job ownership differs from media.
START_TEST(test_4040_directory_initialization_selects_active_page_job) {
    dos_media_bank_t bank = {0};
    image_t first, second;
    mount_blank(&bank, &first, 0);
    mount_peer(&bank, &second, 0);
    fixture_file(&first, 0, 0, "FILE", DISKIMAGE_FTYPE_SEQ, 0x40, 300);
    fixture_file(&second, 0, 0, "PEER", DISKIMAGE_FTYPE_SEQ, 0x80, 300);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t owner = open_name(&unit, &bank, 2, "0:FILE");
    const uint8_t peer = open_name(&unit, &bank, 3, "1:PEER");
    ck_assert_uint_ne(owner, DOS_UNUSED);
    ck_assert_uint_ne(peer, DOS_UNUSED);
    unit.channels[owner].active = unit.channels[peer].active = _i;
    unit.buffer_drives |= (uint16_t)(1u << unit.channels[owner].buffers[_i]);
    unit.buffer_drives &= (uint16_t)~(1u << unit.channels[peer].buffers[_i]);
    const uint8_t listing = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, "$0");
    ck_assert_uint_ne(listing, DOS_UNUSED);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), owner);
    ck_assert_uint_eq(dos_find_channel(&unit, 3), DOS_UNUSED);
    uint8_t byte;
    bool eoi;
    ck_assert_uint_eq(dos_stage(&unit, &bank, owner, &byte, 1, &eoi), 1);
    ck_assert_uint_eq(byte, (uint8_t)(0x40u + _i * DISKIMAGE_SECTOR_PAYLOAD_SIZE));
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// SCHTBL reserves a final fallback pass when both default preferences occur.
START_TEST(test_directory_both_default_preferences_retain_fallback_pass) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool peer_absent = (unsigned int)_i % 2u != 0;
    dos_media_bank_t bank = {0};
    image_t first, second;
    mount_blank(&bank, &first, format);
    mount_peer(&bank, &second, format);
    fixture_file(&first, format, 0, "ALPHA", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    fixture_file(&first, format, 1, "GAMMA", DISKIMAGE_FTYPE_SEQ, 0x60, 22);
    fixture_file(&first, format, 2, "DELTA", DISKIMAGE_FTYPE_SEQ, 0x80, 22);
    fixture_file(&first, format, 3, "EPSILON", DISKIMAGE_FTYPE_SEQ, 0xa0, 22);
    fixture_file(&second, format, 0, "BETA", DISKIMAGE_FTYPE_SEQ, 0xc0, 22);
    if (peer_absent) {
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, NULL), DOS_MEDIA_OK);
    }
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t listing = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, "$0:ALPHA,1:BETA,GAMMA,0:DELTA,EPSILON");
    ck_assert_uint_ne(listing, DOS_UNUSED);
    uint8_t bytes[352];
    ck_assert_uint_eq(directory_bytes(&unit, &bank, listing, bytes, sizeof(bytes)), peer_absent ? 192 : sizeof(bytes));
    ck_assert_mem_eq(bytes + 40, "ALPHA", 5);
    ck_assert_mem_eq(bytes + (peer_absent ? 104 : 72), "DELTA", 5);
    ck_assert_mem_eq(bytes + (peer_absent ? 136 : 104), "EPSILON", 7);
    ck_assert_mem_eq(bytes + (peer_absent ? 72 : 296), "GAMMA", 5);
    if (!peer_absent) {
        ck_assert_mem_eq(bytes + 200, "BETA", 4);
    }
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// CMPCHK retains zero FILTRK, and 4040 retains a negative FILTRK as a wildcard.
START_TEST(test_directory_completion_preserves_raw_track_state) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int scenario = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0};
    image_t first, second;
    mount_blank(&bank, &first, format);
    mount_peer(&bank, &second, format);
    fixture_file(&first, format, 0, "FILE", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    fixture_file(&second, format, 0, "FILE", DISKIMAGE_FTYPE_USR, 0x80, 22);
    sector_bytes(&first, bank.units[0].slots[0].io.image.type, format == 0 ? 18 : 39, 1)[3] = scenario == 0 ? 0 : 0x80;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t listing = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, scenario == 2 ? "$0:A,FILE,1:B,OTHER" : "$FILE");
    ck_assert_uint_ne(listing, DOS_UNUSED);
    uint8_t bytes[288];
    const unsigned int expected = scenario == 2 ? (format == 0 ? 288 : 224) : scenario == 0 || format == 0 ? 192 : 96;
    ck_assert_uint_eq(directory_bytes(&unit, &bank, listing, bytes, sizeof(bytes)), expected);
    ck_assert_mem_eq(bytes + 40, "FILE", 4);
    ck_assert_mem_eq(bytes + 58, "SEQ", 3);
    if (scenario == 0 || format == 0) {
        ck_assert_mem_eq(bytes + 136, "FILE", 4);
        ck_assert_mem_eq(bytes + 154, "USR", 3);
    }
    if (scenario == 2 && format == 0) {
        // The negative track remains wildcard state after the valid peer match.
        ck_assert_mem_eq(bytes + 232, "FILE", 4);
        ck_assert_mem_eq(bytes + 250, "SEQ", 3);
    }
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// TC35 stops suffix fields at shifted CR and the shared filename-table limit.
START_TEST(test_directory_suffix_respects_parser_boundaries) {
    static const struct {
        const char* request;
        unsigned int rows;
        const char* first;
    } cases[] = {
        {"$0:*=S\x8d,U", 1, "SEQUENTIAL"},    {"$0:*=U,S\x8d,P", 1, "SEQUENTIAL"}, {"$0:*=S,\x8dU", 3, "FILE"},
        {"$0:*=S\x8d", 1, "SEQUENTIAL"},      {"$0:*=S,U,P,S,U", 1, "SEQUENTIAL"}, {"$0:NO,NO,NO,*=S,U,P", 1, "FILE"},
        {"$0:NO,NO,NO,NO,FILE=S", 1, "FILE"}, {"$0:NO,NO,NO,NO,*", 1, "FILE"},
    };
    const unsigned int format = (unsigned int)_i / count_of(cases);
    const unsigned int scenario = (unsigned int)_i % count_of(cases);
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "FILE", DISKIMAGE_FTYPE_USR, 0x40, 22);
    fixture_file(&image, format, 1, "SEQUENTIAL", DISKIMAGE_FTYPE_SEQ, 0x60, 22);
    fixture_file(&image, format, 2, "PROGRAM", DISKIMAGE_FTYPE_PRG, 0x80, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t listing = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, cases[scenario].request);
    ck_assert_uint_ne(listing, DOS_UNUSED);
    uint8_t bytes[160];
    ck_assert_uint_eq(directory_bytes(&unit, &bank, listing, bytes, sizeof(bytes)), 64u + 32u * cases[scenario].rows);
    ck_assert_mem_eq(bytes + 40, cases[scenario].first, strlen(cases[scenario].first));
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    free(image.bytes);
}
END_TEST

// SIMPRS/SETANY uses the pre-colon or final byte, not LOADIR's drive grammar.
START_TEST(test_raw_directory_uses_simple_drive_selection) {
    static const struct {
        const char* request;
        uint8_t last;
        uint8_t drive;
        bool default_drive;
    } cases[] = {
        {"$1*", 0, 0, true},     {"$0*", 1, 1, true},       {"$X1", 0, 1, false},      {"$X0", 1, 0, false},
        {"$0:1", 1, 0, false},   {"$X:1", 0, 1, false},     {"$:0", 1, 0, false},      {"$X:Q", 1, 1, true},
        {"$1*,0:", 1, 0, false}, {"$0,,,1:X", 0, 1, false}, {"$0,,,,0:X", 1, 1, true}, {"$", 1, 1, true},
    };
    const unsigned int format = (unsigned int)_i / count_of(cases);
    const unsigned int scenario = (unsigned int)_i % count_of(cases);
    dos_media_bank_t bank = {0};
    image_t first, second;
    mount_blank(&bank, &first, format);
    mount_peer(&bank, &second, format);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    const unsigned int track = format == 0 ? 18u : 39u;
    sector_bytes(&first, type, track, 0)[format == 0 ? 144 : 6] = '0';
    sector_bytes(&second, type, track, 0)[format == 0 ? 144 : 6] = '1';
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    unit.last_drive = cases[scenario].last;
    uint8_t channel = open_name(&unit, &bank, 2, cases[scenario].request);
    ck_assert_uint_ne(channel, DOS_UNUSED);
    ck_assert_uint_eq(unit.channels[channel].media.slot, cases[scenario].drive);
    uint8_t bytes[6u * DISKIMAGE_SECTOR_PAYLOAD_SIZE];
    const unsigned int size = (format == 0 ? 2u : format == 1 ? 4u : 6u) * DISKIMAGE_SECTOR_PAYLOAD_SIZE;
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, bytes, sizeof(bytes)), size);
    ck_assert_mem_eq(
        bytes, sector_bytes(cases[scenario].drive ? &second : &first, type, track, 0) + DISKIMAGE_SECTOR_LINK_SIZE,
        DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));

    // Admission may fall back only when SETANY actually selected LSTDRV.
    dos_reset(&unit, bank.units[0].model);
    unit.last_drive = cases[scenario].last;
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, cases[scenario].drive, NULL), DOS_MEDIA_OK);
    channel = open_name(&unit, &bank, 2, cases[scenario].request);
    if (cases[scenario].default_drive) {
        ck_assert_uint_ne(channel, DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        ck_assert_uint_eq(unit.channels[channel].media.slot, cases[scenario].drive ^ 1u);
        ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, bytes, sizeof(bytes)), size);
        ck_assert_mem_eq(
            bytes, sector_bytes(cases[scenario].drive ? &first : &second, type, track, 0) + DISKIMAGE_SECTOR_LINK_SIZE,
            DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    } else {
        ck_assert_uint_eq(channel, DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_NOT_READY);
        ck_assert_uint_eq(unit.buffer_use, 0);
    }
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// FFST returns a zero-track name match to OP90 without searching another link.
START_TEST(test_open_zero_track_match_stops_search) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int scenario = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0};
    image_t first, second;
    mount_blank(&bank, &first, format);
    mount_peer(&bank, &second, format);
    const uint8_t type = scenario == 2 ? DISKIMAGE_FTYPE_PRG : DISKIMAGE_FTYPE_SEQ;
    fixture_file(&first, format, 0, "FILE", type, 0x40, 22);
    fixture_file(&second, format, 0, "FILE", type, 0x80, 22);
    const diskimage_type_t image_type = bank.units[0].slots[0].io.image.type;
    const unsigned int track = format == 0 ? 18u : 39u;
    uint8_t* const directory = sector_bytes(&first, image_type, track, 1);
    if (scenario == 1) {
        memcpy(sector_bytes(&first, image_type, track, 2), directory, DISKIMAGE_SECTOR_SIZE);
        directory[0] = track;
        directory[1] = 2;
    }
    directory[3] = 0;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const unsigned int peer_reads = second.reads;
    const uint8_t channel = open_name(&unit, &bank, scenario == 2 ? IEEE_LOAD_CHANNEL : 2,
                                      scenario == 2   ? "*"
                                      : scenario == 1 ? "0:FILE"
                                                      : "FILE");
    ck_assert_uint_eq(channel, DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NOT_FOUND);
    ck_assert_uint_eq(unit.selected_drive, 0);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(second.reads, peer_reads);
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// OP90/OP130 decode six track bits on 4040, but DOS 2.7 retains the whole byte.
START_TEST(test_open_decodes_model_specific_track_bits) {
    static const uint8_t tracks[] = {0x40, 0x41, 0x80, 0x81, 0xc1};
    const unsigned int format = (unsigned int)_i / count_of(tracks);
    const unsigned int scenario = (unsigned int)_i % count_of(tracks);
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "FILE", DISKIMAGE_FTYPE_PRG, 0x40, 22);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    sector_bytes(&image, type, format == 0 ? 18u : 39u, 1)[3] = tracks[scenario];
    const bool missing = format == 0 && (scenario == 0 || scenario == 2);
    const bool invalid = format != 0 && tracks[scenario] > (format == 1 ? 77u : 154u);
    if (format != 0 && !invalid) {
        uint8_t* const sector = sector_bytes(&image, type, tracks[scenario], 0);
        sector[0] = 0;
        sector[1] = 23;
        for (unsigned int byte = 0; byte < 22; ++byte) {
            sector[byte + DISKIMAGE_SECTOR_LINK_SIZE] = 0xa0u + byte;
        }
    }
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    uint8_t channel = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, "0:FILE");
    if (missing || invalid) {
        ck_assert_uint_eq(channel, DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, missing ? DOS_STATUS_NOT_FOUND : DOS_STATUS_READ_ERROR);
        ck_assert_uint_eq(unit.buffer_use, 0);
    } else {
        ck_assert_uint_ne(channel, DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        expect_bytes(&unit, &bank, channel, 22, format == 0 ? 0x40 : 0xa0);
        ck_assert(dos_close(&unit, &bank, bank.units[0].model, IEEE_LOAD_CHANNEL));
        channel = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, "*");
        ck_assert_uint_ne(channel, DOS_UNUSED);
        expect_bytes(&unit, &bank, channel, 22, format == 0 ? 0x40 : 0xa0);
    }
    free(image.bytes);
}
END_TEST

// Write deterministic bytes through decoded ordinary data, not the name buffer.
static void put_bytes(dos_unit_t* const unit, dos_media_bank_t* const bank, const uint8_t secondary,
                      const unsigned int first, const unsigned int count) {
    for (unsigned int index = 0; index < count; ++index) {
        dos_put(unit, bank, secondary, (uint8_t)(first + index));
    }
}

// Resolve the fixture's independently laid-out BAM entries, not DOS cache state.
static uint8_t* fixture_bam_entry(image_t* const image, const unsigned int format, const unsigned int track) {
    const diskimage_type_t type = format == 0   ? diskimage_type_d64
                                  : format == 1 ? diskimage_type_d80
                                                : diskimage_type_d82;
    return format == 0 ? sector_bytes(image, type, 18, 0) + track * 4u
                       : sector_bytes(image, type, 38, ((track - 1u) / 50u) * 3u) + 6u + ((track - 1u) % 50u) * 5u;
}

// Leave only a requested number of native data blocks, preserving directory metadata.
static void limit_free_blocks(image_t* const image, const unsigned int format, const unsigned int count) {
    const unsigned int tracks = format == 0 ? 35u : format == 1 ? 77u : 154u;
    for (unsigned int track = 1; track <= tracks; ++track) {
        if (track != (format == 0 ? 18u : 39u)) {
            memset(fixture_bam_entry(image, format, track), 0, format == 0 ? 4u : 5u);
        }
    }
    uint8_t* const entry = fixture_bam_entry(image, format, format == 0 ? 17u : 37u);
    entry[0] = count;
    entry[1] = (uint8_t)((1u << count) - 1u);
}

// Populate every first-directory slot without borrowing the production writer.
static void fill_directory(image_t* const image, const unsigned int format) {
    const diskimage_type_t type = format == 0   ? diskimage_type_d64
                                  : format == 1 ? diskimage_type_d80
                                                : diskimage_type_d82;
    uint8_t* const bytes = sector_bytes(image, type, format == 0 ? 18u : 39u, 1);
    for (unsigned int entry = 0; entry < DISKIMAGE_SECTOR_SIZE; entry += DISKIMAGE_DIRECTORY_ENTRY_SIZE) {
        bytes[entry + 2] = 0x81;
        bytes[entry + 3] = 1;
        memset(bytes + entry + 5, 0xa0, DISKIMAGE_FILENAME_SIZE);
        bytes[entry + 5] = 'A' + entry / DISKIMAGE_DIRECTORY_ENTRY_SIZE;
    }
}

// CLSWRT preserves empty/one-byte quirks and exact 254-byte sector boundaries.
START_TEST(test_native_create_close_and_boundaries) {
    static const unsigned int lengths[] = {0, 1, 253, 254, 255, 508, 509};
    const unsigned int format = (unsigned int)_i / count_of(lengths);
    const unsigned int length = lengths[(unsigned int)_i % count_of(lengths)];
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t writer = open_name(&unit, &bank, 2, "0:FILE,S,WRITE");
    ck_assert_uint_ne(writer, DOS_UNUSED);
    ck_assert_uint_eq(unit.channels[writer].first_track, format == 0 ? 17 : 38);
    ck_assert_uint_eq(unit.channels[writer].first_sector, format == 0 ? 0 : 1);
    put_bytes(&unit, &bank, 2, 0x40, length);
    ck_assert(dos_close(&unit, &bank, model, 2));
    ck_assert_uint_eq(unit.buffer_use, 0);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    const uint8_t* const directory = sector_bytes(&image, type, format == 0 ? 18 : 39, 1);
    ck_assert_uint_eq(directory[2], 0x81);
    ck_assert_mem_eq(directory + 5, "FILE", 4);
    const unsigned int blocks = length == 0 ? 1u : (length + 253u) / 254u;
    ck_assert_uint_eq(directory[30] | ((unsigned int)directory[31] << 8u), blocks);
    // The media is persisted before any readback can mask a missing write.
    ck_assert_uint_gt(image.flushes, 0);
    ck_assert(!bank.units[0].slots[0].dirty);
    const uint8_t reader = open_name(&unit, &bank, 3, "0:F?L*,S,READ");
    ck_assert_uint_ne(reader, DOS_UNUSED);
    expect_bytes(&unit, &bank, reader, length, 0x40);
    ck_assert(dos_close(&unit, &bank, model, 3));
    free(image.bytes);
}
END_TEST

// APPEND retains the old payload and replacement publishes its new chain only on CLOSE.
START_TEST(test_native_append_replace_and_peer_directory_entries) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, (unsigned int)_i);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:FILE,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x20, 300);
    ck_assert(dos_close(&unit, &bank, model, 2));
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:FILE,A"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x20 + 300, 50);
    ck_assert(dos_close(&unit, &bank, model, 2));
    uint8_t reader = open_name(&unit, &bank, 3, "0:FILE");
    expect_bytes(&unit, &bank, reader, 350, 0x20);
    ck_assert(dos_close(&unit, &bank, model, 3));
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "@0:FILE,S,W"), DOS_UNUSED);
    ck_assert_uint_ne(open_name(&unit, &bank, 4, "0:PEER,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x60, 22);
    put_bytes(&unit, &bank, 4, 0x70, 12);
    ck_assert(dos_close(&unit, &bank, model, 4));
    ck_assert(dos_close(&unit, &bank, model, 2));
    reader = open_name(&unit, &bank, 3, "0:FILE");
    expect_bytes(&unit, &bank, reader, 22, 0x60);
    ck_assert(dos_close(&unit, &bank, model, 3));
    reader = open_name(&unit, &bank, 3, "0:PEER");
    expect_bytes(&unit, &bank, reader, 12, 0x70);
    ck_assert(dos_close(&unit, &bank, model, 3));
    free(image.bytes);
}
END_TEST

// Partial writes and flush errors keep the exact CLOSE phase and native owners retryable.
START_TEST(test_close_storage_failures_retain_and_recover) {
    const unsigned int format = (unsigned int)_i / 4u, phase = (unsigned int)_i % 4u;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t writer = open_name(&unit, &bank, 2, "0:FILE,S,W");
    ck_assert_uint_ne(writer, DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x40, 20);
    if (phase == 3) {
        image.fail_flush = true;
    } else {
        image.fail_write = image.writes + phase + 1u;
    }
    ck_assert(!dos_close(&unit, &bank, model, 2));
    ck_assert_uint_eq(dos_find_channel(&unit, 2), writer);
    ck_assert_uint_ne(unit.buffer_use, 0);
    ck_assert_mem_eq(unit.status, "25, WRITE ERROR,", strlen("25, WRITE ERROR,"));
    ck_assert(bank.units[0].slots[0].dirty);
    image.fail_flush = false;
    image.fail_write = 0;
    ck_assert(dos_close(&unit, &bank, model, 2));
    ck_assert_uint_eq(unit.buffer_use, 0);
    const uint8_t reader = open_name(&unit, &bank, 3, "0:FILE");
    ck_assert_uint_ne(reader, DOS_UNUSED);
    expect_bytes(&unit, &bank, reader, 20, 0x40);
    ck_assert(dos_close(&unit, &bank, model, 3));
    free(image.bytes);
}
END_TEST

// OP77 discards an unclosed entry without freeing its chain, before normal W checks.
START_TEST(test_write_errors_and_unclosed_recreation) {
    const unsigned int format = (unsigned int)_i;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_eq(open_name(&unit, &bank, 2, "0:F*,S,W"), DOS_UNUSED);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_INVALID_NAME);
    ck_assert_uint_eq(open_name(&unit, &bank, 2, "@0:F*,S,W"), DOS_UNUSED);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_TYPE_MISMATCH);
    uint8_t writer = open_name(&unit, &bank, 2, "0:FILE,S,W");
    ck_assert_uint_ne(writer, DOS_UNUSED);
    const uint8_t old_track = unit.channels[writer].first_track;
    const uint8_t old_sector = unit.channels[writer].first_sector;
    put_bytes(&unit, &bank, 2, 0x20, 22);
    ck_assert(dos_close(&unit, &bank, model, 2));
    ck_assert_uint_eq(open_name(&unit, &bank, 2, "0:FILE,U,W"), DOS_UNUSED);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_FILE_EXISTS);
    uint8_t* const entry = sector_bytes(&image, type, format == 0 ? 18u : 39u, 1);
    entry[2] &= 0x7f;
    ck_assert_uint_eq(open_name(&unit, &bank, 3, "0:FILE"), DOS_UNUSED);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_FILE_OPEN);
    writer = open_name(&unit, &bank, 2, "0:FILE,U,W");
    ck_assert_uint_ne(writer, DOS_UNUSED);
    ck_assert(!unit.channels[writer].replaced);
    put_bytes(&unit, &bank, 2, 0x40, 22);
    ck_assert(dos_close(&unit, &bank, model, 2));
    ck_assert_uint_eq(entry[2], 0x83);
    ck_assert_uint_eq(fixture_bam_entry(&image, format, old_track)[1u + old_sector / 8u] & (1u << (old_sector & 7u)),
                      0);
    const uint8_t reader = open_name(&unit, &bank, 3, "0:FILE,U,R");
    ck_assert_uint_ne(reader, DOS_UNUSED);
    expect_bytes(&unit, &bank, reader, 22, 0x40);
    free(image.bytes);
}
END_TEST

// USEDT3's near-full warning is transient, ENDCMD publishes OK after OPEN/CLOSE.
START_TEST(test_near_full_warning_and_full_channel_release) {
    const unsigned int format = (unsigned int)_i;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    limit_free_blocks(&image, format, 3);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:FILE,S,W"), DOS_UNUSED);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_OK);
    put_bytes(&unit, &bank, 2, 0x20, DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    ck_assert_int_eq(unit.status_code, format == 0 ? DOS_STATUS_OK : DOS_STATUS_DISK_FULL);
    ck_assert_uint_ne(dos_find_channel(&unit, 2), DOS_UNUSED);
    ck_assert(dos_close(&unit, &bank, model, 2));
    ck_assert_int_eq(unit.status_code, DOS_STATUS_OK);
    free(image.bytes);

    bank = (dos_media_bank_t){0};
    mount_blank(&bank, &image, format);
    limit_free_blocks(&image, format, 1);
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:FULL,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x40, DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_DISK_FULL);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), DOS_UNUSED);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert(dos_close(&unit, &bank, model, 2));
    ck_assert_int_eq(unit.status_code, DOS_STATUS_OK);
    free(image.bytes);
}
END_TEST

// A failed link/data write retains both the full sector and its reserved successor.
START_TEST(test_put_write_failure_retries_without_losing_data) {
    const unsigned int format = (unsigned int)_i;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t writer = open_name(&unit, &bank, 2, "0:FILE,S,W");
    image.fail_write = image.writes + 1u;
    put_bytes(&unit, &bank, 2, 0x20, DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), writer);
    ck_assert_uint_ne(unit.channels[writer].next_track, 0);
    image.fail_write = 0;
    ck_assert(dos_close(&unit, &bank, model, 2));
    const uint8_t reader = open_name(&unit, &bank, 3, "0:FILE");
    expect_bytes(&unit, &bank, reader, DISKIMAGE_SECTOR_PAYLOAD_SIZE, 0x20);
    free(image.bytes);
}
END_TEST

// NXDRBK retries partial tail, initialization and entry writes without erasing peers.
START_TEST(test_directory_growth_storage_failure_and_recovery) {
    const unsigned int format = (unsigned int)_i / 3u, phase = (unsigned int)_i % 3u;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    fill_directory(&image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    image.fail_write = image.writes + phase + 1u;
    const uint8_t writer = open_name(&unit, &bank, 2, "0:FILE,S,W");
    ck_assert_uint_ne(writer, DOS_UNUSED);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    image.fail_write = 0;
    if (phase < 2) {
        ck_assert_uint_eq(open_name(&unit, &bank, 4, "0:PEER,S,W"), DOS_UNUSED);
        ck_assert_int_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    } else {
        ck_assert_uint_ne(open_name(&unit, &bank, 4, "0:PEER,S,W"), DOS_UNUSED);
        put_bytes(&unit, &bank, 4, 0x60, 22);
        ck_assert(dos_close(&unit, &bank, model, 4));
    }
    ck_assert(dos_close(&unit, &bank, model, 2));
    const uint8_t reader = open_name(&unit, &bank, 3, "0:FILE");
    ck_assert_uint_ne(reader, DOS_UNUSED);
    expect_bytes(&unit, &bank, reader, 0, 0);
    ck_assert(dos_close(&unit, &bank, model, 3));
    if (phase == 2) {
        const uint8_t peer = open_name(&unit, &bank, 3, "0:PEER");
        ck_assert_uint_ne(peer, DOS_UNUSED);
        expect_bytes(&unit, &bank, peer, 22, 0x60);
    }
    free(image.bytes);
}
END_TEST

// Directory exhaustion must not leave a closable writer pointing at the header.
START_TEST(test_directory_full_releases_failed_open_safely) {
    const unsigned int format = (unsigned int)_i;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    fill_directory(&image, format);
    memset(fixture_bam_entry(&image, format, format == 0 ? 18u : 39u), 0, format == 0 ? 4u : 5u);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    uint8_t header[DISKIMAGE_SECTOR_SIZE];
    memcpy(header, sector_bytes(&image, type, format == 0 ? 18u : 39u, 0), sizeof(header));
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    ck_assert_uint_eq(open_name(&unit, &bank, 2, "0:FILE,S,W"), DOS_UNUSED);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_DISK_FULL);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    ck_assert_mem_eq(header, sector_bytes(&image, type, format == 0 ? 18u : 39u, 0), sizeof(header));
    free(image.bytes);
}
END_TEST

// APPEND yields between data sectors and abandons an opening read fault without writes.
START_TEST(test_append_lookup_yields_and_failed_read_preserves_media) {
    const unsigned int format = (unsigned int)_i;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:FILE,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x20, 509);
    ck_assert(dos_close(&unit, &bank, model, 2));
    const unsigned int writes = image.writes;
    const uint8_t name[] = "0:FILE,A";
    dos_open(&unit, &bank, 0, 2, name, sizeof(name) - 1);
    dos_work(&unit, &bank);
    ck_assert(dos_work_pending(&unit));
    const unsigned int reads = image.reads;
    dos_work(&unit, &bank);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert(dos_work_pending(&unit));
    image.fail_read = image.reads + 1u;
    finish_open(&unit, &bank);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), DOS_UNUSED);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(unit.buffer_use, 0);
    image.fail_read = 0;
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:FILE,A"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x20 + 509, 22);
    ck_assert(dos_close(&unit, &bank, model, 2));
    const uint8_t reader = open_name(&unit, &bank, 3, "0:FILE");
    expect_bytes(&unit, &bank, reader, 531, 0x20);
    free(image.bytes);
}
END_TEST

// Read-only native media rejects WRITE/APPEND before allocation or mutation.
START_TEST(test_read_only_create_and_append_leave_media_unchanged) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, (unsigned int)_i);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:FILE,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x20, 22);
    ck_assert(dos_close(&unit, &bank, model, 2));
    const unsigned int writes = image.writes;
    bank.units[0].slots[0].io.image.write = NULL;
    ck_assert_uint_eq(open_name(&unit, &bank, 2, "0:NEXT,S,W"), DOS_UNUSED);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_WRITE_PROTECT);
    ck_assert_uint_eq(open_name(&unit, &bank, 2, "0:FILE,A"), DOS_UNUSED);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_WRITE_PROTECT);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(unit.buffer_use, 0);
    const uint8_t reader = open_name(&unit, &bank, 3, "0:FILE");
    expect_bytes(&unit, &bank, reader, 22, 0x20);
    free(image.bytes);
}
END_TEST

// SA 1 forces SAVE, remembers its first link and does not clear errors on CLOSE.
START_TEST(test_save_override_last_program_and_status_retention) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, (unsigned int)_i);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t writer = open_name(&unit, &bank, IEEE_SAVE_CHANNEL, "0:PROGRAM,R");
    ck_assert_uint_ne(writer, DOS_UNUSED);
    ck_assert_int_eq(unit.channels[writer].access, DOS_ACCESS_WRITE);
    ck_assert_uint_eq(unit.channels[writer].file_type, DISKIMAGE_FTYPE_PRG);
    put_bytes(&unit, &bank, IEEE_SAVE_CHANNEL, 0x40, 255);
    dos_status(&unit, model, DOS_STATUS_SYNTAX, 0, 0);
    ck_assert(dos_close(&unit, &bank, model, IEEE_SAVE_CHANNEL));
    ck_assert_int_eq(unit.status_code, DOS_STATUS_SYNTAX);
    const uint8_t reader = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, "*");
    ck_assert_uint_ne(reader, DOS_UNUSED);
    expect_bytes(&unit, &bank, reader, 255, 0x40);
    free(image.bytes);
}
END_TEST

// SETMAP retains the dirty BAM if allocating on another range cannot write it.
START_TEST(test_allocation_bam_write_failure_preserves_peer_and_retries) {
    const unsigned int format = (unsigned int)_i;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    limit_free_blocks(&image, format, 0);
    uint8_t* entry = fixture_bam_entry(&image, format, 38);
    entry[0] = 1;
    entry[1] = 1u << 1u;
    entry = fixture_bam_entry(&image, format, format == 1 ? 60u : 120u);
    entry[0] = entry[1] = 1;
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t peer = open_name(&unit, &bank, 2, "0:PEER,S,W");
    ck_assert_uint_ne(peer, DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x40, 22);
    image.fail_write = image.writes + 1u;
    ck_assert_uint_eq(open_name(&unit, &bank, 3, "0:NEXT,S,W"), DOS_UNUSED);
    ck_assert_int_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), peer);
    ck_assert(unit.bam[0].dirty);
    image.fail_write = 0;
    ck_assert_uint_ne(open_name(&unit, &bank, 3, "0:NEXT,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 3, 0x60, 22);
    ck_assert(dos_close(&unit, &bank, model, 2));
    ck_assert(dos_close(&unit, &bank, model, 3));
    uint8_t reader = open_name(&unit, &bank, 2, "0:PEER");
    expect_bytes(&unit, &bank, reader, 22, 0x40);
    ck_assert(dos_close(&unit, &bank, model, 2));
    reader = open_name(&unit, &bank, 3, "0:NEXT");
    expect_bytes(&unit, &bank, reader, 22, 0x60);
    free(image.bytes);
}
END_TEST

// JSCRBM/AVCK reject an inconsistent count even outside the allocated track.
START_TEST(test_bam_count_mismatch_is_not_persisted_as_success) {
    const unsigned int format = (unsigned int)_i;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    --fixture_bam_entry(&image, format, 1)[0];
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t writer = open_name(&unit, &bank, 2, "0:FILE,S,W");
    ck_assert_uint_ne(writer, DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x40, 22);
    ck_assert(!dos_close(&unit, &bank, model, 2));
    ck_assert_int_eq(unit.status_code, DOS_STATUS_DIRECTORY_ERROR);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), writer);
    ck_assert(unit.bam[0].dirty);
    free(image.bytes);
}
END_TEST

// Both native disks retain independent live writers and bare '$' lists both.
START_TEST(test_dual_drive_writers_and_directory_selection) {
    const unsigned int format = (unsigned int)_i;
    image_t first, second;
    dos_media_bank_t bank = {0}, other = {0};
    mount_blank(&bank, &first, format);
    mount_blank(&other, &second, format);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:FIRST,S,W"), DOS_UNUSED);
    ck_assert_uint_ne(open_name(&unit, &bank, 3, "1:SECOND,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x20, 255);
    put_bytes(&unit, &bank, 3, 0x60, 509);
    ck_assert(dos_close(&unit, &bank, model, 2));
    ck_assert(dos_close(&unit, &bank, model, 3));
    uint8_t channel = open_name(&unit, &bank, 0, "$");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    uint8_t bytes[192];
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, bytes, sizeof(bytes)), sizeof(bytes));
    ck_assert_uint_eq(bytes[4], 1);
    ck_assert_uint_eq(bytes[98], 0);
    ck_assert(dos_close(&unit, &bank, model, 0));
    channel = open_name(&unit, &bank, 2, "0:FIRST");
    expect_bytes(&unit, &bank, channel, 255, 0x20);
    ck_assert(dos_close(&unit, &bank, model, 2));
    channel = open_name(&unit, &bank, 3, "1:SECOND");
    expect_bytes(&unit, &bank, channel, 509, 0x60);
    ck_assert(dos_close(&unit, &bank, model, 3));
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, NULL), DOS_MEDIA_OK);
    dos_reset(&unit, model);
    channel = open_name(&unit, &bank, 0, "$");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, bytes, sizeof(bytes)), 96);
    ck_assert_uint_eq(bytes[4], 1);
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// Failed finalization retains completed owners, the failing phase and untouched peers.
START_TEST(test_finalize_writers_retains_owners_and_retries) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, (unsigned int)_i);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t first = open_name(&unit, &bank, 2, "0:FIRST,S,W");
    const uint8_t second = open_name(&unit, &bank, 3, "0:SECOND,S,W");
    const uint8_t peer = open_name(&unit, &bank, 4, "0:PEER,S,W");
    ck_assert_uint_ne(first, DOS_UNUSED);
    ck_assert_uint_ne(second, DOS_UNUSED);
    ck_assert_uint_ne(peer, DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x20, 22);
    put_bytes(&unit, &bank, 3, 0x40, 22);
    put_bytes(&unit, &bank, 4, 0x60, 22);
    const dos_channel_t saved_peer = unit.channels[peer];
    const uint16_t buffers = unit.buffer_use;
    // FIRST persists data, directory and shared BAM before SECOND's data fails.
    image.fail_write = image.writes + 4u;
    ck_assert(!dos_finalize(&unit, &bank));
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(unit.channels[first].closing, DOS_CLOSE_FLUSH);
    ck_assert_uint_eq(unit.channels[second].closing, DOS_CLOSE_OLD_CHAIN);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    ck_assert_uint_eq(unit.buffer_use, buffers);
    image.fail_write = 0;
    ck_assert(dos_finalize(&unit, &bank));
    ck_assert_uint_eq(dos_find_channel(&unit, 2), first);
    ck_assert_uint_eq(dos_find_channel(&unit, 3), second);
    ck_assert_uint_eq(dos_find_channel(&unit, 4), peer);
    ck_assert(dos_close(&unit, &bank, model, IEEE_COMMAND_CHANNEL));
    ck_assert_uint_eq(unit.buffer_use, 0);
    const uint8_t reader = open_name(&unit, &bank, 2, "0:SECOND");
    ck_assert_uint_ne(reader, DOS_UNUSED);
    expect_bytes(&unit, &bank, reader, 22, 0x40);
    ck_assert(dos_close(&unit, &bank, model, 2));
    free(image.bytes);
}
END_TEST

// Quiescing an unfinished APPEND releases its read cursor without rewriting the file.
START_TEST(test_finalize_cancels_unfinished_append_without_writes) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, (unsigned int)_i);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:FILE,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x20, 509);
    ck_assert(dos_close(&unit, &bank, model, 2));
    const unsigned int writes = image.writes, flushes = image.flushes;
    const uint8_t append[] = "0:FILE,A";
    dos_open(&unit, &bank, 0, 2, append, sizeof(append) - 1u);
    dos_work(&unit, &bank);
    ck_assert(dos_work_pending(&unit));
    ck_assert_uint_ne(unit.open.append_channel, 0);
    ck_assert(dos_finalize(&unit, &bank));
    ck_assert(!dos_work_pending(&unit));
    const uint8_t retired[DOS_COMMAND_LIMIT] = {0};
    ck_assert_mem_eq(unit.command_bytes, retired, sizeof(retired));
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(image.flushes, flushes);
    const uint8_t reader = open_name(&unit, &bank, 3, "0:FILE");
    ck_assert_uint_ne(reader, DOS_UNUSED);
    expect_bytes(&unit, &bank, reader, 509, 0x20);
    ck_assert(dos_close(&unit, &bank, model, 3));
    free(image.bytes);
}
END_TEST

// CLSALL stops at the failing secondary, leaving lower secondaries untouched.
START_TEST(test_close_all_retains_descending_failure_boundary) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, (unsigned int)_i);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 14, "0:HIGH,S,W"), DOS_UNUSED);
    const uint8_t low = open_name(&unit, &bank, 2, "0:LOW,S,W");
    const uint8_t middle = open_name(&unit, &bank, 7, "0:MIDDLE,S,W");
    ck_assert_uint_ne(low, DOS_UNUSED);
    ck_assert_uint_ne(middle, DOS_UNUSED);
    put_bytes(&unit, &bank, 14, 0x20, 22);
    put_bytes(&unit, &bank, 2, 0x40, 22);
    put_bytes(&unit, &bank, 7, 0x60, 22);
    const dos_channel_t saved_low = unit.channels[low];
    image.fail_write = image.writes + 4u;
    ck_assert(!dos_close(&unit, &bank, model, IEEE_COMMAND_CHANNEL));
    ck_assert_uint_eq(dos_find_channel(&unit, 14), DOS_UNUSED);
    ck_assert_uint_eq(dos_find_channel(&unit, 7), middle);
    ck_assert_uint_eq(unit.channels[middle].closing, DOS_CLOSE_OLD_CHAIN);
    ck_assert_mem_eq(&unit.channels[low], &saved_low, sizeof(saved_low));
    image.fail_write = 0;
    ck_assert(dos_close(&unit, &bank, model, IEEE_COMMAND_CHANNEL));
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// Ordinary metadata writes accept native/unspecified BAM versions, not foreign ones.
START_TEST(test_ordinary_write_version_gate) {
    const unsigned int format = (unsigned int)_i / 3u, version = (unsigned int)_i % 3u;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    const unsigned int parts = format == 0 ? 1u : format == 1 ? 2u : 4u;
    for (unsigned int part = 0; part < parts; ++part) {
        sector_bytes(&image, type, format == 0 ? 18u : 38u, part * 3u)[2] = version == 0   ? (format == 0 ? 'A' : 'C')
                                                                            : version == 1 ? 0
                                                                                           : 0xff;
    }
    // A DOS 2.7 directory header's version is not the cached map's version.
    if (format != 0) {
        sector_bytes(&image, type, 39, 0)[2] = 0xff;
    }
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t writer = open_name(&unit, &bank, 2, "0:FILE,S,W");
    if (version == 2) {
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_POWER_ON);
        ck_assert_uint_eq(image.writes, 0);
        ck_assert_uint_eq(image.flushes, 0);
    } else {
        ck_assert_uint_ne(writer, DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        put_bytes(&unit, &bank, 2, 0x20, 22);
        ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    }
    free(image.bytes);
}
END_TEST

// Released writers leave allocation and flush work for lifecycle finalization.
START_TEST(test_finalize_released_writer_persists_unit_state) {
    const unsigned int format = (unsigned int)_i / 3u, fault = (unsigned int)_i % 3u;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    limit_free_blocks(&image, format, 1);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:F,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x20, DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_DISK_FULL);
    ck_assert(bank.units[0].slots[0].dirty);
    image.fail_flush = fault == 1;
    image.fail_write = fault == 2 ? image.writes + 1u : 0;
    if (fault == 1 || (fault == 2 && format == 0)) {
        ck_assert(!dos_finalize(&unit, &bank));
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
        ck_assert(bank.units[0].slots[0].dirty);
        if (fault == 2) {
            ck_assert(unit.bam[0].dirty);
        }
    } else {
        ck_assert(dos_finalize(&unit, &bank));
    }
    image.fail_flush = false;
    image.fail_write = 0;
    ck_assert(dos_finalize(&unit, &bank));
    ck_assert(!unit.bam[0].dirty);
    ck_assert(!bank.units[0].slots[0].dirty);
    ck_assert_uint_gt(image.flushes, 0);
    ck_assert_uint_eq(fixture_bam_entry(&image, format, format == 0 ? 17 : 37)[0], 0);
    const unsigned int flushes = image.flushes;
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, NULL), DOS_MEDIA_OK);
    ck_assert_uint_eq(image.flushes, flushes);
    free(image.bytes);
}
END_TEST

// Terminal failures retire command RAM without discarding retryable writers.
START_TEST(test_failed_write_open_retires_command_bytes) {
    const unsigned int format = (unsigned int)_i / 7u, fault = (unsigned int)_i % 7u;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    if (fault >= 1 && fault <= 3) {
        fixture_file(&image, format, 0, "F", DISKIMAGE_FTYPE_SEQ, 0x40, fault == 1 ? 509 : 22);
    } else if (fault == 5) {
        fixture_file(&image, format, 0, "F", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
        sector_bytes(&image, bank.units[0].slots[0].io.image.type, format == 0 ? 18 : 39, 1)[2] &= 0x7f;
    } else if (fault == 6) {
        fill_directory(&image, format);
    }
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    if (fault == 4) {
        ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:OLD,S,W"), DOS_UNUSED);
    } else if (fault == 6) {
        image.fail_write = image.writes + 1u;
        ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:GROW,S,W"), DOS_UNUSED);
    }
    unit.command_bytes[DOS_COMMAND_LIMIT] = 0x5a;
    if (fault == 1) {
        const uint8_t request[] = "F,A";
        dos_open(&unit, &bank, 0, 2, request, sizeof(request) - 1u);
        dos_work(&unit, &bank);
        ck_assert_uint_ne(unit.open.append_channel, 0);
        ck_assert_mem_eq(unit.command_bytes, request, sizeof(request) - 1u);
        image.fail_read = image.reads + 1u;
        finish_open(&unit, &bank);
    } else {
        if (fault == 2) {
            sector_bytes(&image, bank.units[0].slots[0].io.image.type, 1, 0)[1] = 0;
        } else if (fault != 6) {
            image.fail_write = image.writes + 1u;
        }
        open_name(&unit, &bank, 2, fault >= 1 && fault <= 3 ? "F,A" : fault == 6 ? "NEW,S,W" : "F,S,W");
    }
    ck_assert(!dos_work_pending(&unit));
    ck_assert_uint_eq(unit.status_code, fault == 1   ? DOS_STATUS_READ_ERROR
                                        : fault == 2 ? DOS_STATUS_DIRECTORY_ERROR
                                                     : DOS_STATUS_WRITE_ERROR);
    const uint8_t retired[DOS_COMMAND_LIMIT] = {0};
    ck_assert_mem_eq(unit.command_bytes, retired, sizeof(retired));
    ck_assert_uint_eq(unit.command_bytes[DOS_COMMAND_LIMIT], 0x5a);
    const uint8_t owner = dos_find_channel(&unit, 2);
    ck_assert_uint_eq(owner == DOS_UNUSED, fault == 1 || fault == 2 || fault == 5);
    image.fail_read = 0;
    image.fail_write = 0;
    // NEW must not inherit W from the failed five-byte command.
    const unsigned int writes = image.writes;
    ck_assert_uint_eq(open_name(&unit, &bank, 3, "NEW,"), DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, fault == 6 ? DOS_STATUS_WRITE_ERROR : DOS_STATUS_NOT_FOUND);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), owner);
    free(image.bytes);
}
END_TEST

// DOS 2.7 JINTTS reports MAXTRK through CMDER2, unlike NXTTS's DISK FULL.
START_TEST(test_initial_full_allocation_preserves_native_status) {
    const unsigned int format = (unsigned int)_i;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    limit_free_blocks(&image, format, 0);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    ck_assert_uint_eq(open_name(&unit, &bank, 2, "0:F,S,W"), DOS_UNUSED);
    ck_assert_str_eq(unit.status, format == 0   ? "72, DISK FULL,00,00\r"
                                  : format == 1 ? "67,ILLEGAL TRACK OR SECTOR,78,01,0\r"
                                                : "67,ILLEGAL TRACK OR SECTOR,155,01,0\r");
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// WRT0 selects the data owner's drive before warnings and allocation errors.
START_TEST(test_put_reselects_writer_drive) {
    const unsigned int format = (unsigned int)_i / 2u, count = (unsigned int)_i % 2u ? 3 : 1;
    image_t first, second;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &first, format);
    limit_free_blocks(&first, format, count);
    mount_peer(&bank, &second, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:F,S,W"), DOS_UNUSED);
    const uint8_t peer = open_name(&unit, &bank, 3, "1:PEER,S,W");
    ck_assert_uint_ne(peer, DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x40, DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    ck_assert_uint_eq(unit.selected_drive, 0);
    ck_assert_uint_eq(unit.last_drive, 1);
    ck_assert_uint_eq(dos_find_channel(&unit, 3), peer);
    ck_assert_uint_eq(dos_find_channel(&unit, 2) == DOS_UNUSED, count == 1);
    if (format != 0) {
        ck_assert_str_eq(unit.status, "72, DISK FULL,00,00,0\r");
    }
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// An interleaved reader error must not select APPEND's completion drive.
START_TEST(test_append_resume_reselects_drive) {
    const unsigned int format = (unsigned int)_i;
    image_t first, second;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &first, format);
    mount_peer(&bank, &second, format);
    fixture_file(&first, format, 0, "F", DISKIMAGE_FTYPE_SEQ, 0x40, 509);
    fixture_file(&second, format, 0, "PEER", DISKIMAGE_FTYPE_SEQ, 0x80, 300);
    second.fail_read = 3;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t peer = open_name(&unit, &bank, 3, "1:PEER");
    ck_assert_uint_ne(peer, DOS_UNUSED);
    const uint8_t request[] = "0:F,A";
    dos_open(&unit, &bank, 0, 2, request, sizeof(request) - 1u);
    dos_work(&unit, &bank);
    ck_assert_uint_ne(unit.open.append_channel, 0);
    uint8_t bytes[DOS_STAGE_CAPACITY];
    bool eoi;
    for (unsigned int consumed = 0; consumed < DISKIMAGE_SECTOR_PAYLOAD_SIZE;) {
        const unsigned int count = dos_stage(&unit, &bank, peer, bytes, sizeof(bytes), &eoi);
        ck_assert_uint_gt(count, 0);
        dos_get(&unit, &bank, peer, count);
        consumed += count;
    }
    ck_assert_uint_eq(dos_stage(&unit, &bank, peer, bytes, sizeof(bytes), &eoi), 0);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_uint_eq(unit.selected_drive, 1);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.last_drive, 0);
    ck_assert_str_eq(unit.status, format == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r");
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// Equal pair generations must not alias reservations or growth across slots.
START_TEST(test_directory_writer_ownership_includes_slot) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool growth = (unsigned int)_i % 2u != 0;
    image_t first, second;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &first, format);
    mount_peer(&bank, &second, format);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    const uint8_t track = format == 0 ? 18 : 39;
    fill_directory(&first, format);
    if (growth) {
        fixture_file(&second, format, 0, "PEER", DISKIMAGE_FTYPE_SEQ, 0x80, 22);
        uint8_t* const tail = sector_bytes(&second, type, track, 4);
        memcpy(tail, sector_bytes(&second, type, track, 1), DISKIMAGE_DIRECTORY_ENTRY_SIZE);
        tail[0] = 0;
        tail[1] = DOS_UNUSED;
        fill_directory(&second, format);
        sector_bytes(&second, type, track, 1)[0] = track;
        sector_bytes(&second, type, track, 1)[1] = 4;
        uint8_t* const map = fixture_bam_entry(&second, format, track);
        --map[0];
        map[1] &= (uint8_t)~(1u << 4u);
        first.fail_write = 1;
    } else {
        fill_directory(&second, format);
        sector_bytes(&first, type, track, 1)[226] = 0;
        sector_bytes(&second, type, track, 1)[226] = 0;
    }
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t writer = open_name(&unit, &bank, 2, "0:WRITER,S,W");
    ck_assert_uint_ne(writer, DOS_UNUSED);
    ck_assert_uint_eq(unit.channels[writer].grow_pending, growth);
    const uint8_t peer = open_name(&unit, &bank, 3, growth ? "1:PEER" : "1:NEW,S,W");
    ck_assert_uint_ne(peer, DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    if (growth) {
        expect_bytes(&unit, &bank, peer, 22, 0x80);
        ck_assert(unit.channels[writer].grow_pending);
    } else {
        ck_assert_uint_eq(unit.channels[peer].directory_sector, 1);
        ck_assert_uint_eq(unit.channels[peer].directory_entry, 224);
        ck_assert_uint_eq(sector_bytes(&second, type, track, 1)[0], 0);
    }
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// PARSE retains wildcards past COMPAR's sixteen stored filename bytes.
START_TEST(test_write_rejects_truncated_wildcard_suffix) {
    const unsigned int format = (unsigned int)_i / 4u, scenario = (unsigned int)_i % 4u;
    static const char* const requests[] = {
        "0:ABCDEFGHIJKLMNOP*,S,W",
        "0:ABCDEFGHIJKLMNOP?,S,W",
        "@0:ABCDEFGHIJKLMNOP*,S,W",
        "@0:ABCDEFGHIJKLMNOP?,S,W",
    };
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    ck_assert_uint_eq(open_name(&unit, &bank, 2, requests[scenario]), DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, scenario < 2 ? DOS_STATUS_INVALID_NAME : DOS_STATUS_TYPE_MISMATCH);
    ck_assert_uint_eq(image.writes, 0);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// Retiring a peer writer must not change the replacement OPEN's drive.
START_TEST(test_reused_writer_secondary_restores_requested_drive) {
    const unsigned int format = (unsigned int)_i / 3u, scenario = (unsigned int)_i % 3u;
    static const char* const requests[] = {"0:NEW,S,W", "$0", "0:FILE"};
    image_t first, second;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &first, format);
    mount_peer(&bank, &second, format);
    fixture_file(&first, format, 0, "FILE", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "1:OLD,S,W"), DOS_UNUSED);
    const uint8_t channel = open_name(&unit, &bank, 2, requests[scenario]);
    ck_assert_uint_ne(channel, DOS_UNUSED);
    ck_assert_uint_eq(unit.channels[channel].media.slot, 0);
    ck_assert_uint_eq(unit.last_drive, 0);
    ck_assert_str_eq(unit.status, format == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r");
    ck_assert_uint_eq(sector_bytes(&second, bank.units[0].slots[1].io.image.type, format == 0 ? 18 : 39, 1)[2], 0x81);
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// APPEND's block count includes its initial read and final write job.
START_TEST(test_append_directory_count_includes_initial_read) {
    static const unsigned int lengths[] = {22, 254, 300};
    const unsigned int format = (unsigned int)_i / count_of(lengths);
    const unsigned int length = lengths[(unsigned int)_i % count_of(lengths)];
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:F,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x20, length);
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:F,A"), DOS_UNUSED);
    dos_put(&unit, &bank, 2, (uint8_t)(0x20 + length));
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    const uint8_t* const entry = sector_bytes(&image, bank.units[0].slots[0].io.image.type, format == 0 ? 18 : 39, 1);
    ck_assert_uint_eq(entry[30] | ((unsigned int)entry[31] << 8u), length == 22 ? 2 : 3);
    const uint8_t reader = open_name(&unit, &bank, 3, "0:F");
    expect_bytes(&unit, &bank, reader, length + 1u, 0x20);
    free(image.bytes);
}
END_TEST

// Register the read/status regressions and ordinary-write contracts.
Suite* dos_suite(void) {
    Suite* const suite = suite_create("dos");
    TCase* const core = tcase_create("ordinary files");
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
    tcase_add_test(core, test_empty_slot_and_read_only_writes_fail_explicitly);
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
    tcase_add_loop_test(core, test_failed_reopen_releases_only_dos27_ordinary_owner, 0, 15);
    tcase_add_loop_test(core, test_failed_reopen_releases_dos27_listing_reservations, 0, 3);
    tcase_add_loop_test(core, test_open_preserves_explicit_pattern_length, 0, 15);
    tcase_add_loop_test(core, test_directory_preserves_explicit_pattern_lengths, 0, 15);
    tcase_add_loop_test(core, test_initial_load_star_releases_active_drive_owners, 0, 15);
    tcase_add_loop_test(core, test_directory_load_initializes_only_4040_shorthand, 0, 12);
    tcase_add_loop_test(core, test_directory_numeric_wildcard_uses_available_drive, 0, 6);
    tcase_add_loop_test(core, test_directory_exact_patterns_complete_independently, 0, 12);
    tcase_add_loop_test(core, test_directory_mixed_prefixes_inherit_previous_drive, 0, 9);
    tcase_add_loop_test(core, test_directory_missing_first_drive_falls_back_only_when_requested, 0, 6);
    tcase_add_loop_test(core, test_4040_directory_initialization_selects_active_page_job, 0, 2);
    tcase_add_loop_test(core, test_directory_both_default_preferences_retain_fallback_pass, 0, 6);
    tcase_add_loop_test(core, test_directory_completion_preserves_raw_track_state, 0, 9);
    tcase_add_loop_test(core, test_directory_suffix_respects_parser_boundaries, 0, 24);
    tcase_add_loop_test(core, test_raw_directory_uses_simple_drive_selection, 0, 36);
    tcase_add_loop_test(core, test_open_zero_track_match_stops_search, 0, 9);
    tcase_add_loop_test(core, test_open_decodes_model_specific_track_bits, 0, 15);
    tcase_add_loop_test(core, test_native_create_close_and_boundaries, 0, 21);
    tcase_add_loop_test(core, test_native_append_replace_and_peer_directory_entries, 0, 3);
    tcase_add_loop_test(core, test_close_storage_failures_retain_and_recover, 0, 12);
    tcase_add_loop_test(core, test_write_errors_and_unclosed_recreation, 0, 3);
    tcase_add_loop_test(core, test_near_full_warning_and_full_channel_release, 0, 3);
    tcase_add_loop_test(core, test_put_write_failure_retries_without_losing_data, 0, 3);
    tcase_add_loop_test(core, test_directory_growth_storage_failure_and_recovery, 0, 9);
    tcase_add_loop_test(core, test_directory_full_releases_failed_open_safely, 0, 3);
    tcase_add_loop_test(core, test_append_lookup_yields_and_failed_read_preserves_media, 0, 3);
    tcase_add_loop_test(core, test_read_only_create_and_append_leave_media_unchanged, 0, 3);
    tcase_add_loop_test(core, test_save_override_last_program_and_status_retention, 0, 3);
    tcase_add_loop_test(core, test_allocation_bam_write_failure_preserves_peer_and_retries, 1, 3);
    tcase_add_loop_test(core, test_bam_count_mismatch_is_not_persisted_as_success, 0, 3);
    tcase_add_loop_test(core, test_dual_drive_writers_and_directory_selection, 0, 3);
    tcase_add_loop_test(core, test_finalize_writers_retains_owners_and_retries, 0, 3);
    tcase_add_loop_test(core, test_finalize_cancels_unfinished_append_without_writes, 0, 3);
    tcase_add_loop_test(core, test_close_all_retains_descending_failure_boundary, 0, 3);
    tcase_add_loop_test(core, test_ordinary_write_version_gate, 0, 9);
    tcase_add_loop_test(core, test_finalize_released_writer_persists_unit_state, 0, 9);
    tcase_add_loop_test(core, test_failed_write_open_retires_command_bytes, 0, 21);
    tcase_add_loop_test(core, test_initial_full_allocation_preserves_native_status, 0, 3);
    tcase_add_loop_test(core, test_put_reselects_writer_drive, 0, 6);
    tcase_add_loop_test(core, test_append_resume_reselects_drive, 0, 3);
    tcase_add_loop_test(core, test_directory_writer_ownership_includes_slot, 0, 6);
    tcase_add_loop_test(core, test_write_rejects_truncated_wildcard_suffix, 0, 12);
    tcase_add_loop_test(core, test_reused_writer_secondary_restores_requested_drive, 0, 9);
    tcase_add_loop_test(core, test_append_directory_count_includes_initial_read, 0, 9);
    suite_add_tcase(suite, core);
    return suite;
}

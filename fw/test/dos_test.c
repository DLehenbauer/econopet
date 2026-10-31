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
    uint32_t fail_read_offset;
    bool fail_flush;
    bool fail_write_without_changes;
} image_t;

#define REL_FIXTURE_BLOCK_LIMIT 730u
#define REL_FIXTURE_RECORD_SIZE 127u

static bool read_image(void* context, uint32_t offset, void* output, size_t length);

// Injects a partial or untouched sector write failure at a persistence boundary.
static bool write_image(void* const context, const uint32_t offset, const void* const input, const size_t length) {
    image_t* const image = context;
    if (offset > image->size || length > image->size - offset) {
        return false;
    }
    if (++image->writes == image->fail_write) {
        if (!image->fail_write_without_changes) {
            memcpy(image->bytes + offset, input, length / 2u);
        }
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
    if (image->reads == image->fail_read || (image->fail_read_offset != 0 && offset == image->fail_read_offset)) {
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
    const uint8_t channel = dos_find_channel(unit, secondary);
    if (channel != DOS_UNUSED && unit->channels[channel].file_type == DISKIMAGE_FTYPE_REL &&
        unit->channels[channel].directory == DOS_DIRECTORY_NONE) {
        dos_input_address(unit, bank, bank->units[0].model, IEEE_CMD_SECONDARY(secondary), true);
        for (unsigned int index = 0; index < count; ++index) {
            dos_listen_put(unit, bank, (uint8_t)(first + index), index + 1u == count);
            finish_open(unit, bank);
        }
        return;
    }
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

// CLSDIR uses the live replacement bit/link and retains one retry owner per entry.
START_TEST(test_replacement_close_uses_current_entry) {
    const unsigned int format = (unsigned int)_i / 3u, fault = (unsigned int)_i % 3u;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "F", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    limit_free_blocks(&image, format, 2);
    const dos_model_t model = bank.units[0].model;
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "@0:F,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x40, 22);
    const uint8_t second = open_name(&unit, &bank, 3, "@0:F,S,W");
    ck_assert_uint_ne(second, DOS_UNUSED);
    const uint8_t replacement_track = unit.channels[second].first_track;
    const uint8_t replacement_sector = unit.channels[second].first_sector;
    put_bytes(&unit, &bank, 3, 0x80, 22);
    if (fault != 0) {
        image.fail_write = image.writes + 2u;
        image.fail_write_without_changes = fault == 1;
        ck_assert(!dos_close(&unit, &bank, model, 2));
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
        image.fail_write = 0;
    } else {
        ck_assert(dos_close(&unit, &bank, model, 2));
        const uint8_t* const entry = sector_bytes(&image, type, format == 0 ? 18 : 39, 1);
        ck_assert_uint_eq(entry[3], replacement_track);
        ck_assert_uint_eq(entry[4], replacement_sector);
    }
    // The original chain is the only free data block and is reassigned live.
    const uint8_t peer = open_name(&unit, &bank, 4, "0:PEER,S,W");
    ck_assert_uint_ne(peer, DOS_UNUSED);
    ck_assert_uint_eq(unit.channels[peer].first_track, 1);
    ck_assert_uint_eq(unit.channels[peer].first_sector, 0);
    put_bytes(&unit, &bank, 4, 0xc0, 22);
    ck_assert(dos_close(&unit, &bank, model, 4));
    if (fault != 0) {
        const unsigned int writes = image.writes;
        ck_assert_uint_eq(open_name(&unit, &bank, 5, "@0:F,S,W"), DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
        ck_assert_uint_eq(image.writes, writes);
        ck_assert(!dos_close(&unit, &bank, model, 3));
        ck_assert_uint_eq(dos_find_channel(&unit, 3), second);
        ck_assert(dos_close(&unit, &bank, model, 2));
    }
    ck_assert(dos_close(&unit, &bank, model, 3));
    ck_assert_uint_eq(fixture_bam_entry(&image, format, 1)[1] & 1u, 0);
    uint8_t reader = open_name(&unit, &bank, 2, "0:PEER");
    expect_bytes(&unit, &bank, reader, 22, 0xc0);
    ck_assert(dos_close(&unit, &bank, model, 2));
    reader = open_name(&unit, &bank, 3, "0:F");
    expect_bytes(&unit, &bank, reader, 22, 0x80);
    free(image.bytes);
}
END_TEST

// CLSDIR rejects a missing live alternate before releasing the original chain.
START_TEST(test_replacement_close_rejects_missing_alternate) {
    const unsigned int format = (unsigned int)_i;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "F", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t writer = open_name(&unit, &bank, 2, "@0:F,S,W");
    ck_assert_uint_ne(writer, DOS_UNUSED);
    uint8_t* const entry = sector_bytes(&image, bank.units[0].slots[0].io.image.type, format == 0 ? 18 : 39, 1);
    entry[28] = 0;
    entry[29] = 25;
    ck_assert(!dos_close(&unit, &bank, model, 2));
    ck_assert_str_eq(unit.status,
                     format == 0 ? "67,ILLEGAL TRACK OR SECTOR,00,25\r" : "67,ILLEGAL TRACK OR SECTOR,00,25,0\r");
    ck_assert_uint_eq(dos_find_channel(&unit, 2), writer);
    ck_assert_uint_eq(fixture_bam_entry(&image, format, 1)[1] & 1u, 0);
    free(image.bytes);
}
END_TEST

// Retrying unpublished OPEN metadata must retain its planned name and new chain.
START_TEST(test_unpublished_writer_retries_closed_metadata) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool replace = (unsigned int)_i % 2u != 0;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    if (replace) {
        fixture_file(&image, format, 0, "F", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    }
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    image.fail_write = 1;
    image.fail_write_without_changes = true;
    const uint8_t writer = open_name(&unit, &bank, 2, replace ? "@0:F,S,W" : "0:F,S,W");
    ck_assert_uint_ne(writer, DOS_UNUSED);
    ck_assert(!unit.channels[writer].directory_published);
    const uint8_t track = unit.channels[writer].first_track;
    const uint8_t sector = unit.channels[writer].first_sector;
    image.fail_write = image.writes + 2u;
    ck_assert(!dos_close(&unit, &bank, model, 2));
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    image.fail_write = 0;
    ck_assert(dos_close(&unit, &bank, model, 2));
    const uint8_t* const entry = sector_bytes(&image, bank.units[0].slots[0].io.image.type, format == 0 ? 18 : 39, 1);
    ck_assert_uint_eq(entry[2], 0x81);
    ck_assert_uint_eq(entry[3], track);
    ck_assert_uint_eq(entry[4], sector);
    ck_assert_uint_eq(entry[5], 'F');
    const uint8_t reader = open_name(&unit, &bank, 3, "0:F");
    expect_bytes(&unit, &bank, reader, 0, 0);
    free(image.bytes);
}
END_TEST

// An untouched predecessor write still owns both ends of pending NXDRBK.
START_TEST(test_untouched_growth_failure_reserves_both_links) {
    const unsigned int format = (unsigned int)_i;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    fill_directory(&image, format);
    const dos_model_t model = bank.units[0].model;
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    const uint8_t track = format == 0 ? 18 : 39;
    dos_unit_t unit;
    dos_reset(&unit, model);
    image.fail_write = 1;
    image.fail_write_without_changes = true;
    const uint8_t owner = open_name(&unit, &bank, 2, "0:FIRST,S,W");
    ck_assert_uint_ne(owner, DOS_UNUSED);
    ck_assert(unit.channels[owner].grow_pending);
    ck_assert_uint_eq(sector_bytes(&image, type, track, 1)[0], 0);
    const unsigned int writes = image.writes;
    ck_assert_uint_eq(open_name(&unit, &bank, 3, "0:SECOND,S,W"), DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert(dos_close(&unit, &bank, model, 2));
    ck_assert_uint_eq(sector_bytes(&image, type, track, 1)[0], track);
    ck_assert_uint_ne(open_name(&unit, &bank, 3, "0:SECOND,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 3, 0x60, 22);
    ck_assert(dos_close(&unit, &bank, model, 3));
    const uint8_t reader = open_name(&unit, &bank, 2, "0:SECOND");
    expect_bytes(&unit, &bank, reader, 22, 0x60);
    const uint8_t tail_sector = sector_bytes(&image, type, track, 1)[1];
    const uint8_t* const tail = sector_bytes(&image, type, track, tail_sector);
    ck_assert_mem_eq(tail + 5, "FIRST", 5);
    ck_assert_mem_eq(tail + DISKIMAGE_DIRECTORY_ENTRY_SIZE + 5, "SECOND", 6);
    free(image.bytes);
}
END_TEST

// Host writer retirement invalidates lookup links and deleted-slot reservations.
START_TEST(test_reused_secondary_revalidates_directory) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool recreate = (unsigned int)_i % 2u != 0;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, recreate ? 1 : 0, "F", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    limit_free_blocks(&image, format, 1);
    const dos_model_t model = bank.units[0].model;
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, recreate ? "0:F,A" : "@0:F,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, recreate ? 0x20 + 22 : 0x40, 22);
    const uint8_t writer = open_name(&unit, &bank, 2, recreate ? "0:F,S,W" : "@0:F,S,W");
    if (recreate) {
        ck_assert_uint_eq(writer, DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_FILE_EXISTS);
        ck_assert_uint_eq(sector_bytes(&image, type, format == 0 ? 18 : 39, 1)[2], 0);
    } else {
        ck_assert_uint_ne(writer, DOS_UNUSED);
        ck_assert_uint_eq(unit.channels[writer].first_track, 1);
        ck_assert_uint_eq(unit.channels[writer].first_sector, 0);
        put_bytes(&unit, &bank, 2, 0x80, 22);
        ck_assert(dos_close(&unit, &bank, model, 2));
        ck_assert_uint_eq(fixture_bam_entry(&image, format, 1)[1] & 1u, 0);
    }
    const uint8_t reader = open_name(&unit, &bank, 3, "0:F");
    expect_bytes(&unit, &bank, reader, recreate ? 44 : 22, recreate ? 0x20 : 0x80);
    free(image.bytes);
}
END_TEST

// Lookup I/O failure cannot discard the old secondary's buffered or retry data.
START_TEST(test_lookup_failure_preserves_writer) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool closing = (unsigned int)_i % 2u != 0;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t writer = open_name(&unit, &bank, 2, "0:OLD,S,W");
    ck_assert_uint_ne(writer, DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x40, 22);
    if (closing) {
        image.fail_write = image.writes + 1u;
        ck_assert(!dos_close(&unit, &bank, model, 2));
        image.fail_write = 0;
    }
    const dos_channel_t saved = unit.channels[writer];
    const uint16_t buffers = unit.buffer_use;
    image.fail_read = image.reads + 1u;
    ck_assert_uint_eq(open_name(&unit, &bank, 2, "0:NEW"), writer);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_mem_eq(&unit.channels[writer], &saved, sizeof(saved));
    ck_assert_uint_eq(unit.buffer_use, buffers);
    const uint8_t retired[DOS_COMMAND_LIMIT] = {0};
    ck_assert_mem_eq(unit.command_bytes, retired, sizeof(retired));
    image.fail_read = 0;
    ck_assert(dos_close(&unit, &bank, model, 2));
    const uint8_t reader = open_name(&unit, &bank, 3, "0:OLD");
    expect_bytes(&unit, &bank, reader, 22, 0x40);
    free(image.bytes);
}
END_TEST

// Lazy write-version BAM loads must not retag an earlier image's NUMFRE total.
START_TEST(test_append_rebind_invalidates_free_total) {
    const unsigned int format = (unsigned int)_i;
    image_t first, second;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &first, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    uint8_t before[96], after[96];
    uint8_t channel = open_name(&unit, &bank, 0, "$0");
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, before, sizeof(before)), 64);
    ck_assert(dos_close(&unit, &bank, model, 0));
    mount_blank(&bank, &second, format);
    fixture_file(&second, format, 0, "F", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:F,A"), DOS_UNUSED);
    dos_put(&unit, &bank, 2, 0x56);
    ck_assert(dos_close(&unit, &bank, model, 2));
    channel = open_name(&unit, &bank, 0, "$0");
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, after, sizeof(after)), 96);
    const unsigned int old_free = before[34] | ((unsigned int)before[35] << 8u);
    const unsigned int new_free = after[66] | ((unsigned int)after[67] << 8u);
    ck_assert_uint_eq(new_free + 1u, old_free);
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// Failed unpublished writers reserve their actual filename, not only a slot.
START_TEST(test_unpublished_writer_reserves_filename) {
    const unsigned int format = (unsigned int)_i / 4u;
    const bool replace = (unsigned int)_i % 4u >= 2u, reverse = (unsigned int)_i % 2u != 0;
    image_t image, peer_image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    if (replace) {
        fixture_file(&image, format, 0, "F", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    } else if (!reverse) {
        mount_peer(&bank, &peer_image, format);
    }
    const dos_model_t model = bank.units[0].model;
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    dos_unit_t unit;
    dos_reset(&unit, model);
    image.fail_write = 1;
    image.fail_write_without_changes = true;
    const uint8_t owner = open_name(&unit, &bank, 2, replace ? "@0:F*,S,W" : "0:F,S,W");
    ck_assert_uint_ne(owner, DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert(!unit.channels[owner].directory_published);
    const dos_channel_t saved = unit.channels[owner];
    const unsigned int writes = image.writes;
    if (replace) {
        const uint8_t reader = open_name(&unit, &bank, 5, "0:F");
        expect_bytes(&unit, &bank, reader, 22, 0x20);
        ck_assert(dos_close(&unit, &bank, model, 5));
        ck_assert_uint_eq(open_name(&unit, &bank, 3, "0:F,A"), DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    }
    ck_assert_uint_eq(open_name(&unit, &bank, 3, replace ? "@0:F,S,W" : "0:F,S,W"), DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_mem_eq(&unit.channels[owner], &saved, sizeof(saved));
    if (!replace && !reverse) {
        ck_assert_uint_ne(open_name(&unit, &bank, 4, "1:F,S,W"), DOS_UNUSED);
        put_bytes(&unit, &bank, 4, 0x60, 22);
        ck_assert(dos_close(&unit, &bank, model, 4));
    }
    ck_assert(dos_close(&unit, &bank, model, reverse ? 2 : 3));
    ck_assert(dos_close(&unit, &bank, model, reverse ? 3 : 2));
    ck_assert_uint_ne(open_name(&unit, &bank, 3, "@0:F,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 3, 0x80, 22);
    ck_assert(dos_close(&unit, &bank, model, 3));
    const uint8_t reader = open_name(&unit, &bank, 2, "0:F");
    expect_bytes(&unit, &bank, reader, 22, 0x80);
    const uint8_t* const directory = sector_bytes(&image, type, format == 0 ? 18 : 39, 1);
    unsigned int matches = 0;
    for (unsigned int offset = 0; offset < DISKIMAGE_SECTOR_SIZE; offset += DISKIMAGE_DIRECTORY_ENTRY_SIZE) {
        matches += directory[offset + 2] != 0 && directory[offset + 5] == 'F';
    }
    ck_assert_uint_eq(matches, 1);
    free(image.bytes);
    if (!replace && !reverse) {
        free(peer_image.bytes);
    }
}
END_TEST

// A secondary's own unpublished name must not prevent its safe retirement.
START_TEST(test_unpublished_writer_retires_before_same_secondary_open) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, (unsigned int)_i);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    image.fail_write = 1;
    image.fail_write_without_changes = true;
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:F,S,W"), DOS_UNUSED);
    ck_assert_uint_eq(open_name(&unit, &bank, 2, "0:F,S,W"), DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_FILE_EXISTS);
    const uint8_t reader = open_name(&unit, &bank, 3, "0:F");
    expect_bytes(&unit, &bank, reader, 0, 0);
    free(image.bytes);
}
END_TEST

// PARSE retains prefix/final-prescan patterns as well as an equals suffix.
START_TEST(test_write_pattern_flags_across_parse_passes) {
    const unsigned int format = (unsigned int)_i / 10u, scenario = (unsigned int)_i % 10u;
    static const char* const requests[] = {
        "0:F=*,S,W", "0:F=?,S,W", "@0:F=*,S,W", "@0:F=?,S,W", "*:F,S,W",
        "?:F,S,W",   "F,S,W*",    "F,S?,W",     "0:F,S,W*",   "0:F=TAIL,S,W",
    };
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t writer = open_name(&unit, &bank, 2, requests[scenario]);
    if (scenario < 7) {
        ck_assert_uint_eq(writer, DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code,
                          scenario == 2 || scenario == 3 ? DOS_STATUS_TYPE_MISMATCH : DOS_STATUS_INVALID_NAME);
        ck_assert_uint_eq(image.writes, 0);
        ck_assert_uint_eq(unit.buffer_use, 0);
    } else {
        ck_assert_uint_ne(writer, DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    }
    free(image.bytes);
}
END_TEST

// Submit a complete metadata request through the direct command API.
static void send_command(dos_unit_t* const unit, dos_media_bank_t* const bank, const char* const text) {
    dos_command(unit, bank, 0, (const uint8_t*)text, (unsigned int)strlen(text));
}

// Exercise either command-channel collection entry before ordered dispatch.
static void send_input_command(dos_unit_t* const unit, dos_media_bank_t* const bank, const char* const text,
                               const bool open) {
    const unsigned int length = strlen(text);
    dos_input_address(unit, bank, bank->units[0].model,
                      open ? IEEE_CMD_OPEN(IEEE_COMMAND_CHANNEL) : IEEE_CMD_SECONDARY(IEEE_COMMAND_CHANNEL), true);
    for (unsigned int offset = 0; offset < length; ++offset) {
        dos_input_put(unit, (uint8_t)text[offset], offset + 1u == length);
    }
    dos_input_dispatch(unit, bank, 0);
}

// Create closed source/collision entries without borrowing rename implementation.
static void rename_fixture(dos_unit_t* const unit, dos_media_bank_t* const bank) {
    ck_assert_uint_ne(open_name(unit, bank, 2, "0:SOURCE,S,W"), DOS_UNUSED);
    put_bytes(unit, bank, 2, 0x20, 4);
    ck_assert(dos_close(unit, bank, bank->units[0].model, 2));
    ck_assert_uint_ne(open_name(unit, bank, 2, "0:EXISTS,S,W"), DOS_UNUSED);
    put_bytes(unit, bank, 2, 0x60, 2);
    ck_assert(dos_close(unit, bank, bank->units[0].model, 2));
}

// CMDSET trims CR before its logical limit and retires only native CLRCB bytes.
START_TEST(test_metadata_command_boundaries_and_retirement) {
    const unsigned int format = (unsigned int)_i / 7u, variant = (unsigned int)_i % 7u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    uint8_t request[DOS_COMMAND_RECEIVE_CAPACITY + 1u];
    memset(request, 'X', sizeof(request));
    request[0] = 'V';
    request[1] = '0';
    unsigned int length = sizeof(request);
    if (variant != 0) {
        length = variant == 4 ? DOS_COMMAND_LIMIT : DOS_COMMAND_RECEIVE_CAPACITY;
    }
    if (variant == 2 || variant == 3) {
        request[length - (variant == 2 ? 1u : 2u)] = '\r';
    }
    if (variant >= 5) {
        request[0] = variant == 5 ? 'M' : 'Z';
        length = 1;
    }
    const unsigned int reads = image.reads, writes = image.writes;
    dos_command(&unit, &bank, 0, request, length);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    const uint8_t cleared[DOS_COMMAND_LIMIT] = {0};
    ck_assert_mem_eq(unit.command_bytes, cleared, sizeof(cleared));
    if (length == DOS_COMMAND_RECEIVE_CAPACITY) {
        ck_assert_uint_eq(unit.command_bytes[DOS_COMMAND_LIMIT], request[DOS_COMMAND_LIMIT]);
    }
    ck_assert_int_eq(dos_work_pending(&unit), variant >= 2 && variant <= 4);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, variant < 2    ? DOS_STATUS_COMMAND_TOO_LONG
                                        : variant == 5 ? DOS_STATUS_SYNTAX
                                        : variant == 6 ? DOS_STATUS_INVALID_COMMAND
                                                       : DOS_STATUS_OK);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// Metadata matching checks explicit bytes before testing the directory's padding.
START_TEST(test_metadata_names_retain_explicit_padding_and_native_limits) {
    static const char* const names[] = {"SOURCE", "SOURCE\xa0Y", "SOURCE\xa0X"};
    const unsigned int format = (unsigned int)_i / 6u;
    const unsigned int operation = (unsigned int)_i / 3u % 2u, variant = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x20, 4);
    uint8_t* const entry = sector_bytes(&image, bank.units[0].slots[0].io.image.type, format == 0 ? 18 : 39, 1);
    entry[5 + 7] = 'X';
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    char request[DOS_COMMAND_RECEIVE_CAPACITY];
    snprintf(request, sizeof(request), "%s%s", operation == 0 ? "R0:NEW=" : "S0:", names[variant]);
    send_command(&unit, &bank, request);
    finish_open(&unit, &bank);
    const bool matched = variant != 1;
    ck_assert_uint_eq(unit.status_code, operation != 0 ? DOS_STATUS_FILES_SCRATCHED
                                        : matched      ? DOS_STATUS_OK
                                                       : DOS_STATUS_NOT_FOUND);
    if (operation == 0 && matched) {
        ck_assert_mem_eq(entry + 5, "NEW", 3);
    } else {
        ck_assert_uint_eq(entry[2] == 0, operation != 0 && matched);
    }
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// RENAME changes exactly the source name while retaining a paused reader.
START_TEST(test_rename_changes_only_directory_name_and_preserves_reader) {
    image_t image;
    dos_media_bank_t bank = {0};
    const unsigned int format = (unsigned int)_i;
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    rename_fixture(&unit, &bank);
    const uint8_t reader = open_name(&unit, &bank, 2, "0:SOURCE,S,R");
    ck_assert_uint_ne(reader, DOS_UNUSED);
    dos_get(&unit, &bank, reader, 1);
    const dos_channel_t before = unit.channels[reader];
    const unsigned int reads = image.reads, writes = image.writes;
    uint8_t* const expected = malloc(image.size);
    ck_assert_ptr_nonnull(expected);
    memcpy(expected, image.bytes, image.size);
    const unsigned int directory = format == 0 ? 18u : 39u;
    uint8_t* const name = sector_bytes(&image, bank.units[0].slots[0].io.image.type, directory, 1) + 5;
    memset(expected + (name - image.bytes), 0xa0, 16);
    memcpy(expected + (name - image.bytes), "NEW", 3);
    send_command(&unit, &bank, "R0:NEW=SOURCE");
    ck_assert(dos_work_pending(&unit));
    ck_assert_uint_eq(image.reads, reads);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(image.writes, writes + 1);
    ck_assert_mem_eq(image.bytes, expected, image.size);
    ck_assert_mem_eq(&unit.channels[reader], &before, sizeof(before));
    expect_bytes(&unit, &bank, reader, 3, 0x21);
    ck_assert(dos_close(&unit, &bank, model, 2));
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:NEW,S,R"), DOS_UNUSED);
    expect_bytes(&unit, &bank, dos_find_channel(&unit, 2), 4, 0x20);
    free(expected);
    free(image.bytes);
}
END_TEST

// TAGCMD distinguishes missing colon, malformed structure and missing source.
START_TEST(test_rename_native_parser_and_error_precedence) {
    static const struct {
        const char* command;
        dos_status_code_t status;
    } cases[] = {
        {"R0:NEW=SOURCE", DOS_STATUS_OK},
        {"RENAME0:NEW=SOURCE", DOS_STATUS_OK},
        {"R0:EXISTS=ABSENT", DOS_STATUS_NOT_FOUND},
        {"R0:EXISTS=SOURCE", DOS_STATUS_FILE_EXISTS},
        {"R0:SOURCE=SOURCE", DOS_STATUS_FILE_EXISTS},
        {"R0:X*=SOURCE", DOS_STATUS_SYNTAX},
        {"R0:X=SO?RCE", DOS_STATUS_SYNTAX},
        {"R0:X=SOURCE,EXISTS", DOS_STATUS_SYNTAX},
        {"R,0:NEW=SOURCE", DOS_STATUS_NO_FILE},
        {"RNEW=SOURCE", DOS_STATUS_NO_FILE},
        {"R0:NEW", DOS_STATUS_SYNTAX},
        {"R0:NEW=", DOS_STATUS_NOT_FOUND},
        {"R0:=SOURCE", DOS_STATUS_OK},
        {"R2:NEW=SOURCE", DOS_STATUS_OK},
        {"R:NEW=SOURCE", DOS_STATUS_OK},
        {"R0:NEW=SOURCE=IGNORED", DOS_STATUS_OK},
        {"R0:12345678901234567=SOURCE", DOS_STATUS_OK},
        {"R0:NEW=X:SOURCE", DOS_STATUS_OK},
        {"R0:NEW=:SOURCE", DOS_STATUS_OK},
        {"R0:NEW=SOURCE,", DOS_STATUS_SYNTAX},
        {"R0:NEW=::SOURCE", DOS_STATUS_OK},
        {"R0:NEW=:::SOURCE", DOS_STATUS_NOT_FOUND},
    };
    const unsigned int format = (unsigned int)_i / count_of(cases), which = (unsigned int)_i % count_of(cases);
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    rename_fixture(&unit, &bank);
    const unsigned int writes = image.writes;
    send_command(&unit, &bank, cases[which].command);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, cases[which].status);
    ck_assert_uint_eq(image.writes, writes + (cases[which].status == DOS_STATUS_OK));
    ck_assert_uint_eq(unit.buffer_use, 0);
    if (which == 12) {
        const uint8_t* const entry =
            sector_bytes(&image, bank.units[0].slots[0].io.image.type, format == 0 ? 18 : 39, 1);
        ck_assert_uint_eq(entry[5], '=');
        for (unsigned int offset = 6; offset <= 21; ++offset) {
            ck_assert_uint_eq(entry[offset], 0xa0);
        }
    }
    free(image.bytes);
}
END_TEST

// A differing destination drive checks both disks but never moves the source.
START_TEST(test_rename_cross_drive_collision_and_source_selection) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool collision = (unsigned int)_i % 2u;
    image_t first, second;
    dos_media_bank_t bank = {0}, other = {0};
    mount_blank(&bank, &first, format);
    mount_blank(&other, &second, format);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "1:SOURCE,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x20, 4);
    ck_assert(dos_close(&unit, &bank, model, 2));
    if (collision) {
        ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:NEW,S,W"), DOS_UNUSED);
        put_bytes(&unit, &bank, 2, 0x60, 2);
        ck_assert(dos_close(&unit, &bank, model, 2));
    }
    const unsigned int first_writes = first.writes, second_writes = second.writes;
    send_command(&unit, &bank, "R0:NEW=1:SOURCE");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, collision ? DOS_STATUS_FILE_EXISTS : DOS_STATUS_OK);
    ck_assert_uint_eq(unit.selected_drive, collision ? 0 : 1);
    ck_assert_uint_eq(first.writes, first_writes);
    ck_assert_uint_eq(second.writes, second_writes + !collision);
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// Read-only, partial read/write and failed flush must not erase unrelated cursors.
START_TEST(test_rename_storage_failures_release_only_internal_ownership) {
    const unsigned int format = (unsigned int)_i / 5u, failure = (unsigned int)_i % 5u;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    rename_fixture(&unit, &bank);
    if (failure == 4) {
        dos_media_io_t readonly = bank.units[0].slots[0].io;
        readonly.image.write = NULL;
        readonly.flush = NULL;
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, &readonly), DOS_MEDIA_OK);
    }
    const uint8_t reader = open_name(&unit, &bank, 2, "0:SOURCE,S,R");
    ck_assert_uint_ne(reader, DOS_UNUSED);
    dos_get(&unit, &bank, reader, 1);
    const dos_channel_t before = unit.channels[reader];
    const uint16_t buffers = unit.buffer_use;
    image.fail_read = failure < 2 ? image.reads + failure + 1u : 0;
    image.fail_write = failure == 2 ? image.writes + 1u : 0;
    image.fail_flush = failure == 3;
    send_command(&unit, &bank, "R0:NEW=SOURCE");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, failure < 2    ? DOS_STATUS_READ_ERROR
                                        : failure == 4 ? DOS_STATUS_WRITE_PROTECT
                                                       : DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(unit.buffer_use, buffers);
    ck_assert_mem_eq(&unit.channels[reader], &before, sizeof(before));
    expect_bytes(&unit, &bank, reader, 3, 0x21);
    if (failure == 3) {
        ck_assert(bank.units[0].slots[0].dirty);
        image.fail_flush = false;
        ck_assert_int_eq(dos_media_flush_unit(&bank, 0), DOS_MEDIA_OK);
    }
    free(image.bytes);
}
END_TEST

// Linked-directory lookup yields, and commit rereads rather than overwriting peer edits.
START_TEST(test_rename_yields_and_preserves_later_directory_edits) {
    image_t image;
    dos_media_bank_t bank = {0};
    const unsigned int format = (unsigned int)_i;
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    rename_fixture(&unit, &bank);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    const unsigned int track = format == 0 ? 18 : 39;
    uint8_t* const first = sector_bytes(&image, type, track, 1);
    uint8_t* const second = sector_bytes(&image, type, track, 2);
    memcpy(second, first, DISKIMAGE_SECTOR_SIZE);
    memset(first, 0, DISKIMAGE_SECTOR_SIZE);
    first[0] = track;
    first[1] = 2;
    const unsigned int reads = image.reads;
    send_command(&unit, &bank, "R0:NEW=SOURCE");
    dos_work(&unit, &bank);
    ck_assert(dos_work_pending(&unit));
    ck_assert(!unit.rename.commit);
    ck_assert_uint_eq(image.reads, reads + 1);
    dos_work(&unit, &bank);
    ck_assert(unit.rename.commit);
    second[DISKIMAGE_DIRECTORY_ENTRY_SIZE + 25u] = 0x55;
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(second[DISKIMAGE_DIRECTORY_ENTRY_SIZE + 25u], 0x55);
    ck_assert_mem_eq(second + 5, "NEW", 3);
    free(image.bytes);
}
END_TEST

// A queued lookup or commit cannot follow a replacement media binding.
START_TEST(test_rename_rejects_stale_media_at_lookup_and_commit) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool commit = (unsigned int)_i % 2u;
    image_t old, replacement;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &old, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    rename_fixture(&unit, &bank);
    send_command(&unit, &bank, "R0:NEW=SOURCE");
    if (commit) {
        dos_work(&unit, &bank);
        ck_assert(unit.rename.commit);
    }
    const unsigned int reads = old.reads, writes = old.writes;
    mount_blank(&bank, &replacement, format);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_uint_eq(old.reads, reads);
    ck_assert_uint_eq(old.writes, writes);
    ck_assert_uint_eq(replacement.reads + replacement.writes, 0);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(old.bytes);
    free(replacement.bytes);
}
END_TEST

// Native RENAME leaves an unfinished writer open, and CLOSE must retain its new name.
START_TEST(test_rename_unclosed_writer_survives_close_without_restoring_old_name) {
    image_t image;
    dos_media_bank_t bank = {0};
    const unsigned int format = (unsigned int)_i;
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t writer = open_name(&unit, &bank, 2, "0:SOURCE,S,W");
    ck_assert_uint_ne(writer, DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x20, 4);
    send_command(&unit, &bank, "R0:NEW=SOURCE");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), writer);
    put_bytes(&unit, &bank, 2, 0x24, 2);
    ck_assert(dos_close(&unit, &bank, model, 2));
    const uint8_t reader = open_name(&unit, &bank, 2, "0:NEW,S,R");
    ck_assert_uint_ne(reader, DOS_UNUSED);
    expect_bytes(&unit, &bank, reader, 6, 0x20);
    free(image.bytes);
}
END_TEST

// Omitted drives use LSTDRV, but an explicitly absent source cannot redirect.
START_TEST(test_rename_default_drive_and_absent_source) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool absent = (unsigned int)_i % 2u;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_media_io_t pair[2] = {{0}, bank.units[0].slots[0].io};
    ck_assert_int_eq(dos_media_set_unit(&bank, 0, pair), DOS_MEDIA_OK);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "1:SOURCE,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x20, 4);
    ck_assert(dos_close(&unit, &bank, model, 2));
    ck_assert_uint_eq(unit.last_drive, 1);
    unit.selected_drive = 0;
    dos_status(&unit, model, DOS_STATUS_NOT_FOUND, 0, 0);
    const unsigned int reads = image.reads;
    send_command(&unit, &bank, absent ? "R0:NEW=SOURCE" : "R:NEW=SOURCE");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, absent ? DOS_STATUS_NOT_READY : DOS_STATUS_OK);
    ck_assert_uint_eq(unit.last_drive, 1);
    ck_assert_uint_eq(unit.selected_drive, absent ? 0 : 1);
    if (absent) {
        ck_assert_uint_eq(image.reads, reads);
    }
    free(image.bytes);
}
END_TEST

// Internal lookup uses the same last two buffers and returns partial claims on failure.
START_TEST(test_rename_shared_resource_exhaustion_and_recovery) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, (unsigned int)_i);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    rename_fixture(&unit, &bank);
    for (uint8_t secondary = 2; secondary < 7; ++secondary) {
        ck_assert_uint_ne(open_name(&unit, &bank, secondary, "0:SOURCE,S,R"), DOS_UNUSED);
    }
    const uint16_t buffers = unit.buffer_use;
    uint8_t extra = 0;
    while (extra < DOS_BUFFER_COUNT && (buffers & (1u << extra))) {
        ++extra;
    }
    ck_assert_uint_lt(extra, DOS_BUFFER_COUNT);
    unit.buffer_use |= 1u << extra;
    const uint16_t exhausted = unit.buffer_use;
    send_command(&unit, &bank, "R0:NEW=SOURCE");
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
    ck_assert_uint_eq(unit.buffer_use, exhausted);
    ck_assert(!dos_work_pending(&unit));
    unit.buffer_use = buffers;
    send_command(&unit, &bank, "R0:NEW=SOURCE");
    ck_assert(dos_work_pending(&unit));
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.buffer_use, buffers);
    for (uint8_t secondary = 2; secondary < 7; ++secondary) {
        ck_assert_uint_ne(dos_find_channel(&unit, secondary), DOS_UNUSED);
    }
    free(image.bytes);
}
END_TEST

// SCRATCH changes only the type byte and allocation bits, preserving retained readers.
START_TEST(test_scratch_releases_exact_chain_and_preserves_peer_reader) {
    image_t image;
    dos_media_bank_t bank = {0};
    const unsigned int format = (unsigned int)_i;
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:SOURCE,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x20, 300);
    ck_assert(dos_close(&unit, &bank, model, 2));
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:EXISTS,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x60, 4);
    ck_assert(dos_close(&unit, &bank, model, 2));
    const uint8_t reader = open_name(&unit, &bank, 2, "0:EXISTS,S,R");
    dos_get(&unit, &bank, reader, 1);
    const dos_channel_t before = unit.channels[reader];
    const uint16_t buffers = unit.buffer_use;
    image_t expected = image;
    expected.bytes = malloc(image.size);
    ck_assert_ptr_nonnull(expected.bytes);
    memcpy(expected.bytes, image.bytes, image.size);
    uint8_t* const entry = sector_bytes(&expected, type, format == 0 ? 18 : 39, 1);
    entry[2] = 0;
    uint8_t track = entry[3], sector = entry[4];
    unsigned int blocks = 0;
    while (track != 0) {
        uint8_t* const bam = fixture_bam_entry(&expected, format, track);
        ck_assert_uint_eq(bam[1u + sector / 8u] & (1u << (sector & 7u)), 0);
        ++bam[0];
        bam[1u + sector / 8u] |= 1u << (sector & 7u);
        const uint8_t* const data = sector_bytes(&expected, type, track, sector);
        track = data[0];
        sector = data[1];
        ck_assert_uint_lt(blocks++, 3);
    }
    ck_assert_uint_eq(blocks, 2);
    const unsigned int reads = image.reads;
    send_command(&unit, &bank, "S0:SOURCE");
    ck_assert(dos_work_pending(&unit));
    ck_assert_uint_eq(image.reads, reads);
    finish_open(&unit, &bank);
    ck_assert_str_eq(unit.status, format == 0 ? "01, FILES SCRATCHED,01,00\r" : "01, FILES SCRATCHED,01,00,0\r");
    ck_assert_mem_eq(image.bytes, expected.bytes, image.size);
    ck_assert_mem_eq(&unit.channels[reader], &before, sizeof(before));
    ck_assert_uint_eq(unit.buffer_use, buffers);
    expect_bytes(&unit, &bank, reader, 3, 0x61);
    free(expected.bytes);
    free(image.bytes);
}
END_TEST

// FS1SET uses equals-type syntax, while comma names are native grouped patterns.
START_TEST(test_scratch_native_parser_patterns_and_counts) {
    static const struct {
        const char* command;
        dos_status_code_t status;
        unsigned int count;
    } cases[] = {
        {"S0:SOURCE", DOS_STATUS_FILES_SCRATCHED, 1},
        {"SCRATCH0:SOURCE", DOS_STATUS_FILES_SCRATCHED, 1},
        {"S0:SOUR?E", DOS_STATUS_FILES_SCRATCHED, 1},
        {"S0:SO*", DOS_STATUS_FILES_SCRATCHED, 1},
        {"S0:SOURCE,0:EXISTS", DOS_STATUS_FILES_SCRATCHED, 2},
        {"S0:SOURCE,EXISTS", DOS_STATUS_FILES_SCRATCHED, 2},
        {"S0:SOURCE=S", DOS_STATUS_FILES_SCRATCHED, 1},
        {"S0:SOURCE=P", DOS_STATUS_FILES_SCRATCHED, 0},
        {"S0:SOURCE=X", DOS_STATUS_FILES_SCRATCHED, 1},
        {"S0:SOURCE=SEQ", DOS_STATUS_FILES_SCRATCHED, 1},
        {"S0:SOURCE=", DOS_STATUS_FILES_SCRATCHED, 1},
        {"S0:SOURCE=*", DOS_STATUS_SYNTAX, 0},
        {"S0:SOURCE=P,S", DOS_STATUS_SYNTAX, 0},
        {"S0:ABSENT", DOS_STATUS_FILES_SCRATCHED, 0},
        {"S0:", DOS_STATUS_FILES_SCRATCHED, 0},
        {"S0:SOURCE,", DOS_STATUS_FILES_SCRATCHED, 1},
        {"SSOURCE", DOS_STATUS_NO_FILE, 0},
        {"S,0:SOURCE", DOS_STATUS_NO_FILE, 0},
        {"S2:SOURCE", DOS_STATUS_FILES_SCRATCHED, 1},
        {"S:SOURCE", DOS_STATUS_FILES_SCRATCHED, 1},
        {"S0:ABSENT,::SOURCE", DOS_STATUS_FILES_SCRATCHED, 1},
        {"S0:ABSENT,:::SOURCE", DOS_STATUS_FILES_SCRATCHED, 0},
    };
    const unsigned int format = (unsigned int)_i / count_of(cases), which = (unsigned int)_i % count_of(cases);
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    rename_fixture(&unit, &bank);
    const unsigned int writes = image.writes;
    send_command(&unit, &bank, cases[which].command);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, cases[which].status);
    if (cases[which].status == DOS_STATUS_FILES_SCRATCHED) {
        char expected[DOS_STATUS_LINE_CAPACITY];
        snprintf(expected, sizeof(expected),
                 format == 0 ? "01, FILES SCRATCHED,%02u,00\r" : "01, FILES SCRATCHED,%02u,00,0\r", cases[which].count);
        ck_assert_str_eq(unit.status, expected);
    }
    if (cases[which].count == 0) {
        ck_assert_uint_eq(image.writes, writes);
    }
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// TSTCHN and the lock bit skip files, while abandoned unclosed entries leak their old chains natively.
START_TEST(test_scratch_active_locked_unclosed_and_rel_entries) {
    const unsigned int format = (unsigned int)_i / 6u, mode = (unsigned int)_i % 6u;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    rename_fixture(&unit, &bank);
    uint8_t* const entry = sector_bytes(&image, bank.units[0].slots[0].io.image.type, format == 0 ? 18 : 39, 1);
    uint8_t channel = DOS_UNUSED;
    if (mode < 2) {
        channel = open_name(&unit, &bank, 2, mode == 0 ? "0:SOURCE,S,R" : "0:SOURCE,S,A");
        ck_assert_uint_ne(channel, DOS_UNUSED);
    } else if (mode == 2) {
        entry[2] |= 0x40;
    } else if (mode == 3) {
        entry[2] &= 0x7f;
    } else if (mode == 4) {
        entry[2] = 0x84;
    } else if (mode == 5) {
        entry[3] = 0;
    }
    const dos_unit_t before = unit;
    uint8_t* const expected = malloc(image.size);
    ck_assert_ptr_nonnull(expected);
    memcpy(expected, image.bytes, image.size);
    if (mode == 3 || mode == 4 || mode == 5) {
        expected[entry - image.bytes + 2u] = 0;
    }
    if (mode == 4) {
        image_t oracle = image;
        oracle.bytes = expected;
        uint8_t* const bam = fixture_bam_entry(&oracle, format, entry[3]);
        ++bam[0];
        bam[1u + entry[4] / 8u] |= (uint8_t)(1u << (entry[4] & 7u));
    }
    send_command(&unit, &bank, "S0:SOURCE");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, mode != 5 ? DOS_STATUS_FILES_SCRATCHED : DOS_STATUS_ILLEGAL_TRACK_SECTOR);
    ck_assert_mem_eq(image.bytes, expected, image.size);
    ck_assert_uint_eq(unit.buffer_use, before.buffer_use);
    if (channel != DOS_UNUSED) {
        ck_assert_mem_eq(&unit.channels[channel], &before.channels[channel], sizeof(dos_channel_t));
    }
    free(expected);
    free(image.bytes);
}
END_TEST

// Explicit drives restrict matches, while omitted group prefixes search both native disks.
START_TEST(test_scratch_cross_drive_patterns_and_default_selection) {
    const unsigned int format = (unsigned int)_i / 3u, mode = (unsigned int)_i % 3u;
    image_t first, second;
    dos_media_bank_t bank = {0}, other = {0};
    mount_blank(&bank, &first, format);
    mount_blank(&other, &second, format);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    for (unsigned int slot = 0; slot < 2; ++slot) {
        for (const char* name = "AB"; *name != 0; ++name) {
            if (mode == 2 && slot == 1 && *name == 'B') {
                continue;
            }
            char request[16];
            snprintf(request, sizeof(request), "%u:%c,S,W", slot, *name);
            ck_assert_uint_ne(open_name(&unit, &bank, 2, request), DOS_UNUSED);
            put_bytes(&unit, &bank, 2, 0x30, 4);
            ck_assert(dos_close(&unit, &bank, model, 2));
        }
    }
    send_command(&unit, &bank, mode == 0 ? "S1:A" : mode == 1 ? "S0:A,1:B" : "S1:A,B");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_FILES_SCRATCHED);
    const unsigned int directory = format == 0 ? 18 : 39;
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    const uint8_t* const zero = sector_bytes(&first, type, directory, 1);
    const uint8_t* const one = sector_bytes(&second, type, directory, 1);
    ck_assert_uint_eq(zero[2] == 0, mode == 1);
    ck_assert_uint_eq(zero[34] == 0, mode == 2);
    ck_assert_uint_eq(one[2] == 0, mode != 1);
    ck_assert_uint_eq(one[34] == 0, mode != 0);
    ck_assert_uint_eq(unit.selected_drive, mode == 1 ? 1 : mode == 2 ? 0 : 1);
    ck_assert_uint_eq(unit.last_drive, unit.selected_drive);
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// Native parsing retains five patterns, and command/resource errors release only owned buffers.
START_TEST(test_scratch_pattern_limit_and_resource_recovery) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, (unsigned int)_i);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    for (const char* name = "ABCDEF"; *name != 0; ++name) {
        char request[16];
        snprintf(request, sizeof(request), "0:%c,S,W", *name);
        ck_assert_uint_ne(open_name(&unit, &bank, 2, request), DOS_UNUSED);
        put_bytes(&unit, &bank, 2, 0x20, 4);
        ck_assert(dos_close(&unit, &bank, model, 2));
    }
    for (uint8_t secondary = 2; secondary < 7; ++secondary) {
        ck_assert_uint_ne(open_name(&unit, &bank, secondary, "0:F,S,R"), DOS_UNUSED);
    }
    const uint16_t buffers = unit.buffer_use;
    uint8_t extra = 0;
    while (extra < DOS_BUFFER_COUNT && (buffers & (1u << extra))) {
        ++extra;
    }
    ck_assert_uint_lt(extra, DOS_BUFFER_COUNT);
    unit.buffer_use |= 1u << extra;
    const uint16_t exhausted = unit.buffer_use;
    const unsigned int reads = image.reads;
    send_command(&unit, &bank, "S0:A");
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(unit.buffer_use, exhausted);
    ck_assert(!dos_work_pending(&unit));
    unit.buffer_use = buffers;
    const uint16_t retained = unit.buffer_use;
    send_command(&unit, &bank, "S0:A,0:B,0:C,0:D,0:E,0:F");
    finish_open(&unit, &bank);
    ck_assert_str_eq(unit.status, _i == 0 ? "01, FILES SCRATCHED,05,00\r" : "01, FILES SCRATCHED,05,00,0\r");
    ck_assert_uint_eq(unit.buffer_use, retained);
    expect_bytes(&unit, &bank, dos_find_channel(&unit, 2), 4, 0x20);
    free(image.bytes);
}
END_TEST

// SCHTBL's mixed defaults defer an opposite-drive fallback until the third search.
START_TEST(test_scratch_mixed_defaults_require_third_drive_search) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool mounted_peer = (unsigned int)_i % 2u != 0;
    image_t first, second;
    dos_media_bank_t bank = {0}, other = {0};
    mount_blank(&bank, &first, format);
    if (mounted_peer) {
        mount_blank(&other, &second, format);
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    }
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    for (const char* name = "ABCDE"; *name != 0; ++name) {
        if (*name == 'B' && !mounted_peer) {
            continue;
        }
        char request[16];
        snprintf(request, sizeof(request), "%u:%c,S,W", *name == 'B' ? 1u : 0u, *name);
        ck_assert_uint_ne(open_name(&unit, &bank, 2, request), DOS_UNUSED);
        put_bytes(&unit, &bank, 2, 0x20, 4);
        ck_assert(dos_close(&unit, &bank, model, 2));
    }
    send_command(&unit, &bank, "S0:A,1:B,C,0:D,E");
    ck_assert_uint_eq(unit.scratch.scans, mounted_peer ? 2 : 0);
    finish_open(&unit, &bank);
    char expected[DOS_STATUS_LINE_CAPACITY];
    snprintf(expected, sizeof(expected),
             format == 0 ? "01, FILES SCRATCHED,%02u,00\r" : "01, FILES SCRATCHED,%02u,00,0\r", mounted_peer ? 5u : 4u);
    ck_assert_str_eq(unit.status, expected);
    ck_assert_uint_eq(unit.last_drive, 0);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(first.bytes);
    if (mounted_peer) {
        free(second.bytes);
    }
}
END_TEST

// Lookup/chain/write/BAM/flush failures retain peers and never roll back already published deletion.
START_TEST(test_scratch_storage_failures_and_stale_media) {
    const unsigned int format = (unsigned int)_i / 9u, failure = (unsigned int)_i % 9u;
    image_t image, replacement;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    rename_fixture(&unit, &bank);
    if (failure == 4) {
        dos_media_io_t readonly = bank.units[0].slots[0].io;
        readonly.image.write = NULL;
        readonly.flush = NULL;
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, &readonly), DOS_MEDIA_OK);
    }
    const uint8_t peer = open_name(&unit, &bank, 2, "0:EXISTS,S,R");
    ck_assert_uint_ne(peer, DOS_UNUSED);
    const dos_channel_t before = unit.channels[peer];
    const uint16_t buffers = unit.buffer_use;
    image.fail_read = failure < 2 ? image.reads + failure + 1u : 0;
    image.fail_write = failure == 2 ? image.writes + 1u : failure == 7 ? image.writes + 2u : 0;
    image.fail_flush = failure == 3;
    send_command(&unit, &bank, "S0:SOURCE");
    const bool stale = failure == 5 || failure == 6 || failure == 8;
    if (stale) {
        if (failure != 5) {
            dos_work(&unit, &bank);
            ck_assert_uint_ne(unit.scratch.chain_track, 0);
        }
        if (failure == 8) {
            dos_work(&unit, &bank);
            ck_assert_uint_eq(unit.scratch.chain_track, 0);
        }
        mount_blank(&bank, &replacement, format);
    }
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, failure < 2 || failure == 5 || failure == 6 ? DOS_STATUS_READ_ERROR
                                        : failure == 4                              ? DOS_STATUS_WRITE_PROTECT
                                                                                    : DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(unit.buffer_use, buffers);
    ck_assert_mem_eq(&unit.channels[peer], &before, sizeof(before));
    if (stale) {
        ck_assert_uint_eq(replacement.reads + replacement.writes, 0);
        free(replacement.bytes);
    } else {
        image.fail_read = 0;
        expect_bytes(&unit, &bank, peer, 2, 0x60);
        if (failure == 3) {
            ck_assert(bank.units[0].slots[0].dirty);
        }
    }
    free(image.bytes);
}
END_TEST

// Cross-part DELFIL traversal persists each dirty DOS 2.7 BAM without losing prior frees.
START_TEST(test_scratch_releases_chains_across_bam_parts) {
    const unsigned int format = (unsigned int)_i;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    const uint8_t first_track = 50, last_track = format == 1 ? 51 : 101;
    uint8_t* const entry = sector_bytes(&image, type, 39, 1);
    entry[2] = 0x81;
    entry[3] = first_track;
    memset(entry + 5, 0xa0, 16);
    memcpy(entry + 5, "PARTS", 5);
    sector_bytes(&image, type, first_track, 0)[0] = last_track;
    sector_bytes(&image, type, last_track, 0)[1] = 4;
    for (unsigned int block = 0; block < 2; ++block) {
        uint8_t* const bam = fixture_bam_entry(&image, format, block == 0 ? first_track : last_track);
        --bam[0];
        bam[1] &= 0xfe;
    }
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, "S0:PARTS");
    finish_open(&unit, &bank);
    ck_assert_str_eq(unit.status, "01, FILES SCRATCHED,01,00,0\r");
    ck_assert_uint_eq(entry[2], 0);
    for (unsigned int block = 0; block < 2; ++block) {
        const uint8_t track = block == 0 ? first_track : last_track;
        const uint8_t* const bam = fixture_bam_entry(&image, format, track);
        unsigned int sectors;
        ck_assert(diskimage_track_sectors(type, track, &sectors));
        ck_assert_uint_eq(bam[0], sectors);
        ck_assert_uint_ne(bam[1] & 1u, 0);
    }
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// Directory re-entry retains later edits, and corrupt chains cannot own the scheduler forever.
START_TEST(test_scratch_yields_reenters_and_bounds_corrupt_chains) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool corrupt = (unsigned int)_i % 2u;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    rename_fixture(&unit, &bank);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    const unsigned int track = format == 0 ? 18 : 39;
    uint8_t* const first = sector_bytes(&image, type, track, 1);
    uint8_t* const second = sector_bytes(&image, type, track, 2);
    if (corrupt) {
        uint8_t* const data = sector_bytes(&image, type, first[3], first[4]);
        data[0] = first[3];
        data[1] = first[4];
    } else {
        memcpy(second, first, DISKIMAGE_SECTOR_SIZE);
        memset(first, 0, DISKIMAGE_SECTOR_SIZE);
        first[0] = track;
        first[1] = 2;
    }
    const unsigned int reads = image.reads;
    send_command(&unit, &bank, "S0:S*");
    dos_work(&unit, &bank);
    ck_assert(dos_work_pending(&unit));
    ck_assert_uint_eq(image.reads, reads + 1);
    if (!corrupt) {
        ck_assert_uint_eq(unit.scratch.sector, 2);
        dos_work(&unit, &bank);
        ck_assert_uint_ne(unit.scratch.chain_track, 0);
        second[32 + 25] = 0x55;
    }
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, corrupt ? DOS_STATUS_DIRECTORY_ERROR : DOS_STATUS_FILES_SCRATCHED);
    ck_assert_uint_eq(unit.buffer_use, 0);
    if (!corrupt) {
        ck_assert_uint_eq(second[2], 0);
        ck_assert_uint_eq(second[32 + 25], 0x55);
    }
    free(image.bytes);
}
END_TEST

// Compare all persisted bytes against an independently edited allocation oracle.
START_TEST(test_validate_repairs_maps_deletes_splats_and_preserves_counts) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    dos_unit_t unit;
    dos_reset(&unit, model);
    rename_fixture(&unit, &bank);
    uint8_t* const directory = sector_bytes(&image, type, format == 0 ? 18 : 39, 1);
    directory[2] |= 0x40;
    directory[30] = 77;
    directory[34] = 0x41;
    const uint8_t track = directory[35], sector = directory[36];
    uint8_t* const expected = malloc(image.size);
    ck_assert_ptr_nonnull(expected);
    memcpy(expected, image.bytes, image.size);
    image_t oracle = image;
    oracle.bytes = expected;
    sector_bytes(&oracle, type, format == 0 ? 18 : 39, 1)[34] = 0;
    uint8_t* const released = fixture_bam_entry(&oracle, format, track);
    ++released[0];
    released[1u + sector / 8u] |= (uint8_t)(1u << (sector % 8u));
    uint8_t* const orphan = fixture_bam_entry(&image, format, 1);
    --orphan[0];
    orphan[1] &= (uint8_t)~1u;
    if (format != 0) {
        // NEWMPV clears reserved bytes, not merely the valid bitmap records.
        sector_bytes(&image, type, 38, 0)[3] = 0x5a;
        sector_bytes(&image, type, 38, (format == 1 ? 1 : 3) * 3)[255] = 0x5a;
    }
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:SOURCE,S,R"), DOS_UNUSED);
    send_command(&unit, &bank, "V0");
    ck_assert(dos_work_pending(&unit));
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), DOS_UNUSED);
    ck_assert_mem_eq(image.bytes, expected, image.size);
    unsigned int free_count = 0;
    const unsigned int tracks = format == 0 ? 35 : format == 1 ? 77 : 154;
    for (unsigned int current = 1; current <= tracks; ++current) {
        if (current != (format == 0 ? 18u : 39u)) {
            free_count += fixture_bam_entry(&oracle, format, current)[0];
        }
    }
    ck_assert_uint_eq(unit.free_blocks[0], free_count);
    send_command(&unit, &bank, "VALIDATE0:IGNORED");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_mem_eq(image.bytes, expected, image.size);
    ck_assert_uint_ne(open_name(&unit, &bank, 3, "0:NEW,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 3, 0x20, 4);
    ck_assert(dos_close(&unit, &bank, model, 3));
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    send_command(&unit, &bank, "R0:RENAMED=NEW");
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "S0:RENAMED");
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "V0");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.buffer_use, 0);
    const uint8_t reader = open_name(&unit, &bank, 2, "0:SOURCE,S,R");
    expect_bytes(&unit, &bank, reader, 4, 0x20);
    free(expected);
    free(image.bytes);
}
END_TEST

// SIMPRS selects one disk even when INITIALIZE would select both.
START_TEST(test_validate_drive_selection_and_peer_cursor) {
    static const char* const commands[] = {"V", "VALIDATE", "V0", "V1", "V0:1", "VJUNK:0"};
    static const uint8_t drives[] = {1, 1, 0, 1, 0, 0};
    const unsigned int format = (unsigned int)_i / count_of(commands), which = (unsigned int)_i % count_of(commands);
    const uint8_t drive = drives[which], peer = drive ^ 1u;
    dos_media_bank_t bank = {0}, other = {0};
    image_t first, second;
    mount_blank(&bank, &first, format);
    mount_blank(&other, &second, format);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    for (uint8_t slot = 0; slot < 2; ++slot) {
        char name[] = "0:FILE,S,W";
        name[0] += slot;
        ck_assert_uint_ne(open_name(&unit, &bank, 2, name), DOS_UNUSED);
        put_bytes(&unit, &bank, 2, 0x20, 4);
        ck_assert(dos_close(&unit, &bank, model, 2));
    }
    char name[] = "0:FILE,S,R";
    name[0] += peer;
    const uint8_t reader = open_name(&unit, &bank, 2, name);
    dos_get(&unit, &bank, reader, 1);
    const dos_channel_t saved = unit.channels[reader];
    char writer[] = "0:UNCLOSED,S,W";
    writer[0] += drive;
    ck_assert_uint_ne(open_name(&unit, &bank, 3, writer), DOS_UNUSED);
    put_bytes(&unit, &bank, 3, 0x60, 12);
    unit.last_drive = 1;
    const unsigned int peer_writes = peer == 0 ? first.writes : second.writes;
    send_command(&unit, &bank, commands[which]);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.selected_drive, drive);
    ck_assert_uint_eq(unit.last_drive, drive);
    ck_assert_uint_eq(dos_find_channel(&unit, 3), DOS_UNUSED);
    ck_assert_mem_eq(&unit.channels[reader], &saved, sizeof(saved));
    ck_assert_uint_eq(peer == 0 ? first.writes : second.writes, peer_writes);
    expect_bytes(&unit, &bank, reader, 3, 0x21);
    writer[sizeof(writer) - 2] = 'R';
    ck_assert_uint_eq(open_name(&unit, &bank, 3, writer), DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NOT_FOUND);
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// Original 4040 USEDTS tolerates shared blocks. DOS 2.7 VUSED returns 65.
START_TEST(test_validate_crosslinks_are_model_specific) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    rename_fixture(&unit, &bank);
    uint8_t* const directory = sector_bytes(&image, bank.units[0].slots[0].io.image.type, format == 0 ? 18 : 39, 1);
    directory[35] = directory[3];
    directory[36] = directory[4];
    const unsigned int writes = image.writes;
    send_command(&unit, &bank, "V0");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, format == 0 ? DOS_STATUS_OK : DOS_STATUS_NO_BLOCK);
    if (format != 0) {
        ck_assert_uint_eq(image.writes, writes);
        char expected[DOS_STATUS_LINE_CAPACITY];
        snprintf(expected, sizeof(expected), "65,NO BLOCK,%02u,%02u,0\r", directory[3], directory[4]);
        ck_assert_str_eq(unit.status, expected);
    }
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// Fault recovery abandons owned work, never finalizes a splat or rolls back prior writes.
START_TEST(test_validate_storage_errors_and_retry) {
    const unsigned int format = (unsigned int)_i / 6u, failure = (unsigned int)_i % 6u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    rename_fixture(&unit, &bank);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:UNCLOSED,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x60, 12);
    if (failure == 5) {
        bank.units[0].slots[0].io.image.write = NULL;
    }
    send_command(&unit, &bank, "V0");
    if (failure == 0) {
        image.fail_read = image.reads + 1;
    } else {
        while (unit.initialize.pending) {
            dos_work(&unit, &bank);
        }
        image.fail_read = failure == 1 ? image.reads + 2 : 0;
        image.fail_write = failure == 2 ? image.writes + 1 : failure == 3 ? image.writes + 2 : 0;
        image.fail_flush = failure == 4;
    }
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, failure < 2    ? DOS_STATUS_READ_ERROR
                                        : failure == 5 ? DOS_STATUS_WRITE_PROTECT
                                                       : DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), DOS_UNUSED);
    ck_assert(!unit.bam[0].dirty);
    image.fail_read = image.fail_write = 0;
    image.fail_flush = false;
    bank.units[0].slots[0].io.image.write = write_image;
    send_command(&unit, &bank, "V0");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:SOURCE,S,R"), DOS_UNUSED);
    free(image.bytes);
}
END_TEST

// Cycles and out-of-range links are bounded failures, while closed REL is traversed.
START_TEST(test_validate_corruption_and_side_links) {
    const unsigned int format = (unsigned int)_i / 4u, variant = (unsigned int)_i % 4u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    rename_fixture(&unit, &bank);
    const uint16_t free_blocks = unit.free_blocks[0];
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    uint8_t* const directory = sector_bytes(&image, type, format == 0 ? 18 : 39, 1);
    uint8_t* const data = sector_bytes(&image, type, directory[3], directory[4]);
    if (variant == 0) {
        directory[3] = 255;
    } else if (variant == 1) {
        data[0] = directory[3];
        data[1] = directory[4];
    } else if (variant == 2) {
        directory[2] = 0x84;
    } else {
        directory[21] = 2;
        directory[22] = 0;
        sector_bytes(&image, type, 2, 0)[1] = 0xff;
    }
    send_command(&unit, &bank, "V0");
    unsigned int steps = 0;
    while (dos_work_pending(&unit)) {
        ck_assert_uint_lt(steps++, image.size / DISKIMAGE_SECTOR_SIZE + 128);
        dos_work(&unit, &bank);
    }
    ck_assert_uint_eq(unit.status_code, variant >= 2   ? DOS_STATUS_OK
                                        : variant == 0 ? DOS_STATUS_ILLEGAL_TRACK_SECTOR
                                        : format == 0  ? DOS_STATUS_READ_ERROR
                                                       : DOS_STATUS_NO_BLOCK);
    if (variant == 3) {
        ck_assert_uint_eq(fixture_bam_entry(&image, format, 2)[1] & 1u, 0);
    } else if (variant == 2) {
        ck_assert(!unit.bam[0].dirty);
        ck_assert_uint_eq(unit.free_blocks[0], free_blocks);
    }
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// A fresh identity cannot receive the old job's maps, even between cooperative phases.
START_TEST(test_validate_stale_media_never_reaches_replacement_io) {
    const unsigned int format = (unsigned int)_i / 3u, stage = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0}, replacement = {0};
    image_t image, next;
    mount_blank(&bank, &image, format);
    mount_blank(&replacement, &next, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    rename_fixture(&unit, &bank);
    send_command(&unit, &bank, "V0");
    if (stage != 0) {
        while (unit.initialize.pending) {
            dos_work(&unit, &bank);
        }
    }
    if (stage == 2) {
        while (!unit.validate.directory_chain) {
            ck_assert(dos_work_pending(&unit));
            dos_work(&unit, &bank);
        }
    }
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, &replacement.units[0].slots[0].io), DOS_MEDIA_OK);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_uint_eq(next.reads + next.writes, 0);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
    free(next.bytes);
}
END_TEST

// Near-capacity peer ownership must not be overwritten by fixed scratch pages.
START_TEST(test_validate_resource_pressure_and_absent_slot) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0}, other = {0};
    image_t first, second;
    mount_blank(&bank, &first, format);
    mount_blank(&other, &second, format);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    for (uint8_t secondary = 2; secondary < 7; ++secondary) {
        char name[] = "1:PEER0,S,W";
        name[6] += secondary;
        ck_assert_uint_ne(open_name(&unit, &bank, 2, name), DOS_UNUSED);
        put_bytes(&unit, &bank, 2, 0x20, 4);
        ck_assert(dos_close(&unit, &bank, model, 2));
    }
    for (uint8_t secondary = 2; secondary < 7; ++secondary) {
        char name[] = "1:PEER0,S,R";
        name[6] += secondary;
        ck_assert_uint_ne(open_name(&unit, &bank, secondary, name), DOS_UNUSED);
    }
    dos_channel_t saved[DOS_CHANNEL_COUNT];
    memcpy(saved, unit.channels, sizeof(saved));
    const uint16_t buffers = unit.buffer_use;
    send_command(&unit, &bank, "V0");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, format == 0 ? DOS_STATUS_OK : DOS_STATUS_NO_CHANNEL);
    ck_assert_uint_eq(unit.buffer_use, buffers);
    ck_assert_mem_eq(unit.channels, saved, sizeof(saved));
    ck_assert(dos_close(&unit, &bank, model, 15));
    send_command(&unit, &bank, "V0");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, NULL), DOS_MEDIA_OK);
    send_command(&unit, &bank, "V1");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NOT_READY);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// Rebuild every native BAM range and reserve an extended directory by its links.
START_TEST(test_validate_marks_all_bam_parts_and_directory_extensions) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    const uint8_t directory_track = format == 0 ? 18 : 39;
    const uint8_t tracks[] = {1, format == 0 ? 35 : 51, format == 2 ? 101 : 0, format == 2 ? 151 : 0};
    uint8_t* const directory = sector_bytes(&image, type, directory_track, 1);
    directory[0] = directory_track;
    directory[1] = 2;
    sector_bytes(&image, type, directory_track, 2)[1] = 0xff;
    directory[2] = 0x81;
    directory[3] = tracks[0];
    memcpy(directory + 5, "CHAIN", 5);
    for (unsigned int index = 0; index < count_of(tracks) && tracks[index] != 0; ++index) {
        uint8_t* const bytes = sector_bytes(&image, type, tracks[index], 0);
        bytes[0] = index + 1 < count_of(tracks) ? tracks[index + 1] : 0;
        bytes[1] = bytes[0] == 0 ? 3 : 0;
        bytes[2] = 0x20 + index;
    }
    image_t oracle = image;
    oracle.bytes = malloc(image.size);
    ck_assert_ptr_nonnull(oracle.bytes);
    memcpy(oracle.bytes, image.bytes, image.size);
    for (unsigned int index = 0; index < count_of(tracks) && tracks[index] != 0; ++index) {
        uint8_t* const entry = fixture_bam_entry(&oracle, format, tracks[index]);
        --entry[0];
        entry[1] &= (uint8_t)~1u;
    }
    uint8_t* const directory_map = fixture_bam_entry(&oracle, format, directory_track);
    --directory_map[0];
    directory_map[1] &= (uint8_t)~4u;
    dos_unit_t unit;
    dos_reset(&unit, model);
    send_command(&unit, &bank, "V0");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_mem_eq(image.bytes, oracle.bytes, image.size);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(oracle.bytes);
    free(image.bytes);
}
END_TEST

// RENAME must not bypass an unpublished writer's canonical namespace reservation.
START_TEST(test_rename_respects_unpublished_writer_namespace) {
    const unsigned int format = (unsigned int)_i / 4u, scenario = (unsigned int)_i % 4u;
    dos_media_bank_t bank = {0};
    image_t first, second;
    mount_blank(&bank, &first, format);
    if (scenario == 1 || scenario == 2) {
        mount_peer(&bank, &second, format);
    }
    image_t* const source = scenario == 1 ? &second : &first;
    image_t* const pending = scenario == 2 ? &second : &first;
    fixture_file(source, format, 0, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    const dos_model_t model = bank.units[0].model;
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    const uint8_t track = format == 0 ? 18 : 39;
    if (scenario == 3) {
        uint8_t* const directory = sector_bytes(&first, type, track, 1);
        memcpy(sector_bytes(&first, type, track, 2), directory, DISKIMAGE_SECTOR_SIZE);
        memset(directory, 0, DISKIMAGE_SECTOR_SIZE);
        directory[0] = track;
        directory[1] = 2;
    }
    dos_unit_t unit;
    dos_reset(&unit, model);
    pending->fail_write = pending->writes + 1u;
    pending->fail_write_without_changes = true;
    const uint8_t owner = open_name(&unit, &bank, 2, scenario == 2 ? "1:NEW,S,W" : "0:NEW,S,W");
    ck_assert_uint_ne(owner, DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    const dos_channel_t saved = unit.channels[owner];
    send_command(&unit, &bank, scenario == 1 ? "R0:NEW=1:ABSENT" : "R0:NEW=ABSENT");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NOT_FOUND);
    const unsigned int writes = first.writes + (scenario == 1 || scenario == 2 ? second.writes : 0);
    send_command(&unit, &bank, scenario == 1 ? "R0:NEW=1:SOURCE" : "R0:NEW=SOURCE");
    if (scenario == 3) {
        dos_work(&unit, &bank);
        ck_assert(dos_work_pending(&unit));
        ck_assert(dos_close(&unit, &bank, model, 2));
    }
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, scenario == 2 ? DOS_STATUS_OK : DOS_STATUS_WRITE_ERROR);
    if (scenario != 3) {
        ck_assert_mem_eq(&unit.channels[owner], &saved, sizeof(saved));
        ck_assert_uint_eq(first.writes + (scenario == 1 || scenario == 2 ? second.writes : 0),
                          writes + (scenario == 2));
        ck_assert(dos_close(&unit, &bank, model, 2));
    }
    const uint8_t reader = open_name(&unit, &bank, 3,
                                     scenario == 1   ? "1:SOURCE"
                                     : scenario == 2 ? "0:NEW"
                                                     : "0:SOURCE");
    expect_bytes(&unit, &bank, reader, 22, 0x20);
    ck_assert(dos_close(&unit, &bank, model, 3));
    const uint8_t empty = open_name(&unit, &bank, 3, scenario == 2 ? "1:NEW" : "0:NEW");
    expect_bytes(&unit, &bank, empty, 0, 0);
    free(first.bytes);
    if (scenario == 1 || scenario == 2) {
        free(second.bytes);
    }
}
END_TEST

// Failed source publication must not acquire a second, partially renamed namespace.
START_TEST(test_rename_preserves_failed_source_publication_intent) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    image.fail_write = 1;
    const uint8_t owner = open_name(&unit, &bank, 2, "0:SOURCE,S,W");
    ck_assert_uint_ne(owner, DOS_UNUSED);
    ck_assert(!unit.channels[owner].directory_published);
    const dos_channel_t saved = unit.channels[owner];
    const unsigned int writes = image.writes;
    image.fail_write = writes + 1u;
    send_command(&unit, &bank, "R0:NEW=SOURCE");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_mem_eq(&unit.channels[owner], &saved, sizeof(saved));
    const uint8_t* const entry = sector_bytes(&image, bank.units[0].slots[0].io.image.type, format == 0 ? 18 : 39, 1);
    ck_assert_mem_eq(entry + 5, "SOURCE", 6);
    image.fail_write = 0;
    ck_assert_uint_ne(open_name(&unit, &bank, 3, "0:NEW,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 3, 0x40, 22);
    ck_assert(dos_close(&unit, &bank, model, 3));
    ck_assert(dos_close(&unit, &bank, model, 2));
    const uint8_t retained = open_name(&unit, &bank, 2, "0:NEW");
    expect_bytes(&unit, &bank, retained, 22, 0x40);
    ck_assert(dos_close(&unit, &bank, model, 2));
    send_command(&unit, &bank, "R0:DONE=SOURCE");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    const uint8_t reader = open_name(&unit, &bank, 2, "0:DONE");
    expect_bytes(&unit, &bank, reader, 0, 0);
    free(image.bytes);
}
END_TEST

// Completion must validate the old generation before any unit-wide flush callback.
START_TEST(test_scratch_stale_completion_never_flushes_replacement) {
    const unsigned int format = (unsigned int)_i / 4u, scenario = (unsigned int)_i % 4u;
    const bool matched = scenario % 2u == 0, peer_change = scenario >= 2;
    dos_media_bank_t bank = {0}, replacement = {0};
    image_t image, next, peer;
    mount_blank(&bank, &image, format);
    if (peer_change) {
        mount_peer(&bank, &peer, format);
    }
    fixture_file(&image, format, 0, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, matched ? "S0:SOURCE" : "S0:ABSENT");
    if (matched) {
        while (unit.scratch.count == 0) {
            ck_assert(dos_work_pending(&unit));
            dos_work(&unit, &bank);
        }
    } else {
        dos_work(&unit, &bank);
        ck_assert_uint_eq(unit.scratch.track, 0);
    }
    ck_assert(dos_work_pending(&unit));
    mount_blank(&replacement, &next, format);
    const uint8_t slot = peer_change ? 1 : 0, track = format == 0 ? 18 : 39;
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, slot, &replacement.units[0].slots[0].io), DOS_MEDIA_OK);
    dos_media_ref_t reference;
    ck_assert_int_eq(dos_media_reference(&bank, 0, slot, &reference), DOS_MEDIA_OK);
    ck_assert_int_eq(dos_media_write(&bank, reference, track, 1,
                                     sector_bytes(&next, bank.units[0].slots[slot].io.image.type, track, 1)),
                     DOS_MEDIA_OK);
    const unsigned int next_reads = next.reads, next_writes = next.writes, next_flushes = next.flushes;
    const unsigned int old_reads = image.reads, old_writes = image.writes, old_flushes = image.flushes;
    finish_open(&unit, &bank);
    ck_assert_str_eq(unit.status, format == 0 ? "25, WRITE ERROR,00,00\r" : "25, WRITE ERROR,00,00,0\r");
    ck_assert_uint_eq(next.reads, next_reads);
    ck_assert_uint_eq(next.writes, next_writes);
    ck_assert_uint_eq(next.flushes, next_flushes);
    ck_assert_uint_eq(image.reads, old_reads);
    ck_assert_uint_eq(image.writes, old_writes);
    ck_assert_uint_eq(image.flushes, old_flushes);
    ck_assert(bank.units[0].slots[slot].dirty);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
    free(next.bytes);
    if (peer_change) {
        free(peer.bytes);
    }
}
END_TEST

// Peer status cannot stand in for delegated INITDR success or recovery failure.
START_TEST(test_validate_initialization_ignores_interleaved_peer_status) {
    const unsigned int format = (unsigned int)_i / 3u, scenario = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0};
    image_t image, peer;
    mount_blank(&bank, &image, format);
    mount_peer(&bank, &peer, format);
    fixture_file(&image, format, 0, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    fixture_file(&peer, format, 0, "PEER", DISKIMAGE_FTYPE_SEQ, 0x40, 255);
    uint8_t* const orphan = fixture_bam_entry(&image, format, 2);
    orphan[1] &= (uint8_t)~1u;
    --orphan[0];
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    peer.fail_read = peer.reads + 3u;
    const uint8_t reader = open_name(&unit, &bank, 2, "1:PEER");
    ck_assert_uint_ne(reader, DOS_UNUSED);
    const uint16_t buffers = unit.buffer_use;
    send_command(&unit, &bank, "V0");
    if (scenario != 0) {
        while (unit.initialize.pending) {
            dos_work(&unit, &bank);
        }
        image.fail_write = image.writes + 1u;
        while (!unit.initialize.pending) {
            ck_assert(dos_work_pending(&unit));
            dos_work(&unit, &bank);
        }
        ck_assert_uint_eq(unit.validate.error, DOS_STATUS_WRITE_ERROR);
        if (scenario == 2) {
            image.fail_read = image.reads + 1u;
        }
    }
    uint8_t bytes[DOS_STAGE_CAPACITY];
    bool eoi;
    for (unsigned int remaining = DISKIMAGE_SECTOR_PAYLOAD_SIZE; remaining != 0;) {
        const unsigned int count = MIN(remaining, sizeof(bytes));
        ck_assert_uint_eq(dos_stage(&unit, &bank, reader, bytes, count, &eoi), count);
        dos_get(&unit, &bank, reader, count);
        remaining -= count;
    }
    ck_assert_uint_eq(dos_stage(&unit, &bank, reader, bytes, sizeof(bytes), &eoi), 0);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    const unsigned int peer_writes = peer.writes;
    finish_open(&unit, &bank);
    char expected[DOS_STATUS_LINE_CAPACITY];
    if (scenario == 0) {
        strcpy(expected, format == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r");
        ck_assert_uint_ne(fixture_bam_entry(&image, format, 2)[1] & 1u, 0);
    } else {
        snprintf(expected, sizeof(expected),
                 scenario == 1 ? (format == 0 ? "25, WRITE ERROR,%02u,00\r" : "25, WRITE ERROR,%02u,00,0\r")
                               : (format == 0 ? "23,READ ERROR,%02u,00\r" : "23,READ ERROR,%02u,00,0\r"),
                 scenario == 1 && format != 0 ? 38u
                 : format == 0                ? 18u
                                              : 39u);
    }
    ck_assert_str_eq(unit.status, expected);
    ck_assert_uint_eq(unit.buffer_use, buffers);
    ck_assert(unit.channels[reader].allocated);
    ck_assert_uint_eq(peer.writes, peer_writes);
    free(image.bytes);
    free(peer.bytes);
}
END_TEST

// VD17 processes saved data starts, and DOS 2.7 TSCHK rejects invalid geometry.
START_TEST(test_validate_invalid_data_start_after_side_chain) {
    const unsigned int format = (unsigned int)_i / 4u;
    const bool side = (unsigned int)_i % 2u != 0;
    const bool zero_track = (unsigned int)_i % 4u < 2;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    uint8_t* const entry = sector_bytes(&image, type, format == 0 ? 18 : 39, 1);
    unsigned int sectors;
    ck_assert(diskimage_track_sectors(type, 1, &sectors));
    entry[3] = zero_track ? 0 : 1;
    entry[4] = zero_track ? 7 : sectors;
    if (side) {
        entry[21] = 2;
        entry[22] = 0;
        sector_bytes(&image, type, 2, 0)[1] = 0xff;
    }
    const unsigned int writes = image.writes;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, "V0");
    finish_open(&unit, &bank);
    char expected[DOS_STATUS_LINE_CAPACITY];
    snprintf(expected, sizeof(expected),
             format == 0 ? "66,ILLEGAL TRACK OR SECTOR,%02u,%02u\r" : "66,ILLEGAL TRACK OR SECTOR,%02u,%02u,0\r",
             entry[3], entry[4]);
    ck_assert_str_eq(unit.status, expected);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// SC15 persists side-chain release before SC17 attempts even an invalid data start.
START_TEST(test_scratch_side_chain_precedes_invalid_data_start) {
    const unsigned int format = (unsigned int)_i / 5u, scenario = (unsigned int)_i % 5u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    const dos_model_t model = bank.units[0].model;
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    uint8_t* const entry = sector_bytes(&image, type, format == 0 ? 18 : 39, 1);
    entry[3] = 0;
    entry[4] = 7;
    if (scenario != 1) {
        entry[21] = 2;
        uint8_t* const first = sector_bytes(&image, type, 2, 0);
        first[0] = 2;
        first[1] = 1;
        sector_bytes(&image, type, 2, 1)[1] = 0xff;
        uint8_t* const bam = fixture_bam_entry(&image, format, 2);
        bam[0] -= 2;
        bam[1] &= (uint8_t)~3u;
    }
    if (scenario == 2) {
        entry[2] &= 0x7f;
    }
    image_t expected = image;
    expected.bytes = malloc(image.size);
    ck_assert_ptr_nonnull(expected.bytes);
    memcpy(expected.bytes, image.bytes, image.size);
    expected.bytes[entry - image.bytes + 2] = 0;
    if (scenario == 0 || scenario == 2) {
        uint8_t* const bam = fixture_bam_entry(&expected, format, 2);
        bam[0] += 2;
        bam[1] |= 3u;
    }
    dos_unit_t unit;
    dos_reset(&unit, model);
    send_command(&unit, &bank, "S0:SOURCE");
    dos_work(&unit, &bank);
    if (scenario == 3) {
        image.fail_write = image.writes + 1u;
        image.fail_write_without_changes = true;
    } else if (scenario == 4) {
        dos_work(&unit, &bank);
        image.fail_read = image.reads + 1u;
    }
    finish_open(&unit, &bank);
    char status[DOS_STATUS_LINE_CAPACITY];
    if (scenario < 2) {
        strcpy(status, format == 0 ? "66,ILLEGAL TRACK OR SECTOR,00,07\r" : "66,ILLEGAL TRACK OR SECTOR,00,07,0\r");
    } else if (scenario == 2) {
        strcpy(status, format == 0 ? "01, FILES SCRATCHED,01,00\r" : "01, FILES SCRATCHED,01,00,0\r");
    } else if (scenario == 3) {
        snprintf(status, sizeof(status), format == 0 ? "25, WRITE ERROR,%02u,00\r" : "25, WRITE ERROR,%02u,00,0\r",
                 format == 0 ? 18u : 38u);
    } else {
        strcpy(status, format == 0 ? "23,READ ERROR,02,01\r" : "23,READ ERROR,02,01,0\r");
    }
    ck_assert_str_eq(unit.status, status);
    ck_assert_mem_eq(image.bytes, expected.bytes, image.size);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(expected.bytes);
    free(image.bytes);
}
END_TEST

// HEXDEC keeps two digits on 4040 and an optional hundreds digit on DOS 2.7.
START_TEST(test_scratch_native_count_digit_boundaries) {
    static const struct {
        unsigned int count;
        const char* digits_4040;
        const char* digits_27;
    } cases[] = {
        {99, "99", "99"},   {100, "00", "100"}, {199, "99", "199"},
        {200, "00", "200"}, {255, "55", "255"}, {256, "00", "00"},
    };
    const unsigned int format = (unsigned int)_i / count_of(cases), scenario = (unsigned int)_i % count_of(cases);
    dos_media_bank_t bank = {0};
    image_t images[2];
    mount_blank(&bank, &images[0], format);
    mount_peer(&bank, &images[1], format);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    for (unsigned int index = 0; index < cases[scenario].count; ++index) {
        char name[24];
        snprintf(name, sizeof(name), "%u:F%03u,S,W", index / 128u, index);
        ck_assert_uint_ne(open_name(&unit, &bank, 2, name), DOS_UNUSED);
        ck_assert(dos_close(&unit, &bank, model, 2));
    }
    send_command(&unit, &bank, "S0:F*,1:F*");
    finish_open(&unit, &bank);
    char status[DOS_STATUS_LINE_CAPACITY];
    snprintf(status, sizeof(status), format == 0 ? "01, FILES SCRATCHED,%s,00\r" : "01, FILES SCRATCHED,%s,00,1\r",
             format == 0 ? cases[scenario].digits_4040 : cases[scenario].digits_27);
    ck_assert_str_eq(unit.status, status);
    for (unsigned int slot = 0; slot < count_of(images); ++slot) {
        const diskimage_type_t type = bank.units[0].slots[slot].io.image.type;
        uint8_t track = format == 0 ? 18 : 39, sector = 1;
        unsigned int visited = 0, deleted = 0;
        while (track != 0) {
            ck_assert_uint_lt(visited++, 18);
            const uint8_t* const bytes = sector_bytes(&images[slot], type, track, sector);
            for (unsigned int offset = 0; offset < DISKIMAGE_SECTOR_SIZE; offset += DISKIMAGE_DIRECTORY_ENTRY_SIZE) {
                ck_assert_uint_eq(bytes[offset + 2], 0);
                deleted += bytes[offset + 5] == 'F';
            }
            track = bytes[0];
            sector = bytes[1];
        }
        ck_assert_uint_eq(deleted, slot == 0 ? MIN(cases[scenario].count, 128u)
                                             : cases[scenario].count - MIN(cases[scenario].count, 128u));
        free(images[slot].bytes);
    }
    ck_assert_uint_eq(unit.buffer_use, 0);
}
END_TEST

// The same ROM number conversion applies to error track and sector fields.
START_TEST(test_status_native_decimal_field_boundaries) {
    static const struct {
        uint8_t track;
        uint8_t sector;
        const char* fields_4040;
        const char* fields_27;
    } cases[] = {
        {99, 100, "99,00", "99,100"},
        {100, 255, "00,55", "100,255"},
        {255, 99, "55,99", "255,99"},
    };
    const unsigned int format = (unsigned int)_i / count_of(cases), scenario = (unsigned int)_i % count_of(cases);
    const dos_model_t model = format == 0 ? DOS_MODEL_4040 : format == 1 ? DOS_MODEL_8050 : DOS_MODEL_8250;
    dos_unit_t unit;
    dos_reset(&unit, model);
    dos_status(&unit, model, DOS_STATUS_READ_ERROR, cases[scenario].track, cases[scenario].sector);
    char status[DOS_STATUS_LINE_CAPACITY];
    snprintf(status, sizeof(status), format == 0 ? "23,READ ERROR,%s\r" : "23,READ ERROR,%s,0\r",
             format == 0 ? cases[scenario].fields_4040 : cases[scenario].fields_27);
    ck_assert_str_eq(unit.status, status);
}
END_TEST

// A BASIC listing cursor is not OPREAD's retained file-entry binding.
START_TEST(test_scratch_listing_cursors_do_not_protect_unopened_files) {
    const unsigned int format = (unsigned int)_i / 4u, scenario = (unsigned int)_i % 4u;
    dos_media_bank_t bank = {0};
    image_t images[2];
    mount_blank(&bank, &images[0], format);
    mount_peer(&bank, &images[1], format);
    fixture_file(&images[0], format, 0, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    fixture_file(&images[0], format, 1, "EXISTS", DISKIMAGE_FTYPE_SEQ, 0x40, 22);
    fixture_file(&images[1], format, 0, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x60, 22);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    uint8_t reader = DOS_UNUSED;
    const uint8_t listing = open_name(&unit, &bank, 0, "$0");
    ck_assert_uint_ne(listing, DOS_UNUSED);
    if (scenario == 1 || scenario == 2) {
        for (unsigned int row = 0; row < 2; ++row) {
            uint8_t bytes[DOS_STAGE_CAPACITY];
            bool eoi;
            const unsigned int length = dos_stage(&unit, &bank, listing, bytes, sizeof(bytes), &eoi);
            ck_assert_uint_eq(length, 32);
            dos_get(&unit, &bank, listing, length);
        }
    }
    if (scenario == 2) {
        reader = open_name(&unit, &bank, 2, "0:SOURCE");
        ck_assert_uint_ne(reader, DOS_UNUSED);
        ck_assert_uint_eq(dos_find_channel(&unit, 0), listing);
    }
    const dos_channel_t saved = unit.channels[listing];
    const uint16_t buffers = unit.buffer_use;
    send_command(&unit, &bank,
                 scenario == 0   ? "S0:SOURCE"
                 : scenario == 1 ? "S0:EXISTS"
                 : scenario == 2 ? "S0:SOURCE,EXISTS"
                                 : "S1:SOURCE");
    finish_open(&unit, &bank);
    char status[DOS_STATUS_LINE_CAPACITY];
    if (format == 0) {
        strcpy(status, "01, FILES SCRATCHED,01,00\r");
    } else {
        snprintf(status, sizeof(status), "01, FILES SCRATCHED,01,00,%u\r", scenario == 3);
    }
    ck_assert_str_eq(unit.status, status);
    const unsigned int slot = scenario == 3, offset = scenario == 1 || scenario == 2 ? 32u : 0u;
    const uint8_t* const entry =
        sector_bytes(&images[slot], bank.units[0].slots[slot].io.image.type, format == 0 ? 18 : 39, 1);
    ck_assert_uint_eq(entry[offset + 2], 0);
    if (scenario == 2) {
        ck_assert_uint_eq(entry[2], 0x80 | DISKIMAGE_FTYPE_SEQ);
        expect_bytes(&unit, &bank, reader, 22, 0x20);
    }
    ck_assert_mem_eq(&unit.channels[listing], &saved, sizeof(saved));
    ck_assert_uint_eq(unit.buffer_use, buffers);
    ck_assert(dos_close(&unit, &bank, model, 0));
    if (reader != DOS_UNUSED) {
        ck_assert(dos_close(&unit, &bank, model, 2));
    }
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(images[0].bytes);
    free(images[1].bytes);
}
END_TEST

// Retained EOF bytes may form names, but selectors cannot escape saved command RAM.
START_TEST(test_scratch_cross_request_command_storage_boundary) {
    const unsigned int format = (unsigned int)_i / 3u, scenario = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, scenario == 0 ? "\x03" : scenario == 1 ? "Z" : ":", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    uint8_t prior[DOS_COMMAND_RECEIVE_CAPACITY];
    memset(prior, 'X', sizeof(prior));
    prior[DOS_COMMAND_LIMIT] = scenario == 1 ? 'Z' : ':';
    dos_command(&unit, &bank, 0, prior, sizeof(prior));
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_COMMAND_TOO_LONG);
    image_t expected = image;
    expected.bytes = malloc(image.size);
    ck_assert_ptr_nonnull(expected.bytes);
    memcpy(expected.bytes, image.bytes, image.size);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    uint8_t* const entry = sector_bytes(&expected, type, format == 0 ? 18 : 39, 1);
    if (scenario != 0) {
        entry[2] = 0;
        uint8_t* const bam = fixture_bam_entry(&expected, format, 1);
        ++bam[0];
        bam[1] |= 1u;
    }
    uint8_t request[DOS_COMMAND_LIMIT];
    memset(request, 'A', sizeof(request));
    memcpy(request, "S0:", 3);
    request[sizeof(request) - 1u] = ',';
    if (scenario == 2) {
        request[sizeof(request) - 2u] = ',';
        request[sizeof(request) - 1u] = ':';
    }
    const unsigned int reads = image.reads, writes = image.writes, flushes = image.flushes;
    dos_command(&unit, &bank, 0, request, sizeof(request));
    finish_open(&unit, &bank);
    ck_assert_mem_eq(image.bytes, expected.bytes, image.size);
    ck_assert_str_eq(unit.status,
                     scenario == 0 ? (format == 0 ? "30,SYNTAX ERROR,00,00\r" : "30,SYNTAX ERROR,00,00,0\r")
                                   : (format == 0 ? "01, FILES SCRATCHED,01,00\r" : "01, FILES SCRATCHED,01,00,0\r"));
    if (scenario == 0) {
        ck_assert_uint_eq(image.reads, reads);
        ck_assert_uint_eq(image.writes, writes);
        ck_assert_uint_eq(image.flushes, flushes);
    }
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(expected.bytes);
    free(image.bytes);
}
END_TEST

// Compare preview/committed bytes, retaining circular direct channels at EOI.
static void expect_direct(dos_unit_t* const unit, dos_media_bank_t* const bank, const uint8_t channel,
                          const uint8_t* const expected, const unsigned int count, const bool final_eoi) {
    unsigned int position = 0;
    while (position < count) {
        uint8_t bytes[DOS_STAGE_CAPACITY], again[DOS_STAGE_CAPACITY];
        const unsigned int capacity = MIN(count - position, DOS_STAGE_CAPACITY);
        const dos_channel_t before = unit->channels[channel];
        bool eoi, repeated_eoi;
        const unsigned int size = dos_stage(unit, bank, channel, bytes, capacity, &eoi);
        ck_assert_uint_eq(size, capacity);
        ck_assert_mem_eq(bytes, expected + position, size);
        ck_assert_mem_eq(&unit->channels[channel], &before, sizeof(before));
        ck_assert_uint_eq(dos_stage(unit, bank, channel, again, capacity, &repeated_eoi), size);
        ck_assert_mem_eq(bytes, again, size);
        ck_assert_int_eq(eoi, repeated_eoi);
        ck_assert_int_eq(eoi, final_eoi && position + size == count);
        const unsigned int consumed = MIN(size, 3u);
        dos_get(unit, bank, channel, consumed);
        position += consumed;
    }
    ck_assert_int_eq(unit->channels[channel].ready, unit->channels[channel].access == DOS_ACCESS_DIRECT || !final_eoi);
}

// Build linked and singleton readable files directly, including deterministic unused tail bytes.
static void mount_reader_pointer_image(dos_media_bank_t* const bank, image_t* const image, const unsigned int format) {
    mount_blank(bank, image, format);
    const diskimage_type_t type = bank->units[0].slots[0].io.image.type;
    uint8_t* const directory = sector_bytes(image, type, format == 0 ? 18 : 39, 1);
    for (unsigned int entry = 0; entry < 2; ++entry) {
        uint8_t* const file = directory + entry * 32u;
        file[2] = 0x82;
        file[3] = 1;
        file[4] = entry == 0 ? 0 : 2;
        memset(file + 5, 0xa0, 16);
        memcpy(file + 5, entry == 0 ? "READ" : "ONE", entry == 0 ? 4 : 3);
        file[30] = entry == 0 ? 2 : 1;
    }
    uint8_t* const first = sector_bytes(image, type, 1, 0);
    first[0] = 1;
    first[1] = 1;
    for (unsigned int index = 2; index < DISKIMAGE_SECTOR_SIZE; ++index) {
        first[index] = index - 2u;
    }
    uint8_t* const last = sector_bytes(image, type, 1, 1);
    memset(last, 0xcc, DISKIMAGE_SECTOR_SIZE);
    last[0] = 0;
    last[1] = 8;
    for (unsigned int index = 2; index <= last[1]; ++index) {
        last[index] = 254u + index - 2u;
    }
    uint8_t* const one = sector_bytes(image, type, 1, 2);
    memset(one, 0xcc, DISKIMAGE_SECTOR_SIZE);
    one[0] = 0;
    one[1] = 2;
    one[2] = 'A';
    uint8_t* const bam = sector_bytes(image, type, format == 0 ? 18 : 38, 0);
    uint8_t* const entry = bam + (format == 0 ? 4 : 6);
    entry[0] -= 3;
    entry[1] &= ~7u;
}

static const struct {
    const char* name;
    dos_status_code_t status;
    uint8_t buffer;
} direct_names[] = {
    {"#", DOS_STATUS_OK, 11},
    {"#0", DOS_STATUS_OK, 0},
    {"#11", DOS_STATUS_OK, 11},
    {"#12", DOS_STATUS_NO_CHANNEL, DOS_UNUSED},
    {"#255", DOS_STATUS_NO_CHANNEL, DOS_UNUSED},
    {"#256", DOS_STATUS_OK, 0},
    {"#1000", DOS_STATUS_OK, 0},
    {"#A", DOS_STATUS_SYNTAX, DOS_UNUSED},
    {"# ", DOS_STATUS_OK, 0},
    {"#, 3", DOS_STATUS_OK, 3},
    {"#0,1,2,3", DOS_STATUS_OK, 0},
    {"#0,1,2,3,4", DOS_STATUS_SYNTAX, DOS_UNUSED},
    {"#:", DOS_STATUS_OK, 10},
    {"#?", DOS_STATUS_NO_CHANNEL, DOS_UNUSED},
    {"#\0352", DOS_STATUS_OK, 2},
    {"#11\r", DOS_STATUS_OK, 11},
};

static const struct {
    const char* command;
    dos_status_code_t status;
    uint8_t position;
} pointer_commands[] = {
    {"B-P:2,0", DOS_STATUS_OK, 0},           {"B-P2,13", DOS_STATUS_OK, 13},
    {"BUFFER-P:2,254", DOS_STATUS_OK, 254},  {"B-P:2,255", DOS_STATUS_OK, 255},
    {"B-P:258,256", DOS_STATUS_OK, 0},       {"B-P:1002,1255", DOS_STATUS_OK, 255},
    {"B-P:\0352, 17", DOS_STATUS_OK, 17},    {"B-P:2,,17", DOS_STATUS_OK, 17},
    {"B-P:2,13A", DOS_STATUS_OK, 13},        {"B-P:2", DOS_STATUS_OK, 0},
    {"B-P:2,:", DOS_STATUS_OK, 10},          {"B-P:2,?", DOS_STATUS_OK, 15},
    {"B-P:2,0,1,2", DOS_STATUS_OK, 0},       {"B-P:2,0,1,2,3", DOS_STATUS_SYNTAX, 0},
    {"B,P-:2,0", DOS_STATUS_SYNTAX, 0},      {"B-X:2,0", DOS_STATUS_INVALID_COMMAND, 0},
    {"B,-", DOS_STATUS_SYNTAX, 0},           {"B", DOS_STATUS_INVALID_COMMAND, 0},
    {"B-", DOS_STATUS_INVALID_COMMAND, 0},   {"B-P:0,0", DOS_STATUS_NO_CHANNEL, 0},
    {"B-P:1,0", DOS_STATUS_NO_CHANNEL, 0},   {"B-P:9,0", DOS_STATUS_NO_CHANNEL, 0},
    {"B-P:14,0", DOS_STATUS_NO_CHANNEL, 0},  {"B-P:15,0", DOS_STATUS_NO_CHANNEL, 0},
    {"B-P:255,0", DOS_STATUS_NO_CHANNEL, 0}, {"B-P:A,0", DOS_STATUS_SYNTAX, 0},
    {"B-P:2,A", DOS_STATUS_OK, 0},           {"B-P:2,0\r", DOS_STATUS_OK, 0},
    {"B-P:258,A", DOS_STATUS_OK, 200},
};

static const struct {
    const char* command;
    dos_status_code_t status;
} block_read_commands[] = {
    {"B-R2,0,1,0", DOS_STATUS_OK},
    {"BLOCK-R:258,2,257,256", DOS_STATUS_OK},
    {"U1:2,0,1,0", DOS_STATUS_OK},
    {"UA:2,0,1,0", DOS_STATUS_OK},
    {"U!:2,0,1,0", DOS_STATUS_OK},
    {"UQ:2,0,1,0", DOS_STATUS_OK},
    {"Ua:2,0,1,0", DOS_STATUS_OK},
    {"U1 2,0,1,0", DOS_STATUS_OK},
    {"B-R:2,0,0,0", DOS_STATUS_ILLEGAL_TRACK_SECTOR},
    {"B-R:2,0,255,255", DOS_STATUS_ILLEGAL_TRACK_SECTOR},
    {"B-R:9,0,0,0", DOS_STATUS_NO_CHANNEL},
    {"B-R:14,0,1,0", DOS_STATUS_NO_CHANNEL},
    {"B-R:1,0,1,0", DOS_STATUS_NO_CHANNEL},
    {"B-R:2,0,1,255", DOS_STATUS_ILLEGAL_TRACK_SECTOR},
    {"B-R:2,0,1,0,0", DOS_STATUS_SYNTAX},
    {"U1:2,0,1,0,0", DOS_STATUS_SYNTAX},
    {"B-R:2,0,A,0", DOS_STATUS_ILLEGAL_TRACK_SECTOR},
    {"U3:2,0,1,0", DOS_STATUS_SYNTAX},
    {"B-E:2,0,1,0", DOS_STATUS_SYNTAX},
    {"B-R:2", DOS_STATUS_ILLEGAL_TRACK_SECTOR},
    {"B-R:2,0,1", DOS_STATUS_OK},
    {"U1:2,0,1,0\r", DOS_STATUS_OK},
};

// Sets independent native map versions, not the DOS 2.7 directory-header version.
static void set_map_version(image_t* const image, const diskimage_type_t type, const unsigned int format,
                            const uint8_t version) {
    const unsigned int parts = format == 0 ? 1u : format == 1 ? 2u : 4u;
    for (unsigned int part = 0; part < parts; ++part) {
        sector_bytes(image, type, format == 0 ? 18u : 38u, part * 3u)[2] = version;
    }
    if (format != 0) {
        sector_bytes(image, type, 39, 0)[2] = 0xff;
    }
}

// INITDR frees only selected-disk channels, without closing unfinished files.
START_TEST(test_initialize_abandons_only_selected_disk_and_reloads_metadata) {
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
    put_bytes(&unit, &bank, 2, 0x20, 4);
    ck_assert(dos_close(&unit, &bank, model, 2));
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "1:SECOND,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x60, 4);
    ck_assert(dos_close(&unit, &bank, model, 2));
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:FIRST,S,R"), DOS_UNUSED);
    const uint8_t peer = open_name(&unit, &bank, 3, "1:SECOND,S,R");
    ck_assert_uint_ne(peer, DOS_UNUSED);
    dos_get(&unit, &bank, peer, 1);
    ck_assert_uint_ne(open_name(&unit, &bank, 4, "0:ABANDON,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 4, 0x20, 6);
    const uint8_t writer = open_name(&unit, &bank, 5, "1:KEEP,S,W");
    ck_assert_uint_ne(writer, DOS_UNUSED);
    put_bytes(&unit, &bank, 5, 0x60, 6);
    const dos_channel_t peer_before = unit.channels[peer], writer_before = unit.channels[writer];
    const dos_bam_t bam_before = unit.bam[1];
    const unsigned int reads = first.reads, writes = first.writes, flushes = first.flushes;
    send_command(&unit, &bank, "I0");
    ck_assert(dos_work_pending(&unit));
    ck_assert_uint_eq(first.reads, reads);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), DOS_UNUSED);
    ck_assert_uint_eq(dos_find_channel(&unit, 4), DOS_UNUSED);
    ck_assert_uint_eq(first.writes, writes);
    ck_assert_uint_eq(first.flushes, flushes);
    ck_assert_mem_eq(&unit.channels[peer], &peer_before, sizeof(peer_before));
    ck_assert_mem_eq(&unit.channels[writer], &writer_before, sizeof(writer_before));
    ck_assert_mem_eq(&unit.bam[1], &bam_before, sizeof(bam_before));
    ck_assert(unit.free_blocks_valid[0]);
    ck_assert_uint_eq(unit.free_blocks[0], format == 0 ? 663 : format == 1 ? 2051 : 4132);
    ck_assert_uint_eq(unit.last_drive, 0);
    ck_assert_uint_eq(unit.selected_drive, 0);

    // Initializing the peer also abandons it, and bare I starts at LSTDRV.
    send_command(&unit, &bank, "I1");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.last_drive, 1);
    ck_assert_uint_eq(unit.buffer_use, 0);
    send_command(&unit, &bank, "I");
    const unsigned int first_reads = first.reads, second_reads = second.reads;
    dos_work(&unit, &bank);
    ck_assert_uint_eq(unit.selected_drive, 1);
    ck_assert_uint_eq(first.reads, first_reads);
    ck_assert_uint_gt(second.reads, second_reads);
    finish_open(&unit, &bank);
    ck_assert_str_eq(unit.status, format == 0 ? "00, OK,00,00\r" : "00, OK,00,00,1\r");
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// SIMPRS accepts native abbreviations/suffixes instead of a strict I0/I1 grammar.
START_TEST(test_initialize_native_drive_selection) {
    static const struct {
        const char* text;
        uint8_t first;
        bool both;
    } cases[] = {
        {"I0", 0, false},          {"I1", 1, false},          {"I", 1, true},
        {"INITIALIZE0", 0, false}, {"INITIALIZE1", 1, false}, {"I0:IGNORED1", 0, false},
        {"IX:0", 0, false},        {"IX1:ANY0", 1, false},    {"I1JUNK", 1, true},
    };
    const unsigned int format = (unsigned int)_i / count_of(cases);
    const unsigned int which = (unsigned int)_i % count_of(cases);
    image_t first, second;
    dos_media_bank_t bank = {0}, other = {0};
    mount_blank(&bank, &first, format);
    mount_blank(&other, &second, format);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    unit.last_drive = 1;
    send_command(&unit, &bank, cases[which].text);
    ck_assert(dos_work_pending(&unit));
    ck_assert_uint_eq(unit.initialize.first_drive, cases[which].first);
    ck_assert_uint_eq(unit.initialize.slots, cases[which].both ? 3 : 1u << cases[which].first);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.selected_drive, cases[which].first);
    ck_assert_int_eq(first.reads != 0, cases[which].both || cases[which].first == 0);
    ck_assert_int_eq(second.reads != 0, cases[which].both || cases[which].first == 1);
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// A failed reload leaves no partly resident metadata and admits a fresh retry.
START_TEST(test_initialize_storage_failures_and_absent_slot_do_not_redirect) {
    const unsigned int format = (unsigned int)_i / 2u, phase = (unsigned int)_i % 2u;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    send_command(&unit, &bank, "I1");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NOT_READY);
    ck_assert_uint_eq(unit.selected_drive, 1);
    ck_assert_uint_eq(unit.last_drive, 0);
    ck_assert_uint_eq(image.reads, 0);
    image.fail_read = 1u + (phase != 0 && format != 0);
    send_command(&unit, &bank, "I0");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert(!unit.free_blocks_valid[0]);
    ck_assert(!unit.bam[0].buffer.resident);
    ck_assert_uint_eq(image.writes + image.flushes, 0);
    image.fail_read = 0;
    send_command(&unit, &bank, "I0");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert(unit.free_blocks_valid[0]);
    free(image.bytes);
}
END_TEST

// A captured INITIALIZE cannot reload a replacement binding through its old job.
START_TEST(test_initialize_rejects_replaced_media_before_io) {
    image_t old, replacement;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &old, (unsigned int)_i);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, "I0");
    mount_blank(&bank, &replacement, (unsigned int)_i);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_uint_eq(old.reads + replacement.reads, 0);
    ck_assert(!unit.free_blocks_valid[0]);
    free(old.bytes);
    free(replacement.bytes);
}
END_TEST

// CMDSET checks the trimmed length, while unsupported execution has no side effects.
START_TEST(test_command_termination_unknown_and_execution_rejection) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, (unsigned int)_i);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    static const char* const rejected[] = {
        "M-R", "M-W", "M-E", "B-E", "U3", "UC", "U8", "UH", "U?",
    };
    for (unsigned int index = 0; index < count_of(rejected); ++index) {
        send_command(&unit, &bank, rejected[index]);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_SYNTAX);
    }
    send_command(&unit, &bank, "?");
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_INVALID_COMMAND);
    ck_assert_uint_eq(image.reads + image.writes + image.flushes, 0);
    char request[DOS_COMMAND_RECEIVE_CAPACITY + 1u];
    memset(request, 'X', sizeof(request));
    request[0] = 'U';
    request[1] = '0';
    request[DOS_COMMAND_LIMIT] = '\r';
    request[DOS_COMMAND_RECEIVE_CAPACITY] = 0;
    send_command(&unit, &bank, request);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    request[DOS_COMMAND_LIMIT] = 'X';
    send_command(&unit, &bank, request);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_COMMAND_TOO_LONG);
    free(image.bytes);
}
END_TEST

// ATNIRQ is a transport re-entry, preserving channels, maps and non-returning command bytes.
START_TEST(test_user_interrupt_retains_channels_maps_and_command_bytes) {
    static const char* const aliases[] = {"U;", "UK", "U[", "Uk", "U+", "U\v"};
    const unsigned int format = (unsigned int)_i / count_of(aliases);
    dos_media_bank_t bank = {0}, other = {0};
    image_t images[2];
    mount_blank(&bank, &images[0], format);
    mount_blank(&other, &images[1], format);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    send_command(&unit, &bank, "I0");
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "I1");
    finish_open(&unit, &bank);
    ck_assert_uint_ne(open_name(&unit, &bank, 3, "0:READ,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 3, 0x40, 4);
    ck_assert(dos_close(&unit, &bank, model, 3));
    ck_assert_uint_ne(open_name(&unit, &bank, 3, "0:READ,S,R"), DOS_UNUSED);
    ck_assert_uint_ne(open_name(&unit, &bank, 4, "1:WRITE,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 4, 0x50, 4);
    const uint8_t direct = open_name(&unit, &bank, 2, "#");
    ck_assert_uint_ne(direct, DOS_UNUSED);
    memcpy(unit.buffers[unit.channels[direct].buffers[0]].bytes, "IRQ!", 4);
    send_command(&unit, &bank, "B-P:2,0");
    unit.selected_drive = 0;
    unit.last_drive = unit.last_program_drive = 1;
    unit.last_program_track = 1;
    unit.last_program_sector = 3;
    send_command(&unit, &bank, "?");
    const dos_unit_t before = unit;
    const dos_media_bank_t bindings = bank;
    const unsigned int reads = images[0].reads + images[1].reads;
    const unsigned int writes = images[0].writes + images[1].writes;
    const unsigned int flushes = images[0].flushes + images[1].flushes;
    for (unsigned int repeat = 0; repeat < 2; ++repeat) {
        send_command(&unit, &bank, repeat == 0 ? aliases[(unsigned int)_i % count_of(aliases)] : "U");
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        ck_assert(!dos_work_pending(&unit));
        ck_assert_mem_eq(unit.channels, before.channels, sizeof(unit.channels));
        ck_assert_mem_eq(unit.buffers, before.buffers, sizeof(unit.buffers));
        ck_assert_mem_eq(unit.bam, before.bam, sizeof(unit.bam));
        ck_assert_mem_eq(unit.free_blocks, before.free_blocks, sizeof(unit.free_blocks));
        ck_assert_mem_eq(unit.free_blocks_valid, before.free_blocks_valid, sizeof(unit.free_blocks_valid));
        ck_assert_uint_eq(unit.buffer_use, before.buffer_use);
        ck_assert_uint_eq(unit.buffer_drives, before.buffer_drives);
        ck_assert_uint_eq(unit.selected_drive, before.selected_drive);
        ck_assert_uint_eq(unit.last_drive, before.last_drive);
        ck_assert_uint_eq(unit.last_program_drive, before.last_program_drive);
        ck_assert_uint_eq(unit.last_program_track, before.last_program_track);
        ck_assert_uint_eq(unit.last_program_sector, before.last_program_sector);
        ck_assert_mem_eq(&bank, &bindings, sizeof(bank));
        ck_assert_uint_eq(images[0].reads + images[1].reads, reads);
        ck_assert_uint_eq(images[0].writes + images[1].writes, writes);
        ck_assert_uint_eq(images[0].flushes + images[1].flushes, flushes);
    }
    expect_direct(&unit, &bank, direct, (const uint8_t*)"IRQ!", 4, false);
    free(images[0].bytes);
    free(images[1].bytes);
}
END_TEST

// CMDSET's length rejection still precedes ATNIRQ, including its trimmed boundary.
START_TEST(test_user_interrupt_overlong_and_trimmed_request) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, (unsigned int)_i);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "#"), DOS_UNUSED);
    const unsigned int reads = image.reads, writes = image.writes, flushes = image.flushes;
    char request[DOS_COMMAND_RECEIVE_CAPACITY + 1u];
    memset(request, 'X', sizeof(request));
    request[0] = 'U';
    request[1] = 'K';
    request[DOS_COMMAND_RECEIVE_CAPACITY] = 0;
    send_command(&unit, &bank, request);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_COMMAND_TOO_LONG);
    ck_assert_uint_ne(dos_find_channel(&unit, 2), DOS_UNUSED);
    request[DOS_COMMAND_LIMIT] = '\r';
    send_command(&unit, &bank, request);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_ne(dos_find_channel(&unit, 2), DOS_UNUSED);
    send_command(&unit, &bank, "U");
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(image.flushes, flushes);
    free(image.bytes);
}
END_TEST

// Warm vectors release channels without clearing data RAM or finalizing either drive.
START_TEST(test_warm_reset_retains_ram_and_model_specific_maps) {
    static const char* const aliases[] = {"U9", "UI", "UY", "Ui", "U)", "U\t"};
    const unsigned int format = (unsigned int)_i / count_of(aliases);
    dos_media_bank_t bank = {0}, other = {0};
    image_t images[2];
    mount_blank(&bank, &images[0], format);
    mount_blank(&other, &images[1], format);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    send_command(&unit, &bank, "I0");
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "I1");
    finish_open(&unit, &bank);
    ck_assert_uint_ne(open_name(&unit, &bank, 3, "1:ABORT,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 3, 0x40, 4);
    const uint8_t channel = open_name(&unit, &bank, 2, "#");
    const uint8_t buffer = unit.channels[channel].buffers[0];
    memcpy(unit.buffers[buffer].bytes, "WARM", 4);
    unit.last_program_track = 1;
    unit.last_program_sector = 3;
    unit.last_program_drive = 1;
    const uint8_t selected = unit.selected_drive, last = unit.last_drive;
    const uint16_t drives = unit.buffer_drives;
    dos_bam_t maps[DOS_MEDIA_SLOTS_PER_UNIT] = {unit.bam[0], unit.bam[1]};
    for (unsigned int slot = 0; slot < DOS_MEDIA_SLOTS_PER_UNIT; ++slot) {
        maps[slot].reinitialize = format != 0;
    }
    const dos_media_bank_t bindings = bank;
    const unsigned int reads = images[0].reads + images[1].reads;
    const unsigned int writes = images[0].writes + images[1].writes;
    const unsigned int flushes = images[0].flushes + images[1].flushes;
    send_command(&unit, &bank, aliases[(unsigned int)_i % count_of(aliases)]);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_POWER_ON);
    ck_assert(!dos_work_pending(&unit));
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(unit.directory_internal, 0);
    for (unsigned int index = 0; index < DOS_FILE_CHANNEL_COUNT; ++index) {
        ck_assert(!unit.channels[index].allocated);
    }
    ck_assert_mem_eq(unit.buffers[buffer].bytes, "WARM", 4);
    ck_assert_uint_eq(unit.buffer_drives, drives);
    ck_assert_uint_eq(unit.selected_drive, selected);
    ck_assert_uint_eq(unit.last_drive, last);
    ck_assert_uint_eq(unit.last_program_track, 1);
    ck_assert_uint_eq(unit.last_program_sector, 3);
    ck_assert_uint_eq(unit.last_program_drive, 1);
    ck_assert_mem_eq(&bank, &bindings, sizeof(bank));
    ck_assert_mem_eq(unit.bam, maps, sizeof(maps));
    ck_assert_uint_eq(images[0].reads + images[1].reads, reads);
    ck_assert_uint_eq(images[0].writes + images[1].writes, writes);
    ck_assert_uint_eq(images[0].flushes + images[1].flushes, flushes);
    // The restarted command pointer does not erase the selector left in RAM.
    send_command(&unit, &bank, "U");
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_POWER_ON);
    char name[8];
    snprintf(name, sizeof(name), "#%u", buffer);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, name), DOS_UNUSED);
    send_command(&unit, &bank, "B-P:2,0");
    expect_direct(&unit, &bank, dos_find_channel(&unit, 2), (const uint8_t*)"WARM", 4, false);
    ck_assert_int_eq(unit.bam[0].reinitialize, format != 0);
    ck_assert(!unit.bam[1].reinitialize);
    free(images[0].bytes);
    free(images[1].bytes);
}
END_TEST

// DOS 2.7 delays its warm metadata reload until a real AUTOI caller runs.
START_TEST(test_warm_reset_auto_initialize_entry_points) {
    const unsigned int format = (unsigned int)_i / 6u, operation = (unsigned int)_i % 6u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, "I0");
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "B-A:0,1,0");
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "UI");
    if (operation == 1) {
        ck_assert_uint_ne(open_name(&unit, &bank, 2, "#"), DOS_UNUSED);
    } else if (operation == 2) {
        ck_assert_uint_eq(open_name(&unit, &bank, 3, "0:ABSENT,S,R"), DOS_UNUSED);
    } else if (operation == 5) {
        const uint8_t listing = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, "$0");
        ck_assert_uint_ne(listing, DOS_UNUSED);
        uint8_t bytes[128];
        ck_assert_uint_gt(directory_bytes(&unit, &bank, listing, bytes, sizeof(bytes)), 0);
    } else if (operation >= 3) {
        static const char* const commands[] = {"R0:NEW=ABSENT", "S0:ABSENT"};
        send_command(&unit, &bank, commands[operation - 3u]);
        finish_open(&unit, &bank);
    }
    send_command(&unit, &bank, "B-A:0,1,0");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, format != 0 && operation != 0 ? DOS_STATUS_OK : DOS_STATUS_NO_BLOCK);
    ck_assert_uint_eq(image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

// Each formatted drive header consumes only that drive's deferred warm reload.
START_TEST(test_warm_directory_reload_per_drive_and_raw_exemption) {
    const unsigned int format = (unsigned int)_i / 3u, variant = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0};
    image_t images[2];
    mount_blank(&bank, &images[0], format);
    mount_peer(&bank, &images[1], format);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    uint8_t bytes[256];
    const uint8_t initial = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, "$0:*,1:*");
    const unsigned int length = directory_bytes(&unit, &bank, initial, bytes, sizeof(bytes));
    ck_assert_uint_eq(length, 128);
    const unsigned int totals[2] = {bytes[34] | ((unsigned int)bytes[35] << 8u),
                                    bytes[98] | ((unsigned int)bytes[99] << 8u)};
    ck_assert(dos_close(&unit, &bank, model, IEEE_LOAD_CHANNEL));
    send_command(&unit, &bank, "B-A:0,1,0");
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "B-A:1,1,0");
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "UI");

    // Raw directory OPEN does not enter NEWDIR/AUTOI.
    if (variant == 2) {
        ck_assert_uint_ne(open_name(&unit, &bank, 2, "$0"), DOS_UNUSED);
        ck_assert_int_eq(unit.bam[0].reinitialize, format != 0);
        ck_assert_int_eq(unit.bam[1].reinitialize, format != 0);
        ck_assert(unit.bam[0].dirty);
        ck_assert(unit.bam[1].dirty);
        ck_assert(dos_close(&unit, &bank, model, 2));
    }

    // Admission initializes the first header even before the listing is read.
    const uint8_t listing = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, variant == 1 ? "$1:*,0:*" : "$0:*,1:*");
    const unsigned int first = variant == 1 ? 1u : 0u;
    ck_assert_uint_ne(listing, DOS_UNUSED);
    ck_assert(!unit.bam[first].reinitialize);
    ck_assert_int_eq(unit.bam[first ^ 1u].reinitialize, format != 0);
    ck_assert_uint_eq(directory_bytes(&unit, &bank, listing, bytes, sizeof(bytes)), length);
    ck_assert_uint_eq(bytes[4], first);
    ck_assert_uint_eq(bytes[66], first ^ 1u);
    for (unsigned int row = 0; row < 2; ++row) {
        const unsigned int offset = row * 64u + 34u, drive = first ^ row;
        const unsigned int total = bytes[offset] | ((unsigned int)bytes[offset + 1u] << 8u);
        ck_assert_uint_eq(total, totals[drive] - (format == 0 ? 1u : 0u));
        ck_assert(!unit.bam[drive].reinitialize);
    }
    ck_assert_uint_eq(images[0].writes + images[1].writes + images[0].flushes + images[1].flushes, 0);
    free(images[0].bytes);
    free(images[1].bytes);
}
END_TEST

// Overlong requests cannot execute a warm vector or release existing ownership.
START_TEST(test_warm_reset_overlong_request) {
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, (unsigned int)_i);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, 2, "#");
    const dos_channel_t before = unit.channels[channel];
    char command[DOS_COMMAND_RECEIVE_CAPACITY + 1u];
    memset(command, 'X', sizeof(command));
    command[0] = 'U';
    command[1] = 'I';
    command[sizeof(command) - 1u] = 0;
    send_command(&unit, &bank, command);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_COMMAND_TOO_LONG);
    ck_assert_mem_eq(&unit.channels[channel], &before, sizeof(before));
    ck_assert_uint_eq(image.reads + image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

// Native block reads use byte zero as a count, while U1 exposes every raw byte.
START_TEST(test_block_reads_counted_and_raw_boundaries) {
    static const uint8_t counts[] = {0, 1, 2, 4, 254, 255};
    const unsigned int format = (unsigned int)_i / (2u * count_of(counts));
    const bool raw = _i % 2 != 0;
    const uint8_t count = counts[((unsigned int)_i / 2u) % count_of(counts)];
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    uint8_t* const page = sector_bytes(&image, bank.units[0].slots[0].io.image.type, 1, 0);
    for (unsigned int offset = 0; offset < DISKIMAGE_SECTOR_SIZE; ++offset) {
        page[offset] = offset ^ 0x5au;
    }
    page[0] = count;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, 2, "#");
    const uint8_t peer = open_name(&unit, &bank, 3, "#");
    const dos_channel_t saved_peer = unit.channels[peer];
    send_command(&unit, &bank, "B-P:2,255");
    send_command(&unit, &bank, "B-P:2,13");
    const dos_channel_t before = unit.channels[channel];
    const uint16_t owned = unit.buffer_use;
    const unsigned int reads = image.reads;
    send_command(&unit, &bank, raw ? "U1:2,0,1,0" : "B-R:2,0,1,0");
    ck_assert_int_eq(unit.command, DOS_COMMAND_BLOCK_READ);
    ck_assert_mem_eq(&unit.channels[channel], &before, sizeof(before));
    ck_assert_uint_eq(image.reads, reads);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(image.reads, reads + 1u);
    ck_assert_uint_eq(unit.buffer_use, owned);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    uint8_t expected[DISKIMAGE_SECTOR_SIZE];
    const unsigned int length = raw || count == 0 ? DISKIMAGE_SECTOR_SIZE : count;
    for (unsigned int offset = 0; offset < length; ++offset) {
        expected[offset] = page[(offset + !raw) % DISKIMAGE_SECTOR_SIZE];
    }
    expect_direct(&unit, &bank, channel, expected, length, true);
    const unsigned int repeated = raw ? DISKIMAGE_SECTOR_SIZE - 1u : length;
    for (unsigned int offset = 0; offset < repeated; ++offset) {
        expected[offset] = page[(offset + 1u) % DISKIMAGE_SECTOR_SIZE];
    }
    expect_direct(&unit, &bank, channel, expected, repeated, true);
    char pointer[24];
    snprintf(pointer, sizeof(pointer), "B-P:2,%u", unit.channels[channel].direct_last);
    send_command(&unit, &bank, pointer);
    const uint8_t last = page[raw ? DISKIMAGE_SECTOR_SIZE - 1u : count];
    expect_direct(&unit, &bank, channel, &last, 1, true);
    if (!raw && count == 4) {
        send_command(&unit, &bank, "B-P:2,3");
        put_bytes(&unit, &bank, 2, 0x69, 1);
        const uint8_t fourth = page[4];
        expect_direct(&unit, &bank, channel, &fourth, 1, true);
    }
    ck_assert_uint_eq(image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

// BUFTST precedes TSCHK, numeric fields use low bytes, and user vectors fold by nibble.
START_TEST(test_block_read_parser_geometry_and_user_aliases) {
    const unsigned int format = (unsigned int)_i / count_of(block_read_commands);
    const unsigned int variant = (unsigned int)_i % count_of(block_read_commands);
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, 2, "#");
    const dos_channel_t before = unit.channels[channel];
    send_command(&unit, &bank, block_read_commands[variant].command);
    if (block_read_commands[variant].status == DOS_STATUS_OK) {
        ck_assert(dos_work_pending(&unit));
        ck_assert_uint_eq(image.reads, 0);
        finish_open(&unit, &bank);
        ck_assert_uint_eq(image.reads, 1);
    } else {
        ck_assert(!dos_work_pending(&unit));
        ck_assert_uint_eq(image.reads, 0);
        ck_assert_mem_eq(&unit.channels[channel], &before, sizeof(before));
    }
    ck_assert_uint_eq(unit.status_code, block_read_commands[variant].status);
    if (strcmp(block_read_commands[variant].command, "B-R:2,0,255,255") == 0) {
        ck_assert_str_eq(unit.status,
                         format == 0 ? "66,ILLEGAL TRACK OR SECTOR,55,55\r" : "66,ILLEGAL TRACK OR SECTOR,255,255,0\r");
    }
    ck_assert_uint_eq(image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

// Pending jobs cannot read a replacement, and failed jobs preserve prepared data for retry.
START_TEST(test_block_read_failures_and_stale_identity) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int variant = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    const dos_media_io_t original = bank.units[0].slots[0].io;
    uint8_t* const page = sector_bytes(&image, original.image.type, 1, 0);
    page[0] = 4;
    memset(page + 1, 0x69, 4);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, 2, "#");
    const uint8_t peer = open_name(&unit, &bank, 3, "#");
    const dos_channel_t before = unit.channels[channel], saved_peer = unit.channels[peer];
    send_command(&unit, &bank, "B-R:2,0,1,0");
    if (variant == 0) {
        image.fail_read = image.reads + 1;
    } else {
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, variant == 1 ? &original : NULL), DOS_MEDIA_OK);
    }
    const unsigned int reads = image.reads;
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_uint_eq(image.reads, reads + (variant == 0));
    ck_assert_mem_eq(&unit.channels[channel], &before, sizeof(before));
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    image.fail_read = 0;
    if (variant != 0) {
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, &original), DOS_MEDIA_OK);
    }
    send_command(&unit, &bank, "B-R:2,0,1,0");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    const uint8_t expected[] = {0x69, 0x69, 0x69, 0x69};
    expect_direct(&unit, &bank, channel, expected, sizeof(expected), true);
    ck_assert_uint_eq(image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

// The job's drive bit overrides allocation affinity, even on read-only last-track media.
START_TEST(test_block_read_drive_affinity_and_readonly_last_track) {
    const unsigned int format = (unsigned int)_i / 2u, drive = (unsigned int)_i % 2u;
    dos_media_bank_t bank = {0};
    image_t images[2];
    mount_blank(&bank, &images[0], format);
    images[1] = (image_t){.bytes = malloc(images[0].size), .size = images[0].size};
    ck_assert_ptr_nonnull(images[1].bytes);
    memcpy(images[1].bytes, images[0].bytes, images[0].size);
    dos_media_io_t second = bank.units[0].slots[0].io;
    second.image.ctx = &images[1];
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &second), DOS_MEDIA_OK);
    dos_media_io_t target = bank.units[0].slots[drive].io;
    target.image.write = NULL;
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, drive, &target), DOS_MEDIA_OK);
    const unsigned int track = format == 0 ? 35u : format == 1 ? 77u : 154u;
    const unsigned int sector = format == 0 ? 16u : 22u;
    uint8_t* const page = sector_bytes(&images[drive], target.image.type, track, sector);
    memset(page, 0x69, DISKIMAGE_SECTOR_SIZE);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    unit.selected_drive = unit.last_drive = drive ^ 1u;
    const uint8_t channel = open_name(&unit, &bank, 2, "#");
    const uint8_t peer = open_name(&unit, &bank, 3, "#");
    const dos_channel_t saved_peer = unit.channels[peer];
    char command[32];
    snprintf(command, sizeof(command), "UA:2,%u,%u,%u", drive | 2u, track, sector);
    send_command(&unit, &bank, command);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.channels[channel].media.slot, drive);
    ck_assert_uint_eq((unit.buffer_drives >> unit.channels[channel].buffers[0]) & 1u, drive);
    ck_assert_uint_eq(unit.last_drive, drive);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    const uint8_t expected = 0x69;
    expect_direct(&unit, &bank, channel, &expected, 1, false);
    ck_assert_uint_eq(images[drive].reads, 1);
    ck_assert_uint_eq(images[drive ^ 1u].reads, 0);
    snprintf(command, sizeof(command), "I%u", drive);
    send_command(&unit, &bank, command);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), DOS_UNUSED);
    ck_assert_uint_eq(dos_find_channel(&unit, 3), peer);
    ck_assert_uint_eq(images[0].writes + images[1].writes + images[0].flushes + images[1].flushes, 0);
    free(images[0].bytes);
    free(images[1].bytes);
}
END_TEST

// Counted writes clamp short pointers, preserve LSTCHR/EOI, and write every raw byte.
START_TEST(test_block_write_pointer_count_and_raw_state) {
    static const uint8_t positions[] = {0, 1, 2, 5, 254, 255};
    const unsigned int format = (unsigned int)_i / (2u * 3u * count_of(positions));
    const unsigned int variant = ((unsigned int)_i / count_of(positions)) % 3u;
    const bool raw = ((unsigned int)_i / (3u * count_of(positions))) % 2u != 0;
    const uint8_t position = positions[(unsigned int)_i % count_of(positions)];
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, 2, "#");
    const uint8_t peer = open_name(&unit, &bank, 3, "#");
    const dos_channel_t saved_peer = unit.channels[peer];
    dos_channel_t* const state = &unit.channels[channel];
    uint8_t* const page = unit.buffers[state->buffers[state->active]].bytes;
    uint8_t expected[DISKIMAGE_SECTOR_SIZE];
    for (unsigned int offset = 0; offset < DISKIMAGE_SECTOR_SIZE; ++offset) {
        expected[offset] = page[offset] = offset ^ 0x40u;
    }
    state->direct_last = variant == 1 ? 1 : 255;
    state->direct_eoi = variant == 2;
    state->position = position;
    state->direct_byte = page[position];
    const dos_channel_t before = *state;
    const uint16_t owned = unit.buffer_use;
    send_command(&unit, &bank, raw ? "U2:2,0,1,0" : "B-W:2,0,1,0");
    ck_assert_int_eq(unit.command, DOS_COMMAND_BLOCK_WRITE);
    ck_assert_uint_eq(image.writes + image.flushes + image.reads, 0);
    ck_assert_mem_eq(state, &before, sizeof(before));
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    if (!raw) {
        expected[0] = position < 2 ? 1 : position - 1u;
    }
    ck_assert_mem_eq(sector_bytes(&image, bank.units[0].slots[0].io.image.type, 1, 0), expected, sizeof(expected));
    ck_assert_mem_eq(page, expected, sizeof(expected));
    ck_assert_uint_eq(state->position, raw ? position : 1);
    ck_assert_uint_eq(state->direct_byte, raw ? before.direct_byte : expected[1]);
    ck_assert_uint_eq(state->direct_last, before.direct_last);
    ck_assert_int_eq(state->direct_eoi, before.direct_eoi || (!raw && before.direct_last == 1));
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    ck_assert_uint_eq(unit.buffer_use, owned);
    ck_assert_uint_eq(image.writes, 1);
    ck_assert_uint_eq(image.flushes, 1);
    ck_assert_uint_eq(image.reads, format == 0 ? 1 : format == 1 ? 2 : 4);
    free(image.bytes);
}
END_TEST

// Failed writes retain their RAM page and can be retried raw without recomputing a count.
START_TEST(test_block_write_failures_and_stale_identity) {
    const unsigned int format = (unsigned int)_i / 5u, variant = (unsigned int)_i % 5u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    const dos_media_io_t original = bank.units[0].slots[0].io;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, 2, "#");
    const uint8_t peer = open_name(&unit, &bank, 3, "#");
    const dos_channel_t saved_peer = unit.channels[peer];
    dos_channel_t* const state = &unit.channels[channel];
    uint8_t* const page = unit.buffers[state->buffers[state->active]].bytes;
    memset(page, 0x69, DISKIMAGE_SECTOR_SIZE);
    state->position = 5;
    const uint8_t prepared = state->direct_byte;
    if (variant == 2) {
        dos_media_io_t readonly = original;
        readonly.image.write = NULL;
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, &readonly), DOS_MEDIA_OK);
    }
    send_command(&unit, &bank, "B-W:2,0,1,0");
    if (variant == 0) {
        image.fail_write = 1;
    } else if (variant == 1) {
        image.fail_flush = 1;
    } else if (variant >= 3) {
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, variant == 3 ? &original : NULL), DOS_MEDIA_OK);
    }
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, variant == 2 ? DOS_STATUS_WRITE_PROTECT : DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(image.writes, variant < 2 ? 1 : 0);
    ck_assert_uint_eq(image.flushes, variant == 1 ? 1 : 0);
    ck_assert_uint_eq(page[0], 4);
    ck_assert_uint_eq(state->position, 1);
    ck_assert_uint_eq(state->direct_byte, prepared);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    image.fail_write = image.fail_flush = 0;
    if (variant >= 2) {
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, &original), DOS_MEDIA_OK);
    }
    send_command(&unit, &bank, "UB:2,0,1,0");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_mem_eq(sector_bytes(&image, original.image.type, 1, 0), page, DISKIMAGE_SECTOR_SIZE);
    ck_assert_uint_eq(page[0], 4);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    free(image.bytes);
}
END_TEST

// Write decoding shares numeric limits, alias folding and channel-before-geometry precedence.
START_TEST(test_block_write_parser_and_aliases) {
    static const struct {
        const char* command;
        dos_status_code_t status;
    } cases[] = {
        {"B-W2,0,1,0", DOS_STATUS_OK},
        {"BLOCK-W:258,2,257,256", DOS_STATUS_OK},
        {"U2:2,0,1,0", DOS_STATUS_OK},
        {"UB:2,0,1,0", DOS_STATUS_OK},
        {"UR:2,0,1,0", DOS_STATUS_OK},
        {"Ub:2,0,1,0", DOS_STATUS_OK},
        {"U\":2,0,1,0", DOS_STATUS_OK},
        {"B-W:2,0,1,0,0", DOS_STATUS_SYNTAX},
        {"U2:2,0,1,0,0", DOS_STATUS_SYNTAX},
        {"B-W:2,0,A,0", DOS_STATUS_ILLEGAL_TRACK_SECTOR},
        {"B-W:9,0,0,0", DOS_STATUS_NO_CHANNEL},
        {"B-W:2,0,255,255", DOS_STATUS_ILLEGAL_TRACK_SECTOR},
        {"B-W:2", DOS_STATUS_ILLEGAL_TRACK_SECTOR},
        {"U2:2,0,1", DOS_STATUS_OK},
    };
    const unsigned int format = (unsigned int)_i / count_of(cases), variant = (unsigned int)_i % count_of(cases);
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, 2, "#");
    const dos_channel_t before = unit.channels[channel];
    send_command(&unit, &bank, cases[variant].command);
    ck_assert_uint_eq(image.writes, 0);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, cases[variant].status);
    ck_assert_uint_eq(image.writes, cases[variant].status == DOS_STATUS_OK);
    ck_assert_uint_eq(image.flushes, cases[variant].status == DOS_STATUS_OK);
    ck_assert_uint_eq(image.reads, cases[variant].status == DOS_STATUS_OK ? (format == 0   ? 1
                                                                             : format == 1 ? 2
                                                                                           : 4)
                                                                          : 0);
    if (cases[variant].status != DOS_STATUS_OK) {
        ck_assert_mem_eq(&unit.channels[channel], &before, sizeof(before));
    }
    free(image.bytes);
}
END_TEST

// SETJOB checks the selected drive's cached version after B-W mutates its page.
START_TEST(test_write_version_counted_raw_and_drive) {
    const unsigned int format = (unsigned int)_i / 16u, drive = ((unsigned int)_i / 8u) % 2u;
    const unsigned int version = (unsigned int)_i % 4u;
    const bool raw = ((unsigned int)_i / 4u) % 2u != 0;
    const bool accepted = version < 2;
    dos_media_bank_t bank = {0}, other = {0};
    image_t images[2];
    mount_blank(&bank, &images[0], format);
    mount_blank(&other, &images[1], format);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    const diskimage_type_t type = bank.units[0].slots[drive].io.image.type;
    set_map_version(&images[drive], type, format,
                    version == 0   ? (format == 0 ? 'A' : 'C')
                    : version == 1 ? 0
                    : version == 2 ? 0xff
                                   : (format == 0 ? 'C' : 'A'));
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    char command[32];
    snprintf(command, sizeof(command), "I%u", drive);
    send_command(&unit, &bank, command);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    const uint8_t channel = open_name(&unit, &bank, 2, "#");
    const uint8_t peer = open_name(&unit, &bank, 3, "#");
    const dos_channel_t saved_peer = unit.channels[peer];
    dos_channel_t* const state = &unit.channels[channel];
    uint8_t* const page = unit.buffers[state->buffers[state->active]].bytes;
    memset(page, 0x69, DISKIMAGE_SECTOR_SIZE);
    state->position = 5;
    state->direct_byte = 0x42;
    const dos_channel_t before = *state;
    uint8_t* const original = malloc(images[drive].size);
    ck_assert_ptr_nonnull(original);
    memcpy(original, images[drive].bytes, images[drive].size);
    const unsigned int reads = images[drive].reads;
    snprintf(command, sizeof(command), "%s:2,%u,1,1", raw ? "U2" : "B-W", drive | 2u);
    send_command(&unit, &bank, command);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, accepted ? DOS_STATUS_OK : DOS_STATUS_POWER_ON);
    ck_assert_uint_eq(images[drive].reads, reads);
    ck_assert_uint_eq(images[drive].writes, accepted ? 1 : 0);
    ck_assert_uint_eq(images[drive].flushes, accepted ? 1 : 0);
    ck_assert_uint_eq(images[drive ^ 1u].reads + images[drive ^ 1u].writes + images[drive ^ 1u].flushes, 0);
    ck_assert_uint_eq(page[0], raw ? 0x69 : 4);
    ck_assert_uint_eq(state->position, raw ? 5 : 1);
    ck_assert_uint_eq(state->direct_byte, accepted && !raw ? 0x69 : 0x42);
    ck_assert_uint_eq(state->direct_eoi, before.direct_eoi);
    ck_assert_uint_eq(state->direct_last, before.direct_last);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    if (accepted) {
        memcpy(original + DISKIMAGE_SECTOR_SIZE, page, DISKIMAGE_SECTOR_SIZE);
    } else {
        char expected[DOS_STATUS_LINE_CAPACITY];
        snprintf(expected, sizeof(expected), format == 0 ? "73,CBM DOS V2,01,01\r" : "73,CBM DOS V2.7,01,01,%u\r",
                 drive);
        ck_assert_str_eq(unit.status, expected);
    }
    ck_assert_mem_eq(images[drive].bytes, original, images[drive].size);
    free(original);
    free(images[0].bytes);
    free(images[1].bytes);
}
END_TEST

// Failed lazy map reads cannot bless a version, and successful cache replacements take effect.
START_TEST(test_write_version_read_retry_and_cached_part) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool read_failure = (unsigned int)_i % 2u != 0;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "#"), DOS_UNUSED);
    if (read_failure) {
        image.fail_read = 1;
    } else {
        send_command(&unit, &bank, "I0");
        finish_open(&unit, &bank);
        ck_assert_uint_ne(open_name(&unit, &bank, 2, "#"), DOS_UNUSED);
        set_map_version(&image, type, format, 0xff);
    }
    send_command(&unit, &bank, "U2:2,0,1,1");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, read_failure ? DOS_STATUS_READ_ERROR : DOS_STATUS_OK);
    ck_assert_uint_eq(image.writes, read_failure ? 0 : 1);
    image.fail_read = 0;
    if (read_failure) {
        send_command(&unit, &bank, "U2:2,0,1,1");
        finish_open(&unit, &bank);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    } else {
        send_command(&unit, &bank, "I0");
        finish_open(&unit, &bank);
        ck_assert_uint_ne(open_name(&unit, &bank, 2, "#"), DOS_UNUSED);
        send_command(&unit, &bank, "U2:2,0,1,1");
        finish_open(&unit, &bank);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_POWER_ON);
        ck_assert_uint_eq(image.writes, 1);
    }
    free(image.bytes);
}
END_TEST

// Shared SETJOB gating also protects directory writes and dirty direct CLOSE.
START_TEST(test_write_version_metadata_jobs) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool closing = (unsigned int)_i % 2u != 0;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:OLD,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x40, 4);
    ck_assert(dos_close(&unit, &bank, model, 2));
    set_map_version(&image, type, format, 0xff);
    send_command(&unit, &bank, "I0");
    finish_open(&unit, &bank);
    const unsigned int writes = image.writes, flushes = image.flushes;
    uint8_t* const original = malloc(image.size);
    ck_assert_ptr_nonnull(original);
    memcpy(original, image.bytes, image.size);
    if (closing) {
        const uint8_t channel = open_name(&unit, &bank, 2, "#");
        send_command(&unit, &bank, "B-A:0,1,0");
        finish_open(&unit, &bank);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        ck_assert(!dos_close(&unit, &bank, model, 2));
        ck_assert_uint_eq(dos_find_channel(&unit, 2), channel);
        ck_assert(unit.bam[0].dirty);
    } else {
        send_command(&unit, &bank, "R0:NEW=OLD");
        finish_open(&unit, &bank);
    }
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_POWER_ON);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(image.flushes, flushes);
    ck_assert_mem_eq(image.bytes, original, image.size);
    free(original);
    free(image.bytes);
}
END_TEST

// DOS 2.7 uses whichever map part is cached, not a permanently remembered version.
START_TEST(test_write_version_follows_cached_map_part) {
    const unsigned int format = (unsigned int)_i + 1u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    sector_bytes(&image, type, 38, 0)[2] = 0xff;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, "I0");
    finish_open(&unit, &bank);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "#"), DOS_UNUSED);
    send_command(&unit, &bank, "U2:2,0,1,1");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    send_command(&unit, &bank, "B-F:0,1,0");
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "U2:2,0,1,1");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_POWER_ON);
    send_command(&unit, &bank, format == 1 ? "B-F:0,77,0" : "B-F:0,154,0");
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "U2:2,0,1,1");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(image.writes, 2);
    free(image.bytes);
}
END_TEST

// DRTWRT's SETJOB version rejection precedes publishing the active page's LSTJOB.
START_TEST(test_rejected_block_write_preserves_drive_affinity) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool raw = (unsigned int)_i % 2u != 0;
    dos_media_bank_t bank = {0}, other = {0};
    image_t images[2];
    mount_blank(&bank, &images[0], format);
    mount_blank(&other, &images[1], format);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    const dos_model_t model = bank.units[0].model;
    set_map_version(&images[1], bank.units[0].slots[1].io.image.type, format, 0xff);
    dos_unit_t unit;
    dos_reset(&unit, model);
    send_command(&unit, &bank, "I0");
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "I1");
    finish_open(&unit, &bank);
    const uint8_t channel = open_name(&unit, &bank, 2, "#");
    send_command(&unit, &bank, "U1:2,0,1,0");
    finish_open(&unit, &bank);
    const dos_media_ref_t original = unit.channels[channel].media;
    const uint16_t drives = unit.buffer_drives;
    send_command(&unit, &bank, raw ? "U2:2,1,1,1" : "B-W:2,1,1,1");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_POWER_ON);
    ck_assert_mem_eq(&unit.channels[channel].media, &original, sizeof(original));
    ck_assert_uint_eq(unit.buffer_drives, drives);
    send_command(&unit, &bank, "I1");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), channel);
    ck_assert_uint_eq(images[0].writes + images[1].writes, 0);
    free(images[0].bytes);
    free(images[1].bytes);
}
END_TEST

// Each write uses the requested low drive bit and preserves the other page's affinity.
START_TEST(test_block_write_drive_affinity_and_last_track) {
    const unsigned int format = (unsigned int)_i / 4u, drive = ((unsigned int)_i / 2u) % 2u;
    dos_media_bank_t bank = {0};
    image_t images[2];
    mount_blank(&bank, &images[0], format);
    images[1] = (image_t){.bytes = malloc(images[0].size), .size = images[0].size};
    ck_assert_ptr_nonnull(images[1].bytes);
    memcpy(images[1].bytes, images[0].bytes, images[0].size);
    dos_media_io_t second = bank.units[0].slots[0].io;
    second.image.ctx = &images[1];
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &second), DOS_MEDIA_OK);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    unit.selected_drive = unit.last_drive = drive ^ 1u;
    const uint8_t channel = open_name(&unit, &bank, 2, "#");
    const uint8_t peer = open_name(&unit, &bank, 3, "#");
    const dos_channel_t saved_peer = unit.channels[peer];
    uint8_t* const page = unit.buffers[unit.channels[channel].buffers[0]].bytes;
    memset(page, 0x69, DISKIMAGE_SECTOR_SIZE);
    const unsigned int track = format == 0 ? 35u : format == 1 ? 77u : 154u;
    const unsigned int sector = format == 0 ? 16u : 22u;
    char command[32];
    snprintf(command, sizeof(command), "%s:2,%u,%u,%u", _i % 2 ? "UB" : "B-W", drive | 2u, track, sector);
    send_command(&unit, &bank, command);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(images[drive].writes, 1);
    ck_assert_uint_eq(images[drive ^ 1u].writes, 0);
    ck_assert_mem_eq(sector_bytes(&images[drive], second.image.type, track, sector), page, DISKIMAGE_SECTOR_SIZE);
    ck_assert_uint_eq(unit.channels[channel].media.slot, drive);
    ck_assert_uint_eq((unit.buffer_drives >> unit.channels[channel].buffers[0]) & 1u, drive);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    snprintf(command, sizeof(command), "I%u", drive);
    send_command(&unit, &bank, command);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), DOS_UNUSED);
    ck_assert_uint_eq(dos_find_channel(&unit, 3), peer);
    free(images[0].bytes);
    free(images[1].bytes);
}
END_TEST

// B-A/B-F need no channel, preserve peers and publish their dirty map only at CLOSE.
START_TEST(test_block_map_claim_free_and_close_persistence) {
    const unsigned int format = (unsigned int)_i / 2u, drive = (unsigned int)_i % 2u;
    dos_media_bank_t bank = {0};
    image_t images[2];
    mount_blank(&bank, &images[0], format);
    images[1] = (image_t){.bytes = malloc(images[0].size), .size = images[0].size};
    ck_assert_ptr_nonnull(images[1].bytes);
    memcpy(images[1].bytes, images[0].bytes, images[0].size);
    dos_media_io_t second = bank.units[0].slots[0].io;
    second.image.ctx = &images[1];
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &second), DOS_MEDIA_OK);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    unit.selected_drive = unit.last_drive = drive ^ 1u;
    const uint8_t peer = open_name(&unit, &bank, 3, "#");
    const dos_channel_t saved_peer = unit.channels[peer];
    const uint16_t owned = unit.buffer_use;
    static const char operations[] = {'F', 'A', 'A', 'A', 'F', 'F', 'A'};
    char command[32];
    for (unsigned int step = 0; step < count_of(operations); ++step) {
        snprintf(command, sizeof(command), "B-%c:%u,1,%u", operations[step], drive | 2u, step == 3 ? 1u : 0u);
        send_command(&unit, &bank, command);
        ck_assert_int_eq(unit.command, DOS_COMMAND_BLOCK_MAP);
        finish_open(&unit, &bank);
        ck_assert_uint_eq(unit.status_code, step == 2 ? DOS_STATUS_NO_BLOCK : DOS_STATUS_OK);
        if (step == 2) {
            ck_assert_str_eq(unit.status, format == 0  ? "65,NO BLOCK,01,01\r"
                                          : drive == 0 ? "65,NO BLOCK,01,01,0\r"
                                                       : "65,NO BLOCK,01,01,1\r");
        }
        ck_assert_uint_eq(unit.buffer_use, owned);
        ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
        ck_assert_uint_eq(images[0].writes + images[1].writes, 0);
    }
    image_t expected = {.bytes = malloc(images[drive].size), .size = images[drive].size};
    ck_assert_ptr_nonnull(expected.bytes);
    memcpy(expected.bytes, images[drive].bytes, expected.size);
    uint8_t* const entry = fixture_bam_entry(&expected, format, 1);
    entry[0] -= 2;
    entry[1] &= ~3u;
    unit.selected_drive = unit.last_drive = drive;
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "#"), DOS_UNUSED);
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    ck_assert_mem_eq(images[drive].bytes, expected.bytes, expected.size);
    ck_assert_uint_eq(images[drive].writes, 1);
    ck_assert_uint_eq(images[drive ^ 1u].writes, 0);
    ck_assert_uint_eq(dos_find_channel(&unit, 3), peer);
    free(expected.bytes);
    free(images[0].bytes);
    free(images[1].bytes);
}
END_TEST

// Successor search never wraps to lower sectors/tracks and never claims the suggestion.
START_TEST(test_block_map_successor_search_and_exhaustion) {
    const unsigned int format = (unsigned int)_i / 5u, variant = (unsigned int)_i % 5u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    unsigned int track = variant == 2   ? (format == 0   ? 35u
                                           : format == 1 ? 77u
                                                         : 154u)
                         : variant == 4 ? (format == 0 ? 17u : 50u)
                                        : 1u;
    unsigned int sectors;
    ck_assert(diskimage_track_sectors(bank.units[0].slots[0].io.image.type, track, &sectors));
    const unsigned int sector = variant == 0 ? 2u : sectors - 1u;
    uint8_t* const entry = fixture_bam_entry(&image, format, track);
    if (variant == 0) {
        entry[0] -= 3;
        entry[1] &= ~(7u << 2u);
    } else {
        --entry[0];
        entry[1u + sector / 8u] &= ~(1u << (sector % 8u));
    }
    if (variant == 3) {
        memset(entry, 0, format == 0 ? 4u : 5u);
        memset(fixture_bam_entry(&image, format, 2), 0, format == 0 ? 4u : 5u);
    }
    uint8_t* const original = malloc(image.size);
    ck_assert_ptr_nonnull(original);
    memcpy(original, image.bytes, image.size);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    char command[32];
    snprintf(command, sizeof(command), "B-A:0,%u,%u", track, sector);
    send_command(&unit, &bank, command);
    unsigned int steps = 0;
    while (dos_work_pending(&unit)) {
        ck_assert_uint_lt(steps++, 5);
        dos_work(&unit, &bank);
    }
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_BLOCK);
    const unsigned int next_track = variant == 2 ? 0u : variant == 0 ? 1u : variant == 3 ? 3u : track + 1u;
    const unsigned int next_sector = variant == 0 ? 5u : variant == 4 && format == 0 ? 2u : 0u;
    snprintf(command, sizeof(command), "65,NO BLOCK,%02u,%02u%s", next_track, next_sector, format == 0 ? "\r" : ",0\r");
    ck_assert_str_eq(unit.status, command);
    ck_assert_mem_eq(image.bytes, original, image.size);
    ck_assert(!unit.bam[0].dirty);
    ck_assert_uint_eq(image.writes, 0);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(original);
    free(image.bytes);
}
END_TEST

// Map parsing uses three low-byte values and never requires a direct channel.
START_TEST(test_block_map_parser_geometry_and_no_channel) {
    static const struct {
        const char* command;
        dos_status_code_t status;
    } cases[] = {
        {"B-A0,1,0", DOS_STATUS_OK},
        {"BLOCK-A:2,257,256", DOS_STATUS_OK},
        {"B-F0,1,0", DOS_STATUS_OK},
        {"BUFFER-F:2,257,256", DOS_STATUS_OK},
        {"B-A:0,1,0,9", DOS_STATUS_OK},
        {"B-F:0,1", DOS_STATUS_OK},
        {"B-A:0,1,0,0,0", DOS_STATUS_SYNTAX},
        {"B-F:0,1,0,0,0", DOS_STATUS_SYNTAX},
        {"B-A:0,A,0", DOS_STATUS_ILLEGAL_TRACK_SECTOR},
        {"B-A:0,0,0", DOS_STATUS_ILLEGAL_TRACK_SECTOR},
        {"B-F:0,255,255", DOS_STATUS_ILLEGAL_TRACK_SECTOR},
        {"B-A:0", DOS_STATUS_ILLEGAL_TRACK_SECTOR},
        {"B-F:0,1,255", DOS_STATUS_ILLEGAL_TRACK_SECTOR},
        {"B,F-:0,1,0", DOS_STATUS_SYNTAX},
    };
    const unsigned int format = (unsigned int)_i / count_of(cases), variant = (unsigned int)_i % count_of(cases);
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, cases[variant].command);
    ck_assert_uint_eq(image.reads + image.writes, 0);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, cases[variant].status);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(image.writes, 0);
    free(image.bytes);
}
END_TEST

// Map jobs reject stale media and retain dirty state and CLOSE ownership after host failures.
START_TEST(test_block_map_failures_and_readonly_close) {
    const unsigned int format = (unsigned int)_i / 6u, variant = (unsigned int)_i % 6u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    const dos_media_io_t original = bank.units[0].slots[0].io;
    if (variant == 3) {
        bank.units[0].slots[0].io.image.write = NULL;
    } else if (variant == 4) {
        --fixture_bam_entry(&image, format, 1)[0];
    }
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, "B-A:0,1,0");
    if (variant == 0) {
        image.fail_read = 1;
    } else if (variant == 1 || variant == 2) {
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, variant == 1 ? &original : NULL), DOS_MEDIA_OK);
    }
    finish_open(&unit, &bank);
    if (variant == 3 || variant == 5) {
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        ck_assert(unit.bam[0].dirty);
        const uint8_t channel = open_name(&unit, &bank, 2, "#");
        image.fail_flush = variant == 5;
        ck_assert(!dos_close(&unit, &bank, bank.units[0].model, 2));
        ck_assert_uint_eq(unit.status_code, variant == 3 ? DOS_STATUS_WRITE_PROTECT : DOS_STATUS_WRITE_ERROR);
        ck_assert_uint_eq(dos_find_channel(&unit, 2), channel);
        bank.units[0].slots[0].io.image.write = original.image.write;
        image.fail_flush = false;
        ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
        ck_assert_uint_eq(fixture_bam_entry(&image, format, 1)[1] & 1u, 0);
    } else {
        ck_assert_uint_eq(unit.status_code, variant == 4 ? DOS_STATUS_DIRECTORY_ERROR : DOS_STATUS_READ_ERROR);
        ck_assert(!unit.bam[0].dirty);
        ck_assert_uint_eq(image.reads, variant == 1 || variant == 2 ? 0u : 1u);
        ck_assert_uint_eq(image.writes, 0);
    }
    free(image.bytes);
}
END_TEST

// BLOCK/BLKPAR applies the bounded native separator and low-byte conversion rules.
START_TEST(test_block_pointer_parser_and_rejection_atomicity) {
    const unsigned int format = (unsigned int)_i / count_of(pointer_commands);
    const unsigned int variant = (unsigned int)_i % count_of(pointer_commands);
    dos_media_bank_t bank = {0};
    image_t image;
    mount_image(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, 2, "#");
    uint8_t* const bytes = unit.buffers[unit.channels[channel].buffers[0]].bytes;
    for (unsigned int offset = 0; offset < DISKIMAGE_SECTOR_SIZE; ++offset) {
        bytes[offset] = (uint8_t)(offset ^ 0xa5u);
    }
    const dos_channel_t saved = unit.channels[channel];
    send_command(&unit, &bank, pointer_commands[variant].command);
    ck_assert_uint_eq(unit.status_code, pointer_commands[variant].status);
    if (pointer_commands[variant].status == DOS_STATUS_OK) {
        const uint8_t position = pointer_commands[variant].position;
        ck_assert_uint_eq(unit.channels[channel].position, position);
        const uint8_t expected = bytes[position];
        expect_direct(&unit, &bank, channel, &expected, 1, position == DISKIMAGE_SECTOR_SIZE - 1u);
    } else {
        ck_assert_mem_eq(&unit.channels[channel], &saved, sizeof(saved));
    }
    ck_assert_uint_eq(unit.buffer_use, 1u << (DOS_BUFFER_COUNT - 1u));
    ck_assert_uint_eq(image.reads + image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

// BLKPTR refreshes the prepared byte, preserves sticky EOI and leaves peers untouched.
START_TEST(test_block_pointer_all_offsets_put_and_sticky_eoi) {
    dos_media_bank_t bank = {0};
    image_t image;
    mount_image(&bank, &image, (unsigned int)_i);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, 2, "#");
    const uint8_t peer = open_name(&unit, &bank, 3, "#");
    const dos_channel_t saved_peer = unit.channels[peer];
    send_command(&unit, &bank, "B-P:2,0");
    put_bytes(&unit, &bank, 2, 0, DISKIMAGE_SECTOR_SIZE);
    const uint8_t zero = 0;
    expect_direct(&unit, &bank, channel, &zero, 1, true);
    for (unsigned int offset = 0; offset < DISKIMAGE_SECTOR_SIZE; ++offset) {
        char command[24];
        snprintf(command, sizeof(command), "B-P:2,%u", offset);
        send_command(&unit, &bank, command);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        const uint8_t expected = offset;
        expect_direct(&unit, &bank, channel, &expected, 1, offset + 1u == DISKIMAGE_SECTOR_SIZE);
    }
    send_command(&unit, &bank, "B-P:2,255");
    send_command(&unit, &bank, "B-P:2,13");
    const uint8_t cr = '\r';
    expect_direct(&unit, &bank, channel, &cr, 1, true);
    send_command(&unit, &bank, "B-P:2,0");
    put_bytes(&unit, &bank, 2, 0x5a, 1);
    send_command(&unit, &bank, "B-P:2,0");
    const uint8_t changed = 0x5a;
    expect_direct(&unit, &bank, channel, &changed, 1, false);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    ck_assert_uint_eq(image.reads + image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

// RNGET2 duplicates selected bytes, exposes links/tails and sets EOI only on pointer equality.
START_TEST(test_reader_pointer_offsets_and_prepared_byte) {
    static const uint8_t offsets[] = {0, 1, 2, 7, 8, 9, 127, 254, 255};
    const unsigned int format = (unsigned int)_i / (2u * count_of(offsets));
    const bool singleton = ((unsigned int)_i / count_of(offsets)) % 2u != 0;
    const uint8_t offset = offsets[(unsigned int)_i % count_of(offsets)];
    dos_media_bank_t bank = {0};
    image_t image;
    mount_reader_pointer_image(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t reader = open_name(&unit, &bank, 2, singleton ? "0:ONE,P,R" : "0:READ,P,R");
    ck_assert_uint_ne(reader, DOS_UNUSED);
    const uint8_t* const page = unit.buffers[unit.channels[reader].buffers[0]].bytes;
    const uint8_t expected[] = {page[offset], page[offset]};
    const unsigned int reads = image.reads, writes = image.writes, flushes = image.flushes;
    char command[20];
    snprintf(command, sizeof(command), "B-P:2,%u", offset);
    send_command(&unit, &bank, command);
    ck_assert_uint_eq(image.reads, reads);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    const bool eoi = offset == (singleton ? 2 : 0);
    expect_direct(&unit, &bank, reader, expected, eoi ? 1 : 2, eoi || (singleton && offset == 255));
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(image.flushes, flushes);
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    ck_assert_uint_eq(image.writes, writes);
    free(image.bytes);
}
END_TEST

// Native DBLBUF has already switched when the last old-page byte is prepared.
START_TEST(test_reader_pointer_materializes_active_buffer) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int consumed = 252u + (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_reader_pointer_image(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t reader = open_name(&unit, &bank, 2, "0:READ,P,R");
    uint8_t prefix[255];
    for (unsigned int index = 0; index < consumed; ++index) {
        prefix[index] = index;
    }
    expect_direct(&unit, &bank, reader, prefix, consumed, false);
    send_command(&unit, &bank, "B-P:2,2");
    finish_open(&unit, &bank);
    const uint8_t first = consumed >= 253u ? 254 : 0;
    const uint8_t expected[] = {first, first, (uint8_t)(first + 1u)};
    expect_direct(&unit, &bank, reader, expected, sizeof(expected), false);
    free(image.bytes);
}
END_TEST

// B-P preserves pending EOI, and only equality revives an exhausted ordinary reader.
START_TEST(test_reader_pointer_sticky_eoi_and_revival) {
    dos_media_bank_t bank = {0};
    image_t image;
    mount_reader_pointer_image(&bank, &image, (unsigned int)_i);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t reader = open_name(&unit, &bank, 2, "0:ONE,P,R");
    const uint8_t initial[] = {'A', 0, 2};
    expect_direct(&unit, &bank, reader, initial, sizeof(initial), false);
    send_command(&unit, &bank, "B-P:2,0");
    finish_open(&unit, &bank);
    const uint8_t zero = 0, a = 'A';
    expect_direct(&unit, &bank, reader, &zero, 1, true);
    send_command(&unit, &bank, "B-P:2,0");
    finish_open(&unit, &bank);
    uint8_t byte;
    bool eoi;
    ck_assert_uint_eq(dos_stage(&unit, &bank, reader, &byte, 1, &eoi), 0);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    send_command(&unit, &bank, "B-P:2,2");
    finish_open(&unit, &bank);
    expect_direct(&unit, &bank, reader, &a, 1, true);
    free(image.bytes);
}
END_TEST

// FNDRCH also permits modify-opened files and raw non-LOAD directory channels.
START_TEST(test_reader_pointer_modify_and_raw_directory_are_read_only) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int mode = (unsigned int)_i % 3u;
    static const char* const names[] = {"0:READ,P,R", "0:READ,P,M", "$0"};
    dos_media_bank_t bank = {0};
    image_t image;
    mount_reader_pointer_image(&bank, &image, format);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    if (mode == 1) {
        sector_bytes(&image, type, format == 0 ? 18 : 39, 1)[2] &= ~0x80u;
    }
    uint8_t* const before = malloc(image.size);
    ck_assert_ptr_nonnull(before);
    memcpy(before, image.bytes, image.size);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t reader = open_name(&unit, &bank, 2, names[mode]);
    ck_assert_uint_ne(reader, DOS_UNUSED);
    const unsigned int reads = image.reads;
    send_command(&unit, &bank, "B-P:2,4");
    ck_assert_uint_eq(image.reads, reads);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    const uint8_t byte = mode == 2 ? sector_bytes(&image, type, format == 0 ? 18 : 39, 0)[4] : 2;
    const uint8_t expected[] = {byte, byte};
    expect_direct(&unit, &bank, reader, expected, sizeof(expected), false);
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    ck_assert_mem_eq(image.bytes, before, image.size);
    ck_assert_uint_eq(image.writes + image.flushes, 0);
    free(before);
    free(image.bytes);
}
END_TEST

// Failed DBLBUF preserves the old prepared byte and the cached failed job.
START_TEST(test_reader_pointer_failed_normalization_preserves_cursor) {
    dos_media_bank_t bank = {0};
    image_t image;
    mount_reader_pointer_image(&bank, &image, (unsigned int)_i);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t reader = open_name(&unit, &bank, 2, "0:READ,P,R");
    uint8_t prefix[253];
    for (unsigned int index = 0; index < sizeof(prefix); ++index) {
        prefix[index] = index;
    }
    expect_direct(&unit, &bank, reader, prefix, sizeof(prefix), false);
    unit.buffers[unit.channels[reader].buffers[1]].resident = false;
    const dos_channel_t before = unit.channels[reader];
    image.fail_read = image.reads + 1;
    send_command(&unit, &bank, "B-P:2,2");
    ck_assert_uint_eq(image.reads + 1, image.fail_read);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_mem_eq(&unit.channels[reader], &before, sizeof(before));
    image.fail_read = 0;
    const uint8_t retained = 253;
    expect_direct(&unit, &bank, reader, &retained, 1, false);
    uint8_t byte;
    bool eoi;
    const unsigned int reads = image.reads;
    ck_assert_uint_eq(dos_stage(&unit, &bank, reader, &byte, 1, &eoi), 0);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

// Block reads keep sequential GET, count-byte LSTCHR and ordinary EOF readiness.
START_TEST(test_reader_block_counted_and_raw_boundaries) {
    static const uint8_t counts[] = {0, 1, 2, 4, 254, 255};
    const unsigned int format = (unsigned int)_i / (2u * count_of(counts));
    const bool raw = _i % 2 != 0;
    const uint8_t count = counts[((unsigned int)_i / 2u) % count_of(counts)];
    dos_media_bank_t bank = {0};
    image_t image;
    mount_reader_pointer_image(&bank, &image, format);
    uint8_t* const page = sector_bytes(&image, bank.units[0].slots[0].io.image.type, 1, 3);
    for (unsigned int offset = 0; offset < DISKIMAGE_SECTOR_SIZE; ++offset) {
        page[offset] = offset ^ 0x5au;
    }
    page[0] = count;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t reader = open_name(&unit, &bank, 2, "0:READ,P,R");
    const uint8_t peer = open_name(&unit, &bank, 3, "0:ONE,P,R");
    const dos_channel_t saved_peer = unit.channels[peer];
    const unsigned int reads = image.reads;
    const uint16_t owned = unit.buffer_use;
    send_command(&unit, &bank, raw ? "U1:2,0,1,3" : "B-R:2,0,1,3");
    ck_assert_uint_eq(image.reads, reads);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(image.reads, reads + 1);
    ck_assert_uint_eq(unit.buffer_use, owned);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    uint8_t expected[DISKIMAGE_SECTOR_SIZE + 1u];
    unsigned int length;
    if (raw) {
        expected[0] = page[0];
        memcpy(expected + 1, page, DISKIMAGE_SECTOR_SIZE);
        length = sizeof(expected);
    } else {
        expected[0] = page[1];
        length = count == 0 ? 8u : count == 1 ? 1u : count + 1u;
        memcpy(expected + 1, page + 1, length - 1u);
    }
    expect_direct(&unit, &bank, reader, expected, length, raw || count != 0);
    if (raw || count != 0) {
        bool eoi;
        ck_assert_uint_eq(dos_stage(&unit, &bank, reader, expected, 1, &eoi), 0);
    }
    const unsigned int last = raw ? 255u : count;
    char command[20];
    snprintf(command, sizeof(command), "B-P:2,%u", last);
    send_command(&unit, &bank, command);
    finish_open(&unit, &bank);
    expect_direct(&unit, &bank, reader, page + last, 1, true);
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    ck_assert_uint_eq(image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

// GETACT must overwrite the already switched page, not the lazy old-page cursor.
START_TEST(test_reader_block_materializes_active_page) {
    const unsigned int format = (unsigned int)_i / 8u;
    const unsigned int consumed = 252u + (unsigned int)_i % 4u;
    const bool pointed = ((unsigned int)_i / 4u) % 2u != 0;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_reader_pointer_image(&bank, &image, format);
    uint8_t* const page = sector_bytes(&image, bank.units[0].slots[0].io.image.type, 1, 3);
    page[0] = 4;
    memcpy(page + 1, "TEST", 4);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t reader = open_name(&unit, &bank, 2, "0:READ,P,R");
    uint8_t prefix[255];
    for (unsigned int offset = 0; offset < consumed; ++offset) {
        prefix[offset] = offset;
    }
    expect_direct(&unit, &bank, reader, prefix, consumed, false);
    if (pointed) {
        send_command(&unit, &bank, "B-P:2,4");
        finish_open(&unit, &bank);
    }
    const uint8_t active = consumed >= 253u ? 1 : 0;
    const uint8_t other = unit.channels[reader].buffers[active ^ 1u];
    const dos_buffer_t saved_other = unit.buffers[other];
    send_command(&unit, &bank, "B-R:2,0,1,3");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.channels[reader].active, active);
    ck_assert_mem_eq(&unit.buffers[other], &saved_other, sizeof(saved_other));
    const uint8_t expected[] = {'T', 'T', 'E', 'S', 'T'};
    expect_direct(&unit, &bank, reader, expected, sizeof(expected), true);
    ck_assert_uint_eq(image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

// A zero count switches to the old prefetched job, not the overwritten page's link.
START_TEST(test_reader_block_zero_count_retains_prefetched_source_drive) {
    dos_media_bank_t bank = {0}, other = {0};
    image_t source, target;
    mount_reader_pointer_image(&bank, &source, (unsigned int)_i);
    mount_blank(&other, &target, (unsigned int)_i);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    uint8_t* const page = sector_bytes(&target, bank.units[0].slots[0].io.image.type, 1, 3);
    memset(page, 0x5a, DISKIMAGE_SECTOR_SIZE);
    page[0] = 0;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t reader = open_name(&unit, &bank, 2, "0:READ,P,R");
    const unsigned int reads = source.reads;
    send_command(&unit, &bank, "B-R:2,1,1,3");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    send_command(&unit, &bank, "B-P:2,255");
    finish_open(&unit, &bank);
    const uint8_t expected[] = {0x5a, 0x5a, 254, 255};
    expect_direct(&unit, &bank, reader, expected, sizeof(expected), false);
    ck_assert_uint_eq(unit.channels[reader].media.slot, 0);
    send_command(&unit, &bank, "B-P:2,8");
    finish_open(&unit, &bank);
    const uint8_t last = 4;
    expect_direct(&unit, &bank, reader, &last, 1, true);
    ck_assert_uint_eq(source.reads, reads);
    ck_assert_uint_eq(source.writes + target.writes + source.flushes + target.flushes, 0);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// RD1 interprets retained alternate RAM even when a terminal OPEN submitted no job.
START_TEST(test_reader_zero_count_terminal_uses_retained_alternate_ram) {
    const unsigned int format = (unsigned int)_i / 3u, variant = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_reader_pointer_image(&bank, &image, format);
    uint8_t* const block = sector_bytes(&image, bank.units[0].slots[0].io.image.type, 1, 3);
    memset(block, 0x5a, DISKIMAGE_SECTOR_SIZE);
    block[0] = 0;
    uint8_t* const successor = sector_bytes(&image, bank.units[0].slots[0].io.image.type, 1, 4);
    successor[0] = 0;
    successor[1] = 4;
    memcpy(successor + 2, "END", 3);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);

    // Seed a returned direct page before OPEN reclaims it as an unused alternate.
    const uint8_t seed = open_name(&unit, &bank, 0, "#8");
    const uint8_t buffer = unit.channels[seed].buffers[0];
    uint8_t* const retained = unit.buffers[buffer].bytes;
    memset(retained, 'R', DISKIMAGE_SECTOR_SIZE);
    retained[0] = variant == 0 ? 0 : 1;
    retained[1] = 4;
    memcpy(retained + 2, "RAM", 3);
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 0));
    const uint8_t reader = open_name(&unit, &bank, 2, "0:ONE,P,R");
    ck_assert_uint_eq(unit.channels[reader].buffers[1], buffer);
    send_command(&unit, &bank, "B-R:2,0,1,3");
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "B-P:2,255");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    if (variant == 2) {
        const dos_media_io_t io = bank.units[0].slots[0].io;
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, &io), DOS_MEDIA_OK);
    }
    const unsigned int reads = image.reads;
    uint8_t expected[DISKIMAGE_SECTOR_SIZE + 3u] = {0x5a, 0x5a};
    const unsigned int retained_count = variant == 0 ? 3u : DISKIMAGE_SECTOR_SIZE - DISKIMAGE_SECTOR_LINK_SIZE;
    memcpy(expected + 2, retained + 2, retained_count);
    if (variant != 2) {
        if (variant == 1) {
            memcpy(expected + 2u + retained_count, "END", 3);
        }
        expect_direct(&unit, &bank, reader, expected, 2u + retained_count + (variant == 1 ? 3u : 0u), true);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        ck_assert_uint_eq(image.reads, reads + (variant == 1 ? 1u : 0u));
    } else {
        // Cached bytes remain observable, but their link cannot read rebound media.
        expect_direct(&unit, &bank, reader, expected, 5, false);
        for (unsigned int count = 0; count < DISKIMAGE_SECTOR_SIZE; ++count) {
            uint8_t byte;
            bool eoi;
            if (dos_stage(&unit, &bank, reader, &byte, 1, &eoi) == 0) {
                break;
            }
            ck_assert(!eoi);
            dos_get(&unit, &bank, reader, 1);
        }
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
        ck_assert_uint_eq(image.reads, reads);
    }
    ck_assert_uint_eq(image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

// A terminal-page overwrite must switch to the prior page, not follow a null link.
START_TEST(test_reader_zero_count_terminal_reuses_previous_page) {
    dos_media_bank_t bank = {0};
    image_t image;
    mount_reader_pointer_image(&bank, &image, (unsigned int)_i);
    uint8_t* const block = sector_bytes(&image, bank.units[0].slots[0].io.image.type, 1, 3);
    memset(block, 0x5a, DISKIMAGE_SECTOR_SIZE);
    block[0] = 0;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t reader = open_name(&unit, &bank, 2, "0:READ,P,R");
    uint8_t prefix[DISKIMAGE_SECTOR_SIZE - DISKIMAGE_SECTOR_LINK_SIZE];
    for (unsigned int index = 0; index < sizeof(prefix); ++index) {
        prefix[index] = index;
    }
    expect_direct(&unit, &bank, reader, prefix, sizeof(prefix), false);
    send_command(&unit, &bank, "B-R:2,0,1,3");
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "B-P:2,255");
    finish_open(&unit, &bank);
    const unsigned int reads = image.reads;
    uint8_t expected[DISKIMAGE_SECTOR_SIZE + 7u] = {0x5a, 0x5a};
    memcpy(expected + 2, prefix, sizeof(prefix));
    for (unsigned int index = 0; index < 7u; ++index) {
        expected[2u + sizeof(prefix) + index] = (uint8_t)(254u + index);
    }
    expect_direct(&unit, &bank, reader, expected, sizeof(expected), true);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(image.reads, reads + 1u);
    ck_assert_uint_eq(image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

// Switching a cached alternate page must not renew an old pair's generation.
START_TEST(test_reader_alternate_switch_preserves_captured_generation) {
    const unsigned int format = (unsigned int)_i / 6u, variant = (unsigned int)_i % 3u;
    const unsigned int source_drive = ((unsigned int)_i / 3u) % 2u, target_drive = source_drive ^ 1u;
    dos_media_bank_t bank = {0}, other = {0};
    image_t source, target, replacement;
    mount_reader_pointer_image(&bank, &source, format);
    mount_peer(&bank, &target, format);
    mount_blank(&other, &replacement, format);
    if (source_drive != 0) {
        const dos_media_io_t pair[] = {bank.units[0].slots[1].io, bank.units[0].slots[0].io};
        ck_assert_int_eq(dos_media_set_unit(&bank, 0, pair), DOS_MEDIA_OK);
    }
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    uint8_t* const alternate = sector_bytes(&source, type, 1, 1);
    alternate[0] = 1;
    alternate[1] = 4;
    uint8_t* const page = sector_bytes(&target, type, 1, 3);
    memset(page, 0x5a, DISKIMAGE_SECTOR_SIZE);
    page[0] = 0;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    char command[24];
    snprintf(command, sizeof(command), "%u:READ,P,R", source_drive);
    const uint8_t reader = open_name(&unit, &bank, 2, command);
    const uint32_t identity = unit.channels[reader].media.identity;
    snprintf(command, sizeof(command), "B-R:2,%u,1,3", target_drive);
    send_command(&unit, &bank, command);
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "B-P:2,255");
    finish_open(&unit, &bank);
    const unsigned int changed = variant == 2 ? target_drive : source_drive;
    const dos_media_io_t retained = bank.units[0].slots[changed].io;
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, changed, variant == 0 ? &retained : &other.units[0].slots[0].io),
                     DOS_MEDIA_OK);
    const unsigned int reads = source.reads + target.reads + replacement.reads;
    const uint8_t prefix[] = {0x5a, 0x5a, 254, 255};
    expect_direct(&unit, &bank, reader, prefix, sizeof(prefix), false);

    // Cached old bytes remain usable, but the next refill must reject the old token.
    for (unsigned int count = 0; count < 300u; ++count) {
        uint8_t byte;
        bool eoi;
        if (dos_stage(&unit, &bank, reader, &byte, 1, &eoi) == 0) {
            break;
        }
        dos_get(&unit, &bank, reader, 1);
        if (eoi) {
            break;
        }
    }
    ck_assert_uint_eq(source.reads + target.reads + replacement.reads, reads);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_uint_eq(unit.channels[reader].media.identity, identity);
    free(source.bytes);
    free(target.bytes);
    free(replacement.bytes);
}
END_TEST

// A cached directory footer cannot renew its captured pair when entering the peer.
START_TEST(test_directory_peer_switch_preserves_captured_generation) {
    const unsigned int format = (unsigned int)_i / 2u, first = (unsigned int)_i % 2u;
    dos_media_bank_t bank = {0}, other = {0};
    image_t images[3];
    mount_blank(&bank, &images[0], format);
    mount_peer(&bank, &images[1], format);
    mount_blank(&other, &images[2], format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, "I0");
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "I1");
    finish_open(&unit, &bank);
    const uint8_t listing = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, first == 0 ? "$0:*,1:*" : "$1:*,0:*");
    uint8_t bytes[32];
    bool eoi;
    ck_assert_uint_eq(dos_stage(&unit, &bank, listing, bytes, sizeof(bytes), &eoi), sizeof(bytes));
    dos_get(&unit, &bank, listing, sizeof(bytes));
    ck_assert_uint_eq(dos_stage(&unit, &bank, listing, bytes, sizeof(bytes), &eoi), 0);
    ck_assert_int_eq(unit.channels[listing].directory, DOS_DIRECTORY_FOOTER);
    const uint32_t identity = unit.channels[listing].media.identity;
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, first ^ 1u, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    ck_assert_uint_eq(dos_stage(&unit, &bank, listing, bytes, sizeof(bytes), &eoi), sizeof(bytes));
    dos_get(&unit, &bank, listing, sizeof(bytes));
    ck_assert_uint_eq(dos_stage(&unit, &bank, listing, bytes, 1, &eoi), 0);
    ck_assert_uint_eq(images[2].reads, 0);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_uint_eq(unit.channels[listing].media.identity, identity);
    for (unsigned int index = 0; index < count_of(images); ++index) {
        free(images[index].bytes);
    }
}
END_TEST

// Modify and raw-directory readers use sequential GET too, without becoming writers.
START_TEST(test_reader_block_modify_and_raw_directory_are_read_only) {
    static const char* const names[] = {"0:READ,P,R", "0:READ,P,M", "$0"};
    const unsigned int format = (unsigned int)_i / 6u;
    const unsigned int mode = ((unsigned int)_i / 2u) % count_of(names);
    const bool raw = _i % 2 != 0;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_reader_pointer_image(&bank, &image, format);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    if (mode == 1) {
        sector_bytes(&image, type, format == 0 ? 18 : 39, 1)[2] &= ~0x80u;
    }
    uint8_t* const page = sector_bytes(&image, type, 1, 3);
    page[0] = 4;
    memcpy(page + 1, "TEST", 4);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t reader = open_name(&unit, &bank, 2, names[mode]);
    ck_assert_uint_ne(reader, DOS_UNUSED);
    send_command(&unit, &bank, raw ? "UA:2,0,1,3" : "B-R:2,0,1,3");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    const uint8_t counted[] = {'T', 'T', 'E', 'S', 'T'}, whole[] = {4, 4, 'T', 'E'};
    expect_direct(&unit, &bank, reader, raw ? whole : counted, raw ? sizeof(whole) : sizeof(counted), !raw);
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    ck_assert_uint_eq(image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

// Failed source normalization retains the prepared byte and failed successor job.
START_TEST(test_reader_block_failed_normalization_preserves_cursor) {
    static const char* const commands[] = {"B-R:2,0,1,3", "B-W:2,0,1,3", "U2:2,0,1,3"};
    dos_media_bank_t bank = {0};
    image_t image;
    mount_reader_pointer_image(&bank, &image, (unsigned int)_i / count_of(commands));
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t reader = open_name(&unit, &bank, 2, "0:READ,P,R");
    uint8_t prefix[253];
    for (unsigned int index = 0; index < sizeof(prefix); ++index) {
        prefix[index] = index;
    }
    expect_direct(&unit, &bank, reader, prefix, sizeof(prefix), false);
    unit.buffers[unit.channels[reader].buffers[1]].resident = false;
    const dos_channel_t before = unit.channels[reader];
    image.fail_read = image.reads + 1;
    send_command(&unit, &bank, commands[(unsigned int)_i % count_of(commands)]);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_mem_eq(&unit.channels[reader], &before, sizeof(before));
    image.fail_read = 0;
    const uint8_t retained = 253;
    expect_direct(&unit, &bank, reader, &retained, 1, false);
    uint8_t byte;
    bool eoi;
    const unsigned int reads = image.reads;
    ck_assert_uint_eq(dos_stage(&unit, &bank, reader, &byte, 1, &eoi), 0);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

// GETPNT observes the post-preparation pointer, including DBLBUF and terminal wrap.
START_TEST(test_reader_block_writes_materialize_native_pointer) {
    static const unsigned int consumed[] = {0, 4, 252, 253, 254, 255, 260, 261};
    static const uint8_t pointers[] = {3, 7, 255, 2, 3, 4, 0, 0};
    static const char* const operations[] = {"B-W", "U2", "UB"};
    const unsigned int sample = (unsigned int)_i % count_of(consumed);
    const unsigned int operation = ((unsigned int)_i / count_of(consumed)) % count_of(operations);
    const unsigned int drive = ((unsigned int)_i / (count_of(consumed) * count_of(operations))) % 2u;
    const unsigned int format = (unsigned int)_i / (2u * count_of(consumed) * count_of(operations));
    const bool raw = operation != 0, exhausted = consumed[sample] == 261;
    dos_media_bank_t bank = {0}, other = {0};
    image_t images[2];
    mount_reader_pointer_image(&bank, &images[0], format);
    mount_blank(&other, &images[1], format);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    uint8_t* originals[2];
    for (unsigned int slot = 0; slot < 2; ++slot) {
        originals[slot] = malloc(images[slot].size);
        ck_assert_ptr_nonnull(originals[slot]);
        memcpy(originals[slot], images[slot].bytes, images[slot].size);
    }
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t reader = open_name(&unit, &bank, 2, drive ? "READ,P,R" : "0:READ,P,R");
    const uint8_t peer = open_name(&unit, &bank, 3, "0:ONE,P,R");
    uint8_t prefix[261];
    for (unsigned int offset = 0; offset < sizeof(prefix); ++offset) {
        prefix[offset] = offset;
    }
    expect_direct(&unit, &bank, reader, prefix, consumed[sample], exhausted);
    const dos_channel_t saved_peer = unit.channels[peer], before = unit.channels[reader];
    const uint8_t active = consumed[sample] >= 253u ? 1 : 0;
    const dos_buffer_t alternate = unit.buffers[before.buffers[active ^ 1u]];
    uint8_t expected[DISKIMAGE_SECTOR_SIZE];
    memcpy(expected, sector_bytes(&images[0], type, 1, active), sizeof(expected));
    if (!raw) {
        expected[0] = sample == 0 ? 2 : sample == 1 ? 6 : sample == 2 ? 254 : sample == 4 ? 2 : sample == 5 ? 3 : 1;
    }
    const unsigned int writes = images[drive].writes, flushes = images[drive].flushes;
    char command[24];
    snprintf(command, sizeof(command), "%s:2,%u,1,3", operations[operation], drive | 2u);
    send_command(&unit, &bank, command);
    ck_assert_mem_eq(&unit.channels[reader], &before, sizeof(before));
    ck_assert_uint_eq(images[drive].writes, writes);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    dos_channel_t* const state = &unit.channels[reader];
    ck_assert_uint_eq(state->active, active);
    ck_assert_uint_eq(state->position, raw ? pointers[sample] : 1);
    ck_assert_uint_eq(state->read_byte, raw ? (uint8_t)(exhausted ? 260 : consumed[sample]) : expected[1]);
    ck_assert_int_eq(state->read_eoi, consumed[sample] == 260);
    ck_assert_int_eq(state->ready, !exhausted);
    ck_assert_int_eq(state->access, DOS_ACCESS_READ);
    ck_assert_uint_eq(state->file_type, before.file_type);
    ck_assert_uint_eq(state->read_last, active ? 8 : 0);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    ck_assert_mem_eq(&unit.buffers[before.buffers[active ^ 1u]], &alternate, sizeof(alternate));
    ck_assert_uint_eq((unit.buffer_drives >> before.buffers[active]) & 1u, drive);
    ck_assert_uint_eq(images[drive].writes, writes + 1);
    ck_assert_uint_eq(images[drive].flushes, flushes + 1);
    uint32_t destination;
    ck_assert(diskimage_sector_offset(type, 1, 3, &destination));
    memcpy(originals[drive] + destination, expected, sizeof(expected));
    for (unsigned int slot = 0; slot < 2; ++slot) {
        ck_assert_mem_eq(images[slot].bytes, originals[slot], images[slot].size);
    }
    // Native ordinary GET preserves EOF, rather than adopting circular direct GET.
    if (!exhausted) {
        const uint8_t output[] = {state->read_byte, raw ? expected[pointers[sample]] : expected[1]};
        expect_direct(&unit, &bank, reader, output, state->read_eoi ? 1 : 2, state->read_eoi);
    }
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    ck_assert_uint_eq(images[drive].writes, writes + 1);
    ck_assert_uint_eq(images[drive].flushes, flushes + 1);
    // Recreate DOS state and read the raw destination from the persisted image.
    dos_reset(&unit, bank.units[0].model);
    const uint8_t direct = open_name(&unit, &bank, 2, "#");
    snprintf(command, sizeof(command), "U1:2,%u,1,3", drive);
    send_command(&unit, &bank, command);
    finish_open(&unit, &bank);
    expect_direct(&unit, &bank, direct, expected, sizeof(expected), true);
    for (unsigned int slot = 0; slot < 2; ++slot) {
        free(originals[slot]);
        free(images[slot].bytes);
    }
}
END_TEST

// B-W refreshes CHNDAT only on success, while raw aliases retain every reader field.
START_TEST(test_reader_block_write_failures_retain_page_and_source) {
    const unsigned int variant = (unsigned int)_i % 7u;
    const bool raw = ((unsigned int)_i / 7u) % 2u != 0;
    const unsigned int format = (unsigned int)_i / 14u;
    dos_media_bank_t bank = {0}, other = {0};
    image_t source, target;
    mount_reader_pointer_image(&bank, &source, format);
    mount_blank(&other, &target, format);
    const dos_media_io_t original = other.units[0].slots[0].io;
    dos_media_io_t io = original;
    if (variant == 2) {
        io.image.write = NULL;
    }
    if (variant == 5) {
        set_map_version(&target, io.image.type, format, 0xff);
    }
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &io), DOS_MEDIA_OK);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t reader = open_name(&unit, &bank, 2, "0:READ,P,R");
    const uint8_t peer = open_name(&unit, &bank, 3, "0:ONE,P,R");
    const dos_channel_t saved_peer = unit.channels[peer];
    const dos_buffer_t saved_other = unit.buffers[unit.channels[reader].buffers[1]];
    const uint8_t prefix[] = {0, 1, 2, 3};
    expect_direct(&unit, &bank, reader, prefix, sizeof(prefix), false);
    send_command(&unit, &bank, raw ? "U2:2,1,1,3" : "B-W:2,1,1,3");
    if (variant == 0) {
        target.fail_write = 1;
    } else if (variant == 1) {
        target.fail_flush = true;
    } else if (variant == 3 || variant == 4) {
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, variant == 3 ? &original : NULL), DOS_MEDIA_OK);
    } else if (variant == 6) {
        target.fail_read = target.reads + 1;
    }
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, variant == 2   ? DOS_STATUS_WRITE_PROTECT
                                        : variant == 5 ? DOS_STATUS_POWER_ON
                                        : variant == 6 ? DOS_STATUS_READ_ERROR
                                                       : DOS_STATUS_WRITE_ERROR);
    dos_channel_t* const state = &unit.channels[reader];
    ck_assert_uint_eq(state->position, raw ? 7 : 1);
    ck_assert_uint_eq(state->read_byte, 4);
    ck_assert_uint_eq(unit.buffers[state->buffers[state->active]].bytes[0], raw ? 1 : 6);
    ck_assert_uint_eq(target.writes, variant < 2 ? 1 : 0);
    ck_assert_uint_eq(target.flushes, variant == 1 ? 1 : 0);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    ck_assert_mem_eq(&unit.buffers[state->buffers[1]], &saved_other, sizeof(saved_other));
    ck_assert_uint_eq(source.writes + source.flushes, 0);
    target.fail_read = target.fail_write = 0;
    target.fail_flush = false;
    if (variant >= 2 && variant <= 4) {
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &original), DOS_MEDIA_OK);
    } else if (variant == 5) {
        unit.bam[1].buffer.bytes[2] = format == 0 ? 'A' : 'C';
    }
    send_command(&unit, &bank, "UB:2,1,1,3");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_mem_eq(sector_bytes(&target, original.image.type, 1, 3),
                     unit.buffers[state->buffers[state->active]].bytes, DISKIMAGE_SECTOR_SIZE);
    ck_assert_uint_eq(state->read_byte, 4);
    ck_assert_uint_eq(state->position, raw ? 7 : 1);
    const uint8_t retained[] = {4, raw ? 5 : 1};
    expect_direct(&unit, &bank, reader, retained, sizeof(retained), false);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// Singleton wrap and RNGET2 equality retain native EOI/readiness across writes.
START_TEST(test_reader_block_write_singleton_and_eof) {
    const unsigned int phase = (unsigned int)_i % 3u;
    const bool raw = ((unsigned int)_i / 3u) % 2u != 0;
    const unsigned int format = (unsigned int)_i / 6u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_reader_pointer_image(&bank, &image, format);
    uint8_t* const counted = sector_bytes(&image, bank.units[0].slots[0].io.image.type, 1, 4);
    counted[0] = 1;
    counted[1] = 0x44;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t reader = open_name(&unit, &bank, 2, "0:ONE,P,R");
    if (phase == 1) {
        const uint8_t a = 'A';
        expect_direct(&unit, &bank, reader, &a, 1, false);
    } else if (phase == 2) {
        send_command(&unit, &bank, "B-R:2,0,1,4");
        finish_open(&unit, &bank);
        const uint8_t byte = 0x44;
        expect_direct(&unit, &bank, reader, &byte, 1, true);
    }
    send_command(&unit, &bank, raw ? "UB:2,0,1,3" : "B-W:2,0,1,3");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_int_eq(unit.channels[reader].access, DOS_ACCESS_READ);
    if (phase == 2) {
        if (raw) {
            uint8_t byte;
            bool eoi;
            ck_assert_uint_eq(dos_stage(&unit, &bank, reader, &byte, 1, &eoi), 0);
        } else {
            const uint8_t byte = 0x44;
            expect_direct(&unit, &bank, reader, &byte, 1, true);
        }
    } else {
        const uint8_t raw_initial[] = {'A', 0, 2, 'A'}, raw_after[] = {0, 2, 'A'}, counted_bytes[] = {2, 2, 'A'};
        expect_direct(&unit, &bank, reader, raw ? (phase == 0 ? raw_initial : raw_after) : counted_bytes,
                      raw && phase == 0 ? sizeof(raw_initial) : sizeof(counted_bytes), true);
    }
    // A block job never grants the reader ordinary LISTEN access.
    const dos_channel_t before = unit.channels[reader];
    dos_put(&unit, &bank, 2, 0x99);
    ck_assert_mem_eq(&unit.channels[reader], &before, sizeof(before));
    free(image.bytes);
}
END_TEST

// Modify and raw-directory readers expose the same page without acquiring PUT access.
START_TEST(test_reader_block_write_modify_and_directory) {
    static const char* const names[] = {"0:READ,P,R", "0:READ,P,M", "$0"};
    const unsigned int mode = (unsigned int)_i % count_of(names);
    const bool raw = ((unsigned int)_i / count_of(names)) % 2u != 0;
    const unsigned int format = (unsigned int)_i / (2u * count_of(names));
    dos_media_bank_t bank = {0};
    image_t image;
    mount_reader_pointer_image(&bank, &image, format);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    if (mode == 1) {
        sector_bytes(&image, type, format == 0 ? 18 : 39, 1)[2] &= ~0x80u;
    }
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t reader = open_name(&unit, &bank, 2, names[mode]);
    ck_assert_uint_ne(reader, DOS_UNUSED);
    uint8_t expected[DISKIMAGE_SECTOR_SIZE];
    memcpy(expected, unit.buffers[unit.channels[reader].buffers[0]].bytes, sizeof(expected));
    const dos_access_t access = unit.channels[reader].access;
    send_command(&unit, &bank, "B-P:2,4");
    finish_open(&unit, &bank);
    send_command(&unit, &bank, raw ? "U2:2,0,1,3" : "B-W:2,0,1,3");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    if (!raw) {
        expected[0] = 3;
    }
    ck_assert_mem_eq(sector_bytes(&image, type, 1, 3), expected, sizeof(expected));
    ck_assert_int_eq(unit.channels[reader].access, access);
    ck_assert_uint_eq(unit.channels[reader].read_byte, expected[raw ? 4 : 1]);
    const dos_channel_t before = unit.channels[reader];
    dos_put(&unit, &bank, 2, 0x99);
    ck_assert_mem_eq(&unit.channels[reader], &before, sizeof(before));
    ck_assert_mem_eq(unit.buffers[before.buffers[0]].bytes, expected, sizeof(expected));
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    ck_assert_uint_eq(image.writes, 1);
    ck_assert_uint_eq(image.flushes, 1);
    free(image.bytes);
}
END_TEST

// Shared BUFTST rejects writers, permits ordinary reads and excludes SA 14.
START_TEST(test_block_pointer_channel_permissions_and_excluded_readers) {
    const unsigned int operation = (unsigned int)_i % 5u;
    static const char* const writer_commands[] = {"B-P:2,0", "B-R:2,0,1,0", "U1:2,0,1,0", "B-W:2,0,1,0", "U2:2,0,1,0"};
    static const char* const allowed_commands[] = {"B-P:13,255", "B-R:13,0,1,0", "U1:13,0,1,0", "B-W:13,0,1,0",
                                                   "U2:13,0,1,0"};
    static const char* const excluded_commands[] = {"B-P:14,0", "B-R:14,0,1,0", "U1:14,0,1,0", "B-W:14,0,1,0",
                                                    "U2:14,0,1,0"};
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, (unsigned int)_i / 5u);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t writer = open_name(&unit, &bank, 2, "0:WRITER,S,W");
    ck_assert_uint_ne(writer, DOS_UNUSED);
    const dos_channel_t saved_writer = unit.channels[writer];
    unsigned int reads = image.reads;
    send_command(&unit, &bank, writer_commands[operation]);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_mem_eq(&unit.channels[writer], &saved_writer, sizeof(saved_writer));
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    const uint8_t reader = open_name(&unit, &bank, 2, "0:WRITER,S,R");
    ck_assert_uint_ne(reader, DOS_UNUSED);
    reads = image.reads;
    send_command(&unit, &bank, writer_commands[operation]);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(image.reads, reads + (operation == 1 || operation == 2 ? 1 : 0));
    const uint8_t allowed = open_name(&unit, &bank, 13, "#");
    ck_assert_uint_ne(allowed, DOS_UNUSED);
    send_command(&unit, &bank, allowed_commands[operation]);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.channels[allowed].position, operation == 0 ? 255 : operation == 2 ? 0 : 1);
    const uint8_t excluded = open_name(&unit, &bank, 14, "#");
    ck_assert_uint_ne(excluded, DOS_UNUSED);
    const dos_channel_t saved_excluded = unit.channels[excluded];
    reads = image.reads;
    send_command(&unit, &bank, excluded_commands[operation]);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_mem_eq(&unit.channels[excluded], &saved_excluded, sizeof(saved_excluded));
    free(image.bytes);
}
END_TEST

// RAM positioning remains independent of removed or replaced backing media.
START_TEST(test_block_pointer_media_changes_do_not_issue_storage_jobs) {
    const unsigned int format = (unsigned int)_i / 2u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_image(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, 2, "#");
    unit.buffers[unit.channels[channel].buffers[0]].bytes[17] = 0x69;
    const dos_media_io_t original = bank.units[0].slots[0].io;
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, _i % 2 ? &original : NULL), DOS_MEDIA_OK);
    send_command(&unit, &bank, "B-P:2,17");
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    const uint8_t expected = 0x69;
    expect_direct(&unit, &bank, channel, &expected, 1, false);
    ck_assert_uint_eq(image.reads + image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

// BP05/ASCHEX uses three low-nibble decimal digits and four bounded parameters.
START_TEST(test_direct_parser_and_single_buffer_ownership) {
    const unsigned int format = (unsigned int)_i / count_of(direct_names);
    const unsigned int variant = (unsigned int)_i % count_of(direct_names);
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, 2, direct_names[variant].name);
    ck_assert_uint_eq(unit.status_code, direct_names[variant].status);
    ck_assert_uint_eq(image.reads + image.writes, 0);
    if (direct_names[variant].status == DOS_STATUS_OK) {
        const uint8_t buffer = direct_names[variant].buffer;
        ck_assert_uint_eq(channel, 0);
        ck_assert_uint_eq(unit.buffer_use, 1u << buffer);
        ck_assert_uint_eq(unit.channels[channel].buffers[0], buffer);
        ck_assert_uint_eq(unit.channels[channel].buffers[1], DOS_UNUSED);
        ck_assert_uint_eq(unit.channels[channel].access, DOS_ACCESS_DIRECT);
        expect_direct(&unit, &bank, channel, &buffer, 1, false);
        ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    } else {
        ck_assert_uint_eq(channel, DOS_UNUSED);
    }
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// PUT shares GET's pointer, refreshes prepared data and retains EOI until a GET.
START_TEST(test_direct_put_wrap_sticky_eoi_and_partial_get) {
    const unsigned int format = (unsigned int)_i / 3u;
    const uint8_t secondary = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, secondary, "#");
    const uint8_t buffer = 11;
    expect_direct(&unit, &bank, channel, &buffer, 1, false);
    put_bytes(&unit, &bank, secondary, 0, DISKIMAGE_SECTOR_SIZE);
    const uint8_t zero = 0;
    expect_direct(&unit, &bank, channel, &zero, 1, true);
    uint8_t tail[253], repeated[255];
    for (unsigned int index = 0; index < sizeof(tail); ++index) {
        tail[index] = index + 1;
    }
    for (unsigned int index = 0; index < sizeof(repeated); ++index) {
        repeated[index] = (uint8_t)(index - 1u);
    }
    if (secondary == IEEE_LOAD_CHANNEL) {
        uint8_t load_tail[254], load_repeated[256];
        load_tail[0] = 1;
        load_repeated[0] = 255;
        for (unsigned int index = 1; index < sizeof(load_tail); ++index) {
            load_tail[index] = index;
        }
        for (unsigned int index = 1; index < sizeof(load_repeated); ++index) {
            load_repeated[index] = (uint8_t)(index - 2u);
        }
        expect_direct(&unit, &bank, channel, load_tail, sizeof(load_tail), true);
        expect_direct(&unit, &bank, channel, load_repeated, sizeof(load_repeated), true);
        expect_direct(&unit, &bank, channel, load_repeated, sizeof(load_repeated), true);
    } else {
        expect_direct(&unit, &bank, channel, tail, sizeof(tail), true);
        expect_direct(&unit, &bank, channel, repeated, sizeof(repeated), true);
        expect_direct(&unit, &bank, channel, repeated, sizeof(repeated), true);
    }
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(image.reads + image.writes, 0);
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, IEEE_COMMAND_CHANNEL));
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// Explicit reservation precedes FNDLNX, while ordinary GETRCH frees an old owner first.
START_TEST(test_direct_capacity_reopen_and_reserved_page_retention) {
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, (unsigned int)_i);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    for (uint8_t secondary = 2; secondary < 2 + DOS_FILE_CHANNEL_COUNT; ++secondary) {
        ck_assert_uint_eq(open_name(&unit, &bank, secondary, "#"), secondary - 2u);
    }
    const uint16_t owners = unit.buffer_use;
    ck_assert_uint_eq(open_name(&unit, &bank, 8, "#"), DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
    ck_assert_uint_eq(unit.buffer_use, owners);
    ck_assert_uint_eq(open_name(&unit, &bank, 8, "#0"), DOS_UNUSED);
    ck_assert_uint_eq(unit.buffer_use, owners | 1u);
    const dos_channel_t before = unit.channels[0];
    ck_assert_uint_eq(open_name(&unit, &bank, 2, "#11"), 0);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
    ck_assert_mem_eq(&unit.channels[0], &before, sizeof(before));
    ck_assert_uint_eq(open_name(&unit, &bank, 2, "#"), 0);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert(dos_close(&unit, &bank, model, IEEE_COMMAND_CHANNEL));
    ck_assert_uint_eq(unit.buffer_use, 1);
    dos_reset(&unit, model);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// Only DOS 2.7 resets an explicitly requested page's previous LSTJOB drive bit.
START_TEST(test_direct_explicit_page_drive_and_initialize) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0}, other = {0};
    image_t image, peer;
    mount_blank(&bank, &image, format);
    mount_blank(&other, &peer, format);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    unit.selected_drive = unit.last_drive = 1;
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "#"), DOS_UNUSED);
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    unit.selected_drive = unit.last_drive = 0;
    const uint8_t channel = open_name(&unit, &bank, 2, "#11");
    ck_assert_uint_eq(unit.channels[channel].media.slot, format == 0 ? 1 : 0);
    send_command(&unit, &bank, "I0");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), format == 0 ? channel : DOS_UNUSED);
    send_command(&unit, &bank, "I1");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.buffer_use, 0);
    // A fallback directory read updates the scratch page's drive after GETBUF.
    ck_assert_uint_ne(open_name(&unit, &bank, 4, "1:PEER,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 4, 0x40, 4);
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 4));
    unit.selected_drive = unit.last_drive = 0;
    ck_assert_uint_ne(open_name(&unit, &bank, 3, "PEER,S,R"), DOS_UNUSED);
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 3));
    unit.selected_drive = unit.last_drive = 0;
    const uint8_t fallback = open_name(&unit, &bank, 2, "#11");
    ck_assert_uint_ne(fallback, DOS_UNUSED);
    ck_assert_uint_eq(unit.channels[fallback].media.slot, format == 0 ? 1 : 0);
    free(image.bytes);
    free(peer.bytes);
}
END_TEST

// Reopening as a direct channel abandons a writer instead of finalizing its tail.
START_TEST(test_direct_reopen_abandons_writer_and_validate_releases_page) {
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, (unsigned int)_i);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:ABORT,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x20, 3);
    const unsigned int writes = image.writes;
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "#"), DOS_UNUSED);
    ck_assert_uint_eq(image.writes, writes);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    uint8_t* const directory = sector_bytes(&image, type, _i == 0 ? 18 : 39, 1);
    ck_assert_uint_eq(directory[2], 1);
    send_command(&unit, &bank, "V0");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(directory[2], 0);
    free(image.bytes);
}
END_TEST

// RAM byte I/O does not consult a missing or replaced disk identity.
START_TEST(test_direct_ram_survives_media_change_without_replacement_io) {
    const unsigned int format = (unsigned int)_i / 2u;
    dos_media_bank_t bank = {0}, other = {0};
    image_t image, replacement;
    mount_image(&bank, &image, format);
    mount_image(&other, &replacement, format);
    if (_i % 2 == 0) {
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, NULL), DOS_MEDIA_OK);
    }
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, 2, "#");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    put_bytes(&unit, &bank, 2, 0, DISKIMAGE_SECTOR_SIZE);
    const uint8_t zero = 0;
    expect_direct(&unit, &bank, channel, &zero, 1, true);
    ck_assert(dos_finalize(&unit, &bank));
    if (format == 0) {
        ck_assert(!dos_close(&unit, &bank, bank.units[0].model, 2));
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
        ck_assert_uint_eq(dos_find_channel(&unit, 2), channel);
    } else {
        ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    }
    ck_assert_uint_eq(
        image.reads + image.writes + image.flushes + replacement.reads + replacement.writes + replacement.flushes, 0);
    free(image.bytes);
    free(replacement.bytes);
}
END_TEST

// MAPOUT and backing flush failures retain the direct owner without closing a peer writer.
START_TEST(test_direct_close_retries_dirty_bam_and_backing_flush) {
    const unsigned int format = (unsigned int)_i / 3u, variant = (unsigned int)_i % 3u;
    const bool flush_failure = variant != 0;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t writer = open_name(&unit, &bank, 2, "0:PENDING,S,W");
    ck_assert_uint_ne(writer, DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x40, 3);
    const uint8_t direct = open_name(&unit, &bank, 3, "#");
    ck_assert_uint_ne(direct, DOS_UNUSED);
    ck_assert(unit.bam[0].dirty);
    const dos_channel_t peer = unit.channels[writer];
    image.fail_write = flush_failure ? 0 : image.writes + 1;
    image.fail_flush = flush_failure;
    ck_assert(!dos_close(&unit, &bank, bank.units[0].model, 3));
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(dos_find_channel(&unit, 3), direct);
    ck_assert_mem_eq(&unit.channels[writer], &peer, sizeof(peer));
    const unsigned int written = image.writes, flushed = image.flushes;
    image.fail_write = 0;
    image.fail_flush = false;
    if (variant == 2) {
        // Even the same callback must not receive an old generation's flush.
        const dos_media_io_t replacement = bank.units[0].slots[0].io;
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, &replacement), DOS_MEDIA_OK);
        const unsigned int rebound_flushes = image.flushes;
        ck_assert(!dos_close(&unit, &bank, bank.units[0].model, 3));
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
        ck_assert_uint_eq(image.writes, written);
        ck_assert_uint_eq(image.flushes, rebound_flushes);
        ck_assert_uint_eq(dos_find_channel(&unit, 3), direct);
        ck_assert_mem_eq(&unit.channels[writer], &peer, sizeof(peer));
        dos_reset(&unit, bank.units[0].model);
    } else {
        ck_assert(dos_close(&unit, &bank, bank.units[0].model, 3));
        ck_assert_uint_eq(image.writes, written + (flush_failure ? 0 : 1));
        ck_assert_uint_eq(image.flushes, flushed + 1u);
        ck_assert_uint_eq(dos_find_channel(&unit, 3), DOS_UNUSED);
        ck_assert_mem_eq(&unit.channels[writer], &peer, sizeof(peer));
        ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    }
    free(image.bytes);
}
END_TEST

// 4040 MAPOUT leaves DRVNUM alone, while DOS 2.7 MO10 selects the page's job drive.
START_TEST(test_direct_close_remembered_drive_is_model_specific) {
    const unsigned int format = (unsigned int)_i / 4u, drive = ((unsigned int)_i / 2u) % 2u;
    const bool close_all = _i % 2 != 0;
    dos_media_bank_t bank = {0};
    image_t images[2];
    mount_blank(&bank, &images[0], format);
    mount_peer(&bank, &images[1], format);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    unit.selected_drive = unit.last_drive = drive;
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "#"), DOS_UNUSED);
    char command[24];
    snprintf(command, sizeof(command), "B-A:%u,1,0", drive ^ 1u);
    send_command(&unit, &bank, command);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.last_drive, drive ^ 1u);
    ck_assert(dos_close(&unit, &bank, model, close_all ? IEEE_COMMAND_CHANNEL : 2));
    ck_assert_uint_eq(unit.selected_drive, format == 0 ? drive ^ 1u : drive);
    ck_assert_uint_eq(unit.last_drive, format == 0 ? drive ^ 1u : drive);
    free(images[0].bytes);
    free(images[1].bytes);
}
END_TEST

// Clean 4040 MAPOUT still validates and writes, unlike DOS 2.7's dirty-map gate.
START_TEST(test_direct_close_clean_map_native_errors_and_retry) {
    const unsigned int format = (unsigned int)_i / 14u, variant = (unsigned int)_i % 7u;
    const bool close_all = ((unsigned int)_i / 7u) % 2u != 0, cached = variant != 4 && variant != 5;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    if (variant == 1) {
        bank.units[0].slots[0].io.image.write = NULL;
    } else if (variant == 2) {
        --fixture_bam_entry(&image, format, 1)[0];
    } else if (variant == 3) {
        sector_bytes(&image, type, format == 0 ? 18 : 38, 0)[2] = 'X';
    }
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    if (cached) {
        send_command(&unit, &bank, "I0");
        finish_open(&unit, &bank);
    }
    const uint8_t direct = open_name(&unit, &bank, 2, "#");
    ck_assert_uint_ne(direct, DOS_UNUSED);
    const unsigned int reads = image.reads;
    if (variant == 4) {
        image.fail_read = reads + 1u;
    } else if (variant == 6) {
        image.fail_write = 1;
        image.fail_write_without_changes = true;
    }
    const bool failed = format == 0 && variant != 0 && variant != 5;
    ck_assert_int_eq(dos_close(&unit, &bank, model, close_all ? IEEE_COMMAND_CHANNEL : 2), !failed);
    static const dos_status_code_t errors[] = {
        DOS_STATUS_OK, DOS_STATUS_WRITE_PROTECT, DOS_STATUS_DIRECTORY_ERROR, DOS_STATUS_POWER_ON, DOS_STATUS_READ_ERROR,
        DOS_STATUS_OK, DOS_STATUS_WRITE_ERROR};
    ck_assert_uint_eq(unit.status_code, format == 0 ? errors[variant] : DOS_STATUS_OK);
    ck_assert_uint_eq(image.writes, format == 0 && (!failed || variant == 6) ? 1u : 0u);
    ck_assert_uint_eq(image.flushes, format == 0 && !failed ? 1u : 0u);
    ck_assert_uint_eq(image.reads, reads + (format == 0 && !cached ? 1u : 0u));
    ck_assert_uint_eq(dos_find_channel(&unit, 2), failed ? direct : DOS_UNUSED);
    if (failed && variant == 1) {
        bank.units[0].slots[0].io.image.write = write_image;
        ck_assert(dos_close(&unit, &bank, model, close_all ? IEEE_COMMAND_CHANNEL : 2));
        ck_assert_uint_eq(image.writes, 1);
        ck_assert_uint_eq(image.flushes, 1);
    }
    free(image.bytes);
}
END_TEST

// Direct LOAD/SAVE allocate pages too, and length rejection precedes parsing.
START_TEST(test_direct_load_save_and_overlong_request) {
    dos_media_bank_t bank = {0};
    image_t image;
    mount_image(&bank, &image, (unsigned int)_i / 2u);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, 2, "#");
    const dos_channel_t saved = unit.channels[channel];
    for (uint8_t secondary = IEEE_LOAD_CHANNEL; secondary <= IEEE_SAVE_CHANNEL; ++secondary) {
        const uint8_t direct = open_name(&unit, &bank, secondary, "#");
        ck_assert_uint_ne(direct, DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        const uint8_t number = secondary == IEEE_LOAD_CHANNEL ? 10 : 9;
        expect_direct(&unit, &bank, direct, &number, 1, false);
    }
    uint8_t overlong[DOS_COMMAND_RECEIVE_CAPACITY + 1u];
    memset(overlong, '0', sizeof(overlong));
    overlong[0] = '#';
    dos_open(&unit, &bank, 0, 2, overlong, DOS_COMMAND_RECEIVE_CAPACITY + (unsigned int)_i % 2u);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_COMMAND_TOO_LONG);
    ck_assert_mem_eq(&unit.channels[channel], &saved, sizeof(saved));
    ck_assert_uint_eq(image.reads + image.writes, 0);
    free(image.bytes);
}
END_TEST

// Direct OPEN errors use CMDERR's ordinary-owner retirement and DIRTYP exemption.
START_TEST(test_direct_open_errors_retire_only_model_eligible_owners) {
    static const char* const failures[] = {"#12", "#11", "#X", "#0,1,2,3,4"};
    const unsigned int variant = (unsigned int)_i % count_of(failures);
    const unsigned int owner = ((unsigned int)_i / count_of(failures)) % 3u;
    const unsigned int format = (unsigned int)_i / (3u * count_of(failures));
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "READER", DISKIMAGE_FTYPE_SEQ, 0x40, 4);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    const uint8_t channel = open_name(&unit, &bank, 2, owner == 0 ? "0:READER,S,R" : owner == 1 ? "0:WRITER,S,W" : "#");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    const dos_channel_t before = unit.channels[channel];
    const uint8_t peer = open_name(&unit, &bank, 3, "#");
    ck_assert_uint_ne(peer, DOS_UNUSED);
    const dos_channel_t saved_peer = unit.channels[peer];
    const uint16_t buffers = unit.buffer_use;
    const unsigned int reads = image.reads, writes = image.writes, flushes = image.flushes;
    open_name(&unit, &bank, 2, failures[variant]);
    ck_assert_uint_eq(unit.status_code, variant < 2 ? DOS_STATUS_NO_CHANNEL : DOS_STATUS_SYNTAX);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(image.flushes, flushes);
    if (format != 0 && owner != 2) {
        ck_assert_uint_eq(dos_find_channel(&unit, 2), DOS_UNUSED);
        uint16_t released = 0;
        for (unsigned int index = 0; index < count_of(before.buffers); ++index) {
            if (before.buffers[index] != DOS_UNUSED) {
                released |= (uint16_t)(1u << before.buffers[index]);
            }
        }
        ck_assert_uint_eq(unit.buffer_use, buffers & (uint16_t)~released);
    } else {
        ck_assert_uint_eq(dos_find_channel(&unit, 2), channel);
        ck_assert_mem_eq(&unit.channels[channel], &before, sizeof(before));
        ck_assert_uint_eq(unit.buffer_use, buffers);
        if (owner == 0) {
            expect_bytes(&unit, &bank, channel, 4, 0x40);
        } else if (owner == 2) {
            expect_direct(&unit, &bank, channel, &before.direct_byte, 1, false);
        } else {
            put_bytes(&unit, &bank, 2, 0x60, 4);
            ck_assert(dos_close(&unit, &bank, model, 2));
            const uint8_t reader = open_name(&unit, &bank, 2, "0:WRITER,S,R");
            ck_assert_uint_ne(reader, DOS_UNUSED);
            expect_bytes(&unit, &bank, reader, 4, 0x60);
        }
    }
    free(image.bytes);
}
END_TEST

// A clean BAM does not excuse a pending raw/count-write backing flush at CLOSE.
START_TEST(test_direct_close_retries_block_write_without_dirty_bam) {
    const unsigned int format = (unsigned int)_i / 6u, variant = (unsigned int)_i % 6u;
    const unsigned int lifecycle = variant / 2u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    const uint8_t channel = open_name(&unit, &bank, 2, "#");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x40, DISKIMAGE_SECTOR_SIZE);
    image.fail_flush = true;
    send_command(&unit, &bank, variant % 2u == 0 ? "U2:2,0,2,0" : "B-W:2,0,2,0");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert(!unit.bam[0].dirty);
    ck_assert(bank.units[0].slots[0].dirty);
    uint8_t expected[DISKIMAGE_SECTOR_SIZE];
    memcpy(expected, unit.buffers[unit.channels[channel].buffers[0]].bytes, sizeof(expected));
    ck_assert_mem_eq(sector_bytes(&image, bank.units[0].slots[0].io.image.type, 2, 0), expected, sizeof(expected));
    const unsigned int writes = image.writes, flushes = image.flushes;
    const unsigned int expected_writes = writes + (format == 0 && lifecycle != 2 ? 1u : 0u);
    ck_assert(!(lifecycle == 2 ? dos_finalize(&unit, &bank)
                               : dos_close(&unit, &bank, model, lifecycle == 0 ? 2 : IEEE_COMMAND_CHANNEL)));
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), channel);
    ck_assert_uint_eq(image.writes, expected_writes);
    ck_assert_uint_eq(image.flushes, flushes + 1u);
    image.fail_flush = false;
    ck_assert(lifecycle == 2 ? dos_finalize(&unit, &bank)
                             : dos_close(&unit, &bank, model, lifecycle == 0 ? 2 : IEEE_COMMAND_CHANNEL));
    ck_assert(!bank.units[0].slots[0].dirty);
    ck_assert_uint_eq(image.writes, expected_writes);
    ck_assert_uint_eq(image.flushes, flushes + 2u);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), lifecycle == 2 ? channel : DOS_UNUSED);
    ck_assert_mem_eq(sector_bytes(&image, bank.units[0].slots[0].io.image.type, 2, 0), expected, sizeof(expected));
    free(image.bytes);
}
END_TEST

// Retrying direct CLOSE must include allocation edits made after its failed flush.
START_TEST(test_direct_close_retry_includes_intervening_map_changes) {
    const unsigned int format = (unsigned int)_i / 3u, lifecycle = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    const uint8_t channel = open_name(&unit, &bank, 2, "#");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    send_command(&unit, &bank, "B-A:0,1,0");
    finish_open(&unit, &bank);
    image.fail_flush = true;
    ck_assert(!(lifecycle == 2 ? dos_finalize(&unit, &bank)
                               : dos_close(&unit, &bank, model, lifecycle == 0 ? 2 : IEEE_COMMAND_CHANNEL)));
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(image.writes, 1);
    ck_assert(!unit.bam[0].dirty);
    const uint8_t persisted_count = fixture_bam_entry(&image, format, 1)[0];
    send_command(&unit, &bank, "B-A:0,1,1");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert(unit.bam[0].dirty);
    image.fail_flush = false;
    ck_assert(lifecycle == 2 ? dos_finalize(&unit, &bank)
                             : dos_close(&unit, &bank, model, lifecycle == 0 ? 2 : IEEE_COMMAND_CHANNEL));
    ck_assert_uint_eq(image.writes, 2);
    ck_assert_uint_eq(fixture_bam_entry(&image, format, 1)[0], persisted_count - 1u);
    ck_assert_uint_eq(fixture_bam_entry(&image, format, 1)[1] & 3u, 0);
    ck_assert(!unit.bam[0].dirty);
    ck_assert(!bank.units[0].slots[0].dirty);
    free(image.bytes);
}
END_TEST

// Explicit CLOSE 0 frees its page without MAPOUT, unlike a CLOSE 15 visit to SA 0.
START_TEST(test_direct_close_zero_bypasses_mapout_only_when_explicit) {
    const unsigned int format = (unsigned int)_i / 4u, variant = (unsigned int)_i % 4u;
    const bool readonly = variant % 2u != 0, close_all = variant >= 2u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    const uint8_t channel = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, "#");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    send_command(&unit, &bank, "B-A:0,1,0");
    finish_open(&unit, &bank);
    const uint8_t count = fixture_bam_entry(&image, format, 1)[0];
    if (readonly) {
        bank.units[0].slots[0].io.image.write = NULL;
    }
    ck_assert_int_eq(dos_close(&unit, &bank, model, close_all ? IEEE_COMMAND_CHANNEL : IEEE_LOAD_CHANNEL),
                     !(close_all && readonly));
    if (close_all && readonly) {
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_PROTECT);
        ck_assert_uint_eq(dos_find_channel(&unit, IEEE_LOAD_CHANNEL), channel);
    } else {
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        ck_assert_uint_eq(dos_find_channel(&unit, IEEE_LOAD_CHANNEL), DOS_UNUSED);
    }
    ck_assert_uint_eq(image.writes, close_all && !readonly ? 1u : 0u);
    ck_assert_uint_eq(image.flushes, close_all && !readonly ? 1u : 0u);
    ck_assert_uint_eq(fixture_bam_entry(&image, format, 1)[0], count - (close_all && !readonly ? 1u : 0u));

    // The skipped map remains available for a later ordinary direct CLOSE.
    if (!close_all || readonly) {
        bank.units[0].slots[0].io.image.write = write_image;
        if (!close_all) {
            ck_assert(unit.bam[0].dirty);
            ck_assert_uint_ne(open_name(&unit, &bank, 2, "#"), DOS_UNUSED);
        }
        ck_assert(dos_close(&unit, &bank, model, IEEE_COMMAND_CHANNEL));
        ck_assert_uint_eq(fixture_bam_entry(&image, format, 1)[0], count - 1u);
        ck_assert_uint_eq(image.writes, 1);
        ck_assert_uint_eq(image.flushes, 1);
    }
    free(image.bytes);
}
END_TEST

// GETRCH replaces a direct owner without CLOSE/MAPOUT, even for read-only images.
START_TEST(test_reopening_direct_owner_does_not_finalize_map) {
    const unsigned int format = (unsigned int)_i / 8u, variant = (unsigned int)_i % 8u;
    const unsigned int operation = variant / 2u;
    const uint8_t secondary = operation == 0 || operation == 3 ? IEEE_LOAD_CHANNEL : 2;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "READER", DISKIMAGE_FTYPE_PRG, 0x40, 4);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    if (operation == 3) {
        ck_assert_uint_ne(open_name(&unit, &bank, IEEE_LOAD_CHANNEL, "0:READER,P,R"), DOS_UNUSED);
        ck_assert(dos_close(&unit, &bank, model, IEEE_LOAD_CHANNEL));
    }
    ck_assert_uint_ne(open_name(&unit, &bank, secondary, "#"), DOS_UNUSED);
    send_command(&unit, &bank, "B-A:0,2,0");
    finish_open(&unit, &bank);
    const uint8_t count = fixture_bam_entry(&image, format, 2)[0];
    if (variant % 2u != 0) {
        bank.units[0].slots[0].io.image.write = NULL;
    }
    static const char* const names[] = {"$0:*", "$0", "0:READER,P,R", "*"};
    const uint8_t replacement = open_name(&unit, &bank, secondary, names[operation]);
    ck_assert_uint_ne(replacement, DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.channels[replacement].access, DOS_ACCESS_READ);
    if (operation >= 2) {
        expect_bytes(&unit, &bank, replacement, 4, 0x40);
    } else {
        uint8_t byte;
        bool eoi;
        ck_assert_uint_eq(dos_stage(&unit, &bank, replacement, &byte, 1, &eoi), 1);
    }
    ck_assert(unit.bam[0].dirty);
    ck_assert_uint_eq(fixture_bam_entry(&image, format, 2)[0], count);
    ck_assert_uint_eq(image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

// A stale RAM owner cannot MAPOUT a later generation's cached allocation state.
START_TEST(test_stale_direct_close_cannot_flush_current_map) {
    const unsigned int format = (unsigned int)_i / 3u, variant = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0}, other = {0};
    image_t images[3];
    mount_blank(&bank, &images[0], format);
    mount_peer(&bank, &images[1], format);
    mount_blank(&other, &images[2], format);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    const uint8_t direct = open_name(&unit, &bank, 2, "#");
    const dos_channel_t owner = unit.channels[direct];
    const dos_media_io_t unchanged = bank.units[0].slots[0].io;
    ck_assert_int_eq(
        dos_media_set_slot(&bank, 0, variant == 2 ? 1 : 0, variant == 0 ? &unchanged : &other.units[0].slots[0].io),
        DOS_MEDIA_OK);
    send_command(&unit, &bank, "B-A:0,2,0");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert(unit.bam[0].dirty);
    const dos_bam_t map = unit.bam[0];
    ck_assert(!dos_close(&unit, &bank, model, 2));
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    for (unsigned int index = 0; index < count_of(images); ++index) {
        ck_assert_uint_eq(images[index].writes + images[index].flushes, 0);
        free(images[index].bytes);
    }
    ck_assert_mem_eq(&unit.channels[direct], &owner, sizeof(owner));
    ck_assert_mem_eq(&unit.bam[0], &map, sizeof(map));
}
END_TEST

// BUFTST rejects allocated SA 0/1 before inspecting their listing/direct pages.
START_TEST(test_block_commands_reject_allocated_load_and_save) {
    static const char* const operations[] = {"B-P", "B-R", "B-W", "U1", "U2"};
    const unsigned int operation = (unsigned int)_i % count_of(operations);
    const uint8_t secondary = ((unsigned int)_i / count_of(operations)) % 2u;
    const unsigned int format = (unsigned int)_i / (2u * count_of(operations));
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t channel = open_name(&unit, &bank, secondary, secondary == IEEE_LOAD_CHANNEL ? "$0" : "#");
    const dos_channel_t before = unit.channels[channel];
    const unsigned int reads = image.reads, writes = image.writes;
    char request[DOS_COMMAND_RECEIVE_CAPACITY];
    snprintf(request, sizeof(request), operation == 0 ? "%s:%u,0" : "%s:%u,0,1,0", operations[operation], secondary);
    send_command(&unit, &bank, request);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
    ck_assert_mem_eq(&unit.channels[channel], &before, sizeof(before));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    free(image.bytes);
}
END_TEST

// U0 preserves live owners and command RAM, unlike the factory cold-reset vectors.
START_TEST(test_user_table_reset_and_cold_vectors) {
    const unsigned int format = (unsigned int)_i / 3u, operation = (unsigned int)_i % 3u;
    static const char* const commands[] = {"U0", "U:", "UJ"};
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    ck_assert_uint_ne(open_name(&unit, &bank, 3, "0:ABANDON,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 3, 0x40, 4);
    const uint8_t direct = open_name(&unit, &bank, 2, "#");
    ck_assert_uint_ne(direct, DOS_UNUSED);
    const uint8_t page = unit.channels[direct].buffers[0];
    memcpy(unit.buffers[page].bytes, "COLD", 4);
    unit.last_drive = unit.last_program_drive = 1;
    unit.last_program_track = 1;
    const dos_unit_t before = unit;
    const dos_media_bank_t bindings = bank;
    const unsigned int reads = image.reads, writes = image.writes, flushes = image.flushes;
    send_command(&unit, &bank, commands[operation]);
    ck_assert(!dos_work_pending(&unit));
    if (operation == 0) {
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        ck_assert_mem_eq(unit.channels, before.channels, sizeof(unit.channels));
        ck_assert_mem_eq(unit.buffers, before.buffers, sizeof(unit.buffers));
        ck_assert_mem_eq(unit.bam, before.bam, sizeof(unit.bam));
        ck_assert_uint_eq(unit.buffer_use, before.buffer_use);
        ck_assert_uint_eq(unit.last_drive, 1);
        ck_assert_mem_eq(unit.command_bytes, "U0", 2);
        send_command(&unit, &bank, "U");
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        ck_assert_uint_eq(dos_find_channel(&unit, 2), direct);
    } else {
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_POWER_ON);
        ck_assert_uint_eq(unit.buffer_use, 0);
        ck_assert_uint_eq(dos_find_channel(&unit, 2), DOS_UNUSED);
        ck_assert_uint_eq(dos_find_channel(&unit, 3), DOS_UNUSED);
        ck_assert_uint_eq(unit.last_drive, 0);
        ck_assert_uint_eq(unit.last_program_track, 0);
        const uint8_t cleared[DISKIMAGE_SECTOR_SIZE] = {0};
        ck_assert_mem_eq(unit.buffers[page].bytes, cleared, sizeof(cleared));
        ck_assert_uint_eq(unit.command_bytes[1], 0);
    }
    ck_assert_mem_eq(&bank, &bindings, sizeof(bank));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(image.flushes, flushes);
    free(image.bytes);
}
END_TEST

// Inspect persisted file bytes independently of the DOS streaming implementation.
static void expect_copy_payload(image_t* const image, const diskimage_type_t type, const char* const name,
                                const uint8_t* const expected, const unsigned int length, const uint8_t file_type) {
    uint8_t padded[DISKIMAGE_FILENAME_SIZE];
    memset(padded, DISKIMAGE_FILENAME_PADDING, sizeof(padded));
    memcpy(padded, name, strlen(name));
    const unsigned int directory = type == diskimage_type_d64 ? 18 : 39;
    uint8_t track = directory, sector = 1;
    const uint8_t* match = NULL;
    unsigned int visited = 0;
    while (track != 0 && match == NULL) {
        ck_assert_uint_lt(++visited, image->size / DISKIMAGE_SECTOR_SIZE);
        const uint8_t* const bytes = sector_bytes(image, type, track, sector);
        for (unsigned int offset = 0; offset < DISKIMAGE_SECTOR_SIZE; offset += 32u) {
            if (bytes[offset + 2u] != 0 && memcmp(bytes + offset + 5u, padded, sizeof(padded)) == 0) {
                match = bytes + offset;
                break;
            }
        }
        track = bytes[0];
        sector = bytes[1];
    }
    ck_assert_ptr_nonnull(match);
    ck_assert_uint_eq(match[2], 0x80u | file_type);
    track = match[3];
    sector = match[4];
    unsigned int position = 0;
    visited = 0;
    while (track != 0) {
        ck_assert_uint_lt(++visited, image->size / DISKIMAGE_SECTOR_SIZE);
        const uint8_t* const bytes = sector_bytes(image, type, track, sector);
        const unsigned int count = bytes[0] != 0 ? 254u : bytes[1] - 1u;
        ck_assert_uint_le(position + count, length);
        ck_assert_mem_eq(bytes + 2u, expected + position, count);
        position += count;
        track = bytes[0];
        sector = bytes[1];
    }
    ck_assert_uint_eq(position, length);
}

// DOS 2.7 LK25/COMPAR repeats an existing patterned target without further I/O.
static void expect_pattern_copy_stalled(dos_unit_t* const unit, dos_media_bank_t* const bank,
                                        const image_t* const target, const image_t* const source) {
    const unsigned int probe_quanta = 128;
    for (unsigned int step = 0; step < probe_quanta; ++step) {
        dos_work(unit, bank);
    }
    ck_assert(dos_work_pending(unit));
    ck_assert(unit->copy.pending);
    const unsigned int io =
        target->reads + target->writes + target->flushes + source->reads + source->writes + source->flushes;
    for (unsigned int step = 0; step < probe_quanta; ++step) {
        dos_work(unit, bank);
    }
    ck_assert_uint_eq(
        target->reads + target->writes + target->flushes + source->reads + source->writes + source->flushes, io);
}

// Native internal GET copies singleton and exact-sector payloads without IEEE EOF artifacts.
START_TEST(test_copy_preserves_boundary_payloads_and_peer_cursor) {
    static const unsigned int sizes[] = {0, 1, 253, 254, 255, 508, 509};
    const unsigned int format = (unsigned int)_i % 3u, size = sizes[(unsigned int)_i / 3u];
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:SOURCE,P,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x20, size);
    ck_assert(dos_close(&unit, &bank, model, 2));
    const uint8_t peer = open_name(&unit, &bank, 2, "0:SOURCE,P,R");
    const dos_channel_t before = unit.channels[peer];
    const uint16_t buffers = unit.buffer_use;
    const unsigned int reads = image.reads;
    send_command(&unit, &bank, "C0:NEW=0:SOURCE");
    ck_assert_uint_eq(image.reads, reads);
    ck_assert(dos_work_pending(&unit));
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.buffer_use, buffers);
    ck_assert_mem_eq(&unit.channels[peer], &before, sizeof(before));
    uint8_t expected[509];
    for (unsigned int index = 0; index < size; ++index) {
        expected[index] = (uint8_t)(0x20u + index);
    }
    if (size == 0) {
        expected[0] = '\r';
    }
    expect_copy_payload(&image, bank.units[0].slots[0].io.image.type, "NEW", expected, size == 0 ? 1 : size,
                        DISKIMAGE_FTYPE_PRG);
    free(image.bytes);
}
END_TEST

// COPY's structural errors and CHKIN-before-CHKIO ordering come from the ROM.
START_TEST(test_copy_parser_precedence_and_command_union_reuse) {
    static const struct {
        const char* command;
        dos_status_code_t status;
    } cases[] = {
        {"C0:NEW=0:SOURCE", DOS_STATUS_OK},
        {"C0:EXISTS=0:ABSENT", DOS_STATUS_NOT_FOUND},
        {"C0:EXISTS=0:SOURCE", DOS_STATUS_FILE_EXISTS},
        {"C0:SOURCE=0:SOURCE", DOS_STATUS_FILE_EXISTS},
        {"C0:BAD=0:S*", DOS_STATUS_SYNTAX},
        {"C0:BAD,OTHER=0:SOURCE", DOS_STATUS_SYNTAX},
        {"CFOO=SOURCE", DOS_STATUS_SYNTAX},
        {"C0:BAD", DOS_STATUS_SYNTAX},
        {"C0:BAD=", DOS_STATUS_NOT_FOUND},
        {"COPY0:NEW=SOURCE", DOS_STATUS_OK},
        {"C2:NEW=SOURCE", DOS_STATUS_OK},
        {"C:NEW=:SOURCE", DOS_STATUS_OK},
        {"C0:NEW=0:SOURCE=IGNORED", DOS_STATUS_OK},
        {"C0:NEW=0:SOURCE=X,0:ABSENT", DOS_STATUS_NOT_FOUND},
        {"C0:NEW=SOURCE,EXISTS=X,ABSENT", DOS_STATUS_NOT_FOUND},
        {"C0:SOURCE=SOURCE,EXISTS", DOS_STATUS_FILE_EXISTS},
        {"C0:NEW=SOURCE,ABSENT", DOS_STATUS_NOT_FOUND},
        {"C0:NEW=SOURCE,SOURCE,SOURCE,SOURCE,*", DOS_STATUS_OK},
        {"C,0:NEW=SOURCE", DOS_STATUS_NO_FILE},
    };
    const unsigned int format = (unsigned int)_i % 3u, which = (unsigned int)_i / 3u;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    rename_fixture(&unit, &bank);
    const unsigned int writes = image.writes;
    send_command(&unit, &bank, cases[which].command);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, cases[which].status);
    ck_assert_uint_eq(unit.buffer_use, 0);
    if (cases[which].status != DOS_STATUS_OK) {
        ck_assert_uint_eq(image.writes, writes);
    } else {
        send_command(&unit, &bank, "R0:RENAMED=NEW");
        finish_open(&unit, &bank);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        send_command(&unit, &bank, "S0:RENAMED");
        finish_open(&unit, &bank);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_FILES_SCRATCHED);
    }
    send_command(&unit, &bank, "C0:RECOVER=0:SOURCE");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.buffer_use, 0);
    const uint8_t expected[] = {0x20, 0x21, 0x22, 0x23};
    expect_copy_payload(&image, bank.units[0].slots[0].io.image.type, "RECOVER", expected, sizeof(expected),
                        DISKIMAGE_FTYPE_SEQ);
    free(image.bytes);
}
END_TEST

// New concatenation inherits the first type, while self-append requires explicit matching drives.
START_TEST(test_copy_concat_append_mixed_types_and_metadata) {
    const unsigned int format = (unsigned int)_i % 3u, kind = (unsigned int)_i / 3u;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const char* const name = kind == 0 ? "0:SOURCE,S,W" : kind == 1 ? "0:SOURCE,U,W" : "0:SOURCE,P,W";
    ck_assert_uint_ne(open_name(&unit, &bank, 2, name), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x20, 254);
    ck_assert(dos_close(&unit, &bank, model, 2));
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:OTHER,P,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x60, 2);
    ck_assert(dos_close(&unit, &bank, model, 2));
    send_command(&unit, &bank, "C0:NEW=0:SOURCE,0:OTHER");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    uint8_t expected[256];
    for (unsigned int index = 0; index < 254u; ++index) {
        expected[index] = (uint8_t)(0x20u + index);
    }
    expected[254] = 0x60;
    expected[255] = 0x61;
    const uint8_t type = kind == 0 ? DISKIMAGE_FTYPE_SEQ : kind == 1 ? DISKIMAGE_FTYPE_USR : DISKIMAGE_FTYPE_PRG;
    expect_copy_payload(&image, bank.units[0].slots[0].io.image.type, "NEW", expected, sizeof(expected), type);
    send_command(&unit, &bank, "C0:SOURCE=0:SOURCE,0:OTHER");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, kind == 2 ? DOS_STATUS_TYPE_MISMATCH : DOS_STATUS_OK);
    expect_copy_payload(&image, bank.units[0].slots[0].io.image.type, "SOURCE", expected, kind == 2 ? 254 : 256, type);
    const uint8_t* const directory =
        sector_bytes(&image, bank.units[0].slots[0].io.image.type, format == 0 ? 18u : 39u, 1);
    ck_assert_uint_eq(directory[30], kind == 2 ? 1 : 3);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// Failed internal jobs must not leak channels or become host-finalized partial files.
START_TEST(test_copy_faults_abandon_only_owned_channels) {
    const unsigned int format = (unsigned int)_i % 3u, fault = (unsigned int)_i / 3u;
    image_t image, replacement = {0};
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    rename_fixture(&unit, &bank);
    const uint8_t peer = open_name(&unit, &bank, 2, "0:EXISTS,S,R");
    const dos_channel_t before = unit.channels[peer];
    const uint16_t buffers = unit.buffer_use;
    send_command(&unit, &bank, "C0:NEW=0:SOURCE");
    if (fault == 0) {
        image.fail_read = image.reads + 1u;
    } else if (fault == 1) {
        image.fail_write = image.writes + 1u;
    } else if (fault == 2) {
        image.fail_flush = true;
    } else {
        mount_blank(&bank, &replacement, format);
    }
    finish_open(&unit, &bank);
    ck_assert_uint_ne(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.buffer_use, buffers);
    ck_assert_mem_eq(&unit.channels[peer], &before, sizeof(before));
    const unsigned int writes = image.writes + replacement.writes;
    uint8_t directory[DISKIMAGE_SECTOR_SIZE];
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    memcpy(directory, sector_bytes(&image, type, format == 0 ? 18 : 39, 1), sizeof(directory));
    // Landed finalization persists orphaned BAM allocations and rejects
    // persistent flush failures or stale generations, but never CLOSEs COPY.
    ck_assert_int_eq(dos_finalize(&unit, &bank), fault < 2);
    ck_assert_uint_eq(image.writes + replacement.writes, writes + (fault == 1));
    ck_assert_mem_eq(sector_bytes(&image, type, format == 0 ? 18 : 39, 1), directory, sizeof(directory));
    if (fault == 2) {
        image.fail_flush = false;
        ck_assert(dos_finalize(&unit, &bank));
        ck_assert_mem_eq(sector_bytes(&image, type, format == 0 ? 18 : 39, 1), directory, sizeof(directory));
    }
    free(replacement.bytes);
    free(image.bytes);
}
END_TEST

// Resource pressure and media policies must release only the internal COPY claims.
START_TEST(test_copy_resource_read_only_full_and_corrupt_recovery) {
    const unsigned int format = (unsigned int)_i % 3u, mode = (unsigned int)_i / 3u;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    rename_fixture(&unit, &bank);
    const unsigned int directory = format == 0 ? 18 : 39;
    if (mode < 2u) {
        for (uint8_t secondary = 2; secondary < 6u + mode; ++secondary) {
            ck_assert_uint_ne(open_name(&unit, &bank, secondary, "0:EXISTS,S,R"), DOS_UNUSED);
        }
    } else if (mode == 2) {
        bank.units[0].slots[0].io.image.write = NULL;
    } else if (mode == 3) {
        limit_free_blocks(&image, format, 0);
        unit.bam[0].buffer.resident = false;
        unit.free_blocks_valid[0] = false;
    } else {
        uint8_t* const bytes = sector_bytes(&image, bank.units[0].slots[0].io.image.type, directory, 1);
        bytes[0] = directory;
        bytes[1] = 1;
    }
    const dos_status_code_t expected[] = {DOS_STATUS_OK, DOS_STATUS_NO_CHANNEL, DOS_STATUS_WRITE_PROTECT,
                                          format == 0 ? DOS_STATUS_DISK_FULL : DOS_STATUS_ILLEGAL_SYSTEM_TRACK_SECTOR,
                                          DOS_STATUS_DIRECTORY_ERROR};
    const uint16_t buffers = unit.buffer_use;
    const unsigned int writes = image.writes;
    send_command(&unit, &bank, "C0:NEW=0:SOURCE");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, expected[mode]);
    ck_assert_uint_eq(unit.buffer_use, buffers);
    if (mode != 0) {
        ck_assert_uint_eq(image.writes, writes);
    }
    if (mode < 2u) {
        expect_bytes(&unit, &bank, dos_find_channel(&unit, 2), 2, 0x60);
    }
    free(image.bytes);
}
END_TEST

// Mixed inherited defaults may require LOOKUP's third directory pass before creation.
START_TEST(test_copy_cross_drive_defaults_use_native_third_search) {
    const unsigned int format = (unsigned int)_i;
    image_t first, second;
    dos_media_bank_t bank = {0}, other = {0};
    mount_blank(&bank, &first, format);
    mount_blank(&other, &second, format);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    for (const char* name = "ABC"; *name != 0; ++name) {
        char request[16];
        snprintf(request, sizeof(request), "%u:%c,S,W", *name == 'B' ? 1u : 0u, *name);
        ck_assert_uint_ne(open_name(&unit, &bank, 2, request), DOS_UNUSED);
        put_bytes(&unit, &bank, 2, 0x20, 4);
        ck_assert(dos_close(&unit, &bank, model, 2));
    }
    send_command(&unit, &bank, "C0:NEW=A,1:B,C");
    ck_assert_uint_eq(unit.copy.scans, 2);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.last_drive, 0);
    ck_assert_uint_eq(unit.buffer_use, 0);
    const uint8_t expected[] = {0x20, 0x21, 0x22, 0x23, 0x20, 0x21, 0x22, 0x23, 0x20, 0x21, 0x22, 0x23};
    expect_copy_payload(&first, bank.units[0].slots[0].io.image.type, "NEW", expected, sizeof(expected),
                        DISKIMAGE_FTYPE_SEQ);
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// PRSEQ uses adjacent drive bytes, ignores other text and copies every source entry.
START_TEST(test_copy_all_aliases_preserve_source_and_peer) {
    static const char* const commands[] = {"C0=1",
                                           "COPY0=1TRAIL",
                                           "CFOO0=1",
                                           "C,0=1",
                                           "C0=1,,,,,IGNORED",
                                           "C0:*=1:*",
                                           "C0:*IGNORED=1:*",
                                           "C0:*=1:*12345678901234567890"};
    const unsigned int format = (unsigned int)_i / count_of(commands);
    dos_media_bank_t bank = {0}, source_bank = {0};
    image_t target, source;
    mount_blank(&bank, &target, format);
    mount_reader_pointer_image(&source_bank, &source, format);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    uint8_t* const directory = sector_bytes(&source, type, format == 0 ? 18 : 39, 1);
    directory[2] |= 0x40;
    directory[30] = 77;
    directory[34] = 0x81;
    dos_media_io_t readonly = source_bank.units[0].slots[0].io;
    readonly.image.write = NULL;
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &readonly), DOS_MEDIA_OK);
    uint8_t* const before = malloc(source.size);
    ck_assert_ptr_nonnull(before);
    memcpy(before, source.bytes, source.size);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t peer = open_name(&unit, &bank, 3, "1:READ,P,R");
    const uint8_t prefix[] = {0, 1, 2}, suffix[] = {3, 4, 5};
    expect_direct(&unit, &bank, peer, prefix, sizeof(prefix), false);
    const unsigned int reads = source.reads + target.reads;
    send_command(&unit, &bank, commands[(unsigned int)_i % count_of(commands)]);
    ck_assert_uint_eq(source.reads + target.reads, reads);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.last_drive, 1);
    uint8_t payload[261];
    for (unsigned int index = 0; index < sizeof(payload); ++index) {
        payload[index] = index;
    }
    expect_copy_payload(&target, type, "READ", payload, sizeof(payload), DISKIMAGE_FTYPE_PRG);
    const uint8_t a = 'A';
    expect_copy_payload(&target, type, "ONE", &a, 1, DISKIMAGE_FTYPE_SEQ);
    expect_direct(&unit, &bank, peer, suffix, sizeof(suffix), false);
    ck_assert_mem_eq(source.bytes, before, source.size);
    ck_assert_uint_eq(source.writes + source.flushes, 0);
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 3));
    ck_assert_uint_eq(unit.buffer_use, 0);
    const unsigned int writes = target.writes;
    send_command(&unit, &bank, "C0=1");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_FILE_EXISTS);
    ck_assert_uint_eq(target.writes, writes);
    free(before);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// CPYDTD retains the source pattern while FIXIT substitutes each matched target name.
START_TEST(test_copy_pattern_selects_only_matching_files_and_preserves_peer) {
    static const char* const commands[] = {"C0:*IGNORED=1:R?AD", "C0:*=1:R*IGNORED", "C*0:*=1:READ", "COPY0:*=1:READ",
                                           "C0:*?=1:READ",       "C0:*=1:REA?",      "C0:*=1:READ"};
    const unsigned int format = (unsigned int)_i / count_of(commands);
    dos_media_bank_t bank = {0}, source_bank = {0};
    image_t target, source;
    mount_blank(&bank, &target, format);
    mount_reader_pointer_image(&source_bank, &source, format);
    dos_media_io_t readonly = source_bank.units[0].slots[0].io;
    readonly.image.write = NULL;
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &readonly), DOS_MEDIA_OK);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t peer = open_name(&unit, &bank, 3, "1:READ,P,R");
    const uint8_t prefix[] = {0, 1}, suffix[] = {2, 3};
    expect_direct(&unit, &bank, peer, prefix, sizeof(prefix), false);
    const unsigned int reads = source.reads + target.reads;
    send_command(&unit, &bank, commands[(unsigned int)_i % count_of(commands)]);
    ck_assert_uint_eq(source.reads + target.reads, reads);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.last_drive, 1);
    uint8_t payload[261];
    for (unsigned int index = 0; index < sizeof(payload); ++index) {
        payload[index] = index;
    }
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    expect_copy_payload(&target, type, "READ", payload, sizeof(payload), DISKIMAGE_FTYPE_PRG);
    ck_assert_uint_eq(sector_bytes(&target, type, format == 0 ? 18 : 39, 1)[32 + 2], 0);
    expect_direct(&unit, &bank, peer, suffix, sizeof(suffix), false);
    ck_assert_uint_eq(source.writes + source.flushes, 0);
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 3));
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// FFST exhaustion is successful when no source filename matches the pattern.
START_TEST(test_copy_pattern_without_matches_does_not_create_files) {
    static const char* const patterns[] = {"NONE*", "READ?", "?????", "?"};
    const unsigned int format = (unsigned int)_i / count_of(patterns);
    dos_media_bank_t bank = {0}, source_bank = {0};
    image_t target, source;
    mount_blank(&bank, &target, format);
    mount_reader_pointer_image(&source_bank, &source, format);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &source_bank.units[0].slots[0].io), DOS_MEDIA_OK);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    char command[32];
    snprintf(command, sizeof(command), "C0:*=1:%s", patterns[(unsigned int)_i % count_of(patterns)]);
    send_command(&unit, &bank, command);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.last_drive, 1);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(source.writes + source.flushes + target.writes + target.flushes, 0);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// DOS 2.7 compares full ALLDRS bytes, unlike 4040, so a default flag bypasses equality.
START_TEST(test_copy_pattern_same_drive_and_default_flag_are_model_specific) {
    static const char* const commands[] = {"C0:*=0:READ", "C0:*=READ", "C0:*=x:READ", "C0:*=0:NONE"};
    const unsigned int format = (unsigned int)_i / count_of(commands);
    const unsigned int variant = (unsigned int)_i % count_of(commands);
    dos_media_bank_t bank = {0};
    image_t source;
    mount_reader_pointer_image(&bank, &source, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, commands[variant]);
    ck_assert_uint_eq(source.reads + source.writes, 0);
    const bool rejected = format != 0 && variant != 1;
    if (format != 0 && variant == 1) {
        expect_pattern_copy_stalled(&unit, &bank, &source, &source);
        dos_reset(&unit, bank.units[0].model);
    } else {
        finish_open(&unit, &bank);
        ck_assert_uint_eq(unit.status_code, rejected       ? DOS_STATUS_SYNTAX
                                            : variant == 3 ? DOS_STATUS_OK
                                                           : DOS_STATUS_FILE_EXISTS);
    }
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(source.writes + source.flushes, 0);
    free(source.bytes);
}
END_TEST

// CPYDTD requires a leading target star and a single source, not a source group.
START_TEST(test_copy_pattern_rejects_nonstar_targets_and_source_groups) {
    static const char* const commands[] = {
        "C0:B*=1:READ", "C0:?=1:READ",      "C0:*=1:READ,1:ONE", "C0:*,X=1:READ",
        "C0:*",         "C0:*?=1:READ,ONE", "C0:*=1:",           "C0:*=1:*123456789012345678901"};
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, (unsigned int)_i / count_of(commands));
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, commands[(unsigned int)_i % count_of(commands)]);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_SYNTAX);
    ck_assert(!dos_work_pending(&unit));
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(image.reads + image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

// Missing, invalid and equal drive selectors fail before borrowing buffers or issuing I/O.
START_TEST(test_copy_all_rejects_invalid_drive_syntax) {
    static const char* const commands[] = {"C",    "C0",   "C0=",      "C0=0",     "C1=1",
                                           "C2=1", "C0=2", "CFOO=ONE", "C,,,,0=1", "C0=1:JUNK"};
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, (unsigned int)_i / count_of(commands));
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, commands[(unsigned int)_i % count_of(commands)]);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_SYNTAX);
    ck_assert(!dos_work_pending(&unit));
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(image.reads + image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

// A later existing destination stops iteration without undoing completed earlier files.
START_TEST(test_copy_all_keeps_completed_prefix_and_recovers) {
    const unsigned int format = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0}, source_bank = {0};
    image_t target, source;
    mount_reader_pointer_image(&bank, &target, format);
    mount_reader_pointer_image(&source_bank, &source, format);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    sector_bytes(&target, type, format == 0 ? 18 : 39, 1)[2] = 0;
    sector_bytes(&target, type, 1, 2)[2] = 'B';
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &source_bank.units[0].slots[0].io), DOS_MEDIA_OK);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, _i < 3 ? "C0=1" : "C0:*=1:*");
    const bool stalls = _i >= 3 && format != 0;
    if (stalls) {
        expect_pattern_copy_stalled(&unit, &bank, &target, &source);
    } else {
        finish_open(&unit, &bank);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_FILE_EXISTS);
        ck_assert_uint_eq(unit.buffer_use, 0);
    }
    ck_assert_uint_eq(unit.last_drive, 0);
    uint8_t payload[261];
    for (unsigned int index = 0; index < sizeof(payload); ++index) {
        payload[index] = index;
    }
    expect_copy_payload(&target, type, "READ", payload, sizeof(payload), DISKIMAGE_FTYPE_PRG);
    const uint8_t b = 'B', a = 'A';
    expect_copy_payload(&target, type, "ONE", &b, 1, DISKIMAGE_FTYPE_PRG);
    if (stalls) {
        dos_reset(&unit, bank.units[0].model);
        ck_assert_uint_eq(unit.buffer_use, 0);
    }
    send_command(&unit, &bank, "C0:AFTER=1:ONE");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    expect_copy_payload(&target, type, "AFTER", &a, 1, DISKIMAGE_FTYPE_PRG);
    ck_assert_uint_eq(source.writes + source.flushes, 0);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// Empty source iteration succeeds, while a failed source read releases only owned work.
START_TEST(test_copy_all_empty_source_and_read_failure) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool fail = (unsigned int)_i % 2u != 0;
    dos_media_bank_t bank = {0}, source_bank = {0};
    image_t target, source;
    mount_blank(&bank, &target, format);
    mount_blank(&source_bank, &source, format);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &source_bank.units[0].slots[0].io), DOS_MEDIA_OK);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    if (fail) {
        source.fail_read = 1;
    }
    send_command(&unit, &bank, "C0=1");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, fail ? DOS_STATUS_READ_ERROR : DOS_STATUS_OK);
    ck_assert_uint_eq(unit.last_drive, fail ? 0 : 1);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(target.writes + target.flushes + source.writes + source.flushes, 0);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// PRSEQ accepts either drive direction and can read a retained byte beyond CMDSIZ.
START_TEST(test_copy_all_reverse_direction_and_retained_selector) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0}, target_bank = {0};
    image_t target, source;
    mount_reader_pointer_image(&bank, &source, format);
    mount_blank(&target_bank, &target, format);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &target_bank.units[0].slots[0].io), DOS_MEDIA_OK);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, "C1=0");
    ck_assert(unit.copy.pending);
    ck_assert_uint_eq(source.reads + target.reads + source.writes + target.writes, 0);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.last_drive, 0);
    const uint8_t expected[] = {'A'};
    expect_copy_payload(&target, bank.units[0].slots[0].io.image.type, "ONE", expected, sizeof(expected),
                        DISKIMAGE_FTYPE_PRG);
    const unsigned int writes = target.writes;
    send_command(&unit, &bank, "U0X0");
    send_command(&unit, &bank, "C1=");
    ck_assert(unit.copy.pending);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_FILE_EXISTS);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(target.writes, writes);
    ck_assert_uint_eq(source.writes + source.flushes, 0);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// FFRE resumes across source pages while independent target creation grows its directory.
START_TEST(test_copy_all_crosses_source_and_target_directory_pages) {
    const unsigned int format = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0}, source_bank = {0};
    image_t target, source;
    mount_blank(&bank, &target, format);
    mount_reader_pointer_image(&source_bank, &source, format);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    const uint8_t root = format == 0 ? 18 : 39;
    uint8_t* const first = sector_bytes(&source, type, root, 1);
    first[0] = root;
    first[1] = 2;
    uint8_t* const second = sector_bytes(&source, type, root, 2);
    memset(second, 0, DISKIMAGE_SECTOR_SIZE);
    second[1] = 255;
    for (unsigned int index = 0; index < 8; ++index) {
        const unsigned int slot = index + 2u;
        uint8_t* const entry = (slot < 8 ? first : second) + (slot % 8u) * 32u;
        entry[2] = 0x83;
        entry[3] = 1;
        entry[4] = index + 3u;
        memset(entry + 5, 0xa0, DISKIMAGE_FILENAME_SIZE);
        entry[5] = 'F';
        entry[6] = '0' + index;
        entry[30] = 1;
        uint8_t* const bytes = sector_bytes(&source, type, 1, index + 3u);
        bytes[0] = 0;
        bytes[1] = 3;
        bytes[2] = 0x80u + index;
        bytes[3] = 0xf0;
    }
    // Reserve each new raw data page and the linked directory page in the fixture BAM.
    for (unsigned int index = 0; index < 9; ++index) {
        const unsigned int sector = index < 8 ? index + 3u : 2;
        uint8_t* const bam = fixture_bam_entry(&source, format, index < 8 ? 1 : root);
        const uint8_t bit = 1u << (sector % 8u);
        ck_assert_uint_ne(bam[1 + sector / 8u] & bit, 0);
        --bam[0];
        bam[1 + sector / 8u] &= ~bit;
    }
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &source_bank.units[0].slots[0].io), DOS_MEDIA_OK);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, _i < 3 ? "C0=1" : "C0:*=1:F?");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.copy.all_visited, 2);
    ck_assert_uint_eq(unit.buffer_use, 0);
    for (unsigned int index = 0; index < 8; ++index) {
        const char name[] = {'F', '0' + index, 0};
        const uint8_t expected[] = {0x80u + index, 0xf0};
        expect_copy_payload(&target, type, name, expected, sizeof(expected), DISKIMAGE_FTYPE_USR);
    }
    ck_assert_uint_eq(source.writes + source.flushes, 0);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// COPY must preserve the landed OPEN/RENAME reservation for unpublished names.
START_TEST(test_copy_respects_unpublished_writer_namespace) {
    const unsigned int format = (unsigned int)_i / 3u, scenario = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0};
    image_t target, source;
    mount_blank(&bank, &target, format);
    if (scenario == 2) {
        mount_peer(&bank, &source, format);
        fixture_file(&source, format, 0, "NEW", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    } else {
        fixture_file(&target, format, 0, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    }
    const dos_model_t model = bank.units[0].model;
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    const uint8_t track = format == 0 ? 18 : 39;
    if (scenario == 1) {
        uint8_t* const directory = sector_bytes(&target, type, track, 1);
        memcpy(sector_bytes(&target, type, track, 2), directory, DISKIMAGE_SECTOR_SIZE);
        memset(directory, 0, DISKIMAGE_SECTOR_SIZE);
        directory[0] = track;
        directory[1] = 2;
    }
    dos_unit_t unit;
    dos_reset(&unit, model);
    target.fail_write = 1;
    target.fail_write_without_changes = true;
    const uint8_t owner = open_name(&unit, &bank, 2, "0:NEW,S,W");
    ck_assert_uint_ne(owner, DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    const dos_channel_t saved = unit.channels[owner];
    send_command(&unit, &bank, "C0:NEW=0:ABSENT");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NOT_FOUND);
    const unsigned int writes = target.writes;
    send_command(&unit, &bank, scenario == 2 ? "C0:*=1:*" : "C0:NEW=0:SOURCE");
    if (scenario == 1) {
        dos_work(&unit, &bank);
        ck_assert(dos_work_pending(&unit));
        ck_assert(dos_close(&unit, &bank, model, 2));
    }
    const unsigned int before_copy = target.writes;
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(target.writes, before_copy);
    if (scenario != 1) {
        ck_assert_uint_eq(target.writes, writes);
        ck_assert_mem_eq(&unit.channels[owner], &saved, sizeof(saved));
        ck_assert(dos_close(&unit, &bank, model, 2));
    }
    const uint8_t reader = open_name(&unit, &bank, 3, "0:NEW");
    expect_bytes(&unit, &bank, reader, 0, 0);
    if (scenario == 2) {
        ck_assert_uint_eq(source.writes + source.flushes, 0);
        free(source.bytes);
    }
    free(target.bytes);
}
END_TEST

// A literal $A0 in the saved pattern is not the end of its parsed name.
START_TEST(test_copy_pattern_retains_literal_padding_byte) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t target, source;
    mount_blank(&bank, &target, format);
    mount_peer(&bank, &source, format);
    fixture_file(&source, format, 0, "R\xa0X", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    fixture_file(&source, format, 1, "R", DISKIMAGE_FTYPE_SEQ, 0x60, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, "C0:*=1:R\xa0?");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    uint8_t expected[22];
    for (unsigned int index = 0; index < sizeof(expected); ++index) {
        expected[index] = 0x20u + index;
    }
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    expect_copy_payload(&target, type, "R\xa0X", expected, sizeof(expected), DISKIMAGE_FTYPE_SEQ);
    ck_assert_uint_eq(sector_bytes(&target, type, format == 0 ? 18 : 39, 1)[DISKIMAGE_DIRECTORY_ENTRY_SIZE + 2], 0);
    ck_assert_uint_eq(source.writes + source.flushes, 0);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// COPY's OPREAD masks high directory-track bits on 4040, not DOS 2.7.
START_TEST(test_copy_decodes_model_specific_source_track) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t target, source;
    mount_blank(&bank, &target, format);
    mount_peer(&bank, &source, format);
    fixture_file(&source, format, 0, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    sector_bytes(&source, type, format == 0 ? 18 : 39, 1)[3] |= format == 0 ? 0x40u : 0xc0u;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, "C0:NEW=1:SOURCE");
    finish_open(&unit, &bank);
    if (format == 0) {
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        uint8_t expected[22];
        for (unsigned int index = 0; index < sizeof(expected); ++index) {
            expected[index] = 0x20u + index;
        }
        expect_copy_payload(&target, type, "NEW", expected, sizeof(expected), DISKIMAGE_FTYPE_SEQ);
    } else {
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
        ck_assert_uint_eq(sector_bytes(&target, type, 39, 1)[2], DISKIMAGE_FTYPE_SEQ);
    }
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(source.writes + source.flushes, 0);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// COPY must not reuse a different unpublished owner's deleted slot or growth links.
START_TEST(test_copy_preserves_unpublished_directory_coordinates) {
    const unsigned int format = (unsigned int)_i / 6u, variant = (unsigned int)_i % 6u;
    const bool growth = variant >= 3;
    dos_media_bank_t bank = {0};
    image_t target, source;
    mount_blank(&bank, &target, format);
    mount_peer(&bank, &source, format);
    fixture_file(&source, format, 0, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    if (growth) {
        fill_directory(&target, format);
    }
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    target.fail_write = 1;
    target.fail_write_without_changes = true;
    const uint8_t held = open_name(&unit, &bank, 2, "0:HELD,S,W");
    ck_assert_uint_ne(held, DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    const dos_channel_t before = unit.channels[held];
    static const char* const commands[] = {"C0:NEW=1:SOURCE", "C0=1", "C0:*=1:*"};
    send_command(&unit, &bank, commands[variant % count_of(commands)]);
    const unsigned int writes = target.writes;
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, growth ? DOS_STATUS_WRITE_ERROR : DOS_STATUS_OK);
    ck_assert_mem_eq(&unit.channels[held], &before, sizeof(before));
    if (growth) {
        ck_assert_uint_eq(target.writes, writes);
    }
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    if (growth) {
        send_command(&unit, &bank, commands[variant % count_of(commands)]);
        finish_open(&unit, &bank);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    }
    uint8_t expected[22];
    for (unsigned int index = 0; index < sizeof(expected); ++index) {
        expected[index] = 0x20u + index;
    }
    expect_copy_payload(&target, bank.units[0].slots[0].io.image.type, variant % 3u == 0 ? "NEW" : "SOURCE", expected,
                        sizeof(expected), DISKIMAGE_FTYPE_SEQ);
    const uint8_t reader = open_name(&unit, &bank, 3, "0:HELD");
    expect_bytes(&unit, &bank, reader, 0, 0);
    ck_assert_uint_eq(source.writes + source.flushes, 0);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// The copy-all source iterator must honor the same incomplete-growth guard as lookup.
START_TEST(test_copy_all_rejects_pending_source_directory_growth) {
    const unsigned int format = (unsigned int)_i / 2u;
    dos_media_bank_t bank = {0};
    image_t target, source;
    mount_blank(&bank, &target, format);
    mount_peer(&bank, &source, format);
    fill_directory(&source, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    source.fail_write = 1;
    source.fail_write_without_changes = true;
    const uint8_t owner = open_name(&unit, &bank, 2, "1:HELD,S,W");
    ck_assert_uint_ne(owner, DOS_UNUSED);
    ck_assert(unit.channels[owner].grow_pending);
    const dos_channel_t before = unit.channels[owner];
    const unsigned int writes = source.writes;
    send_command(&unit, &bank, _i % 2 ? "C0:*=1:*" : "C0=1");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(target.writes + target.flushes, 0);
    ck_assert_uint_eq(source.writes, writes);
    ck_assert_mem_eq(&unit.channels[owner], &before, sizeof(before));
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// OPREAD discards the directory successor job when it reopens IRSA for file data.
START_TEST(test_copy_self_append_discards_unused_directory_error) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "A", DISKIMAGE_FTYPE_SEQ, 0x20, 4);
    fixture_file(&image, format, 1, "B", DISKIMAGE_FTYPE_SEQ, 0x60, 2);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    const unsigned int track = format == 0 ? 18u : 39u;
    uint8_t* const directory = sector_bytes(&image, type, track, 1);
    directory[0] = track;
    directory[1] = 2;
    ck_assert(diskimage_sector_offset(type, track, 2, &image.fail_read_offset));
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, "C0:A=0:A,0:B");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.buffer_use, 0);
    const uint8_t expected[] = {0x20, 0x21, 0x22, 0x23, 0x60, 0x61};
    expect_copy_payload(&image, type, "A", expected, sizeof(expected), DISKIMAGE_FTYPE_SEQ);
    free(image.bytes);
}
END_TEST

// FFRE reopens the current directory page after CY, discarding the old speculative job.
START_TEST(test_copy_all_defers_directory_error_and_reopens_jobs) {
    const unsigned int format = (unsigned int)_i % 3u;
    const bool persistent = (unsigned int)_i / 6u != 0;
    dos_media_bank_t bank = {0};
    image_t target, source;
    mount_blank(&bank, &target, format);
    mount_peer(&bank, &source, format);
    fixture_file(&source, format, 0, "A", DISKIMAGE_FTYPE_SEQ, 0x20, 4);
    fixture_file(&source, format, 1, "B", DISKIMAGE_FTYPE_SEQ, 0x60, 2);
    fixture_file(&source, format, 2, "C", DISKIMAGE_FTYPE_SEQ, 0x80, 3);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    const unsigned int track = format == 0 ? 18u : 39u;
    uint8_t* const directory = sector_bytes(&source, type, track, 1);
    memcpy(sector_bytes(&source, type, track, 2), directory + 64, DISKIMAGE_DIRECTORY_ENTRY_SIZE);
    directory[66] = 0;
    directory[0] = track;
    directory[1] = 2;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, (unsigned int)_i % 6u < 3u ? "C0=1" : "C0:*=1:*");
    if (persistent) {
        ck_assert(diskimage_sector_offset(type, track, 2, &source.fail_read_offset));
    } else {
        // Only the first prefetch fails. A later FFRE owns a new, healthy job.
        source.fail_read = source.reads + 2u;
    }
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, persistent ? DOS_STATUS_READ_ERROR : DOS_STATUS_OK);
    ck_assert_uint_eq(unit.buffer_use, 0);
    const uint8_t a[] = {0x20, 0x21, 0x22, 0x23}, b[] = {0x60, 0x61}, c[] = {0x80, 0x81, 0x82};
    expect_copy_payload(&target, type, "A", a, sizeof(a), DISKIMAGE_FTYPE_SEQ);
    expect_copy_payload(&target, type, "B", b, sizeof(b), DISKIMAGE_FTYPE_SEQ);
    if (persistent) {
        ck_assert_uint_eq(sector_bytes(&target, type, track, 1)[66], 0);
    } else {
        expect_copy_payload(&target, type, "C", c, sizeof(c), DISKIMAGE_FTYPE_SEQ);
    }
    ck_assert_uint_eq(source.writes + source.flushes, 0);
    source.fail_read_offset = 0;
    send_command(&unit, &bank, "C0:AFTER=1:A");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    expect_copy_payload(&target, type, "AFTER", a, sizeof(a), DISKIMAGE_FTYPE_SEQ);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// Named lookup stops at one required padding byte, while TRFNME transfers all 16 bytes.
START_TEST(test_copy_padded_name_lookup_and_full_transferred_target) {
    const unsigned int format = (unsigned int)_i / 3u, variant = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0};
    image_t target, source;
    mount_blank(&bank, &target, format);
    mount_peer(&bank, &source, format);
    fixture_file(&source, format, 0, "R\xa0X", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    if (variant != 0) {
        fixture_file(&target, format, 0, variant == 1 ? "N\xa0X" : "R\xa0Y", DISKIMAGE_FTYPE_SEQ, 0x60, 22);
    }
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const unsigned int writes = target.writes;
    send_command(&unit, &bank, variant == 0 ? "C0:NEW=1:R" : variant == 1 ? "C0:N=1:R" : "C0=1");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, variant == 1 ? DOS_STATUS_FILE_EXISTS : DOS_STATUS_OK);
    uint8_t expected[22];
    for (unsigned int index = 0; index < sizeof(expected); ++index) {
        expected[index] = 0x20u + index;
    }
    if (variant == 1) {
        ck_assert_uint_eq(target.writes, writes);
    } else {
        expect_copy_payload(&target, bank.units[0].slots[0].io.image.type, variant == 0 ? "NEW" : "R\xa0X", expected,
                            sizeof(expected), DISKIMAGE_FTYPE_SEQ);
    }
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// A 4040 raw bit-7 match repeats LOOKUP without reaching OPREAD or creating output.
START_TEST(test_4040_copy_bit7_match_remains_pending) {
    const unsigned int variant = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t target, source;
    mount_blank(&bank, &target, 0);
    mount_peer(&bank, &source, 0);
    fixture_file(&source, 0, 0, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    if (variant >= 2) {
        fixture_file(&target, 0, 0, "NEW", DISKIMAGE_FTYPE_SEQ, 0x60, 22);
    }
    image_t* const matched = variant >= 2 ? &target : &source;
    sector_bytes(matched, diskimage_type_d64, 18, 1)[3] |= variant % 2u == 0 ? 0x80u : 0xc0u;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, "C0:NEW=1:SOURCE");
    for (unsigned int step = 0; step < 20; ++step) {
        ck_assert(dos_work_pending(&unit));
        dos_work(&unit, &bank);
    }
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(source.writes + source.flushes + target.writes + target.flushes, 0);
    const unsigned int reads = source.reads + target.reads;
    for (unsigned int step = 0; step < 20; ++step) {
        dos_work(&unit, &bank);
    }
    ck_assert(dos_work_pending(&unit));
    ck_assert_uint_eq(source.reads + target.reads, reads);
    dos_reset(&unit, bank.units[0].model);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// GIBYTE waits for a successor before PIBYTE can persist the current block's final byte.
START_TEST(test_copy_read_failure_preserves_native_partial_chain) {
    const unsigned int format = (unsigned int)_i / 2u, failed_block = (unsigned int)_i % 2u + 2u;
    dos_media_bank_t bank = {0}, source_bank = {0};
    image_t target, source;
    mount_blank(&bank, &target, format);
    mount_blank(&source_bank, &source, format);
    dos_unit_t producer;
    dos_reset(&producer, source_bank.units[0].model);
    ck_assert_uint_ne(open_name(&producer, &source_bank, 2, "0:SOURCE,S,W"), DOS_UNUSED);
    put_bytes(&producer, &source_bank, 2, 0x20, 700);
    ck_assert(dos_close(&producer, &source_bank, source_bank.units[0].model, 2));
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &source_bank.units[0].slots[0].io), DOS_MEDIA_OK);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, "C0:NEW=1:SOURCE");
    for (unsigned int step = 0; unit.copy.writer == DOS_UNUSED; ++step) {
        ck_assert_uint_lt(step, 10);
        ck_assert(dos_work_pending(&unit));
        dos_work(&unit, &bank);
    }
    const unsigned int writes = target.writes;
    source.fail_read = source.reads + failed_block + 1u;
    dos_work(&unit, &bank);
    ck_assert(dos_work_pending(&unit));
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_uint_eq(target.writes, writes + failed_block - 2u);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    const uint8_t* const entry = sector_bytes(&target, type, format == 0 ? 18 : 39, 1);
    ck_assert_uint_eq(entry[2], DISKIMAGE_FTYPE_SEQ);
    if (failed_block == 3) {
        const uint8_t* const first = sector_bytes(&target, type, entry[3], entry[4]);
        ck_assert_uint_ne(first[0], 0);
        for (unsigned int index = 2; index < DISKIMAGE_SECTOR_SIZE; ++index) {
            ck_assert_uint_eq(first[index], (uint8_t)(0x20u + index - 2u));
        }
        const uint8_t* const unwritten = sector_bytes(&target, type, first[0], first[1]);
        const uint8_t zero[DISKIMAGE_SECTOR_SIZE] = {0};
        ck_assert_mem_eq(unwritten, zero, sizeof(zero));
    }
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// A peer's read status is not the completion result of COPY's healthy APPEND.
START_TEST(test_copy_self_append_ignores_peer_read_error_status) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:SOURCE,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x20, 255);
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:OTHER,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x60, 2);
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    const uint8_t peer = open_name(&unit, &bank, 3, "0:SOURCE,S,R");
    uint8_t prefix[DISKIMAGE_SECTOR_SIZE - DISKIMAGE_SECTOR_LINK_SIZE];
    for (unsigned int index = 0; index < sizeof(prefix); ++index) {
        prefix[index] = (uint8_t)(0x20u + index);
    }
    expect_direct(&unit, &bank, peer, prefix, sizeof(prefix), false);
    send_command(&unit, &bank, "C0:SOURCE=0:SOURCE,0:OTHER");
    for (unsigned int step = 0; unit.copy.writer == DOS_UNUSED; ++step) {
        ck_assert_uint_lt(step, 10);
        dos_work(&unit, &bank);
    }
    dos_work(&unit, &bank);
    unit.buffers[unit.channels[peer].buffers[1]].resident = false;
    image.fail_read = image.reads + 1u;
    uint8_t byte;
    bool eoi;
    ck_assert_uint_eq(dos_stage(&unit, &bank, peer, &byte, 1, &eoi), 0);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    image.fail_read = 0;
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    uint8_t expected[257];
    memcpy(expected, prefix, sizeof(prefix));
    expected[254] = (uint8_t)(0x20u + 254u);
    expected[255] = 0x60;
    expected[256] = 0x61;
    expect_copy_payload(&image, bank.units[0].slots[0].io.image.type, "SOURCE", expected, sizeof(expected),
                        DISKIMAGE_FTYPE_SEQ);
    ck_assert_uint_eq(dos_find_channel(&unit, 3), peer);
    free(image.bytes);
}
END_TEST

// Retained CLSDIR ownership prevents COPY from following an already reclaimed chain.
START_TEST(test_copy_respects_retained_reclaim_entry) {
    const unsigned int format = (unsigned int)_i / 5u, variant = (unsigned int)_i % 5u;
    dos_media_bank_t bank = {0};
    image_t source, target;
    mount_blank(&bank, &source, format);
    mount_peer(&bank, &target, format);
    fixture_file(&source, format, 0, "F", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    limit_free_blocks(&source, format, 2);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    const uint8_t owner = open_name(&unit, &bank, 2, "@0:F,S,W");
    ck_assert_uint_ne(owner, DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x40, 22);
    ck_assert_uint_ne(open_name(&unit, &bank, 3, "0:PEER,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 3, 0x60, 22);
    ck_assert(dos_close(&unit, &bank, model, 3));
    if (variant == 4) {
        send_command(&unit, &bank, "C1:NEW=0:PEER,0:F");
        for (unsigned int step = 0; unit.copy.writer == DOS_UNUSED; ++step) {
            ck_assert_uint_lt(step, 10);
            dos_work(&unit, &bank);
        }
    }
    // Fail after reclaiming F's original sector but before publishing its new link.
    source.fail_write = source.writes + 2u;
    source.fail_write_without_changes = true;
    ck_assert(!dos_close(&unit, &bank, model, 2));
    ck_assert_uint_eq(unit.channels[owner].closing, DOS_CLOSE_RECLAIM);
    source.fail_write = 0;
    if (variant != 4) {
        const uint8_t reused = open_name(&unit, &bank, 4, "0:REUSED,S,W");
        ck_assert_uint_ne(reused, DOS_UNUSED);
        ck_assert_uint_eq(unit.channels[reused].first_track, 1);
        ck_assert_uint_eq(unit.channels[reused].first_sector, 0);
        put_bytes(&unit, &bank, 4, 0xc0, 22);
        ck_assert(dos_close(&unit, &bank, model, 4));
        static const char* const commands[] = {"C1:NEW=0:F", "C0:F=0:F,0:PEER", "C1=0", "C1:*=0:*"};
        send_command(&unit, &bank, commands[variant]);
    }
    const dos_channel_t before = unit.channels[owner];
    const unsigned int writes = source.writes + target.writes;
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(source.writes + target.writes, writes);
    ck_assert_mem_eq(&unit.channels[owner], &before, sizeof(before));
    ck_assert_uint_eq(unit.buffer_use, (1u << before.buffers[0]) | (1u << before.buffers[1]));
    if (variant != 4) {
        uint8_t expected[22];
        for (unsigned int index = 0; index < sizeof(expected); ++index) {
            expected[index] = 0xc0u + index;
        }
        expect_copy_payload(&source, bank.units[0].slots[0].io.image.type, "REUSED", expected, sizeof(expected),
                            DISKIMAGE_FTYPE_SEQ);
    }
    ck_assert(dos_close(&unit, &bank, model, 2));
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// A peer error cannot select the status drive of a later COPY APPEND read failure.
START_TEST(test_copy_append_reselects_owner_after_peer_error) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t peer_image, source;
    mount_blank(&bank, &peer_image, format);
    mount_peer(&bank, &source, format);
    fixture_file(&peer_image, format, 0, "PEER", DISKIMAGE_FTYPE_SEQ, 0x80, 255);
    fixture_file(&source, format, 0, "A", DISKIMAGE_FTYPE_SEQ, 0x20, 509);
    fixture_file(&source, format, 2, "B", DISKIMAGE_FTYPE_SEQ, 0x60, 2);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t peer = open_name(&unit, &bank, 3, "0:PEER,S,R");
    uint8_t prefix[DISKIMAGE_SECTOR_PAYLOAD_SIZE];
    for (unsigned int index = 0; index < sizeof(prefix); ++index) {
        prefix[index] = 0x80u + index;
    }
    expect_direct(&unit, &bank, peer, prefix, sizeof(prefix), false);
    send_command(&unit, &bank, "C1:A=1:A,1:B");
    for (unsigned int step = 0; unit.copy.writer == DOS_UNUSED; ++step) {
        ck_assert_uint_lt(step, 10);
        dos_work(&unit, &bank);
    }
    dos_work(&unit, &bank);
    unit.buffers[unit.channels[peer].buffers[1]].resident = false;
    peer_image.fail_read = peer_image.reads + 1u;
    uint8_t byte;
    bool eoi;
    ck_assert_uint_eq(dos_stage(&unit, &bank, peer, &byte, 1, &eoi), 0);
    ck_assert_uint_eq(unit.selected_drive, 0);
    source.fail_read = source.reads + 1u;
    finish_open(&unit, &bank);
    ck_assert_str_eq(unit.status, format == 0 ? "23,READ ERROR,01,02\r" : "23,READ ERROR,01,02,1\r");
    ck_assert_uint_eq(unit.selected_drive, 1);
    ck_assert_uint_eq(dos_find_channel(&unit, 3), peer);
    ck_assert_uint_eq(source.writes + source.flushes, 0);
    free(source.bytes);
    free(peer_image.bytes);
}
END_TEST

// Alternating GIBYTE/PIBYTE boundaries must report the failing operation's drive.
START_TEST(test_copy_cross_drive_read_and_allocation_status) {
    const unsigned int format = (unsigned int)_i % 3u, drive = (unsigned int)_i / 3u % 2u;
    const bool read_error = (unsigned int)_i / 6u != 0;
    dos_media_bank_t bank = {0};
    image_t images[2];
    mount_blank(&bank, &images[0], format);
    mount_peer(&bank, &images[1], format);
    image_t* const target = &images[drive];
    image_t* const source = &images[drive ^ 1u];
    limit_free_blocks(target, format, read_error ? 3 : 1);
    fixture_file(source, format, 0, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x20, 300);
    if (read_error) {
        ck_assert(diskimage_sector_offset(bank.units[0].slots[0].io.image.type, 1, 1, &source->fail_read_offset));
    }
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    char command[32], expected[40];
    snprintf(command, sizeof(command), "C%u:NEW=%u:SOURCE", drive, drive ^ 1u);
    send_command(&unit, &bank, command);
    finish_open(&unit, &bank);
    const unsigned int failing = read_error ? drive ^ 1u : drive;
    snprintf(expected, sizeof(expected), read_error ? "23,READ ERROR,01,01%s" : "72, DISK FULL,00,00%s",
             format == 0 ? "\r"
             : failing   ? ",1\r"
                         : ",0\r");
    ck_assert_str_eq(unit.status, expected);
    ck_assert_uint_eq(unit.selected_drive, failing);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(source->writes + source->flushes, 0);
    free(images[0].bytes);
    free(images[1].bytes);
}
END_TEST

// CHKIO reports the last LOOKUP drive before CY selects its output drive.
START_TEST(test_copy_cross_drive_collision_retains_lookup_drive) {
    const unsigned int format = (unsigned int)_i % 3u, target = (unsigned int)_i / 3u;
    const unsigned int source = target ^ 1u;
    dos_media_bank_t bank = {0};
    image_t images[2];
    mount_blank(&bank, &images[0], format);
    mount_peer(&bank, &images[1], format);
    fixture_file(&images[0], format, 0, "SAME", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    fixture_file(&images[1], format, 0, "SAME", DISKIMAGE_FTYPE_SEQ, 0x60, 22);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    char command[sizeof("C0:SAME=1:SAME")], expected[sizeof("63, FILE EXISTS,00,00,1\r")];
    snprintf(command, sizeof(command), "C%c:SAME=%c:SAME", '0' + (int)target, '0' + (int)source);
    send_command(&unit, &bank, command);
    finish_open(&unit, &bank);
    snprintf(expected, sizeof(expected), "63, FILE EXISTS,00,00%s", format == 0 ? "\r" : source ? ",1\r" : ",0\r");
    ck_assert_str_eq(unit.status, expected);
    ck_assert_uint_eq(unit.selected_drive, source);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(images[0].writes + images[1].writes, 0);
    free(images[0].bytes);
    free(images[1].bytes);
}
END_TEST

// A successful peer replacement can retire lookup's chain before its deferred OPEN.
START_TEST(test_copy_revalidates_source_after_successful_peer_close) {
    const unsigned int format = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0};
    image_t source, target;
    mount_blank(&bank, &source, format);
    mount_peer(&bank, &target, format);
    fixture_file(&source, format, 0, "F", DISKIMAGE_FTYPE_SEQ, 0x20, 22);
    fixture_file(&source, format, 1, "FIRST", DISKIMAGE_FTYPE_SEQ, 0x60, 4);
    limit_free_blocks(&source, format, 2);
    const dos_model_t model = bank.units[0].model;
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "@0:F,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x40, 22);
    const uint8_t reused = open_name(&unit, &bank, 4, "0:REUSED,S,W");
    ck_assert_uint_ne(reused, DOS_UNUSED);
    put_bytes(&unit, &bank, 4, 0x80, DISKIMAGE_SECTOR_PAYLOAD_SIZE - 1u);
    static const char* const commands[] = {"C1:NEW=0:F", "C1:NEW=0:FIRST,0:F", "C1=0", "C1:*=0:*"};
    send_command(&unit, &bank, commands[(unsigned int)_i / 3u]);
    for (unsigned int step = 0; unit.copy.writer == DOS_UNUSED; ++step) {
        ck_assert_uint_lt(step, 10);
        dos_work(&unit, &bank);
    }
    ck_assert(dos_close(&unit, &bank, model, 2));
    // Existing-channel PUT now allocates F's reclaimed original sector.
    put_bytes(&unit, &bank, 4, 0x55, 1);
    ck_assert_uint_eq(unit.buffers[unit.channels[reused].buffers[unit.channels[reused].active]].track, 1);
    ck_assert_uint_eq(unit.buffers[unit.channels[reused].buffers[unit.channels[reused].active]].sector, 0);
    put_bytes(&unit, &bank, 4, 0xc0, 22);
    ck_assert(dos_close(&unit, &bank, model, 4));
    const unsigned int writes = source.writes + target.writes;
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(source.writes + target.writes, writes);
    ck_assert_uint_eq(unit.buffer_use, 0);
    uint8_t expected[DISKIMAGE_SECTOR_PAYLOAD_SIZE + 22u];
    for (unsigned int index = 0; index < DISKIMAGE_SECTOR_PAYLOAD_SIZE - 1u; ++index) {
        expected[index] = 0x80u + index;
    }
    expected[DISKIMAGE_SECTOR_PAYLOAD_SIZE - 1u] = 0x55;
    for (unsigned int index = 0; index < 22; ++index) {
        expected[DISKIMAGE_SECTOR_PAYLOAD_SIZE + index] = 0xc0u + index;
    }
    expect_copy_payload(&source, type, "REUSED", expected, sizeof(expected), DISKIMAGE_FTYPE_SEQ);
    send_command(&unit, &bank, "C1:AFTER=0:F");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    uint8_t fresh[22];
    for (unsigned int index = 0; index < sizeof(fresh); ++index) {
        fresh[index] = 0x40u + index;
    }
    expect_copy_payload(&target, type, "AFTER", fresh, sizeof(fresh), DISKIMAGE_FTYPE_SEQ);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// Peer CLSDIR must retire already-open COPY readers and self-append writers before reuse.
START_TEST(test_copy_retires_active_binding_before_sector_reuse) {
    const unsigned int format = (unsigned int)_i % 3u;
    const bool append = _i >= 3;
    dos_media_bank_t bank = {0};
    image_t source, target;
    mount_blank(&bank, &source, format);
    mount_peer(&bank, &target, format);
    fixture_file(&source, format, 0, "F", DISKIMAGE_FTYPE_SEQ, 0x20, 600);
    fixture_file(&source, format, 2, "TAIL", DISKIMAGE_FTYPE_SEQ, 0x60, 2);
    limit_free_blocks(&source, format, 2);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "@0:F,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x40, 22);
    ck_assert_uint_ne(open_name(&unit, &bank, 4, "0:REUSED,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 4, 0x80, DISKIMAGE_SECTOR_PAYLOAD_SIZE - 1u);
    send_command(&unit, &bank, append ? "C0:F=0:F,0:TAIL" : "C1:NEW=0:F");
    for (unsigned int step = 0; unit.copy.writer == DOS_UNUSED; ++step) {
        ck_assert_uint_lt(step, 10);
        dos_work(&unit, &bank);
    }
    // Advance into APPEND traversal or a live source's first payload burst.
    dos_work(&unit, &bank);
    if (!append) {
        dos_work(&unit, &bank);
    }
    ck_assert(dos_close(&unit, &bank, model, 2));
    put_bytes(&unit, &bank, 4, 0x55, 1);
    put_bytes(&unit, &bank, 4, 0xc0, DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    put_bytes(&unit, &bank, 4, 0xa0, DISKIMAGE_SECTOR_PAYLOAD_SIZE);
    put_bytes(&unit, &bank, 4, 0x60, 22);
    ck_assert(dos_close(&unit, &bank, model, 4));
    const unsigned int writes = source.writes + target.writes;
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(source.writes + target.writes, writes);
    ck_assert_uint_eq(unit.buffer_use, 0);
    uint8_t expected[3u * DISKIMAGE_SECTOR_PAYLOAD_SIZE + 22u];
    for (unsigned int index = 0; index < DISKIMAGE_SECTOR_PAYLOAD_SIZE; ++index) {
        expected[index] = 0x80u + index;
        expected[DISKIMAGE_SECTOR_PAYLOAD_SIZE + index] = 0xc0u + index;
        expected[2u * DISKIMAGE_SECTOR_PAYLOAD_SIZE + index] = 0xa0u + index;
    }
    expected[DISKIMAGE_SECTOR_PAYLOAD_SIZE - 1u] = 0x55;
    for (unsigned int index = 0; index < 22; ++index) {
        expected[3u * DISKIMAGE_SECTOR_PAYLOAD_SIZE + index] = 0x60u + index;
    }
    expect_copy_payload(&source, bank.units[0].slots[0].io.image.type, "REUSED", expected, sizeof(expected),
                        DISKIMAGE_FTYPE_SEQ);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// A source that already completed no longer owns a live binding in copy-all.
START_TEST(test_copy_all_allows_close_of_completed_source) {
    const unsigned int format = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0};
    image_t source, target;
    mount_blank(&bank, &source, format);
    mount_peer(&bank, &target, format);
    fixture_file(&source, format, 0, "F", DISKIMAGE_FTYPE_SEQ, 0x20, 4);
    fixture_file(&source, format, 1, "NEXT", DISKIMAGE_FTYPE_SEQ, 0x60, 2);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const dos_model_t model = bank.units[0].model;
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "@0:F,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x40, 4);
    send_command(&unit, &bank, _i < 3 ? "C1=0" : "C1:*=0:*");
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    const uint8_t* const directory = sector_bytes(&target, type, format == 0 ? 18 : 39, 1);
    for (unsigned int step = 0; !(directory[2] & 0x80u) || unit.copy.writer != DOS_UNUSED; ++step) {
        ck_assert_uint_lt(step, 50);
        dos_work(&unit, &bank);
    }
    ck_assert(dos_close(&unit, &bank, model, 2));
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    const uint8_t f[] = {0x20, 0x21, 0x22, 0x23}, next[] = {0x60, 0x61};
    expect_copy_payload(&target, type, "F", f, sizeof(f), DISKIMAGE_FTYPE_SEQ);
    expect_copy_payload(&target, type, "NEXT", next, sizeof(next), DISKIMAGE_FTYPE_SEQ);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// CMPCHK leaves zero FILTRK unresolved and LK26 repeats the matching directory entry.
START_TEST(test_copy_zero_track_match_remains_pending) {
    const unsigned int format = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x20, 4);
    fixture_file(&image, format, 1, "TARGET", DISKIMAGE_FTYPE_SEQ, 0x60, 2);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    sector_bytes(&image, type, format == 0 ? 18 : 39, 1)[(_i < 3 ? 0 : 32) + 3] = 0;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, "C0:TARGET=0:SOURCE");
    for (unsigned int step = 0; step < 10; ++step) {
        dos_work(&unit, &bank);
    }
    ck_assert(dos_work_pending(&unit));
    const unsigned int reads = image.reads;
    for (unsigned int step = 0; step < 10; ++step) {
        dos_work(&unit, &bank);
    }
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes + image.flushes, 0);
    dos_reset(&unit, bank.units[0].model);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert(!dos_work_pending(&unit));
    free(image.bytes);
}
END_TEST

// NEW replaces only metadata without an ID, and all sector data with an ID.
START_TEST(test_format_quick_and_full_complete_images) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool full = (unsigned int)_i % 2u != 0;
    dos_media_bank_t bank = {0}, expected_bank = {0};
    image_t image, expected;
    mount_blank(&bank, &image, format);
    mount_blank(&expected_bank, &expected, format);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    const unsigned int directory = format == 0 ? 18u : 39u;
    const unsigned int name = format == 0 ? 144u : 6u;
    memset(sector_bytes(&image, type, 1, 0), 0x5a, DISKIMAGE_SECTOR_SIZE);
    if (!full) {
        memset(sector_bytes(&expected, type, 1, 0), 0x5a, DISKIMAGE_SECTOR_SIZE);
    } else {
        // Full FORMAT must replace a foreign version without first reading it.
        set_map_version(&image, type, format, 0xff);
    }
    uint8_t* const header = sector_bytes(&expected, type, directory, 0);
    memset(header + name, 0xa0, 27);
    memcpy(header + name, "RENAMED", 7);
    memcpy(header + name + 18u, full ? "AB" : "01", 2);
    header[name + 21u] = '2';
    header[name + 22u] = format == 0 ? 'A' : 'C';
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, full ? "N0:RENAMED,AB" : "N0:RENAMED");
    ck_assert_uint_eq(image.writes, 0);
    ck_assert(dos_work_pending(&unit));
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert(unit.free_blocks_valid[0]);
    ck_assert_mem_eq(image.bytes, expected.bytes, image.size);
    ck_assert_uint_eq(image.flushes, 0);
    ck_assert_uint_eq(dos_media_flush_unit(&bank, 0), DOS_MEDIA_OK);
    ck_assert_uint_eq(image.flushes, 1);
    free(image.bytes);
    free(expected.bytes);
}
END_TEST

// DUPLICATE copies every raw sector, including free data, in either direction.
START_TEST(test_duplicate_complete_image_and_source_isolation) {
    const unsigned int format = (unsigned int)_i / 2u;
    const unsigned int target = (unsigned int)_i % 2u;
    dos_media_bank_t bank = {0}, other = {0};
    image_t images[2];
    mount_reader_pointer_image(&bank, &images[0], format);
    mount_blank(&other, &images[1], format);
    ck_assert_uint_eq(dos_media_set_slot(&bank, 0, 1, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    const unsigned int source = target ^ 1u;
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    memset(sector_bytes(&images[source], type, 1, 0), 0xa5, DISKIMAGE_SECTOR_SIZE);
    uint8_t* const original = malloc(images[source].size);
    ck_assert_ptr_nonnull(original);
    memcpy(original, images[source].bytes, images[source].size);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, target == 0 ? "D0=1" : "D1=0");
    ck_assert_uint_eq(images[0].writes + images[1].writes, 0);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.last_drive, target);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_mem_eq(images[target].bytes, original, images[target].size);
    ck_assert_mem_eq(images[source].bytes, original, images[source].size);
    ck_assert_uint_eq(images[source].writes + images[source].flushes, 0);
    free(original);
    free(images[0].bytes);
    free(images[1].bytes);
}
END_TEST

// Destructive command failures retain their written prefix and release scratch.
START_TEST(test_format_and_duplicate_storage_failures) {
    const unsigned int format = (unsigned int)_i / 9u;
    const unsigned int variant = (unsigned int)_i % 9u;
    dos_media_bank_t bank = {0}, other = {0};
    image_t image, source;
    mount_blank(&bank, &image, format);
    mount_blank(&other, &source, format);
    ck_assert_uint_eq(dos_media_set_slot(&bank, 0, 1, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    if (variant == 0) {
        bank.units[0].slots[0].io.image.write = NULL;
    } else if (variant == 1) {
        image.fail_read = 1;
    } else if (variant == 2) {
        set_map_version(&image, type, format, 0xff);
    } else if (variant == 3) {
        image.fail_write = 3;
    } else if (variant == 4) {
        source.fail_read = 1;
    } else if (variant == 5) {
        source.fail_read = format == 0 ? 2 : format == 1 ? 4 : 6;
    } else if (variant == 6) {
        set_map_version(&source, type, format, 0xff);
    } else if (variant == 7) {
        bank.units[0].slots[0].io.image.write = NULL;
    } else {
        image.fail_write = 3;
    }
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, variant >= 4 ? "D0=1" : variant == 3 ? "N0:NEW,AB" : "N0:NEW");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, variant == 0 || variant == 7   ? DOS_STATUS_WRITE_PROTECT
                                        : variant == 2 || variant == 6 ? DOS_STATUS_POWER_ON
                                        : variant == 3 || variant == 8 ? DOS_STATUS_WRITE_ERROR
                                                                       : DOS_STATUS_READ_ERROR);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(image.flushes + source.flushes + source.writes, 0);
    if (variant == 1 || variant == 2 || variant == 4 || variant == 6 || variant == 7) {
        ck_assert_uint_eq(image.writes, 0);
    }
    if (variant == 3 || variant == 8) {
        ck_assert_uint_eq(image.writes, 3);
    }
    free(image.bytes);
    free(source.bytes);
}
END_TEST

// INITDR/CLRCHN abandon writers without CLOSE and preserve native disk scope.
START_TEST(test_format_channel_lifecycle) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool full = (unsigned int)_i % 2u != 0;
    dos_media_bank_t bank = {0}, other = {0};
    image_t image, peer_image;
    mount_reader_pointer_image(&bank, &image, format);
    mount_reader_pointer_image(&other, &peer_image, format);
    ck_assert_uint_eq(dos_media_set_slot(&bank, 0, 1, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t selected = open_name(&unit, &bank, 2, "0:READ,P,R");
    const uint8_t peer = open_name(&unit, &bank, 3, "1:READ,P,R");
    const dos_channel_t saved_peer = unit.channels[peer];
    const unsigned int peer_reads = peer_image.reads;
    send_command(&unit, &bank, full ? "N0:NEW,AB" : "N0:NEW");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_int_eq(unit.channels[selected].allocated, full && format == 0);
    ck_assert_int_eq(unit.channels[peer].allocated, !full || format == 0);
    if (!full || format == 0) {
        ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    }
    ck_assert_uint_eq(peer_image.reads, peer_reads);
    ck_assert_uint_eq(peer_image.writes + peer_image.flushes, 0);
    free(image.bytes);
    free(peer_image.bytes);
}
END_TEST

// Host formatter safeguards retain live/orphan scratch ownership on rejection.
START_TEST(test_format_and_duplicate_preserve_busy_scratch) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool duplicate = (unsigned int)_i % 2u != 0;
    dos_media_bank_t bank = {0}, other = {0};
    image_t target, source;
    mount_blank(&bank, &target, format);
    mount_blank(&other, &source, format);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    uint8_t retained = DOS_UNUSED;
    if (duplicate) {
        // Explicit reservation precedes failed logical-channel allocation.
        for (uint8_t offset = 0; offset < DOS_FILE_CHANNEL_COUNT; ++offset) {
            ck_assert_uint_ne(open_name(&unit, &bank, offset + 2u, "#"), DOS_UNUSED);
        }
        ck_assert_uint_eq(open_name(&unit, &bank, DOS_FILE_CHANNEL_COUNT + 2u, "#0"), DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
    } else {
        retained = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, "#0");
        ck_assert_uint_ne(retained, DOS_UNUSED);
    }
    const dos_channel_t saved = retained == DOS_UNUSED ? (dos_channel_t){0} : unit.channels[retained];
    send_command(&unit, &bank, duplicate ? "D0=1" : "N0:NEW,AB");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
    ck_assert_uint_eq(unit.buffer_use, 1u);
    ck_assert_uint_eq(target.writes + source.writes, 0);
    if (!duplicate) {
        ck_assert_mem_eq(&unit.channels[retained], &saved, sizeof(saved));
    }
    free(target.bytes);
    free(source.bytes);
}
END_TEST

// Captured destructive jobs must never read or write a replacement generation.
START_TEST(test_format_and_duplicate_reject_replaced_media) {
    const unsigned int format = (unsigned int)_i / 8u;
    const unsigned int variant = ((unsigned int)_i / 2u) % 4u;
    const bool prefix = (unsigned int)_i % 2u != 0;
    dos_media_bank_t bank = {0}, source_bank = {0}, replacement_bank = {0};
    image_t target, source, replacement;
    mount_blank(&bank, &target, format);
    mount_blank(&source_bank, &source, format);
    mount_blank(&replacement_bank, &replacement, format);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &source_bank.units[0].slots[0].io), DOS_MEDIA_OK);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, variant >= 2 ? "D0=1" : variant == 1 ? "N0:NEW,AB" : "N0:NEW");

    // Replace either captured participant before work or after a persisted prefix.
    while (prefix && target.writes < 2) {
        ck_assert(dos_work_pending(&unit));
        dos_work(&unit, &bank);
    }
    const unsigned int old_writes = target.writes;
    const unsigned int slot = variant == 3 ? 1u : 0u;
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, slot, &replacement_bank.units[0].slots[0].io), DOS_MEDIA_OK);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, !prefix && variant != 1 ? DOS_STATUS_READ_ERROR : DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(replacement.reads + replacement.writes + replacement.flushes, 0);
    ck_assert_uint_eq(target.writes, old_writes);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(target.bytes);
    free(source.bytes);
    free(replacement.bytes);
}
END_TEST

// NEW's ID boundary preserves retained command bytes and trailing-comma quick mode.
START_TEST(test_format_id_and_inherited_drive_boundaries) {
    const unsigned int format = (unsigned int)_i / 3u;
    const unsigned int variant = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0}, other = {0};
    image_t untouched, image;
    mount_blank(&bank, &untouched, format);
    mount_blank(&other, &image, format);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    const diskimage_type_t type = bank.units[0].slots[1].io.image.type;
    memset(sector_bytes(&image, type, 1, 0), 0x5a, DISKIMAGE_SECTOR_SIZE);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, "U;1234Z");
    ck_assert_uint_eq(unit.command_bytes[5], '4');
    unit.last_drive = 1;
    send_command(&unit, &bank, variant == 0 ? "N:X,A" : variant == 1 ? "N:X," : "N:,");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.last_drive, 1);
    ck_assert_uint_eq(untouched.reads + untouched.writes + untouched.flushes, 0);
    const unsigned int name = format == 0 ? 144u : 6u;
    const uint8_t* const header = sector_bytes(&image, type, format == 0 ? 18u : 39u, 0);
    ck_assert_uint_eq(header[name], variant == 2 ? ',' : 'X');
    if (variant == 2) {
        ck_assert_uint_eq(header[name + 27u], DISKIMAGE_FILENAME_PADDING);
    }
    ck_assert_uint_eq(header[name + 18u], variant == 0 ? 'A' : '0');
    // "N:X,A" has length five, so its second ID byte is the retained byte '4'.
    ck_assert_uint_eq(header[name + 19u], variant == 0 ? '4' : '1');
    ck_assert_uint_eq(sector_bytes(&image, type, 1, 0)[0], variant == 0 ? 0 : 0x5a);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(untouched.bytes);
    free(image.bytes);
}
END_TEST

// Reading status after failed INITDR cannot authorize NEW or finish DUPLICATE.
START_TEST(test_format_initialization_result_survives_status_consumption) {
    const unsigned int format = (unsigned int)_i / 3u, variant = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0};
    image_t target, source;
    mount_blank(&bank, &target, format);
    mount_peer(&bank, &source, format);
    if (variant == 1) {
        source.fail_read = 1;
    } else {
        target.fail_read = 1;
    }
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    unit.last_drive = 1;
    send_command(&unit, &bank, variant == 0 ? "N0:NEW" : "D0=1");
    unsigned int turns = 0;
    while (unit.initialize.pending || (variant == 2 && target.reads == 0)) {
        ck_assert_uint_lt(++turns, 20000);
        ck_assert(dos_work_pending(&unit));
        dos_work(&unit, &bank);
    }
    ck_assert(!unit.initialize.succeeded);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    const unsigned int writes = target.writes;
    dos_status_get(&unit, model, unit.status_length);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(target.writes, writes);
    ck_assert_uint_eq(source.writes + source.flushes, 0);
    if (variant != 2) {
        ck_assert_uint_eq(writes, 0);
    } else {
        // Successful ENDCMD would overwrite the last completed drive.
        ck_assert_uint_eq(unit.last_drive, 1);
        ck_assert(writes > 0);
    }
    free(target.bytes);
    free(source.bytes);
}
END_TEST

// Invalid NEW groups and non-distinct PRSEQ selectors fail before media access.
START_TEST(test_format_and_duplicate_native_parser_rejections) {
    static const char* const commands[] = {
        "N0",      "N0:*,AB",    "N0:X=Y", "N0,1:X",    "D0=0",       "D2=1",       "N*0:NEW,AB",   "N?1:NEW",
        "N*0:NEW", "N?0:NEW,AB", "N=:NEW", "N=:NEW,AB", "N0:X,AB,,*", "N0:X,AB,,=", "N*0:X,AB,,,*",
    };
    const unsigned int format = (unsigned int)_i / count_of(commands);
    const unsigned int variant = (unsigned int)_i % count_of(commands);
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, commands[variant]);
    ck_assert(!dos_work_pending(&unit));
    ck_assert_uint_eq(unit.status_code, variant == 0 || variant == 3 ? DOS_STATUS_NO_FILE : DOS_STATUS_SYNTAX);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(image.reads + image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

// INITDR failure must retain the dirty disk's flush owner across every shared caller.
START_TEST(test_failed_initialization_retains_prior_format_flush) {
    static const char* const commands[] = {"N0:NEW", "I0", "D1=0", "V0"};
    const unsigned int format = (unsigned int)_i % 3u;
    const unsigned int command = (unsigned int)_i / 3u;
    dos_media_bank_t bank = {0};
    image_t image, target;
    mount_blank(&bank, &image, format);
    if (command == 2) {
        mount_peer(&bank, &target, format);
    }
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, "N0:OLD,AB");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert(bank.units[0].slots[0].dirty);
    image.fail_read = image.reads + 1u;
    send_command(&unit, &bank, commands[command]);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    image.fail_flush = true;
    ck_assert(!dos_finalize(&unit, &bank));
    ck_assert_uint_eq(image.flushes, 1);
    ck_assert(bank.units[0].slots[0].dirty);
    image.fail_flush = false;
    ck_assert(dos_finalize(&unit, &bank));
    ck_assert_uint_eq(image.flushes, 2);
    ck_assert(!bank.units[0].slots[0].dirty);
    free(image.bytes);
    if (command == 2) {
        ck_assert_uint_eq(target.reads + target.writes + target.flushes, 0);
        free(target.bytes);
    }
}
END_TEST

// PARSE stops at the fourth comma, before examining any following pattern or equals.
START_TEST(test_format_ignores_suffix_after_native_comma_limit) {
    static const char* const commands[] = {"N0:X,AB,,,*", "N0:X,AB,,,=", "N0:X,AB,,,?"};
    const unsigned int format = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    memset(sector_bytes(&image, type, 1, 0), 0x55, DISKIMAGE_SECTOR_SIZE);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, commands[(unsigned int)_i / 3u]);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    const uint8_t* const label = sector_bytes(&image, type, format == 0 ? 18 : 39, 0) + (format == 0 ? 144 : 6);
    ck_assert_uint_eq(label[0], 'X');
    ck_assert_mem_eq(label + 18, "AB", 2);
    ck_assert_uint_eq(sector_bytes(&image, type, 1, 0)[0], 0);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// Quick NEW reuses INITDR's ID without another header read before metadata writes.
START_TEST(test_format_quick_reuses_initialized_header_id) {
    const unsigned int format = (unsigned int)_i % 3u;
    const unsigned int drive = (unsigned int)_i / 3u;
    dos_media_bank_t bank = {0};
    image_t images[2];
    mount_blank(&bank, &images[0], format);
    mount_peer(&bank, &images[1], format);
    image_t* const image = &images[drive];
    const diskimage_type_t type = bank.units[0].slots[drive].io.image.type;
    const unsigned int track = format == 0 ? 18u : 39u;
    const unsigned int name = format == 0 ? 144u : 6u;
    memcpy(sector_bytes(image, type, track, 0) + name + 18u, "Q7", 2);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, drive == 0 ? "N0:NEW" : "N1:NEW");
    for (unsigned int step = 0; unit.initialize.pending; ++step) {
        ck_assert_uint_lt(step, 12);
        dos_work(&unit, &bank);
    }
    ck_assert(unit.initialize.succeeded);
    ck_assert_uint_eq(image->writes, 0);
    const unsigned int reads = image->reads;
    image->fail_read = reads + 1u;
    for (unsigned int step = 0; image->writes == 0 && dos_work_pending(&unit); ++step) {
        ck_assert_uint_lt(step, 12);
        dos_work(&unit, &bank);
    }
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(image->reads, reads);
    ck_assert_uint_eq(image->writes, 1);
    image->fail_read = 0;
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_mem_eq(sector_bytes(image, type, track, 0) + name + 18u, "Q7", 2);
    ck_assert_uint_eq(images[drive ^ 1u].reads + images[drive ^ 1u].writes, 0);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(images[0].bytes);
    free(images[1].bytes);
}
END_TEST

// A retained SA0 footer must wait for NEW's shared BAM and count ownership to end.
START_TEST(test_format_serializes_surviving_directory_footer) {
    const unsigned int format = (unsigned int)_i % 2u + 1u;
    const bool counted = (unsigned int)_i / 2u % 2u != 0;
    const bool fail = _i >= 4;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t listing = open_name(&unit, &bank, IEEE_LOAD_CHANNEL, "$0");
    ck_assert_uint_ne(listing, DOS_UNUSED);
    uint8_t output[DOS_STAGE_CAPACITY];
    bool eoi;
    const unsigned int header = dos_stage(&unit, &bank, listing, output, sizeof(output), &eoi);
    ck_assert_uint_eq(header, 32);
    dos_get(&unit, &bank, listing, header);
    ck_assert_uint_eq(dos_stage(&unit, &bank, listing, output, sizeof(output), &eoi), 0);
    const uint16_t use = unit.buffer_use;
    send_command(&unit, &bank, "N0:NEW,AB");
    const unsigned int prefix = image.size / DISKIMAGE_SECTOR_SIZE + (format == 1 ? 5u : 7u);
    for (unsigned int step = 0; image.writes < prefix; ++step) {
        ck_assert_uint_lt(step, DISKIMAGE_D82_SIZE / DISKIMAGE_SECTOR_SIZE + 100u);
        dos_work(&unit, &bank);
    }
    if (counted) {
        dos_work(&unit, &bank);
    }
    const dos_bam_t saved = unit.bam[0];
    const unsigned int total = unit.free_blocks[0];
    const unsigned int reads = image.reads;
    ck_assert_uint_eq(dos_stage(&unit, &bank, listing, output, sizeof(output), &eoi), 0);
    ck_assert(!eoi);
    ck_assert(unit.channels[listing].ready);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_mem_eq(&unit.bam[0], &saved, sizeof(saved));
    ck_assert_uint_eq(unit.free_blocks[0], total);
    if (fail) {
        image.fail_read = image.reads + 1u;
    }
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, fail ? DOS_STATUS_READ_ERROR : DOS_STATUS_OK);
    ck_assert_uint_eq(unit.buffer_use, use);
    image.fail_read = 0;
    const unsigned int footer = dos_stage(&unit, &bank, listing, output, sizeof(output), &eoi);
    ck_assert_uint_eq(footer, 32);
    ck_assert(eoi);
    ck_assert_uint_eq(output[2] | ((unsigned int)output[3] << 8u), format == 1 ? 2052u : 4133u);
    dos_get(&unit, &bank, listing, footer);
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, IEEE_LOAD_CHANNEL));
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// 4040 ENDCMD follows the final metadata write without another controller read.
START_TEST(test_format_4040_completes_without_final_bam_read) {
    const bool full = _i != 0;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, 0);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, full ? "N0:NEW,AB" : "N0:NEW");
    const unsigned int writes = (full ? image.size / DISKIMAGE_SECTOR_SIZE : 0u) + 2u;
    for (unsigned int step = 0; image.writes < writes; ++step) {
        ck_assert_uint_lt(step, DISKIMAGE_D64_SIZE / DISKIMAGE_SECTOR_SIZE + 100u);
        dos_work(&unit, &bank);
    }
    const unsigned int reads = image.reads;
    image.fail_read = reads + 1u;
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert(unit.bam[0].buffer.resident);
    ck_assert_mem_eq(unit.bam[0].buffer.bytes, sector_bytes(&image, bank.units[0].slots[0].io.image.type, 18, 0),
                     DISKIMAGE_SECTOR_SIZE);
    ck_assert_uint_eq(unit.free_blocks[0], 664);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// Required late header/BAM/directory and NF05 reads cannot be replaced by synthetic success.
START_TEST(test_format_propagates_late_metadata_chain_failures) {
    const unsigned int format = (unsigned int)_i % 2u + 1u;
    const bool full = (unsigned int)_i / 2u % 2u != 0;
    const unsigned int stage = (unsigned int)_i / 4u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, full ? "N0:NEW,AB" : "N0:NEW");
    const unsigned int prefix =
        (full ? image.size / DISKIMAGE_SECTOR_SIZE : 0u) + (format == 1 ? 4u : 6u) + (stage >= 4);
    for (unsigned int step = 0; image.writes < prefix; ++step) {
        ck_assert_uint_lt(step, DISKIMAGE_D82_SIZE / DISKIMAGE_SECTOR_SIZE + 100u);
        dos_work(&unit, &bank);
    }
    const unsigned int track = stage == 0 || stage == 3 ? 39u : 38u;
    const unsigned int sector = stage == 0 || stage == 1 || stage == 5 ? 0u
                                : stage == 3                           ? 1u
                                : stage == 4                           ? 3u
                                : format == 1                          ? 3u
                                                                       : 9u;
    ck_assert(diskimage_sector_offset(type, track, sector, &image.fail_read_offset));
    for (unsigned int step = 0; dos_work_pending(&unit); ++step) {
        ck_assert_uint_lt(step, 40);
        const unsigned int operations = image.reads + image.writes;
        dos_work(&unit, &bank);
        ck_assert_uint_le(image.reads + image.writes - operations, 1);
    }
    ck_assert_uint_eq(unit.status_code, stage == 5 ? DOS_STATUS_OK : DOS_STATUS_READ_ERROR);
    if (stage != 5) {
        char expected[40];
        snprintf(expected, sizeof(expected), "23,READ ERROR,%02u,%02u,0\r", track, sector);
        ck_assert_str_eq(unit.status, expected);
        ck_assert(!unit.bam[0].buffer.resident);
        ck_assert(!unit.free_blocks_valid[0]);
    }
    ck_assert_uint_eq(unit.buffer_use, 0);
    image.fail_read_offset = 0;
    send_command(&unit, &bank, "N0:RECOVER,AB");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// Distinct drive selectors must not authorize destructive copying of one backing image onto itself.
START_TEST(test_duplicate_rejects_aliased_image) {
    const unsigned int format = (unsigned int)_i % 3u;
    const unsigned int target = (unsigned int)_i / 3u % 2u;
    const bool readonly_source = _i >= 6;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_media_io_t pair[2] = {bank.units[0].slots[0].io, bank.units[0].slots[0].io};
    if (readonly_source) {
        pair[target ^ 1u].image.write = NULL;
    }
    ck_assert_uint_eq(dos_media_set_unit(&bank, 0, pair), DOS_MEDIA_OK);
    uint8_t* const saved = malloc(image.size);
    ck_assert_ptr_nonnull(saved);
    memcpy(saved, image.bytes, image.size);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, target == 0 ? "D0=1" : "D1=0");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(image.writes + image.reads + image.flushes, 0);
    ck_assert_mem_eq(image.bytes, saved, image.size);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(saved);
    free(image.bytes);
}
END_TEST

// Invalidating formatter scratch must retain the identity needed to flush its destructive prefix.
START_TEST(test_format_failure_finalization_flushes_captured_media) {
    const unsigned int format = (unsigned int)_i % 3u;
    const unsigned int failure = (unsigned int)_i / 3u;
    dos_media_bank_t bank = {0};
    image_t image, source;
    mount_blank(&bank, &image, format);
    if (failure == 3) {
        mount_peer(&bank, &source, format);
    }
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, failure == 3 ? "D0=1" : failure == 0 ? "N0:NEW,AB" : "N0:NEW");
    if (failure == 2) {
        const unsigned int writes = format == 0 ? 2u : format == 1 ? 5u : 7u;
        for (unsigned int step = 0; image.writes < writes; ++step) {
            ck_assert_uint_lt(step, 40);
            dos_work(&unit, &bank);
        }
        image.fail_read = image.reads + 1u;
    } else {
        image.fail_write = failure == 1 ? 1u : 3u;
    }
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, failure != 2  ? DOS_STATUS_WRITE_ERROR
                                        : format == 0 ? DOS_STATUS_OK
                                                      : DOS_STATUS_READ_ERROR);
    ck_assert(bank.units[0].slots[0].dirty);
    const unsigned int writes = image.writes;
    image.fail_flush = true;
    ck_assert(!dos_finalize(&unit, &bank));
    ck_assert_uint_eq(image.flushes, 1);
    ck_assert(bank.units[0].slots[0].dirty);
    image.fail_flush = false;
    ck_assert(dos_finalize(&unit, &bank));
    ck_assert_uint_eq(image.flushes, 2);
    ck_assert(!bank.units[0].slots[0].dirty);
    ck_assert(dos_finalize(&unit, &bank));
    ck_assert_uint_eq(image.flushes, 2);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
    if (failure == 3) {
        free(source.bytes);
    }
}
END_TEST

// Retained failed-format flush ownership must not acquire a replacement generation.
START_TEST(test_format_failure_finalization_rejects_replaced_media) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0}, replacement_bank = {0};
    image_t image, replacement;
    mount_blank(&bank, &image, format);
    mount_blank(&replacement_bank, &replacement, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    image.fail_write = 3;
    send_command(&unit, &bank, "N0:NEW,AB");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(dos_media_set_slot(&bank, 0, 0, &replacement_bank.units[0].slots[0].io), DOS_MEDIA_OK);
    ck_assert(!dos_finalize(&unit, &bank));
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(replacement.reads + replacement.writes + replacement.flushes, 0);
    free(image.bytes);
    free(replacement.bytes);
}
END_TEST

// DOS 2.7's late OPNIRD needs one logical index and two pages, preserving surviving peers.
START_TEST(test_format_reserves_native_reader_with_exhausted_peers) {
    const bool fault = _i >= 6;
    const unsigned int format = fault ? ((unsigned int)_i - 6u) % 2u + 1u : (unsigned int)_i % 3u;
    const bool pages = fault ? _i >= 8 : _i >= 3;
    dos_media_bank_t bank = {0};
    image_t target, peer;
    mount_blank(&bank, &target, format);
    mount_peer(&bank, &peer, format);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    send_command(&unit, &bank, "I1");
    finish_open(&unit, &bank);
    for (unsigned int index = 0; index < DOS_FILE_CHANNEL_COUNT; ++index) {
        ck_assert_uint_ne(open_name(&unit, &bank, index + 2u, "#"), DOS_UNUSED);
    }
    if (pages) {
        for (unsigned int index = 0; index < 6; ++index) {
            char name[8];
            snprintf(name, sizeof(name), "#%u", index);
            ck_assert_uint_eq(open_name(&unit, &bank, 8, name), DOS_UNUSED);
            ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
        }
        ck_assert(dos_close(&unit, &bank, model, 7));
    }
    const uint16_t use = unit.buffer_use;
    const unsigned int peer_operations = peer.writes + peer.flushes;
    dos_channel_t saved[DOS_FILE_CHANNEL_COUNT];
    memcpy(saved, unit.channels, sizeof(saved));
    send_command(&unit, &bank, "N0:NEW");
    if (fault) {
        const unsigned int prefix = target.writes + (format == 1 ? 4u : 6u);
        for (unsigned int step = 0; target.writes < prefix; ++step) {
            ck_assert_uint_lt(step, 40);
            dos_work(&unit, &bank);
        }
        target.fail_read = target.reads + 1u;
    }
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, format == 0 ? DOS_STATUS_OK
                                        : fault     ? DOS_STATUS_READ_ERROR
                                                    : DOS_STATUS_NO_CHANNEL);
    if (fault) {
        ck_assert_str_eq(unit.status, "23,READ ERROR,38,00,0\r");
        target.fail_read = 0;
    }
    ck_assert_uint_eq(unit.buffer_use, use);
    ck_assert_mem_eq(unit.channels, saved, sizeof(saved));
    ck_assert_uint_eq(peer.writes + peer.flushes, peer_operations);
    if (format != 0) {
        // Free just enough peer capacity, then fail after acquiring the internal reader.
        ck_assert(dos_close(&unit, &bank, model, 6));
        const uint16_t retained = unit.buffer_use;
        send_command(&unit, &bank, "N0:NEW");
        const unsigned int writes = target.writes + (format == 1 ? 5u : 7u);
        for (unsigned int step = 0; target.writes < writes; ++step) {
            ck_assert_uint_lt(step, 40);
            dos_work(&unit, &bank);
        }
        target.fail_read = target.reads + 1u;
        finish_open(&unit, &bank);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
        ck_assert_uint_eq(unit.buffer_use, retained);
        target.fail_read = 0;
        send_command(&unit, &bank, "N0:RECOVER");
        finish_open(&unit, &bank);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        ck_assert_uint_eq(unit.buffer_use, retained);
    }
    free(target.bytes);
    free(peer.bytes);
}
END_TEST

// A peer read failure must not change NEW's completion or version-rejection drive.
START_TEST(test_format_restores_drive_after_peer_error) {
    const unsigned int format = (unsigned int)_i % 3u;
    const unsigned int drive = (unsigned int)_i / 3u % 2u;
    const bool version_error = _i >= 6;
    const bool duplicate = _i >= 12;
    dos_media_bank_t bank = {0};
    image_t images[2];
    mount_blank(&bank, &images[0], format);
    mount_peer(&bank, &images[1], format);
    image_t* const target = &images[drive];
    image_t* const peer_image = &images[drive ^ 1u];
    fixture_file(peer_image, format, 0, "PEER", DISKIMAGE_FTYPE_SEQ, 0x80, 255);
    const diskimage_type_t type = bank.units[0].slots[drive].io.image.type;
    if (version_error) {
        set_map_version(target, type, format, 0xff);
    }
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    char name[24], command[24];
    snprintf(name, sizeof(name), "%u:PEER,S,R", drive ^ 1u);
    const uint8_t peer = open_name(&unit, &bank, 3, name);
    ck_assert_uint_ne(peer, DOS_UNUSED);
    uint8_t prefix[DISKIMAGE_SECTOR_PAYLOAD_SIZE];
    for (unsigned int index = 0; index < sizeof(prefix); ++index) {
        prefix[index] = 0x80u + index;
    }
    expect_direct(&unit, &bank, peer, prefix, sizeof(prefix), false);
    if (duplicate) {
        snprintf(command, sizeof(command), "D%u=%u", drive ^ 1u, drive);
    } else {
        snprintf(command, sizeof(command), "N%u:NEW", drive);
    }
    send_command(&unit, &bank, command);
    const unsigned int writes = format == 0 ? 2u : format == 1 ? 5u : 7u;
    for (unsigned int step = 0; version_error ? unit.initialize.pending : target->writes < writes; ++step) {
        ck_assert_uint_lt(step, 40);
        dos_work(&unit, &bank);
    }
    unit.buffers[unit.channels[peer].buffers[1]].resident = false;
    peer_image->fail_read = peer_image->reads + 1u;
    uint8_t byte;
    bool eoi;
    ck_assert_uint_eq(dos_stage(&unit, &bank, peer, &byte, 1, &eoi), 0);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_uint_eq(unit.selected_drive, drive ^ 1u);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, version_error ? DOS_STATUS_POWER_ON : DOS_STATUS_OK);
    if (!version_error) {
        char status[32];
        snprintf(status, sizeof(status), format == 0 ? "00, OK,00,00\r" : "00, OK,00,00,%u\r", drive);
        ck_assert_str_eq(unit.status, status);
    }
    ck_assert_uint_eq(unit.selected_drive, drive);
    if (!version_error) {
        ck_assert_uint_eq(unit.last_drive, drive);
        send_command(&unit, &bank, "N:AGAIN");
        finish_open(&unit, &bank);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        ck_assert_uint_eq(unit.last_drive, drive);
    }
    ck_assert_uint_eq(peer_image->writes + peer_image->flushes, 0);
    ck_assert_uint_eq(dos_find_channel(&unit, 3), peer);
    free(images[0].bytes);
    free(images[1].bytes);
}
END_TEST

// Failed scratch reads and writes must not poison later native writes or MAPOUT.
START_TEST(test_format_failure_reloads_bam_before_recovery) {
    const unsigned int format = (unsigned int)_i % 3u;
    const unsigned int failure = (unsigned int)_i / 3u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, failure == 3 ? "N0:NEW,AB" : "N0:NEW");
    if (failure == 0) {
        image.fail_read = image.reads + 1u;
    } else if (failure == 2) {
        const unsigned int writes = format == 0 ? 2u : format == 1 ? 5u : 7u;
        for (unsigned int step = 0; image.writes < writes; ++step) {
            ck_assert_uint_lt(step, 40);
            dos_work(&unit, &bank);
        }
        image.fail_read = image.reads + 1u;
    } else {
        image.fail_write = 1;
        image.fail_write_without_changes = true;
    }
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, failure == 2 && format == 0    ? DOS_STATUS_OK
                                        : failure == 0 || failure == 2 ? DOS_STATUS_READ_ERROR
                                                                       : DOS_STATUS_WRITE_ERROR);
    image.fail_read = image.fail_write = 0;
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "#"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x40, 4);
    send_command(&unit, &bank, "B-W:2,0,2,0");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    send_command(&unit, &bank, "B-A:0,1,0");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    uint8_t zero[DISKIMAGE_SECTOR_SIZE];
    memcpy(zero, sector_bytes(&image, type, 1, 0), sizeof(zero));
    ck_assert(dos_finalize(&unit, &bank));
    ck_assert_uint_eq(fixture_bam_entry(&image, format, 1)[1] & 1u, 0);
    ck_assert_mem_eq(sector_bytes(&image, type, 1, 0), zero, sizeof(zero));
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// TAGCMD's equals scan starts after the colon, unlike its retained pattern flag.
START_TEST(test_format_allows_equals_in_prefix) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, "N=0:OK");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    ck_assert_mem_eq(sector_bytes(&image, type, format == 0 ? 18 : 39, 0) + (format == 0 ? 144 : 6), "OK", 2);
    free(image.bytes);
}
END_TEST

// A formatter rejected before borrowing BAM bytes must retain dirty cached allocations.
START_TEST(test_format_prescratch_rejection_preserves_bam) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    ck_assert_uint_ne(open_name(&unit, &bank, IEEE_LOAD_CHANNEL, "#0"), DOS_UNUSED);
    send_command(&unit, &bank, "B-A:0,1,0");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    const dos_bam_t saved = unit.bam[0];
    const bool valid = unit.free_blocks_valid[0];
    send_command(&unit, &bank, "N0:NEW,AB");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
    ck_assert_mem_eq(&unit.bam[0], &saved, sizeof(saved));
    ck_assert_int_eq(unit.free_blocks_valid[0], valid);
    ck_assert(dos_finalize(&unit, &bank));
    ck_assert_uint_eq(fixture_bam_entry(&image, format, 1)[1] & 1u, 0);
    free(image.bytes);
}
END_TEST

// Register the read/status regressions and ordinary-write contracts.

// Build native data/side chains independently of REL allocation and OPEN.
static unsigned int fixture_rel_chains(image_t* const image, const unsigned int format, const unsigned int variant,
                                       uint8_t coordinates[REL_FIXTURE_BLOCK_LIMIT][2],
                                       unsigned int* const data_count) {
    const diskimage_type_t type = format == 0   ? diskimage_type_d64
                                  : format == 1 ? diskimage_type_d80
                                                : diskimage_type_d82;
    const unsigned int sides = variant == 2 ? 1u : format == 2 ? DOS_REL_SIDE_COUNT + 1u : 2u;
    *data_count = (sides - 1u) * DOS_REL_POINTER_COUNT + 2u;
    const unsigned int blocks = *data_count + sides + (format == 2);
    ck_assert_uint_le(blocks, REL_FIXTURE_BLOCK_LIMIT);
    unsigned int track = variant == 1 ? (format == 0 ? 25u : format == 1 ? 51u : 101u) : 2u;
    unsigned int sector = 0;
    for (unsigned int index = 0; index < *data_count; ++index) {
        coordinates[index][0] = track;
        coordinates[index][1] = sector;
        unsigned int sectors;
        ck_assert(diskimage_track_sectors(type, track, &sectors));
        if (++sector == sectors) {
            ++track;
            sector = 0;
        }
    }
    for (unsigned int index = 0; index < sides; ++index) {
        coordinates[*data_count + index][0] = 1;
        coordinates[*data_count + index][1] = index + 1u;
    }
    if (format == 2) {
        coordinates[blocks - 1u][0] = 1;
        coordinates[blocks - 1u][1] = 0;
    }
    for (unsigned int index = 0; index < blocks; ++index) {
        const uint8_t current_track = coordinates[index][0], current_sector = coordinates[index][1];
        uint8_t* const bam = fixture_bam_entry(image, format, current_track);
        const uint8_t mask = (uint8_t)(1u << (current_sector % 8u));
        ck_assert(bam[1u + current_sector / 8u] & mask);
        --bam[0];
        bam[1u + current_sector / 8u] &= (uint8_t)~mask;
        uint8_t* const page = sector_bytes(image, type, current_track, current_sector);
        memset(page, 0, DISKIMAGE_SECTOR_SIZE);
        if (index < *data_count) {
            if (index + 1u < *data_count) {
                memcpy(page, coordinates[index + 1u], DISKIMAGE_SECTOR_LINK_SIZE);
            } else {
                page[1] = UINT8_MAX;
            }
            // Length 127 divides the native 254-byte payload exactly.
            page[DISKIMAGE_SECTOR_LINK_SIZE] = page[DISKIMAGE_SECTOR_LINK_SIZE + REL_FIXTURE_RECORD_SIZE] = UINT8_MAX;
        } else if (index < *data_count + sides) {
            const unsigned int side = index - *data_count;
            const unsigned int group = side / DOS_REL_SIDE_COUNT;
            page[2] = side % DOS_REL_SIDE_COUNT;
            page[3] = REL_FIXTURE_RECORD_SIZE;
            if (side + 1u < sides) {
                memcpy(page, coordinates[index + 1u], DISKIMAGE_SECTOR_LINK_SIZE);
            } else {
                page[1] = DOS_REL_POINTER_OFFSET + 2u * (*data_count - side * DOS_REL_POINTER_COUNT) - 1u;
            }
            for (unsigned int other = group * DOS_REL_SIDE_COUNT;
                 other < sides && other < (group + 1u) * DOS_REL_SIDE_COUNT; ++other) {
                memcpy(page + DOS_REL_SIDE_TABLE_OFFSET + 2u * (other % DOS_REL_SIDE_COUNT),
                       coordinates[*data_count + other], DISKIMAGE_SECTOR_LINK_SIZE);
            }
            for (unsigned int pointer = 0;
                 pointer < DOS_REL_POINTER_COUNT && side * DOS_REL_POINTER_COUNT + pointer < *data_count; ++pointer) {
                memcpy(page + DOS_REL_POINTER_OFFSET + 2u * pointer,
                       coordinates[side * DOS_REL_POINTER_COUNT + pointer], DISKIMAGE_SECTOR_LINK_SIZE);
            }
        } else {
            memcpy(page, coordinates[*data_count], DISKIMAGE_SECTOR_LINK_SIZE);
            page[2] = DOS_REL_GROUP_SUPER;
            for (unsigned int side = 0; side < sides; side += DOS_REL_SIDE_COUNT) {
                memcpy(page + DOS_REL_GROUP_TABLE_OFFSET + 2u * (side / DOS_REL_SIDE_COUNT),
                       coordinates[*data_count + side], DISKIMAGE_SECTOR_LINK_SIZE);
            }
        }
    }
    uint8_t* const entry = sector_bytes(image, type, format == 0 ? 18 : 39, 1);
    entry[2] = DISKIMAGE_FTYPE_REL | (variant == 2 ? 0 : 0x80);
    memcpy(entry + 3, coordinates[0], DISKIMAGE_SECTOR_LINK_SIZE);
    memset(entry + 5, DISKIMAGE_FILENAME_PADDING, DISKIMAGE_FILENAME_SIZE);
    memcpy(entry + 5, "REL", 3);
    memcpy(entry + 21, coordinates[format == 2 ? blocks - 1u : *data_count], DISKIMAGE_SECTOR_LINK_SIZE);
    entry[23] = REL_FIXTURE_RECORD_SIZE;
    entry[30] = (uint8_t)blocks;
    entry[31] = (uint8_t)(blocks >> 8u);
    return blocks;
}

// Reduce the independent REL chain to three short, trailing-zero-trimmed records.
static void fixture_rel_short(image_t* const image, const unsigned int format, const uint8_t size) {
    uint8_t coordinates[REL_FIXTURE_BLOCK_LIMIT][2];
    unsigned int data_count;
    const unsigned int blocks = fixture_rel_chains(image, format, 2, coordinates, &data_count);
    const diskimage_type_t type = format == 0   ? diskimage_type_d64
                                  : format == 1 ? diskimage_type_d80
                                                : diskimage_type_d82;
    uint8_t* const unused = fixture_bam_entry(image, format, coordinates[1][0]);
    ++unused[0];
    unused[1u + coordinates[1][1] / 8u] |= (uint8_t)(1u << (coordinates[1][1] % 8u));
    uint8_t* const data = sector_bytes(image, type, coordinates[0][0], coordinates[0][1]);
    memset(data, 0, DISKIMAGE_SECTOR_SIZE);
    data[1] = 3u * size + 1u;
    data[2] = 'A';
    data[2u + size] = 'B';
    data[2u + 2u * size] = 'C';
    uint8_t* const side = sector_bytes(image, type, coordinates[data_count][0], coordinates[data_count][1]);
    side[1] = DOS_REL_POINTER_OFFSET + 1u;
    side[3] = size;
    memset(side + DOS_REL_POINTER_OFFSET + DISKIMAGE_SECTOR_LINK_SIZE, 0,
           DISKIMAGE_SECTOR_SIZE - DOS_REL_POINTER_OFFSET - DISKIMAGE_SECTOR_LINK_SIZE);
    uint8_t* const entry = sector_bytes(image, type, format == 0 ? 18 : 39, 1);
    entry[2] |= 0x80;
    entry[23] = size;
    entry[30] = blocks - 1u;
}

// Exercise public binary OPEN without treating a record-size byte as a C string.
static uint8_t open_rel(dos_unit_t* const unit, dos_media_bank_t* const bank, const uint8_t size) {
    const uint8_t name[] = {'0', ':', 'R', 'E', 'L', ',', 'L', ',', size};
    dos_input_address(unit, bank, bank->units[0].model, IEEE_CMD_OPEN(2), true);
    for (unsigned int index = 0; index < sizeof(name); ++index) {
        dos_input_put(unit, name[index], index + 1u == sizeof(name));
    }
    ck_assert(dos_input_ready(unit));
    dos_input_dispatch(unit, bank, 0);
    finish_open(unit, bank);
    return dos_find_channel(unit, 2);
}

// EOI records a request, while only the ordered dispatch boundary starts OPEN.
START_TEST(test_input_open_is_deferred_and_reset_is_unit_local) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_image(&bank, &image, (unsigned int)_i);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit, peer;
    dos_reset(&unit, model);
    dos_reset(&peer, model);
    const uint8_t name[] = "0:BASIC,PRG,R";

    // An unselected input stream cannot create a request.
    dos_input_put(&unit, 'X', true);
    ck_assert(!dos_input_ready(&unit));
    dos_input_address(&unit, &bank, model, IEEE_CMD_OPEN(IEEE_LOAD_CHANNEL), true);
    dos_input_address(&peer, &bank, model, IEEE_CMD_SECONDARY(IEEE_COMMAND_CHANNEL), true);
    dos_input_put(&peer, 'I', true);
    for (unsigned int index = 0; index < sizeof(name) - 1; ++index) {
        dos_input_put(&unit, name[index], false);
    }
    dos_input_dispatch(&unit, &bank, 0);
    ck_assert(!dos_work_pending(&unit));
    ck_assert_uint_eq(image.reads, 0);

    // Restarting OPEN replaces the incomplete name, then EOI only marks it.
    dos_input_address(&unit, &bank, model, IEEE_CMD_OPEN(IEEE_LOAD_CHANNEL), true);
    for (unsigned int index = 0; index < sizeof(name) - 1; ++index) {
        dos_input_put(&unit, name[index], index == sizeof(name) - 2);
    }
    ck_assert(dos_input_ready(&unit));
    ck_assert(!dos_work_pending(&unit));
    ck_assert_uint_eq(image.reads, 0);
    dos_input_dispatch(&unit, &bank, 0);
    ck_assert(!dos_input_ready(&unit));
    ck_assert(dos_work_pending(&unit));
    ck_assert_uint_eq(image.reads, 0);
    finish_open(&unit, &bank);
    ck_assert_uint_ne(dos_find_channel(&unit, IEEE_LOAD_CHANNEL), DOS_UNUSED);

    // Reset abandons only this unit's partial input and resources.
    dos_input_address(&unit, &bank, model, IEEE_CMD_OPEN(2), true);
    dos_input_put(&unit, 'X', true);
    dos_reset(&unit, model);
    ck_assert(!dos_input_ready(&unit));
    ck_assert(!dos_work_pending(&unit));
    ck_assert(dos_input_ready(&peer));
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// PUT retains one overflow byte, still notices EOI, and rejects oversized commands.
START_TEST(test_input_command_capacity_and_channel_selection) {
    dos_media_bank_t bank = {0};
    bank.units[0].model = _i < 3 ? DOS_MODEL_4040 : DOS_MODEL_8050;
    const dos_model_t model = bank.units[0].model;
    const unsigned int length = DOS_COMMAND_LIMIT + (unsigned int)(_i % 3);
    dos_unit_t unit;
    dos_reset(&unit, model);

    // TALK on 15 and LISTEN on an ordinary channel do not start collection.
    dos_input_address(&unit, &bank, model, IEEE_CMD_SECONDARY(IEEE_COMMAND_CHANNEL), false);
    dos_input_put(&unit, 'I', true);
    ck_assert(!dos_input_ready(&unit));
    dos_input_address(&unit, &bank, model, IEEE_CMD_SECONDARY(2), true);
    dos_input_put(&unit, 'I', true);
    ck_assert(!dos_input_ready(&unit));
    dos_input_address(&unit, &bank, model, IEEE_CMD_SECONDARY(IEEE_COMMAND_CHANNEL), true);
    for (unsigned int index = 0; index < length; ++index) {
        dos_input_put(&unit, 'M', index + 1 == length);
    }
    ck_assert(dos_input_ready(&unit));
    ck_assert_str_eq(unit.status, model == DOS_MODEL_4040 ? "73,CBM DOS V2,00,00\r" : "73,CBM DOS V2.7,00,00,0\r");
    dos_input_dispatch(&unit, &bank, 0);
    const char* const expected =
        model == DOS_MODEL_4040
            ? (length == DOS_COMMAND_LIMIT ? "30,SYNTAX ERROR,00,00\r" : "32,SYNTAX ERROR,00,00\r")
            : (length == DOS_COMMAND_LIMIT ? "30,SYNTAX ERROR,00,00,0\r" : "32,SYNTAX ERROR,00,00,0\r");
    ck_assert_str_eq(unit.status, expected);
    ck_assert(!dos_input_ready(&unit));
    ck_assert(!dos_work_pending(&unit));

    // A dispatched collection cannot accept trailing bytes or execute twice.
    dos_input_put(&unit, 'X', true);
    ck_assert(!dos_input_ready(&unit));
    dos_input_dispatch(&unit, &bank, 0);
    ck_assert_str_eq(unit.status, expected);
}
END_TEST

// OPEN 15 uses command semantics, while CLOSE retains its channel-specific rules.
START_TEST(test_input_command_open_and_close_semantics) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_image(&bank, &image, (unsigned int)_i);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t name[] = "BASIC";
    dos_open(&unit, &bank, 0, IEEE_LOAD_CHANNEL, name, sizeof(name) - 1);
    finish_open(&unit, &bank);
    dos_status(&unit, model, DOS_STATUS_SYNTAX, 0, 0);

    // CLOSE 0 releases its buffers without replacing the retained error.
    dos_input_address(&unit, &bank, model, IEEE_CMD_CLOSE(IEEE_LOAD_CHANNEL), true);
    ck_assert_uint_eq(dos_find_channel(&unit, IEEE_LOAD_CHANNEL), DOS_UNUSED);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_str_eq(unit.status, model == DOS_MODEL_4040 ? "30,SYNTAX ERROR,00,00\r" : "30,SYNTAX ERROR,00,00,0\r");
    dos_input_address(&unit, &bank, model, IEEE_CMD_OPEN(IEEE_COMMAND_CHANNEL), true);
    dos_input_put(&unit, 'M', true);
    dos_input_dispatch(&unit, &bank, 0);
    ck_assert(!dos_work_pending(&unit));
    ck_assert_str_eq(unit.status, model == DOS_MODEL_4040 ? "30,SYNTAX ERROR,00,00\r" : "30,SYNTAX ERROR,00,00,0\r");
    dos_input_address(&unit, &bank, model, IEEE_CMD_CLOSE(IEEE_COMMAND_CHANNEL), true);
    ck_assert_str_eq(unit.status, model == DOS_MODEL_4040 ? "00, OK,00,00\r" : "00, OK,00,00,0\r");
    free(image.bytes);
}
END_TEST

// FNDRCH source resolution stays in DOS, and preview never acknowledges GET.
START_TEST(test_output_retains_source_and_commits_only_actual_progress) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_image(&bank, &image, (unsigned int)_i);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t name[] = "BASIC";
    dos_open(&unit, &bank, 0, 2, name, sizeof(name) - 1);
    finish_open(&unit, &bank);
    dos_open(&unit, &bank, 0, 3, name, sizeof(name) - 1);
    finish_open(&unit, &bank);
    uint8_t bytes[DOS_STAGE_CAPACITY], repeated[DOS_STAGE_CAPACITY];
    bool eoi;
    dos_source_t first, second, status_source;

    // An unopened source produces neither data nor EOI.
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 4, bytes, sizeof(bytes), &eoi, &first), 0);
    ck_assert(!eoi);
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, bytes, 2, &eoi, &first), 2);
    ck_assert(!eoi);
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 3, repeated, 2, &eoi, &second), 2);
    ck_assert_mem_eq(bytes, repeated, 2);
    dos_output_get(&unit, &bank, model, first, 1);
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 2, repeated, 1, &eoi, &first), 1);
    ck_assert_uint_eq(repeated[0], bytes[1]);
    ck_assert_uint_eq(dos_output_stage(&unit, &bank, 3, repeated, 2, &eoi, &second), 2);
    ck_assert_mem_eq(bytes, repeated, 2);

    // Preview status repeatedly, then consume all but CR before acknowledging it.
    dos_status(&unit, model, DOS_STATUS_POWER_ON, 0, 0);
    const char* const power = model == DOS_MODEL_4040 ? "73,CBM DOS V2,00,00\r" : "73,CBM DOS V2.7,00,00,0\r";
    const unsigned int length =
        dos_output_stage(&unit, &bank, IEEE_COMMAND_CHANNEL, bytes, sizeof(bytes), &eoi, &status_source);
    ck_assert_uint_eq(length, strlen(power));
    ck_assert_mem_eq(bytes, power, length);
    ck_assert(eoi);
    ck_assert_uint_eq(dos_status_progress(&unit).position, 0);
    dos_output_get(&unit, &bank, model, status_source, length - 1);
    ck_assert_uint_eq(dos_status_progress(&unit).position, length - 1);
    ck_assert_uint_eq(
        dos_output_stage(&unit, &bank, IEEE_COMMAND_CHANNEL, repeated, sizeof(repeated), &eoi, &status_source), 1);
    ck_assert_uint_eq(repeated[0], '\r');
    ck_assert(eoi);
    ck_assert_str_eq(unit.status, power);
    dos_output_get(&unit, &bank, model, status_source, 1);
    ck_assert_uint_eq(dos_status_progress(&unit).position, 0);
    ck_assert_str_eq(unit.status, model == DOS_MODEL_4040 ? "00, OK,00,00\r" : "00, OK,00,00,0\r");
    free(image.bytes);
}
END_TEST

// OPEN cannot replace an owner whose failed CLSREL outlived its command tag.
START_TEST(test_open_retains_failed_rel_close) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_image(&bank, &image, (unsigned int)_i);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    dos_media_ref_t reference;
    ck_assert_int_eq(dos_media_reference(&bank, 0, 0, &reference), DOS_MEDIA_OK);
    const uint8_t channel = dos_rel_channel_claim(&unit, reference, 3);
    dos_channel_t* const state = &unit.channels[channel];
    state->rel_model = model;
    state->rel.record.result = DOS_REL_RECORD_MISSING;
    state->closing = DOS_CLOSE_REL_DIRECTORY_READ;
    state->failed = true;
    const dos_channel_t owner = *state;
    const uint16_t use = unit.buffer_use;
    const unsigned int reads = image.reads, writes = image.writes;
    const uint8_t name[] = "BASIC";
    dos_open(&unit, &bank, 0, 3, name, sizeof(name) - 1u);
    ck_assert(!dos_work_pending(&unit));
    ck_assert_int_eq(unit.status_code, DOS_STATUS_SYNTAX);
    ck_assert_mem_eq(state, &owner, sizeof(owner));
    ck_assert_uint_eq(unit.buffer_use, use);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    free(image.bytes);
}
END_TEST

// SCRTCH and VD15 walk the leading side/super chain, then the saved data chain.
START_TEST(test_rel_scratch_and_validate_native_chains) {
    const unsigned int format = (unsigned int)_i / 6u, operation = (unsigned int)_i / 3u % 2u;
    const unsigned int variant = (unsigned int)_i % 3u;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    uint8_t coordinates[REL_FIXTURE_BLOCK_LIMIT][2];
    unsigned int data_count;
    const unsigned int blocks = fixture_rel_chains(&image, format, variant, coordinates, &data_count);
    image_t oracle = image;
    oracle.bytes = malloc(image.size);
    ck_assert_ptr_nonnull(oracle.bytes);
    memcpy(oracle.bytes, image.bytes, image.size);
    if (operation == 0 || variant == 2) {
        sector_bytes(&oracle, type, format == 0 ? 18 : 39, 1)[2] = 0;
        // SCRATCH leaks an unclosed data chain but still releases its index.
        const unsigned int first = operation == 0 && variant == 2 ? data_count : 0;
        for (unsigned int index = first; index < blocks; ++index) {
            const uint8_t track = coordinates[index][0], sector = coordinates[index][1];
            uint8_t* const bam = fixture_bam_entry(&oracle, format, track);
            ++bam[0];
            bam[1u + sector / 8u] |= (uint8_t)(1u << (sector % 8u));
        }
    }
    if (operation == 1) {
        // An unrelated orphan must be released without touching any REL bytes.
        uint8_t* const orphan = fixture_bam_entry(&image, format, 35);
        ck_assert(orphan[1] & 1u);
        --orphan[0];
        orphan[1] &= (uint8_t)~1u;
    }
    dos_unit_t unit;
    dos_reset(&unit, model);
    const char* const command = operation == 0 ? "S0:REL=R" : "V0";
    send_command(&unit, &bank, command);
    finish_open(&unit, &bank);
    ck_assert_str_eq(unit.status, operation == 0
                                      ? (format == 0 ? "01, FILES SCRATCHED,01,00\r" : "01, FILES SCRATCHED,01,00,0\r")
                                      : (format == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r"));
    ck_assert_mem_eq(image.bytes, oracle.bytes, image.size);
    ck_assert_uint_eq(unit.buffer_use, 0);
    // Repeated commands are idempotent and preserve the same persistent image.
    send_command(&unit, &bank, command);
    finish_open(&unit, &bank);
    ck_assert_str_eq(unit.status, operation == 0
                                      ? (format == 0 ? "01, FILES SCRATCHED,00,00\r" : "01, FILES SCRATCHED,00,00,0\r")
                                      : (format == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r"));
    ck_assert_mem_eq(image.bytes, oracle.bytes, image.size);
    free(oracle.bytes);
    free(image.bytes);
}
END_TEST

// Corrupt zero-size entries use a bounded host rejection, not the native creation loop.
START_TEST(test_public_rel_zero_size_directory_policy) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    uint8_t coordinates[REL_FIXTURE_BLOCK_LIMIT][2];
    unsigned int data_count;
    fixture_rel_chains(&image, format, 2, coordinates, &data_count);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    uint8_t* const entry = sector_bytes(&image, type, format == 0 ? 18 : 39, 1);
    entry[2] |= 0x80;
    entry[23] = 0;
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t peer = open_name(&unit, &bank, 3, "#");
    const dos_channel_t saved_peer = unit.channels[peer];
    const uint16_t buffers = unit.buffer_use;
    const unsigned int writes = image.writes;
    ck_assert_uint_eq(open_name(&unit, &bank, 2, "0:REL"), DOS_UNUSED);
    ck_assert_str_eq(unit.status, format == 0 ? "71,DIR ERROR,00,00\r" : "71,DIR ERROR,00,00,0\r");
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    ck_assert_uint_eq(unit.buffer_use, buffers);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert(!dos_work_pending(&unit));
    free(image.bytes);
}
END_TEST

// OPNWCH/ADDFIL publish closed REL with count zero. Actual LISTEN survives remount.
START_TEST(test_public_rel_create_write_close_and_remount) {
    static const uint8_t sizes[] = {1, 13, 44, 58, 127, 254, 255};
    const unsigned int format = (unsigned int)_i / count_of(sizes);
    const uint8_t size = sizes[(unsigned int)_i % count_of(sizes)];
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t index = open_rel(&unit, &bank, size);
    ck_assert_uint_ne(index, DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    const dos_channel_t* const state = &unit.channels[index];
    ck_assert_uint_eq(state->file_type, DISKIMAGE_FTYPE_REL);
    ck_assert_uint_eq(state->rel.record.size, size);
    ck_assert_uint_eq(__builtin_popcount(unit.buffer_use), DOS_CHANNEL_BUFFER_COUNT);
    uint8_t* const directory = sector_bytes(&image, type, format == 0 ? 18 : 39, 1);
    ck_assert_uint_eq(directory[2], 0x84);
    ck_assert_uint_eq(directory[23], size);
    ck_assert_uint_eq(directory[30] | (unsigned int)directory[31] << 8u, 0);
    ck_assert_uint_eq(directory[21], state->rel_root_track);
    ck_assert_uint_eq(directory[22], state->rel_root_sector);
    ck_assert_mem_eq(directory + 5, "REL", 3);

    // EOI retains the byte through missing-record growth, padding and RD40.
    put_bytes(&unit, &bank, 2, 'A', 1);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    dos_close(&unit, &bank, model, 2);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), DOS_UNUSED);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(directory[30] | (unsigned int)directory[31] << 8u,
                      (format == 2 ? 3u : 2u) + (size > DISKIMAGE_SECTOR_SIZE - DISKIMAGE_SECTOR_LINK_SIZE));

    // A fresh identity and DOS state must read bytes from the persisted image.
    const dos_media_io_t io = bank.units[0].slots[0].io;
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, &io), DOS_MEDIA_OK);
    dos_reset(&unit, model);
    const uint8_t reader = open_name(&unit, &bank, 2, "0:REL");
    ck_assert_uint_ne(reader, DOS_UNUSED);
    bool eoi;
    uint8_t byte;
    ck_assert_uint_eq(dos_stage(&unit, &bank, reader, &byte, 1, &eoi), 1);
    ck_assert_uint_eq(byte, 'A');
    ck_assert(eoi);
    dos_get(&unit, &bank, reader, 1);
    finish_open(&unit, &bank);
    dos_close(&unit, &bank, model, 2);
    finish_open(&unit, &bank);
    free(image.bytes);
}
END_TEST

// Creation faults retain the same pages and ADDFIL context for explicit CLOSE retry.
START_TEST(test_public_rel_creation_faults_retain_publication_and_peer) {
    const unsigned int format = (unsigned int)_i / 6u, failure = (unsigned int)_i % 6u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t peer = open_name(&unit, &bank, 3, "#");
    const dos_channel_t saved_peer = unit.channels[peer];
    const uint16_t peer_pages = unit.buffer_use;
    const uint8_t name[] = "0:REL,L,\x7f";
    dos_open(&unit, &bank, 0, 2, name, sizeof(name) - 1u);
    const unsigned int first_write = image.writes;
    if (failure < 3 || failure == 4) {
        const unsigned int job = failure == 0   ? 1u
                                 : failure == 1 ? (format == 2 ? 3u : 2u)
                                 : failure == 2 ? (format == 2 ? 4u : 3u)
                                                : (format == 2 ? 5u : 4u);
        image.fail_write = first_write + job;
    } else if (failure == 3) {
        uint8_t index;
        do {
            ck_assert(dos_work_pending(&unit));
            dos_work(&unit, &bank);
            index = dos_find_channel(&unit, 2);
        } while (index == DOS_UNUSED || unit.channels[index].rel.record.result == DOS_REL_RECORD_PENDING);
        image.fail_read = image.reads + 1u;
    }
    finish_open(&unit, &bank);
    const uint8_t index = dos_find_channel(&unit, 2);
    ck_assert_uint_ne(index, DOS_UNUSED);
    if (failure == 5) {
        image.fail_flush = true;
        dos_close(&unit, &bank, model, 2);
        finish_open(&unit, &bank);
    }
    ck_assert_uint_eq(unit.status_code, failure == 3 ? DOS_STATUS_READ_ERROR : DOS_STATUS_WRITE_ERROR);
    ck_assert(unit.channels[index].failed);
    ck_assert(!unit.channels[index].ready);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    ck_assert_uint_eq(__builtin_popcount(unit.buffer_use & (uint16_t)~peer_pages), DOS_CHANNEL_BUFFER_COUNT);
    if (failure != 5) {
        // A different OPEN must not overwrite the retained filename/phase.
        const dos_channel_t retained = unit.channels[index];
        const uint8_t other[] = "0:OTHER,S,W";
        dos_open(&unit, &bank, 0, 4, other, sizeof(other) - 1u);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
        ck_assert_uint_eq(dos_find_channel(&unit, 4), DOS_UNUSED);
        ck_assert_mem_eq(&unit.channels[index], &retained, sizeof(retained));
        const uint8_t position[] = {'P', 2, 1, 0, 1};
        dos_input_address(&unit, &bank, model, IEEE_CMD_OPEN(15), true);
        for (unsigned int byte = 0; byte < sizeof(position); ++byte) {
            dos_input_put(&unit, position[byte], byte + 1u == sizeof(position));
        }
        dos_input_dispatch(&unit, &bank, 0);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
        ck_assert_mem_eq(&unit.channels[index], &retained, sizeof(retained));
        send_command(&unit, &bank, "C0:COPY=0:MISSING");
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
        ck_assert_mem_eq(&unit.channels[index], &retained, sizeof(retained));
    }
    image.fail_read = image.fail_write = 0;
    image.fail_flush = false;
    dos_close(&unit, &bank, model, 2);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), DOS_UNUSED);
    ck_assert_uint_eq(unit.buffer_use, peer_pages);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:REL,L,\x7f"), DOS_UNUSED);
    free(image.bytes);
}
END_TEST

// Host finalization persists NXDRBK's late BAM bit without changing public CLSREL.
START_TEST(test_public_rel_directory_growth_finalization_and_retry) {
    const unsigned int format = (unsigned int)_i / 3u, failure = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fill_directory(&image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t owner = open_rel(&unit, &bank, 13);
    ck_assert_uint_ne(owner, DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    const unsigned int track = unit.channels[owner].directory_track;
    const unsigned int sector = unit.channels[owner].directory_sector;
    ck_assert_uint_ne(sector, 1);
    uint8_t* const bam = fixture_bam_entry(&image, format, track);
    const uint8_t mask = (uint8_t)(1u << (sector % 8u));
    ck_assert(bam[1u + sector / 8u] & mask);
    dos_close(&unit, &bank, model, 2);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert(bam[1u + sector / 8u] & mask);
    ck_assert(unit.bam[0].dirty);

    // Both failed MAPOUT and failed backing flush remain explicitly retryable.
    if (failure != 0) {
        image.fail_write = failure == 1 ? image.writes + 1u : 0;
        image.fail_flush = failure == 2;
        ck_assert(!dos_finalize(&unit, &bank));
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
        image.fail_write = 0;
        image.fail_flush = false;
    }
    ck_assert(dos_finalize(&unit, &bank));
    ck_assert(!(bam[1u + sector / 8u] & mask));
    ck_assert(!unit.bam[0].dirty);
    const dos_media_io_t io = bank.units[0].slots[0].io;
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, &io), DOS_MEDIA_OK);
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:REL"), DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    free(image.bytes);
}
END_TEST

// Native zero-size creation stalls only its unit and never spins in finalization.
START_TEST(test_public_zero_size_rel_is_cooperative_and_resettable) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0}, other_bank = {0};
    image_t first, second;
    mount_blank(&bank, &first, format);
    mount_blank(&other_bank, &second, format);
    ck_assert_int_eq(dos_media_set_slot(&bank, 1, 0, &other_bank.units[0].slots[0].io), DOS_MEDIA_OK);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t pending, other;
    dos_reset(&pending, model);
    dos_reset(&other, model);
    const uint8_t zero[] = {'0', ':', 'R', 'E', 'L', ',', 'L', ',', 0};
    dos_open(&pending, &bank, 0, 2, zero, sizeof(zero));
    for (unsigned int step = 0; step < 100u; ++step) {
        ck_assert(dos_work_pending(&pending));
        dos_work(&pending, &bank);
    }
    const uint8_t owner = dos_find_channel(&pending, 2);
    ck_assert_uint_ne(owner, DOS_UNUSED);
    const unsigned int writes = first.writes;
    const dos_channel_t saved = pending.channels[owner];
    const uint8_t ordinary[] = "0:OTHER,S,W";
    dos_open(&other, &bank, 1, 2, ordinary, sizeof(ordinary) - 1u);
    finish_open(&other, &bank);
    ck_assert_uint_eq(other.status_code, DOS_STATUS_OK);
    put_bytes(&other, &bank, 2, 'Q', 1);
    ck_assert(dos_close(&other, &bank, model, 2));
    ck_assert_uint_eq(other.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(first.writes, writes);
    ck_assert_mem_eq(&pending.channels[owner], &saved, sizeof(saved));
    ck_assert(!dos_finalize(&pending, &bank));
    ck_assert(dos_work_pending(&pending));
    dos_reset(&pending, model);
    ck_assert(!dos_work_pending(&pending));
    ck_assert_uint_eq(pending.buffer_use, 0);
    free(first.bytes);
    free(second.bytes);
}
END_TEST

// CYEXT retains native preallocation quirks while routed COPY preserves a read-only source.
START_TEST(test_copy_rel_short_records_and_native_preallocation) {
    const unsigned int format = (unsigned int)_i / 2u;
    const uint8_t size = (unsigned int)_i % 2u == 0 ? 1u : 13u;
    dos_media_bank_t bank = {0}, source_bank = {0};
    image_t target, source;
    mount_blank(&bank, &target, format);
    mount_blank(&source_bank, &source, format);
    fixture_rel_short(&source, format, size);
    dos_media_io_t io = source_bank.units[0].slots[0].io;
    io.image.write = NULL;
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &io), DOS_MEDIA_OK);
    uint8_t* const saved_source = malloc(source.size);
    ck_assert_ptr_nonnull(saved_source);
    memcpy(saved_source, source.bytes, source.size);
    const dos_model_t model = bank.units[0].model;
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t peer = open_name(&unit, &bank, 3, "#");
    const dos_channel_t saved_peer = unit.channels[peer];
    const uint16_t peer_pages = unit.buffer_use;
    send_command(&unit, &bank, "C0:COPY=1:REL");
    finish_open(&unit, &bank);
    ck_assert_str_eq(unit.status, format == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r");
    ck_assert_uint_eq(unit.buffer_use, peer_pages);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    ck_assert_mem_eq(source.bytes, saved_source, source.size);
    ck_assert_uint_eq(source.writes, 0);
    const uint8_t* const entry = sector_bytes(&target, type, format == 0 ? 18 : 39, 1);
    ck_assert_uint_eq(entry[2], 0x84);
    ck_assert_uint_eq(entry[23], size);
    ck_assert_uint_eq(entry[30], format == 0 ? 2 : 3);
    const uint8_t* const data = sector_bytes(&target, type, entry[3], entry[4]);
    ck_assert_int_eq(data[0] != 0, format == 1);

    // A fresh public reader sees record EOI, not sector links or zero padding.
    const uint8_t reader = open_name(&unit, &bank, 2, "0:COPY");
    ck_assert_uint_ne(reader, DOS_UNUSED);
    for (unsigned int record = 0; record < 3u; ++record) {
        uint8_t byte;
        bool eoi;
        ck_assert_uint_eq(dos_stage(&unit, &bank, reader, &byte, 1, &eoi), 1);
        ck_assert_uint_eq(byte, 'A' + record);
        ck_assert(eoi);
        dos_get(&unit, &bank, reader, 1);
        finish_open(&unit, &bank);
    }
    dos_close(&unit, &bank, model, 2);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.buffer_use, peer_pages);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    free(saved_source);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// Native logical-channel pressure does not let failed COPY release public peers.
START_TEST(test_copy_rel_resource_pressure_preserves_public_owners) {
    const unsigned int format = (unsigned int)_i / 2u, peers = 4u + (unsigned int)_i % 2u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_rel_short(&image, format, 13);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    dos_channel_t saved[DOS_FILE_CHANNEL_COUNT];
    for (unsigned int index = 0; index < peers; ++index) {
        const uint8_t peer = open_name(&unit, &bank, 3u + index, "#");
        ck_assert_uint_ne(peer, DOS_UNUSED);
        saved[index] = unit.channels[peer];
        memset(unit.buffers[unit.channels[peer].buffers[0]].bytes, 'A' + index, DISKIMAGE_SECTOR_SIZE);
    }
    const uint16_t pages = unit.buffer_use;
    send_command(&unit, &bank, "C0:COPY=0:REL");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, peers == 4u ? DOS_STATUS_OK : DOS_STATUS_NO_CHANNEL);
    ck_assert_uint_eq(unit.buffer_use, pages);
    for (unsigned int index = 0; index < peers; ++index) {
        const uint8_t peer = dos_find_channel(&unit, 3u + index);
        ck_assert_uint_ne(peer, DOS_UNUSED);
        ck_assert_mem_eq(&unit.channels[peer], &saved[index], sizeof(saved[index]));
        const uint8_t* const bytes = unit.buffers[unit.channels[peer].buffers[0]].bytes;
        for (unsigned int byte = 0; byte < DISKIMAGE_SECTOR_SIZE; ++byte) {
            ck_assert_uint_eq(bytes[byte], 'A' + index);
        }
    }
    free(image.bytes);
}
END_TEST

// COPY's FREICH, unlike public CLOSE retry, abandons only its internal owners.
START_TEST(test_copy_rel_storage_faults_release_only_internal_pages) {
    const unsigned int format = (unsigned int)_i;
    for (unsigned int operation = 0; operation < 3u; ++operation) {
        bool completed = false;
        for (unsigned int job = 1; !completed && job < DISKIMAGE_SECTOR_SIZE; ++job) {
            dos_media_bank_t bank = {0};
            image_t image;
            mount_blank(&bank, &image, format);
            uint8_t coordinates[REL_FIXTURE_BLOCK_LIMIT][2];
            unsigned int data_count;
            fixture_rel_chains(&image, format, 2, coordinates, &data_count);
            const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
            uint8_t* const entry =
                sector_bytes(&image, type, format == 0 ? 18 : 39, 1) + DISKIMAGE_DIRECTORY_ENTRY_SIZE;
            entry[2] = 0x82;
            entry[3] = 35;
            memset(entry + 5, DISKIMAGE_FILENAME_PADDING, DISKIMAGE_FILENAME_SIZE);
            memcpy(entry + 5, "PEER", 4);
            uint8_t* const payload = sector_bytes(&image, type, 35, 0);
            payload[1] = 2;
            payload[2] = 'P';
            uint8_t* const bam = fixture_bam_entry(&image, format, 35);
            --bam[0];
            bam[1] &= (uint8_t)~1u;
            dos_unit_t unit;
            const dos_model_t model = bank.units[0].model;
            dos_reset(&unit, model);
            const uint8_t peer = open_name(&unit, &bank, 3, "0:PEER,P,R");
            const dos_channel_t saved_peer = unit.channels[peer];
            const uint16_t pages = unit.buffer_use;
            image.fail_write = operation == 0 ? image.writes + job : 0;
            image.fail_read = operation == 1 ? image.reads + job : 0;
            image.fail_flush = operation == 2;
            send_command(&unit, &bank, "C0:COPY=0:REL");
            finish_open(&unit, &bank);
            completed = unit.status_code == DOS_STATUS_OK || operation == 2;
            if (unit.status_code != DOS_STATUS_OK) {
                ck_assert_uint_eq(unit.status_code, operation == 1 ? DOS_STATUS_READ_ERROR : DOS_STATUS_WRITE_ERROR);
            }
            ck_assert_uint_eq(unit.buffer_use, pages);
            ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
            const unsigned int writes = image.writes;
            image_t expected = image;
            expected.bytes = malloc(image.size);
            ck_assert_ptr_nonnull(expected.bytes);
            memcpy(expected.bytes, image.bytes, image.size);
            unsigned int maps = 0;
            // Main's lifecycle policy persists retained BAMs, never freed REL pages.
            for (unsigned int slot = 0; slot < DOS_MEDIA_SLOTS_PER_UNIT; ++slot) {
                const dos_bam_t* const bam = &unit.bam[slot];
                if (bam->media.identity != 0 && bam->dirty) {
                    memcpy(sector_bytes(&expected, type, bam->buffer.track, bam->buffer.sector), bam->buffer.bytes,
                           DISKIMAGE_SECTOR_SIZE);
                    ++maps;
                }
            }
            image.fail_read = image.fail_write = 0;
            image.fail_flush = false;
            ck_assert(dos_finalize(&unit, &bank));
            ck_assert_uint_eq(image.writes, writes + maps);
            ck_assert_mem_eq(image.bytes, expected.bytes, image.size);
            free(expected.bytes);
            free(image.bytes);
        }
        ck_assert(completed);
    }
}
END_TEST

// RECORD resolves binary secondary bytes before testing file type, without I/O.
START_TEST(test_record_rejects_missing_and_nonrelative_channels) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool command_open = (unsigned int)_i % 2u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t direct = open_name(&unit, &bank, 2, "#");
    const uint8_t writer = open_name(&unit, &bank, 3, "0:WRITE,S,W");
    const dos_channel_t before = unit.channels[direct];
    const dos_channel_t writer_before = unit.channels[writer];
    const unsigned int reads = image.reads, writes = image.writes, flushes = image.flushes;
    static const uint8_t secondaries[] = {2, 0x62, 3, 15, 16, 17, 18, 0xff, 0, 13, 0x63};
    static const dos_status_code_t expected[] = {
        DOS_STATUS_TYPE_MISMATCH, DOS_STATUS_TYPE_MISMATCH, DOS_STATUS_NO_CHANNEL, DOS_STATUS_TYPE_MISMATCH,
        DOS_STATUS_TYPE_MISMATCH, DOS_STATUS_NO_CHANNEL,    DOS_STATUS_NO_CHANNEL, DOS_STATUS_TYPE_MISMATCH,
        DOS_STATUS_NO_CHANNEL,    DOS_STATUS_NO_CHANNEL,    DOS_STATUS_NO_CHANNEL,
    };
    for (unsigned int test = 0; test < count_of(secondaries); ++test) {
        // Zero record bytes must pass through command collection, not strlen.
        const uint8_t request[] = {'P', secondaries[test], 0, 0, 1};
        unit.selected_drive = 1;
        dos_input_address(&unit, &bank, bank.units[0].model,
                          command_open ? IEEE_CMD_OPEN(IEEE_COMMAND_CHANNEL) : IEEE_CMD_SECONDARY(IEEE_COMMAND_CHANNEL),
                          true);
        for (unsigned int byte = 0; byte < sizeof(request); ++byte) {
            dos_input_put(&unit, request[byte], byte + 1u == sizeof(request));
        }
        dos_input_dispatch(&unit, &bank, 0);
        ck_assert_uint_eq(unit.status_code, expected[test]);
        ck_assert_str_eq(unit.status,
                         expected[test] == DOS_STATUS_NO_CHANNEL
                             ? (format == 0 ? "70,NO CHANNEL,00,00\r" : "70,NO CHANNEL,00,00,1\r")
                             : (format == 0 ? "64, FILE TYPE MISMATCH,00,00\r" : "64, FILE TYPE MISMATCH,00,00,1\r"));
        ck_assert_mem_eq(&unit.channels[direct], &before, sizeof(before));
        ck_assert_mem_eq(&unit.channels[writer], &writer_before, sizeof(writer_before));
        ck_assert(!dos_work_pending(&unit));
    }
    // Missing record/position bytes do not preempt channel/type errors.
    send_input_command(&unit, &bank, "P\002", command_open);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_TYPE_MISMATCH);
    send_input_command(&unit, &bank, "P", command_open);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(image.flushes, flushes);
    free(image.bytes);
}
END_TEST

// A read-only initial data failure releases publication context for a fresh OPEN.
START_TEST(test_public_rel_read_failure_releases_open_context) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_rel_short(&image, format, 13);
    dos_media_io_t io = bank.units[0].slots[0].io;
    io.image.write = NULL;
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 0, &io), DOS_MEDIA_OK);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t peer = open_name(&unit, &bank, 3, "#");
    const dos_channel_t saved = unit.channels[peer];
    const uint16_t pages = unit.buffer_use;
    const diskimage_type_t type = io.image.type;
    uint32_t data;
    ck_assert(diskimage_sector_offset(type, 2, 0, &data));
    image.fail_read_offset = data;
    ck_assert_uint_eq(open_name(&unit, &bank, 2, "0:REL"), DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert(!dos_work_pending(&unit));
    ck_assert_uint_eq(unit.buffer_use, pages);
    ck_assert_mem_eq(&unit.channels[peer], &saved, sizeof(saved));
    image.fail_read_offset = 0;
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:REL"), DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    free(image.bytes);
}
END_TEST

// Both positioning entries must preserve an already-captured LISTEN byte's address.
START_TEST(test_listen_position_entries_preserve_captured_byte) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool buffer_position = (unsigned int)_i % 2u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_rel_short(&image, format, 13);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    const uint8_t owner = open_name(&unit, &bank, 2, "0:REL");
    ck_assert_uint_ne(owner, DOS_UNUSED);
    const dos_channel_t* const state = &unit.channels[owner];
    const dos_buffer_t* const active = &unit.buffers[state->buffers[state->rel.record.active]];
    const uint8_t track = active->track, sector = active->sector;
    dos_input_address(&unit, &bank, bank.units[0].model, IEEE_CMD_SECONDARY(2), true);
    dos_listen_put(&unit, &bank, 'Z', true);
    const dos_unit_t retained = unit;
    const unsigned int reads = image.reads, writes = image.writes;
    ck_assert(!(buffer_position ? dos_rel_channel_position_buffer(&unit, owner, 2, 2, track, sector)
                                : dos_rel_channel_position(&unit, owner, 2, 2)));
    ck_assert_mem_eq(&unit, &retained, sizeof(unit));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    dos_close(&unit, &bank, bank.units[0].model, 2);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), DOS_UNUSED);
    const uint8_t* const data = sector_bytes(&image, bank.units[0].slots[0].io.image.type, track, sector);
    ck_assert_uint_eq(data[2], 'Z');
    ck_assert_uint_eq(data[2 + 13], 'B');
    free(image.bytes);
}
END_TEST

// Reselected ordinary and direct listeners must not feed an interrupted request.
START_TEST(test_input_reselection_routes_current_data) {
    const unsigned int format = (unsigned int)_i / 4u;
    const bool direct = (unsigned int)_i % 2u, opening = (unsigned int)_i % 4u < 2u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    const uint8_t owner = open_name(&unit, &bank, 2, direct ? "#" : "0:DATA,S,W");
    ck_assert_uint_ne(owner, DOS_UNUSED);
    dos_input_address(&unit, &bank, model, opening ? IEEE_CMD_OPEN(4) : IEEE_CMD_SECONDARY(15), true);
    dos_input_put(&unit, 'X', false);
    dos_input_address(&unit, &bank, model, IEEE_CMD_SECONDARY(2), true);
    dos_listen_put(&unit, &bank, 'A', true);
    ck_assert(!dos_input_ready(&unit));
    ck_assert(!unit.input.collecting);
    ck_assert_uint_eq(dos_find_channel(&unit, 4), DOS_UNUSED);
    if (direct) {
        ck_assert_uint_eq(unit.buffers[unit.channels[owner].buffers[0]].bytes[1], 'A');
    }
    ck_assert(dos_close(&unit, &bank, model, 2));
    if (!direct) {
        const uint8_t expected = 'A';
        expect_copy_payload(&image, bank.units[0].slots[0].io.image.type, "DATA", &expected, 1, DISKIMAGE_FTYPE_SEQ);
    }
    free(image.bytes);
}
END_TEST

// A new command's EOI must be observed independently of the preceding collection.
START_TEST(test_input_new_command_clears_previous_completion) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool opening = (unsigned int)_i % 2u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    dos_input_address(&unit, &bank, model, opening ? IEEE_CMD_OPEN(4) : IEEE_CMD_SECONDARY(15), true);
    dos_input_put(&unit, 'X', true);
    ck_assert(dos_input_ready(&unit));
    dos_input_address(&unit, &bank, model, IEEE_CMD_SECONDARY(15), true);
    ck_assert(!dos_input_ready(&unit));
    dos_input_put(&unit, 'I', false);
    dos_input_dispatch(&unit, &bank, 0);
    ck_assert_uint_eq(image.reads, 0);
    dos_input_put(&unit, '0', true);
    dos_input_dispatch(&unit, &bank, 0);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(dos_find_channel(&unit, 4), DOS_UNUSED);
    free(image.bytes);
}
END_TEST

// REL copy-all CLOSE releases its writer once, even before another file reuses pages.
START_TEST(test_copy_all_rel_close_preserves_buffer_zero_peer) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool ordinary_next = (unsigned int)_i % 2u;
    dos_media_bank_t bank = {0}, source_bank = {0};
    image_t target, source;
    mount_blank(&bank, &target, format);
    mount_blank(&source_bank, &source, format);
    fixture_rel_short(&source, format, 13);
    const dos_model_t model = bank.units[0].model;
    if (ordinary_next) {
        dos_unit_t creator;
        dos_reset(&creator, model);
        ck_assert_uint_ne(open_name(&creator, &source_bank, 2, "0:ONE,S,W"), DOS_UNUSED);
        put_bytes(&creator, &source_bank, 2, 'Q', 1);
        ck_assert(dos_close(&creator, &source_bank, model, 2));
    }
    dos_media_io_t io = source_bank.units[0].slots[0].io;
    io.image.write = NULL;
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &io), DOS_MEDIA_OK);
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t peer = open_name(&unit, &bank, 3, "#0");
    ck_assert_uint_eq(unit.channels[peer].buffers[0], 0);
    dos_put(&unit, &bank, 3, 'P');
    const dos_channel_t saved_peer = unit.channels[peer];
    const dos_buffer_t saved_buffer = unit.buffers[0];
    const uint16_t pages = unit.buffer_use;
    send_command(&unit, &bank, "C0=1");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.buffer_use, pages);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    ck_assert_mem_eq(&unit.buffers[0], &saved_buffer, sizeof(saved_buffer));
    if (ordinary_next) {
        const uint8_t expected = 'Q';
        expect_copy_payload(&target, bank.units[0].slots[0].io.image.type, "ONE", &expected, 1, DISKIMAGE_FTYPE_SEQ);
    }
    const uint8_t reader = open_name(&unit, &bank, 2, "0:REL");
    ck_assert_uint_ne(reader, DOS_UNUSED);
    for (unsigned int record = 0; record < 3; ++record) {
        uint8_t byte;
        bool eoi;
        ck_assert_uint_eq(dos_stage(&unit, &bank, reader, &byte, 1, &eoi), 1);
        ck_assert_uint_eq(byte, 'A' + record);
        ck_assert(eoi);
        dos_get(&unit, &bank, reader, 1);
        finish_open(&unit, &bank);
    }
    dos_close(&unit, &bank, model, 2);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.buffer_use, pages);
    ck_assert_mem_eq(&unit.buffers[0], &saved_buffer, sizeof(saved_buffer));
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// Mixed COPY uses ordinary successor checks before retaining a seam byte for REL PUT.
START_TEST(test_copy_rel_ordinary_sector_boundary) {
    const unsigned int format = (unsigned int)_i / 3u, variant = (unsigned int)_i % 3u;
    const unsigned int length = variant == 0 ? 253u : 255u;
    dos_media_bank_t bank = {0}, source_bank = {0};
    image_t target, source;
    mount_blank(&bank, &target, format);
    mount_blank(&source_bank, &source, format);
    const dos_model_t model = bank.units[0].model;
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    dos_unit_t creator;
    dos_reset(&creator, model);
    ck_assert_uint_ne(open_rel(&creator, &source_bank, 254), DOS_UNUSED);
    put_bytes(&creator, &source_bank, 2, 'R', 1);
    dos_close(&creator, &source_bank, model, 2);
    finish_open(&creator, &source_bank);
    ck_assert_uint_eq(dos_find_channel(&creator, 2), DOS_UNUSED);
    ck_assert_uint_ne(open_name(&creator, &source_bank, 2, "0:SEQ,S,W"), DOS_UNUSED);
    put_bytes(&creator, &source_bank, 2, 1, length);
    ck_assert(dos_close(&creator, &source_bank, model, 2));
    const uint8_t* const directory = sector_bytes(&source, type, format == 0 ? 18 : 39, 1);
    const uint8_t* const entry = directory + DISKIMAGE_DIRECTORY_ENTRY_SIZE;
    ck_assert_mem_eq(entry + 5, "SEQ", 3);
    const uint8_t* const data = sector_bytes(&source, type, entry[3], entry[4]);
    if (variant == 2) {
        ck_assert_uint_ne(data[0], 0);
        ck_assert(diskimage_sector_offset(type, data[0], data[1], &source.fail_read_offset));
    }
    dos_media_io_t io = source_bank.units[0].slots[0].io;
    io.image.write = NULL;
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &io), DOS_MEDIA_OK);
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t peer = open_name(&unit, &bank, 3, "#");
    const dos_channel_t saved_peer = unit.channels[peer];
    const dos_buffer_t saved_buffer = unit.buffers[unit.channels[peer].buffers[0]];
    const uint16_t pages = unit.buffer_use;
    send_command(&unit, &bank, "C0:OUT=1:REL,1:SEQ");
    unsigned int seam_writes = 0;
    bool observed_seam = false;
    for (unsigned int step = 0; dos_work_pending(&unit); ++step) {
        ck_assert_uint_lt(step, 8192);
        if (variant == 2 && unit.copy.reader != DOS_UNUSED && unit.copy.writer != DOS_UNUSED) {
            const dos_channel_t* const reader = &unit.channels[unit.copy.reader];
            const dos_channel_t* const writer = &unit.channels[unit.copy.writer];
            if (!observed_seam && reader->file_type == DISKIMAGE_FTYPE_SEQ && reader->position == 255 &&
                writer->rel.input_phase == DOS_REL_INPUT_IDLE && writer->rel.record.result == DOS_REL_RECORD_WRITING) {
                observed_seam = true;
                seam_writes = target.writes;
            }
        }
        dos_work(&unit, &bank);
    }
    ck_assert_uint_eq(unit.status_code, variant == 2   ? DOS_STATUS_READ_ERROR
                                        : variant == 1 ? DOS_STATUS_RECORD_OVERFLOW
                                                       : DOS_STATUS_OK);
    ck_assert_uint_eq(unit.buffer_use, pages);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    ck_assert_mem_eq(&unit.buffers[unit.channels[peer].buffers[0]], &saved_buffer, sizeof(saved_buffer));
    if (variant == 2) {
        ck_assert(observed_seam);
        ck_assert_uint_eq(target.writes, seam_writes);
    } else if (variant == 0) {
        const uint8_t reader = open_name(&unit, &bank, 2, "0:OUT");
        ck_assert_uint_ne(reader, DOS_UNUSED);
        uint8_t byte;
        bool eoi;
        ck_assert_uint_eq(dos_stage(&unit, &bank, reader, &byte, 1, &eoi), 1);
        ck_assert_uint_eq(byte, 'R');
        ck_assert(eoi);
        dos_get(&unit, &bank, reader, 1);
        finish_open(&unit, &bank);
        for (unsigned int offset = 0; offset < length; ++offset) {
            ck_assert_uint_eq(dos_stage(&unit, &bank, reader, &byte, 1, &eoi), 1);
            ck_assert_uint_eq(byte, offset + 1u);
            ck_assert_int_eq(eoi, offset + 1u == length);
            dos_get(&unit, &bank, reader, 1);
            finish_open(&unit, &bank);
        }
        dos_close(&unit, &bank, model, 2);
        finish_open(&unit, &bank);
    }
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// INIT's deliberate channel abandonment retires only that owner's OPEN continuation.
START_TEST(test_initialize_retires_failed_rel_publication) {
    const unsigned int format = (unsigned int)_i / 4u, variant = (unsigned int)_i % 4u;
    dos_media_bank_t bank = {0}, peer_bank = {0};
    image_t image, peer_image;
    mount_blank(&bank, &image, format);
    mount_blank(&peer_bank, &peer_image, format);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &peer_bank.units[0].slots[0].io), DOS_MEDIA_OK);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    const uint8_t peer = open_name(&unit, &bank, 3, "$1");
    ck_assert_uint_ne(peer, DOS_UNUSED);
    const dos_channel_t saved_peer = unit.channels[peer];
    const uint16_t peer_pages = unit.buffer_use;
    const uint8_t name[] = "0:REL,L,\x7f";
    dos_open(&unit, &bank, 0, 2, name, sizeof(name) - 1u);
    image.fail_write = image.writes + (variant % 2u == 0 ? 1u : (format == 2 ? 5u : 4u));
    finish_open(&unit, &bank);
    const uint8_t owner = dos_find_channel(&unit, 2);
    ck_assert_uint_ne(owner, DOS_UNUSED);
    ck_assert(unit.channels[owner].failed);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    image.fail_write = 0;
    if (variant >= 2) {
        const dos_channel_t retained = unit.channels[owner];
        const dos_open_work_t publication = unit.open;
        send_command(&unit, &bank, "I1");
        finish_open(&unit, &bank);
        ck_assert_mem_eq(&unit.channels[owner], &retained, sizeof(retained));
        ck_assert_mem_eq(&unit.open, &publication, sizeof(publication));
        ck_assert_uint_eq(dos_find_channel(&unit, 3), DOS_UNUSED);
    }
    send_command(&unit, &bank, "I0");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), DOS_UNUSED);
    ck_assert_uint_eq(unit.open.append_channel, 0);
    ck_assert(!unit.open.pending);
    ck_assert_uint_eq(unit.buffer_use, variant >= 2 ? 0 : peer_pages);
    if (variant < 2) {
        ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    }
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:NEW,L,\x7f"), DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    put_bytes(&unit, &bank, 2, 'N', 1);
    ck_assert(dos_finalize(&unit, &bank));
    ck_assert_uint_eq(dos_find_channel(&unit, 2), DOS_UNUSED);
    ck_assert_uint_eq(unit.buffer_use, variant >= 2 ? 0 : peer_pages);
    free(peer_image.bytes);
    free(image.bytes);
}
END_TEST

// Replacing an EOI-completed collection cannot run it under the new secondary.
START_TEST(test_input_completed_request_reselection_discards_old_dispatch) {
    const unsigned int format = (unsigned int)_i / 6u, selection = (unsigned int)_i % 3u;
    const bool opening = (unsigned int)_i % 6u < 3u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t peer = open_name(&unit, &bank, 2, "#");
    ck_assert_uint_ne(peer, DOS_UNUSED);
    const dos_channel_t saved = unit.channels[peer];
    dos_input_address(&unit, &bank, model, opening ? IEEE_CMD_OPEN(4) : IEEE_CMD_SECONDARY(15), true);
    dos_input_put(&unit, 'I', false);
    dos_input_put(&unit, '0', true);
    ck_assert(dos_input_ready(&unit));
    const unsigned int reads = image.reads, writes = image.writes;
    dos_input_address(&unit, &bank, model, selection == 2 ? IEEE_CMD_CLOSE(4) : IEEE_CMD_SECONDARY(2), selection != 1);
    ck_assert(!dos_input_ready(&unit));
    dos_input_dispatch(&unit, &bank, 0);
    finish_open(&unit, &bank);
    ck_assert(!dos_input_ready(&unit));
    ck_assert_uint_eq(dos_find_channel(&unit, 2), peer);
    ck_assert_mem_eq(&unit.channels[peer], &saved, sizeof(saved));
    ck_assert_uint_eq(dos_find_channel(&unit, 4), DOS_UNUSED);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    free(image.bytes);
}
END_TEST

// Raw and BASIC directory OPEN abandon a healthy REL writer without ordinary CLOSE.
START_TEST(test_directory_reopen_after_rel_write_releases_only_previous_owner) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool basic = (unsigned int)_i % 2u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_rel_short(&image, format, 13);
    const dos_model_t model = bank.units[0].model;
    const uint8_t secondary = basic ? IEEE_LOAD_CHANNEL : 2;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, secondary, basic ? "0:REL,L" : "0:REL"), DOS_UNUSED);
    const uint8_t peer = open_name(&unit, &bank, 3, "#");
    ck_assert_uint_ne(peer, DOS_UNUSED);
    const dos_channel_t saved = unit.channels[peer];
    dos_input_address(&unit, &bank, model, IEEE_CMD_SECONDARY(secondary), true);
    dos_listen_put(&unit, &bank, 'Z', false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.channels[dos_find_channel(&unit, secondary)].access, DOS_ACCESS_MODIFY);
    const uint8_t directory = open_name(&unit, &bank, secondary, basic ? "$0:*" : "$0");
    ck_assert_uint_ne(directory, DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_ne(unit.channels[directory].file_type, DISKIMAGE_FTYPE_REL);
    ck_assert_uint_eq(unit.channels[directory].directory, basic ? DOS_DIRECTORY_HEADER : DOS_DIRECTORY_NONE);
    if (!basic) {
        const dos_buffer_t* const header = &unit.buffers[unit.channels[directory].buffers[0]];
        ck_assert_uint_eq(header->track, format == 0 ? 18 : 39);
        ck_assert_uint_eq(header->sector, 0);
    }
    ck_assert_mem_eq(&unit.channels[peer], &saved, sizeof(saved));
    free(image.bytes);
}
END_TEST

// CLRCB's retained overflow CR supplies the next bounded OPEN's record-size byte.
START_TEST(test_input_open_retains_overflow_byte_for_next_rel_request) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    uint8_t first[DOS_COMMAND_RECEIVE_CAPACITY];
    memset(first, 'X', sizeof(first));
    first[0] = '0';
    first[1] = ':';
    first[DOS_COMMAND_LIMIT] = '\r';
    dos_input_address(&unit, &bank, model, IEEE_CMD_OPEN(4), true);
    for (unsigned int byte = 0; byte < sizeof(first); ++byte) {
        dos_input_put(&unit, first[byte], byte + 1u == sizeof(first));
    }
    dos_input_dispatch(&unit, &bank, 0);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.input.bytes[DOS_COMMAND_LIMIT], '\r');

    // The size lies just beyond logical EOF, in the preserved receive byte.
    uint8_t second[DOS_COMMAND_LIMIT];
    memset(second, 'X', sizeof(second));
    second[0] = '0';
    second[1] = ':';
    memcpy(second + sizeof(second) - 3u, ",L,", 3);
    dos_input_address(&unit, &bank, model, IEEE_CMD_OPEN(2), true);
    for (unsigned int byte = 0; byte < sizeof(second); ++byte) {
        dos_input_put(&unit, second[byte], byte + 1u == sizeof(second));
    }
    dos_input_dispatch(&unit, &bank, 0);
    finish_open(&unit, &bank);
    const uint8_t owner = dos_find_channel(&unit, 2);
    ck_assert_uint_ne(owner, DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.channels[owner].file_type, DISKIMAGE_FTYPE_REL);
    ck_assert_uint_eq(unit.channels[owner].rel.record.size, '\r');
    ck_assert(!dos_work_pending(&unit));
    free(image.bytes);
}
END_TEST

// Worker retirement must preserve a newer OPEN/command until its ordered dispatch.
START_TEST(test_input_new_collection_survives_pending_worker_retirement) {
    const unsigned int format = (unsigned int)_i / 8u, worker = (unsigned int)_i % 8u / 2u;
    const bool opening = (unsigned int)_i % 2u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    if (worker == 2) {
        fixture_file(&image, format, 0, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x20, 4);
        send_command(&unit, &bank, "C0:OUT=0:SOURCE");
    } else {
        fixture_rel_short(&image, format, 13);
        if (worker == 1) {
            const uint8_t name[] = "0:REL";
            dos_open(&unit, &bank, 0, 2, name, sizeof(name) - 1u);
        } else {
            ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:REL"), DOS_UNUSED);
            if (worker == 0) {
                const uint8_t position[] = {'P', 2, 2, 0, 1};
                dos_command(&unit, &bank, 0, position, sizeof(position));
            } else {
                dos_close(&unit, &bank, model, 2);
            }
        }
    }
    ck_assert(dos_work_pending(&unit));
    dos_input_address(&unit, &bank, model, opening ? IEEE_CMD_OPEN(4) : IEEE_CMD_SECONDARY(15), true);
    dos_input_put(&unit, opening ? '#' : 'I', opening);
    if (!opening) {
        dos_input_put(&unit, '0', true);
    }
    ck_assert(dos_input_ready(&unit));
    const dos_input_t retained = unit.input;
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_mem_eq(&unit.input, &retained, sizeof(retained));
    dos_input_dispatch(&unit, &bank, 0);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_int_eq(dos_find_channel(&unit, 4) != DOS_UNUSED, opening);
    if (!opening) {
        ck_assert_uint_eq(dos_find_channel(&unit, 2), DOS_UNUSED);
    }
    free(image.bytes);
}
END_TEST

// Public peer errors and status GET cannot replace a retained REL COPY outcome.
START_TEST(test_copy_rel_completion_uses_writer_outcome_not_public_status) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool overflow = (unsigned int)_i % 2u;
    dos_media_bank_t bank = {0}, peer_bank = {0};
    image_t image, peer_image;
    mount_blank(&bank, &image, format);
    fixture_rel_short(&image, format, 13);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    uint8_t peer = DOS_UNUSED;
    if (overflow) {
        ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:SEQ,S,W"), DOS_UNUSED);
        put_bytes(&unit, &bank, 2, 'S', 14);
        ck_assert(dos_close(&unit, &bank, model, 2));
    } else {
        mount_blank(&peer_bank, &peer_image, format);
        fixture_file(&peer_image, format, 0, "PEER", DISKIMAGE_FTYPE_SEQ, 0x20, 255);
        ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &peer_bank.units[0].slots[0].io), DOS_MEDIA_OK);
        peer_image.fail_read = peer_image.reads + 3u;
        peer = open_name(&unit, &bank, 3, "1:PEER,S,R");
        ck_assert_uint_ne(peer, DOS_UNUSED);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        peer_image.fail_read = 0;
    }
    send_command(&unit, &bank, overflow ? "C0:OUT=0:REL,0:SEQ" : "C0:OUT=0:REL");
    bool interleaved = false;
    for (unsigned int step = 0; dos_work_pending(&unit); ++step) {
        ck_assert_uint_lt(step, 8192);
        if (!interleaved && unit.command == DOS_COMMAND_COPY && unit.copy.writer != DOS_UNUSED) {
            const dos_channel_t* const writer = &unit.channels[unit.copy.writer];
            if (writer->allocated && writer->ready && writer->file_type == DISKIMAGE_FTYPE_REL &&
                writer->rel.input_phase == DOS_REL_INPUT_IDLE && writer->rel.input_eoi &&
                writer->rel.input_overflow == overflow) {
                const dos_channel_t retained = *writer;
                if (overflow) {
                    ck_assert_uint_eq(unit.status_code, DOS_STATUS_RECORD_OVERFLOW);
                    uint8_t status[DOS_STATUS_LINE_CAPACITY];
                    bool eoi;
                    const unsigned int length = dos_status_stage(&unit, status, sizeof(status), &eoi);
                    ck_assert(eoi);
                    dos_status_get(&unit, model, length);
                    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
                } else {
                    unsigned int consumed = 0;
                    while (consumed < DISKIMAGE_SECTOR_PAYLOAD_SIZE) {
                        uint8_t bytes[DOS_STAGE_CAPACITY];
                        bool eoi;
                        const unsigned int length = dos_stage(&unit, &bank, peer, bytes, sizeof(bytes), &eoi);
                        ck_assert_uint_gt(length, 0);
                        dos_get(&unit, &bank, peer, length);
                        consumed += length;
                    }
                    uint8_t byte;
                    bool eoi;
                    ck_assert_uint_eq(dos_stage(&unit, &bank, peer, &byte, 1, &eoi), 0);
                    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
                }
                ck_assert_mem_eq(writer, &retained, sizeof(retained));
                interleaved = true;
            }
        }
        dos_work(&unit, &bank);
    }
    ck_assert(interleaved);
    ck_assert_uint_eq(unit.status_code, overflow ? DOS_STATUS_RECORD_OVERFLOW : DOS_STATUS_OK);
    if (!overflow) {
        const uint8_t reader = open_name(&unit, &bank, 2, "0:OUT");
        ck_assert_uint_ne(reader, DOS_UNUSED);
        for (unsigned int record = 0; record < 3; ++record) {
            uint8_t byte;
            bool eoi;
            ck_assert_uint_eq(dos_stage(&unit, &bank, reader, &byte, 1, &eoi), 1);
            ck_assert_uint_eq(byte, 'A' + record);
            ck_assert(eoi);
            dos_get(&unit, &bank, reader, 1);
            finish_open(&unit, &bank);
        }
        free(peer_image.bytes);
    }
    free(image.bytes);
}
END_TEST

// Failed REL publication reserves its name until retry or actual abandonment.
START_TEST(test_rename_cannot_claim_failed_rel_publication_name) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool partial = (unsigned int)_i % 2u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x20, 4);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t name[] = "0:REL,L,\x7f";
    image.fail_write = image.writes + (format == 2 ? 5u : 4u);
    image.fail_write_without_changes = !partial;
    dos_open(&unit, &bank, 0, 2, name, sizeof(name) - 1u);
    finish_open(&unit, &bank);
    const uint8_t owner = dos_find_channel(&unit, 2);
    ck_assert_uint_ne(owner, DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert(unit.channels[owner].failed);
    const dos_channel_t retained = unit.channels[owner];
    const dos_open_work_t publication = unit.open;
    image.fail_write = 0;
    const unsigned int writes = image.writes;
    send_command(&unit, &bank, "R0:REL=0:ABSENT");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NOT_FOUND);
    send_command(&unit, &bank, partial ? "R0:OTHER=0:REL" : "R0:REL=0:SOURCE");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_mem_eq(&unit.channels[owner], &retained, sizeof(retained));
    ck_assert_mem_eq(&unit.open, &publication, sizeof(publication));

    // Retry publication without another live entry taking its retained name.
    dos_close(&unit, &bank, model, 2);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), DOS_UNUSED);
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    const uint8_t* const directory = sector_bytes(&image, type, format == 0 ? 18 : 39, 1);
    unsigned int relatives = 0, sources = 0;
    for (unsigned int offset = 0; offset < DISKIMAGE_SECTOR_SIZE; offset += DISKIMAGE_DIRECTORY_ENTRY_SIZE) {
        const uint8_t* const entry = directory + offset;
        if (entry[2] != DISKIMAGE_FTYPE_DEL) {
            relatives += memcmp(entry + 5, "REL", 3) == 0;
            sources += memcmp(entry + 5, "SOURCE", 6) == 0;
        }
    }
    ck_assert_uint_eq(relatives, 1);
    ck_assert_uint_eq(sources, 1);
    const uint8_t source = open_name(&unit, &bank, 3, "0:SOURCE");
    ck_assert_uint_ne(source, DOS_UNUSED);
    expect_bytes(&unit, &bank, source, 4, 0x20);
    free(image.bytes);
}
END_TEST

// TSTCHN protects a partially visible ADDFIL slot while its failed owner retains pages.
START_TEST(test_scratch_cannot_free_partial_rel_publication) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x20, 4);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t name[] = "0:REL,L,\x7f";
    image.fail_write = image.writes + (format == 2 ? 5u : 4u);
    dos_open(&unit, &bank, 0, 2, name, sizeof(name) - 1u);
    finish_open(&unit, &bank);
    const uint8_t owner = dos_find_channel(&unit, 2);
    ck_assert_uint_ne(owner, DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert(unit.channels[owner].failed);
    const dos_channel_t retained = unit.channels[owner];
    const dos_open_work_t publication = unit.open;
    const dos_bam_t map = unit.bam[0];
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    uint8_t directory[DISKIMAGE_SECTOR_SIZE];
    memcpy(directory, sector_bytes(&image, type, format == 0 ? 18 : 39, 1), sizeof(directory));
    ck_assert_mem_eq(directory + unit.open.empty_entry + 5, "REL", 3);
    image.fail_write = 0;
    const unsigned int writes = image.writes;
    send_command(&unit, &bank, "S0:REL");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_FILES_SCRATCHED);
    ck_assert_str_eq(unit.status,
                     model == DOS_MODEL_4040 ? "01, FILES SCRATCHED,00,00\r" : "01, FILES SCRATCHED,00,00,0\r");
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_mem_eq(sector_bytes(&image, type, format == 0 ? 18 : 39, 1), directory, sizeof(directory));
    ck_assert_mem_eq(&unit.bam[0], &map, sizeof(map));
    ck_assert_mem_eq(&unit.channels[owner], &retained, sizeof(retained));
    ck_assert_mem_eq(&unit.open, &publication, sizeof(publication));
    dos_close(&unit, &bank, model, 2);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), DOS_UNUSED);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:REL"), DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    free(image.bytes);
}
END_TEST

// Cached REL source bytes must not turn stale media into successful partial COPY.
START_TEST(test_copy_rel_cached_source_replacement_reports_failure) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0}, source_bank = {0}, replacement_bank = {0};
    image_t target, source, replacement;
    mount_blank(&bank, &target, format);
    mount_blank(&source_bank, &source, format);
    mount_blank(&replacement_bank, &replacement, format);
    fixture_rel_short(&source, format, 13);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &source_bank.units[0].slots[0].io), DOS_MEDIA_OK);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    send_command(&unit, &bank, "C0:OUT=1:REL");
    bool replaced = false;
    unsigned int writes = 0;
    for (unsigned int step = 0; dos_work_pending(&unit); ++step) {
        ck_assert_uint_lt(step, 8192);
        if (!replaced && unit.command == DOS_COMMAND_COPY && unit.copy.writer != DOS_UNUSED) {
            const dos_channel_t* const writer = &unit.channels[unit.copy.writer];
            const dos_channel_t* const reader = &unit.channels[unit.copy.reader];
            if (writer->allocated && writer->ready && writer->file_type == DISKIMAGE_FTYPE_REL &&
                writer->rel.input_phase == DOS_REL_INPUT_IDLE && !writer->rel.input_eoi &&
                reader->rel.record.result == DOS_REL_RECORD_READY) {
                writes = target.writes;
                ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &replacement_bank.units[0].slots[0].io), DOS_MEDIA_OK);
                replaced = true;
            }
        }
        dos_work(&unit, &bank);
    }
    ck_assert(replaced);
    ck_assert_uint_ne(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(target.writes, writes);
    ck_assert_uint_eq(replacement.writes, 0);
    free(replacement.bytes);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

// An unpublished REL owns its planned entry, not an unrelated lookup cursor.
START_TEST(test_pending_rel_does_not_bind_unrelated_directory_entry) {
    const unsigned int format = (unsigned int)_i / 4u, operation = (unsigned int)_i % 4u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_file(&image, format, 0, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x20, 4);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t name[] = "0:REL,L,\x7f";
    image.fail_write = image.writes + (operation < 2 ? 1u : (format == 2 ? 5u : 4u));
    dos_open(&unit, &bank, 0, 2, name, sizeof(name) - 1u);
    finish_open(&unit, &bank);
    const uint8_t owner = dos_find_channel(&unit, 2);
    ck_assert_uint_ne(owner, DOS_UNUSED);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    const dos_channel_t retained = unit.channels[owner];
    const dos_open_work_t publication = unit.open;
    image.fail_write = 0;

    // Mutate only SOURCE while the failed creation retains its separate slot.
    send_command(&unit, &bank, operation % 2u == 0 ? "S0:SOURCE" : "R0:OTHER=0:SOURCE");
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, operation % 2u == 0 ? DOS_STATUS_FILES_SCRATCHED : DOS_STATUS_OK);
    if (operation % 2u == 0) {
        ck_assert_str_eq(unit.status,
                         model == DOS_MODEL_4040 ? "01, FILES SCRATCHED,01,00\r" : "01, FILES SCRATCHED,01,00,0\r");
    }
    ck_assert_mem_eq(&unit.channels[owner], &retained, sizeof(retained));
    ck_assert_mem_eq(&unit.open, &publication, sizeof(publication));
    dos_close(&unit, &bank, model, 2);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:REL"), DOS_UNUSED);
    if (operation % 2u != 0) {
        const uint8_t source = open_name(&unit, &bank, 3, "0:OTHER");
        ck_assert_uint_ne(source, DOS_UNUSED);
        expect_bytes(&unit, &bank, source, 4, 0x20);
    }
    free(image.bytes);
}
END_TEST

// Failed REL OPEN retires parser RAM before a short RECORD can reuse its tail.
START_TEST(test_failed_rel_open_retires_command_without_repositioning_peer) {
    const unsigned int format = (unsigned int)_i / 7u, fault = (unsigned int)_i % 7u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_rel_short(&image, format, 13);
    fixture_file(&image, format, 1, "SOURCE", DISKIMAGE_FTYPE_SEQ, 0x20, 4);
    const dos_model_t model = bank.units[0].model;
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t peer = open_name(&unit, &bank, 10, "0:REL");
    ck_assert_uint_ne(peer, DOS_UNUSED);
    if (fault == 4) {
        for (uint8_t secondary = 3; secondary < 8; ++secondary) {
            ck_assert_uint_ne(open_name(&unit, &bank, secondary, "#"), DOS_UNUSED);
        }
    } else if (fault == 5) {
        ck_assert_uint_ne(open_name(&unit, &bank, 3, "0:SOURCE"), DOS_UNUSED);
        ck_assert_uint_ne(open_name(&unit, &bank, 4, "0:SOURCE"), DOS_UNUSED);
        ck_assert_uint_ne(open_name(&unit, &bank, 5, "#"), DOS_UNUSED);
    }
    const dos_channel_t saved_peer = unit.channels[peer];
    unit.command_bytes[DOS_COMMAND_LIMIT] = 0x5a;
    if (fault == 0 || fault == 6) {
        ck_assert(diskimage_sector_offset(type, fault == 6 ? (format == 0 ? 18 : 38) : 2, 0, &image.fail_read_offset));
    } else if (fault == 2 || fault == 3) {
        image.fail_write = image.writes + (fault == 2 ? 1u : (format == 2 ? 5u : 4u));
    }
    const uint8_t* const request = (const uint8_t*)(fault == 2 || fault == 3 || fault == 6 ? "0:NEW,L,\x7f" : "0:REL");
    dos_open(&unit, &bank, 0, 2, request, (unsigned int)strlen((const char*)request));
    if (fault == 1) {
        dos_work(&unit, &bank);
        ck_assert_uint_ne(unit.open.append_channel, 0);
        image.fail_read = image.reads + 1u;
    }
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, fault == 1 && format == 2  ? DOS_STATUS_OK
                                        : fault == 4 || fault == 5 ? DOS_STATUS_NO_CHANNEL
                                        : fault == 2 || fault == 3 ? DOS_STATUS_WRITE_ERROR
                                                                   : DOS_STATUS_READ_ERROR);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    image.fail_read_offset = 0;
    image.fail_read = 0;
    image.fail_write = 0;

    // A one-byte P must resolve cleared SA zero, not ':' from the failed OPEN.
    send_input_command(&unit, &bank, "P", false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    ck_assert_uint_eq(unit.command_bytes[DOS_COMMAND_LIMIT], 0x5a);
    if (fault == 2 || fault == 3) {
        dos_close(&unit, &bank, model, 2);
        finish_open(&unit, &bank);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    }
    free(image.bytes);
}
END_TEST

// Failed creation retirement cannot erase a newer complete or partial RECORD.
START_TEST(test_failed_rel_open_preserves_newer_record_collection) {
    const unsigned int format = (unsigned int)_i / 2u;
    const bool complete = (unsigned int)_i % 2u;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    fixture_rel_short(&image, format, 13);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t peer = open_name(&unit, &bank, 10, "0:REL");
    ck_assert_uint_ne(peer, DOS_UNUSED);
    const uint8_t name[] = "0:NEW,L,\x7f";
    image.fail_write = image.writes + (format == 2 ? 5u : 4u);
    dos_open(&unit, &bank, 0, 2, name, sizeof(name) - 1u);
    dos_input_address(&unit, &bank, model, IEEE_CMD_SECONDARY(IEEE_COMMAND_CHANNEL), true);
    const uint8_t position[] = {'P', 10, 2, 0, 1};
    const unsigned int prefix = complete ? sizeof(position) : 3u;
    for (unsigned int index = 0; index < prefix; ++index) {
        dos_input_put(&unit, position[index], index + 1u == sizeof(position));
    }
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_WRITE_ERROR);
    ck_assert_mem_eq(unit.input.bytes, position, prefix);
    ck_assert_uint_eq(unit.input.length, prefix);
    ck_assert_uint_eq(dos_input_ready(&unit), complete);
    for (unsigned int index = prefix; index < sizeof(position); ++index) {
        dos_input_put(&unit, position[index], index + 1u == sizeof(position));
    }
    dos_input_dispatch(&unit, &bank, 0);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    bool eoi;
    uint8_t byte;
    ck_assert_uint_eq(dos_stage(&unit, &bank, peer, &byte, 1, &eoi), 1);
    ck_assert_uint_eq(byte, 'B');
    ck_assert(eoi);
    image.fail_write = 0;
    dos_close(&unit, &bank, model, 2);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    free(image.bytes);
}
END_TEST

// CYEXT must not turn an initially missing record into a successful EOF.
START_TEST(test_copy_rel_missing_first_record_reports_native_error) {
    const unsigned int format = (unsigned int)_i / 3u, mode = (unsigned int)_i % 3u;
    dos_media_bank_t bank = {0}, source_bank = {0};
    image_t target, source;
    mount_blank(&bank, &target, format);
    mount_blank(&source_bank, &source, format);
    fixture_rel_short(&source, format, 13);
    const diskimage_type_t type = source_bank.units[0].slots[0].io.image.type;
    uint8_t* const entry = sector_bytes(&source, type, format == 0 ? 18 : 39, 1);
    uint8_t* side = sector_bytes(&source, type, entry[21], entry[22]);
    if (format == 2) {
        side = sector_bytes(&source, type, side[DOS_REL_GROUP_TABLE_OFFSET], side[DOS_REL_GROUP_TABLE_OFFSET + 1u]);
    }
    entry[23] = UINT8_MAX;
    side[3] = UINT8_MAX;
    uint8_t* const data = sector_bytes(&source, type, entry[3], entry[4]);
    memset(data, 'A', DISKIMAGE_SECTOR_SIZE);
    data[0] = 0;
    data[1] = UINT8_MAX;
    dos_media_io_t io = source_bank.units[0].slots[0].io;
    io.image.write = NULL;
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &io), DOS_MEDIA_OK);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    const uint8_t peer = open_name(&unit, &bank, 3, "#");
    const dos_channel_t saved_peer = unit.channels[peer];
    const uint16_t pages = unit.buffer_use;
    send_command(&unit, &bank, mode == 1 ? "C0=1" : "C0:OUT=1:REL");
    bool injected = false;
    for (unsigned int step = 0; dos_work_pending(&unit); ++step) {
        ck_assert_uint_lt(step, 8192);
        if (mode == 2 && !injected && unit.copy.reader != DOS_UNUSED) {
            const dos_channel_t* const reader = &unit.channels[unit.copy.reader];
            if (reader->allocated && reader->file_type == DISKIMAGE_FTYPE_REL &&
                reader->rel.record.result == DOS_REL_RECORD_MISSING) {
                source.fail_read = source.reads + 1u;
                injected = true;
            }
        }
        dos_work(&unit, &bank);
    }
    ck_assert_uint_eq(injected, mode == 2);
    const bool index_failure = mode == 2 && model == DOS_MODEL_8250;
    ck_assert_uint_eq(unit.status_code, index_failure ? DOS_STATUS_READ_ERROR : DOS_STATUS_NO_RECORD);
    if (!index_failure) {
        ck_assert_str_eq(unit.status, model == DOS_MODEL_4040 ? "50, RECORD NOT PRESENT,00,00\r"
                                                              : "50, RECORD NOT PRESENT,00,00,1\r");
    }
    ck_assert_uint_eq(unit.buffer_use, pages);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    ck_assert_uint_eq(source.writes, 0);
    free(source.bytes);
    free(target.bytes);
}
END_TEST

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
    tcase_add_loop_test(core, test_replacement_close_uses_current_entry, 0, 9);
    tcase_add_loop_test(core, test_replacement_close_rejects_missing_alternate, 0, 3);
    tcase_add_loop_test(core, test_unpublished_writer_retries_closed_metadata, 0, 6);
    tcase_add_loop_test(core, test_untouched_growth_failure_reserves_both_links, 0, 3);
    tcase_add_loop_test(core, test_reused_secondary_revalidates_directory, 0, 6);
    tcase_add_loop_test(core, test_lookup_failure_preserves_writer, 0, 6);
    tcase_add_loop_test(core, test_append_rebind_invalidates_free_total, 0, 3);
    tcase_add_loop_test(core, test_unpublished_writer_reserves_filename, 0, 12);
    tcase_add_loop_test(core, test_unpublished_writer_retires_before_same_secondary_open, 0, 3);
    tcase_add_loop_test(core, test_write_pattern_flags_across_parse_passes, 0, 30);
    tcase_add_loop_test(core, test_metadata_command_boundaries_and_retirement, 0, 21);
    tcase_add_loop_test(core, test_metadata_names_retain_explicit_padding_and_native_limits, 0, 18);
    tcase_add_loop_test(core, test_rename_changes_only_directory_name_and_preserves_reader, 0, 3);
    tcase_add_loop_test(core, test_rename_native_parser_and_error_precedence, 0, 69);
    tcase_add_loop_test(core, test_rename_cross_drive_collision_and_source_selection, 0, 6);
    tcase_add_loop_test(core, test_rename_storage_failures_release_only_internal_ownership, 0, 15);
    tcase_add_loop_test(core, test_rename_yields_and_preserves_later_directory_edits, 0, 3);
    tcase_add_loop_test(core, test_rename_rejects_stale_media_at_lookup_and_commit, 0, 6);
    tcase_add_loop_test(core, test_rename_unclosed_writer_survives_close_without_restoring_old_name, 0, 3);
    tcase_add_loop_test(core, test_rename_default_drive_and_absent_source, 0, 6);
    tcase_add_loop_test(core, test_rename_shared_resource_exhaustion_and_recovery, 0, 3);
    tcase_add_loop_test(core, test_scratch_releases_exact_chain_and_preserves_peer_reader, 0, 3);
    tcase_add_loop_test(core, test_scratch_native_parser_patterns_and_counts, 0, 66);
    tcase_add_loop_test(core, test_scratch_active_locked_unclosed_and_rel_entries, 0, 18);
    tcase_add_loop_test(core, test_scratch_cross_drive_patterns_and_default_selection, 0, 9);
    tcase_add_loop_test(core, test_scratch_pattern_limit_and_resource_recovery, 0, 3);
    tcase_add_loop_test(core, test_scratch_mixed_defaults_require_third_drive_search, 0, 6);
    tcase_add_loop_test(core, test_scratch_storage_failures_and_stale_media, 0, 27);
    tcase_add_loop_test(core, test_scratch_releases_chains_across_bam_parts, 1, 3);
    tcase_add_loop_test(core, test_scratch_yields_reenters_and_bounds_corrupt_chains, 0, 6);
    tcase_add_loop_test(core, test_validate_repairs_maps_deletes_splats_and_preserves_counts, 0, 3);
    tcase_add_loop_test(core, test_validate_drive_selection_and_peer_cursor, 0, 18);
    tcase_add_loop_test(core, test_validate_crosslinks_are_model_specific, 0, 3);
    tcase_add_loop_test(core, test_validate_storage_errors_and_retry, 0, 18);
    tcase_add_loop_test(core, test_validate_corruption_and_side_links, 0, 12);
    tcase_add_loop_test(core, test_validate_stale_media_never_reaches_replacement_io, 0, 9);
    tcase_add_loop_test(core, test_validate_resource_pressure_and_absent_slot, 0, 3);
    tcase_add_loop_test(core, test_validate_marks_all_bam_parts_and_directory_extensions, 0, 3);
    tcase_add_loop_test(core, test_rename_respects_unpublished_writer_namespace, 0, 12);
    tcase_add_loop_test(core, test_rename_preserves_failed_source_publication_intent, 0, 3);
    tcase_add_loop_test(core, test_scratch_stale_completion_never_flushes_replacement, 0, 12);
    tcase_add_loop_test(core, test_validate_initialization_ignores_interleaved_peer_status, 0, 9);
    tcase_add_loop_test(core, test_validate_invalid_data_start_after_side_chain, 0, 12);
    tcase_add_loop_test(core, test_scratch_side_chain_precedes_invalid_data_start, 0, 15);
    tcase_add_loop_test(core, test_scratch_native_count_digit_boundaries, 0, 18);
    tcase_add_loop_test(core, test_status_native_decimal_field_boundaries, 0, 9);
    tcase_add_loop_test(core, test_scratch_listing_cursors_do_not_protect_unopened_files, 0, 12);
    tcase_add_loop_test(core, test_scratch_cross_request_command_storage_boundary, 0, 9);
    tcase_add_loop_test(core, test_initialize_abandons_only_selected_disk_and_reloads_metadata, 0, 3);
    tcase_add_loop_test(core, test_initialize_native_drive_selection, 0, 27);
    tcase_add_loop_test(core, test_initialize_storage_failures_and_absent_slot_do_not_redirect, 0, 6);
    tcase_add_loop_test(core, test_initialize_rejects_replaced_media_before_io, 0, 3);
    tcase_add_loop_test(core, test_command_termination_unknown_and_execution_rejection, 0, 3);
    tcase_add_loop_test(core, test_user_interrupt_retains_channels_maps_and_command_bytes, 0, 18);
    tcase_add_loop_test(core, test_user_interrupt_overlong_and_trimmed_request, 0, 3);
    tcase_add_loop_test(core, test_warm_reset_retains_ram_and_model_specific_maps, 0, 18);
    tcase_add_loop_test(core, test_warm_reset_auto_initialize_entry_points, 0, 18);
    tcase_add_loop_test(core, test_warm_directory_reload_per_drive_and_raw_exemption, 0, 9);
    tcase_add_loop_test(core, test_warm_reset_overlong_request, 0, 3);
    tcase_add_loop_test(core, test_block_reads_counted_and_raw_boundaries, 0, 3 * 2 * 6);
    tcase_add_loop_test(core, test_block_read_parser_geometry_and_user_aliases, 0, 3 * count_of(block_read_commands));
    tcase_add_loop_test(core, test_block_read_failures_and_stale_identity, 0, 9);
    tcase_add_loop_test(core, test_block_read_drive_affinity_and_readonly_last_track, 0, 6);
    tcase_add_loop_test(core, test_block_write_pointer_count_and_raw_state, 0, 3 * 2 * 3 * 6);
    tcase_add_loop_test(core, test_block_write_failures_and_stale_identity, 0, 15);
    tcase_add_loop_test(core, test_block_write_parser_and_aliases, 0, 42);
    tcase_add_loop_test(core, test_write_version_counted_raw_and_drive, 0, 48);
    tcase_add_loop_test(core, test_write_version_read_retry_and_cached_part, 0, 6);
    tcase_add_loop_test(core, test_write_version_metadata_jobs, 0, 6);
    tcase_add_loop_test(core, test_write_version_follows_cached_map_part, 0, 2);
    tcase_add_loop_test(core, test_rejected_block_write_preserves_drive_affinity, 0, 6);
    tcase_add_loop_test(core, test_block_write_drive_affinity_and_last_track, 0, 12);
    tcase_add_loop_test(core, test_block_map_claim_free_and_close_persistence, 0, 6);
    tcase_add_loop_test(core, test_block_map_successor_search_and_exhaustion, 0, 15);
    tcase_add_loop_test(core, test_block_map_parser_geometry_and_no_channel, 0, 42);
    tcase_add_loop_test(core, test_block_map_failures_and_readonly_close, 0, 18);
    tcase_add_loop_test(core, test_block_pointer_parser_and_rejection_atomicity, 0, 3 * count_of(pointer_commands));
    tcase_add_loop_test(core, test_block_pointer_all_offsets_put_and_sticky_eoi, 0, 3);
    tcase_add_loop_test(core, test_reader_pointer_offsets_and_prepared_byte, 0, 3 * 2 * 9);
    tcase_add_loop_test(core, test_reader_pointer_materializes_active_buffer, 0, 9);
    tcase_add_loop_test(core, test_reader_pointer_sticky_eoi_and_revival, 0, 3);
    tcase_add_loop_test(core, test_reader_pointer_modify_and_raw_directory_are_read_only, 0, 9);
    tcase_add_loop_test(core, test_reader_pointer_failed_normalization_preserves_cursor, 0, 3);
    tcase_add_loop_test(core, test_reader_block_counted_and_raw_boundaries, 0, 3 * 2 * 6);
    tcase_add_loop_test(core, test_reader_block_materializes_active_page, 0, 24);
    tcase_add_loop_test(core, test_reader_block_zero_count_retains_prefetched_source_drive, 0, 3);
    tcase_add_loop_test(core, test_reader_zero_count_terminal_uses_retained_alternate_ram, 0, 9);
    tcase_add_loop_test(core, test_reader_zero_count_terminal_reuses_previous_page, 0, 3);
    tcase_add_loop_test(core, test_reader_alternate_switch_preserves_captured_generation, 0, 18);
    tcase_add_loop_test(core, test_directory_peer_switch_preserves_captured_generation, 0, 6);
    tcase_add_loop_test(core, test_reader_block_modify_and_raw_directory_are_read_only, 0, 18);
    tcase_add_loop_test(core, test_reader_block_failed_normalization_preserves_cursor, 0, 9);
    tcase_add_loop_test(core, test_reader_block_writes_materialize_native_pointer, 0, 3 * 2 * 3 * 8);
    tcase_add_loop_test(core, test_reader_block_write_failures_retain_page_and_source, 0, 3 * 2 * 7);
    tcase_add_loop_test(core, test_reader_block_write_singleton_and_eof, 0, 3 * 2 * 3);
    tcase_add_loop_test(core, test_reader_block_write_modify_and_directory, 0, 3 * 2 * 3);
    tcase_add_loop_test(core, test_block_pointer_channel_permissions_and_excluded_readers, 0, 15);
    tcase_add_loop_test(core, test_block_pointer_media_changes_do_not_issue_storage_jobs, 0, 6);
    tcase_add_loop_test(core, test_direct_parser_and_single_buffer_ownership, 0, 3 * count_of(direct_names));
    tcase_add_loop_test(core, test_direct_put_wrap_sticky_eoi_and_partial_get, 0, 9);
    tcase_add_loop_test(core, test_direct_capacity_reopen_and_reserved_page_retention, 0, 3);
    tcase_add_loop_test(core, test_direct_explicit_page_drive_and_initialize, 0, 3);
    tcase_add_loop_test(core, test_direct_reopen_abandons_writer_and_validate_releases_page, 0, 3);
    tcase_add_loop_test(core, test_direct_ram_survives_media_change_without_replacement_io, 0, 6);
    tcase_add_loop_test(core, test_direct_close_retries_dirty_bam_and_backing_flush, 0, 9);
    tcase_add_loop_test(core, test_direct_load_save_and_overlong_request, 0, 6);
    tcase_add_loop_test(core, test_direct_close_remembered_drive_is_model_specific, 0, 12);
    tcase_add_loop_test(core, test_direct_close_clean_map_native_errors_and_retry, 0, 42);
    tcase_add_loop_test(core, test_direct_open_errors_retire_only_model_eligible_owners, 0, 36);
    tcase_add_loop_test(core, test_direct_close_retries_block_write_without_dirty_bam, 0, 18);
    tcase_add_loop_test(core, test_direct_close_retry_includes_intervening_map_changes, 0, 9);
    tcase_add_loop_test(core, test_direct_close_zero_bypasses_mapout_only_when_explicit, 0, 12);
    tcase_add_loop_test(core, test_reopening_direct_owner_does_not_finalize_map, 0, 24);
    tcase_add_loop_test(core, test_stale_direct_close_cannot_flush_current_map, 0, 9);
    tcase_add_loop_test(core, test_block_commands_reject_allocated_load_and_save, 0, 30);
    tcase_add_loop_test(core, test_user_table_reset_and_cold_vectors, 0, 9);
    tcase_add_loop_test(core, test_copy_preserves_boundary_payloads_and_peer_cursor, 0, 21);
    tcase_add_loop_test(core, test_copy_parser_precedence_and_command_union_reuse, 0, 57);
    tcase_add_loop_test(core, test_copy_concat_append_mixed_types_and_metadata, 0, 9);
    tcase_add_loop_test(core, test_copy_faults_abandon_only_owned_channels, 0, 12);
    tcase_add_loop_test(core, test_copy_resource_read_only_full_and_corrupt_recovery, 0, 15);
    tcase_add_loop_test(core, test_copy_cross_drive_defaults_use_native_third_search, 0, 3);
    tcase_add_loop_test(core, test_copy_all_aliases_preserve_source_and_peer, 0, 24);
    tcase_add_loop_test(core, test_copy_pattern_selects_only_matching_files_and_preserves_peer, 0, 21);
    tcase_add_loop_test(core, test_copy_pattern_without_matches_does_not_create_files, 0, 12);
    tcase_add_loop_test(core, test_copy_pattern_same_drive_and_default_flag_are_model_specific, 0, 12);
    tcase_add_loop_test(core, test_copy_pattern_rejects_nonstar_targets_and_source_groups, 0, 24);
    tcase_add_loop_test(core, test_copy_all_rejects_invalid_drive_syntax, 0, 30);
    tcase_add_loop_test(core, test_copy_all_keeps_completed_prefix_and_recovers, 0, 6);
    tcase_add_loop_test(core, test_copy_all_empty_source_and_read_failure, 0, 6);
    tcase_add_loop_test(core, test_copy_all_reverse_direction_and_retained_selector, 0, 3);
    tcase_add_loop_test(core, test_copy_all_crosses_source_and_target_directory_pages, 0, 6);
    tcase_add_loop_test(core, test_copy_respects_unpublished_writer_namespace, 0, 9);

    tcase_add_loop_test(core, test_copy_pattern_retains_literal_padding_byte, 0, 3);
    tcase_add_loop_test(core, test_copy_decodes_model_specific_source_track, 0, 3);
    tcase_add_loop_test(core, test_copy_preserves_unpublished_directory_coordinates, 0, 18);
    tcase_add_loop_test(core, test_copy_all_rejects_pending_source_directory_growth, 0, 6);
    tcase_add_loop_test(core, test_copy_self_append_discards_unused_directory_error, 0, 3);
    tcase_add_loop_test(core, test_copy_all_defers_directory_error_and_reopens_jobs, 0, 12);
    tcase_add_loop_test(core, test_copy_padded_name_lookup_and_full_transferred_target, 0, 9);
    tcase_add_loop_test(core, test_4040_copy_bit7_match_remains_pending, 0, 4);
    tcase_add_loop_test(core, test_copy_read_failure_preserves_native_partial_chain, 0, 6);
    tcase_add_loop_test(core, test_copy_self_append_ignores_peer_read_error_status, 0, 3);
    tcase_add_loop_test(core, test_copy_respects_retained_reclaim_entry, 0, 15);
    tcase_add_loop_test(core, test_copy_append_reselects_owner_after_peer_error, 0, 3);
    tcase_add_loop_test(core, test_copy_cross_drive_read_and_allocation_status, 0, 12);
    tcase_add_loop_test(core, test_copy_cross_drive_collision_retains_lookup_drive, 0, 6);
    tcase_add_loop_test(core, test_copy_revalidates_source_after_successful_peer_close, 0, 12);
    tcase_add_loop_test(core, test_copy_retires_active_binding_before_sector_reuse, 0, 6);
    tcase_add_loop_test(core, test_copy_all_allows_close_of_completed_source, 0, 6);
    tcase_add_loop_test(core, test_copy_zero_track_match_remains_pending, 0, 6);
    tcase_add_loop_test(core, test_format_quick_and_full_complete_images, 0, 6);
    tcase_add_loop_test(core, test_duplicate_complete_image_and_source_isolation, 0, 6);
    tcase_add_loop_test(core, test_format_and_duplicate_storage_failures, 0, 27);
    tcase_add_loop_test(core, test_format_channel_lifecycle, 0, 6);
    tcase_add_loop_test(core, test_format_and_duplicate_preserve_busy_scratch, 0, 6);
    tcase_add_loop_test(core, test_format_and_duplicate_reject_replaced_media, 0, 24);
    tcase_add_loop_test(core, test_format_id_and_inherited_drive_boundaries, 0, 9);
    tcase_add_loop_test(core, test_format_initialization_result_survives_status_consumption, 0, 9);
    tcase_add_loop_test(core, test_format_and_duplicate_native_parser_rejections, 0, 45);
    tcase_add_loop_test(core, test_format_restores_drive_after_peer_error, 0, 18);
    tcase_add_loop_test(core, test_format_failure_reloads_bam_before_recovery, 0, 12);
    tcase_add_loop_test(core, test_format_prescratch_rejection_preserves_bam, 0, 3);
    tcase_add_loop_test(core, test_format_allows_equals_in_prefix, 0, 3);
    tcase_add_loop_test(core, test_duplicate_rejects_aliased_image, 0, 12);
    tcase_add_loop_test(core, test_format_failure_finalization_flushes_captured_media, 0, 12);
    tcase_add_loop_test(core, test_format_failure_finalization_rejects_replaced_media, 0, 3);
    tcase_add_loop_test(core, test_format_reserves_native_reader_with_exhausted_peers, 0, 10);
    tcase_add_loop_test(core, test_failed_initialization_retains_prior_format_flush, 0, 12);
    tcase_add_loop_test(core, test_format_ignores_suffix_after_native_comma_limit, 0, 9);
    tcase_add_loop_test(core, test_format_propagates_late_metadata_chain_failures, 0, 24);
    tcase_add_loop_test(core, test_format_serializes_surviving_directory_footer, 0, 8);
    tcase_add_loop_test(core, test_format_4040_completes_without_final_bam_read, 0, 2);
    tcase_add_loop_test(core, test_format_quick_reuses_initialized_header_id, 0, 6);
    tcase_add_loop_test(core, test_input_open_is_deferred_and_reset_is_unit_local, 0, 3);
    tcase_add_loop_test(core, test_input_command_capacity_and_channel_selection, 0, 6);
    tcase_add_loop_test(core, test_input_command_open_and_close_semantics, 0, 3);
    tcase_add_loop_test(core, test_output_retains_source_and_commits_only_actual_progress, 0, 3);
    tcase_add_loop_test(core, test_open_retains_failed_rel_close, 0, 3);
    tcase_add_loop_test(core, test_rel_scratch_and_validate_native_chains, 0, 18);
    tcase_add_loop_test(core, test_public_rel_zero_size_directory_policy, 0, 3);
    tcase_add_loop_test(core, test_public_rel_create_write_close_and_remount, 0, 21);
    tcase_add_loop_test(core, test_public_rel_creation_faults_retain_publication_and_peer, 0, 18);
    tcase_add_loop_test(core, test_public_rel_directory_growth_finalization_and_retry, 0, 9);
    tcase_add_loop_test(core, test_public_zero_size_rel_is_cooperative_and_resettable, 0, 3);
    tcase_add_loop_test(core, test_copy_rel_short_records_and_native_preallocation, 0, 6);
    tcase_add_loop_test(core, test_copy_rel_resource_pressure_preserves_public_owners, 0, 6);
    tcase_add_loop_test(core, test_copy_rel_storage_faults_release_only_internal_pages, 0, 3);
    tcase_add_loop_test(core, test_record_rejects_missing_and_nonrelative_channels, 0, 6);
    tcase_add_loop_test(core, test_public_rel_read_failure_releases_open_context, 0, 3);
    tcase_add_loop_test(core, test_listen_position_entries_preserve_captured_byte, 0, 6);
    tcase_add_loop_test(core, test_input_reselection_routes_current_data, 0, 12);
    tcase_add_loop_test(core, test_input_new_command_clears_previous_completion, 0, 6);
    tcase_add_loop_test(core, test_copy_all_rel_close_preserves_buffer_zero_peer, 0, 6);
    tcase_add_loop_test(core, test_copy_rel_ordinary_sector_boundary, 0, 9);
    tcase_add_loop_test(core, test_initialize_retires_failed_rel_publication, 0, 12);
    tcase_add_loop_test(core, test_input_completed_request_reselection_discards_old_dispatch, 0, 18);
    tcase_add_loop_test(core, test_directory_reopen_after_rel_write_releases_only_previous_owner, 0, 6);
    tcase_add_loop_test(core, test_input_open_retains_overflow_byte_for_next_rel_request, 0, 3);
    tcase_add_loop_test(core, test_input_new_collection_survives_pending_worker_retirement, 0, 24);
    tcase_add_loop_test(core, test_copy_rel_completion_uses_writer_outcome_not_public_status, 0, 6);
    tcase_add_loop_test(core, test_rename_cannot_claim_failed_rel_publication_name, 0, 6);
    tcase_add_loop_test(core, test_scratch_cannot_free_partial_rel_publication, 0, 3);
    tcase_add_loop_test(core, test_copy_rel_cached_source_replacement_reports_failure, 0, 3);
    tcase_add_loop_test(core, test_pending_rel_does_not_bind_unrelated_directory_entry, 0, 12);
    tcase_add_loop_test(core, test_failed_rel_open_retires_command_without_repositioning_peer, 0, 21);
    tcase_add_loop_test(core, test_failed_rel_open_preserves_newer_record_collection, 0, 6);
    tcase_add_loop_test(core, test_copy_rel_missing_first_record_reports_native_error, 0, 9);
    suite_add_tcase(suite, core);
    return suite;
}

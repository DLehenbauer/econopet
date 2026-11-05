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

#define REL_FIXTURE_BLOCK_LIMIT 730u
#define REL_FIXTURE_RECORD_SIZE 127u

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

// PARSE keeps empty fields, while CKTM only gives the first L a raw successor.
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

// GETERC clears the status on CR GET, not staging or an interrupted TALK.
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

// A delayed OPEN cannot read a replacement image through its old media identity.
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

// A linked-directory search yields between sectors, leaving native resources owned.
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

// OPNRCH reserves two shared buffers and GET consumes only observed bytes.
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

// Native six ordinary indices and twelve buffers are shared, not per disk.
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

// Four REL owners exhaust the shared twelve pages, not six logical indices.
START_TEST(test_rel_claim_three_pages_and_release) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_image(&bank, &image, (unsigned int)_i);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &bank.units[0].slots[0].io), DOS_MEDIA_OK);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const unsigned int reads = image.reads;
    char status[DOS_STATUS_LINE_CAPACITY];
    memcpy(status, unit.status, sizeof(status));
    for (unsigned int owner = 0; owner < DOS_BUFFER_COUNT / DOS_CHANNEL_BUFFER_COUNT; ++owner) {
        dos_media_ref_t reference;
        ck_assert_int_eq(dos_media_reference(&bank, 0, owner & 1u, &reference), DOS_MEDIA_OK);
        const uint8_t channel = dos_rel_channel_claim(&unit, reference, owner + 2u);
        ck_assert_uint_eq(channel, owner);
        const dos_channel_t* const state = &unit.channels[channel];
        ck_assert_uint_eq(state->file_type, DISKIMAGE_FTYPE_REL);
        ck_assert(!state->ready);
        ck_assert_int_eq(state->rel.record.result, DOS_REL_RECORD_IDLE);
        for (unsigned int page = 0; page < DOS_CHANNEL_BUFFER_COUNT; ++page) {
            const uint8_t index = DOS_BUFFER_COUNT - 1u - owner * DOS_CHANNEL_BUFFER_COUNT - page;
            ck_assert_uint_eq(state->buffers[page], index);
            ck_assert_int_eq(!!(unit.buffer_drives & (1u << index)), owner & 1u);
            unit.buffers[index].resident = true;
        }
    }
    const uint16_t full = (1u << DOS_BUFFER_COUNT) - 1u;
    ck_assert_uint_eq(unit.buffer_use, full);
    dos_media_ref_t reference;
    ck_assert_int_eq(dos_media_reference(&bank, 0, 0, &reference), DOS_MEDIA_OK);
    ck_assert_uint_eq(dos_rel_channel_claim(&unit, reference, 6), DOS_UNUSED);
    ck_assert_mem_eq(unit.status, status, sizeof(status));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, 0);
    ck_assert(dos_close(&unit, &bank, model, 3));
    const uint16_t released = (1u << 8) | (1u << 7) | (1u << 6);
    ck_assert_uint_eq(unit.buffer_use, full & ~released);
    for (unsigned int page = 6; page <= 8; ++page) {
        ck_assert(!unit.buffers[page].resident);
    }
    ck_assert_uint_eq(dos_rel_channel_claim(&unit, reference, 6), 1);
    ck_assert_uint_eq(unit.buffer_use, full);
    ck_assert_uint_eq(unit.channels[1].buffers[2], 6);
    ck_assert(dos_close(&unit, &bank, model, IEEE_COMMAND_CHANNEL));
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, 0);
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
    ck_assert_int_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
    ck_assert_mem_eq(state, &owner, sizeof(owner));
    ck_assert_uint_eq(unit.buffer_use, use);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    free(image.bytes);
}
END_TEST

// Failed first/second/side claims release only partial ownership, not peers.
START_TEST(test_rel_claim_partial_exhaustion_and_logical_reservations) {
    const unsigned int format = (unsigned int)_i / 5u;
    const unsigned int variant = (unsigned int)_i % 5u;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_image(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    dos_media_ref_t reference;
    ck_assert_int_eq(dos_media_reference(&bank, 0, 0, &reference), DOS_MEDIA_OK);
    if (variant < DOS_CHANNEL_BUFFER_COUNT) {
        const uint16_t available = variant == 0 ? 0 : variant == 1 ? 1u << 11 : (1u << 11) | (1u << 5);
        unit.buffer_use = ((1u << DOS_BUFFER_COUNT) - 1u) & ~available;
    } else {
        for (unsigned int channel = 0; channel < DOS_FILE_CHANNEL_COUNT; ++channel) {
            unit.channels[channel].allocated = variant == DOS_CHANNEL_BUFFER_COUNT || channel != 0;
        }
        unit.directory_internal = variant == DOS_CHANNEL_BUFFER_COUNT ? 0 : 1;
    }
    for (unsigned int page = 0; page < DOS_BUFFER_COUNT; ++page) {
        memset(unit.buffers[page].bytes, page, DISKIMAGE_SECTOR_SIZE);
        unit.buffers[page].resident = true;
    }
    const dos_unit_t before = unit;
    const unsigned int reads = image.reads;
    ck_assert_uint_eq(dos_rel_channel_claim(&unit, reference, 12), DOS_UNUSED);
    ck_assert_uint_eq(unit.buffer_use, before.buffer_use);
    ck_assert_uint_eq(unit.directory_internal, before.directory_internal);
    ck_assert_mem_eq(unit.channels, before.channels, sizeof(unit.channels));
    ck_assert_mem_eq(unit.status, before.status, sizeof(unit.status));
    for (unsigned int page = 0; page < DOS_BUFFER_COUNT; ++page) {
        ck_assert_mem_eq(unit.buffers[page].bytes, before.buffers[page].bytes, DISKIMAGE_SECTOR_SIZE);
        if (before.buffer_use & (1u << page)) {
            ck_assert_mem_eq(&unit.buffers[page], &before.buffers[page], sizeof(unit.buffers[page]));
        }
    }
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, 0);
    free(image.bytes);
}
END_TEST

// REL teardown/reuse preserves a live ordinary peer and the directory index.
START_TEST(test_rel_claim_mixed_peer_and_reserved_index) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_image(&bank, &image, (unsigned int)_i);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    const uint8_t name[] = "BASIC";
    dos_open(&unit, &bank, 0, 2, name, sizeof(name) - 1);
    finish_open(&unit, &bank);
    const dos_channel_t peer = unit.channels[0];
    const uint16_t owned = unit.buffer_use;
    unit.directory_internal = 2;
    dos_media_ref_t reference;
    ck_assert_int_eq(dos_media_reference(&bank, 0, 0, &reference), DOS_MEDIA_OK);
    ck_assert_uint_eq(dos_rel_channel_claim(&unit, reference, 3), 2);
    unit.channels[2].rel.number = UINT16_MAX;
    unit.channels[2].rel.no_record = true;
    ck_assert_uint_eq(unit.channels[2].buffers[0], 11);
    ck_assert_uint_eq(unit.channels[2].buffers[1], 10);
    ck_assert_uint_eq(unit.channels[2].buffers[2], 7);
    ck_assert(dos_close(&unit, &bank, model, 3));
    ck_assert_uint_eq(unit.buffer_use, owned);
    ck_assert_uint_eq(unit.directory_internal, 2);
    ck_assert_mem_eq(&unit.channels[0], &peer, sizeof(peer));
    ck_assert_uint_eq(dos_rel_channel_claim(&unit, reference, 3), 2);
    ck_assert_uint_eq(unit.channels[2].rel.number, 0);
    ck_assert(!unit.channels[2].rel.no_record);
    ck_assert(dos_close(&unit, &bank, model, 3));
    ck_assert_mem_eq(&unit.channels[0], &peer, sizeof(peer));
    dos_reset(&unit, model);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

// Missing media and unsupported writes never run a legacy operation.
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
    ck_assert_str_eq(unit.status, "26, WRITE PROTECT ON,00,00\r");
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

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

// Collects native command bytes through either public command-channel entry.
static void send_command(dos_unit_t* const unit, dos_media_bank_t* const bank, const char* const text,
                         const bool open) {
    const unsigned int length = strlen(text);
    dos_input_address(unit, bank, bank->units[0].model,
                      open ? IEEE_CMD_OPEN(IEEE_COMMAND_CHANNEL) : IEEE_CMD_SECONDARY(IEEE_COMMAND_CHANNEL), true);
    for (unsigned int offset = 0; offset < length; ++offset) {
        dos_input_put(unit, (uint8_t)text[offset], offset + 1u == length);
    }
    dos_input_dispatch(unit, bank, 0);
}

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

// Opens a bounded native request through the same cooperative work entry point.
static uint8_t open_name(dos_unit_t* const unit, dos_media_bank_t* const bank, const uint8_t secondary,
                         const char* const name) {
    dos_open(unit, bank, 0, secondary, (const uint8_t*)name, strlen(name));
    finish_open(unit, bank);
    return dos_find_channel(unit, secondary);
}

// Opens a native request whose fixed-width filename may contain NUL bytes.
static uint8_t open_bytes(dos_unit_t* const unit, dos_media_bank_t* const bank, const uint8_t secondary,
                          const uint8_t* const name, const unsigned int length) {
    dos_open(unit, bank, 0, secondary, name, length);
    finish_open(unit, bank);
    return dos_find_channel(unit, secondary);
}

// Writes deterministic bytes through ordinary LISTEN, rather than the name buffer.
static void put_bytes(dos_unit_t* const unit, dos_media_bank_t* const bank, const uint8_t secondary,
                      const unsigned int first, const unsigned int count) {
    dos_input_address(unit, bank, bank->units[0].model, IEEE_CMD_SECONDARY(secondary), true);
    for (unsigned int index = 0; index < count; ++index) {
        dos_listen_put(unit, bank, (uint8_t)(first + index), index + 1u == count);
    }
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

// STDIR/GETNAM send native dummy links, 27-byte names, free blocks and final three zeros.
START_TEST(test_formatted_directory_retains_exact_rows_and_final_eoi) {
    const unsigned int format = (unsigned int)_i;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:FILE,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x40, 22);
    ck_assert(dos_close(&unit, &bank, model, 2));
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

// Resolve the fixture's independently laid-out BAM entries, not DOS cache state.
static uint8_t* fixture_bam_entry(image_t* const image, const unsigned int format, const unsigned int track) {
    const diskimage_type_t type = format == 0   ? diskimage_type_d64
                                  : format == 1 ? diskimage_type_d80
                                                : diskimage_type_d82;
    return format == 0 ? sector_bytes(image, type, 18, 0) + track * 4u
                       : sector_bytes(image, type, 38, ((track - 1u) / 50u) * 3u) + 6u + ((track - 1u) % 50u) * 5u;
}

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

// TC35/FS1SET apply '=type' separately from comma-separated filename patterns.
START_TEST(test_directory_type_filters_patterns_and_raw_chain) {
    const unsigned int format = (unsigned int)_i;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:FILE,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x40, 22);
    ck_assert(dos_close(&unit, &bank, model, 2));
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:USER,U,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x60, 22);
    ck_assert(dos_close(&unit, &bank, model, 2));
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

// Exercise the directory reservation and EOI path for the native REL type filter.
START_TEST(test_filtered_directory_rel_type_stays_a_directory) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, (unsigned int)_i);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    uint8_t bytes[2u * DISKIMAGE_SECTOR_PAYLOAD_SIZE];
    uint8_t channel = open_name(&unit, &bank, 0, "$0:*=L");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    ck_assert_uint_gt(unit.directory_internal, 0);
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, bytes, sizeof(bytes)), 64);
    ck_assert(dos_close(&unit, &bank, model, 0));
    ck_assert_uint_eq(unit.directory_internal, 0);
    ck_assert_uint_eq(unit.buffer_use, 0);

    channel = open_name(&unit, &bank, 0, "$0:*=L");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    ck_assert_uint_eq(directory_bytes(&unit, &bank, channel, bytes, sizeof(bytes)), 64);
    ck_assert(dos_close(&unit, &bank, model, 0));
    ck_assert_uint_eq(unit.directory_internal, 0);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(image.bytes);
}
END_TEST

static const struct {
    const char* filename;
    size_t filename_length;
    const char* pattern;
    bool matches;
} native_open_wildcard_cases[] = {
    {"BASIC", 5, "?ASIC", true},
    {"BASIC", 5, "BASI?", true},
    {"BASIC", 5, "BA?IC", true},
    {"BASIC", 5, "B?S?C", true},
    {"BASIC", 5, "?????", true},
    {"BASIC", 5, "?*", true},
    {"BASIC", 5, "B?S*", true},
    {"BASIC", 5, "?????*", true},
    {"BASIC", 5, "BASIC*?", true},
    {"BASIC", 5, "BASIC", true},
    {"BASIC", 5, "BAS*", true},
    {"BASIC", 5, "BASIC?", false},
    {"BASIC", 5, "BASI??", false},
    {"BASIC", 5, "BA?IX", false},
    {"BASIC", 5, "B?SI", false},
    {"BASIC", 5, "BASIC?*", false},
    {"ABCDEFGHIJKLMNOP", 16, "ABCDEFGHIJKLMNO?", true},
    {"ABCDEFGHIJKLMNOP", 16, "????????????????", true},
    {"ABCDEFGHIJKLMNOP", 16, "ABCDEFGHIJKLMN?", false},
    {"A\xa4"
     "B",
     3, "A?B", true},
    {"A", 1, "?", true},
    {"A", 1, "??", false},
    {"BASIC", 5, "?", false},
    {"BASIC", 5, "*", true},
    {"BASIC", 5, "B*X", true},
    {"BASIC", 5, "X*", false},
    {"ABCDEFGHIJKLMNOP", 16, "?????????????????", true},
    {"ABCDEFGHIJKLMNOP", 16, "ABCDEFGHIJKLMNOPX", true},
    {"ABCDEFGHIJKLMNOP", 16, "ABCDEFGHIJKLMNO?X", true},
    {"ABCDEFGHIJKLMNOP", 16, "ABCDEFGHIJKLMNOX?", false},
    {"ABCDEFGHIJKLMNO", 15, "?????????????????", false},
    {"ABCDEFGHIJKLMNO\0", 16, "?????????????????", true},
    {"BaSIC", 5, "BAS?C", false},
    {"BaSIC", 5, "BaS?C", true},
    {"BaSIC", 5, "B?SIC", true},
    {"BASIC", 5, "bAS?C", false},
    {"A\0B", 3, "A?B", true},
    {"A\0B", 3, "A", false},
    {"A\0B", 3, "A?", false},
    {"\0AB", 3, "?AB", true},
    {"ABCDEFGHIJKLMNO\0", 16, "????????????????", true},
};

// Verify native OPEN selection, file bytes, EOI and exact misses for wildcard boundaries.
START_TEST(test_open_native_wildcard_boundaries) {
    const unsigned int format = (unsigned int)_i / count_of(native_open_wildcard_cases);
    const size_t index = (size_t)_i % count_of(native_open_wildcard_cases);
    const diskimage_type_t type = format == 0 ? diskimage_type_d64 : diskimage_type_d80;
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    const uint8_t writer = open_name(&unit, &bank, 2, "0:FIXTURE,S,W");
    ck_assert_uint_ne(writer, DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 'W', 4);
    ck_assert(dos_close(&unit, &bank, model, 2));

    const uint8_t directory_track = format == 0 ? 18 : 39;
    uint8_t* const entry = sector_bytes(&image, type, directory_track, 1);
    memset(entry + 5, DISKIMAGE_FILENAME_PADDING, DISKIMAGE_FILENAME_SIZE);
    memcpy(entry + 5, native_open_wildcard_cases[index].filename, native_open_wildcard_cases[index].filename_length);
    uint8_t name[2u + DISKIMAGE_FILENAME_SIZE + 1u + 4u];
    name[0] = '0';
    name[1] = ':';
    const unsigned int pattern_length = (unsigned int)strlen(native_open_wildcard_cases[index].pattern);
    memcpy(name + 2, native_open_wildcard_cases[index].pattern, pattern_length);
    name[2u + pattern_length] = ',';
    name[3u + pattern_length] = 'S';
    name[4u + pattern_length] = ',';
    name[5u + pattern_length] = 'R';
    const uint8_t channel = open_bytes(&unit, &bank, IEEE_LOAD_CHANNEL, name, 2u + pattern_length + 4u);
    if (native_open_wildcard_cases[index].matches) {
        ck_assert_msg(channel != DOS_UNUSED, "format %u pattern '%s' status %u", format,
                      native_open_wildcard_cases[index].pattern, unit.status_code);
        expect_bytes(&unit, &bank, channel, 4, 'W');
        ck_assert(dos_close(&unit, &bank, model, IEEE_LOAD_CHANNEL));
    } else {
        ck_assert_uint_eq(channel, DOS_UNUSED);
        ck_assert_int_eq(unit.status_code, DOS_STATUS_NOT_FOUND);
    }
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

// USRINT preserves channels/cursors and does not perform ENDCMD's LSTDRV update.
START_TEST(test_user_zero_retains_native_resources_and_stale_selector) {
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, (unsigned int)_i);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:FILE,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x20, 4);
    ck_assert(dos_close(&unit, &bank, model, 2));
    const uint8_t channel = open_name(&unit, &bank, 2, "0:FILE,S,R");
    ck_assert_uint_ne(channel, DOS_UNUSED);
    dos_get(&unit, &bank, channel, 1);
    const dos_channel_t before = unit.channels[channel];
    const unsigned int reads = image.reads, writes = image.writes, flushes = image.flushes;
    const uint16_t buffers = unit.buffer_use;
    unit.last_drive = 1;
    unit.last_program_track = 17;
    unit.last_program_sector = 2;
    unit.last_program_drive = 1;
    dos_status(&unit, model, DOS_STATUS_READ_ERROR, 1, 2);
    dos_status_get(&unit, model, 3);
    send_command(&unit, &bank, "U0", true);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.status_position, 0);
    ck_assert_uint_eq(unit.last_drive, 1);
    ck_assert_uint_eq(unit.last_program_track, 17);
    ck_assert_uint_eq(unit.last_program_sector, 2);
    ck_assert_uint_eq(unit.last_program_drive, 1);
    ck_assert_uint_eq(unit.buffer_use, buffers);
    ck_assert_mem_eq(&unit.channels[channel], &before, sizeof(before));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(image.flushes, flushes);

    // USER's early RTS retains '0' for a following one-byte U request.
    send_command(&unit, &bank, "U", false);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.last_drive, 1);
    send_command(&unit, &bank, "N", false);
    send_command(&unit, &bank, "U", false);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_SYNTAX);
    ck_assert_mem_eq(&unit.channels[channel], &before, sizeof(before));
    free(image.bytes);
}
END_TEST

// All safe default-vector aliases reach cold DOS reset, not host finalization.
START_TEST(test_user_reset_abandons_writers_but_retains_backing_ownership) {
    static const uint8_t selectors[] = {':', 'J', 'Z', 'j', 'z', 0x8a, 0xfa};
    const unsigned int format = (unsigned int)_i / count_of(selectors);
    const uint8_t selector = selectors[(unsigned int)_i % count_of(selectors)];
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:UNCLOSED,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x20, 12);
    ck_assert(unit.bam[0].dirty);
    const unsigned int reads = image.reads, writes = image.writes, flushes = image.flushes;
    const uint32_t identity = bank.units[0].slots[0].identity;
    uint8_t* const before = malloc(image.size);
    ck_assert_ptr_nonnull(before);
    memcpy(before, image.bytes, image.size);
    image.fail_flush = true;
    const char request[] = {'U', (char)selector, 0};
    send_command(&unit, &bank, request, false);
    ck_assert_str_eq(unit.status, format == 0 ? "73,CBM DOS V2,00,00\r" : "73,CBM DOS V2.7,00,00,0\r");
    ck_assert_uint_eq(unit.buffer_use, 0);
    ck_assert(!dos_work_pending(&unit));
    ck_assert(!dos_input_ready(&unit));
    ck_assert(!unit.bam[0].dirty);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(image.flushes, flushes);
    ck_assert_mem_eq(image.bytes, before, image.size);
    ck_assert_uint_eq(bank.units[0].slots[0].identity, identity);
    ck_assert_ptr_eq(bank.units[0].slots[0].io.image.ctx, &image);
    ck_assert(bank.units[0].slots[0].dirty);
    ck_assert_int_eq(dos_media_flush_unit(&bank, 0), DOS_MEDIA_IO_ERROR);
    ck_assert(bank.units[0].slots[0].dirty);
    image.fail_flush = false;
    ck_assert_int_eq(dos_media_flush_unit(&bank, 0), DOS_MEDIA_OK);
    free(before);
    free(image.bytes);
}
END_TEST

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
    send_command(&unit, &bank, "I0", false);
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
    send_command(&unit, &bank, "I1", true);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.last_drive, 1);
    ck_assert_uint_eq(unit.buffer_use, 0);
    send_command(&unit, &bank, "I", false);
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
    send_command(&unit, &bank, cases[which].text, false);
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
    send_command(&unit, &bank, "I1", false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NOT_READY);
    ck_assert_uint_eq(unit.selected_drive, 1);
    ck_assert_uint_eq(unit.last_drive, 0);
    ck_assert_uint_eq(image.reads, 0);
    image.fail_read = 1u + (phase != 0 && format != 0);
    send_command(&unit, &bank, "I0", false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert(!unit.free_blocks_valid[0]);
    ck_assert(!unit.bam[0].buffer.resident);
    ck_assert_uint_eq(image.writes + image.flushes, 0);
    image.fail_read = 0;
    send_command(&unit, &bank, "I0", false);
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
    send_command(&unit, &bank, "I0", false);
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
        send_command(&unit, &bank, rejected[index], false);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_SYNTAX);
    }
    send_command(&unit, &bank, "?", false);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_INVALID_COMMAND);
    ck_assert_uint_eq(image.reads + image.writes + image.flushes, 0);
    char request[DOS_COMMAND_RECEIVE_CAPACITY + 1u];
    memset(request, 'X', sizeof(request));
    request[0] = 'U';
    request[1] = '0';
    request[DOS_COMMAND_LIMIT] = '\r';
    request[DOS_COMMAND_RECEIVE_CAPACITY] = 0;
    send_command(&unit, &bank, request, true);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    request[DOS_COMMAND_LIMIT] = 'X';
    send_command(&unit, &bank, request, true);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_COMMAND_TOO_LONG);
    free(image.bytes);
}
END_TEST

// Compare circular direct data and EOI without retiring readiness.
static void expect_direct(dos_unit_t* unit, dos_media_bank_t* bank, uint8_t channel, const uint8_t* expected,
                          unsigned int count, bool final_eoi);

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
    send_command(&unit, &bank, "I0", false);
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "I1", false);
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
    send_command(&unit, &bank, "B-P:2,0", false);
    unit.selected_drive = 0;
    unit.last_drive = unit.last_program_drive = 1;
    unit.last_program_track = 1;
    unit.last_program_sector = 3;
    send_command(&unit, &bank, "?", false);
    const dos_unit_t before = unit;
    const dos_media_bank_t bindings = bank;
    const unsigned int reads = images[0].reads + images[1].reads;
    const unsigned int writes = images[0].writes + images[1].writes;
    const unsigned int flushes = images[0].flushes + images[1].flushes;
    for (unsigned int repeat = 0; repeat < 2; ++repeat) {
        send_command(&unit, &bank, repeat == 0 ? aliases[(unsigned int)_i % count_of(aliases)] : "U", false);
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
    send_command(&unit, &bank, request, true);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_COMMAND_TOO_LONG);
    ck_assert_uint_ne(dos_find_channel(&unit, 2), DOS_UNUSED);
    request[DOS_COMMAND_LIMIT] = '\r';
    send_command(&unit, &bank, request, true);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_ne(dos_find_channel(&unit, 2), DOS_UNUSED);
    send_command(&unit, &bank, "U", false);
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
    send_command(&unit, &bank, "I0", false);
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "I1", false);
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
    send_command(&unit, &bank, aliases[(unsigned int)_i % count_of(aliases)], false);
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
    send_command(&unit, &bank, "U", false);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_POWER_ON);
    char name[8];
    snprintf(name, sizeof(name), "#%u", buffer);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, name), DOS_UNUSED);
    send_command(&unit, &bank, "B-P:2,0", false);
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
    send_command(&unit, &bank, "I0", false);
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "B-A:0,1,0", false);
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "UI", false);
    if (operation == 1) {
        ck_assert_uint_ne(open_name(&unit, &bank, 2, "#"), DOS_UNUSED);
    } else if (operation == 2) {
        ck_assert_uint_eq(open_name(&unit, &bank, 3, "0:ABSENT,S,R"), DOS_UNUSED);
    } else if (operation >= 3) {
        static const char* const commands[] = {"R0:NEW=ABSENT", "S0:ABSENT", "C0:NEW=0:ABSENT"};
        send_command(&unit, &bank, commands[operation - 3u], false);
        finish_open(&unit, &bank);
    }
    send_command(&unit, &bank, "B-A:0,1,0", false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, format != 0 && operation != 0 ? DOS_STATUS_OK : DOS_STATUS_NO_BLOCK);
    ck_assert_uint_eq(image.writes + image.flushes, 0);
    free(image.bytes);
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
    send_command(&unit, &bank, command, false);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_COMMAND_TOO_LONG);
    ck_assert_mem_eq(&unit.channels[channel], &before, sizeof(before));
    ck_assert_uint_eq(image.reads + image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

// Create closed source/collision entries without borrowing rename implementation.
static void rename_fixture(dos_unit_t* const unit, dos_media_bank_t* const bank) {
    ck_assert_uint_ne(open_name(unit, bank, 2, "0:SOURCE,S,W"), DOS_UNUSED);
    put_bytes(unit, bank, 2, 0x20, 4);
    ck_assert(dos_close(unit, bank, bank->units[0].model, 2));
    ck_assert_uint_ne(open_name(unit, bank, 2, "0:EXISTS,S,W"), DOS_UNUSED);
    put_bytes(unit, bank, 2, 0x60, 2);
    ck_assert(dos_close(unit, bank, bank->units[0].model, 2));
}

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
    send_command(&unit, &bank, "R0:NEW=SOURCE", false);
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
    send_command(&unit, &bank, "B-P:2,255", false);
    send_command(&unit, &bank, "B-P:2,13", false);
    const dos_channel_t before = unit.channels[channel];
    const uint16_t owned = unit.buffer_use;
    const unsigned int reads = image.reads;
    send_command(&unit, &bank, raw ? "U1:2,0,1,0" : "B-R:2,0,1,0", false);
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
    send_command(&unit, &bank, pointer, false);
    const uint8_t last = page[raw ? DISKIMAGE_SECTOR_SIZE - 1u : count];
    expect_direct(&unit, &bank, channel, &last, 1, true);
    if (!raw && count == 4) {
        send_command(&unit, &bank, "B-P:2,3", false);
        put_bytes(&unit, &bank, 2, 0x69, 1);
        const uint8_t fourth = page[4];
        expect_direct(&unit, &bank, channel, &fourth, 1, true);
    }
    ck_assert_uint_eq(image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

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
    send_command(&unit, &bank, block_read_commands[variant].command, false);
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
    send_command(&unit, &bank, "B-R:2,0,1,0", false);
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
    send_command(&unit, &bank, "B-R:2,0,1,0", false);
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
    send_command(&unit, &bank, command, false);
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
    send_command(&unit, &bank, command, false);
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
    send_command(&unit, &bank, raw ? "U2:2,0,1,0" : "B-W:2,0,1,0", false);
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
    send_command(&unit, &bank, "B-W:2,0,1,0", false);
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
    send_command(&unit, &bank, "UB:2,0,1,0", false);
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
    send_command(&unit, &bank, cases[variant].command, false);
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
    send_command(&unit, &bank, command, false);
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
    send_command(&unit, &bank, command, false);
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
        send_command(&unit, &bank, "I0", false);
        finish_open(&unit, &bank);
        ck_assert_uint_ne(open_name(&unit, &bank, 2, "#"), DOS_UNUSED);
        set_map_version(&image, type, format, 0xff);
    }
    send_command(&unit, &bank, "U2:2,0,1,1", false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, read_failure ? DOS_STATUS_READ_ERROR : DOS_STATUS_OK);
    ck_assert_uint_eq(image.writes, read_failure ? 0 : 1);
    image.fail_read = 0;
    if (read_failure) {
        send_command(&unit, &bank, "U2:2,0,1,1", false);
        finish_open(&unit, &bank);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    } else {
        send_command(&unit, &bank, "I0", false);
        finish_open(&unit, &bank);
        ck_assert_uint_ne(open_name(&unit, &bank, 2, "#"), DOS_UNUSED);
        send_command(&unit, &bank, "U2:2,0,1,1", false);
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
    send_command(&unit, &bank, "I0", false);
    finish_open(&unit, &bank);
    const unsigned int writes = image.writes, flushes = image.flushes;
    uint8_t* const original = malloc(image.size);
    ck_assert_ptr_nonnull(original);
    memcpy(original, image.bytes, image.size);
    if (closing) {
        const uint8_t channel = open_name(&unit, &bank, 2, "#");
        send_command(&unit, &bank, "B-A:0,1,0", false);
        finish_open(&unit, &bank);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        ck_assert(!dos_close(&unit, &bank, model, 2));
        ck_assert_uint_eq(dos_find_channel(&unit, 2), channel);
        ck_assert(unit.bam[0].dirty);
    } else {
        send_command(&unit, &bank, "R0:NEW=OLD", false);
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
    send_command(&unit, &bank, "I0", false);
    finish_open(&unit, &bank);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "#"), DOS_UNUSED);
    send_command(&unit, &bank, "U2:2,0,1,1", false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    send_command(&unit, &bank, "B-F:0,1,0", false);
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "U2:2,0,1,1", false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_POWER_ON);
    send_command(&unit, &bank, format == 1 ? "B-F:0,77,0" : "B-F:0,154,0", false);
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "U2:2,0,1,1", false);
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
    send_command(&unit, &bank, "I0", false);
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "I1", false);
    finish_open(&unit, &bank);
    const uint8_t channel = open_name(&unit, &bank, 2, "#");
    send_command(&unit, &bank, "U1:2,0,1,0", false);
    finish_open(&unit, &bank);
    const dos_media_ref_t original = unit.channels[channel].media;
    const uint16_t drives = unit.buffer_drives;
    send_command(&unit, &bank, raw ? "U2:2,1,1,1" : "B-W:2,1,1,1", false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_POWER_ON);
    ck_assert_mem_eq(&unit.channels[channel].media, &original, sizeof(original));
    ck_assert_uint_eq(unit.buffer_drives, drives);
    send_command(&unit, &bank, "I1", false);
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
    send_command(&unit, &bank, command, false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(images[drive].writes, 1);
    ck_assert_uint_eq(images[drive ^ 1u].writes, 0);
    ck_assert_mem_eq(sector_bytes(&images[drive], second.image.type, track, sector), page, DISKIMAGE_SECTOR_SIZE);
    ck_assert_uint_eq(unit.channels[channel].media.slot, drive);
    ck_assert_uint_eq((unit.buffer_drives >> unit.channels[channel].buffers[0]) & 1u, drive);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    snprintf(command, sizeof(command), "I%u", drive);
    send_command(&unit, &bank, command, false);
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
        send_command(&unit, &bank, command, false);
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
    send_command(&unit, &bank, command, false);
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
    send_command(&unit, &bank, cases[variant].command, false);
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
    send_command(&unit, &bank, "B-A:0,1,0", false);
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
    };
    const unsigned int format = (unsigned int)_i / count_of(cases), which = (unsigned int)_i % count_of(cases);
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    rename_fixture(&unit, &bank);
    const unsigned int writes = image.writes;
    send_command(&unit, &bank, cases[which].command, true);
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
    send_command(&unit, &bank, "R0:NEW=1:SOURCE", false);
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
    send_command(&unit, &bank, "R0:NEW=SOURCE", false);
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
    send_command(&unit, &bank, "R0:NEW=SOURCE", false);
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
    send_command(&unit, &bank, "R0:NEW=SOURCE", false);
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
    send_command(&unit, &bank, "R0:NEW=SOURCE", false);
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
    send_command(&unit, &bank, absent ? "R0:NEW=SOURCE" : "R:NEW=SOURCE", false);
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
    send_command(&unit, &bank, "R0:NEW=SOURCE", false);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
    ck_assert_uint_eq(unit.buffer_use, exhausted);
    ck_assert(!dos_work_pending(&unit));
    unit.buffer_use = buffers;
    send_command(&unit, &bank, "R0:NEW=SOURCE", false);
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
    send_command(&unit, &bank, "S0:SOURCE", false);
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
    };
    const unsigned int format = (unsigned int)_i / count_of(cases), which = (unsigned int)_i % count_of(cases);
    image_t image;
    dos_media_bank_t bank = {0};
    mount_blank(&bank, &image, format);
    dos_unit_t unit;
    dos_reset(&unit, bank.units[0].model);
    rename_fixture(&unit, &bank);
    const unsigned int writes = image.writes;
    send_command(&unit, &bank, cases[which].command, true);
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
    const unsigned int format = (unsigned int)_i / 7u, mode = (unsigned int)_i % 7u;
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
    } else {
        entry[2] = 0x84;
        dos_media_ref_t reference;
        ck_assert_int_eq(dos_media_reference(&bank, 0, 0, &reference), DOS_MEDIA_OK);
        channel = dos_rel_channel_claim(&unit, reference, 2);
        ck_assert_uint_ne(channel, DOS_UNUSED);
        unit.channels[channel].directory_track = format == 0 ? 18 : 39;
        unit.channels[channel].directory_sector = 1;
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
    send_command(&unit, &bank, "S0:SOURCE", false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, mode == 5 ? DOS_STATUS_READ_ERROR : DOS_STATUS_FILES_SCRATCHED);
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
    send_command(&unit, &bank, mode == 0 ? "S1:A" : mode == 1 ? "S0:A,1:B" : "S1:A,B", false);
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
    send_command(&unit, &bank, "S0:A", false);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(unit.buffer_use, exhausted);
    ck_assert(!dos_work_pending(&unit));
    unit.buffer_use = buffers;
    const uint16_t retained = unit.buffer_use;
    send_command(&unit, &bank, "S0:A,0:B,0:C,0:D,0:E,0:F", false);
    finish_open(&unit, &bank);
    ck_assert_str_eq(unit.status, _i == 0 ? "01, FILES SCRATCHED,05,00\r" : "01, FILES SCRATCHED,05,00,0\r");
    ck_assert_uint_eq(unit.buffer_use, retained);
    expect_bytes(&unit, &bank, dos_find_channel(&unit, 2), 4, 0x20);
    free(image.bytes);
}
END_TEST

// SCHTBL's mixed defaults defer an opposite-drive fallback until the third search.
START_TEST(test_scratch_mixed_defaults_require_third_drive_search) {
    const unsigned int format = (unsigned int)_i;
    image_t first, second;
    dos_media_bank_t bank = {0}, other = {0};
    mount_blank(&bank, &first, format);
    mount_blank(&other, &second, format);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, &other.units[0].slots[0].io), DOS_MEDIA_OK);
    dos_unit_t unit;
    const dos_model_t model = bank.units[0].model;
    dos_reset(&unit, model);
    for (const char* name = "ABCDE"; *name != 0; ++name) {
        char request[16];
        snprintf(request, sizeof(request), "%u:%c,S,W", *name == 'B' ? 1u : 0u, *name);
        ck_assert_uint_ne(open_name(&unit, &bank, 2, request), DOS_UNUSED);
        put_bytes(&unit, &bank, 2, 0x20, 4);
        ck_assert(dos_close(&unit, &bank, model, 2));
    }
    send_command(&unit, &bank, "S0:A,1:B,C,0:D,E", false);
    ck_assert_uint_eq(unit.scratch.scans, 2);
    finish_open(&unit, &bank);
    ck_assert_str_eq(unit.status, format == 0 ? "01, FILES SCRATCHED,05,00\r" : "01, FILES SCRATCHED,05,00,0\r");
    ck_assert_uint_eq(unit.last_drive, 0);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(first.bytes);
    free(second.bytes);
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
    send_command(&unit, &bank, "S0:SOURCE", false);
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
    send_command(&unit, &bank, "S0:PARTS", false);
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
    send_command(&unit, &bank, "S0:S*", false);
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
    send_command(&unit, &bank, "C0:NEW=0:SOURCE", false);
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
    send_command(&unit, &bank, cases[which].command, false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, cases[which].status);
    ck_assert_uint_eq(unit.buffer_use, 0);
    if (cases[which].status != DOS_STATUS_OK) {
        ck_assert_uint_eq(image.writes, writes);
    } else {
        send_command(&unit, &bank, "R0:RENAMED=NEW", false);
        finish_open(&unit, &bank);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        send_command(&unit, &bank, "S0:RENAMED", false);
        finish_open(&unit, &bank);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_FILES_SCRATCHED);
    }
    send_command(&unit, &bank, "C0:RECOVER=0:SOURCE", false);
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
    send_command(&unit, &bank, "C0:NEW=0:SOURCE,0:OTHER", false);
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
    send_command(&unit, &bank, "C0:SOURCE=0:SOURCE,0:OTHER", false);
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
    send_command(&unit, &bank, "C0:NEW=0:SOURCE", false);
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
    ck_assert(dos_finalize(&unit, &bank));
    ck_assert_uint_eq(image.writes + replacement.writes, writes);
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
                                          DOS_STATUS_DISK_FULL, DOS_STATUS_DIRECTORY_ERROR};
    const uint16_t buffers = unit.buffer_use;
    const unsigned int writes = image.writes;
    send_command(&unit, &bank, "C0:NEW=0:SOURCE", false);
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
    send_command(&unit, &bank, "C0:NEW=A,1:B,C", false);
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
    send_command(&unit, &bank, "V0", false);
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
    send_command(&unit, &bank, "VALIDATE0:IGNORED", false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_mem_eq(image.bytes, expected, image.size);
    send_command(&unit, &bank, "C0:NEW=0:SOURCE", false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    send_command(&unit, &bank, "R0:RENAMED=NEW", false);
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "S0:RENAMED", false);
    finish_open(&unit, &bank);
    send_command(&unit, &bank, "V0", false);
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
    send_command(&unit, &bank, commands[which], false);
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
    send_command(&unit, &bank, "V0", false);
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
    send_command(&unit, &bank, "V0", false);
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
    send_command(&unit, &bank, "V0", false);
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
    send_command(&unit, &bank, "V0", false);
    unsigned int steps = 0;
    while (dos_work_pending(&unit)) {
        ck_assert_uint_lt(steps++, image.size / DISKIMAGE_SECTOR_SIZE + 128);
        dos_work(&unit, &bank);
    }
    ck_assert_uint_eq(unit.status_code, variant >= 2                  ? DOS_STATUS_OK
                                        : variant == 1 && format != 0 ? DOS_STATUS_NO_BLOCK
                                                                      : DOS_STATUS_READ_ERROR);
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
    send_command(&unit, &bank, command, false);
    finish_open(&unit, &bank);
    ck_assert_str_eq(unit.status, operation == 0
                                      ? (format == 0 ? "01, FILES SCRATCHED,01,00\r" : "01, FILES SCRATCHED,01,00,0\r")
                                      : (format == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r"));
    ck_assert_mem_eq(image.bytes, oracle.bytes, image.size);
    ck_assert_uint_eq(unit.buffer_use, 0);
    // Repeated commands are idempotent and preserve the same persistent image.
    send_command(&unit, &bank, command, false);
    finish_open(&unit, &bank);
    ck_assert_str_eq(unit.status, operation == 0
                                      ? (format == 0 ? "01, FILES SCRATCHED,00,00\r" : "01, FILES SCRATCHED,00,00,0\r")
                                      : (format == 0 ? "00, OK,00,00\r" : "00, OK,00,00,0\r"));
    ck_assert_mem_eq(image.bytes, oracle.bytes, image.size);
    free(oracle.bytes);
    free(image.bytes);
}
END_TEST

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
        send_command(&unit, &bank, "C0:COPY=0:MISSING", false);
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
    send_command(&unit, &bank, "C0:COPY=1:REL", false);
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
    send_command(&unit, &bank, "C0:COPY=0:REL", false);
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
            send_command(&unit, &bank, "C0:COPY=0:REL", false);
            finish_open(&unit, &bank);
            completed = unit.status_code == DOS_STATUS_OK || operation == 2;
            if (unit.status_code != DOS_STATUS_OK) {
                ck_assert_uint_eq(unit.status_code, operation == 1 ? DOS_STATUS_READ_ERROR : DOS_STATUS_WRITE_ERROR);
            }
            ck_assert_uint_eq(unit.buffer_use, pages);
            ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
            const unsigned int writes = image.writes;
            ck_assert(dos_finalize(&unit, &bank));
            ck_assert_uint_eq(image.writes, writes);
            free(image.bytes);
        }
        ck_assert(completed);
    }
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
    send_command(&unit, &bank, "V0", false);
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
    send_command(&unit, &bank, "V0", false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, format == 0 ? DOS_STATUS_OK : DOS_STATUS_NO_CHANNEL);
    ck_assert_uint_eq(unit.buffer_use, buffers);
    ck_assert_mem_eq(unit.channels, saved, sizeof(saved));
    ck_assert(dos_close(&unit, &bank, model, 15));
    send_command(&unit, &bank, "V0", false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_int_eq(dos_media_set_slot(&bank, 0, 1, NULL), DOS_MEDIA_OK);
    send_command(&unit, &bank, "V1", false);
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
    send_command(&unit, &bank, "V0", false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_mem_eq(image.bytes, oracle.bytes, image.size);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(oracle.bytes);
    free(image.bytes);
}
END_TEST

// STRDBL's first prefetch survives INITIALIZE and later becomes COPY's unused tail.
START_TEST(test_validate_preserves_abandoned_reader_prefetch_residue) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    const diskimage_type_t type = bank.units[0].slots[0].io.image.type;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:SOURCE,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0, 261);
    ck_assert(dos_close(&unit, &bank, model, 2));
    uint8_t* const directory = sector_bytes(&image, type, format == 0 ? 18 : 39, 1);
    uint8_t* const first = sector_bytes(&image, type, directory[3], directory[4]);
    uint8_t* const second = sector_bytes(&image, type, first[0], first[1]);
    for (unsigned int offset = 9; offset < DISKIMAGE_SECTOR_SIZE; ++offset) {
        second[offset] = offset + 0x30;
    }
    const uint8_t reader = open_name(&unit, &bank, 2, "0:SOURCE,S,R");
    ck_assert_mem_eq(unit.buffers[unit.channels[reader].buffers[1]].bytes, second, DISKIMAGE_SECTOR_SIZE);
    send_command(&unit, &bank, "V0", false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    send_command(&unit, &bank, "C0:NEW=0:SOURCE", false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    uint8_t* const copied_first = sector_bytes(&image, type, directory[35], directory[36]);
    ck_assert_mem_eq(sector_bytes(&image, type, copied_first[0], copied_first[1]), second, DISKIMAGE_SECTOR_SIZE);
    free(image.bytes);
}
END_TEST

// A failed asynchronous OPEN prefetch becomes an error only at the sector boundary.
START_TEST(test_open_prefetch_failure_waits_for_actual_buffer_selection) {
    const unsigned int format = (unsigned int)_i;
    dos_media_bank_t bank = {0};
    image_t image;
    mount_blank(&bank, &image, format);
    const dos_model_t model = bank.units[0].model;
    dos_unit_t unit;
    dos_reset(&unit, model);
    ck_assert_uint_ne(open_name(&unit, &bank, 2, "0:SOURCE,S,W"), DOS_UNUSED);
    put_bytes(&unit, &bank, 2, 0x20, 255);
    ck_assert(dos_close(&unit, &bank, model, 2));
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
    send_command(&unit, &bank, pointer_commands[variant].command, false);
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
    send_command(&unit, &bank, "B-P:2,0", false);
    put_bytes(&unit, &bank, 2, 0, DISKIMAGE_SECTOR_SIZE);
    const uint8_t zero = 0;
    expect_direct(&unit, &bank, channel, &zero, 1, true);
    for (unsigned int offset = 0; offset < DISKIMAGE_SECTOR_SIZE; ++offset) {
        char command[24];
        snprintf(command, sizeof(command), "B-P:2,%u", offset);
        send_command(&unit, &bank, command, false);
        ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
        const uint8_t expected = offset;
        expect_direct(&unit, &bank, channel, &expected, 1, offset + 1u == DISKIMAGE_SECTOR_SIZE);
    }
    send_command(&unit, &bank, "B-P:2,255", false);
    send_command(&unit, &bank, "B-P:2,13", false);
    const uint8_t cr = '\r';
    expect_direct(&unit, &bank, channel, &cr, 1, true);
    send_command(&unit, &bank, "B-P:2,0", false);
    put_bytes(&unit, &bank, 2, 0x5a, 1);
    send_command(&unit, &bank, "B-P:2,0", false);
    const uint8_t changed = 0x5a;
    expect_direct(&unit, &bank, channel, &changed, 1, false);
    ck_assert_mem_eq(&unit.channels[peer], &saved_peer, sizeof(saved_peer));
    ck_assert_uint_eq(image.reads + image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

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
    send_command(&unit, &bank, commands[(unsigned int)_i % count_of(commands)], false);
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
    send_command(&unit, &bank, "C0=1", false);
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
    send_command(&unit, &bank, commands[(unsigned int)_i % count_of(commands)], (bool)(_i & 1));
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
    send_command(&unit, &bank, command, false);
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
    send_command(&unit, &bank, commands[variant], false);
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
    send_command(&unit, &bank, commands[(unsigned int)_i % count_of(commands)], false);
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
    send_command(&unit, &bank, commands[(unsigned int)_i % count_of(commands)], false);
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
    send_command(&unit, &bank, _i < 3 ? "C0=1" : "C0:*=1:*", false);
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
    send_command(&unit, &bank, "C0:AFTER=1:ONE", false);
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
    send_command(&unit, &bank, "C0=1", false);
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
    send_command(&unit, &bank, "C1=0", true);
    ck_assert(unit.copy.pending);
    ck_assert_uint_eq(source.reads + target.reads + source.writes + target.writes, 0);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.last_drive, 0);
    const uint8_t expected[] = {'A'};
    expect_copy_payload(&target, bank.units[0].slots[0].io.image.type, "ONE", expected, sizeof(expected),
                        DISKIMAGE_FTYPE_PRG);
    const unsigned int writes = target.writes;
    send_command(&unit, &bank, "U0X0", false);
    send_command(&unit, &bank, "C1=", false);
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
    send_command(&unit, &bank, _i < 3 ? "C0=1" : "C0:*=1:F?", false);
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
    send_command(&unit, &bank, command, false);
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
    send_command(&unit, &bank, "B-P:2,2", false);
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
    send_command(&unit, &bank, "B-P:2,0", false);
    finish_open(&unit, &bank);
    const uint8_t zero = 0, a = 'A';
    expect_direct(&unit, &bank, reader, &zero, 1, true);
    send_command(&unit, &bank, "B-P:2,0", false);
    finish_open(&unit, &bank);
    uint8_t byte;
    bool eoi;
    ck_assert_uint_eq(dos_stage(&unit, &bank, reader, &byte, 1, &eoi), 0);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    send_command(&unit, &bank, "B-P:2,2", false);
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
    send_command(&unit, &bank, "B-P:2,4", false);
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

// A failed lazy DBLBUF normalization must not consume the old prepared source.
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
    send_command(&unit, &bank, "B-P:2,2", false);
    ck_assert_uint_eq(image.reads + 1, image.fail_read);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_mem_eq(&unit.channels[reader], &before, sizeof(before));
    image.fail_read = 0;
    const uint8_t retained[] = {253, 254};
    expect_direct(&unit, &bank, reader, retained, sizeof(retained), false);
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
    send_command(&unit, &bank, raw ? "U1:2,0,1,3" : "B-R:2,0,1,3", false);
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
    send_command(&unit, &bank, command, false);
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
        send_command(&unit, &bank, "B-P:2,4", false);
        finish_open(&unit, &bank);
    }
    const uint8_t active = consumed >= 253u ? 1 : 0;
    const uint8_t other = unit.channels[reader].buffers[active ^ 1u];
    const dos_buffer_t saved_other = unit.buffers[other];
    send_command(&unit, &bank, "B-R:2,0,1,3", false);
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
    send_command(&unit, &bank, "B-R:2,1,1,3", false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    send_command(&unit, &bank, "B-P:2,255", false);
    finish_open(&unit, &bank);
    const uint8_t expected[] = {0x5a, 0x5a, 254, 255};
    expect_direct(&unit, &bank, reader, expected, sizeof(expected), false);
    ck_assert_uint_eq(unit.channels[reader].media.slot, 0);
    send_command(&unit, &bank, "B-P:2,8", false);
    finish_open(&unit, &bank);
    const uint8_t last = 4;
    expect_direct(&unit, &bank, reader, &last, 1, true);
    ck_assert_uint_eq(source.reads, reads);
    ck_assert_uint_eq(source.writes + target.writes + source.flushes + target.flushes, 0);
    free(source.bytes);
    free(target.bytes);
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
    send_command(&unit, &bank, raw ? "UA:2,0,1,3" : "B-R:2,0,1,3", true);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    const uint8_t counted[] = {'T', 'T', 'E', 'S', 'T'}, whole[] = {4, 4, 'T', 'E'};
    expect_direct(&unit, &bank, reader, raw ? whole : counted, raw ? sizeof(whole) : sizeof(counted), !raw);
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    ck_assert_uint_eq(image.writes + image.flushes, 0);
    free(image.bytes);
}
END_TEST

// Failed source normalization must not consume or replace the prepared reader.
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
    send_command(&unit, &bank, commands[(unsigned int)_i % count_of(commands)], false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_READ_ERROR);
    ck_assert_mem_eq(&unit.channels[reader], &before, sizeof(before));
    image.fail_read = 0;
    const uint8_t retained[] = {253, 254};
    expect_direct(&unit, &bank, reader, retained, sizeof(retained), false);
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
    send_command(&unit, &bank, command, operation == 2);
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
    send_command(&unit, &bank, command, false);
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
    send_command(&unit, &bank, raw ? "U2:2,1,1,3" : "B-W:2,1,1,3", false);
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
    send_command(&unit, &bank, "UB:2,1,1,3", true);
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
        send_command(&unit, &bank, "B-R:2,0,1,4", false);
        finish_open(&unit, &bank);
        const uint8_t byte = 0x44;
        expect_direct(&unit, &bank, reader, &byte, 1, true);
    }
    send_command(&unit, &bank, raw ? "UB:2,0,1,3" : "B-W:2,0,1,3", false);
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
    dos_input_address(&unit, &bank, bank.units[0].model, IEEE_CMD_SECONDARY(2), true);
    dos_listen_put(&unit, &bank, 0x99, true);
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
    send_command(&unit, &bank, "B-P:2,4", false);
    finish_open(&unit, &bank);
    send_command(&unit, &bank, raw ? "U2:2,0,1,3" : "B-W:2,0,1,3", false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    if (!raw) {
        expected[0] = 3;
    }
    ck_assert_mem_eq(sector_bytes(&image, type, 1, 3), expected, sizeof(expected));
    ck_assert_int_eq(unit.channels[reader].access, access);
    ck_assert_uint_eq(unit.channels[reader].read_byte, expected[raw ? 4 : 1]);
    const dos_channel_t before = unit.channels[reader];
    dos_input_address(&unit, &bank, bank.units[0].model, IEEE_CMD_SECONDARY(2), true);
    dos_listen_put(&unit, &bank, 0x99, true);
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
    send_command(&unit, &bank, writer_commands[operation], false);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_mem_eq(&unit.channels[writer], &saved_writer, sizeof(saved_writer));
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
    const uint8_t reader = open_name(&unit, &bank, 2, "0:WRITER,S,R");
    ck_assert_uint_ne(reader, DOS_UNUSED);
    reads = image.reads;
    send_command(&unit, &bank, writer_commands[operation], false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(image.reads, reads + (operation == 1 || operation == 2 ? 1 : 0));
    const uint8_t allowed = open_name(&unit, &bank, 13, "#");
    ck_assert_uint_ne(allowed, DOS_UNUSED);
    send_command(&unit, &bank, allowed_commands[operation], false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.channels[allowed].position, operation == 0 ? 255 : operation == 2 ? 0 : 1);
    const uint8_t excluded = open_name(&unit, &bank, 14, "#");
    ck_assert_uint_ne(excluded, DOS_UNUSED);
    const dos_channel_t saved_excluded = unit.channels[excluded];
    reads = image.reads;
    send_command(&unit, &bank, excluded_commands[operation], false);
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
    send_command(&unit, &bank, "B-P:2,17", false);
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
    mount_image(&bank, &image, format);
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
    mount_image(&bank, &image, format);
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
    mount_image(&bank, &image, (unsigned int)_i);
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
    send_command(&unit, &bank, "I0", false);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(dos_find_channel(&unit, 2), format == 0 ? channel : DOS_UNUSED);
    send_command(&unit, &bank, "I1", false);
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
    send_command(&unit, &bank, "V0", false);
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
    ck_assert(dos_close(&unit, &bank, bank.units[0].model, 2));
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

// Direct LOAD/SAVE allocate pages too, and length rejection precedes parsing.
START_TEST(test_direct_load_save_and_overlong_request) {
    dos_media_bank_t bank = {0};
    image_t image;
    mount_image(&bank, &image, (unsigned int)_i);
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
    uint8_t overlong[DOS_COMMAND_RECEIVE_CAPACITY];
    memset(overlong, '0', sizeof(overlong));
    overlong[0] = '#';
    dos_open(&unit, &bank, 0, 2, overlong, sizeof(overlong));
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_COMMAND_TOO_LONG);
    ck_assert_mem_eq(&unit.channels[channel], &saved, sizeof(saved));
    ck_assert_uint_eq(image.reads + image.writes, 0);
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
    send_command(&unit, &bank, full ? "N0:RENAMED,AB" : "N0:RENAMED", false);
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
    send_command(&unit, &bank, target == 0 ? "D0=1" : "D1=0", false);
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
    send_command(&unit, &bank, variant >= 4 ? "D0=1" : variant == 3 ? "N0:NEW,AB" : "N0:NEW", false);
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
    send_command(&unit, &bank, full ? "N0:NEW,AB" : "N0:NEW", false);
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
    send_command(&unit, &bank, duplicate ? "D0=1" : "N0:NEW,AB", false);
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
    send_command(&unit, &bank, variant >= 2 ? "D0=1" : variant == 1 ? "N0:NEW,AB" : "N0:NEW", false);

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
    send_command(&unit, &bank, "U;1234Z", false);
    ck_assert_uint_eq(unit.input.bytes[5], '4');
    unit.last_drive = 1;
    send_command(&unit, &bank, variant == 0 ? "N:X,A" : variant == 1 ? "N:X," : "N:,", true);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_OK);
    ck_assert_uint_eq(unit.last_drive, 1);
    ck_assert_uint_eq(untouched.reads + untouched.writes + untouched.flushes, 0);
    const unsigned int name = format == 0 ? 144u : 6u;
    const uint8_t* const header = sector_bytes(&image, type, format == 0 ? 18u : 39u, 0);
    ck_assert_uint_eq(header[name], variant == 2 ? 0xa0 : 'X');
    ck_assert_uint_eq(header[name + 18u], variant == 0 ? 'A' : '0');
    // "N:X,A" has length five, so its second ID byte is the retained byte '4'.
    ck_assert_uint_eq(header[name + 19u], variant == 0 ? '4' : '1');
    ck_assert_uint_eq(sector_bytes(&image, type, 1, 0)[0], variant == 0 ? 0 : 0x5a);
    ck_assert_uint_eq(unit.buffer_use, 0);
    free(untouched.bytes);
    free(image.bytes);
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
    send_command(&unit, &bank, request, operation % 2u != 0);
    finish_open(&unit, &bank);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
    ck_assert_mem_eq(&unit.channels[channel], &before, sizeof(before));
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    free(image.bytes);
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
    send_command(&unit, &bank, "P\002", command_open);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_TYPE_MISMATCH);
    send_command(&unit, &bank, "P", command_open);
    ck_assert_uint_eq(unit.status_code, DOS_STATUS_NO_CHANNEL);
    ck_assert_uint_eq(image.reads, reads);
    ck_assert_uint_eq(image.writes, writes);
    ck_assert_uint_eq(image.flushes, flushes);
    free(image.bytes);
}
END_TEST

// Register direct DOS tests independently of SPI and fabric mocks.
Suite* dos_suite(void) {
    Suite* const suite = suite_create("dos");
    TCase* const core = tcase_create("core");
    tcase_add_loop_test(core, test_open_parse_binary_record_size, 0, 256);
    tcase_add_loop_test(core, test_open_parse_rel_parameter_order_and_tail, 0, 8);
    tcase_add_loop_test(core, test_rel_scratch_and_validate_native_chains, 0, 18);
    tcase_add_loop_test(core, test_public_rel_create_write_close_and_remount, 0, 21);
    tcase_add_loop_test(core, test_public_rel_creation_faults_retain_publication_and_peer, 0, 18);
    tcase_add_loop_test(core, test_copy_rel_short_records_and_native_preallocation, 0, 6);
    tcase_add_loop_test(core, test_copy_rel_resource_pressure_preserves_public_owners, 0, 6);
    tcase_add_loop_test(core, test_public_rel_directory_growth_finalization_and_retry, 0, 9);
    tcase_add_loop_test(core, test_public_zero_size_rel_is_cooperative_and_resettable, 0, 3);
    tcase_add_loop_test(core, test_public_rel_zero_size_directory_policy, 0, 3);
    tcase_add_loop_test(core, test_copy_rel_storage_faults_release_only_internal_pages, 0, 3);
    tcase_add_loop_test(core, test_record_rejects_missing_and_nonrelative_channels, 0, 6);
    tcase_add_loop_test(core, test_format_quick_and_full_complete_images, 0, 6);
    tcase_add_loop_test(core, test_duplicate_complete_image_and_source_isolation, 0, 6);
    tcase_add_loop_test(core, test_format_and_duplicate_storage_failures, 0, 27);
    tcase_add_loop_test(core, test_format_channel_lifecycle, 0, 6);
    tcase_add_loop_test(core, test_format_and_duplicate_preserve_busy_scratch, 0, 6);
    tcase_add_loop_test(core, test_format_and_duplicate_reject_replaced_media, 0, 24);
    tcase_add_loop_test(core, test_format_id_and_inherited_drive_boundaries, 0, 9);
    tcase_add_loop_test(core, test_block_commands_reject_allocated_load_and_save, 0, 30);
    tcase_add_loop_test(core, test_block_reads_counted_and_raw_boundaries, 0, 3 * 2 * 6);
    tcase_add_loop_test(core, test_block_read_parser_geometry_and_user_aliases, 0, 3 * count_of(block_read_commands));
    tcase_add_loop_test(core, test_block_read_failures_and_stale_identity, 0, 9);
    tcase_add_loop_test(core, test_block_read_drive_affinity_and_readonly_last_track, 0, 6);
    tcase_add_loop_test(core, test_block_write_pointer_count_and_raw_state, 0, 3 * 2 * 3 * 6);
    tcase_add_loop_test(core, test_block_write_failures_and_stale_identity, 0, 15);
    tcase_add_loop_test(core, test_write_version_counted_raw_and_drive, 0, 48);
    tcase_add_loop_test(core, test_write_version_read_retry_and_cached_part, 0, 6);
    tcase_add_loop_test(core, test_write_version_metadata_jobs, 0, 6);
    tcase_add_loop_test(core, test_rejected_block_write_preserves_drive_affinity, 0, 6);
    tcase_add_loop_test(core, test_write_version_follows_cached_map_part, 0, 2);
    tcase_add_loop_test(core, test_warm_reset_retains_ram_and_model_specific_maps, 0, 18);
    tcase_add_loop_test(core, test_user_interrupt_retains_channels_maps_and_command_bytes, 0, 18);
    tcase_add_loop_test(core, test_user_interrupt_overlong_and_trimmed_request, 0, 3);
    tcase_add_loop_test(core, test_warm_reset_overlong_request, 0, 3);
    tcase_add_loop_test(core, test_warm_reset_auto_initialize_entry_points, 0, 18);
    tcase_add_loop_test(core, test_block_write_parser_and_aliases, 0, 42);
    tcase_add_loop_test(core, test_block_write_drive_affinity_and_last_track, 0, 12);
    tcase_add_loop_test(core, test_block_map_claim_free_and_close_persistence, 0, 6);
    tcase_add_loop_test(core, test_block_map_successor_search_and_exhaustion, 0, 15);
    tcase_add_loop_test(core, test_block_map_parser_geometry_and_no_channel, 0, 42);
    tcase_add_loop_test(core, test_block_map_failures_and_readonly_close, 0, 18);
    tcase_add_loop_test(core, test_block_pointer_parser_and_rejection_atomicity, 0, 3 * count_of(pointer_commands));
    tcase_add_loop_test(core, test_block_pointer_all_offsets_put_and_sticky_eoi, 0, 3);
    tcase_add_loop_test(core, test_block_pointer_channel_permissions_and_excluded_readers, 0, 15);
    tcase_add_loop_test(core, test_reader_pointer_offsets_and_prepared_byte, 0, 3 * 2 * 9);
    tcase_add_loop_test(core, test_reader_pointer_materializes_active_buffer, 0, 9);
    tcase_add_loop_test(core, test_reader_pointer_sticky_eoi_and_revival, 0, 3);
    tcase_add_loop_test(core, test_reader_pointer_modify_and_raw_directory_are_read_only, 0, 9);
    tcase_add_loop_test(core, test_reader_pointer_failed_normalization_preserves_cursor, 0, 3);
    tcase_add_loop_test(core, test_reader_block_counted_and_raw_boundaries, 0, 3 * 2 * 6);
    tcase_add_loop_test(core, test_reader_block_materializes_active_page, 0, 24);
    tcase_add_loop_test(core, test_reader_block_zero_count_retains_prefetched_source_drive, 0, 3);
    tcase_add_loop_test(core, test_reader_block_modify_and_raw_directory_are_read_only, 0, 18);
    tcase_add_loop_test(core, test_reader_block_failed_normalization_preserves_cursor, 0, 9);
    tcase_add_loop_test(core, test_reader_block_writes_materialize_native_pointer, 0, 3 * 2 * 3 * 8);
    tcase_add_loop_test(core, test_reader_block_write_failures_retain_page_and_source, 0, 3 * 2 * 7);
    tcase_add_loop_test(core, test_reader_block_write_singleton_and_eof, 0, 3 * 2 * 3);
    tcase_add_loop_test(core, test_reader_block_write_modify_and_directory, 0, 3 * 2 * 3);
    tcase_add_loop_test(core, test_block_pointer_media_changes_do_not_issue_storage_jobs, 0, 6);
    tcase_add_loop_test(core, test_direct_parser_and_single_buffer_ownership, 0, 3 * count_of(direct_names));
    tcase_add_loop_test(core, test_direct_put_wrap_sticky_eoi_and_partial_get, 0, 9);
    tcase_add_loop_test(core, test_direct_capacity_reopen_and_reserved_page_retention, 0, 3);
    tcase_add_loop_test(core, test_direct_explicit_page_drive_and_initialize, 0, 3);
    tcase_add_loop_test(core, test_direct_reopen_abandons_writer_and_validate_releases_page, 0, 3);
    tcase_add_loop_test(core, test_direct_ram_survives_media_change_without_replacement_io, 0, 6);
    tcase_add_loop_test(core, test_direct_close_retries_dirty_bam_and_backing_flush, 0, 9);
    tcase_add_loop_test(core, test_direct_load_save_and_overlong_request, 0, 3);
    tcase_add_loop_test(core, test_validate_repairs_maps_deletes_splats_and_preserves_counts, 0, 3);
    tcase_add_loop_test(core, test_validate_drive_selection_and_peer_cursor, 0, 18);
    tcase_add_loop_test(core, test_validate_crosslinks_are_model_specific, 0, 3);
    tcase_add_loop_test(core, test_validate_storage_errors_and_retry, 0, 18);
    tcase_add_loop_test(core, test_validate_corruption_and_side_links, 0, 12);
    tcase_add_loop_test(core, test_validate_stale_media_never_reaches_replacement_io, 0, 9);
    tcase_add_loop_test(core, test_validate_resource_pressure_and_absent_slot, 0, 3);
    tcase_add_loop_test(core, test_validate_marks_all_bam_parts_and_directory_extensions, 0, 3);
    tcase_add_loop_test(core, test_validate_preserves_abandoned_reader_prefetch_residue, 0, 3);
    tcase_add_loop_test(core, test_open_prefetch_failure_waits_for_actual_buffer_selection, 0, 3);
    tcase_add_loop_test(core, test_copy_preserves_boundary_payloads_and_peer_cursor, 0, 21);
    tcase_add_loop_test(core, test_copy_parser_precedence_and_command_union_reuse, 0, 51);
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
    tcase_add_loop_test(core, test_scratch_releases_exact_chain_and_preserves_peer_reader, 0, 3);
    tcase_add_loop_test(core, test_scratch_native_parser_patterns_and_counts, 0, 60);
    tcase_add_loop_test(core, test_scratch_active_locked_unclosed_and_rel_entries, 0, 21);
    tcase_add_loop_test(core, test_scratch_cross_drive_patterns_and_default_selection, 0, 9);
    tcase_add_loop_test(core, test_scratch_pattern_limit_and_resource_recovery, 0, 3);
    tcase_add_loop_test(core, test_scratch_mixed_defaults_require_third_drive_search, 0, 3);
    tcase_add_loop_test(core, test_scratch_storage_failures_and_stale_media, 0, 27);
    tcase_add_loop_test(core, test_scratch_releases_chains_across_bam_parts, 1, 3);
    tcase_add_loop_test(core, test_scratch_yields_reenters_and_bounds_corrupt_chains, 0, 6);
    tcase_add_loop_test(core, test_geterc_retains_partial_status_and_consumes_cr, 0, 2);
    tcase_add_loop_test(core, test_opnrch_staging_is_reversible_across_sector_links, 0, 3);
    tcase_add_loop_test(core, test_getrch_fndlnx_and_frechn_own_finite_resources, 0, 3);
    tcase_add_loop_test(core, test_rel_claim_three_pages_and_release, 0, 3);
    tcase_add_loop_test(core, test_open_retains_failed_rel_close, 0, 3);
    tcase_add_loop_test(core, test_rel_claim_partial_exhaustion_and_logical_reservations, 0, 15);
    tcase_add_loop_test(core, test_rel_claim_mixed_peer_and_reserved_index, 0, 3);
    tcase_add_test(core, test_empty_slot_and_staged_features_fail_explicitly);
    tcase_add_test(core, test_open_job_yields_between_directory_sectors);
    tcase_add_loop_test(core, test_open_job_rejects_remounted_media_before_io, 0, 3);
    tcase_add_loop_test(core, test_input_open_is_deferred_and_reset_is_unit_local, 0, 3);
    tcase_add_loop_test(core, test_input_command_capacity_and_channel_selection, 0, 6);
    tcase_add_loop_test(core, test_input_command_open_and_close_semantics, 0, 3);
    tcase_add_loop_test(core, test_output_retains_source_and_commits_only_actual_progress, 0, 3);
    tcase_add_loop_test(core, test_native_create_close_and_boundaries, 0, 21);
    tcase_add_loop_test(core, test_native_append_replace_and_peer_directory_entries, 0, 3);
    tcase_add_loop_test(core, test_close_storage_failures_retain_and_recover, 0, 12);
    tcase_add_loop_test(core, test_formatted_directory_retains_exact_rows_and_final_eoi, 0, 3);
    tcase_add_loop_test(core, test_write_errors_and_unclosed_recreation, 0, 3);
    tcase_add_loop_test(core, test_near_full_warning_and_full_channel_release, 0, 3);
    tcase_add_loop_test(core, test_put_write_failure_retries_without_losing_data, 0, 3);
    tcase_add_loop_test(core, test_directory_growth_storage_failure_and_recovery, 0, 9);
    tcase_add_loop_test(core, test_directory_full_releases_failed_open_safely, 0, 3);
    tcase_add_loop_test(core, test_append_lookup_yields_and_failed_read_preserves_media, 0, 3);
    tcase_add_loop_test(core, test_read_only_create_and_append_leave_media_unchanged, 0, 3);
    tcase_add_loop_test(core, test_save_override_last_program_and_status_retention, 0, 3);
    tcase_add_loop_test(core, test_directory_type_filters_patterns_and_raw_chain, 0, 3);
    tcase_add_loop_test(core, test_filtered_directory_rel_type_stays_a_directory, 0, 3);
    tcase_add_loop_test(core, test_open_native_wildcard_boundaries, 0, 2 * count_of(native_open_wildcard_cases));
    tcase_add_loop_test(core, test_dual_drive_writers_and_directory_selection, 0, 3);
    tcase_add_loop_test(core, test_allocation_bam_write_failure_preserves_peer_and_retries, 1, 3);
    tcase_add_loop_test(core, test_bam_count_mismatch_is_not_persisted_as_success, 0, 3);
    tcase_add_loop_test(core, test_user_zero_retains_native_resources_and_stale_selector, 0, 3);
    tcase_add_loop_test(core, test_user_reset_abandons_writers_but_retains_backing_ownership, 0, 21);
    tcase_add_loop_test(core, test_initialize_abandons_only_selected_disk_and_reloads_metadata, 0, 3);
    tcase_add_loop_test(core, test_initialize_native_drive_selection, 0, 27);
    tcase_add_loop_test(core, test_initialize_storage_failures_and_absent_slot_do_not_redirect, 0, 6);
    tcase_add_loop_test(core, test_initialize_rejects_replaced_media_before_io, 0, 3);
    tcase_add_loop_test(core, test_command_termination_unknown_and_execution_rejection, 0, 3);
    tcase_add_loop_test(core, test_rename_changes_only_directory_name_and_preserves_reader, 0, 3);
    tcase_add_loop_test(core, test_rename_native_parser_and_error_precedence, 0, 60);
    tcase_add_loop_test(core, test_rename_cross_drive_collision_and_source_selection, 0, 6);
    tcase_add_loop_test(core, test_rename_storage_failures_release_only_internal_ownership, 0, 15);
    tcase_add_loop_test(core, test_rename_yields_and_preserves_later_directory_edits, 0, 3);
    tcase_add_loop_test(core, test_rename_rejects_stale_media_at_lookup_and_commit, 0, 6);
    tcase_add_loop_test(core, test_rename_unclosed_writer_survives_close_without_restoring_old_name, 0, 3);
    tcase_add_loop_test(core, test_rename_default_drive_and_absent_source, 0, 6);
    tcase_add_loop_test(core, test_rename_shared_resource_exhaustion_and_recovery, 0, 3);
    suite_add_tcase(suite, core);
    return suite;
}

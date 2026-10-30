// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "controller4040.h"

#include <string.h>

#include "diskimage.h"

enum {
    JOB_READ = 0x80, JOB_WRITE = 0x90, JOB_VERIFY = 0xa0,
    JOB_SEEK = 0xb0, JOB_BUMP = 0xc0, JOB_JUMP = 0xd0, JOB_EXECUTE = 0xe0,
    RESULT_OK = 1, RESULT_HEADER = 2, RESULT_SYNC = 3,
    RESULT_DATA = 5, RESULT_VERIFY = 7, RESULT_PROTECTED = 8,
    RESULT_ID = 11, RESULT_DRIVE = 13,
    HEADER_TRACK = 2, HEADER_SECTOR = 3, HEADER_CHECKSUM = 4,
};

// Native FED3 publishes the XOR of the four physical sector-header bytes.
static void header_checksum(uint8_t header[8]) {
    header[HEADER_CHECKSUM] = header[0] ^ header[1] ^ header[HEADER_TRACK] ^ header[HEADER_SECTOR];
}

bool controller4040_offset(unsigned track, unsigned sector, uint32_t* offset) {
    return diskimage_sector_offset(diskimage_type_d64, track, sector, offset);
}

// Replace only the stock formatter payload. Unknown controller code fails loudly.
static uint8_t format(controller4040_t* c, controller4040_disk_t* disk, uint8_t header[8]) {
    rom4040_t* const m = c->machine;
    if (memcmp(m->shared + 0x100, m->rom, 0x200) != 0) {
        c->unsupported_code = true;
        m->fault = ROM4040_FAULT_CONTROLLER_CODE;
        return RESULT_DRIVE;
    }
    header[HEADER_TRACK] = 1;
    header[HEADER_SECTOR] = 0;
    header_checksum(header);
    if (!disk->writable) return RESULT_PROTECTED;
    uint8_t zero[ROM4040_SECTOR_SIZE] = {0};
    for (uint32_t offset = 0; offset < CONTROLLER4040_IMAGE_SIZE; offset += sizeof(zero))
        if (!disk->io(disk->context, offset, zero, true)) return RESULT_DATA;
    memcpy(disk->id, header, sizeof(disk->id));
    header[HEADER_TRACK] = CONTROLLER4040_TRACKS;
    header_checksum(header);
    return RESULT_OK;
}

uint8_t controller4040_job(void* context, uint8_t command,
    uint8_t header[8], uint8_t sector[ROM4040_SECTOR_SIZE]) {
    controller4040_t* const c = context;
    controller4040_disk_t* const disk = &c->disk[command & 1];
    const uint8_t job = command & 0xfe;
    if (job == JOB_JUMP) {
        c->unsupported_code = true;
        c->machine->fault = ROM4040_FAULT_CONTROLLER_CODE;
        return RESULT_DRIVE;
    }
    if (job == JOB_BUMP) return RESULT_OK;
    if (disk->io == NULL) return RESULT_SYNC;
    if (job == JOB_SEEK) {
        memcpy(header, disk->id, sizeof(disk->id));
        // Native SEEK returns an encountered physical header. Pick a valid
        // representative without adding rotational scheduling.
        if (header[HEADER_TRACK] == 0) header[HEADER_TRACK] = 1;
        header[HEADER_SECTOR] = 1;
        header_checksum(header);
        return RESULT_OK;
    }
    if (job == JOB_EXECUTE) return format(c, disk, header);
    uint32_t offset;
    // A head moved beyond the D64's last recorded track finds no sync marks.
    if (header[HEADER_TRACK] > CONTROLLER4040_TRACKS) return RESULT_SYNC;
    if (memcmp(header, disk->id, sizeof(disk->id)) != 0) return RESULT_ID;
    if (job == JOB_READ || job == JOB_WRITE || job == JOB_VERIFY) {
        if (job == JOB_WRITE && !disk->writable) return RESULT_PROTECTED;
        header_checksum(header);
        if (!controller4040_offset(header[HEADER_TRACK], header[HEADER_SECTOR], &offset))
            return RESULT_HEADER;
    }
    switch (job) {
        case JOB_READ:
            return disk->io(disk->context, offset, sector, false) ? RESULT_OK : RESULT_DATA;
        case JOB_WRITE:
            return disk->io(disk->context, offset, sector, true) ? RESULT_OK : RESULT_DATA;
        case JOB_VERIFY: {
            uint8_t expected[ROM4040_SECTOR_SIZE];
            if (!disk->io(disk->context, offset, expected, false)) return RESULT_DATA;
            return memcmp(expected, sector, sizeof(expected)) == 0 ? RESULT_OK : RESULT_VERIFY;
        }
        default:
            c->unsupported_code = true;
            c->machine->fault = ROM4040_FAULT_CONTROLLER_CODE;
            return RESULT_DRIVE;
    }
}

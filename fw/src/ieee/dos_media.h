// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ieee/diskimage.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DOS_MEDIA_UNIT_COUNT 4u
#define DOS_MEDIA_SLOTS_PER_UNIT 2u

typedef enum {
    DOS_MODEL_NONE,
    DOS_MODEL_4040,
    DOS_MODEL_8050,
    DOS_MODEL_8250,
} dos_model_t;

// These are host-storage results, not native DOS status codes.
typedef enum {
    DOS_MEDIA_OK,
    DOS_MEDIA_INVALID,
    DOS_MEDIA_UNSUPPORTED,
    DOS_MEDIA_MIXED_PAIR,
    DOS_MEDIA_ABSENT,
    DOS_MEDIA_STALE,
    DOS_MEDIA_RANGE,
    DOS_MEDIA_READ_ONLY,
    DOS_MEDIA_IO_ERROR,
    DOS_MEDIA_ID_EXHAUSTED,
} dos_media_result_t;

// Completes buffered writes using the diskimage_t context, without closing it.
typedef bool (*dos_media_flush_fn)(void* context);

// Handles are borrowed. The facade retains them on failure and closes retired
// handles only after a successful binding change. Zero denotes an empty slot.
typedef struct {
    diskimage_t image;
    dos_media_flush_fn flush;
} dos_media_io_t;

typedef struct {
    dos_media_io_t io;
    uint32_t identity;
    bool dirty;
} dos_media_slot_t;

typedef struct {
    dos_media_slot_t slots[DOS_MEDIA_SLOTS_PER_UNIT];
    dos_model_t model;
} dos_media_unit_t;

// Zero-initialize once per bank lifetime. Do not zero a live bank to reset DOS.
// The generation survives unmounts, so old jobs cannot target remounted media.
typedef struct {
    dos_media_unit_t units[DOS_MEDIA_UNIT_COUNT];
    uint32_t generation;
} dos_media_bank_t;

typedef struct {
    uint32_t identity;
    uint8_t unit;
    uint8_t slot;
} dos_media_ref_t;

// Checks exact raw sizes, writable flush support, and the complete pair.
// Does no I/O and leaves model unchanged on failure.
dos_media_result_t dos_media_validate_pair(const dos_media_io_t pair[DOS_MEDIA_SLOTS_PER_UNIT], dos_model_t* model);

// Publishes a complete pair only after validation and flushing the old pair.
// The facade must first quiesce the unit and finalize its pending DOS work.
// A successful change invalidates both slots' old references, not other units.
dos_media_result_t dos_media_set_unit(dos_media_bank_t* bank, unsigned int unit,
                                      const dos_media_io_t pair[DOS_MEDIA_SLOTS_PER_UNIT]);

// Applies the same unit-local lifecycle when mounting or removing one slot.
dos_media_result_t dos_media_set_slot(dos_media_bank_t* bank, unsigned int unit, unsigned int slot,
                                      const dos_media_io_t* media);

// Issues a unit-and-slot-bound reference, leaving output unchanged on failure.
dos_media_result_t dos_media_reference(const dos_media_bank_t* bank, unsigned int unit, unsigned int slot,
                                       dos_media_ref_t* reference);

// Reads one validated sector. A failed callback may partially modify output.
dos_media_result_t dos_media_read(dos_media_bank_t* bank, dos_media_ref_t reference, unsigned int track,
                                  unsigned int sector, uint8_t output[DISKIMAGE_SECTOR_SIZE]);

// Reads one native BAM sector on demand (one for 4040, two for 8050, four
// for 8250). This is raw access, not allocation, validation, or BAM repair.
// BAM track 18 (4040) or 38 (8050/8250) is not a generic directory location.
dos_media_result_t dos_media_read_bam(dos_media_bank_t* bank, dos_media_ref_t reference, unsigned int part,
                                      uint8_t output[DISKIMAGE_SECTOR_SIZE]);

// Marks the media dirty before attempting a write, including partial failures.
// Success means written to the backing callback, not yet flushed or DOS-acked.
dos_media_result_t dos_media_write(dos_media_bank_t* bank, dos_media_ref_t reference, unsigned int track,
                                   unsigned int sector, const uint8_t input[DISKIMAGE_SECTOR_SIZE]);

// Flushes dirty media without discarding dirty state on failure.
dos_media_result_t dos_media_flush_unit(dos_media_bank_t* bank, unsigned int unit);

// Flushes every unit before detaching any. The facade must first finalize DOS
// work for every unit. Failure retains every binding and any unflushed state.
dos_media_result_t dos_media_unmount_all(dos_media_bank_t* bank);

#ifdef __cplusplus
}
#endif

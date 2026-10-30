// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Maximum native formatted status text (excludes the transmitted CR).
#define IEEE_STATUS_TEXT_MAX 36u
#define IEEE_STATUS_LINE_CAPACITY (IEEE_STATUS_TEXT_MAX + 2u)

// Experimental device-8 4040, two 35-track D64 drives. DOS is the original ROM.
typedef struct {
    uint64_t calls, cycles, service_us, elapsed_us, max_service_us, max_service_gap_us;
    uint32_t presented, consumed;
} ieee_drive_metrics_t;

// Disable the virtual bus and detach all images.
void ieee_drive_init(void);

// Cold-start the ROM and flush the fabric, retaining mounted images.
void ieee_drive_reset(void);

// Close all images and restore the physical IEEE bus.
void ieee_drive_unmount_all(void);

// Mount a D64 in device 8, drive 0 or 1. Failure is logged and preserves media.
bool ieee_drive_mount(unsigned slot, const char* filename);

// Cooperatively service the ROM/FIFOs on core 0, never core 1 (DVI).
void ieee_drive_task(void);

// Read scheduling measurements for debugger/host throughput investigation.
ieee_drive_metrics_t ieee_drive_metrics(void);

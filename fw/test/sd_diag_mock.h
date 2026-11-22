// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct blockdevice blockdevice_t;
typedef struct filesystem filesystem_t;

/** Bind the fake root mount to the fixture's heap-backed FAT card. */
void sd_diag_mock_bind(blockdevice_t* device, filesystem_t* filesystem);

/** Observe the workspace allocation made by the next diagnostic preparation. */
void sd_diag_mock_capture_workspace(void);

/** Release resources abandoned by a terminal diagnostic failure in a forked test. */
void sd_diag_mock_cleanup(void);

/** Count live fake descriptors, streams, and directory handles. */
size_t sd_diag_mock_open_handles(void);

/** Count completed root reformats for cycle-ordering checks. */
size_t sd_diag_mock_reformats(void);

/** Count successful root mounts, including the clean persistence remount. */
size_t sd_diag_mock_mounts(void);

/** Report the maximum observed live descriptor, stream, and directory population. */
size_t sd_diag_mock_peak_handles(void);

/** Corrupt one image read immediately or only after the clean persistence remount. */
void sd_diag_mock_corrupt_image_read(bool after_remount);

/** Make descriptor stat report an incorrect size. */
void sd_diag_mock_break_stat(void);

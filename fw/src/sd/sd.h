// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// SD-card path capacity in bytes, including the terminating null. Match the
// pinned pico-vfs's 256-byte buffers: 255 single-byte path characters plus the
// null. Directory prefixes count toward this limit. Keep it separate from the
// host OS's PATH_MAX.
#define SD_PATH_MAX 256

typedef enum {
    SD_DIR_NONE,  // No prefix for an already complete path.
    SD_DIR_ROOT,
    SD_DIR_DISKS,
    SD_DIR_ROMS,
    SD_DIR_PRGS,
    SD_DIR_UKM,
    SD_DIR_FPGA,
    SD_DIR_COUNT,
} sd_dir_t;

// Returns the directory's absolute prefix, including the trailing slash.
// NONE returns an empty prefix. An invalid directory is fatal.
const char* sd_dir_prefix(sd_dir_t directory);

// Builds a prefixed path in an SD_PATH_MAX-byte buffer. A formatting error or
// overlong full path is fatal, never silently truncated.
void sd_make_path(char path[SD_PATH_MAX], sd_dir_t directory, const char* name);

bool sd_init();

FILE* sd_open(const char* path, const char* mode);

size_t sd_read(const char* filename, FILE* file, uint8_t* dest, size_t size);

typedef void (*sd_read_callback_t)(size_t offset, uint8_t* buffer, size_t bytes_read, void* context);
void sd_read_file(const char* path, sd_read_callback_t callback, void* context, size_t max_bytes);

// Return the free space on the SD card in bytes. Returns 0 on error.
uint64_t sd_free_bytes(void);

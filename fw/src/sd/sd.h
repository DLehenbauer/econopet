// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

bool sd_init();

FILE* sd_open(const char* path, const char* mode);

size_t sd_read(const char* filename, FILE* file, uint8_t* dest, size_t size);

typedef void (*sd_read_callback_t)(size_t offset, uint8_t* buffer, size_t bytes_read, void* context);
void sd_read_file(const char* path, sd_read_callback_t callback, void* context, size_t max_bytes);

/**
 * PET Clone - Open hardware implementation of the Commodore PET
 * by Daniel Lehenbauer and contributors.
 * 
 * https://github.com/DLehenbauer/commodore-pet-clone
 *
 * To the extent possible under law, I, Daniel Lehenbauer, have waived all
 * copyright and related or neighboring rights to this project. This work is
 * published from the United States.
 *
 * @copyright CC0 http://creativecommons.org/publicdomain/zero/1.0/
 * @author Daniel Lehenbauer <DLehenbauer@users.noreply.github.com> and contributors
 */

#pragma once

#include "../pch.h"

bool sd_init();

FILE* sd_open(const char* path, const char* mode);

typedef void (*sd_read_callback_t)(uint8_t* buffer, size_t bytes_read, void* context);
void sd_read(const char* path, FILE* file, sd_read_callback_t callback, void* context);

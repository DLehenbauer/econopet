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

typedef enum {
    VIDEO_MODE_40_COLUMNS,
    VIDEO_MODE_80_COLUMNS
} VideoMode;

#define VIDEO_CHAR_BUFFER_BYTE_SIZE 2000

extern VideoMode video_mode;
extern uint8_t video_char_buffer[VIDEO_CHAR_BUFFER_BYTE_SIZE];

void video_init();
static inline bool __not_in_flash_func(video_is_80_col)();

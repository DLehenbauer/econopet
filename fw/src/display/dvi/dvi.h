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

#include <stdbool.h>
#include <stdint.h>

#define VIDEO_CHAR_BUFFER_BYTE_SIZE 0x1000  // 4KB video RAM

extern uint8_t video_char_buffer[VIDEO_CHAR_BUFFER_BYTE_SIZE];
extern bool video_is_80_col;
extern bool video_graphics;

#define CRTC_REG_COUNT 14
extern uint8_t pet_crtc_registers[CRTC_REG_COUNT];

void video_init();

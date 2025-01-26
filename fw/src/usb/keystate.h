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

typedef enum _KeyStateFlags {
    KEYSTATE_PRESSED_FLAG = (1 << 0),
    KEYSTATE_SHIFTED_FLAG = (1 << 1),
} KeyStateFlags;

KeyStateFlags keystate_reset(uint8_t keycode);
void keystate_set(uint8_t keycode, KeyStateFlags flags);

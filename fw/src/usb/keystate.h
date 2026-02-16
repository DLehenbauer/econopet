// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once
typedef enum _KeyStateFlags {
    KEYSTATE_PRESSED_FLAG = (1 << 0),
    KEYSTATE_SHIFTED_FLAG = (1 << 1),
} KeyStateFlags;

KeyStateFlags keystate_reset(uint8_t keycode);
void keystate_set(uint8_t keycode, KeyStateFlags flags);

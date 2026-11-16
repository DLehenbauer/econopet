// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "keymap.h"

static const uint8_t __in_flash(".usb_keymap") usb_keymap[] = {
    #include "usb/us_keymap.h"
};

static_assert(sizeof(usb_keymap) == sizeof(((system_state_t*) 0)->usb_keymap_data),
              "embedded USB keymap size does not match system state");

void usb_keymap_reset(system_state_t* const state) {
    memcpy(state->usb_keymap_data, usb_keymap, sizeof(state->usb_keymap_data));
}

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

typedef enum {
    model_flag_none         = 0,
    model_flag_crtc         = 1 << 0,   // 0 = non-CRTC (9"/15kHz display), 1 = CRTC (12"/20kHz display)
    model_flag_business     = 1 << 1,   // 0 = graphics keyboard, 1 = business keyboard
    model_flag_80_cols      = 1 << 2,   // 0 = 40 columns, 1 = 80 columns
} model_flags_t;

typedef struct model_s {
    model_flags_t flags;
} model_t;

typedef enum usb_keymap_kind_e {
    usb_keymap_kind_sym = 0,
    usb_keymap_kind_pos = 1,
} usb_keymap_kind_t;

typedef struct __attribute__((packed)) usb_keymap_entry_s {
    // First byte contains row/col packed as nibbles
    unsigned int row: 4;        // Rows   : 0-7 (F = undefined key mapping)
    unsigned int col: 4;        // Column : 0-9

    // Second byte contains flags
    unsigned int reserved: 6;
    unsigned int unshift: 1;    // 0 = Normal, 1 = Implicitly remove shift
    unsigned int shift: 1;      // 0 = Normal, 1 = Implicitly add shift
} usb_keymap_entry_t;

typedef struct configuration_s {
    usb_keymap_entry_t usb_keymap[512][2];
} configuration_t;

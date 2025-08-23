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

typedef enum pet_keyboard_model_e {
    pet_keyboard_model_graphics = 0,
    pet_keyboard_model_business = 1,
} pet_keyboard_model_t;

typedef enum pet_video_type_e {
    pet_video_type_fixed = 0,   // non-CRTC (9"/15kHz display)
    pet_video_type_crtc = 1,    // CRTC (12"/20kHz display)
} pet_video_type_t;

typedef enum pet_display_columns_e {
    pet_display_columns_40 = 40,
    pet_display_columns_80 = 80,
} pet_display_columns_t;

typedef enum usb_keymap_kind_e {
    usb_keymap_kind_symbolic   = 0,
    usb_keymap_kind_positional = 1,
} usb_keymap_kind_t;

typedef struct __attribute__((packed)) usb_keymap_entry_s {
    // First byte contains PET keyboard matrix row/col packed as nibbles
    unsigned int row: 4;        // Rows   : 0-7 (F = undefined key mapping)
    unsigned int col: 4;        // Column : 0-9

    // Second byte contains flags
    unsigned int reserved: 6;   // (Six reserved bits for future use)
    unsigned int unshift: 1;    // 0 = Normal, 1 = Implicitly remove shift
    unsigned int shift: 1;      // 0 = Normal, 1 = Implicitly add shift
} usb_keymap_entry_t;

typedef struct system_state_s {
    // The currently active usb_keymap_kind.
    usb_keymap_kind_t usb_keymap_kind;

    // The first indexer is graphics vs. business.
    // The second indexer is symbolic vs. positional.
    // The third indexer maps USB HID codes to usb_keymap_entry_t.
    usb_keymap_entry_t usb_keymap_data[2][512];

    // Reflects the state of the current keyboard model (graphics or business)
    // as determined by the config DIP switch on the board.
    pet_keyboard_model_t pet_keyboard_model;

    // Reflects the PET video type (non-CRTC or CRTC) as determined by the config DIP
    // switch on the board.
    pet_video_type_t pet_video_type;

    // Reflects the number of displayed columns (40 or 80).  This is configured by
    // the firmware and and sent to the FPGA via SPI.
    pet_display_columns_t pet_display_columns;
} system_state_t;

extern system_state_t system_state;

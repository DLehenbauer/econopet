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
#include <stddef.h>
#include <stdint.h>

// In the 8296, the CRTC can address 8KB of video RAM from $8000-$9FFF.  This is the
// upper bound on the amount of display RAM we may need to synchronize between PET
// video, DVI output, and the terminal.
#define PET_MAX_VIDEO_RAM_BYTES 0x2000

// Number of CRTC (6545) registers synchronized from the FPGA
#define CRTC_REG_COUNT 14

// ---------------------------------------------------------------------------
// CRTC (6545) register indices
// http://archive.6502.org/datasheets/rockwell_r6545-1_crtc.pdf
// ---------------------------------------------------------------------------

#define CRTC_R0_H_TOTAL             0   // [7:0] Total displayed and non-displayed characters, minus one, per horizontal line.
#define CRTC_R1_H_DISPLAYED         1   // [7:0] Number of displayed characters per horizontal line.
#define CRTC_R2_H_SYNC_POS          2   // [7:0] Position of the HSYNC on the horizontal line.
#define CRTC_R3_SYNC_WIDTH          3   // [3:0] Width of HSYNC, [7:4] Width of VSYNC
#define CRTC_R4_V_TOTAL             4   // [6:0] Total number of character rows in a frame, minus one.
#define CRTC_R5_V_ADJUST            5   // [4:0] Number of additional scan lines to complete a frame.
#define CRTC_R6_V_DISPLAYED         6   // [6:0] Number of displayed character rows in each frame.
#define CRTC_R7_V_SYNC_POS          7   // [6:0] Character row at which VSYNC pulse occurs.
#define CRTC_R8_MODE_CONTROL        8   // [7:0] Operating mode (not implemented)
#define CRTC_R9_MAX_SCAN_LINE       9   // [4:0] Number of scan lines per character row, minus one.
#define CRTC_R10_CURSOR_START_LINE  10  // [6:0] Cursor blink mode and starting scan line (not implemented)
#define CRTC_R11_CURSOR_END_LINE    11  // [4:0] Ending scan line of cursor (not implemented)
#define CRTC_R12_START_ADDR_HI      12  // [5:0] High 6 bits of display start address.
#define CRTC_R13_START_ADDR_LO      13  // [7:0] Low 8 bits of display start address.

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

typedef enum video_source_e {
    video_source_pet,       // HDMI shows what 6502 writes to $8000
    video_source_firmware,  // HDMI shows firmware-controlled buffer
} video_source_t;

typedef enum term_mode_e {
    term_mode_cli,          // Terminal shows CLI prompt, accepts commands
    term_mode_log,          // Terminal shows log messages (legacy, for echo)
    term_mode_video,        // Terminal mirrors video buffer
} term_mode_t;

typedef enum term_input_dest_e {
    term_input_ignore,      // Discard terminal input
    term_input_to_pet,      // Inject as PET keystrokes
    term_input_to_firmware, // Route to firmware (menu, etc.)
} term_input_dest_t;

typedef struct __attribute__((packed)) usb_keymap_entry_s {
    // First byte contains PET keyboard matrix row/col packed as nibbles
    unsigned int row: 4;        // Rows   : 0-7 (F = undefined key mapping)
    unsigned int col: 4;        // Column : 0-9

    // Second byte contains flags
    unsigned int reserved: 6;   // (Six reserved bits for future use)
    unsigned int unshift: 1;    // 0 = Normal, 1 = Implicitly remove shift
    unsigned int shift: 1;      // 0 = Normal, 1 = Implicitly add shift
} usb_keymap_entry_t;

typedef enum usb_keymap_kind_e {
    usb_keymap_kind_symbolic   = 0,
    usb_keymap_kind_positional = 1,
} usb_keymap_kind_t;

typedef struct system_state_s {
    // First indexer:  (0 = graphics, 1 = business)
    // Second indexer: (0 = symbolic, 1 = positional)
    // The third indexer maps USB HID codes to usb_keymap_entry_t.
    usb_keymap_entry_t usb_keymap_data[2][2][512];

    // Reflects the state of the current keyboard model (graphics or business)
    // as determined by the config DIP switch on the board.
    pet_keyboard_model_t pet_keyboard_model;

    // Reflects the PET video type (non-CRTC or CRTC) as determined by the config DIP
    // switch on the board.
    pet_video_type_t pet_video_type;

    // Reflects the number of displayed columns (40 or 80).  This is configured by
    // the firmware and and sent to the FPGA via SPI.
    pet_display_columns_t pet_display_columns;

    // Video RAM mask (0-3).  Controls which address bits are used for video RAM:
    //  00 = 1KB at $8000 (40 column monochrome)
    //  01 = 2KB at $8000 (80 column monochrome)
    //  10 = 1KB at $8000 + 1KB at $8800 (40 column color)
    //  11 = 4KB at $8000 (80 column color)
    // This is configured by the firmware and sent to the FPGA via SPI.
    uint8_t video_ram_mask;

    // Precomputed size of video RAM in bytes. Updated whenever video_ram_mask changes.
    size_t video_ram_bytes;

    // I/O routing configuration
    video_source_t video_source;        // Where HDMI gets its video data
    term_mode_t term_mode;              // What terminal output shows
    term_input_dest_t term_input_dest;  // Where terminal input goes

    // Video character buffer (shared between PET, DVI output, and terminal)
    uint8_t video_char_buffer[PET_MAX_VIDEO_RAM_BYTES];

    // Video graphics mode flag (true = graphics/lowercase charset, false = text/uppercase)
    bool video_graphics;

    // CRTC (6545) registers read from the FPGA, controlling video timing
    uint8_t pet_crtc_registers[CRTC_REG_COUNT];
} system_state_t;

extern system_state_t system_state;

// Setter to keep derived fields in sync.
void system_state_set_video_ram_mask(system_state_t* state, uint8_t video_ram_mask);

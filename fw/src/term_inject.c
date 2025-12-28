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

#include "pch.h"
#include "term_inject.h"
#include "input.h"
#include "system_state.h"
#include "usb/keyboard.h"
#include "diag/log/log.h"
#include "class/hid/hid.h"
#include "tusb.h"  // For KEYBOARD_MODIFIER_LEFTSHIFT

// HID key entry: keycode + shift modifier state
typedef struct {
    uint8_t hid_keycode;
    bool shifted;
} hid_key_t;

// ASCII to HID keycode mapping table (indices 0-127)
// Entry of {0, false} means no mapping available
static const hid_key_t ascii_to_hid[128] = {
    // 0x00-0x0F: Control characters
    [0x00] = {0, false},                    // NUL
    [0x01] = {0, false},                    // SOH
    [0x02] = {0, false},                    // STX
    [0x03] = {0, false},                    // ETX (Ctrl+C - handled separately)
    [0x04] = {0, false},                    // EOT
    [0x05] = {0, false},                    // ENQ
    [0x06] = {0, false},                    // ACK
    [0x07] = {0, false},                    // BEL
    [0x08] = {HID_KEY_BACKSPACE, false},    // BS (Backspace)
    [0x09] = {HID_KEY_TAB, false},          // HT (Tab)
    [0x0A] = {HID_KEY_ENTER, false},        // LF (Enter)
    [0x0B] = {0, false},                    // VT
    [0x0C] = {0, false},                    // FF
    [0x0D] = {HID_KEY_ENTER, false},        // CR (Enter)
    [0x0E] = {0, false},                    // SO
    [0x0F] = {0, false},                    // SI
    
    // 0x10-0x1F: More control characters
    [0x10] = {0, false},                    // DLE
    [0x11] = {0, false},                    // DC1
    [0x12] = {0, false},                    // DC2
    [0x13] = {0, false},                    // DC3
    [0x14] = {0, false},                    // DC4
    [0x15] = {0, false},                    // NAK
    [0x16] = {0, false},                    // SYN
    [0x17] = {0, false},                    // ETB
    [0x18] = {0, false},                    // CAN
    [0x19] = {0, false},                    // EM
    [0x1A] = {0, false},                    // SUB
    [0x1B] = {HID_KEY_ESCAPE, false},       // ESC
    [0x1C] = {0, false},                    // FS
    [0x1D] = {0, false},                    // GS
    [0x1E] = {0, false},                    // RS
    [0x1F] = {0, false},                    // US
    
    // 0x20-0x2F: Punctuation and digits
    [' ']  = {HID_KEY_SPACE, false},
    ['!']  = {HID_KEY_1, true},
    ['"']  = {HID_KEY_APOSTROPHE, true},
    ['#']  = {HID_KEY_3, true},
    ['$']  = {HID_KEY_4, true},
    ['%']  = {HID_KEY_5, true},
    ['&']  = {HID_KEY_7, true},
    ['\''] = {HID_KEY_APOSTROPHE, false},
    ['(']  = {HID_KEY_9, true},
    [')']  = {HID_KEY_0, true},
    ['*']  = {HID_KEY_8, true},
    ['+']  = {HID_KEY_EQUAL, true},
    [',']  = {HID_KEY_COMMA, false},
    ['-']  = {HID_KEY_MINUS, false},
    ['.']  = {HID_KEY_PERIOD, false},
    ['/']  = {HID_KEY_SLASH, false},
    
    // 0x30-0x39: Digits
    ['0']  = {HID_KEY_0, false},
    ['1']  = {HID_KEY_1, false},
    ['2']  = {HID_KEY_2, false},
    ['3']  = {HID_KEY_3, false},
    ['4']  = {HID_KEY_4, false},
    ['5']  = {HID_KEY_5, false},
    ['6']  = {HID_KEY_6, false},
    ['7']  = {HID_KEY_7, false},
    ['8']  = {HID_KEY_8, false},
    ['9']  = {HID_KEY_9, false},
    
    // 0x3A-0x40: More punctuation
    [':']  = {HID_KEY_SEMICOLON, true},
    [';']  = {HID_KEY_SEMICOLON, false},
    ['<']  = {HID_KEY_COMMA, true},
    ['=']  = {HID_KEY_EQUAL, false},
    ['>']  = {HID_KEY_PERIOD, true},
    ['?']  = {HID_KEY_SLASH, true},
    ['@']  = {HID_KEY_2, true},
    
    // 0x41-0x5A: Uppercase letters (shifted)
    ['A']  = {HID_KEY_A, true},
    ['B']  = {HID_KEY_B, true},
    ['C']  = {HID_KEY_C, true},
    ['D']  = {HID_KEY_D, true},
    ['E']  = {HID_KEY_E, true},
    ['F']  = {HID_KEY_F, true},
    ['G']  = {HID_KEY_G, true},
    ['H']  = {HID_KEY_H, true},
    ['I']  = {HID_KEY_I, true},
    ['J']  = {HID_KEY_J, true},
    ['K']  = {HID_KEY_K, true},
    ['L']  = {HID_KEY_L, true},
    ['M']  = {HID_KEY_M, true},
    ['N']  = {HID_KEY_N, true},
    ['O']  = {HID_KEY_O, true},
    ['P']  = {HID_KEY_P, true},
    ['Q']  = {HID_KEY_Q, true},
    ['R']  = {HID_KEY_R, true},
    ['S']  = {HID_KEY_S, true},
    ['T']  = {HID_KEY_T, true},
    ['U']  = {HID_KEY_U, true},
    ['V']  = {HID_KEY_V, true},
    ['W']  = {HID_KEY_W, true},
    ['X']  = {HID_KEY_X, true},
    ['Y']  = {HID_KEY_Y, true},
    ['Z']  = {HID_KEY_Z, true},
    
    // 0x5B-0x60: Brackets and special
    ['[']  = {HID_KEY_BRACKET_LEFT, false},
    ['\\'] = {HID_KEY_BACKSLASH, false},
    [']']  = {HID_KEY_BRACKET_RIGHT, false},
    ['^']  = {HID_KEY_6, true},
    ['_']  = {HID_KEY_MINUS, true},
    ['`']  = {HID_KEY_GRAVE, false},
    
    // 0x61-0x7A: Lowercase letters (unshifted)
    ['a']  = {HID_KEY_A, false},
    ['b']  = {HID_KEY_B, false},
    ['c']  = {HID_KEY_C, false},
    ['d']  = {HID_KEY_D, false},
    ['e']  = {HID_KEY_E, false},
    ['f']  = {HID_KEY_F, false},
    ['g']  = {HID_KEY_G, false},
    ['h']  = {HID_KEY_H, false},
    ['i']  = {HID_KEY_I, false},
    ['j']  = {HID_KEY_J, false},
    ['k']  = {HID_KEY_K, false},
    ['l']  = {HID_KEY_L, false},
    ['m']  = {HID_KEY_M, false},
    ['n']  = {HID_KEY_N, false},
    ['o']  = {HID_KEY_O, false},
    ['p']  = {HID_KEY_P, false},
    ['q']  = {HID_KEY_Q, false},
    ['r']  = {HID_KEY_R, false},
    ['s']  = {HID_KEY_S, false},
    ['t']  = {HID_KEY_T, false},
    ['u']  = {HID_KEY_U, false},
    ['v']  = {HID_KEY_V, false},
    ['w']  = {HID_KEY_W, false},
    ['x']  = {HID_KEY_X, false},
    ['y']  = {HID_KEY_Y, false},
    ['z']  = {HID_KEY_Z, false},
    
    // 0x7B-0x7F: More special chars
    ['{']  = {HID_KEY_BRACKET_LEFT, true},
    ['|']  = {HID_KEY_BACKSLASH, true},
    ['}']  = {HID_KEY_BRACKET_RIGHT, true},
    ['~']  = {HID_KEY_GRAVE, true},
    [0x7F] = {HID_KEY_DELETE, false},       // DEL
};

// HID keycodes for left shift (used for shifted characters)
#define HID_KEY_SHIFT_LEFT  0xE1

// Extended keycodes start at KEY_UP (1000) and are densely packed.
// This table maps (keycode - KEY_UP) to HID keycodes.
// Entry of {0, false} means no mapping available.
#define EXTENDED_KEY_BASE  KEY_UP
#define EXTENDED_KEY_COUNT 8

static const hid_key_t extended_to_hid[EXTENDED_KEY_COUNT] = {
    /* KEY_UP    (1000) */ {HID_KEY_ARROW_UP,    false},
    /* KEY_DOWN  (1001) */ {HID_KEY_ARROW_DOWN,  false},
    /* KEY_RIGHT (1002) */ {HID_KEY_ARROW_RIGHT, false},
    /* KEY_LEFT  (1003) */ {HID_KEY_ARROW_LEFT,  false},
    /* KEY_HOME  (1004) */ {HID_KEY_HOME,        false},
    /* KEY_END   (1005) */ {HID_KEY_END,         false},
    /* KEY_PGUP  (1006) */ {HID_KEY_PAGE_UP,     false},
    /* KEY_PGDN  (1007) */ {HID_KEY_PAGE_DOWN,   false},
};

#define INJECT_QUEUE_SIZE 16
static hid_key_t inject_queue[INJECT_QUEUE_SIZE];
static uint8_t inject_queue_head = 0;
static uint8_t inject_queue_tail = 0;

// Injection state machine
typedef enum {
    INJECT_IDLE,
    INJECT_KEY_DOWN,
    INJECT_KEY_UP,
} inject_state_t;

static inject_state_t inject_state = INJECT_IDLE;
static uint8_t inject_keycode = 0;
static uint8_t inject_modifiers = 0;

bool term_inject_char(int ch) {
    // Check if queue is full
    uint8_t next_head = (inject_queue_head + 1) % INJECT_QUEUE_SIZE;
    if (next_head == inject_queue_tail) {
        return false;  // Queue full
    }
    
    // Translate character to HID keycode + shift state
    const hid_key_t* mapping;
    if (ch >= 0 && ch <= 127) {
        // ASCII character
        mapping = &ascii_to_hid[ch];
        if (mapping->hid_keycode == 0) {
            log_debug("term_inject: no mapping for ASCII char 0x%02x", ch);
            return false;
        }
    } else if (ch >= EXTENDED_KEY_BASE && ch < EXTENDED_KEY_BASE + EXTENDED_KEY_COUNT) {
        // Extended keycode (navigation key)
        mapping = &extended_to_hid[ch - EXTENDED_KEY_BASE];
        if (mapping->hid_keycode == 0) {
            log_debug("term_inject: no mapping for extended key %d", ch);
            return false;
        }
    } else {
        // Unsupported keycode
        log_debug("term_inject: unsupported keycode %d", ch);
        return false;
    }
    
    // Queue the pre-translated HID keycode and shift state
    inject_queue[inject_queue_head] = *mapping;
    inject_queue_head = next_head;
    return true;
}

static bool dequeue_entry(hid_key_t* entry) {
    if (inject_queue_head == inject_queue_tail) {
        return false;  // Queue empty
    }
    *entry = inject_queue[inject_queue_tail];
    inject_queue_tail = (inject_queue_tail + 1) % INJECT_QUEUE_SIZE;
    return true;
}

void term_inject_task(void) {
    switch (inject_state) {
        case INJECT_IDLE: {
            hid_key_t entry;
            if (!dequeue_entry(&entry)) {
                return;  // Nothing to inject
            }
            
            // Store keycode and compute modifiers for the key-up event
            inject_keycode = entry.hid_keycode;
            inject_modifiers = entry.shifted ? KEYBOARD_MODIFIER_LEFTSHIFT : 0;
            
            // Enqueue key-down event to USB keyboard queue
            usb_keyboard_enqueue_key_down(inject_keycode, inject_modifiers);
            
            inject_state = INJECT_KEY_DOWN;
            log_debug("term_inject: enqueue key down HID=0x%02x (shift=%d)", 
                     inject_keycode, entry.shifted);
            break;
        }
        
        case INJECT_KEY_DOWN:
            // Key-down has been enqueued for one cycle, now enqueue key-up
            inject_state = INJECT_KEY_UP;
            break;
            
        case INJECT_KEY_UP:
            // Enqueue key-up event to USB keyboard queue
            usb_keyboard_enqueue_key_up(inject_keycode, inject_modifiers);
            
            log_debug("term_inject: enqueue key up");
            inject_state = INJECT_IDLE;
            break;
    }
}

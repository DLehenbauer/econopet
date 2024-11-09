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

#include "keyboard.h"

#define MODIFIERS_SHIFT_MASK ((HID_KEY_CONTROL_LEFT - HID_KEY_SHIFT_LEFT) | (HID_KEY_CONTROL_LEFT - HID_KEY_SHIFT_RIGHT))


typedef struct __attribute__((packed)) {
    // First byte contains row/col packed as nibbles
    unsigned int row: 4;        // Rows   : 0-7 (F = undefined key mapping)
    unsigned int col: 4;        // Column : 0-9

    // Second byte contains flags
    unsigned int reserved: 6;
    unsigned int unshift: 1;    // 0 = Normal, 1 = Implicitly remove shift
    unsigned int shift: 1;      // 0 = Normal, 1 = Implicitly add shift
} KeyInfo;

typedef struct {
    uint16_t key;
    uint8_t modifiers;
    bool pressed;
} KeyEvent;

#define KEY_BUFFER_LOG2_CAPACITY 4
#define KEY_BUFFER_CAPACITY (1 << KEY_BUFFER_LOG2_CAPACITY)
#define KEY_BUFFER_CAPACITY_MASK (KEY_BUFFER_CAPACITY - 1)

static KeyEvent key_buffer[KEY_BUFFER_CAPACITY];
static uint8_t key_buffer_head = 0;
static uint8_t key_buffer_tail = 0;

static void enqueue_key_event(KeyEvent keyEvent) {
    uint8_t newHead = (key_buffer_head + 1) & KEY_BUFFER_CAPACITY_MASK;
    if (newHead != key_buffer_tail) {
        key_buffer[key_buffer_head] = keyEvent;
        key_buffer_head = newHead;
    }
}

static void enqueue_key_up(uint8_t keycode, uint8_t modifiers) {
    KeyEvent keyEvent = {
        .key = keycode,
        .pressed = false,
        .modifiers = modifiers,
    };
    enqueue_key_event(keyEvent);
}

static void enqueue_key_down(uint8_t keycode, uint8_t modifiers) {
    KeyEvent keyEvent = {
        .key = keycode,
        .pressed = true,
        .modifiers = modifiers,
    };
    enqueue_key_event(keyEvent);
}

static bool peek_key_event(KeyEvent* keyEvent) {
    *keyEvent = key_buffer[key_buffer_tail];
    if (keyEvent->modifiers & MODIFIERS_SHIFT_MASK) {
        keyEvent->key |= 0x0100;
    }
    return key_buffer_head != key_buffer_tail;
}

static void dequeue_key_event() {
    assert(key_buffer_head != key_buffer_tail);
    key_buffer_tail = (key_buffer_tail + 1) & KEY_BUFFER_CAPACITY_MASK;
}

// Graphic US positional keymap
static const uint8_t __in_flash(".keymap_grus_pos") keymap_grus_pos[] = {
    #include "../keymaps/grus_pos.h"
};

// Graphic US symbolic keymap
static const uint8_t __in_flash(".keymap_grus_sym") keymap_grus_sym[] = {
    #include "../keymaps/grus_sym.h"
};

static const KeyInfo* s_keymap = (KeyInfo*)((void*) keymap_grus_sym);

uint8_t key_matrix[KEY_COL_COUNT] = {
    /* 0 */ 0xff,
    /* 1 */ 0xff,
    /* 2 */ 0xff,
    /* 3 */ 0xff,
    /* 4 */ 0xff,
    /* 5 */ 0xff,
    /* 6 */ 0xff,
    /* 7 */ 0xff,
    /* 8 */ 0xff,
    /* 9 */ 0xff,
};

static bool find_key_in_report(hid_keyboard_report_t const* report, uint8_t keycode) {
    for (uint8_t i = 0; i < 6; i++) {
        if (report->keycode[i] == keycode) {
            return true;
        }
    }

    return false;
}

void key_down(uint16_t keycode) {
    KeyInfo key = s_keymap[keycode];
    uint8_t row = key.row;
    uint8_t rowMask = 1 << row;

    if (!rowMask) {
        printf("USB: Key down %d=(undefined)\n", keycode);
        return;
    }

    uint8_t col = key.col;

    if (key_matrix[col] & rowMask) {
        key_matrix[col] &= ~rowMask;
        printf("USB: Key down: %d=(%d,%d)\n", keycode, row, col);
    }
}

void key_up(uint16_t keycode) {
    KeyInfo key = s_keymap[keycode];
    uint8_t row = key.row;
    uint8_t rowMask = 1 << row;

    if (!rowMask) {
        printf("USB: Key up %d=(undefined)\n", keycode);
        return;
    }

    uint8_t col = key.col;

    if (key_matrix[col] & ~rowMask) {
        key_matrix[col] |= rowMask;
        printf("USB: Key up: %d=(%d,%d)\n", keycode, row, col);
    }
}

uint8_t get_key_modifiers(KeyEvent keyEvent) {
    uint8_t modifiers = keyEvent.modifiers;
    KeyInfo keyInfo = s_keymap[keyEvent.key];

    // If the key is pressed, apply shift/unshift modifiers.  We do not adjust
    // modifiers when the key is released since we want to restore the original
    // keyboard state before processing more keys.
    if (keyEvent.pressed) {
        if (keyInfo.shift) {
            modifiers |= MODIFIERS_SHIFT_MASK;
        }

        if (keyInfo.unshift) {
            modifiers &= ~MODIFIERS_SHIFT_MASK;
        }
    }

    return modifiers;
}

void dispatch_key_events() {
    static uint8_t previous_modifiers = 0;

    static const char* const modifiers[] = {
        "CONTROL_LEFT",
        "SHIFT_LEFT",
        "ALT_LEFT",
        "GUI_LEFT",
        "CONTROL_RIGHT",
        "SHIFT_RIGHT",
        "ALT_RIGHT",
        "GUI_RIGHT",
    };
    
    KeyEvent keyEvent;

    if (!peek_key_event(&keyEvent)) {
        return;
    }

    {
        uint8_t saved_modifiers = get_key_modifiers(keyEvent);
        uint8_t current_modifiers = saved_modifiers;

        if (current_modifiers != previous_modifiers) {
            for (uint8_t i = 0; i < 8; i++) {
                uint8_t current_modifier = current_modifiers & 0x01;
                uint8_t previous_modifier = previous_modifiers & 0x01;

                if (current_modifier) {
                    if (!previous_modifier) {
                        printf("USB: Modifier down: %s\n", modifiers[i]);
                        key_down(HID_KEY_CONTROL_LEFT + i);
                    }
                } else if (previous_modifier) {
                    printf("USB: Modifier up: %s\n", modifiers[i]);
                    key_up(HID_KEY_CONTROL_LEFT + i);
                }

                current_modifiers >>= 1;
                previous_modifiers >>= 1;
            }

            // Note that 'current_modifiers' has been cleared via >>= 1.
            previous_modifiers = saved_modifiers;
        }
    }

    do {
        if (keyEvent.pressed) {
            key_down(keyEvent.key);
        } else {
            key_up(keyEvent.key);
        }

        dequeue_key_event();
    } while (peek_key_event(&keyEvent) && get_key_modifiers(keyEvent) == previous_modifiers);
}

void process_kbd_report(hid_keyboard_report_t const* report) {
    static hid_keyboard_report_t prev_report = {0, 0, {0}};

    uint8_t modifiers = report->modifier;

    for (uint8_t i = 0; i < 6; i++) {
        uint8_t keycode = prev_report.keycode[i];
        if (keycode && !find_key_in_report(report, keycode)) {
            enqueue_key_up(keycode, modifiers);
        }
    }

    for (uint8_t i = 0; i < 6; i++) {
        uint8_t keycode = report->keycode[i];
        if (keycode && !find_key_in_report(&prev_report, keycode)) {
            enqueue_key_down(keycode, modifiers);
        }
    }

    prev_report = *report;
}

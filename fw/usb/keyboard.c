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

// https://vice-emu.sourceforge.io/vice_1.html#SEC26

#include "keyboard.h"

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

typedef struct __attribute__((packed)) {
    // First byte contains row/col packed as nibbles
    unsigned int row: 4;        // Rows   : 0-7 (F = undefined key mapping)
    unsigned int col: 4;        // Column : 0-9

    // Second byte contains flags
    unsigned int reserved: 6;
    unsigned int unshift: 1;    // 0 = Normal, 1 = Implicitly remove shift
    unsigned int shift: 1;      // 0 = Normal, 1 = Implicitly add shift
} KeyInfo;

// Graphic US positional keymap
static const uint8_t __in_flash(".keymap_grus_pos") keymap_grus_pos[] = {
    #include "../keymaps/grus_pos.h"
};

// Graphic US symbolic keymap
static const uint8_t __in_flash(".keymap_grus_sym") keymap_grus_sym[] = {
    #include "../keymaps/grus_sym.h"
};

static const KeyInfo* s_keymap = (KeyInfo*)((void*) keymap_grus_sym);

typedef struct {
    uint8_t dev_addr;       // USB device address of keyboard
    uint8_t key;            // USB HID keycode
    uint8_t row;            // PET keyboard row
    uint8_t col;            // PET keyboard column
    uint8_t modifiers;      // USB HID modifier flags (e.g. shift)
    bool pressed;           // true = key pressed, false = key released
} KeyEvent;

#define KEY_BUFFER_LOG2_CAPACITY 4
#define KEY_BUFFER_CAPACITY (1 << KEY_BUFFER_LOG2_CAPACITY)
#define KEY_BUFFER_CAPACITY_MASK (KEY_BUFFER_CAPACITY - 1)

static KeyEvent key_buffer[KEY_BUFFER_CAPACITY];
static uint8_t key_buffer_head = 0;
static uint8_t key_buffer_tail = 0;

static void enqueue_key_event(uint8_t dev_addr, uint8_t keycode, uint8_t row, uint8_t col, uint8_t modifiers, bool pressed) {
    uint8_t newHead = (key_buffer_head + 1) & KEY_BUFFER_CAPACITY_MASK;
    if (newHead != key_buffer_tail) {
        KeyEvent keyEvent = {
            .dev_addr = dev_addr,
            .key = keycode,
            .row = row,
            .col = col,
            .modifiers = modifiers,
            .pressed = pressed,
        };
        key_buffer[key_buffer_head] = keyEvent;
        key_buffer_head = newHead;
    }
}

static void enqueue_key_up(uint8_t dev_addr, uint8_t keycode, uint8_t modifiers) {
    uint8_t i = key_buffer_head;
    uint8_t end = key_buffer_tail;

    // Scan backwards through the key_buffer to find the most recent key press event for the given key.
    while (i != end) {
        KeyEvent keyEvent = key_buffer[i];
        if (key_buffer[i].key == keycode && key_buffer[i].pressed) {
            enqueue_key_event(dev_addr, keycode, keyEvent.row, keyEvent.col, modifiers, /* pressed: */ false);
            return;
        }

        i = (i - 1) & KEY_BUFFER_CAPACITY_MASK;
    }

    printf("USB: Key up %d=(not found)\n", keycode);
}

static void enqueue_key_down(uint8_t dev_addr, uint8_t keycode, uint8_t modifiers) {
    bool is_shifted = (modifiers & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) != 0;
    
    KeyInfo keyInfo = s_keymap[
        is_shifted
            ? keycode | 0x0100      // s_keymap[0x0000..0x00ff] = shifted keymap
            : keycode];             // s_keymap[0x0100..0x01ff] = unshifted keymap
            
    uint8_t row = keyInfo.row;

    if (row > 7) {
        printf("USB: Key down %d=(undefined)\n", keycode);
        return;
    }

    uint8_t col = keyInfo.col;

    if (keyInfo.shift) { 
        modifiers |= KEYBOARD_MODIFIER_LEFTSHIFT;
    }
    
    if (keyInfo.unshift) {
        modifiers &= ~(KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
    }

    enqueue_key_event(dev_addr, keycode, row, col, modifiers, /* pressed: */ true);
}

static bool peek_key_event(KeyEvent* keyEvent) {
    *keyEvent = key_buffer[key_buffer_tail];
    return key_buffer_head != key_buffer_tail;
}

static void dequeue_key_event() {
    assert(key_buffer_head != key_buffer_tail);
    key_buffer_tail = (key_buffer_tail + 1) & KEY_BUFFER_CAPACITY_MASK;
}

static bool shift_lock_enabled = false;

void key_down(uint8_t keycode, uint8_t col, uint8_t row) {
    uint8_t rowMask = 1 << row;

    if (key_matrix[col] & rowMask) {
        key_matrix[col] &= ~rowMask;
        printf("USB: Key down: %d=(%d,%d)\n", keycode, row, col);
    }
}

void modifier_down(uint8_t modifierMask) {
    uint8_t keycode = HID_KEY_CONTROL_LEFT + modifierMask;
    KeyInfo keyInfo = s_keymap[keycode];
    key_down(keycode, keyInfo.col, keyInfo.row);
}

void key_up(uint8_t keycode, uint8_t col, uint8_t row) {
    uint8_t rowMask = 1 << row;

    if (key_matrix[col] & ~rowMask) {
        key_matrix[col] |= rowMask;
        printf("USB: Key up: %d=(%d,%d)\n", keycode, row, col);
    }
}

void modifier_up(uint8_t modifierMask) {
    uint8_t keycode = HID_KEY_CONTROL_LEFT + modifierMask;
    KeyInfo keyInfo = s_keymap[keycode];
    key_up(keycode, keyInfo.col, keyInfo.row);
}

static void sync_leds(uint8_t dev_addr) {
    static uint8_t led_report = 0;

    led_report = shift_lock_enabled
        ? KEYBOARD_LED_CAPSLOCK
        : 0;

    tuh_hid_set_report(dev_addr, 0, 0, HID_REPORT_TYPE_OUTPUT, &led_report, sizeof(led_report));
}

static uint8_t get_modifiers(KeyEvent keyEvent) {
    uint8_t modifiers = keyEvent.modifiers;

    if (shift_lock_enabled) {
        modifiers |= KEYBOARD_MODIFIER_LEFTSHIFT;
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
        // Synchronize modifier keys in the PET keyboard matrix with the USB HID keyboard state.
        uint8_t active_modifiers = get_modifiers(keyEvent);
        uint8_t current_modifiers = active_modifiers;

        if (current_modifiers != previous_modifiers) {
            for (uint8_t i = 0; i < 8; i++) {
                uint8_t current_modifier = current_modifiers & 0x01;
                uint8_t previous_modifier = previous_modifiers & 0x01;

                if (current_modifier) {
                    if (!previous_modifier) {
                        printf("USB: Modifier down: %s\n", modifiers[i]);
                        modifier_down(i);
                    }
                } else if (previous_modifier) {
                    printf("USB: Modifier up: %s\n", modifiers[i]);
                    modifier_up(i);
                }

                current_modifiers >>= 1;
                previous_modifiers >>= 1;
            }

            // Note that 'current_modifiers' has been cleared via >>= 1.
            previous_modifiers = active_modifiers;
        }
    }

    while (true) {
        if (keyEvent.pressed) {
            switch (keyEvent.key) {
                case HID_KEY_CAPS_LOCK:
                    shift_lock_enabled = !shift_lock_enabled;                   
                    printf("USB: Shift lock %s\n", shift_lock_enabled ? "enabled" : "disabled");
                    sync_leds(keyEvent.dev_addr);
                    break;
                default:
                    key_down(keyEvent.key, keyEvent.row, keyEvent.col);
                    break;
            }
        } else {
            key_up(keyEvent.key, keyEvent.row, keyEvent.col);
        }

        dequeue_key_event();

        // Continue updating the PET keyboard matrix as long as there are more events queued
        // and the modifiers are consistent.
    } while (peek_key_event(&keyEvent) && (get_modifiers(keyEvent) == previous_modifiers));
}

static bool find_key_in_report(hid_keyboard_report_t const* report, uint8_t keycode) {
    for (uint8_t i = 0; i < 6; i++) {
        if (report->keycode[i] == keycode) {
            return true;
        }
    }

    return false;
}

void process_kbd_report(uint8_t dev_addr, hid_keyboard_report_t const* report) {
    static hid_keyboard_report_t prev_report = {0, 0, {0}};

    const uint8_t modifiers = report->modifier;

    for (uint8_t i = 0; i < 6; i++) {
        const uint8_t keycode = prev_report.keycode[i];
        if (keycode && !find_key_in_report(report, keycode)) {
            enqueue_key_up(dev_addr, keycode, modifiers);
        }
    }

    for (uint8_t i = 0; i < 6; i++) {
        const uint8_t keycode = report->keycode[i];
        if (keycode && !find_key_in_report(&prev_report, keycode)) {
            enqueue_key_down(dev_addr, keycode, modifiers);
        }
    }

    prev_report = *report;
}

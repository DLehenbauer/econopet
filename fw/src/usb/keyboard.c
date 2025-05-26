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
#include "keystate.h"
#include "keyscan.h"
#include "model.h"

uint8_t usb_key_matrix[KEY_COL_COUNT] = {
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

uint8_t pet_key_matrix[KEY_COL_COUNT] = {
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

static const usb_keymap_entry_t* s_keymap = configuration.usb_keymap;

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
static uint8_t key_buffer_next_write_index = 0;
static uint8_t key_buffer_next_read_index = 0;

static void enqueue_key_event(uint8_t dev_addr, uint8_t keycode, uint8_t row, uint8_t col, uint8_t modifiers, bool pressed) {
    uint8_t new_write_index = (key_buffer_next_write_index + 1) & KEY_BUFFER_CAPACITY_MASK;
    if (new_write_index != key_buffer_next_read_index) {
        KeyEvent keyEvent = {
            .dev_addr = dev_addr,
            .key = keycode,
            .row = row,
            .col = col,
            .modifiers = modifiers,
            .pressed = pressed,
        };
        key_buffer[key_buffer_next_write_index] = keyEvent;
        key_buffer_next_write_index = new_write_index;
    }
}

static bool peek_key_event(KeyEvent* keyEvent) {
    *keyEvent = key_buffer[key_buffer_next_read_index];
    return key_buffer_next_write_index != key_buffer_next_read_index;
}

static void dequeue_key_event() {
    assert(key_buffer_next_write_index != key_buffer_next_read_index);
    key_buffer_next_read_index = (key_buffer_next_read_index + 1) & KEY_BUFFER_CAPACITY_MASK;
}

static void enqueue_key_up(uint8_t dev_addr, uint8_t keycode, uint8_t modifiers) {
    KeyStateFlags keystate = keystate_reset(keycode);

    if (!(keystate & KEYSTATE_PRESSED_FLAG)) {
        printf("USB: Key up %d=(not found)\n", keycode);
        return;
    }

    usb_keymap_entry_t keyInfo = s_keymap[
        (keystate & KEYSTATE_SHIFTED_FLAG)
            ? keycode | 0x0100      // s_keymap[0x0000..0x00ff] = shifted keymap
            : keycode];             // s_keymap[0x0100..0x01ff] = unshifted keymap

    const uint8_t row = keyInfo.row;

    if (row > 7) {
        printf("USB: Key up %d=(undefined)\n", keycode);
        return;
    }

    const uint8_t col = keyInfo.col;

    enqueue_key_event(dev_addr, keycode, row, col, modifiers, /* pressed: */ false);
}

static void enqueue_key_down(uint8_t dev_addr, uint8_t keycode, uint8_t modifiers) {
    bool is_shifted = (modifiers & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) != 0;
    
    usb_keymap_entry_t keyInfo = s_keymap[
        is_shifted
            ? keycode | 0x0100      // s_keymap[0x0000..0x00ff] = shifted keymap
            : keycode];             // s_keymap[0x0100..0x01ff] = unshifted keymap
            
    const uint8_t row = keyInfo.row;

    if (row > 7) {
        printf("USB: Key down %d=(undefined)\n", keycode);
        return;
    }

    const uint8_t col = keyInfo.col;

    if (keyInfo.shift) { 
        modifiers |= KEYBOARD_MODIFIER_LEFTSHIFT;
    }
    
    if (keyInfo.unshift) {
        modifiers &= ~(KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
    }

    enqueue_key_event(dev_addr, keycode, row, col, modifiers, /* pressed: */ true);

    keystate_set(keycode, KEYSTATE_PRESSED_FLAG | (is_shifted ? KEYSTATE_SHIFTED_FLAG : 0));
}

static bool shift_lock_enabled = false;

void key_down(uint8_t keycode, uint8_t row, uint8_t col) {
    uint8_t rowMask = 1 << row;

    if (usb_key_matrix[col] & rowMask) {
        usb_key_matrix[col] &= ~rowMask;
        printf("USB: Key down: %d=(%d,%d)\n", keycode, row, col);
    }
}

void modifier_down(uint8_t modifierMask) {
    uint8_t keycode = HID_KEY_CONTROL_LEFT + modifierMask;
    usb_keymap_entry_t keyInfo = s_keymap[keycode];
    key_down(keycode, keyInfo.row, keyInfo.col);
}

void key_up(uint8_t keycode, uint8_t row, uint8_t col) {
    uint8_t rowMask = 1 << row;

    if (usb_key_matrix[col] & ~rowMask) {
        usb_key_matrix[col] |= rowMask;
        printf("USB: Key up: %d=(%d,%d)\n", keycode, row, col);
    }
}

void modifier_up(uint8_t modifierMask) {
    uint8_t keycode = HID_KEY_CONTROL_LEFT + modifierMask;
    usb_keymap_entry_t keyInfo = s_keymap[keycode];
    key_up(keycode, keyInfo.row, keyInfo.col);
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
    static uint8_t active_modifiers = 0;

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
        uint8_t new_modifiers = get_modifiers(keyEvent);

        if (new_modifiers != active_modifiers) {
            uint8_t old_modifiers = active_modifiers;
            active_modifiers = new_modifiers;

            for (uint8_t i = 0; i < 8; i++) {
                uint8_t new_mod = new_modifiers & 0x01;
                uint8_t old_mod = old_modifiers & 0x01;

                if (new_mod) {
                    if (!old_mod) {
                        printf("USB: Modifier down: %s\n", modifiers[i]);
                        modifier_down(i);
                    }
                } else if (old_mod) {
                    printf("USB: Modifier up: %s\n", modifiers[i]);
                    modifier_up(i);
                }

                new_modifiers >>= 1;
                old_modifiers >>= 1;
            }
        }
    }

    do {
        switch (keyEvent.key) {
            case HID_KEY_CAPS_LOCK: {
                if (keyEvent.pressed) {
                    shift_lock_enabled = !shift_lock_enabled;                   
                    printf("USB: Shift lock %s\n", shift_lock_enabled ? "enabled" : "disabled");
                    sync_leds(keyEvent.dev_addr);
                }
                break;
            }
            default: {
                if (keyEvent.pressed) {
                    key_down(keyEvent.key, keyEvent.row, keyEvent.col);
                } else {
                    key_up(keyEvent.key, keyEvent.row, keyEvent.col);
                }
                break;
            }
        }

        dequeue_key_event();

        // Continue updating the PET keyboard matrix as long as there are more events queued
        // and the modifiers are consistent.
    } while (peek_key_event(&keyEvent) && (get_modifiers(keyEvent) == active_modifiers));
}

static bool find_key_in_report(hid_keyboard_report_t const* report, uint8_t keycode) {
    for (uint8_t i = 0; i < sizeof(report->keycode); i++) {
        if (report->keycode[i] == keycode) {
            return true;
        }
    }

    return false;
}

void process_kbd_report(uint8_t dev_addr, hid_keyboard_report_t const* report) {
    static hid_keyboard_report_t prev_report = {
        .modifier = 0,
        .reserved = 0,
        .keycode = { 0, 0, 0, 0, 0, 0 },
    };

    const uint8_t modifiers = report->modifier;

    for (uint8_t i = 0; i < sizeof(prev_report.keycode); i++) {
        const uint8_t keycode = prev_report.keycode[i];
        if (keycode && !find_key_in_report(report, keycode)) {
            enqueue_key_up(dev_addr, keycode, modifiers);
        }
    }

    for (uint8_t i = 0; i < sizeof(report->keycode); i++) {
        const uint8_t keycode = report->keycode[i];
        if (keycode && !find_key_in_report(&prev_report, keycode)) {
            enqueue_key_down(dev_addr, keycode, modifiers);
        }
    }

    prev_report = *report;
}

int keyboard_getch() {
    tuh_task();
    cdc_app_task();     // TODO: USB serial console unused.  Remove?
    hid_app_task();     // TODO: Remove empty HID task or merge with dispatch_key_events?
    dispatch_key_events();
    sync_state();

    bool use_pet_keys = true;
    
    for (uint8_t i = 0; i < KEY_COL_COUNT; i++) {
        use_pet_keys &= (usb_key_matrix[i] == 0xff);
    }

    return keyscan_getch(use_pet_keys ? pet_key_matrix : usb_key_matrix);
}
